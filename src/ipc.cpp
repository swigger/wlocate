#include "ipc.hpp"
#include <sddl.h>
#include <cstring>
#include <array>

namespace wl {
namespace {
constexpr uint32_t signature = 0x31434C57;
struct RequestHeader {
    uint32_t magic = signature, bytes = 0, operation = 0, flags = 0;
    uint64_t limit = 0, offset = 0;
    uint32_t sort = 0, descending = 0, timeout = 0, reserved = 0;
};
struct ReplyHeader {
    uint32_t magic = signature, error = 0;
    uint64_t total = 0, generation = 0, search_us = 0, count = 0;
};
struct RowHeader { uint64_t size, modified, created, accessed; uint32_t attributes, path_bytes; };
class Connection {
    Handle event_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::array<uint8_t, 65536> input_, output_;
    size_t input_at_ = 0, input_size_ = 0, output_size_ = 0;
    DWORD io(void* data, DWORD amount, bool output) {
        if (!event_) throw Error(win_error());
        DWORD done = 0; OVERLAPPED op{}; op.hEvent = event_.value; ResetEvent(event_.value);
        BOOL ok = output ? WriteFile(pipe, data, amount, &done, &op) : ReadFile(pipe, data, amount, &done, &op);
        if (!ok) {
            auto error = GetLastError(); if (error != ERROR_IO_PENDING) throw Error("IPC: " + win_error(error));
            auto now = GetTickCount64();
            auto timeout = now >= deadline ? 0 : static_cast<DWORD>(std::min<uint64_t>(deadline - now, UINT32_MAX - 1));
            HANDLE events[]{event_.value, cancel};
            auto wait = WaitForMultipleObjects(cancel ? 2 : 1, events, FALSE, timeout);
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &op); GetOverlappedResult(pipe, &op, &done, TRUE);
                throw Error(wait == WAIT_OBJECT_0 + 1 ? "Search cancelled" : "WLocate request timed out");
            }
            if (!GetOverlappedResult(pipe, &op, &done, FALSE)) throw Error("IPC: " + win_error());
        }
        if (!done) throw Error("WLocate connection closed"); return done;
    }
public:
    HANDLE pipe;
    uint64_t deadline;
    HANDLE cancel = nullptr;
    Connection(HANDLE handle, uint32_t timeout, HANDLE cancellation = nullptr) : pipe(handle), deadline(GetTickCount64() + timeout), cancel(cancellation) {}
    void flush() {
        for (size_t sent = 0; sent < output_size_;) sent += io(output_.data() + sent, static_cast<DWORD>(output_size_ - sent), true);
        output_size_ = 0;
    }
    void transfer(void* data, size_t bytes, bool output) {
        if (!output) flush();
        while (bytes) {
            size_t done;
            if (output) {
                done = std::min(bytes, output_.size() - output_size_);
                memcpy(output_.data() + output_size_, data, done); output_size_ += done;
                if (output_size_ == output_.size()) flush();
            } else {
                if (input_at_ == input_size_) { input_size_ = io(input_.data(), static_cast<DWORD>(input_.size()), false); input_at_ = 0; }
                done = std::min(bytes, input_size_ - input_at_); memcpy(data, input_.data() + input_at_, done); input_at_ += done;
            }
            data = static_cast<uint8_t*>(data) + done; bytes -= done;
        }
    }
    template<class T> T read() { T value{}; transfer(&value, sizeof(value), false); return value; }
    template<class T> void write(const T& value) { transfer(const_cast<T*>(&value), sizeof(value), true); }
    std::string string(uint32_t bytes) {
        if (bytes > 1024 * 1024) throw Error("IPC string exceeds limit");
        std::string value(bytes, '\0'); transfer(value.data(), value.size(), false); return value;
    }
    void string(const std::string& value) { write(static_cast<uint32_t>(value.size())); transfer(const_cast<char*>(value.data()), value.size(), true); }
};
void handle_client(HANDLE pipe, Engine& engine, HANDLE stop, const std::function<void()>& rebuild) {
    Connection connection(pipe, 3000, stop);
    try {
        auto header = connection.read<RequestHeader>();
        if (header.magic != signature || header.bytes > 65536 || header.sort > uint32_t(Sort::None) || header.flags & ~1023u || header.reserved) throw Error("Invalid WLocate protocol request");
        Request request; request.operation = header.operation; request.flags = header.flags; request.limit = header.limit;
        request.offset = header.offset; request.sort = Sort(header.sort); request.descending = header.descending != 0;
        request.timeout_ms = std::clamp(header.timeout, 100u, 600000u);
        auto data = connection.string(header.bytes);
        for (size_t at = 0; at < data.size();) {
            if (data.size() - at < 4) throw Error("Truncated query pattern");
            uint32_t length; memcpy(&length, data.data() + at, 4); at += 4;
            if (length > data.size() - at || request.patterns.size() >= 256) throw Error("Invalid query pattern length");
            request.patterns.emplace_back(data.substr(at, length)); at += length;
        }
        connection.deadline = GetTickCount64() + request.timeout_ms;
        auto cancelled = [&] { return GetTickCount64() >= connection.deadline || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || !PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr); };
        Reply reply;
        if (request.operation == 0) reply = engine.search(request, cancelled);
        else if (request.operation == 1) reply = engine.status();
        else if (request.operation == 2) {
            if (!ImpersonateNamedPipeClient(pipe)) throw Error("Cannot verify caller privileges");
            bool admin = is_admin(); RevertToSelf();
            if (!admin) throw Error("Rebuilding the system index requires an elevated terminal");
            rebuild(); reply.message = "Index rebuild scheduled";
        } else throw Error("Unknown operation");
        connection.write(ReplyHeader{signature, 0, reply.total, reply.generation, reply.search_us, reply.results.size()});
        connection.string(reply.message);
        for (auto& row : reply.results) {
            connection.write(RowHeader{row.size, row.modified, row.created, row.accessed, row.attributes, static_cast<uint32_t>(row.path.size())});
            connection.transfer(row.path.data(), row.path.size(), true);
        }
        // DisconnectNamedPipe discards unread buffered bytes. Wait until the client
        // confirms the complete reply instead of flushing synchronously without a timeout.
        connection.deadline = GetTickCount64() + 3000;
        if (connection.read<uint32_t>() != signature) throw Error("Invalid reply acknowledgement");
    } catch (const std::exception& error) {
        try { connection.deadline = GetTickCount64() + 1000; connection.write(ReplyHeader{signature, 1}); connection.string(std::string(error.what())); connection.read<uint32_t>(); } catch (...) {}
    }
}
}
Reply request_service(const Request& request, HANDLE cancel) {
    auto deadline = GetTickCount64() + std::min(request.timeout_ms, 10000u);
    Handle pipe;
    for (;;) {
        pipe.reset(CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr));
        if (pipe) break;
        auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY || GetTickCount64() >= deadline) throw Error("WLocate service is unavailable: " + win_error(error) + "\nRun wlocate --install-service from an elevated terminal.");
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) throw Error("Search cancelled");
        WaitNamedPipeW(pipe_name, 100);
    }
    Connection connection(pipe.value, request.timeout_ms, cancel);
    std::string patterns;
    for (auto& value : request.patterns) { auto length = static_cast<uint32_t>(value.size()); patterns.append(reinterpret_cast<const char*>(&length), 4); patterns += value; }
    if (patterns.size() > 65536) throw Error("Search patterns exceed 64 KiB");
    connection.write(RequestHeader{signature, static_cast<uint32_t>(patterns.size()), request.operation, request.flags, request.limit, request.offset,
        uint32_t(request.sort), request.descending ? 1u : 0u, request.timeout_ms, 0});
    connection.transfer(patterns.data(), patterns.size(), true);
    auto header = connection.read<ReplyHeader>(); if (header.magic != signature || header.count > 50000000) throw Error("Invalid WLocate service response");
    Reply reply; reply.message = connection.string(connection.read<uint32_t>());
    if (header.error) { connection.write(signature); connection.flush(); throw Error(reply.message); }
    reply.total = header.total; reply.generation = header.generation; reply.search_us = header.search_us;
    reply.results.reserve(static_cast<size_t>(header.count));
    for (uint64_t i = 0; i < header.count; ++i) {
        auto row = connection.read<RowHeader>();
        reply.results.push_back({connection.string(row.path_bytes), row.size, row.modified, row.created, row.accessed, row.attributes});
    }
    connection.write(signature);
    connection.flush();
    return reply;
}
void serve(Engine& engine, HANDLE stop_event, const std::function<void()>& rebuild) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &descriptor, nullptr)) throw Error(win_error());
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    std::vector<Handle> pipes;
    // Keep every instance open for the server lifetime so another process cannot seize the name.
    for (unsigned i = 0; i < 8; ++i) {
        Handle pipe(CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (i ? 0 : FILE_FLAG_FIRST_PIPE_INSTANCE),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 8, 256 * 1024, 65536, 0, &attributes));
        if (!pipe) { auto error = win_error(); LocalFree(descriptor); throw Error("Create search pipe: " + error); }
        pipes.push_back(std::move(pipe));
    }
    LocalFree(descriptor);
    std::vector<std::jthread> workers;
    for (auto& pipe : pipes) workers.emplace_back([&, handle = pipe.value] {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
            OVERLAPPED op{}; op.hEvent = event.value; ResetEvent(event.value);
            auto connected = ConnectNamedPipe(handle, &op); auto error = connected ? ERROR_SUCCESS : GetLastError();
            if (error == ERROR_IO_PENDING) {
                HANDLE events[]{stop_event, event.value};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                    CancelIoEx(handle, &op); DWORD done; GetOverlappedResult(handle, &op, &done, TRUE); break;
                }
            } else if (error != ERROR_PIPE_CONNECTED && error != ERROR_SUCCESS) { log("Connect pipe: " + win_error(error)); break; }
            handle_client(handle, engine, stop_event, rebuild);
            DisconnectNamedPipe(handle);
        }
    });
    WaitForSingleObject(stop_event, INFINITE);
    for (auto& pipe : pipes) CancelIoEx(pipe.value, nullptr);
}
}

#include "index.hpp"
#include <fstream>
#include <bcrypt.h>
#include <array>

namespace wl {
namespace {
struct Header {
    char magic[8] = {'W','L','O','C','I','D','X','4'};
    uint32_t version = 4, entry_size = sizeof(Entry);
    uint64_t serial = 0, root_id = 0, journal_id = 0, next_usn = 0, name_count = 0, entry_count = 0;
    std::array<uint8_t, 32> digest{};
};
class Hash {
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
public:
    Hash() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw Error("Cannot open snapshot checksum provider");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(algorithm_, 0); throw Error("Cannot create snapshot checksum"); }
    }
    ~Hash() { BCryptDestroyHash(hash_); BCryptCloseAlgorithmProvider(algorithm_, 0); }
    void add(const void* data, size_t bytes) {
        auto at = static_cast<const uint8_t*>(data);
        while (bytes) {
            auto count = static_cast<ULONG>(std::min<size_t>(bytes, 1024 * 1024));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(at), count, 0) < 0) throw Error("Cannot calculate snapshot checksum");
            at += count; bytes -= count;
        }
    }
    void name(std::string_view text) { auto length = static_cast<uint32_t>(text.size()); add(&length, sizeof(length)); add(text.data(), text.size()); }
    std::array<uint8_t, 32> finish() {
        std::array<uint8_t, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw Error("Cannot finish snapshot checksum");
        return digest;
    }
};
template<class T> void put(std::ostream& out, const T& value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
template<class T> T get(std::istream& in) { T value{}; in.read(reinterpret_cast<char*>(&value), sizeof(value)); if (!in) throw Error("Index snapshot is truncated"); return value; }
void put_string(std::ostream& out, std::string_view value) { put(out, static_cast<uint32_t>(value.size())); out.write(value.data(), value.size()); }
std::string get_string(std::istream& in) { auto size = get<uint32_t>(in); if (size > 65536) throw Error("Invalid index name length"); std::string value(size, '\0'); in.read(value.data(), size); if (!in) throw Error("Index snapshot is truncated"); return value; }
}
void Volume::save(const std::filesystem::path& path) const {
    std::shared_lock lock(mutex);
    if (!ready) return;
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path; temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw Error("Cannot write index snapshot");
    Header header; header.serial = serial; header.root_id = root_id; header.journal_id = journal_id; header.next_usn = checkpoint_usn ? checkpoint_usn : next_usn;
    header.name_count = names.size(); header.entry_count = entries.size();
    Hash hash; hash.add(&header, sizeof(header));
    put(out, header);
    for (const auto& name : names) { put_string(out, name.text); hash.name(name.text); }
    out.write(reinterpret_cast<const char*>(entries.data()), entries.size() * sizeof(Entry));
    hash.add(entries.data(), entries.size() * sizeof(Entry)); header.digest = hash.finish();
    out.seekp(0); put(out, header);
    out.flush(); if (!out) throw Error("Cannot finish index snapshot"); out.close();
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || !FlushFileBuffers(file.value)) throw Error("Cannot flush index snapshot: " + win_error());
    file.reset();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw Error("Cannot replace index snapshot: " + win_error());
}
std::shared_ptr<Volume> Volume::load(const std::filesystem::path& path, std::string root, uint64_t expected_serial) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    auto header = get<Header>(in);
    if (memcmp(header.magic, "WLOCIDX4", 8) || header.version != 4 || header.entry_size != sizeof(Entry) || header.serial != expected_serial) return {};
    const auto digest = header.digest; header.digest.fill(0); Hash hash; hash.add(&header, sizeof(header));
    if (header.name_count > 20000000 || header.entry_count > 50000000) throw Error("Index snapshot exceeds size limits");
    auto bytes = std::filesystem::file_size(path);
    if (header.entry_count * sizeof(Entry) > bytes) throw Error("Invalid index snapshot record count");
    auto volume = std::make_shared<Volume>(std::move(root));
    volume->serial = header.serial; volume->root_id = header.root_id; volume->journal_id = header.journal_id; volume->next_usn = header.next_usn;
    volume->checkpoint_usn = header.next_usn;
    for (uint64_t i = 0; i < header.name_count; ++i) {
        auto text = get_string(in);
        hash.name(text);
        if (volume->intern(std::move(text)) != i) throw Error("Duplicate name in index snapshot");
    }
    volume->entries.resize(static_cast<size_t>(header.entry_count));
    in.read(reinterpret_cast<char*>(volume->entries.data()), volume->entries.size() * sizeof(Entry));
    if (!in) throw Error("Index snapshot is truncated");
    hash.add(volume->entries.data(), volume->entries.size() * sizeof(Entry));
    if (hash.finish() != digest || in.peek() != std::char_traits<char>::eof()) throw Error("Index snapshot checksum mismatch; rebuilding required");
    for (auto& entry : volume->entries) if (entry.name >= volume->names.size()) throw Error("Invalid name reference in index snapshot");
    volume->finish();
    return volume;
}
}

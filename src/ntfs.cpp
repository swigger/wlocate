#include "ntfs.hpp"
#include <cstring>
#include <unordered_set>

namespace wl {
namespace {
constexpr uint64_t frn_mask = 0xFFFFFFFFFFFFull;
class RetryRecord : public Error { public: using Error::Error; };
template<class T> T get(std::span<const uint8_t> bytes, size_t at) {
    if (at > bytes.size() || sizeof(T) > bytes.size() - at) throw Error("Invalid NTFS field bounds");
    T value; memcpy(&value, bytes.data() + at, sizeof(value)); return value;
}
RawEntry entry(const RecordInfo& base, const std::pair<uint64_t, std::string>& name) {
    return {base.id, name.first, base.size, base.modified, base.created, base.accessed, base.attributes, name.second};
}
}
std::vector<wchar_t> ntfs_drives() {
    std::vector<wchar_t> drives;
    auto mask = GetLogicalDrives();
    for (unsigned i = 0; i < 26; ++i) if (mask & (1u << i)) {
        wchar_t root[] = L"A:\\"; root[0] += static_cast<wchar_t>(i);
        if (GetDriveTypeW(root) != DRIVE_FIXED && GetDriveTypeW(root) != DRIVE_REMOVABLE) continue;
        wchar_t fs[64]{};
        if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 64) && !_wcsicmp(fs, L"NTFS")) drives.push_back(root[0]);
    }
    return drives;
}
std::vector<uint8_t> NtfsReader::read_at(uint64_t offset, size_t bytes) const {
    LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_.value, position, nullptr, FILE_BEGIN)) throw Error("Seek volume: " + win_error());
    std::vector<uint8_t> data(bytes);
    if (!read_exact(handle_.value, data.data(), data.size())) throw Error("Read volume: " + win_error());
    return data;
}
NtfsReader::NtfsReader(wchar_t drive) {
    std::wstring path = L"\\\\.\\A:"; path[4] = drive; root_ = utf8(std::wstring(1, drive) + L":\\");
    handle_.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle_) throw Error("Open " + root_ + ": " + win_error());
    NTFS_VOLUME_DATA_BUFFER info{}; DWORD returned = 0;
    if (!DeviceIoControl(handle_.value, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &info, sizeof(info), &returned, nullptr)) throw Error("NTFS volume: " + win_error());
    sector_size_ = info.BytesPerSector; cluster_size_ = info.BytesPerCluster; record_size_ = info.BytesPerFileRecordSegment;
    serial_ = static_cast<uint64_t>(info.VolumeSerialNumber.QuadPart); mft_length_ = static_cast<uint64_t>(info.MftValidDataLength.QuadPart);
    if (!sector_size_ || !cluster_size_ || record_size_ < 512 || record_size_ > 65536) throw Error("Unsupported NTFS geometry");
    auto record = read_at(static_cast<uint64_t>(info.MftStartLcn.QuadPart) * cluster_size_, record_size_);
    if (!fixup_record(record, sector_size_)) throw Error("Invalid MFT record zero");
    auto mft = parse_record(record, 0); mft_runs_ = mft.data_runs;
    expand_attribute_list(record, mft);
    for (auto id : mft.extension_ids) if (id != mft.id) {
        auto extra = get_record(id); if (extra.empty()) throw Error("Cannot read MFT extension");
        auto part = parse_record(extra, id & frn_mask);
        if (part.base_id != mft.id) throw Error("MFT extension belongs to another record");
        mft_runs_.insert(mft_runs_.end(), part.data_runs.begin(), part.data_runs.end());
    }
    std::sort(mft_runs_.begin(), mft_runs_.end(), [](auto a, auto b) { return a.vcn < b.vcn; });
    mft_runs_.erase(std::unique(mft_runs_.begin(), mft_runs_.end(), [](auto a, auto b) { return a.vcn == b.vcn; }), mft_runs_.end());
    if (mft_runs_.empty()) throw Error("MFT has no data extents");
    USN_JOURNAL_DATA_V0 journal_info{};
    if (!DeviceIoControl(handle_.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal_info, sizeof(journal_info), &returned, nullptr)) {
        auto error = GetLastError();
        if (error != ERROR_JOURNAL_NOT_ACTIVE) throw Error("Query USN journal: " + win_error(error));
        CREATE_USN_JOURNAL_DATA create{64ull * 1024 * 1024, 8ull * 1024 * 1024};
        if (!DeviceIoControl(handle_.value, FSCTL_CREATE_USN_JOURNAL, &create, sizeof(create), nullptr, 0, &returned, nullptr)) throw Error("Create USN journal: " + win_error());
    }
}
USN_JOURNAL_DATA_V0 NtfsReader::journal() const {
    USN_JOURNAL_DATA_V0 value{}; DWORD returned;
    if (!DeviceIoControl(handle_.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &value, sizeof(value), &returned, nullptr)) throw Error("Query journal: " + win_error());
    return value;
}
std::vector<uint8_t> NtfsReader::raw_record(uint64_t index) const {
    auto offset = (index & frn_mask) * record_size_;
    for (auto run : mft_runs_) {
        auto start = run.vcn * cluster_size_, end = start + run.clusters * cluster_size_;
        if (offset >= start && offset + record_size_ <= end && run.lcn >= 0) {
            auto bytes = read_at(uint64_t(run.lcn) * cluster_size_ + offset - start, record_size_);
            if (!fixup_record(bytes, sector_size_)) return {};
            return bytes;
        }
    }
    return {};
}
std::vector<uint8_t> NtfsReader::get_record(uint64_t id) const {
    NTFS_FILE_RECORD_INPUT_BUFFER input{}; input.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(id & frn_mask);
    std::vector<uint8_t> bytes(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + record_size_); DWORD returned;
    if (!DeviceIoControl(handle_.value, FSCTL_GET_NTFS_FILE_RECORD, &input, sizeof(input), bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr)) {
        auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_INVALID_PARAMETER || error == ERROR_HANDLE_EOF) return {};
        throw Error("Read file record: " + win_error(error));
    }
    if (returned < offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer)) throw Error("Truncated file record header");
    auto* output = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(bytes.data());
    if ((uint64_t(output->FileReferenceNumber.QuadPart) & frn_mask) != (id & frn_mask)) return {};
    if (output->FileRecordLength > record_size_ || offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + output->FileRecordLength > returned) throw Error("Truncated file record");
    std::vector<uint8_t> record(output->FileRecordBuffer, output->FileRecordBuffer + output->FileRecordLength);
    // NTFS returns its current in-memory record here. Sector fixups belong only to
    // raw disk reads; the in-memory sector tails can have changed since the last write.
    if (record.size() < 48 || memcmp(record.data(), "FILE", 4)) throw Error("Invalid kernel file record");
    auto parsed = parse_record(record, id & frn_mask);
    if (!parsed.in_use || ((id >> 48) && parsed.id != id)) return {};
    return record;
}
void NtfsReader::expand_attribute_list(std::span<const uint8_t> record, RecordInfo& info) const {
    for (size_t at = get<uint16_t>(record, 0x14); at + 24 <= record.size();) {
        auto type = get<uint32_t>(record, at); if (type == UINT32_MAX) break;
        auto length = get<uint32_t>(record, at + 4); if (length < 24 || length > record.size() - at) throw Error("Invalid attribute list header");
        auto attr = record.subspan(at, length);
        if (type == 0x20 && attr[8]) {
            auto size = get<uint64_t>(attr, 48); auto first = get<uint64_t>(attr, 16);
            if (first || size > 16 * 1024 * 1024) throw Error("Unsupported attribute-list extent");
            std::vector<uint8_t> list(static_cast<size_t>(size));
            auto runs = parse_runs(attr.subspan(get<uint16_t>(attr, 32)), first);
            for (auto run : runs) {
                auto offset = run.vcn * cluster_size_;
                if (offset >= size || run.lcn < 0) continue;
                auto bytes = std::min<uint64_t>(run.clusters * cluster_size_, size - offset);
                auto data = read_at(uint64_t(run.lcn) * cluster_size_, static_cast<size_t>((bytes + sector_size_ - 1) / sector_size_ * sector_size_));
                memcpy(list.data() + offset, data.data(), static_cast<size_t>(bytes));
            }
            for (size_t p = 0; p + 26 <= list.size();) {
                auto item = get<uint32_t>(list, p); if (!item || item == UINT32_MAX) break;
                auto len = get<uint16_t>(list, p + 4); if (len < 26 || len > list.size() - p) throw Error("Invalid external attribute list");
                if (item == 0x30 || item == 0x80) info.extension_ids.push_back(get<uint64_t>(list, p + 16));
                p += len;
            }
        }
        at += length;
    }
    std::sort(info.extension_ids.begin(), info.extension_ids.end());
    info.extension_ids.erase(std::unique(info.extension_ids.begin(), info.extension_ids.end()), info.extension_ids.end());
}
std::vector<RawEntry> NtfsReader::read_links(uint64_t id) {
    try {
    if ((id & frn_mask) < 16 && (id & frn_mask) != 5) return {};
    auto record = get_record(id); if (record.empty()) return {};
    auto base = parse_record(record, id & frn_mask);
    if (base.base_id) {
        id = base.base_id; record = get_record(id); if (record.empty()) return {};
        base = parse_record(record, id & frn_mask);
        if (base.base_id) throw Error("Invalid NTFS base-record chain");
    }
    expand_attribute_list(record, base);
    auto names = base.names;
    auto filename_attributes = base.filename_attribute_count;
    for (auto ref : base.extension_ids) if (ref != base.id) {
        auto bytes = get_record(ref); if (bytes.empty()) continue;
        auto extra = parse_record(bytes, ref & frn_mask);
        if (extra.base_id == base.id) { names.insert(names.end(), extra.names.begin(), extra.names.end()); base.size = std::max(base.size, extra.size); filename_attributes += extra.filename_attribute_count; }
    }
    std::sort(names.begin(), names.end()); names.erase(std::unique(names.begin(), names.end()), names.end());
    // The MFT header also counts separate DOS (8.3) FILE_NAME attributes.
    // Those aliases must count toward completeness even though search output
    // intentionally contains long names only.
    if (!(base.attributes & FILE_ATTRIBUTE_DIRECTORY) && base.link_count > filename_attributes) {
        // A nonresident $ATTRIBUTE_LIST may still be dirty in NTFS's cache when
        // its USN notification arrives. Raw volume reads cannot see those bytes.
        // Ask NTFS for the complete current link set instead of publishing a partial set.
        FILE_ID_DESCRIPTOR descriptor{}; descriptor.dwSize = sizeof(descriptor); descriptor.Type = FileIdType;
        descriptor.FileId.QuadPart = static_cast<LONGLONG>(base.id);
        Handle file(OpenFileById(handle_.value, &descriptor, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_BACKUP_SEMANTICS));
        if (!file) throw RetryRecord("Hardlink record is temporarily unavailable: " + win_error());
        FILE_STANDARD_INFO standard{};
        if (GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard))) base.size = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
        std::wstring path(32768, L'\0');
        auto chars = GetFinalPathNameByHandleW(file.value, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_OPENED | VOLUME_NAME_DOS);
        if (!chars || chars >= path.size()) throw RetryRecord("Cannot resolve updated hardlink path: " + win_error()); path.resize(chars);
        std::vector<wchar_t> buffer(32768); DWORD length = static_cast<DWORD>(buffer.size());
        HANDLE search = FindFirstFileNameW(path.c_str(), 0, &length, buffer.data());
        if (search == INVALID_HANDLE_VALUE && GetLastError() == ERROR_MORE_DATA) { buffer.resize(length); search = FindFirstFileNameW(path.c_str(), 0, &length, buffer.data()); }
        if (search == INVALID_HANDLE_VALUE) throw RetryRecord("Cannot enumerate updated hardlinks: " + win_error());
        std::vector<std::pair<uint64_t, std::string>> current_names;
        try {
            for (;;) {
                std::wstring link = L"\\\\?\\" + wide(root_.substr(0, 2)) + buffer.data();
                auto slash = link.find_last_of(L'\\'); auto parent_path = link.substr(0, slash); if (parent_path.size() == 6) parent_path += L'\\';
                Handle parent(CreateFileW(parent_path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                BY_HANDLE_FILE_INFORMATION information{};
                if (!parent || !GetFileInformationByHandle(parent.value, &information)) throw RetryRecord("Cannot resolve hardlink parent: " + win_error());
                current_names.emplace_back((uint64_t(information.nFileIndexHigh) << 32) | information.nFileIndexLow, utf8(link.substr(slash + 1)));
                length = static_cast<DWORD>(buffer.size());
                if (!FindNextFileNameW(search, &length, buffer.data())) {
                    auto error = GetLastError();
                    if (error == ERROR_MORE_DATA) { buffer.resize(length); if (FindNextFileNameW(search, &length, buffer.data())) continue; error = GetLastError(); }
                    if (error != ERROR_HANDLE_EOF) throw RetryRecord("Hardlink enumeration: " + win_error(error));
                    break;
                }
            }
            FindClose(search);
        } catch (...) { FindClose(search); throw; }
        names = std::move(current_names);
    }
    std::vector<RawEntry> values;
    for (auto& name : names) values.push_back(entry(base, name));
    return values;
    } catch (const Error& error) {
        // A file can change between the kernel record read and raw attribute-list
        // read. Keep its previous entries and retry; a single busy/corrupt record
        // must not take the entire volume's working index offline.
        throw RetryRecord(error.what());
    }
}
std::shared_ptr<Volume> NtfsReader::scan(const std::function<bool()>& cancelled) {
    auto volume = std::make_shared<Volume>(root_); volume->serial = serial_;
    auto before = journal(); volume->journal_id = before.UsnJournalID; volume->next_usn = before.NextUsn;
    auto root_record = get_record(5); if (root_record.empty()) throw Error("Cannot read volume root");
    volume->root_id = parse_record(root_record, 5).id;
    volume->entries.reserve(static_cast<size_t>(mft_length_ / record_size_));
    std::unordered_map<uint64_t, RecordInfo> bases;
    std::unordered_set<uint64_t> extended;
    uint64_t scanned = 0, invalid = 0;
    for (auto run : mft_runs_) {
        if (run.lcn < 0) continue;
        auto logical = run.vcn * cluster_size_;
        auto length = std::min(run.clusters * cluster_size_, mft_length_ > logical ? mft_length_ - logical : 0);
        for (uint64_t offset = 0; offset < length;) {
            if (cancelled()) throw Error("Indexing cancelled");
            auto bytes = static_cast<size_t>(std::min<uint64_t>(8 * 1024 * 1024, length - offset));
            auto data = read_at(uint64_t(run.lcn) * cluster_size_ + offset, bytes);
            for (size_t at = 0; at + record_size_ <= data.size(); at += record_size_) {
                auto number = (logical + offset + at) / record_size_;
                auto record = std::span(data).subspan(at, record_size_);
                if (!fixup_record(record, sector_size_)) { if (!memcmp(record.data(), "FILE", 4)) ++invalid; continue; }
                auto info = parse_record(record, number); if (!info.in_use) continue;
                auto owner = (info.base_id ? info.base_id : info.id) & frn_mask; if (owner < 16 && owner != 5) continue;
                if (info.base_id) extended.insert(info.base_id); else if (info.has_attribute_list) extended.insert(info.id);
                if (info.names.empty()) continue;
                if (info.base_id) {
                    auto found = bases.find(info.base_id);
                    if (found == bases.end()) {
                        auto base = get_record(info.base_id); if (base.empty()) continue;
                        found = bases.emplace(info.base_id, parse_record(base, info.base_id & frn_mask)).first;
                    }
                    for (auto& name : info.names) volume->add_initial(entry(found->second, name));
                } else for (auto& name : info.names) volume->add_initial(entry(info, name));
                ++scanned;
            }
            offset += bytes;
        }
    }
    if (invalid) throw Error("MFT scan encountered " + std::to_string(invalid) + " invalid FILE records; retry required");
    volume->finish();
    // Also resolve DATA attributes moved to extension records. Reconcile the
    // current complete record sets through NTFS before publishing the raw scan.
    for (auto id : extended) {
        if (cancelled()) throw Error("Indexing cancelled");
        try { volume->replace_links(id, read_links(id)); }
        catch (const RetryRecord&) { pending_updates_[id] = GetTickCount64() + 500; pending_from_ = std::min(pending_from_, volume->next_usn); }
    }
    std::erase_if(volume->entries, [](const Entry& value) { return value.attributes & deleted_attribute; });
    volume->finish();
    while (poll(*volume)) if (cancelled()) throw Error("Indexing cancelled");
    log(root_ + " scan complete: " + std::to_string(scanned) + " records, " + std::to_string(volume->files) + " file links");
    return volume;
}
bool NtfsReader::poll(Volume& volume) {
    auto current = journal();
    if (current.UsnJournalID != volume.journal_id || volume.next_usn < static_cast<uint64_t>(current.FirstUsn) || volume.next_usn > static_cast<uint64_t>(current.NextUsn)) throw Error("USN journal gap; index rebuild required");
    if (volume.next_usn == static_cast<uint64_t>(current.NextUsn) && pending_updates_.empty()) return false;
    auto batch_start = volume.next_usn;
    READ_USN_JOURNAL_DATA_V0 request{}; request.StartUsn = static_cast<USN>(volume.next_usn); request.ReasonMask = UINT32_MAX; request.UsnJournalID = volume.journal_id;
    std::vector<uint8_t> bytes(1024 * 1024); DWORD returned = 0;
    if (!DeviceIoControl(handle_.value, FSCTL_READ_USN_JOURNAL, &request, sizeof(request), bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr)) throw Error("Read USN journal: " + win_error());
    if (returned < sizeof(USN)) throw Error("Truncated USN journal reply");
    std::unordered_set<uint64_t> changed;
    for (size_t at = sizeof(USN); at + 8 <= returned;) {
        auto length = get<uint32_t>(bytes, at); auto version = get<uint16_t>(bytes, at + 4);
        if (length < 8 || length > returned - at) throw Error("Invalid USN record length");
        if (version != 2 || length < offsetof(USN_RECORD_V2, FileName)) throw Error("Unsupported USN record version");
        changed.insert(get<uint64_t>(bytes, at + 8)); at += length;
    }
    std::vector<std::pair<uint64_t, std::vector<RawEntry>>> updates; updates.reserve(changed.size());
    auto now = GetTickCount64();
    for (auto [id, retry] : pending_updates_) if (retry <= now) changed.insert(id);
    for (auto id : changed) {
        try {
            auto links = read_links(id); auto base = links.empty() ? id : links.front().id;
            updates.emplace_back(base, std::move(links)); pending_updates_.erase(id);
        }
        catch (const RetryRecord& error) {
            if (!pending_updates_.contains(id) && pending_updates_.size() < 16) log(root_ + " retrying record " + std::to_string(id) + ": " + error.what());
            pending_updates_[id] = now + 500; pending_from_ = std::min(pending_from_, batch_start);
        }
    }
    {
        std::unique_lock lock(volume.mutex);
        for (auto& [id, links] : updates) volume.replace_links(id, links);
        volume.next_usn = get<uint64_t>(bytes, 0);
        if (pending_updates_.empty()) pending_from_ = UINT64_MAX;
        volume.checkpoint_usn = std::min(volume.next_usn, pending_from_);
        volume.pending_updates = pending_updates_.size();
    }
    return volume.next_usn < static_cast<uint64_t>(current.NextUsn);
}
}

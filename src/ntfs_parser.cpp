#include "ntfs.hpp"
#include <cstring>

namespace wl {
namespace {
template<class T> T load(std::span<const uint8_t> data, size_t at) {
    if (at > data.size() || sizeof(T) > data.size() - at) throw Error("NTFS record field is outside its bounds");
    T value; memcpy(&value, data.data() + at, sizeof(value)); return value;
}
void attribute_list(std::span<const uint8_t> value, std::vector<uint64_t>& refs) {
    for (size_t at = 0; at + 26 <= value.size();) {
        auto type = load<uint32_t>(value, at); if (type == UINT32_MAX || !type) break;
        auto length = load<uint16_t>(value, at + 4); if (length < 26 || length > value.size() - at) throw Error("Invalid NTFS attribute list");
        if (type == 0x30 || type == 0x80) refs.push_back(load<uint64_t>(value, at + 16));
        at += length;
    }
}
}
bool fixup_record(std::span<uint8_t> record, uint32_t sector_size) {
    if (record.size() < 48 || memcmp(record.data(), "FILE", 4) || sector_size < 2) return false;
    auto offset = load<uint16_t>(record, 4), count = load<uint16_t>(record, 6);
    if (count < 2 || size_t(offset) + size_t(count) * 2 > record.size() || (uint64_t(count) - 1) * sector_size > record.size()) return false;
    auto sequence = load<uint16_t>(record, offset);
    for (uint32_t i = 1; i < count; ++i) {
        auto end = size_t(i) * sector_size - 2;
        auto original = load<uint16_t>(record, size_t(offset) + i * 2);
        auto current = load<uint16_t>(record, end);
        // FSCTL_GET_NTFS_FILE_RECORD may already have applied the update-sequence fixups.
        if (current != sequence && current != original) return false;
        memcpy(record.data() + end, &original, 2);
    }
    return true;
}
std::vector<DataRun> parse_runs(std::span<const uint8_t> data, uint64_t first_vcn) {
    std::vector<DataRun> runs; size_t at = 0; int64_t lcn = 0;
    while (at < data.size() && data[at]) {
        auto header = data[at++]; unsigned length_bytes = header & 15, offset_bytes = header >> 4;
        if (!length_bytes || length_bytes > 8 || offset_bytes > 8 || at + length_bytes + offset_bytes > data.size()) throw Error("Invalid NTFS data run");
        uint64_t clusters = 0, raw = 0;
        for (unsigned i = 0; i < length_bytes; ++i) clusters |= uint64_t(data[at++]) << (8 * i);
        for (unsigned i = 0; i < offset_bytes; ++i) raw |= uint64_t(data[at++]) << (8 * i);
        if (!clusters || clusters > INT64_MAX || first_vcn > UINT64_MAX - clusters) throw Error("Invalid NTFS extent length");
        if (offset_bytes && offset_bytes < 8 && (raw & (1ull << (offset_bytes * 8 - 1)))) raw |= UINT64_MAX << (offset_bytes * 8);
        if (offset_bytes) {
            auto delta = static_cast<int64_t>(raw);
            if ((delta > 0 && lcn > INT64_MAX - delta) || (delta < 0 && lcn < INT64_MIN - delta)) throw Error("NTFS extent overflow");
            lcn += delta; if (lcn < 0) throw Error("Invalid NTFS extent offset");
        }
        runs.push_back({first_vcn, clusters, offset_bytes ? lcn : -1}); first_vcn += clusters;
    }
    return runs;
}
RecordInfo parse_record(std::span<const uint8_t> record, uint64_t record_number) {
    RecordInfo info;
    if (record.size() < 48 || memcmp(record.data(), "FILE", 4)) return info;
    info.id = (uint64_t(load<uint16_t>(record, 0x10)) << 48) | (record_number & 0xFFFFFFFFFFFFull);
    info.base_id = load<uint64_t>(record, 0x20);
    info.link_count = load<uint16_t>(record, 0x12);
    auto flags = load<uint16_t>(record, 0x16); info.in_use = (flags & 1) != 0;
    if (!info.in_use) return info;
    if (flags & 2) info.attributes |= FILE_ATTRIBUTE_DIRECTORY;
    auto used = load<uint32_t>(record, 0x18);
    if (used < 48 || used > record.size()) throw Error("Invalid NTFS record size");
    record = record.first(used);
    for (size_t at = load<uint16_t>(record, 0x14); at + 8 <= record.size();) {
        auto type = load<uint32_t>(record, at); if (type == UINT32_MAX) break;
        auto length = load<uint32_t>(record, at + 4);
        if (length < 24 || length > record.size() - at) throw Error("Invalid NTFS attribute length");
        auto attr = record.subspan(at, length);
        bool resident = !attr[8]; auto name_length = attr[9];
        std::span<const uint8_t> value;
        if (resident) {
            auto offset = load<uint16_t>(attr, 20); auto size = load<uint32_t>(attr, 16);
            if (offset > attr.size() || size > attr.size() - offset) throw Error("Invalid NTFS resident value");
            value = attr.subspan(offset, size);
        }
        if (type == 0x10 && resident && value.size() >= 36) {
            info.created = load<uint64_t>(value, 0); info.modified = load<uint64_t>(value, 8); info.accessed = load<uint64_t>(value, 24);
            info.attributes |= load<uint32_t>(value, 32);
        } else if (type == 0x30 && resident && value.size() >= 66) {
            auto chars = value[64], name_space = value[65];
            if (size_t(chars) * 2 > value.size() - 66) throw Error("Invalid NTFS filename length");
            ++info.filename_attribute_count;
            std::wstring name(chars, L'\0'); memcpy(name.data(), value.data() + 66, size_t(chars) * 2);
            auto item = std::pair{load<uint64_t>(value, 0), utf8(name)};
            if (name_space != 2) info.names.push_back(std::move(item));
            if (load<uint32_t>(value, 56) & FILE_ATTRIBUTE_DIRECTORY) info.attributes |= FILE_ATTRIBUTE_DIRECTORY;
        } else if (type == 0x80 && !name_length) {
            if (resident) info.size = value.size();
            else if (attr.size() >= 64) {
                auto vcn = load<uint64_t>(attr, 16); auto offset = load<uint16_t>(attr, 32);
                if (offset >= attr.size()) throw Error("Invalid NTFS run-list offset");
                auto runs = parse_runs(attr.subspan(offset), vcn); info.data_runs.insert(info.data_runs.end(), runs.begin(), runs.end());
                if (!vcn) info.size = load<uint64_t>(attr, 48);
            }
        } else if (type == 0x20) { info.has_attribute_list = true; if (resident) attribute_list(value, info.extension_ids); }
        at += length;
    }
    return info;
}
}

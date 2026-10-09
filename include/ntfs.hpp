#pragma once
#include "index.hpp"
#include <winioctl.h>

namespace wl {
struct DataRun { uint64_t vcn = 0, clusters = 0; int64_t lcn = 0; };
struct RecordInfo {
    uint64_t id = 0, base_id = 0, size = 0, modified = 0, created = 0, accessed = 0;
    uint32_t attributes = 0;
    uint16_t link_count = 0;
    uint32_t filename_attribute_count = 0;
    bool in_use = false, has_attribute_list = false;
    std::vector<std::pair<uint64_t, std::string>> names;
    std::vector<uint64_t> extension_ids;
    std::vector<DataRun> data_runs;
};
bool fixup_record(std::span<uint8_t> record, uint32_t sector_size);
RecordInfo parse_record(std::span<const uint8_t> record, uint64_t record_number);
std::vector<DataRun> parse_runs(std::span<const uint8_t> data, uint64_t first_vcn);
class NtfsReader {
public:
    explicit NtfsReader(wchar_t drive);
    std::shared_ptr<Volume> scan(const std::function<bool()>& cancelled);
    std::vector<RawEntry> read_links(uint64_t id);
    bool poll(Volume& volume);
    uint64_t serial() const { return serial_; }
    USN_JOURNAL_DATA_V0 journal() const;
private:
    Handle handle_;
    std::string root_;
    uint32_t sector_size_ = 512, cluster_size_ = 4096, record_size_ = 1024;
    uint64_t serial_ = 0, mft_length_ = 0;
    std::vector<DataRun> mft_runs_;
    std::unordered_map<uint64_t, uint64_t> pending_updates_;
    uint64_t pending_from_ = UINT64_MAX;
    std::vector<uint8_t> read_at(uint64_t offset, size_t bytes) const;
    std::vector<uint8_t> raw_record(uint64_t index) const;
    std::vector<uint8_t> get_record(uint64_t id) const;
    void expand_attribute_list(std::span<const uint8_t> record, RecordInfo& info) const;
};
std::vector<wchar_t> ntfs_drives();
}

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tmm {

struct ZipEntryInfo {
    std::string filename;
    uint32_t crc32 = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint16_t method = 0; // 0 = Store, 8 = Deflate
    uint32_t offset = 0;
};

class ZipWriter {
public:
    ZipWriter() = default;

    bool add_file(const std::string& archive_path, const uint8_t* data, size_t size);
    bool add_file(const std::string& archive_path, const std::string& text);
    bool add_file(const std::string& archive_path, const std::vector<uint8_t>& bytes);
    bool add_file_from_disk(const std::string& archive_path, const std::string& disk_path);

    bool finalize();
    bool write_to_file(const std::string& output_path);

    const std::vector<uint8_t>& buffer() const { return buffer_; }
    size_t file_count() const { return entries_.size(); }
    bool is_finalized() const { return finalized_; }
    void clear();

private:
    std::vector<uint8_t> buffer_;
    std::vector<ZipEntryInfo> entries_;
    bool finalized_ = false;

    void write16(uint16_t val);
    void write32(uint32_t val);
};

class ZipReader {
public:
    ZipReader() = default;
    ~ZipReader() = default;

    bool open_from_memory(const uint8_t* data, size_t size);
    bool open_from_memory(const std::vector<uint8_t>& buffer);
    bool open_from_file(const std::string& path);

    bool is_open() const { return is_open_; }
    size_t file_count() const { return entries_.size(); }
    const std::vector<ZipEntryInfo>& entries() const { return entries_; }

    bool has_file(const std::string& name) const;
    std::vector<std::string> file_names() const;

    bool extract_to_buffer(const std::string& name, std::vector<uint8_t>& out) const;
    bool extract_to_text(const std::string& name, std::string& out) const;

    void close();

private:
    std::vector<uint8_t> data_;
    std::vector<ZipEntryInfo> entries_;
    bool is_open_ = false;

    const ZipEntryInfo* find_entry(const std::string& name) const;
    static uint16_t read16(const uint8_t* p);
    static uint32_t read32(const uint8_t* p);
};

} // namespace tmm

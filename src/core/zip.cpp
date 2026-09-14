#include "zip.h"

#define LODEPNG_NO_COMPILE_CPP
extern "C" {
#include "lodepng.h"
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace tmm {

void ZipWriter::write16(uint16_t val) {
    buffer_.push_back(static_cast<uint8_t>(val & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

void ZipWriter::write32(uint32_t val) {
    buffer_.push_back(static_cast<uint8_t>(val & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buffer_.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

bool ZipWriter::add_file(const std::string& archive_path, const uint8_t* data, size_t size) {
    if (finalized_ || archive_path.empty()) return false;

    ZipEntryInfo e;
    e.filename = archive_path;
    // Normalize path separators to forward slash per ZIP specification
    std::replace(e.filename.begin(), e.filename.end(), '\\', '/');
    while (!e.filename.empty() && e.filename[0] == '/') {
        e.filename.erase(0, 1);
    }
    e.uncomp_size = static_cast<uint32_t>(size);
    e.crc32 = size > 0 ? lodepng_crc32(data, size) : 0;
    e.offset = static_cast<uint32_t>(buffer_.size());

    unsigned char* deflated = nullptr;
    size_t deflated_size = 0;
    bool use_deflate = false;

    if (size > 0 && data) {
        LodePNGCompressSettings s;
        lodepng_compress_settings_init(&s);
        unsigned err = lodepng_deflate(&deflated, &deflated_size, data, size, &s);
        if (err == 0 && deflated_size < size) {
            use_deflate = true;
        }
    }

    e.method = use_deflate ? 8 : 0;
    e.comp_size = use_deflate ? static_cast<uint32_t>(deflated_size) : e.uncomp_size;

    // Write Local File Header (30 bytes + filename)
    write32(0x04034b50);         // Local file header signature
    write16(20);                 // Version needed to extract (2.0)
    write16(0x0800);             // UTF-8 flag
    write16(e.method);           // Compression method
    write16(0);                  // Last mod file time
    write16(0);                  // Last mod file date
    write32(e.crc32);            // CRC-32
    write32(e.comp_size);        // Compressed size
    write32(e.uncomp_size);      // Uncompressed size
    write16(static_cast<uint16_t>(e.filename.size())); // File name length
    write16(0);                  // Extra field length
    buffer_.insert(buffer_.end(), e.filename.begin(), e.filename.end());

    if (use_deflate && deflated) {
        buffer_.insert(buffer_.end(), deflated, deflated + deflated_size);
        free(deflated);
    } else {
        if (deflated) free(deflated);
        if (size > 0 && data) {
            buffer_.insert(buffer_.end(), data, data + size);
        }
    }

    entries_.push_back(e);
    return true;
}

bool ZipWriter::add_file(const std::string& archive_path, const std::string& text) {
    return add_file(archive_path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool ZipWriter::add_file(const std::string& archive_path, const std::vector<uint8_t>& bytes) {
    return add_file(archive_path, bytes.data(), bytes.size());
}

bool ZipWriter::add_file_from_disk(const std::string& archive_path, const std::string& disk_path) {
    std::ifstream f(disk_path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(data.data()), size);
    }
    return add_file(archive_path, data);
}

bool ZipWriter::finalize() {
    if (finalized_) return true;

    const uint32_t cd_offset = static_cast<uint32_t>(buffer_.size());

    // Write Central Directory file headers
    for (const auto& e : entries_) {
        write32(0x02014b50);         // Central file header signature
        write16(20);                 // Version made by
        write16(20);                 // Version needed to extract
        write16(0x0800);             // UTF-8 filename flag
        write16(e.method);           // Compression method
        write16(0);                  // Last mod file time
        write16(0);                  // Last mod file date
        write32(e.crc32);            // CRC-32
        write32(e.comp_size);        // Compressed size
        write32(e.uncomp_size);      // Uncompressed size
        write16(static_cast<uint16_t>(e.filename.size())); // File name length
        write16(0);                  // Extra field length
        write16(0);                  // File comment length
        write16(0);                  // Disk number start
        write16(0);                  // Internal file attributes
        write32(0);                  // External file attributes
        write32(e.offset);           // Relative offset of local header
        buffer_.insert(buffer_.end(), e.filename.begin(), e.filename.end());
    }

    const uint32_t cd_size = static_cast<uint32_t>(buffer_.size() - cd_offset);

    // Write End of Central Directory record (22 bytes)
    write32(0x06054b50);         // EOCD signature
    write16(0);                  // Number of this disk
    write16(0);                  // Disk where central directory starts
    write16(static_cast<uint16_t>(entries_.size())); // Entries on this disk
    write16(static_cast<uint16_t>(entries_.size())); // Total entries
    write32(cd_size);            // Size of central directory
    write32(cd_offset);          // Offset of central directory
    write16(0);                  // Comment length

    finalized_ = true;
    return true;
}

bool ZipWriter::write_to_file(const std::string& output_path) {
    if (!finalize()) return false;
    std::ofstream f(output_path, std::ios::binary);
    if (!f.is_open()) return false;
    if (!buffer_.empty()) {
        f.write(reinterpret_cast<const char*>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
    }
    return f.good();
}

void ZipWriter::clear() {
    buffer_.clear();
    entries_.clear();
    finalized_ = false;
}

uint16_t ZipReader::read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ZipReader::read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static std::string normalize_zip_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    if (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    return path;
}

bool ZipReader::open_from_memory(const uint8_t* data, size_t size) {
    close();
    if (!data || size < 22) return false;

    data_.assign(data, data + size);

    // Search for End of Central Directory record (0x06054b50) from the end
    const size_t max_search = (data_.size() > 65535 + 22) ? (65535 + 22) : data_.size();
    const size_t min_idx = data_.size() - max_search;
    size_t eocd_pos = std::string::npos;

    for (size_t i = data_.size() - 22; ; --i) {
        if (read32(&data_[i]) == 0x06054b50) {
            const uint16_t comment_len = read16(&data_[i + 20]);
            if (i + 22 + comment_len == data_.size()) {
                eocd_pos = i;
                break;
            }
        }
        if (i == min_idx) break;
    }

    if (eocd_pos == std::string::npos) {
        close();
        return false;
    }

    const uint16_t num_entries = read16(&data_[eocd_pos + 10]);
    const uint32_t cd_size = read32(&data_[eocd_pos + 12]);
    const uint32_t cd_offset = read32(&data_[eocd_pos + 16]);

    if (static_cast<size_t>(cd_offset) + cd_size > eocd_pos || static_cast<size_t>(cd_offset) + cd_size > data_.size()) {
        close();
        return false;
    }

    size_t cur = cd_offset;
    for (uint16_t i = 0; i < num_entries; ++i) {
        if (cur + 46 > data_.size()) break;
        if (read32(&data_[cur]) != 0x02014b50) break;

        ZipEntryInfo e;
        e.method = read16(&data_[cur + 10]);
        e.crc32 = read32(&data_[cur + 16]);
        e.comp_size = read32(&data_[cur + 20]);
        e.uncomp_size = read32(&data_[cur + 24]);
        const uint16_t name_len = read16(&data_[cur + 28]);
        const uint16_t extra_len = read16(&data_[cur + 30]);
        const uint16_t comment_len = read16(&data_[cur + 32]);
        e.offset = read32(&data_[cur + 42]);

        if (cur + 46 + name_len > data_.size()) break;
        e.filename = std::string(reinterpret_cast<const char*>(&data_[cur + 46]), name_len);
        entries_.push_back(e);

        cur += 46 + name_len + extra_len + comment_len;
    }

    is_open_ = true;
    return true;
}

bool ZipReader::open_from_memory(const std::vector<uint8_t>& buffer) {
    return open_from_memory(buffer.data(), buffer.size());
}

bool ZipReader::open_from_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize size = f.tellg();
    if (size < 22) return false;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    if (!f.good() && !f.eof()) return false;
    return open_from_memory(buf);
}

const ZipEntryInfo* ZipReader::find_entry(const std::string& name) const {
    if (!is_open_) return nullptr;
    const std::string norm_target = normalize_zip_path(name);
    for (const auto& e : entries_) {
        if (normalize_zip_path(e.filename) == norm_target) {
            return &e;
        }
    }
    return nullptr;
}

bool ZipReader::has_file(const std::string& name) const {
    return find_entry(name) != nullptr;
}

std::vector<std::string> ZipReader::file_names() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& e : entries_) {
        names.push_back(e.filename);
    }
    return names;
}

bool ZipReader::extract_to_buffer(const std::string& name, std::vector<uint8_t>& out) const {
    const ZipEntryInfo* e = find_entry(name);
    if (!e) return false;

    if (static_cast<size_t>(e->offset) + 30 > data_.size()) return false;
    if (read32(&data_[e->offset]) != 0x04034b50) return false;

    const uint16_t local_name_len = read16(&data_[e->offset + 26]);
    const uint16_t local_extra_len = read16(&data_[e->offset + 28]);
    const size_t payload_offset = static_cast<size_t>(e->offset) + 30 + local_name_len + local_extra_len;

    if (payload_offset + e->comp_size > data_.size()) return false;

    if (e->method == 0) {
        // STORE (uncompressed)
        if (e->uncomp_size == 0) {
            out.clear();
            return true;
        }
        out.assign(data_.begin() + payload_offset, data_.begin() + payload_offset + e->uncomp_size);
    } else if (e->method == 8) {
        // DEFLATE
        if (e->uncomp_size == 0) {
            out.clear();
            return true;
        }
        unsigned char* uncomp = nullptr;
        size_t uncomp_size = 0;
        LodePNGDecompressSettings s;
        lodepng_decompress_settings_init(&s);
        unsigned err = lodepng_inflate(&uncomp, &uncomp_size, &data_[payload_offset], e->comp_size, &s);
        if (err != 0 || !uncomp) {
            if (uncomp) free(uncomp);
            return false;
        }
        out.assign(uncomp, uncomp + uncomp_size);
        free(uncomp);
    } else {
        // Unsupported compression method
        return false;
    }

    if (e->crc32 != 0 && !out.empty()) {
        const uint32_t calc_crc = lodepng_crc32(out.data(), out.size());
        if (calc_crc != e->crc32) {
            return false;
        }
    }

    return true;
}

bool ZipReader::extract_to_text(const std::string& name, std::string& out) const {
    std::vector<uint8_t> buf;
    if (!extract_to_buffer(name, buf)) return false;
    out.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
    return true;
}

void ZipReader::close() {
    data_.clear();
    entries_.clear();
    is_open_ = false;
}

} // namespace tmm

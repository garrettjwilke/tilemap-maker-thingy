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

} // namespace tmm

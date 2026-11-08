#include "iso_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "third_party/stb_image.h"  // stbi_zlib_decode_noheader_buffer (implementation in texture_pack.cpp)

namespace burnout::iso {
namespace {

constexpr std::uint32_t kSector = 2048u;

std::uint32_t le32(const std::uint8_t *p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8u | static_cast<std::uint32_t>(p[2]) << 16u |
           static_cast<std::uint32_t>(p[3]) << 24u;
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool seek(std::FILE *file, std::uint64_t offset) {
#if defined(_MSC_VER)
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
#endif
}

} // namespace

Image::~Image() {
    if (file_ != nullptr) std::fclose(file_);
}

std::unique_ptr<Image> Image::open(const std::string &path) {
    std::unique_ptr<Image> image(new Image());
    image->file_ = std::fopen(path.c_str(), "rb");
    if (image->file_ == nullptr) return nullptr;
    std::uint8_t header[24]{};
    if (std::fread(header, 1u, sizeof(header), image->file_) != sizeof(header)) return nullptr;
    if (std::memcmp(header, "CISO", 4) == 0) {
        image->cso_ = true;
        std::uint64_t total = 0u;
        std::memcpy(&total, header + 8, 8);
        image->block_size_ = le32(header + 16);
        image->align_ = header[21];
        if (image->block_size_ == 0u || (image->block_size_ % kSector) != 0u) return nullptr;
        image->blocks_ = static_cast<std::uint32_t>((total + image->block_size_ - 1u) / image->block_size_);
        image->index_.resize(image->blocks_ + 1u);
        if (!seek(image->file_, 24u) ||
            std::fread(image->index_.data(), 4u, image->index_.size(), image->file_) != image->index_.size())
            return nullptr;
    }
    std::uint8_t pvd[kSector];
    if (!image->read_sectors(16u, 1u, pvd) || pvd[0] != 1u || std::memcmp(pvd + 1, "CD001", 5) != 0) return nullptr;
    image->root_lba_ = le32(pvd + 156 + 2);
    image->root_size_ = le32(pvd + 156 + 10);
    return image;
}

bool Image::read_block(std::uint32_t block, std::uint8_t *out) {
    if (block >= blocks_) return false;
    if (block == cached_block_) {
        std::memcpy(out, cached_.data(), block_size_);
        return true;
    }
    const std::uint32_t raw = index_[block], next = index_[block + 1u];
    const bool plain = (raw & 0x80000000u) != 0u;
    const std::uint64_t start = static_cast<std::uint64_t>(raw & 0x7FFFFFFFu) << align_;
    const std::uint64_t end = static_cast<std::uint64_t>(next & 0x7FFFFFFFu) << align_;
    if (end < start) return false;
    const auto size = static_cast<std::size_t>(end - start);
    compressed_.resize(std::max<std::size_t>(size, block_size_));
    if (!seek(file_, start) || std::fread(compressed_.data(), 1u, plain ? block_size_ : size, file_) != (plain ? block_size_ : size))
        return false;
    cached_.resize(block_size_);
    if (plain) {
        std::memcpy(cached_.data(), compressed_.data(), block_size_);
    } else {
        const int got = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char *>(cached_.data()), static_cast<int>(block_size_),
                                                         reinterpret_cast<const char *>(compressed_.data()), static_cast<int>(size));
        if (got < 0) return false;
    }
    cached_block_ = block;
    std::memcpy(out, cached_.data(), block_size_);
    return true;
}

bool Image::read_sectors(std::uint32_t lba, std::uint32_t count, std::uint8_t *out) {
    if (!cso_) {
        return seek(file_, static_cast<std::uint64_t>(lba) * kSector) &&
               std::fread(out, 1u, static_cast<std::size_t>(count) * kSector, file_) == static_cast<std::size_t>(count) * kSector;
    }
    std::vector<std::uint8_t> block(block_size_);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t offset = static_cast<std::uint64_t>(lba + i) * kSector;
        const auto index = static_cast<std::uint32_t>(offset / block_size_);
        if (!read_block(index, block.data())) return false;
        std::memcpy(out + static_cast<std::size_t>(i) * kSector, block.data() + offset % block_size_, kSector);
    }
    return true;
}

void Image::walk(std::uint32_t lba, std::uint32_t size, const std::string &prefix, std::vector<Entry> &out, int depth) {
    if (depth > 16) return;
    const std::uint32_t sectors = (size + kSector - 1u) / kSector;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * kSector);
    if (sectors == 0u || !read_sectors(lba, sectors, data.data())) return;
    for (std::uint32_t at = 0; at < size;) {
        const std::uint8_t length = data[at];
        if (length == 0u) {  // records never cross a sector: go to the next one
            at = (at / kSector + 1u) * kSector;
            continue;
        }
        if (at + length > data.size() || length < 34u) break;
        const std::uint8_t *record = &data[at];
        const std::uint8_t name_length = record[32];
        std::string name(reinterpret_cast<const char *>(record + 33), name_length);
        at += length;
        if (name_length == 1u && (name[0] == '\0' || name[0] == '\1')) continue;  // "." and ".."
        if (const std::size_t semicolon = name.find(';'); semicolon != std::string::npos) name.resize(semicolon);
        Entry entry;
        entry.path = prefix.empty() ? name : prefix + "/" + name;
        entry.lba = le32(record + 2);
        entry.size = le32(record + 10);
        entry.directory = (record[25] & 2u) != 0u;
        out.push_back(entry);
        if (entry.directory) walk(entry.lba, entry.size, entry.path, out, depth + 1);
    }
}

std::vector<Entry> Image::list(const std::string &directory) {
    std::vector<Entry> all;
    if (directory.empty()) {
        walk(root_lba_, root_size_, "", all, 0);
        return all;
    }
    Entry dir;
    if (!find(directory, dir) || !dir.directory) return all;
    walk(dir.lba, dir.size, dir.path, all, 0);
    return all;
}

bool Image::find(const std::string &path, Entry &entry) {
    std::uint32_t lba = root_lba_, size = root_size_;
    std::string done;
    std::size_t at = 0u;
    const std::string wanted = lower(path);
    while (at <= wanted.size()) {
        const std::size_t slash = wanted.find('/', at);
        const std::string part = wanted.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        std::vector<Entry> children;
        // One level only: walk with depth limit by listing then filtering direct children.
        const std::uint32_t sectors = (size + kSector - 1u) / kSector;
        std::vector<std::uint8_t> data(static_cast<std::size_t>(sectors) * kSector);
        if (sectors == 0u || !read_sectors(lba, sectors, data.data())) return false;
        bool found = false;
        for (std::uint32_t pos = 0; pos < size && !found;) {
            const std::uint8_t length = data[pos];
            if (length == 0u) {
                pos = (pos / kSector + 1u) * kSector;
                continue;
            }
            if (pos + length > data.size() || length < 34u) break;
            const std::uint8_t *record = &data[pos];
            std::string name(reinterpret_cast<const char *>(record + 33), record[32]);
            pos += length;
            if (const std::size_t semicolon = name.find(';'); semicolon != std::string::npos) name.resize(semicolon);
            if (lower(name) != part) continue;
            found = true;
            done = done.empty() ? name : done + "/" + name;
            lba = le32(record + 2);
            size = le32(record + 10);
            entry.path = done;
            entry.lba = lba;
            entry.size = size;
            entry.directory = (record[25] & 2u) != 0u;
        }
        if (!found) return false;
        if (slash == std::string::npos) return true;
        if (!entry.directory) return false;
        at = slash + 1u;
    }
    return false;
}

bool Image::read_file(const Entry &entry, std::vector<std::uint8_t> &data) {
    const std::uint32_t sectors = (entry.size + kSector - 1u) / kSector;
    data.resize(static_cast<std::size_t>(sectors) * kSector);
    if (sectors != 0u && !read_sectors(entry.lba, sectors, data.data())) return false;
    data.resize(entry.size);
    return true;
}

bool Image::copy_file(const Entry &entry, const std::string &host_path, const std::function<void(std::uint64_t)> &progress) {
    std::FILE *out = std::fopen(host_path.c_str(), "wb");
    if (out == nullptr) return false;
    constexpr std::uint32_t kChunk = 32u;  // sectors (64 KiB)
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kChunk) * kSector);
    std::uint64_t done = 0u;
    bool ok = true;
    for (std::uint32_t sector = 0; done < entry.size && ok;) {
        const std::uint64_t left = entry.size - done;
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(kChunk, (left + kSector - 1u) / kSector));
        ok = read_sectors(entry.lba + sector, count, buffer.data());
        const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(left, static_cast<std::uint64_t>(count) * kSector));
        ok = ok && std::fwrite(buffer.data(), 1u, bytes, out) == bytes;
        done += bytes;
        sector += count;
        if (progress) progress(done);
    }
    ok = (std::fclose(out) == 0) && ok;
    return ok;
}

} // namespace burnout::iso

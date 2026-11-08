#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Read-only access to a PSP disc image: plain ISO 9660 (.iso) or CISO v1
// (.cso, deflate-compressed 2 KiB blocks).
namespace burnout::iso {

struct Entry {
    std::string path;  // "PSP_GAME/USRDIR/music/EATrax00.mus" (no leading slash)
    std::uint32_t lba{};
    std::uint32_t size{};
    bool directory{};
};

class Image {
public:
    ~Image();
    static std::unique_ptr<Image> open(const std::string &path);

    // `path` relative to the disc root, '/' separated, case-insensitive.
    bool find(const std::string &path, Entry &entry);
    bool read_file(const Entry &entry, std::vector<std::uint8_t> &data);
    // Every file and directory under `directory` ("" = the whole disc).
    std::vector<Entry> list(const std::string &directory);
    // Copies a file in 64 KiB pieces; `progress` gets the bytes copied so far.
    bool copy_file(const Entry &entry, const std::string &host_path, const std::function<void(std::uint64_t)> &progress);

private:
    Image() = default;
    bool read_sectors(std::uint32_t lba, std::uint32_t count, std::uint8_t *out);
    bool read_block(std::uint32_t block, std::uint8_t *out);
    void walk(std::uint32_t lba, std::uint32_t size, const std::string &prefix, std::vector<Entry> &out, int depth);

    std::FILE *file_{};
    bool cso_{};
    std::uint32_t block_size_{2048u};
    std::uint32_t align_{};
    std::uint32_t blocks_{};
    std::vector<std::uint32_t> index_;
    std::vector<std::uint8_t> compressed_;
    std::uint32_t root_lba_{}, root_size_{};
    std::uint32_t cached_block_{UINT32_MAX};
    std::vector<std::uint8_t> cached_;
};

} // namespace burnout::iso

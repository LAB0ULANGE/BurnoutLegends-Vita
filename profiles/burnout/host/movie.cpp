#include "movie.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#endif

namespace burnout::movie {
namespace {

std::uint32_t be32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) << 24u | static_cast<std::uint32_t>(p[1]) << 16u |
           static_cast<std::uint32_t>(p[2]) << 8u | p[3];
}

std::vector<std::uint8_t> read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string base_name(const std::string &path) {
    const std::size_t slash = path.find_last_of("/\\:");
    return slash == std::string::npos ? path : path.substr(slash + 1u);
}

} // namespace

std::vector<std::vector<std::uint8_t>> access_units(const std::vector<std::uint8_t> &file) {
    std::vector<std::vector<std::uint8_t>> units;
    if (file.size() < 16u || std::memcmp(file.data(), "PSMF", 4u) != 0) return units;
    // MPEG-PS from the offset in the PSMF header: video PES payloads joined.
    std::vector<std::uint8_t> es;
    std::size_t i = be32(file.data() + 8u);
    const std::size_t n = file.size();
    while (i + 4u <= n) {
        if (file[i] != 0u || file[i + 1u] != 0u || file[i + 2u] != 1u) { ++i; continue; }
        const std::uint8_t id = file[i + 3u];
        if (id == 0xBAu) {  // pack header (MPEG-2)
            if (i + 14u > n) break;
            i += 14u + (file[i + 13u] & 7u);
            continue;
        }
        if (id == 0xB9u) { i += 4u; continue; }  // end code
        if (i + 6u > n) break;
        const std::size_t length = static_cast<std::size_t>(file[i + 4u]) << 8u | file[i + 5u];
        if (id == 0xE0u && i + 9u <= n) {
            const std::size_t start = i + 9u + file[i + 8u], end = std::min(n, i + 6u + length);
            if (start < end) es.insert(es.end(), file.begin() + static_cast<std::ptrdiff_t>(start), file.begin() + static_cast<std::ptrdiff_t>(end));
        }
        i += 6u + length;
    }
    // One access unit per access unit delimiter (NAL type 9).
    std::vector<std::size_t> starts;
    for (std::size_t j = 0; j + 3u < es.size(); ++j) {
        if (es[j] == 0u && es[j + 1u] == 0u && es[j + 2u] == 1u && (es[j + 3u] & 0x1Fu) == 9u)
            starts.push_back(j > 0u && es[j - 1u] == 0u ? j - 1u : j);
    }
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::size_t end = k + 1u < starts.size() ? starts[k + 1u] : es.size();
        units.emplace_back(es.begin() + static_cast<std::ptrdiff_t>(starts[k]), es.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return units;
}

namespace {

#if defined(__vita__)
// Hardware AVC decoder, RGBA8888 out. Library and decoder memory in
// physically contiguous RAM (vitaGL keeps nearly all CDRAM).
class VitaMovie final : public Movie {
public:
    VitaMovie(std::vector<std::vector<std::uint8_t>> units) : units_(std::move(units)) {}
    ~VitaMovie() override {
        if (decoder_ready_) sceAvcdecDeleteDecoder(&decoder_);
        if (decoder_block_ >= 0) sceKernelFreeMemBlock(decoder_block_);
        if (picture_block_ >= 0) sceKernelFreeMemBlock(picture_block_);
    }

    bool start(std::string &error) {
        static bool library = false;
        if (!library) {
            sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
            SceVideodecQueryInitInfoHwAvcdec init{};
            init.size = sizeof(init);
            init.horizontal = 512;
            init.vertical = 288;
            init.numOfRefFrames = 5;
            init.numOfStreams = 1;
            const int result = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
            if (result < 0) return fail(error, "sceVideodecInitLibrary", result);
            library = true;
        }
        SceAvcdecQueryDecoderInfo query{};
        query.horizontal = 512;
        query.vertical = 288;
        query.numOfRefFrames = 5;
        SceAvcdecDecoderInfo info{};
        int result = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info);
        if (result < 0) return fail(error, "sceAvcdecQueryDecoderMemSize", result);
        const SceSize decoder_size = (info.frameMemSize + 0xFFFFFu) & ~0xFFFFFu;
        decoder_block_ = sceKernelAllocMemBlock("movie_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, decoder_size, nullptr);
        if (decoder_block_ < 0) return fail(error, "decoder memory", decoder_block_);
        void *decoder_memory = nullptr;
        sceKernelGetMemBlockBase(decoder_block_, &decoder_memory);
        decoder_.frameBuf.pBuf = decoder_memory;
        decoder_.frameBuf.size = info.frameMemSize;
        result = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &decoder_, &query);
        if (result < 0) return fail(error, "sceAvcdecCreateDecoder", result);
        decoder_ready_ = true;
        picture_block_ = sceKernelAllocMemBlock("movie_picture", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 0x100000u, nullptr);
        if (picture_block_ < 0) return fail(error, "picture memory", picture_block_);
        sceKernelGetMemBlockBase(picture_block_, &picture_);
        return true;
    }

    bool next_frame(std::vector<std::uint8_t> &rgba) override {
        // A decoder may hold frames back (reordering): feed until one comes out.
        while (next_ < units_.size()) {
            auto &unit = units_[next_++];
            SceAvcdecAu au{};
            au.pts.upper = au.pts.lower = au.dts.upper = au.dts.lower = 0xFFFFFFFFu;
            au.es.pBuf = unit.data();
            au.es.size = static_cast<std::uint32_t>(unit.size());
            SceAvcdecPicture picture{};
            picture.size = sizeof(picture);
            picture.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
            picture.frame.framePitch = kPitch;
            picture.frame.frameWidth = kWidth;
            picture.frame.frameHeight = kHeight;
            picture.frame.pPicture[0] = picture_;
            SceAvcdecPicture *pictures[1]{&picture};
            SceAvcdecArrayPicture array{};
            array.numOfElm = 1;
            array.pPicture = pictures;
            if (sceAvcdecDecode(&decoder_, &au, &array) < 0) continue;
            if (array.numOfOutput == 0u) continue;
            copy_out(rgba);
            return true;
        }
        return false;
    }

    std::size_t frame_count() const override { return units_.size(); }
    // The first unit is an IDR picture: the decoder restarts from it.
    void rewind() override { next_ = 0; }

private:
    static constexpr std::uint32_t kPitch = 512;

    bool fail(std::string &error, const char *what, int code) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08X", what, static_cast<unsigned>(code));
        error = text;
        return false;
    }

    void copy_out(std::vector<std::uint8_t> &rgba) const {
        rgba.resize(static_cast<std::size_t>(kWidth) * kHeight * 4u);
        const auto *source = static_cast<const std::uint8_t *>(picture_);
        for (int y = 0; y < kHeight; ++y)
            std::memcpy(rgba.data() + static_cast<std::size_t>(y) * kWidth * 4u, source + static_cast<std::size_t>(y) * kPitch * 4u,
                        static_cast<std::size_t>(kWidth) * 4u);
    }

    std::vector<std::vector<std::uint8_t>> units_;
    std::size_t next_{};
    SceAvcdecCtrl decoder_{};
    bool decoder_ready_{};
    SceUID decoder_block_{-1}, picture_block_{-1};
    void *picture_{};
};
#endif

// PC test tool: frames decoded beforehand (tools/predecode_movies.py), else black.
class FileMovie final : public Movie {
public:
    FileMovie(std::size_t count, std::string frames) : count_(count), path_(std::move(frames)) {
        if (!path_.empty()) in_.open(path_, std::ios::binary);
    }
    bool next_frame(std::vector<std::uint8_t> &rgba) override {
        if (next_ >= count_) return false;
        ++next_;
        rgba.assign(static_cast<std::size_t>(kWidth) * kHeight * 4u, 0u);
        if (in_) in_.read(reinterpret_cast<char *>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
        for (std::size_t i = 3; i < rgba.size(); i += 4u) rgba[i] = 0xFFu;
        return true;
    }
    std::size_t frame_count() const override { return count_; }
    void rewind() override {
        next_ = 0;
        if (!path_.empty()) { in_.clear(); in_.seekg(0); }
    }

private:
    std::size_t count_{}, next_{};
    std::string path_;
    std::ifstream in_;
};

} // namespace

std::unique_ptr<Movie> open(const std::string &host_path, std::string &error) {
    auto units = access_units(read_file(host_path));
    if (units.empty()) {
        error = "no H.264 access units in " + host_path;
        return nullptr;
    }
#if defined(__vita__)
    auto movie = std::make_unique<VitaMovie>(std::move(units));
    if (!movie->start(error)) return nullptr;
    return movie;
#else
    std::string frames;
    if (const char *dir = std::getenv("PSPRECOMP_BURNOUT_MOVIE_FRAMES")) frames = std::string(dir) + "/" + base_name(host_path) + ".rgba";
    return std::make_unique<FileMovie>(units.size(), frames);
#endif
}

} // namespace burnout::movie

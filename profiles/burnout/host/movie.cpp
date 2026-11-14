#include "movie.hpp"
#include "detached_thread.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#if defined(_WIN32) && !defined(__vita__)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#endif
#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
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

using Units = std::vector<std::vector<std::uint8_t>>;

#if defined(__vita__)
// Hardware AVC decoder, RGBA8888 out, created once and shared by every movie
// (all are 480x272): creating and deleting it at each menu change stalled the
// game ~1 s. Each movie starts with an IDR picture, which resets the decoding.
// Library and decoder memory in physically contiguous RAM (vitaGL keeps
// nearly all CDRAM).
struct SharedDecoder {
    static constexpr std::uint32_t kPitch = 512;
    bool ready{};
    SceAvcdecCtrl ctrl{};
    SceUID decoder_block{-1}, picture_block{-1};
    void *picture{};

    bool start(std::string &error) {
        if (ready) return true;
        sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
        SceVideodecQueryInitInfoHwAvcdec init{};
        init.size = sizeof(init);
        init.horizontal = 512;
        init.vertical = 288;
        init.numOfRefFrames = 5;
        init.numOfStreams = 1;
        int result = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
        if (result < 0) return fail(error, "sceVideodecInitLibrary", result);
        SceAvcdecQueryDecoderInfo query{};
        query.horizontal = 512;
        query.vertical = 288;
        query.numOfRefFrames = 5;
        SceAvcdecDecoderInfo info{};
        result = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info);
        if (result < 0) return fail(error, "sceAvcdecQueryDecoderMemSize", result);
        const SceSize decoder_size = (info.frameMemSize + 0xFFFFFu) & ~0xFFFFFu;
        decoder_block = sceKernelAllocMemBlock("movie_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, decoder_size, nullptr);
        if (decoder_block < 0) return fail(error, "decoder memory", decoder_block);
        void *decoder_memory = nullptr;
        sceKernelGetMemBlockBase(decoder_block, &decoder_memory);
        ctrl.frameBuf.pBuf = decoder_memory;
        ctrl.frameBuf.size = info.frameMemSize;
        result = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &ctrl, &query);
        if (result < 0) return fail(error, "sceAvcdecCreateDecoder", result);
        picture_block = sceKernelAllocMemBlock("movie_picture", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 0x100000u, nullptr);
        if (picture_block < 0) return fail(error, "picture memory", picture_block);
        sceKernelGetMemBlockBase(picture_block, &picture);
        ready = true;
        return true;
    }

    static bool fail(std::string &error, const char *what, int code) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s failed: 0x%08X", what, static_cast<unsigned>(code));
        error = text;
        return false;
    }
};

SharedDecoder &shared_decoder() {
    static SharedDecoder decoder;
    return decoder;
}

class VitaMovie final : public Movie {
public:
    explicit VitaMovie(std::shared_ptr<const Units> units) : units_(std::move(units)) {}

    bool start(std::string &error) { return shared_decoder().start(error); }

    bool next_frame(std::vector<std::uint8_t> &rgba) override {
        SharedDecoder &decoder = shared_decoder();
        // A decoder may hold frames back (reordering): feed until one comes out.
        while (next_ < units_->size()) {
            const auto &unit = (*units_)[next_++];
            SceAvcdecAu au{};
            au.pts.upper = au.pts.lower = au.dts.upper = au.dts.lower = 0xFFFFFFFFu;
            au.es.pBuf = const_cast<std::uint8_t *>(unit.data());
            au.es.size = static_cast<std::uint32_t>(unit.size());
            SceAvcdecPicture picture{};
            picture.size = sizeof(picture);
            picture.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
            picture.frame.framePitch = SharedDecoder::kPitch;
            picture.frame.frameWidth = kWidth;
            picture.frame.frameHeight = kHeight;
            picture.frame.pPicture[0] = decoder.picture;
            SceAvcdecPicture *pictures[1]{&picture};
            SceAvcdecArrayPicture array{};
            array.numOfElm = 1;
            array.pPicture = pictures;
            if (sceAvcdecDecode(&decoder.ctrl, &au, &array) < 0) continue;
            if (array.numOfOutput == 0u) continue;
            copy_out(decoder.picture, rgba);
            return true;
        }
        return false;
    }

    std::size_t frame_count() const override { return units_->size(); }
    // The first unit is an IDR picture: the decoder restarts from it.
    void rewind() override { next_ = 0; }

private:
    static void copy_out(const void *picture, std::vector<std::uint8_t> &rgba) {
        rgba.resize(static_cast<std::size_t>(kWidth) * kHeight * 4u);
        const auto *source = static_cast<const std::uint8_t *>(picture);
        for (int y = 0; y < kHeight; ++y)
            std::memcpy(rgba.data() + static_cast<std::size_t>(y) * kWidth * 4u,
                        source + static_cast<std::size_t>(y) * SharedDecoder::kPitch * 4u, static_cast<std::size_t>(kWidth) * 4u);
    }

    std::shared_ptr<const Units> units_;
    std::size_t next_{};
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

#if defined(_WIN32) && !defined(__vita__)
// The playable PC build: the H.264 decoder of Windows (Media Foundation).
const GUID kH264Decoder{0x62CE7E72, 0x4C71, 0x4D20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};

template <class T> void release(T *&p) {
    if (p != nullptr) p->Release();
    p = nullptr;
}

class WinMovie final : public Movie {
public:
    explicit WinMovie(std::shared_ptr<const Units> units) : units_(std::move(units)) {}
    ~WinMovie() override { release(decoder_); }

    bool start(std::string &error) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (void)co;
        static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        if (!started) { error = "MFStartup failed"; return false; }
        if (FAILED(CoCreateInstance(kH264Decoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_)))) {
            error = "no H.264 decoder";
            return false;
        }
        IMFMediaType *in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, kWidth, kHeight);
        const HRESULT set_in = decoder_->SetInputType(0, in, 0);
        release(in);
        if (FAILED(set_in)) { error = "H.264 input type refused"; return false; }
        if (!choose_output()) { error = "no NV12 output"; return false; }
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool next_frame(std::vector<std::uint8_t> &rgba) override {
        if (shown_ >= units_->size()) return false;
        rgba.assign(static_cast<std::size_t>(kWidth) * kHeight * 4u, 0u);
        for (std::size_t i = 3; i < rgba.size(); i += 4u) rgba[i] = 0xFFu;
        // Units in until a picture comes out (the decoder holds a few back).
        for (int guard = 0; guard < 64; ++guard) {
            if (take_output(rgba)) {
                ++shown_;
                return true;
            }
            if (next_ < units_->size()) {
                feed((*units_)[next_]);
                ++next_;
            } else if (!drained_) {
                decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                drained_ = true;
            } else {
                break;
            }
        }
        // Nothing decoded: the frame stays black, the movie goes on.
        ++shown_;
        return true;
    }
    std::size_t frame_count() const override { return units_->size(); }
    void rewind() override {
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        next_ = shown_ = 0;
        drained_ = false;
    }

private:
    bool choose_output() {
        for (DWORD i = 0;; ++i) {
            IMFMediaType *type = nullptr;
            if (FAILED(decoder_->GetOutputAvailableType(0, i, &type))) return false;
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12) {
                const HRESULT hr = decoder_->SetOutputType(0, type, 0);
                UINT32 w = kWidth, h = kHeight;
                MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
                UINT32 stride = 0;
                if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || stride == 0) stride = w;
                width_ = static_cast<int>(w);
                height_ = static_cast<int>(h);
                stride_ = static_cast<int>(stride);
                release(type);
                return SUCCEEDED(hr);
            }
            release(type);
        }
    }

    void feed(const std::vector<std::uint8_t> &unit) {
        IMFSample *sample = nullptr;
        IMFMediaBuffer *buffer = nullptr;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(static_cast<DWORD>(unit.size()), &buffer);
        BYTE *data = nullptr;
        buffer->Lock(&data, nullptr, nullptr);
        std::memcpy(data, unit.data(), unit.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(unit.size()));
        sample->AddBuffer(buffer);
        sample->SetSampleTime(static_cast<LONGLONG>(next_) * 333667);
        sample->SetSampleDuration(333667);
        decoder_->ProcessInput(0, sample, 0);
        release(buffer);
        release(sample);
    }

    bool take_output(std::vector<std::uint8_t> &rgba) {
        MFT_OUTPUT_STREAM_INFO info{};
        decoder_->GetOutputStreamInfo(0, &info);
        const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0u;
        MFT_OUTPUT_DATA_BUFFER out{};
        IMFSample *own = nullptr;
        if (!provides) {
            IMFMediaBuffer *buffer = nullptr;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(info.cbSize != 0u ? info.cbSize : static_cast<DWORD>(stride_ * height_ * 3 / 2 + 4096), &buffer);
            own->AddBuffer(buffer);
            release(buffer);
            out.pSample = own;
        }
        DWORD status = 0;
        const HRESULT hr = decoder_->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            release(own);
            choose_output();
            return take_output(rgba);
        }
        if (FAILED(hr)) {
            release(own);
            if (out.pEvents != nullptr) out.pEvents->Release();
            return false;
        }
        IMFSample *sample = out.pSample;
        IMFMediaBuffer *buffer = nullptr;
        if (sample != nullptr && SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
            BYTE *data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                convert(data, length, rgba);
                buffer->Unlock();
            }
            release(buffer);
        }
        if (provides && sample != nullptr) sample->Release();
        release(own);
        if (out.pEvents != nullptr) out.pEvents->Release();
        return true;
    }

    // NV12 (BT.601, studio range) to RGBA, the top-left kWidth x kHeight.
    void convert(const BYTE *data, DWORD length, std::vector<std::uint8_t> &rgba) const {
        const int luma_rows = height_;
        if (static_cast<std::size_t>(stride_) * luma_rows * 3u / 2u > length) return;
        const BYTE *uv_plane = data + static_cast<std::size_t>(stride_) * luma_rows;
        const int w = std::min(width_, kWidth), h = std::min(height_, kHeight);
        for (int y = 0; y < h; ++y) {
            const BYTE *row = data + static_cast<std::size_t>(y) * stride_;
            const BYTE *uv = uv_plane + static_cast<std::size_t>(y / 2) * stride_;
            std::uint8_t *dst = rgba.data() + static_cast<std::size_t>(y) * kWidth * 4u;
            for (int x = 0; x < w; ++x) {
                const int c = row[x] - 16, d = uv[x & ~1] - 128, e = uv[(x & ~1) + 1] - 128;
                const auto clamp = [](int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); };
                dst[x * 4] = clamp((298 * c + 409 * e + 128) >> 8);
                dst[x * 4 + 1] = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
                dst[x * 4 + 2] = clamp((298 * c + 516 * d + 128) >> 8);
                dst[x * 4 + 3] = 0xFFu;
            }
        }
    }

    std::shared_ptr<const Units> units_;
    IMFTransform *decoder_{};
    std::size_t next_{}, shown_{};
    bool drained_{};
    int width_{kWidth}, height_{kHeight}, stride_{kWidth};
};
bool g_windows_decoder{};
#endif

} // namespace

// Demuxed movies are kept (up to ~16 MiB): the menu's and the World Tour
// map's come back at every menu change, and reading and splitting their 3 MB
// files each time stalled the game.
SpinLock g_cache_mutex;  // see detached_thread.hpp
std::map<std::string, std::shared_ptr<const Units>> g_cache;
std::size_t g_cached_bytes = 0;

std::shared_ptr<const Units> cached_units(const std::string &host_path) {
    {
        std::lock_guard lock(g_cache_mutex);
        if (const auto it = g_cache.find(host_path); it != g_cache.end()) return it->second;
    }
    auto parsed = std::make_shared<const Units>(access_units(read_file(host_path)));
    std::size_t bytes = 0;
    for (const auto &unit : *parsed) bytes += unit.size();
    std::lock_guard lock(g_cache_mutex);
    if (const auto it = g_cache.find(host_path); it != g_cache.end()) return it->second;  // prefetched meanwhile
    if (g_cached_bytes + bytes > 16u * 1024u * 1024u) {
        g_cache.clear();
        g_cached_bytes = 0;
    }
    g_cache[host_path] = parsed;
    g_cached_bytes += bytes;
    return parsed;
}

void release_cache() {
    std::size_t bytes = 0;
    {
        std::lock_guard lock(g_cache_mutex);
        bytes = g_cached_bytes;
        g_cache.clear();
        g_cached_bytes = 0;
    }
    if (bytes != 0u) std::fprintf(stderr, "[movie] cache released: %u KiB\n", static_cast<unsigned>(bytes / 1024u));
}

void prefetch(std::vector<std::string> host_paths) {
    start_detached([paths = std::move(host_paths)] {
        for (const auto &path : paths) (void)cached_units(path);
    });
}

std::unique_ptr<Movie> open(const std::string &host_path, std::string &error) {
    std::shared_ptr<const Units> units = cached_units(host_path);
    if (units->empty()) {
        error = "no H.264 access units in " + host_path;
        return nullptr;
    }
#if defined(__vita__)
    auto movie = std::make_unique<VitaMovie>(units);
    if (!movie->start(error)) return nullptr;
    return movie;
#else
#if defined(_WIN32)
    if (g_windows_decoder) {
        auto movie = std::make_unique<WinMovie>(units);
        if (movie->start(error)) return movie;
        std::fprintf(stderr, "[movie] Windows decoder: %s\n", error.c_str());
    }
#endif
    std::string frames;
    if (const char *dir = std::getenv("PSPRECOMP_BURNOUT_MOVIE_FRAMES")) frames = std::string(dir) + "/" + base_name(host_path) + ".rgba";
    return std::make_unique<FileMovie>(units->size(), frames);
#endif
}

} // namespace burnout::movie

#if defined(_WIN32) && !defined(__vita__)
namespace burnout::movie {
void use_windows_decoder() { g_windows_decoder = true; }
} // namespace burnout::movie
#endif

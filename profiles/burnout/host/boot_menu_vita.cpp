// Vita launch screen: a few options drawn with an 8x8 font on a plain
// framebuffer, before vitaGL owns the display.
#include "boot_menu_vita.hpp"

#include "font8x8.hpp"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace burnout::boot_menu {
namespace {

constexpr int kWidth = 960, kHeight = 544;
constexpr std::uint32_t kBackground = 0xFF201010u;  // ABGR
constexpr std::uint32_t kText = 0xFFE0E0E0u;
constexpr std::uint32_t kDim = 0xFF909090u;
constexpr std::uint32_t kAccent = 0xFF0090FFu;      // orange
constexpr std::uint32_t kSelected = 0xFF00E0FFu;    // yellow
constexpr std::uint32_t kHighlight = 0xFF402020u;

bool exists(const std::string &path) {
    if (std::FILE *file = std::fopen(path.c_str(), "r")) {
        std::fclose(file);
        return true;
    }
    return false;
}

class Screen {
public:
    bool open() {
        block_ = sceKernelAllocMemBlock("boot_menu_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                        (kWidth * kHeight * 4 + 0x3FFFF) & ~0x3FFFF, nullptr);
        if (block_ < 0) return false;
        void *base = nullptr;
        if (sceKernelGetMemBlockBase(block_, &base) < 0) return false;
        pixels_ = static_cast<std::uint32_t *>(base);
        return true;
    }
    void clear() {
        for (int i = 0; i < kWidth * kHeight; ++i) pixels_[i] = kBackground;
    }
    void fill(int x, int y, int w, int h, std::uint32_t colour) {
        for (int row = std::max(y, 0); row < y + h && row < kHeight; ++row)
            for (int col = std::max(x, 0); col < x + w && col < kWidth; ++col) pixels_[row * kWidth + col] = colour;
    }
    // `scale` pixels per font pixel; x and y in screen pixels.
    void text(int x, int y, const std::string &string, std::uint32_t colour, int scale) {
        for (const char c : string) {
            const unsigned char code = static_cast<unsigned char>(c);
            if (code >= font::kFirst && code <= font::kLast) {
                const std::uint8_t *glyph = font::kGlyphs[code - font::kFirst];
                for (int row = 0; row < 8; ++row)
                    for (int bit = 0; bit < 8; ++bit)
                        if (glyph[row] & (0x80u >> bit)) fill(x + bit * scale, y + row * scale, scale, scale, colour);
            }
            x += 8 * scale;
        }
    }
    void show() {
        SceDisplayFrameBuf frame{};
        frame.size = sizeof(frame);
        frame.base = pixels_;
        frame.pitch = kWidth;
        frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        frame.width = kWidth;
        frame.height = kHeight;
        sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    }

private:
    SceUID block_{-1};
    std::uint32_t *pixels_{};
};

struct Item {
    const char *label;
    std::vector<const char *> choices;
    int *value;
    const char *help;
};

int flag(bool value) { return value ? 1 : 0; }

} // namespace

Settings load(const std::string &root) {
    Settings settings;
    if (std::FILE *file = std::fopen((root + "settings.txt").c_str(), "r")) {
        char key[64];
        int value = 0;
        while (std::fscanf(file, "%63s %d", key, &value) == 2) {
            const std::string name = key;
            if (name == "auto_race") settings.auto_race = value != 0;
            else if (name == "render_scale") settings.render_scale = value == 1 ? 1 : 2;
            else if (name == "sound") settings.sound = value != 0;
            else if (name == "frame_skip") settings.frame_skip = value != 0;
            else if (name == "async_ge") settings.async_ge = value != 0;
            else if (name == "shader_cache") settings.shader_cache = value != 0;
            else if (name == "cached_gpu_memory") settings.cached_gpu_memory = value != 0;
        }
        std::fclose(file);
        return settings;
    }
    // Before the menu existed, each option was a flag file.
    settings.sound = !exists(root + "no_sound.txt");
    settings.async_ge = exists(root + "async_ge.txt");
    if (std::FILE *scale = std::fopen((root + "render_scale.txt").c_str(), "r")) {
        int value = 2;
        if (std::fscanf(scale, "%d", &value) == 1) settings.render_scale = value == 1 ? 1 : 2;
        std::fclose(scale);
    }
    return settings;
}

void save(const std::string &root, const Settings &settings) {
    std::FILE *file = std::fopen((root + "settings.txt").c_str(), "w");
    if (file == nullptr) return;
    std::fprintf(file, "auto_race %d\nrender_scale %d\nsound %d\nframe_skip %d\nasync_ge %d\nshader_cache %d\n"
                       "cached_gpu_memory %d\n",
                 flag(settings.auto_race), settings.render_scale, flag(settings.sound), flag(settings.frame_skip),
                 flag(settings.async_ge), flag(settings.shader_cache), flag(settings.cached_gpu_memory));
    std::fclose(file);
}

Settings run(const std::string &root, const std::string &build) {
    Settings settings = load(root);
    Screen screen;
    if (!screen.open()) return settings;

    // Each value is the index of the choice shown.
    int mode = flag(settings.auto_race);
    int resolution = settings.render_scale == 1 ? 1 : 0;
    int sound = flag(!settings.sound);
    int skip = flag(!settings.frame_skip);
    int shaders = flag(settings.shader_cache);
    int cached = flag(settings.cached_gpu_memory);
    int async = flag(settings.async_ge);
    int start = 0;
    const std::vector<Item> items = {
        {"Mode", {"Jeu libre", "Course test auto"}, &mode, "Course test: menus joues seuls jusqu'a une course."},
        {"Resolution", {"960x544 (Vita)", "480x272 (PSP)"}, &resolution, "480x272: soulage le GPU, image plus floue."},
        {"Son/musique", {"Oui", "Non"}, &sound, "Sans le son, le jeu tourne un peu plus vite."},
        {"Saut d'images", {"Oui", "Non"}, &skip, "Saute une image si le rendu prend du retard."},
        {"Cache shaders", {"Non", "Oui (exp.)"}, &shaders, "Garde les shaders compiles: demarrage rapide."},
        {"Memoire GPU", {"Normale", "En cache (exp.)"}, &cached, "Experimental: plus rapide, glitchs possibles."},
        {"GE parallele", {"Non", "Oui (exp.)"}, &async, "Experimental, pas plus rapide pour l'instant."},
        {"> LANCER LE JEU <", {}, &start, "X ou START pour lancer."},
    };

    constexpr int kTop = 96, kRow = 34;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    int cursor = static_cast<int>(items.size()) - 1;
    std::uint32_t held = 0xFFFFFFFFu;  // ignore buttons already down at launch
    bool dirty = true, launch = false;
    while (!launch) {
        if (dirty) {
            screen.clear();
            screen.text(16, 16, "BURNOUT LEGENDS", kAccent, 4);
            screen.text(32, 60, "recomp PS Vita - " + build, kDim, 2);
            for (std::size_t i = 0; i < items.size(); ++i) {
                const Item &item = items[i];
                const int y = kTop + static_cast<int>(i) * kRow;
                const bool selected = static_cast<int>(i) == cursor;
                const std::uint32_t colour = selected ? kSelected : kText;
                if (selected) screen.fill(0, y - 5, kWidth, 34, kHighlight);
                if (selected) screen.text(8, y, ">", kSelected, 3);
                screen.text(40, y, item.label, colour, 3);
                if (!item.choices.empty())
                    screen.text(390, y, std::string("< ") + item.choices[*item.value] + " >", colour, 3);
            }
            screen.text(32, 384, items[cursor].help, kDim, 2);
            screen.text(32, 456, "Haut/Bas: option  Gauche/Droite: changer  START: lancer", kDim, 2);
            screen.text(32, 488, "En jeu: START+SELECT ensemble pour quitter", kDim, 2);
            screen.show();
            dirty = false;
        }
        sceDisplayWaitVblankStart();
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const std::uint32_t pressed = pad.buttons & ~held;
        held = pad.buttons;
        if (pressed == 0u) continue;
        dirty = true;
        const int count = static_cast<int>(items.size());
        if (pressed & SCE_CTRL_UP) cursor = (cursor + count - 1) % count;
        if (pressed & SCE_CTRL_DOWN) cursor = (cursor + 1) % count;
        const Item &item = items[cursor];
        if (!item.choices.empty()) {
            const int choices = static_cast<int>(item.choices.size());
            if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) *item.value = (*item.value + 1) % choices;
            if (pressed & SCE_CTRL_LEFT) *item.value = (*item.value + choices - 1) % choices;
        } else if (pressed & SCE_CTRL_CROSS) launch = true;
        if (pressed & SCE_CTRL_START) launch = true;
    }

    settings.auto_race = mode == 1;
    settings.render_scale = resolution == 1 ? 1 : 2;
    settings.sound = sound == 0;
    settings.frame_skip = skip == 0;
    settings.shader_cache = shaders == 1;
    settings.cached_gpu_memory = cached == 1;
    settings.async_ge = async == 1;
    save(root, settings);

    screen.clear();
    screen.text(32, 220, "Chargement...", kAccent, 4);
    screen.show();
    // The block stays allocated: the display shows it until vitaGL sets its
    // own frame buffers (2 MiB of CDRAM).
    sceDisplayWaitVblankStart();
    return settings;
}

} // namespace burnout::boot_menu

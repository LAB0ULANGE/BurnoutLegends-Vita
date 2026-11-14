// Burnout Legends 2.0 on Windows: the recompiled game in a window, played with
// a pad (XInput) or the keyboard, with sound, as the Vita build plays it (the
// same renderer, Bonus 2.0 menu, paints, Signature Takedown awards).
//
// Next to the executable:
//   data/           the game files, made at the first launch from the
//                   player's own disc image (ULES00125 .iso or .cso put next
//                   to the executable): EBOOT_DECRYPTED.ELF, PARAM.SFO,
//                   USRDIR/, music_ima/; unlocks.bin (Bonus progress),
//                   memstick/PSP/SAVEDATA (the saves, PSP format).
//   postcards/, signatures/, fonts/   the Bonus menu's pictures and font.
//   controls.ini    the keyboard (made at the first launch, editable).
//   burnout_pc.log  this session's log.
#include "audio_out.hpp"
#include "bonus.hpp"
#include "bonus_menu.hpp"
#include "burnout_hle.hpp"
#include "burnout_run.hpp"
#include "filters.hpp"
#include "game_setup.hpp"
#include "ge_gl.hpp"
#include "ge_render.hpp"
#include "gl_api.hpp"
#include "movie.hpp"
#include "trax.hpp"
#include "music.hpp"
#include "overlay.hpp"
#include "paints.hpp"
#include "texture_pack.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char *kVersion = "0.1 (PC, from Vita 0.0.65)";

// PSP pad bits.
constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u,
                        kL = 0x0100u, kR = 0x0200u, kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

std::string g_root;  // <exe dir>/data/
std::string g_exe_dir;

// ---- keyboard (controls.ini) --------------------------------------------------

struct Binding {
    const char *name;
    std::uint32_t button;
    std::vector<int> keys;  // virtual-key codes
};

std::vector<Binding> g_bindings{
    {"cross", kCross, {VK_SPACE, 'Z'}},
    {"square", kSquare, {'X'}},
    {"circle", kCircle, {VK_BACK, 'C'}},
    {"triangle", kTriangle, {'V'}},
    {"l", kL, {'A', 'Q'}},
    {"r", kR, {'E', VK_SHIFT}},
    {"start", kStart, {VK_RETURN, VK_ESCAPE}},
    {"select", kSelect, {VK_TAB}},
    {"up", kUp, {VK_UP}},
    {"down", kDown, {VK_DOWN}},
    {"left", kLeft, {VK_LEFT}},
    {"right", kRight, {VK_RIGHT}},
};

std::string key_name(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    static const std::map<int, std::string> names{
        {VK_SPACE, "SPACE"}, {VK_RETURN, "ENTER"}, {VK_ESCAPE, "ESCAPE"}, {VK_BACK, "BACKSPACE"}, {VK_TAB, "TAB"},
        {VK_SHIFT, "SHIFT"}, {VK_CONTROL, "CTRL"}, {VK_MENU, "ALT"}, {VK_UP, "UP"}, {VK_DOWN, "DOWN"},
        {VK_LEFT, "LEFT"}, {VK_RIGHT, "RIGHT"}, {VK_NUMPAD0, "NUM0"}, {VK_NUMPAD1, "NUM1"}, {VK_NUMPAD2, "NUM2"},
        {VK_NUMPAD3, "NUM3"}, {VK_NUMPAD4, "NUM4"}, {VK_NUMPAD5, "NUM5"}, {VK_NUMPAD6, "NUM6"}, {VK_NUMPAD7, "NUM7"},
        {VK_NUMPAD8, "NUM8"}, {VK_NUMPAD9, "NUM9"}};
    const auto it = names.find(vk);
    return it != names.end() ? it->second : std::to_string(vk);
}

int key_code(std::string name) {
    for (char &c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (name.size() == 1u && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    for (int vk = 1; vk < 256; ++vk)
        if (key_name(vk) == name) return vk;
    return 0;
}

void load_controls() {
    const std::string path = g_exe_dir + "controls.ini";
    std::ifstream in(path);
    if (!in) {
        std::ofstream out(path);
        out << "# Burnout Legends PC - keyboard. One line per PSP button: button = KEY, KEY\n"
               "# Keys: A-Z, 0-9, SPACE, ENTER, ESCAPE, BACKSPACE, TAB, SHIFT, CTRL, ALT, UP, DOWN, LEFT, RIGHT, NUM0-NUM9.\n"
               "# A pad (Xbox / XInput) works too: A = cross, X = square, B = circle, Y = triangle,\n"
               "# LB/LT = l, RB/RT = r, Start, Back = select, D-pad and left stick.\n"
               "# F11 or Alt+Enter: full screen. F12: screenshot (screenshots/).\n";
        for (const Binding &b : g_bindings) {
            out << b.name << " =";
            for (std::size_t i = 0; i < b.keys.size(); ++i) out << (i == 0u ? " " : ", ") << key_name(b.keys[i]);
            out << "\n";
        }
        return;
    }
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
        for (Binding &b : g_bindings) {
            if (name != b.name) continue;
            b.keys.clear();
            std::stringstream list(line.substr(eq + 1u));
            for (std::string key; std::getline(list, key, ',');) {
                key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
                if (const int vk = key_code(key)) b.keys.push_back(vk);
                else if (!key.empty()) std::fprintf(stderr, "[pc] controls.ini: unknown key '%s' for %s\n", key.c_str(), b.name);
            }
        }
    }
}

// ---- settings.ini ----------------------------------------------------------------

struct Settings {
    int resolution{4};     // the picture: 480x272 times resolution / 2 (2 = the Vita's 960x544, 4 = 1920x1088)
    bool fullscreen{true};
    bool fxaa{true};
    bool sound{true};
};
Settings g_settings;

void load_settings() {
    const std::string path = g_exe_dir + "settings.ini";
    std::ifstream in(path);
    if (!in) {
        std::ofstream out(path);
        out << "# Burnout Legends PC - settings\n"
               "# resolution: 2 = 960x544 (the Vita), 3 = 1440x816, 4 = 1920x1088, 6 = 2880x1632, 8 = 3840x2176\n"
               "resolution = 4\n"
               "# fullscreen: 1 = full screen at launch (F11 or Alt+Enter switches), 0 = window\n"
               "fullscreen = 1\n"
               "# fxaa: smoothing of the edges in races, 1 = on\n"
               "fxaa = 1\n"
               "# sound: 1 = on\n"
               "sound = 1\n"
               "# all_colours: 1 = the cars' 8 colours offered without the Takedown Trophies\n"
               "all_colours = 0\n"
               "# fps60: 1 = 60 frames a second in races too (the game's own 60 mode)\n"
               "fps60 = 0\n";
        return;
    }
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1u);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }), value.end());
        const int number = std::atoi(value.c_str());
        if (key == "resolution") g_settings.resolution = std::clamp(number, 1, 8);
        else if (key == "fullscreen") g_settings.fullscreen = number != 0;
        else if (key == "fxaa") g_settings.fxaa = number != 0;
        else if (key == "sound") g_settings.sound = number != 0;
        else if (key == "all_colours") burnout::paints::set_all_unlocked(number != 0);
        else if (key == "all_cars") burnout::bonus_menu::set_all_cars(number != 0);
        else if (key == "fps60") burnout::set_sixty_fps(number != 0);
    }
}

// ---- XInput pad -----------------------------------------------------------------

struct XInputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XInputState {
    DWORD dwPacketNumber;
    XInputGamepad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XInputState *);
XInputGetStateFn g_xinput_get_state{};

void load_xinput() {
    for (const char *dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE module = LoadLibraryA(dll)) {
            g_xinput_get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (g_xinput_get_state != nullptr) {
                std::fprintf(stderr, "[pc] pad support: %s\n", dll);
                return;
            }
        }
    std::fprintf(stderr, "[pc] no XInput: keyboard only\n");
}

bool pad_state(XInputGamepad &pad) {
    if (g_xinput_get_state == nullptr) return false;
    // Re-polled at most 4 times a second for pads not connected (the call is slow then).
    static DWORD connected = 0xFFFFFFFFu;
    static double next_scan = 0.0;
    const double now = GetTickCount64();
    for (DWORD i = 0; i < 4; ++i) {
        if (connected != 0xFFFFFFFFu && connected != i) continue;
        if (connected == 0xFFFFFFFFu && now < next_scan) return false;
        XInputState state{};
        if (g_xinput_get_state(i, &state) == 0u) {
            if (connected != i) std::fprintf(stderr, "[pc] pad %lu connected\n", static_cast<unsigned long>(i));
            connected = i;
            pad = state.Gamepad;
            return true;
        }
        if (connected == i) {
            std::fprintf(stderr, "[pc] pad %lu disconnected\n", static_cast<unsigned long>(i));
            connected = 0xFFFFFFFFu;
        }
    }
    next_scan = now + 250.0;
    return false;
}

std::uint32_t live_buttons() {
    std::uint32_t buttons = 0u;
    if (burnout::gl::window_focused()) {
        for (const Binding &b : g_bindings)
            for (const int vk : b.keys)
                if ((GetAsyncKeyState(vk) & 0x8000) != 0) buttons |= b.button;
        static bool f12 = false;
        const bool down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (down && !f12) burnout::render::request_capture();
        f12 = down;
    }
    XInputGamepad pad{};
    if (pad_state(pad)) {
        struct Map { WORD bit; std::uint32_t button; };
        static const Map maps[]{{0x1000, kCross}, {0x4000, kSquare}, {0x2000, kCircle}, {0x8000, kTriangle},
                                {0x0100, kL},     {0x0200, kR},      {0x0010, kStart},  {0x0020, kSelect},
                                {0x0001, kUp},    {0x0002, kDown},   {0x0004, kLeft},   {0x0008, kRight}};
        for (const Map &m : maps)
            if ((pad.wButtons & m.bit) != 0u) buttons |= m.button;
        if (pad.bLeftTrigger > 100) buttons |= kL;
        if (pad.bRightTrigger > 100) buttons |= kR;
    }
    return buttons & 0xF3F9u;
}

std::uint32_t live_analog() {
    XInputGamepad pad{};
    if (!pad_state(pad)) return 128u | (128u << 8u);
    const auto axis = [](int value, bool invert) {
        if (std::abs(value) < 7849) value = 0;  // XInput's left stick dead zone
        if (invert) value = -value - 1;
        return static_cast<std::uint32_t>(std::clamp((value + 32768) >> 8, 0, 255));
    };
    return axis(pad.sThumbLX, false) | (axis(pad.sThumbLY, true) << 8u);
}

// ---- Bonus 2.0 (as main_vita.cpp) --------------------------------------------------

burnout::bonus::State g_bonus;
void apply_bonus() {
    static burnout::bonus::State saved = g_bonus;
    burnout::gl::set_filters(burnout::bonus::applied(g_bonus));
    burnout::gl::set_antialias(g_settings.fxaa);  // FXAA in races (the full version: the PC has the room)
    std::fprintf(stderr, "[bonus] filters: %s\n", burnout::filters::describe(burnout::bonus::applied(g_bonus)).c_str());
    if (!(saved == g_bonus)) {
        burnout::bonus::save(g_root + "unlocks.bin", g_bonus);
        saved = g_bonus;
    }
}

void configure_bonus() {
    burnout::overlay::init(g_exe_dir + "fonts/Oswald-Bold.ttf");
    burnout::bonus_menu::configure({
        [] { return g_bonus.enabled; },
        [] { return static_cast<int>(burnout::filters::all().size()); },
        [](int i) { return burnout::filters::all()[static_cast<std::size_t>(i)].name; },
        [](int i) { return burnout::bonus::available(g_bonus, i); },
        [](int i) {
            if (!g_bonus.enabled) return false;
            for (const auto &id : g_bonus.active)
                if (burnout::filters::find(id) == i) return true;
            return false;
        },
        [](int i) {
            if (!g_bonus.enabled) g_bonus.active = {};
            burnout::bonus::toggle(g_bonus, i);
            apply_bonus();
        },
        [](int i) {
            const int signature = burnout::bonus::filter_signature(i);
            return std::string("LOCKED: ") + burnout::bonus::signature_name(signature);
        },
        [](int signature) {
            const int filter = burnout::bonus::signature_filter(signature);
            if (!burnout::bonus::unlock(g_bonus, filter)) return false;
            std::fprintf(stderr, "[bonus] Signature Takedown %s: %s unlocked\n", burnout::bonus::signature_name(signature),
                         burnout::filters::all()[static_cast<std::size_t>(filter)].name);
            apply_bonus();
            return true;
        },
        [](int i) {
            const int signature = burnout::bonus::filter_signature(i);
            return signature < 0 ? std::string() : std::string(burnout::bonus::signature_name(signature));
        },
        [](int i, float x, float y, float w, float h, float angle) {
            burnout::gl::set_postcard(i >= 0 ? std::vector<int>{i} : std::vector<int>{}, x, y, w, h, angle);
        },
        [] { burnout::gl::postcard_photo(); },
        [](int i) {
            const burnout::bonus::Reward reward = burnout::bonus::filter_reward(i);
            return reward.source == burnout::bonus::Source::Free ? -1 : static_cast<int>(reward.source) * 100 + reward.index;
        },
        [](float mph) { burnout::gl::set_car_speed(mph); },
        [](int postcards, int headlines, int percent) {
            const int unlocked = burnout::bonus::unlock_progress(g_bonus, postcards, headlines, percent);
            if (unlocked == 0) return 0;
            std::fprintf(stderr, "[bonus] progress %d postcards, %d headlines, %d %%: %d filters unlocked\n", postcards, headlines,
                         percent, unlocked);
            apply_bonus();
            return unlocked;
        },
        {},
        {},
        {},
        [](bool up) { burnout::gl::set_filters_suspended(up); },
        [](int slot, const std::string &path, float x, float y, float w, float h, float angle) {
            burnout::gl::set_picture(slot, path.empty() ? path : g_exe_dir + path, x, y, w, h, angle);
        },
        [](bool hide) { burnout::gl::set_hide_3d(hide); },
    });
    // Tests: toutes_les_recompenses.txt beside the exe arms "every reward at
    // the end of the next race" (the save copied aside first, the file renamed).
    if (fs::exists(g_exe_dir + "toutes_les_recompenses.txt")) {
        burnout::bonus_menu::force_all_rewards([] {
            std::error_code e;
            const fs::path saves = g_root + "memstick/PSP/SAVEDATA", backup = g_exe_dir + "sauvegardes_avant_toutes_recompenses";
            fs::create_directories(backup, e);
            for (const auto &entry : fs::directory_iterator(saves, e))
                fs::copy(entry.path(), backup / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, e);
            fs::rename(g_exe_dir + "toutes_les_recompenses.txt", g_exe_dir + "toutes_les_recompenses_fait.txt", e);
        });
    }
    burnout::bonus_menu::set_picture_ready([](int slot) { return burnout::gl::picture_ready(slot); });
    burnout::bonus_menu::set_signature_photos(g_exe_dir + "signatures", [](const std::string &path, float x, float y, float w, float h, float angle) {
        burnout::gl::set_photo(path, x, y, w, h, angle);
    });
    burnout::paints::set_texels_written([](std::uint32_t address, std::uint32_t bytes) { burnout::render::cpu_wrote_vram(address, bytes); });
}

// ---- first launch -----------------------------------------------------------------

bool install_if_needed() {
    if (burnout::setup::installed(g_root)) return true;
    // A console for the progress (the game itself has none: windows subsystem).
    if (AllocConsole()) {
        std::freopen("CONOUT$", "w", stdout);
        SetConsoleTitleA("Burnout Legends - first launch");
    }
    std::printf("Burnout Legends PC - first launch: making the game files from your disc image...\n");
    std::fprintf(stderr, "[pc] not installed: looking for a disc image\n");
    const std::vector<std::string> folders{g_exe_dir, g_exe_dir + "iso", g_exe_dir + ".."};
    int last = -1;
    std::string last_step;
    const auto outcome = burnout::setup::install(g_root, folders, [&](const std::string &step, const std::string &detail, double f) {
        const int pct = static_cast<int>(f * 100.0);
        if (pct == last && step == last_step) return;
        last = pct;
        last_step = step;
        std::printf("\r%-40s %-30.30s %3d%%", step.c_str(), detail.c_str(), pct);
        std::fflush(stdout);
    });
    std::printf("\n");
    std::fprintf(stderr, "[pc] install: %d %s (%s)\n", static_cast<int>(outcome.result), outcome.message.c_str(), outcome.image.c_str());
    if (outcome.result == burnout::setup::Result::Ready) return true;
    std::string text;
    switch (outcome.result) {
    case burnout::setup::Result::NoImage:
        text = "No Burnout Legends disc image found.\n\nPut your own copy of the game (ULES00125 Europe or ULUS10025 USA, .iso or .cso) next to "
               "burnout_legends.exe and launch it again.";
        break;
    case burnout::setup::Result::WrongVersion:
        text = "This disc image is not a supported Burnout Legends (ULES00125 Europe or ULUS10025 USA):\n" + outcome.image;
        break;
    default: text = "The game files could not be made:\n" + outcome.message; break;
    }
    MessageBoxA(nullptr, text.c_str(), "Burnout Legends", MB_ICONERROR | MB_OK);
    return false;
}

} // namespace

int main(int argc, char **argv) {
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_exe_dir = fs::path(exe).parent_path().string() + "\\";
    g_root = g_exe_dir + "data/";
    std::error_code ec;
    fs::create_directories(g_root, ec);
    const std::string log = g_exe_dir + "burnout_pc.log";
    std::remove((log + ".prev").c_str());
    std::rename(log.c_str(), (log + ".prev").c_str());
    std::freopen(log.c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[pc] Burnout Legends PC %s\n", kVersion);
    // Options (tests): --windowed, --max-vblanks N, --script FILE (the PC
    // runner's input scripts), --capture V1,V2 (screenshots at those vblanks).
    bool windowed = false;
    std::uint64_t max_vblanks = UINT64_MAX;
    std::string script_path;
    std::set<std::uint64_t> captures;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (key == "--windowed") windowed = true;
        else if (key == "--max-vblanks") max_vblanks = std::stoull(value());
        else if (key == "--script") script_path = value();
        else if (key == "--capture") {
            std::stringstream list(value());
            for (std::string item; std::getline(list, item, ',');) captures.insert(std::stoull(item));
        }
    }
    try {
        if (!install_if_needed()) return 1;
        load_settings();
        load_controls();
        load_xinput();

        burnout::BenchOptions options;
        options.elf = g_root + "EBOOT_DECRYPTED.ELF";
        options.usrdir = g_root + "USRDIR";
        options.disc_root = g_root;
        options.memstick = g_root + "memstick";
        options.title = "Burnout Legends PC";
        options.max_vblanks = max_vblanks;
        options.realtime = true;
        if (!script_path.empty()) options.script.load(script_path);
        else options.script.parse_line("hold   120   126  START");  // the intro movie skipped, as on the Vita
        options.live_buttons = live_buttons;
        options.live_analog = live_analog;
        options.quit_requested = [] { return burnout::gl::window_closed(); };

        // Tests: guest RAM dumps, PSPRECOMP_BURNOUT_DUMP_RAM=v1,v2,... into dump_ram/.
        if (const char *dump = std::getenv("PSPRECOMP_BURNOUT_DUMP_RAM")) {
            std::string list = std::string(dump) + ",";
            for (std::size_t at = 0, comma; (comma = list.find(',', at)) != std::string::npos; at = comma + 1u)
                if (comma > at) options.ram_dump_vblanks.push_back(std::stoull(list.substr(at, comma - at)));
            options.ram_dump_dir = "dump_ram";
            std::filesystem::create_directories(options.ram_dump_dir);
        }
        configure_bonus();
        options.per_vblank = [](psprecomp::GuestMemory &memory) {
            burnout::bonus_menu::update(memory);
            if (burnout::paints::chosen() != g_bonus.paint) {
                g_bonus.paint = burnout::paints::chosen();
                apply_bonus();
            }
        };
        options.button_filter = [](std::uint32_t buttons) { return burnout::bonus_menu::filter_buttons(buttons); };
        options.guest_call = [](std::uint32_t &f, std::uint32_t &a0, std::uint32_t &a1) { return burnout::bonus_menu::take_guest_call(f, a0, a1); };

        // The window and the renderer (as the Vita: 960x544 targets, HD).
        burnout::gl::set_window("Burnout Legends", 1280, 725, g_settings.fullscreen && !windowed);
        burnout::gl::set_present_size(240 * g_settings.resolution, 136 * g_settings.resolution);
        burnout::render::Options render;
        render.enabled = true;
        render.backend = burnout::render::Backend::OpenGL;
        render.render_scale = static_cast<float>(g_settings.resolution) / 2.0f;
        render.capture_dir = g_exe_dir + "screenshots";
        render.capture_vblanks = captures;
        burnout::gl::set_postcard_pictures(g_exe_dir + "postcards");
        burnout::gl::set_sun_glare(0.5f);
        g_bonus = burnout::bonus::load(g_root + "unlocks.bin");
        // Tests: every Special Event filter available (not kept in unlocks.bin).
        if (std::getenv("PSPRECOMP_BURNOUT_ALL_FILTERS") != nullptr) g_bonus.test_unlock_all = true;
        g_bonus.enabled = true;
        g_bonus.active = {};
        burnout::paints::set_chosen(g_bonus.paint);
        apply_bonus();
        for (const char *id : {"ULES00125", "ULUS10025", ""})
            if (burnout::texture_pack::open(g_root + "textures/" + id)) break;
        burnout::render::configure(render);
        if (!burnout::render::active()) throw std::runtime_error("OpenGL could not start (see burnout_pc.log)");
        burnout::movie::use_windows_decoder();
        burnout::movie::prefetch({g_root + "USRDIR/ovid/FEMain.pmf", g_root + "USRDIR/ovid/startB.pmf", g_root + "USRDIR/ovid/WTour.pmf"});

        std::string sound_error;
        if (!g_settings.sound) {
            std::fprintf(stderr, "[pc] sound off (settings.ini)\n");
        } else if (burnout::audio::start_output(sound_error)) {
            const int tracks = burnout::music::set_dir(g_root + "music_ima");
            std::fprintf(stderr, "[pc] sound on, %d music tracks\n", tracks);
        } else {
            std::fprintf(stderr, "[pc] sound unavailable: %s\n", sound_error.c_str());
        }

        // The player's own music in EA Trax (trax.cpp): MP3s in musique_perso next to the game.
        burnout::trax::configure(g_exe_dir + "musique_perso", g_exe_dir + "musique_perso.txt");
        const burnout::BenchResult result = burnout::run_benchmark(options);
        std::fprintf(stderr, "[pc] stopped: %s\n", result.stop_reason.c_str());
        if (burnout::audio::enabled()) {
            const auto sound = burnout::audio::counters();
            std::fprintf(stderr, "[pc] sound: %llu frames played, %llu submitted, %llu underrun, %llu dropped\n",
                         static_cast<unsigned long long>(sound.played_frames), static_cast<unsigned long long>(sound.submitted_frames), static_cast<unsigned long long>(sound.underrun_frames),
                         static_cast<unsigned long long>(sound.dropped_frames));
        }
        burnout::audio::stop();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[pc] error: %s\n", e.what());
        MessageBoxA(nullptr, e.what(), "Burnout Legends", MB_ICONERROR | MB_OK);
        return 1;
    }
    std::fflush(stderr);
    // Threads (renderer, sound, file readers) are not joined: the process ends here.
    ExitProcess(0);
}

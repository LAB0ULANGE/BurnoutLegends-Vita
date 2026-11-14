#include "trax.hpp"
#include "bonus_menu.hpp"

#include "bonus.hpp"
#include "filters.hpp"
#include "overlay.hpp"
#include "save_dialog.hpp"

#include <psprecomp/guest_memory.hpp>

#include "paints.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace burnout::bonus_menu {
int heavy_rank(std::uint32_t lo, std::uint32_t hi);  // below: a Heavyweight's place in the unlock order
namespace {

// The Bonus filters inside the game's own menus (PC RAM dumps, ULES00125):
//   Pause:      RESUME RESTART OPTIONS QUIT
//     OPTIONS:  AUDIO CONTROLS BONUS
//   Main menu:  WORLD TOUR SINGLE EVENT MULTIPLAYER DRIVER DETAILS BONUS
//   BONUS:      the filters, as many rows as the menu shows, scrolling
// The game draws every row (its font, bars, cursor); the filter under the
// cursor is shown on a postcard beside the menu (a photo of the race through
// that filter, drawn by the renderer; paper and caption are overlay).
//
// A list is u16 cursor, u16 count, then {u32 label (UTF-16), u32 flags} per
// row. One view (pointer at kWidgetView) shows the list on screen: +0x0C rows
// on screen (at most 5: one item each), +0x10 scroll, +0x14 the list, +0x18
// its rows, +0x1C the row text items. A list longer than the rows on screen
// scrolls by itself. Nothing is ever added to the game's scene (a sixth item
// froze it) and only items whose kind was checked are written (0.0.3 wrote a
// fixed address that in a race belongs to another item: it crashed).
constexpr std::uint32_t kWidgetCursor = 0x08D82E6Cu;      // the menu's cursor (-1 while it opens)
constexpr std::uint32_t kWidgetDefinition = 0x08D82E70u;  // 0 when no menu is up
constexpr std::uint32_t kWidgetView = 0x08D82EA8u;
constexpr std::uint32_t kPause = 0x08D79438u;
// The table after the pause list (6, 0, 1, 3, 4, 5) tells it apart; its first
// word is 0 while a YES/NO box is up.
constexpr std::uint32_t kPauseSignature = 0x08D79474u;
constexpr std::uint32_t kPauseSignatureWords[6]{6u, 0u, 1u, 3u, 4u, 5u};
constexpr std::uint32_t kGameRows = 5u;  // the pause menu's
// The main menu: its list (4 rows) is followed by its rows' descriptions; the
// screen object holds both (+0 the list, +4 the descriptions).
constexpr std::uint32_t kMainList = 0x08D79400u;
constexpr std::uint32_t kMainDescriptions = 0x08D79424u;
constexpr std::uint32_t kMainScreen = 0x08D830C4u;
constexpr std::uint32_t kMainRows = 4u;
// BONUS LEGENDS 2.0 takes MULTIPLAYER's place (AdHoc only, not supported:
// 2026-11-09), so the menu keeps its four rows.
constexpr std::uint32_t kBonusRow = 2u;

// The UI items (0x60 bytes in a pool): +0x00 x, +0x04 y, +0x08 w, +0x0C h
// (floats, 682.7x480 space), +0x24 a text item's string, +0x48 the item's
// draw function: which kind it is.
constexpr std::uint32_t kPool = 0x08BD0000u, kPoolEnd = 0x08BD3600u, kItem = 0x60u;
constexpr std::uint32_t kTextItem = 0x08A5A158u;   // a row's text
constexpr std::uint32_t kBarItem = 0x0893BD4Cu;    // a row's bar
constexpr std::uint32_t kArrowItem = 0x0893B4BCu;  // the up/down arrows at the end of the selected bar
constexpr float kOffScreen = 2000.0f;

// The game's strings (Global??.bin, loaded in RAM with absolute pointers:
// +0 magic, +8 count, +12 the pointer table). Found from a menu label.
constexpr std::uint32_t kStringsMagic0 = 0x69F60308u, kStringsMagic1 = 0xB93622FAu;
constexpr std::uint32_t kStringPostcard = 1755u, kStringHeadlines = 1202u;
// The Crash Headlines' own titles (game strings 742-751, "ALPINE SMASH"...,
// in the game's language), in our order (the award pictures': Waterfront,
// Downtown, Silver Lake, Winter City, Alpine, Riviera, Vineyard, Golden City,
// Dockside, Island Paradise).
constexpr std::uint32_t kHeadlineTitles[10]{750u, 744u, 748u, 751u, 742u, 747u, 749u, 745u, 743u, 746u};
constexpr std::uint32_t kStringOptions = 40u, kStringBonus = 122u, kStringOff = 2955u, kStringOn = 2956u, kStringNew = 2166u,
                        kStringSignatures = 1756u;
// The Signature Takedowns' names, in Takedown Book order.
constexpr std::uint32_t kSignatureStrings[20]{2240u, 2241u, 2229u, 2230u, 2245u, 2246u, 2247u, 2201u, 2238u, 2242u,
                                              2243u, 2232u, 2233u, 2228u, 2235u, 2236u, 2199u, 2198u, 2234u, 2237u};

// The live profile (same layout as DATA.BIN): Signature Takedowns, one byte
// each from +0xF61E (Takedown Book order), after 64 bytes of 0xFF at +0xF5C0.
constexpr std::uint32_t kProfile = 0x08DC31D0u;
constexpr std::uint32_t kProfileSignatures = kProfile + 0xF61Eu;
constexpr std::uint32_t kProfileMarker = kProfile + 0xF5C0u;
// The game's per-car "unlocked" flags in the profile (0x08DD2368 + 1188, one
// byte per car of the car table): the garage (0x08817E98) counts a car whose
// flag is set as unlocked whatever its condition. The Collector cars, won in
// AdHoc Collector Challenges on the PSP, are given here for the Signature
// Takedowns (Burnout_Legends_Recompenses_2.xlsx); the game then counts them,
// announces them ("NEW CAR UNLOCKED") and keeps them in its save.
constexpr std::uint32_t kProfileCarFlags = kProfileSignatures - 1158u + 1188u;
// Car table: count, then 40-byte records whose +28 is the name's string id.
constexpr std::uint32_t kCarCount = 0x08B9BE44u, kCarTable = 0x08B9BE58u;
// The Collector car (name string id) each Signature Takedown gives, in the
// Takedown Book order (kSignatureStrings).
constexpr std::uint32_t kSignatureCars[20]{
    188u,  // Gone Fishin': Muscle Collector 3
    144u,  // Homewrecker: Compact Collector 5
    180u,  // Pillar Driller: Muscle Collector 5
    155u,  // Hit the Split: Coupe Collector 5
    189u,  // Truck Torpedo: Muscle Collector 4
    162u,  // Euro Tram Ram: Coupe Collector 2
    164u,  // Snowed Under: Coupe Collector 3
    165u,  // Paid the Price: Coupe Collector 4
    209u,  // Riviera Roustabout: Sports Collector 2
    211u,  // Gatecrasher: Sports Collector 3
    212u,  // Grapes of Wrath: Sports Collector 4
    223u,  // Market Stalled: Super Collector 2
    204u,  // Tuk-Down: Sports Collector 5
    225u,  // Ship Wreck: Super Collector 3
    226u,  // Rumble in the Jungle: Super Collector 4
    218u,  // Catch the Tour Bus: Super Collector 5
    150u,  // Taxi Takedown: Compact Collector 3
    148u,  // Tow Away Zone: Compact Collector 2
    186u,  // Bridge Over: Muscle Collector 2
    147u,  // Median Mash: Compact Collector 1
};
constexpr std::uint32_t kCarSpeed = 0x08DCF43Cu;  // the player's car, float mph
// The progress counters the Driver Details pages show (the game's counters
// 0x08816FC4 etc. read them): a struct at 0x08BBFDF0.
constexpr std::uint32_t kProgress = 0x08BBFDF0u;
constexpr std::uint32_t kProgressPercent = kProgress + 420u, kProgressSpecialEvents = kProgress + 444u,
                        kProgressHeadlines = kProgress + 680u;
// Each Crash Headline won (2026-11-11, from the game's count at 0x08818190:
// profile + 1178 + i, copied to the progress table + 364 + i), i in the
// award pictures' order (kHeadlineTitles).
constexpr std::uint32_t kProgressHeadlineWon = kProgress + 364u;
bool headline_won(int i);  // below

// Our memory: the PSP kernel area, never used by the game.
constexpr std::uint32_t kArena = 0x08100000u;
constexpr std::uint32_t kTextBytes = 0x80u;
constexpr std::uint32_t kNewText = kArena;               // "BONUS - NEW"
constexpr std::uint32_t kRowText = kArena + kTextBytes;  // + row * kTextBytes (8 rows)
constexpr std::uint32_t kOurMainDescriptions = kArena + 0x900u;  // 8 pointers
constexpr std::uint32_t kEmptyText = kRowText + 7u * kTextBytes;  // an empty row
constexpr std::uint32_t kBonusDescription = kRowText + 6u * kTextBytes;  // the line under BONUS LEGENDS 2.0
constexpr std::uint32_t kListDescription = kRowText + 5u * kTextBytes;   // the line under our lists' rows
constexpr std::uint32_t kMaxRows = 8u;

constexpr std::uint32_t kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u;
constexpr std::uint32_t kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

enum class Screen { None, Pause, Main };
enum class Level { Main, Options, Bonus };

// What a row does.
enum class Action { Game, Options, Bonus, Filter, BonusSwitch, Picture };
struct Row {
    std::uint32_t text;
    Action action;
    int value;  // the game's row, or the filter
};

Hooks g_hooks;
psprecomp::GuestMemory *g_memory{};

Screen g_screen = Screen::None;  // the menu patched
Level g_level = Level::Main;
std::uint32_t g_list{};      // the list on screen
std::uint32_t g_visible{};   // rows on screen
Row g_rows[kMaxRows];
int g_count = 0;  // rows shown
std::uint32_t g_view{};
std::uint32_t g_labels[kGameRows]{};  // the game's rows
std::uint32_t g_descriptions[kMainRows]{};  // the main menu's (their place holds our fifth row)
std::uint32_t g_texts[kGameRows]{}, g_bars[kGameRows]{};
float g_text_y[kGameRows]{}, g_bar_y[kGameRows]{}, g_bar_w[kGameRows]{}, g_bar_h[kGameRows]{};
bool g_widths{};          // g_bar_w and the arrow were read (this menu)
std::uint32_t g_arrow{};  // 0: not found
float g_arrow_x{};
std::uint32_t g_options{}, g_bonus{}, g_on{}, g_off{}, g_new_word{}, g_signatures{}, g_strings_inside{};  // the game's strings

bool g_main_built{};  // the game built the main menu with our fifth row (burnout_main_menu_rows)
int g_hold_cursor{};  // vblanks the cursor is kept on the row we opened on (the game sets its own)
bool g_wait_box{};    // the game's row opened a YES/NO box
bool g_wait_away{};   // one of the game's rows was chosen: hands off until its list leaves the screen
Level g_reopen = Level::Main;  // after AUDIO or CONTROLS: back in OPTIONS
std::uint16_t g_reopen_cursor{};

std::uint32_t g_scroll{};  // main menu: the view's scroll when BONUS was chosen (our rows start there)
int g_top = 0, g_selected = 0;  // Bonus: first item on screen, item under the cursor
// BONUS LEGENDS 2.0: the lists shown at Level::Bonus. The main menu opens on
// Rewards (Trophies, Signature Takedowns, Special Events = the filters, Crash
// Headlines); the pause menu goes straight to the filters.
enum class ListKind { Rewards, Trophies, Signatures, Filters, Headlines };
ListKind g_kind = ListKind::Filters;
std::uint32_t g_previous{};
bool g_new{};
int g_new_awards{};  // filters unlocked since the Bonus rows were last opened (the game's award notice counts them)
std::uint32_t g_ticks{};
int g_carded = -1;  // filter on the postcard
bool g_colour_prompt{};    // the 2.0 garage on a Type 1: the SQUARE prompt drawn
bool g_reopen_rewards{};   // back from the 2.0 garage: BONUS 2.0's list again

// The Special Events, in the game's order (its SPECIAL EVENT POSTCARD list:
// Winter City .. Super Grand Prix): their names (string ids) and the
// profile's byte for each won (0x0881707C: progress +334 + n). Filter n is
// Special Event n's.
constexpr std::uint32_t kSpecialEventNames[10]{3743u, 2805u, 66u, 3736u, 2896u, 3735u, 2872u, 2895u, 3259u, 736u};
constexpr std::uint32_t kSpecialEventsWon = 0x08BBFDF0u + 334u;
bool g_photo{};     // a photo of the race was taken

std::uint32_t read32(std::uint32_t address) {
    std::uint32_t value = 0u;
    if (const std::uint8_t *p = g_memory->raw_pointer(address, 4u)) std::memcpy(&value, p, 4u);
    return value;
}
void write32(std::uint32_t address, std::uint32_t value) {
    if (std::uint8_t *p = g_memory->raw_pointer(address, 4u)) std::memcpy(p, &value, 4u);
}
std::uint8_t read8(std::uint32_t address) {
    const std::uint8_t *p = g_memory->raw_pointer(address, 1u);
    return p != nullptr ? *p : 0u;
}
std::uint16_t read16(std::uint32_t address) {
    std::uint16_t value = 0u;
    if (const std::uint8_t *p = g_memory->raw_pointer(address, 2u)) std::memcpy(&value, p, 2u);
    return value;
}
void write16(std::uint32_t address, std::uint16_t value) {
    if (std::uint8_t *p = g_memory->raw_pointer(address, 2u)) std::memcpy(p, &value, 2u);
}
float readf(std::uint32_t address) {
    const std::uint32_t bits = read32(address);
    float value;
    std::memcpy(&value, &bits, 4u);
    return value;
}
void writef(std::uint32_t address, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4u);
    write32(address, bits);
}
bool guest_pointer(std::uint32_t address) { return address >= 0x08800000u && address < 0x0A000000u; }

int language() {
    static const int value = save_dialog::system_language();
    return value;
}

// ASCII (upper case, like the game's menus) then a game string, as UTF-16LE.
void write_text(std::uint32_t address, const std::string &text, std::uint32_t game_text = 0u) {
    std::uint8_t *p = g_memory->raw_pointer(address, kTextBytes);
    if (p == nullptr) return;
    std::memset(p, 0, kTextBytes);
    std::uint32_t n = 0;
    for (; n < text.size() && (n + 1u) * 2u < kTextBytes; ++n)
        p[n * 2u] = static_cast<std::uint8_t>(std::toupper(static_cast<unsigned char>(text[n])));
    if (!guest_pointer(game_text)) return;
    for (std::uint32_t i = 0; i < 40u && (n + 1u) * 2u < kTextBytes; ++i, ++n) {
        const std::uint16_t c = read16(game_text + i * 2u);
        if (c == 0u) break;
        std::memcpy(p + n * 2u, &c, 2u);
    }
}

// A game string as ASCII for our font (accents dropped).
std::string ascii(std::uint32_t text, const char *fallback) {
    if (!guest_pointer(text)) return fallback;
    std::string out;
    for (std::uint32_t i = 0; i < 40u; ++i) {
        const std::uint16_t c = read16(text + i * 2u);
        if (c == 0u) break;
        static const char *const kLatin1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        if (c >= 0xC0u && c <= 0xFFu) out += kLatin1[c - 0xC0u];
        else if (c < 0x80u) out += static_cast<char>(c);
    }
    return out.empty() ? fallback : out;
}

std::uint16_t cursor() { return read16(g_list); }
void set_label(std::uint32_t row, std::uint32_t text) {
    write32(g_list + 4u + row * 8u, text);
    write32(g_list + 8u + row * 8u, 0u);
}

bool widget_up() { return read32(kWidgetDefinition) != 0u; }

std::uint32_t shown_view(std::uint32_t of_list) {
    if (!widget_up()) return 0u;
    const std::uint32_t view = read32(kWidgetView);
    return guest_pointer(view) && read32(view + 0x14u) == of_list ? view : 0u;
}

bool text_items(std::uint32_t view, std::uint32_t rows) {
    for (std::uint32_t i = 0; i < rows; ++i) {
        const std::uint32_t item = read32(view + 0x1Cu + i * 4u);
        if (!guest_pointer(item) || read32(item + 0x48u) != kTextItem) return false;
    }
    return true;
}

// The view, when it shows the pause list (not scrolled; with the game's five
// rows, or ours: the view is told how many rows it shows, or it scrolls to
// centre fewer rows).
std::uint32_t pause_view(std::uint32_t rows) {
    const std::uint32_t view = shown_view(kPause);
    if (view == 0u || read32(view + 0x0Cu) != rows || read32(view + 0x10u) != 0u) return 0u;
    for (std::uint32_t i = 0; i < 6u; ++i)
        if (read32(kPauseSignature + i * 4u) != kPauseSignatureWords[i]) return 0u;
    return view;
}

// The main menu as the game shows it (its own list), or with ours.
std::uint32_t main_view(std::uint32_t list, std::uint32_t descriptions) {
    // Four rows on screen, or five when the menu was built with ours.
    const std::uint32_t view = shown_view(list);
    if (view == 0u) return 0u;
    const std::uint32_t rows = read32(view + 0x0Cu);
    if ((rows != kMainRows && rows != kMainRows + 1u) || read32(kMainScreen) != list || read32(kMainScreen + 4u) != descriptions ||
        !text_items(view, rows))
        return 0u;
    return view;
}

// What we changed is still on screen.
bool intact() {
    if (g_screen == Screen::Main) return main_view(kMainList, kOurMainDescriptions) == g_view && g_view != 0u;
    const std::uint32_t view = pause_view(static_cast<std::uint32_t>(g_count));
    if (view == 0u || view != g_view) return false;
    for (std::uint32_t i = 0; i < kGameRows; ++i)
        if (read32(view + 0x1Cu + i * 4u) != g_texts[i] || read32(g_texts[i] + 0x48u) != kTextItem ||
            read32(g_bars[i] + 0x48u) != kBarItem)
            return false;
    return true;
}

// The game's string table, from a string inside it. Its header is looked
// for once (walking back up to 512 KiB, 2026-11-11: done on every call it
// cost the Vita's award screens frames) and kept while still there.
std::uint32_t g_strings_header{};
std::uint32_t game_string(std::uint32_t id) {
    std::uint32_t at = g_strings_header;
    if (at == 0u || read32(at) != kStringsMagic0 || read32(at + 4u) != kStringsMagic1) {
        at = 0u;
        for (std::uint32_t p = g_strings_inside & ~3u, n = 0; n < 0x20000u && p > 0x08800000u; p -= 4u, ++n)
            if (read32(p) == kStringsMagic0 && read32(p + 4u) == kStringsMagic1) {
                at = p;
                break;
            }
        g_strings_header = at;
        if (at == 0u) return 0u;
    }
    if (id >= read32(at + 8u)) return 0u;
    const std::uint32_t table = read32(at + 12u);
    const std::uint32_t text = guest_pointer(table) ? read32(table + id * 4u) : 0u;
    return guest_pointer(text) ? text : 0u;
}
bool find_strings(std::uint32_t inside) {
    g_strings_inside = inside;
    g_strings_header = 0u;
    g_options = game_string(kStringOptions);
    g_bonus = game_string(kStringBonus);
    g_on = game_string(kStringOn);
    g_off = game_string(kStringOff);
    g_new_word = game_string(kStringNew);
    g_signatures = game_string(kStringSignatures);
    return g_options != 0u && g_bonus != 0u;
}

// Each row's bar: the bar item level with its text (8.2 above it).
bool find_bars(std::uint32_t rows) {
    for (std::uint32_t i = 0; i < rows; ++i) {
        g_bars[i] = 0u;
        const float y = readf(g_texts[i] + 4u);
        float best = 3.0f;
        for (std::uint32_t at = kPool; at < kPoolEnd; at += kItem) {
            if (read32(at + 0x48u) != kBarItem || readf(at) != 0.0f || readf(at + 8u) < 1.0f) continue;
            const float d = std::fabs(readf(at + 4u) - (y - 8.24f));
            if (d < best) {
                best = d;
                g_bars[i] = at;
            }
        }
        if (g_bars[i] == 0u) return false;
        // Distinct bars (during the menu's opening animation the rows can
        // share a height).
        for (std::uint32_t j = 0; j < i; ++j)
            if (g_bars[j] == g_bars[i]) {
                g_bars[0] = 0u;
                return false;
            }
    }
    return true;
}

// The bars' widths and the arrow (at the end of the bars).
void find_widths(std::uint32_t rows) {
    g_widths = true;
    for (std::uint32_t i = 0; i < rows; ++i) {
        g_bar_w[i] = readf(g_bars[i] + 8u);
        g_bar_h[i] = readf(g_bars[i] + 12u);
        // (A padding row's bar was flattened: the first row's height.)
        if (g_bar_h[i] <= 0.0f && i > 0u) g_bar_h[i] = g_bar_h[0];
    }
    g_arrow = 0u;
    for (std::uint32_t at = kPool; at < kPoolEnd; at += kItem)
        if (read32(at + 0x48u) == kArrowItem && std::fabs(readf(at) - g_bar_w[0]) < 6.0f) {
            g_arrow = at;
            g_arrow_x = readf(at);
            break;
        }
}

int list_size(ListKind kind);
void list_row(ListKind kind, int item, std::uint32_t at);
// BONUS 2.0's lists (2026-11-11): one bar width for the whole list, from its
// longest row in the game's language (the bars do not change while it
// scrolls); in the menu's units (half the 960 pixels).
float list_bar_width(ListKind kind) {
    static int cached_kind = -1;
    static float cached = 0.0f;
    if (static_cast<int>(kind) == cached_kind) return cached;
    constexpr std::uint32_t kScratch = kArena + 0x1C80u;
    std::uint32_t longest = 0u;
    for (int i = 0; i < list_size(kind); ++i) {
        list_row(kind, i, kScratch);
        std::uint32_t chars = 0;
        while (chars < 60u && read16(kScratch + chars * 2u) != 0u) ++chars;
        longest = std::max(longest, chars);
    }
    cached_kind = static_cast<int>(kind);
    cached = std::clamp(6.0f + 10.2f * static_cast<float>(longest) + 40.0f, 150.0f, 320.0f);
    return cached;
}

// The filters' rows are longer than the game's: their bars grow to fit
// (the game's widths back elsewhere).
void set_widths(bool wide) {
    if (g_bars[0] == 0u) return;
    float width = 0.0f;
    for (std::uint32_t i = 0; i < g_visible; ++i) width = std::max(width, g_bar_w[i]);
    if (wide && g_level == Level::Bonus) {
        // BONUS 2.0's lists: always the same width, the longest name fits
        // (the bars no longer change with the rows shown, 2026-11-11).
        width = list_bar_width(g_kind);
    } else if (wide)
        for (int r = 0; r < g_count; ++r) {
            std::uint32_t chars = 0;
            while (chars < 60u && read16(g_rows[r].text + chars * 2u) != 0u) ++chars;
            width = std::max(width, std::min(385.0f, 12.5f * static_cast<float>(chars) + 24.0f));
        }
    for (std::uint32_t i = 0; i < g_visible; ++i) {
        if (g_bars[i] == 0u || read32(g_bars[i] + 0x48u) != kBarItem) continue;
        // The empty rows padding a short list (main menu) have no bar.
        const bool padding = g_screen == Screen::Main && static_cast<int>(i) >= g_count;
        // (A padding row's bar is flattened, not narrowed: find_bars finds it
        // again by its width when the row is used.)
        writef(g_bars[i] + 8u, wide ? width : g_bar_w[i]);
        const float h = g_bar_h[i] > 0.0f ? g_bar_h[i] : g_bar_h[0];
        if (h > 0.0f) writef(g_bars[i] + 12u, padding ? 0.0f : h);
    }
    if (g_arrow == 0u || read32(g_arrow + 0x48u) != kArrowItem) {
        // Not found with the widths (the game had moved it): the menu's arrow
        // item wherever it is.
        g_arrow = 0u;
        for (std::uint32_t at = kPool; at < kPoolEnd; at += kItem)
            if (read32(at + 0x48u) == kArrowItem) {
                g_arrow = at;
                g_arrow_x = g_bar_w[0];
                break;
            }
    }
    if (g_arrow != 0u) {
        // The game puts the arrow back at the end of its own bar, over our
        // longer rows' text: kept at the end of ours, every frame (2026-11-11).
        writef(g_arrow, wide ? width : g_arrow_x);
        writef(g_arrow + 8u, 16.0f);
        writef(g_arrow + 12u, 8.0f);
    }
}

std::string name(int filter) { return filters::localized_name(filter, language()); }

// "LOCKED" in the game's language (the game's own word is missing).
const char *display_word() {
    switch (language()) {
    case 2: return "AFFICHAGE";
    case 3: return "PANTALLA";
    case 4: return "ANZEIGE";
    case 5: return "SCHERMO";
    case 6: return "BEELD";
    default: return "DISPLAY";
    }
}

const char *locked_word() {
    switch (language()) {
    case 2: return "BLOQUE";
    case 3: return "BLOQUEADO";
    case 4: return "GESPERRT";
    case 5: return "BLOCCATO";
    case 6: return "VERGRENDELD";
    default: return "LOCKED";
    }
}

int filters_count() { return g_hooks.filter_count(); }

std::uint32_t bonus_label() {
    const std::string label = ascii(g_bonus, "BONUS") + " 2.0";
    if (!g_new) write_text(kNewText, label);
    else write_text(kNewText, label + " - ", g_new_word);
    return kNewText;
}

bool french() { return language() == 2; }

// A game string as one line (some have a line break) in ASCII.
std::string one_line(std::uint32_t text, const char *fallback) {
    std::string out = ascii(text, fallback);
    for (char &c : out)
        if (c == '\n' || c == '\r') c = ' ';
    return out;
}

// The Trophy objectives (0x08B9BC24: their names, in the order of the
// progress bytes at 0x08BBFDF0 + 90) and the four trophies (0x08B9BC14).
constexpr std::uint32_t kObjectiveNames = 0x08B9BC24u, kTrophyNames = 0x08B9BC14u, kObjectivesDone = kProgress + 90u;
constexpr int kObjectives = 20, kTrophies = 4;

int list_size(ListKind kind) {
    switch (kind) {
    case ListKind::Rewards: return 5;  // (GARAGE removed, 2026-11-11; EA GAMES TRAX, 2026-11-14: the user)
    case ListKind::Trophies: return kTrophies + 1;  // the four, then all four
    case ListKind::Signatures: return 20;
    case ListKind::Filters: return bonus::postcard_count();
    case ListKind::Headlines: return 11;
    }
    return 0;
}

// The Heavyweights in unlock order (rank 0..10): their car table index.
int heavy_by_rank(int rank) {
    const std::uint32_t cars = read32(kCarCount);
    for (std::uint32_t car = 0; car < cars && car < 128u; ++car)
        if (heavy_rank(read32(kCarTable + car * 40u), read32(kCarTable + car * 40u + 4u)) == rank) return static_cast<int>(car);
    return -1;
}

bool trophy_won(int trophy);
const char *trophy_colour(int trophy);
std::string trophy_name(int trophy);

bool g_everything_won{};  // tests: everything shown as won (set_everything_won)
bool headline_won(int i) {
    if (g_everything_won) return true;
    if (i >= 10) {
        for (int k = 0; k < 10; ++k)
            if (read8(kProgressHeadlineWon + static_cast<std::uint32_t>(k)) == 0u) return false;
        return true;
    }
    return i >= 0 && read8(kProgressHeadlineWon + static_cast<std::uint32_t>(i)) != 0u;
}

// A Special Event's filter is won in THIS profile (2026-11-11, the user: a
// save showed filters unlocked by another one, unlocks.bin being shared):
// unlocked once and its Special Event won in the profile loaded.
bool filter_won(int filter) {
    if (g_everything_won) return true;
    if (!g_hooks.available(filter)) return false;
    for (int i = 0; i < bonus::postcard_count(); ++i)
        if (bonus::postcard_filter(i) == filter) return read8(kSpecialEventsWon + static_cast<std::uint32_t>(i)) == 1u;
    return true;
}

// The line at the bottom for the item under the cursor.
void list_description(ListKind kind, int item) {
    const bool fr = french();
    std::string text;
    if (kind == ListKind::Rewards) {
        static const char *const fr_text[5]{"LES VOITURES COLLECTOR GAGNEES AVEC LES TAKEDOWNS SIGNATURE",
                                            "LES COULEURS DE VOITURE GAGNEES AVEC LES TROPHEES TAKEDOWN",
                                            "LES FILTRES GAGNES DANS LES EPREUVES SPECIALES",
                                            "LES HEAVYWEIGHTS GAGNES AVEC LES GROS TITRES DE CRASH",
                                            "LES MUSIQUES DU JEU ET VOS MUSIQUES PERSO"};
        static const char *const en_text[5]{"THE COLLECTOR CARS WON WITH SIGNATURE TAKEDOWNS", "THE CAR COLOURS WON WITH THE TAKEDOWN TROPHIES",
                                            "THE FILTERS WON IN SPECIAL EVENTS", "THE HEAVYWEIGHTS WON WITH CRASH HEADLINES", "THE GAME'S MUSIC AND YOUR OWN"};
        text = (fr ? fr_text : en_text)[std::clamp(item, 0, 4)];
    } else if (kind == ListKind::Filters && item >= 0 && item < 10) {
        const std::string event = one_line(game_string(kSpecialEventNames[item]), "?");
        const bool won = filter_won(bonus::postcard_filter(item));
        if (fr) text = (won ? "ACCORDE POUR AVOIR GAGNE L'EPREUVE SPECIALE " : "GAGNE L'EPREUVE SPECIALE ") + event;
        else text = (won ? "AWARDED FOR WINNING THE " : "WIN THE ") + event + " SPECIAL EVENT";
    }
    write_text(kListDescription, text);
}

// One row of a list: its text written at `at`.
void list_row(ListKind kind, int item, std::uint32_t at) {
    switch (kind) {
    case ListKind::Rewards: {
        // 2026-11-11: TAKEDOWN SIGNATURE, TROPHEES, EPREUVES SPECIALES.
        // 2026-11-11 (later): GARAGE back, the game's garage with the car turning.
        // The game's own words (its language): SIGNATURE TAKEDOWNS, TROPHIES,
        // SPECIAL EVENTS, CRASH HEADLINES, GARAGE.
        // 2026-11-14: EA GAMES TRAX (the game's words, 1787), its EA Trax screen.
        static constexpr std::uint32_t kWords[5]{kStringSignatures, 2348u, 1221u, kStringHeadlines, 1787u};
        static const char *const fallback[5]{"SIGNATURE TAKEDOWNS", "TROPHIES", "SPECIAL EVENTS", "CRASH HEADLINES", "EA GAMES TRAX"};
        const int i = std::clamp(item, 0, 4);
        write_text(at, one_line(game_string(kWords[i]), fallback[i]));
        return;
    }
    case ListKind::Trophies: {
        // The name alone (2026-11-11): the colour and won or not are on the
        // Polaroid (greyed while not won).
        write_text(at, trophy_name(item));
        return;
    }
    case ListKind::Signatures: {
        // The name alone (2026-11-11): the greyed Polaroids tell a locked one.
        write_text(at, one_line(game_string(kSignatureStrings[item]), "?"));
        return;
    }
    case ListKind::Filters: {
        // The Special Event's name (the filter's is on the card, 2026-11-11).
        const int filter = bonus::postcard_filter(item);
        const std::string event = item >= 0 && item < 10 ? one_line(game_string(kSpecialEventNames[item]), "?") : name(filter);
        if (!filter_won(filter)) write_text(at, event + ": " + locked_word());
        else write_text(at, event + ": ", g_hooks.active(filter) ? g_on : g_off);
        return;
    }
    case ListKind::Headlines: {
        // HEADLINE 1..10, then ALL HEADLINES (the game's words): the n-th
        // Crash Headline gives the n-th Heavyweight, all ten the Firetruck.
        if (item >= 10) write_text(at, one_line(game_string(370u), "ALL HEADLINES"));
        else write_text(at, one_line(game_string(kHeadlineTitles[item]), "HEADLINE"));
        return;
    }
    }
}

int rows_of(Level level, Row *rows) {
    if (level == Level::Bonus) {
        int n = 0;
        for (; n < static_cast<int>(g_visible) && g_top + n < list_size(g_kind); ++n) {
            const int item = g_top + n;
            const std::uint32_t at = kRowText + static_cast<std::uint32_t>(n) * kTextBytes;
            list_row(g_kind, item, at);
            rows[n] = {at, Action::Filter, item};
        }
        return n;
    }
    if (g_screen == Screen::Main) {
        for (std::uint32_t i = 0; i < kMainRows; ++i) rows[i] = {g_labels[i], Action::Game, static_cast<int>(i)};
        rows[kBonusRow] = {bonus_label(), Action::Bonus, 0};
        return static_cast<int>(kMainRows);
    }
    if (level == Level::Main) {
        rows[0] = {g_labels[0], Action::Game, 0};
        rows[1] = {g_labels[1], Action::Game, 1};
        rows[2] = {g_options, Action::Options, 0};
        rows[3] = {g_labels[4], Action::Game, 4};
        return 4;
    }
    int n = 0;
    rows[n++] = {g_labels[2], Action::Game, 2};
    rows[n++] = {g_labels[3], Action::Game, 3};
    const bool on = g_hooks.enabled();
    if (on) rows[n++] = {bonus_label(), Action::Bonus, 0};
    if (g_hooks.set_enabled) {
        const std::uint32_t at = kRowText + 6u * kTextBytes;
        write_text(at, ascii(g_bonus, "BONUS") + " VITA: ", on ? g_on : g_off);
        rows[n++] = {at, Action::BonusSwitch, 0};
    }
    if (g_hooks.set_psp_picture) {
        const std::uint32_t at = kRowText + 7u * kTextBytes;
        const bool psp = g_hooks.psp_picture && g_hooks.psp_picture();
        write_text(at, std::string(display_word()) + (psp ? ": PSP 480x272" : ": HD 960x544"));
        rows[n++] = {at, Action::Picture, 0};
    }
    return n;
}

// The list and the items show the level's rows (every vblank: the game may
// move its items back while it animates the menu).
void show_rows() {
    if (g_screen == Screen::Main) {
        // Our rows from the view's scroll on in the Bonus rows (the view is
        // left as the game has it, so its cursor highlight stays right);
        // the menu's own rows and BONUS otherwise (the game scrolls to it).
        const std::uint32_t first = g_level == Level::Bonus ? g_scroll : 0u;
        for (std::uint32_t i = 0; i < kMaxRows; ++i)
            write32(kOurMainDescriptions + i * 4u, g_level != Level::Bonus && i < kMainRows
                                                       ? (i == kBonusRow ? kBonusDescription : g_descriptions[i])
                                                       : kListDescription);
        if (g_level == Level::Bonus) list_description(g_kind, g_selected);
        for (int i = 0; i < g_count; ++i) set_label(first + static_cast<std::uint32_t>(i), g_rows[i].text);
        // A list shorter than the rows on screen (BONUS LEGENDS 2.0: four
        // rewards, five rows) is padded with empty rows: with fewer rows the
        // view centres them, its scroll goes to -1 and the game read a row
        // before the list (PC: crash backing out of SIGNATURE TAKEDOWNS).
        const std::uint32_t rows = std::max<std::uint32_t>(static_cast<std::uint32_t>(g_count), g_visible);
        // (A space: a row once shown empty stayed blank for the next list.)
        if (rows > static_cast<std::uint32_t>(g_count)) write_text(kEmptyText, " ");
        for (std::uint32_t i = static_cast<std::uint32_t>(g_count); i < rows; ++i) set_label(first + i, kEmptyText);
        write16(g_list + 2u, static_cast<std::uint16_t>(first + rows));
        if (g_level == Level::Bonus) {
            for (std::uint32_t i = 0; i < rows && i < g_visible; ++i)
                write32(read32(g_view + 0x1Cu + i * 4u) + 0x24u, i < static_cast<std::uint32_t>(g_count) ? g_rows[i].text : kEmptyText);
        } else {
            // The rows on screen show the list (BONUS too, when the menu was
            // built before it was there).
            const std::uint32_t scroll = read32(g_view + 0x10u);
            for (std::uint32_t i = 0; i < g_visible; ++i)
                write32(read32(g_view + 0x1Cu + i * 4u) + 0x24u, scroll + i < kMainRows ? g_rows[scroll + i].text : kEmptyText);
        }
        return;
    }
    for (int i = 0; i < g_count; ++i) set_label(static_cast<std::uint32_t>(i), g_rows[i].text);
    write16(kPause + 2u, static_cast<std::uint16_t>(g_count));
    write32(g_view + 0x0Cu, static_cast<std::uint32_t>(g_count));
    if (cursor() >= g_count) write16(kPause, static_cast<std::uint16_t>(g_count - 1));
    for (std::uint32_t i = 0; i < kGameRows; ++i) {
        const bool used = static_cast<int>(i) < g_count;
        if (used) write32(g_texts[i] + 0x24u, g_rows[i].text);
        writef(g_texts[i] + 4u, used ? g_text_y[i] : kOffScreen);
        writef(g_bars[i] + 4u, used ? g_bar_y[i] : kOffScreen);
    }
}

void show() {
    // Main menu: its bars found again each time (the menu animates; their
    // widths as the game made them kept from the first time).
    if (g_screen == Screen::Main) {
        // (The game may build the menu again with our fifth row: five on screen.)
        const std::uint32_t visible = std::min<std::uint32_t>(read32(g_view + 0x0Cu), kGameRows);
        if (visible != g_visible) {
            g_visible = visible;
            g_widths = false;
            if (g_level == Level::Bonus) {
                g_count = rows_of(Level::Bonus, g_rows);
                g_scroll = 0u;  // four rows, no scrolling
            }
        }
        for (std::uint32_t i = 0; i < g_visible; ++i) g_texts[i] = read32(g_view + 0x1Cu + i * 4u);
        if (find_bars(g_visible) && !g_widths) find_widths(g_visible);
    }
    show_rows();
    // Our rows are longer than the game's (BONUS LEGENDS 2.0 - NEW in the
    // main menu too): the bars grow to fit.
    set_widths(g_level == Level::Bonus || g_level == Level::Options || g_screen == Screen::Main);
}

void enter(Level level, std::uint16_t row) {
    g_level = level;
    g_count = rows_of(level, g_rows);
    if (std::getenv("PSPRECOMP_BURNOUT_MENU_TRACE") != nullptr)
        std::fprintf(stderr, "[menu] enter level %d kind %d row %u: count %d scroll %u visible %u list %08X cursor %u rows %u view %u/%u\n",
                     static_cast<int>(level), static_cast<int>(g_kind), row, g_count, g_scroll, g_visible, g_list, cursor(),
                     read16(g_list + 2u), read32(g_view + 0x0Cu), read32(g_view + 0x10u));
    const std::uint32_t first = g_screen == Screen::Main && level == Level::Bonus ? g_scroll : 0u;
    if (g_count > 0) write16(g_list, static_cast<std::uint16_t>(first + std::min<std::uint16_t>(row, static_cast<std::uint16_t>(g_count - 1))));
    show();
}

// The Bonus rows again after a change (scroll, a filter switched).
void refresh_bonus() { enter(Level::Bonus, static_cast<std::uint16_t>(g_selected - g_top)); }

// The game's rows in the pause list, each item where our row for it was, so
// the game's cursor shows on the right row; our other rows (OPTIONS) on the
// items left over, the rest off the screen.
void show_game_rows() {
    bool taken[kGameRows]{};
    int item_of[kMaxRows];
    for (int d = 0; d < g_count; ++d) {
        item_of[d] = -1;
        if (g_rows[d].action == Action::Game) {
            item_of[d] = g_rows[d].value;
            taken[g_rows[d].value] = true;
        }
    }
    for (int d = 0; d < g_count; ++d)
        for (std::uint32_t i = 0; item_of[d] < 0 && i < kGameRows; ++i)
            if (!taken[i]) {
                item_of[d] = static_cast<int>(i);
                taken[i] = true;
            }
    for (std::uint32_t i = 0; i < kGameRows; ++i) {
        int at = -1;
        for (int d = 0; d < g_count; ++d)
            if (item_of[d] == static_cast<int>(i)) at = d;
        write32(g_texts[i] + 0x24u, at >= 0 ? g_rows[at].text : g_labels[i]);
        writef(g_texts[i] + 4u, at >= 0 ? g_text_y[at] : kOffScreen);
        writef(g_bars[i] + 4u, at >= 0 ? g_bar_y[at] : kOffScreen);
    }
}

// The menu's own rows back in its list (and, items: on screen), before the
// game runs one of them or when its menu went away; the main menu's
// descriptions back where its fifth row was.
void restore(bool items) {
    if (g_screen == Screen::Main) {
        // The game's rows and descriptions back, four rows (when the game
        // builds the menu again it writes its list and builds the rows in the
        // same frame: from then on BONUS is reached by scrolling).
        for (std::uint32_t i = 0; i < kMainRows; ++i) {
            set_label(i, g_labels[i]);
            write32(kMainDescriptions + i * 4u, g_descriptions[i]);
        }
        write16(kMainList + 2u, static_cast<std::uint16_t>(kMainRows));
        if (cursor() >= kMainRows) write16(kMainList, static_cast<std::uint16_t>(kMainRows - 1u));
        if (read32(kMainScreen + 4u) == kOurMainDescriptions) write32(kMainScreen + 4u, kMainDescriptions);
    }
    if (g_screen == Screen::Pause) {
        for (std::uint32_t i = 0; i < kGameRows; ++i) set_label(i, g_labels[i]);
        write16(kPause + 2u, static_cast<std::uint16_t>(kGameRows));
        if (items) {
            write32(g_view + 0x0Cu, kGameRows);
            show_game_rows();
        }
    }
    g_screen = Screen::None;
}

// --- The pages' cards ----------------------------------------------------
// BONUS 2.0 (2026-11-11): TAKEDOWN SIGNATURE, TROPHEES, EPREUVES SPECIALES.
// The item under the cursor is shown on paper cards on the right: their
// pictures through the picture hook (gl::set_picture, slots 0-3, files beside
// the exe), the paper, captions and locked "?" drawn here (overlay).

std::string signature_photo_path(int signature);  // below (the awards screen's photos)
bool file_exists(const std::string &relative);     // below: a file beside the exe

struct CardPicture {
    std::string path;    // beside the exe ("postcards/sepia.jpg")
    float dx, dy, w, h;  // centre from the picture area's centre, size
};
struct Card {
    float cx{}, cy{}, image_w{}, image_h{}, angle{};
    std::vector<CardPicture> pictures;
    bool locked{};  // black with "?" instead of the pictures
    // A Polaroid picture (its own paper, the game's Takedown Book style):
    // no paper drawn here, only the title written on it, caption_dy below
    // the picture's centre (pictures[0], centred on cx, cy).
    bool polaroid{};
    int slot{-1};  // picture slot of pictures[0] (show_cards), -1: none
    float caption_dy{}, caption_w{};
    std::string title, line;
    int tone{};  // the line: 0 grey, 1 green, 2 red
};
std::vector<Card> g_cards;
std::function<bool(int)> g_picture_ready;  // set_picture_ready
// The game's postcard award (2026-11-11, the user's idea): the new filter's
// words over the card ("NEW FILTER UNLOCKED", its name), alpha 0..1.
struct AwardText {
    std::string line1, line2;
    float alpha{};
    std::string title, footer;  // our award windows: the title bar's and the bottom bar's words
    float footer_alpha{};
} g_award_text;
int g_card_kind = -1, g_card_item = -1;
constexpr float kCardBorder = 10.0f, kCardCaption = 64.0f;
constexpr int kPictureSlots = 4;

float card_w(const Card &c) { return c.image_w + 2.0f * kCardBorder; }
float card_h(const Card &c) { return c.image_h + kCardBorder + kCardCaption; }

// A point of the card (from its picture area's centre) on the screen.
void card_point(const Card &c, float dx, float dy, float &x, float &y) {
    const float ly = -card_h(c) * 0.5f + kCardBorder + c.image_h * 0.5f + dy;
    const float cs = std::cos(c.angle), sn = std::sin(c.angle);
    x = c.cx + dx * cs - ly * sn;
    y = c.cy + dx * sn + ly * cs;
}

// Trophy 0-3 won (its five objectives); 4: all four trophies.
bool trophy_won(int trophy) {
    if (g_everything_won) return true;
    const int from = trophy < 4 ? trophy : 0, to = trophy < 4 ? trophy + 1 : 4;
    for (int t = from; t < to; ++t)
        for (int i = 0; i < 5; ++i)
            if (read8(kObjectivesDone + static_cast<std::uint32_t>(t * 5 + i)) != 1u) return false;
    return true;
}

// The colour a trophy gives, in the game's language (1 English, 2 French,
// 3 Spanish, 4 German, 5 Italian, 6 Dutch): SILVER and GOLD are the game's
// own words, the others ours.
constexpr std::uint32_t kStringColour = 3316u, kStringGold = 1476u, kStringSilver = 1486u, kStringTrophies = 2348u;
std::string colour_word(int trophy) {
    trophy = std::clamp(trophy, 0, 4);
    if (trophy == 3) return one_line(game_string(kStringSilver), "SILVER");
    if (trophy == 4) return one_line(game_string(kStringGold), "GOLD");
    static const char *const names[7][3]{
        {"ORANGE", "PINK", "LIME GREEN"},     // (0: English)
        {"ORANGE", "PINK", "LIME GREEN"},     // English
        {"ORANGE", "ROSE", "VERT LIME"},      // French
        {"NARANJA", "ROSA", "VERDE LIMA"},    // Spanish
        {"ORANGE", "PINK", "LIMETTENGRUEN"},  // German
        {"ARANCIONE", "ROSA", "VERDE LIME"},  // Italian
        {"ORANJE", "ROZE", "LIMOENGROEN"},    // Dutch
    };
    const int l = language() >= 0 && language() <= 6 ? language() : 1;
    return names[l][trophy];
}
const char *trophy_colour(int trophy) {
    static const char *const fr[5]{"ORANGE", "ROSE", "VERT LIME", "ARGENT", "OR"};
    static const char *const en[5]{"ORANGE", "PINK", "LIME GREEN", "SILVER", "GOLD"};
    return (french() ? fr : en)[std::clamp(trophy, 0, 4)];
}

std::string trophy_name(int trophy) {
    if (trophy >= 4) {
        switch (language()) {
        case 2: return "LES 4 TROPHEES";
        case 3: return "LOS 4 TROFEOS";
        case 4: return "ALLE 4 TROPHAEEN";
        case 5: return "TUTTI E 4 I TROFEI";
        case 6: return "ALLE 4 BEKERS";
        default: return "ALL 4 TROPHIES";
        }
    }
    return one_line(game_string(read32(kTrophyNames + static_cast<std::uint32_t>(trophy) * 4u)), "?");
}

std::vector<Card> cards_for(ListKind kind, int item) {
    std::vector<Card> cards;
    const bool fr = french();
    if (kind == ListKind::Filters) {
        // One postcard (2026-11-11, the user's choice): the photo as it is
        // while the filter is OFF, through the filter once it is ON.
        const int filter = bonus::postcard_filter(item);
        // 2026-11-11: the Special Event's own postcard (the game's), greyed
        // and LOCKED while not won; through its filter while the filter is ON.
        Card after;
        after.cx = 740.0f, after.cy = 250.0f, after.image_w = 330.0f, after.image_h = 165.0f, after.angle = 0.06f;
        const bool locked = !filter_won(filter);
        const bool on = !locked && g_hooks.active(filter);
        const std::string n = std::to_string(item + 1);
        // (Locked: black with "?", the user's choice over the greyed card.)
        after.locked = locked;
        if (!locked) after.pictures.push_back({"bonus/postcard_" + n + (on ? "_filtre" : "") + ".png", 0.0f, 0.0f, 330.0f, 165.0f});
        after.title = name(filter);
        if (locked) {
            after.line = locked_word();
            after.tone = 2;
        } else if (g_hooks.active(filter)) {
            after.line = ascii(g_on, "ON");
            after.tone = 1;
        } else {
            after.line = ascii(g_off, "OFF");
        }
        cards.push_back(after);
    } else if (kind == ListKind::Signatures) {
        // The user's mock-up (2026-11-11): two Polaroids, the Takedown's photo
        // (top left) and the Collector car it gives (bottom right, over it),
        // each with its name written on it in the player's language; greyed
        // while the Signature is not done (the Takedown Book's way).
        const bool done = read8(kProfileSignatures + static_cast<std::uint32_t>(item)) == 1u;
        const std::string n = std::to_string(item + 1);
        Card photo;
        photo.polaroid = true;
        photo.cx = 548.0f, photo.cy = 200.0f, photo.angle = 0.07f;
        photo.pictures.push_back({done ? signature_photo_path(item) : "bonus/signature_" + n + "_bloque.png", 0.0f, 0.0f, 252.0f, 252.0f});
        photo.title = one_line(game_string(kSignatureStrings[item]), "?");
        photo.caption_dy = 88.0f, photo.caption_w = 190.0f;
        cards.push_back(photo);
        Card car;
        car.polaroid = true;
        car.cx = 786.0f, car.cy = 342.0f, car.angle = 0.05f;
        car.pictures.push_back({"bonus/collector_" + n + (done ? "" : "_bloque") + ".png", 0.0f, 0.0f, 292.0f, 249.0f});
        car.title = one_line(game_string(kSignatureCars[item]), "?");
        car.caption_dy = 93.0f, car.caption_w = 240.0f;
        cards.push_back(car);
    } else if (kind == ListKind::Headlines) {
        // As the Signatures (2026-11-11): the Crash Headline's newspaper (top
        // left) and the Heavyweight it gives (a Polaroid, bottom right, its
        // name written on it); greyed until enough Crash Headlines.
        const bool done = headline_won(item);
        const std::string n = std::to_string(item + 1);
        Card paper;
        paper.polaroid = true;
        paper.cx = 548.0f, paper.cy = 196.0f, paper.angle = -0.04f;
        paper.pictures.push_back({"bonus/headline_" + n + (done ? "" : "_bloque") + ".png", 0.0f, 0.0f, 262.0f, 262.0f});
        cards.push_back(paper);
        Card car;
        car.polaroid = true;
        car.cx = 786.0f, car.cy = 342.0f, car.angle = 0.05f;
        car.pictures.push_back({"bonus/heavy_" + n + (done ? "" : "_bloque") + ".png", 0.0f, 0.0f, 292.0f, 249.0f});
        const int vehicle = heavy_by_rank(item);
        car.title = vehicle >= 0 ? one_line(game_string(read32(kCarTable + static_cast<std::uint32_t>(vehicle) * 40u + 28u)), "?") : "?";
        car.caption_dy = 93.0f, car.caption_w = 240.0f;
        cards.push_back(car);
    } else if (kind == ListKind::Trophies) {
        // A Polaroid (as the Signatures', 2026-11-11): a Type 1 in the colour
        // the trophy gives, "COLOUR <name>" written on it in the game's
        // language; greyed while the trophy is not won.
        const std::string colour = std::to_string(paints::kBaseColours + 1 + item);
        Card c;
        c.polaroid = true;
        c.cx = 680.0f, c.cy = 262.0f, c.angle = 0.05f;
        // (PC all_colours: every trophy colour offered, shown as won.)
        const bool shown = trophy_won(item) || paints::unlocked_colours(*g_memory) >= paints::kTrophyColours;
        c.pictures.push_back({"bonus/trophy_polaroid_" + colour + (shown ? "" : "_bloque") + ".png", 0.0f, 0.0f, 360.0f, 307.0f});
        c.title = one_line(game_string(kStringColour), "COLOUR") + " " + colour_word(item);
        c.caption_dy = 115.0f, c.caption_w = 300.0f;
        cards.push_back(c);
    }
    // Polaroids: clear of the list's bars (long names in some languages,
    // 2026-11-11: too small on the French Vita): two of them go one above
    // the other in the column right of the bars, as large as it allows.
    float left = 1e9f;
    int polaroids = 0;
    for (const Card &c : cards)
        if (c.polaroid && !c.pictures.empty()) left = std::min(left, c.cx - c.pictures[0].w * 0.5f), ++polaroids;
    // (The bars' end on the screen: their width in the menu's units x 1.7,
    // measured on PC captures.)
    const float bars = list_bar_width(kind) * 1.7f + 12.0f, edge = 950.0f;
    const auto fit = [](Card &c, float k) {
        for (CardPicture &p : c.pictures) p.w *= k, p.h *= k;
        c.caption_dy *= k, c.caption_w *= k;
    };
    if (polaroids == 2) {
        // (2026-11-11, the user: both pictures seen whole, none over the
        // other.) Side by side or one above the other in the room right of
        // the bars, whichever lets them be larger; a gap between them.
        const float room = edge - bars, top = 80.0f, bottom = 464.0f, gap = 18.0f;
        Card &first = cards[0], &second = cards[1];
        const float w1 = first.pictures[0].w, h1 = first.pictures[0].h, w2 = second.pictures[0].w, h2 = second.pictures[0].h;
        const float side = std::min({1.15f, (room - gap) / (w1 + w2), (bottom - top) / std::max(h1, h2)});
        const float stack = std::min({1.15f, room / std::max(w1, w2), (bottom - top - gap) / (h1 + h2)});
        const float k = std::max(side, stack);
        fit(first, k);
        fit(second, k);
        first.angle = -0.02f, second.angle = 0.02f;
        first.cx = bars + w1 * k * 0.5f;
        second.cx = edge - w2 * k * 0.5f;
        if (side >= stack) {
            // Side by side: the first a little higher.
            first.cy = std::max(top + h1 * k * 0.5f, top + (bottom - top) * 0.42f);
            second.cy = std::min(bottom - h2 * k * 0.5f, top + (bottom - top) * 0.58f);
            first.cy = std::min(first.cy, bottom - h1 * k * 0.5f);
            second.cy = std::max(second.cy, top + h2 * k * 0.5f);
        } else {
            first.cy = top + h1 * k * 0.5f;
            second.cy = bottom - h2 * k * 0.5f;
        }
    } else if (left < 1e8f && bars > left) {
        const float room = edge - bars;
        {
            for (Card &c : cards) {
                if (!c.polaroid) continue;
                fit(c, std::min(1.0f, room / c.pictures[0].w));
                c.cx = edge - c.pictures[0].w * 0.5f;
            }
        }
    }
    return cards;
}

// The cards for this item (kind -1: none), their pictures handed over.
void show_cards(int kind, int item) {
    if (kind == g_card_kind && item == g_card_item) return;
    g_card_kind = kind;
    g_card_item = item;
    g_cards = kind < 0 ? std::vector<Card>{} : cards_for(static_cast<ListKind>(kind), item);
    if (!g_hooks.picture) return;
    int slot = 0;
    for (Card &c : g_cards) {
        if (c.locked) continue;
        if (!c.pictures.empty() && slot < kPictureSlots) c.slot = slot;
        for (const CardPicture &p : c.pictures) {
            if (slot >= kPictureSlots) break;
            float x = c.cx, y = c.cy;
            if (!c.polaroid) card_point(c, p.dx, p.dy, x, y);
            g_hooks.picture(slot++, p.path, x, y, p.w, p.h, c.angle);
        }
    }
    for (; slot < kPictureSlots; ++slot) g_hooks.picture(slot, std::string(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}
// No cards (the old postcard's call).
void card(int none) {
    (void)none;
    show_cards(-1, -1);
}

void photo() {
    if (g_hooks.photo) g_hooks.photo();
    g_photo = true;
}

void publish_card() {
    overlay::Builder b;
    if (!g_cards.empty() && overlay::ready()) {
        constexpr overlay::Colour kShadow{0.0f, 0.0f, 0.0f, 0.45f};
        constexpr overlay::Colour kPaper{0.97f, 0.96f, 0.92f, 1.0f};
        constexpr overlay::Colour kInk{0.12f, 0.12f, 0.14f, 1.0f};
        constexpr overlay::Colour kTones[3]{{0.42f, 0.42f, 0.45f, 1.0f}, {0.10f, 0.55f, 0.20f, 1.0f}, {0.72f, 0.12f, 0.10f, 1.0f}};
        constexpr overlay::Colour kRed{0.72f, 0.12f, 0.10f, 1.0f};
        for (const Card &c : g_cards) {
            if (c.polaroid) {
                // The name written on the Polaroid's paper, turned with it,
                // once its picture is on screen (not alone before it).
                if (c.slot >= 0 && g_picture_ready && !g_picture_ready(c.slot)) continue;
                const std::size_t first = b.size();
                float size = 24.0f;
                while (size > 14.0f && b.text_width(size, c.title) > c.caption_w) size -= 1.0f;
                const std::size_t space = c.title.find(' ', c.title.size() / 2u);
                if (b.text_width(size, c.title) > c.caption_w && space != std::string::npos) {
                    // (2026-11-13) Too long for the paper: two lines, split
                    // at the first space past the middle.
                    const std::string top = c.title.substr(0, space), bottom = c.title.substr(space + 1u);
                    // (2026-11-13, the user: a little smaller, clear of the photo.)
                    size = 16.0f;
                    while (size > 9.0f && std::max(b.text_width(size, top), b.text_width(size, bottom)) > c.caption_w) size -= 1.0f;
                    const float y = c.cy + c.caption_dy - size * 0.5f + size * 0.15f;
                    b.text(c.cx - b.text_width(size, top) * 0.5f, y - size * 0.55f, size, kInk, top);
                    b.text(c.cx - b.text_width(size, bottom) * 0.5f, y + size * 0.55f, size, kInk, bottom);
                } else {
                    while (size > 9.0f && b.text_width(size, c.title) > c.caption_w) size -= 1.0f;
                    b.text(c.cx - b.text_width(size, c.title) * 0.5f, c.cy + c.caption_dy - size * 0.5f, size, kInk, c.title);
                }
                b.rotate(first, c.cx, c.cy, c.angle);
                continue;
            }
            const float w = card_w(c), h = card_h(c);
            const float x = c.cx - w * 0.5f, y = c.cy - h * 0.5f;
            const std::size_t first = b.size();
            // Shadow, then the paper around the picture area (the pictures
            // are drawn by the renderer in the hole), all turned together.
            b.rect(x + w, y + 8.0f, 8.0f, h, kShadow);
            b.rect(x + 8.0f, y + h, w, 8.0f, kShadow);
            b.rect(x, y, w, kCardBorder, kPaper);
            b.rect(x, y + kCardBorder, kCardBorder, c.image_h, kPaper);
            b.rect(x + kCardBorder + c.image_w, y + kCardBorder, kCardBorder, c.image_h, kPaper);
            b.rect(x, y + kCardBorder + c.image_h, w, kCardCaption, kPaper);
            if (c.locked) {
                constexpr overlay::Colour kBlack{0.04f, 0.04f, 0.05f, 1.0f};
                constexpr overlay::Colour kMark{0.35f, 0.35f, 0.38f, 1.0f};
                b.rect(x + kCardBorder, y + kCardBorder, c.image_w, c.image_h, kBlack);
                const float size = 90.0f, tw = b.text_width(size, "?");
                b.text(x + kCardBorder + (c.image_w - tw) * 0.5f, y + kCardBorder + (c.image_h - size) * 0.5f, size, kMark, "?");
            }
            const float ty = y + kCardBorder + c.image_h + 9.0f;
            b.text(x + 14.0f, ty, 20.0f, kInk, c.title);
            if (!c.line.empty()) b.text(x + 14.0f, ty + 28.0f, 13.0f, kTones[std::clamp(c.tone, 0, 2)], c.line);
            // The stamp.
            b.rect(x + w - 38.0f, ty + 2.0f, 24.0f, 28.0f, kRed);
            b.rect(x + w - 35.0f, ty + 5.0f, 18.0f, 22.0f, kPaper);
            b.rect(x + w - 32.0f, ty + 8.0f, 12.0f, 16.0f, kRed);
            b.rotate(first, c.cx, c.cy, c.angle);
        }
    }
    if (g_award_text.alpha > 0.0f && overlay::ready()) {
        // White, shadowed, over the middle of the postcard (as the game's
        // CONGRATULATIONS).
        const float a = g_award_text.alpha;
        const overlay::Colour white{1.0f, 1.0f, 1.0f, a}, shade{0.0f, 0.0f, 0.0f, 0.75f * a};
        constexpr float s1 = 30.0f, s2 = 44.0f, y1 = 206.0f, y2 = 246.0f;
        float w = b.text_width(s1, g_award_text.line1);
        for (int d = 1; d <= 3; ++d) b.text(480.0f - w * 0.5f + d, y1 + d, s1, shade, g_award_text.line1);
        b.text(480.0f - w * 0.5f, y1, s1, white, g_award_text.line1);
        w = b.text_width(s2, g_award_text.line2);
        for (int d = 1; d <= 3; ++d) b.text(480.0f - w * 0.5f + d, y2 + d, s2, shade, g_award_text.line2);
        b.text(480.0f - w * 0.5f, y2, s2, white, g_award_text.line2);
    }
    if (!g_award_text.title.empty() && overlay::ready()) {
        // As the game's: white words, shadowed, in the title bar (left) and
        // the bottom bar.
        const overlay::Colour white{1.0f, 1.0f, 1.0f, 1.0f}, shade{0.0f, 0.0f, 0.0f, 0.6f};
        float size = 46.0f;
        while (size > 20.0f && b.text_width(size, g_award_text.title) > 480.0f) size -= 1.0f;
        for (int d = 1; d <= 3; ++d) b.text(14.0f + d, 4.0f + d, size, shade, g_award_text.title);
        b.text(14.0f, 4.0f, size, white, g_award_text.title);
        const overlay::Colour fw{1.0f, 1.0f, 1.0f, g_award_text.footer_alpha}, fs{0.0f, 0.0f, 0.0f, 0.6f * g_award_text.footer_alpha};
        size = 30.0f;
        while (size > 14.0f && b.text_width(size, g_award_text.footer) > 500.0f) size -= 1.0f;
        const float x = 300.0f - b.text_width(size, g_award_text.footer) * 0.5f;
        for (int d = 1; d <= 2; ++d) b.text(x + d, 470.0f + d, size, fs, g_award_text.footer);
        b.text(x, 470.0f, size, fw, g_award_text.footer);
    }
    if (g_colour_prompt && overlay::ready()) {
        // The 2.0 garage on a Type 1: "COLOUR []" where the game's prompts go
        // (bottom right; this garage has no SELECT).
        constexpr overlay::Colour kWhite{1.0f, 1.0f, 1.0f, 1.0f};
        constexpr overlay::Colour kDark{0.10f, 0.12f, 0.16f, 1.0f};
        constexpr overlay::Colour kPink{0.93f, 0.45f, 0.72f, 1.0f};
        const std::string word = french() ? "COULEUR" : "COLOUR";
        constexpr float size = 19.0f, icon = 24.0f, y = 503.0f;
        const float x = 944.0f - icon;
        b.text(x - 8.0f - b.text_width(size, word), y + 1.0f, size, kWhite, word);
        b.bar(x, y, icon, icon, kWhite, 5.0f);
        b.rect(x + 6.0f, y + 6.0f, icon - 12.0f, icon - 12.0f, kPink);
        b.rect(x + 8.5f, y + 8.5f, icon - 17.0f, icon - 17.0f, kWhite);
        (void)kDark;
    }
    overlay::publish(b.take());
}

// --- Patching -----------------------------------------------------------

// The pause list as the game built it: rearranged. Same items as before (a
// YES/NO box closed): their places are known already (some are off the
// screen now).
void patch_pause(std::uint32_t view, bool same_items) {
    std::uint32_t texts[kGameRows];
    for (std::uint32_t i = 0; i < kGameRows; ++i) {
        texts[i] = read32(view + 0x1Cu + i * 4u);
        if (!guest_pointer(read32(kPause + 4u + i * 8u)) || !guest_pointer(texts[i]) || read32(texts[i] + 0x48u) != kTextItem) return;
    }
    if (!same_items) {
        for (std::uint32_t i = 0; i < kGameRows; ++i) {
            g_texts[i] = texts[i];
            g_text_y[i] = readf(texts[i] + 4u);
        }
        if (!find_bars(kGameRows)) return;
        for (std::uint32_t i = 0; i < kGameRows; ++i) g_bar_y[i] = readf(g_bars[i] + 4u);
        find_widths(kGameRows);
    }
    g_screen = Screen::Pause;
    g_list = kPause;
    g_visible = kGameRows;
    for (std::uint32_t i = 0; i < kGameRows; ++i) g_labels[i] = read32(kPause + 4u + i * 8u);
    if (!find_strings(g_labels[0])) {
        g_screen = Screen::None;
        return;
    }
    g_view = view;
    if (!same_items) photo();  // the race behind, before our rows grow wider
    std::fprintf(stderr, "[menu] pause (%s) level %d row %d\n", same_items ? "same items" : "new items", static_cast<int>(g_reopen),
                 g_reopen_cursor);
    if (g_reopen == Level::Bonus) refresh_bonus();
    else enter(g_reopen, g_reopen_cursor);
    g_hold_cursor = 40;
}

// The main menu: its descriptions move to our memory (the screen object
// points at them), which frees the room for a fifth row in its own list (the
// game moves the cursor in that list, at its fixed address).
void enter_bonus(ListKind kind);

void patch_main(std::uint32_t view) {
    // Built by the game with our fifth row (its descriptions read then), or
    // with its own four.
    const bool built = g_main_built && read16(kMainList + 2u) == kMainRows + 1u;
    g_main_built = false;
    if (!built) {
        if (read16(kMainList + 2u) != kMainRows) return;
        for (std::uint32_t i = 0; i < kMainRows; ++i)
            if (!guest_pointer(read32(kMainDescriptions + i * 4u))) return;
    }
    for (std::uint32_t i = 0; i < kMainRows; ++i)
        if (!guest_pointer(read32(kMainList + 4u + i * 8u))) return;
    for (std::uint32_t i = 0; i < kMainRows; ++i) {
        const std::uint32_t label = read32(kMainList + 4u + i * 8u);
        if (label != kNewText) g_labels[i] = label;  // (ours still there: the game's kept from before)
    }
    if (!find_strings(g_labels[0])) return;
    write_text(kBonusDescription, french() ? "RECOMPENSES ET FILTRES DE LA VERSION 2.0" : "VERSION 2.0 REWARDS AND FILTERS");
    // Descriptions (the line at the bottom): the game's, then "SIGNATURE TAKEDOWNS".
    if (!built)
        for (std::uint32_t i = 0; i < kMainRows; ++i) g_descriptions[i] = read32(kMainDescriptions + i * 4u);
    for (std::uint32_t i = 0; i < kMaxRows; ++i) write32(kOurMainDescriptions + i * 4u, i < kMainRows ? g_descriptions[i] : g_signatures);
    write32(kMainScreen + 4u, kOurMainDescriptions);
    g_screen = Screen::Main;
    g_list = kMainList;
    g_visible = std::min<std::uint32_t>(read32(view + 0x0Cu), kGameRows);
    g_view = view;
    std::fill(std::begin(g_bars), std::end(g_bars), 0u);
    g_arrow = 0u;
    g_widths = false;
    std::fprintf(stderr, "[menu] main menu\n");
    g_level = Level::Main;
    g_count = rows_of(Level::Main, g_rows);
    show();
    if (g_reopen_rewards) {
        // Back from the 2.0 garage or EA Trax: BONUS 2.0's list, on that row.
        g_reopen_rewards = false;
        g_scroll = 0u;
        enter_bonus(ListKind::Rewards);
        g_selected = 4;
        refresh_bonus();
    }
}


// Twice a second: Signature Takedowns earned in the profile unlock filters.
// Each Signature Takedown done gives its Collector car (kSignatureCars).
// The car table's record of Signature n's Collector car, 0 if none.
std::uint32_t collector_car(int signature) {
    const std::uint32_t cars = read32(kCarCount);
    if (signature < 0 || signature >= 20 || cars == 0u || cars > 128u) return 0u;
    for (std::uint32_t car = 0; car < cars; ++car)
        if (read32(kCarTable + car * 40u + 28u) == kSignatureCars[signature]) return car + 1u;
    return 0u;
}

// Signature n's Collector car unlocked (the profile's flag for it).
void give_collector_car(std::uint32_t profile, int signature) {
    if (g_hooks.enabled && !g_hooks.enabled()) return;
    const std::uint32_t car = collector_car(signature);
    if (car == 0u) return;
    std::uint8_t *flag = g_memory->raw_pointer(profile + 1188u + (car - 1u), 1u);
    if (flag == nullptr || *flag != 0u) return;
    *flag = 1u;
    std::fprintf(stderr, "[bonus] Signature Takedown %d: Collector car %u (string %u) unlocked\n", signature, car - 1u,
                 kSignatureCars[signature]);
}

bool g_all_cars{};  // PC settings: every car unlocked
bool g_force_all{};  // force_all_rewards: armed
std::function<void()> g_force_all_fired;

void give_collector_cars() {
    const std::uint8_t *done = g_memory->raw_pointer(kProfileSignatures, 20u);
    if (done == nullptr) return;
#if !defined(__vita__)
    // PC tests and settings (all_cars): every car unlocked (kept by the save).
    static const bool all_cars = std::getenv("PSPRECOMP_BURNOUT_ALL_CARS") != nullptr;
    if (all_cars || g_all_cars) {
        if (std::uint8_t *flags = g_memory->raw_pointer(kProfileCarFlags, 89u)) {
            static bool told = false;
            if (!told) {
                int set = 0;
                for (int i = 0; i < 89; ++i) set += flags[i] != 0u ? 1 : 0;
                std::fprintf(stderr, "[bonus] all cars: %d of 89 flags were set\n", set);
                told = true;
            }
            std::memset(flags, 1, 89u);
        }
        // The unlocked table the game builds from those flags at the
        // profile's load (0x08817E98: one byte per car, the count at +424),
        // which car select reads: built again here.
        constexpr std::uint32_t kUnlockedCars = 0x08BBFDF0u;
        const std::uint32_t cars = std::min<std::uint32_t>(read32(kCarCount), 89u);
        if (std::uint8_t *table = g_memory->raw_pointer(kUnlockedCars, cars); table != nullptr && cars != 0u) {
            std::memset(table, 1, cars);
            write32(kUnlockedCars + 424u, cars);
        }
    }
#endif
    for (int i = 0; i < 20; ++i)
        if (done[i] == 1u) give_collector_car(kProfileCarFlags - 1188u, i);
}

// The Collector cars' description in car select, "AWARDED FOR WINNING A
// COLLECTOR CHALLENGE RACE" (an AdHoc race): "SIGNATURE TAKEDOWNS: <its
// Signature>" in the game's words and language, our text in the arena with
// the string table pointing at it.
constexpr std::uint32_t kCollectorTexts = kArena + 0xA00u;  // 20 x kTextBytes
std::uint32_t string_table(std::uint32_t &count) {
    static std::uint32_t header = 0u;
    if (header == 0u || read32(header) != kStringsMagic0 || read32(header + 4u) != kStringsMagic1) {
        header = 0u;
        const std::uint8_t *ram = g_memory->raw_pointer(0x08800000u, 0x01800000u);
        if (ram == nullptr) return 0u;
        for (std::uint32_t off = 0; off + 16u <= 0x01800000u; off += 4u) {
            std::uint32_t a, b;
            std::memcpy(&a, ram + off, 4u);
            if (a != kStringsMagic0) continue;
            std::memcpy(&b, ram + off + 4u, 4u);
            if (b == kStringsMagic1) {
                header = 0x08800000u + off;
                break;
            }
        }
        if (header == 0u) return 0u;
    }
    count = read32(header + 8u);
    const std::uint32_t table = read32(header + 12u);
    return guest_pointer(table) ? table : 0u;
}

void copy_utf16(std::uint8_t *to, std::uint32_t &n, std::uint32_t text) {
    if (!guest_pointer(text)) return;
    for (std::uint32_t i = 0; i < 48u && (n + 1u) * 2u < kTextBytes; ++i, ++n) {
        const std::uint16_t c = read16(text + i * 2u);
        if (c == 0u) break;
        std::memcpy(to + n * 2u, &c, 2u);
    }
}

// The Collector cars a Signature Takedown gives (Burnout_Legends_Recompenses_2,
// 2026-11-11): on the game's "car unlocked" screen and in car select, the
// title "TAKEDOWN SIGNATURE" (string 1556) in place of "COLLECTOR", and the
// scrolling text "<AWARDED FOR> TAKEDOWN SIGNATURE <NAME> : <WHERE>" in the
// game's own words and language: "AWARDED FOR" from "AWARDED FOR 15
// TAKEDOWNS" (string 273, up to the number), the place from the Takedown
// Book's text for that Signature (kSignaturePlace). Our texts in the arena,
// the string table pointing at them.
constexpr std::uint32_t kCollectorLines = kArena + 0x4000u;  // 20 x kLineBytes
constexpr std::uint32_t kLineBytes = 0x100u;
constexpr std::uint32_t kStringSignatureTitle = 1556u, kStringAwardedFor15 = 273u;

// The Takedown Book's place text of a Signature name: names 2198-2201 have
// theirs at 2202-2205, names 2227-2247 at 2206-2226 (same order).
std::uint32_t signature_place(std::uint32_t name) { return name <= 2201u ? name + 4u : name - 21u; }

struct Utf16Line {
    std::uint8_t *p;
    std::uint32_t n{}, max;
    void add(std::uint16_t c) {
        if (n + 1u >= max) return;
        std::memcpy(p + n * 2u, &c, 2u);
        ++n;
    }
    // A game string; stop_at_digit: up to its first digit, trailing spaces cut.
    void add_string(std::uint32_t text, bool stop_at_digit = false) {
        if (!guest_pointer(text)) return;
        const std::uint32_t start = n;
        for (std::uint32_t i = 0; i < 120u; ++i) {
            const std::uint16_t c = read16(text + i * 2u);
            if (c == 0u || c == 13u || c == 10u) break;
            if (stop_at_digit && c >= '0' && c <= '9') break;
            add(c);
        }
        while (n > start && read_last() == ' ') --n;
    }
    std::uint16_t read_last() const {
        std::uint16_t c;
        std::memcpy(&c, p + (n - 1u) * 2u, 2u);
        return c;
    }
    void add_ascii(const char *s) {
        for (; *s != 0; ++s) add(static_cast<std::uint8_t>(*s));
    }
};

void label_collector_cars() {
    if (g_hooks.enabled && !g_hooks.enabled()) return;
    std::uint32_t count = 0u;
    const std::uint32_t table = string_table(count);
    if (table == 0u || count <= 2247u) return;
    const std::uint32_t signature_title = read32(table + kStringSignatureTitle * 4u);
    for (int i = 0; i < 20; ++i) {
        const std::uint32_t car = collector_car(i);
        if (car == 0u) continue;
        const std::uint32_t record = kCarTable + (car - 1u) * 40u;
        const std::uint32_t description = read32(record + 32u), title = read32(record + 36u);
        const std::uint32_t ours = kCollectorLines + static_cast<std::uint32_t>(i) * kLineBytes;
        if (title < count && read32(table + title * 4u) != signature_title) write32(table + title * 4u, signature_title);
        if (description >= count || read32(table + description * 4u) == ours) continue;
        std::uint8_t *p = g_memory->raw_pointer(ours, kLineBytes);
        if (p == nullptr) return;
        std::memset(p, 0, kLineBytes);
        Utf16Line line{p, 0u, kLineBytes / 2u};
        line.add_string(read32(table + kStringAwardedFor15 * 4u), true);
        line.add(' ');
        line.add_string(signature_title);
        line.add(' ');
        line.add_string(read32(table + kSignatureStrings[i] * 4u));
        line.add_ascii(" : ");
        line.add_string(read32(table + signature_place(kSignatureStrings[i]) * 4u));
        write32(table + description * 4u, ours);
    }
}

void check_signatures() {
    if (++g_ticks % 30u != 0u) return;
    for (std::uint32_t i = 0; i < 64u; i += 4u)
        if (read32(kProfileMarker + i) != 0xFFFFFFFFu) return;
    if (g_hooks.signature_done) {
        if (const std::uint8_t *done = g_memory->raw_pointer(kProfileSignatures, 20u))
            for (int i = 0; i < 20; ++i)
                if (done[i] == 1u && g_hooks.signature_done(i)) g_new = true;
    }
    // Tests (set_everything_won): the Special Events' filters unlocked, so
    // they can be turned on.
    if (g_everything_won && g_hooks.progress) g_hooks.progress(0x3FF, 10, 0);
    give_collector_cars();
    label_collector_cars();
    // A filter ON whose Special Event this profile has not won: off.
    if (g_hooks.active && g_hooks.toggle)
        for (int i = 0; i < bonus::postcard_count(); ++i) {
            const int filter = bonus::postcard_filter(i);
            if (filter >= 0 && g_hooks.active(filter) && !filter_won(filter)) g_hooks.toggle(filter);
        }
    // Postcards and Crash Headlines (counts; plausible values only).
    const std::uint32_t headlines = read32(kProgressHeadlines), percent = read32(kProgressPercent);
    std::uint32_t events = 0u;  // bit n: Special Event n won
    for (std::uint32_t i = 0; i < 10u; ++i)
        if (read8(kSpecialEventsWon + i) == 1u) events |= 1u << i;
    if (g_hooks.progress && headlines <= 32u && percent <= 100u)
        if (const int unlocked = g_hooks.progress(static_cast<int>(events), static_cast<int>(headlines), static_cast<int>(percent));
            unlocked > 0) {
            g_new = true;
            g_new_awards += unlocked;
        }
}

// Waiting for one of the game's pause rows to run (a YES/NO box over the
// menu): until its list leaves the screen, its rows stay where ours were.
void wait_game_row() {
    const std::uint32_t shown = read32(kWidgetView);
    bool same = widget_up() && shown == g_view && read32(shown + 0x14u) == kPause;
    for (std::uint32_t i = 0; same && i < kGameRows; ++i)
        same = read32(shown + 0x1Cu + i * 4u) == g_texts[i] && read32(g_texts[i] + 0x48u) == kTextItem && read32(g_bars[i] + 0x48u) == kBarItem;
    if (!same) {
        g_wait_away = false;
        return;
    }
    const std::uint32_t view = pause_view(kGameRows);
    // While the box is up the word after the list is 0 (no pause view); it
    // is 6 again when the box closed (NO).
    if (view == 0u) g_wait_box = true;
    if (view == 0u || !g_wait_box) {
        show_game_rows();
        return;
    }
    g_wait_away = false;
    patch_pause(view, true);  // back in the list with the same items
}

void enter_bonus(ListKind kind) {
    g_kind = kind;
    g_new = false;
    g_new_awards = 0;
    g_top = g_selected = 0;
    // The postcard's photo (taken when the pause menu opened: the race; in
    // the main menu, the last race's, or the menu itself if there was none).
    if (!g_photo) photo();
    refresh_bonus();
}

} // namespace

// The Heavyweights (the Crash vehicles, the car table's seventh class) in
// Single Event outside Crash: the class offered when car select opens (mode
// 9: Single Event, 0: World Tour; Crash offers it itself) and, while it is,
// the vehicles drivable: the spreadsheet's rule (2026-11-14), the n-th
// Heavyweight (heavy_rank) with the n-th Crash Headline won, the Firetruck
// with all ten (kHeavyTestAll: all eleven, the 0.0.50 test). The progress
// bytes written are the game's derived table (rebuilt from the profile, not
// saved).
constexpr std::uint32_t kSingleEvent = 9u;
constexpr std::uint32_t kHeavyType = 13u, kFiretruckType = 11u;
constexpr bool kHeavyTestAll = false;
bool g_heavy_test_all{};  // test builds: every Heavyweight drivable (set_heavy_test_all)
bool g_heavy_offered{};

bool heavy_vehicle(std::uint32_t car) {
    const std::uint32_t record = kCarTable + car * 40u;
    const std::uint32_t type = read32(record + 12u);
    return read32(record + 4u) == read32(kCarTable + 78u * 40u + 4u) && (type == kHeavyType || type == kFiretruckType);
}

int heavy_rank(std::uint32_t lo, std::uint32_t hi);
void offer_heavy_vehicles() {
    if (!g_heavy_offered) return;
    const std::uint32_t cars = read32(kCarCount);
    if (cars == 0u || cars > 128u) return;
    for (std::uint32_t car = 0; car < cars; ++car) {
        if (!heavy_vehicle(car)) continue;
        const int rank = heavy_rank(read32(kCarTable + car * 40u), read32(kCarTable + car * 40u + 4u));
        const bool won = kHeavyTestAll || g_heavy_test_all || (rank >= 0 && headline_won(rank));
        if (std::uint8_t *flag = g_memory->raw_pointer(kProgress + car, 1u)) *flag = won ? 1u : 0u;
    }
}

// A Heavyweight against Heavyweights is no fun (Road Rage: buses that take
// no takedown): when the player takes one in Single Event, the rivals the
// game drew from the same class become Super cars. The race's vehicle list
// (0x08D76F80, then a copy at 0x08C2ED58 and each racer's record: vehicle
// id, then +16 its class) is written in one frame between car select and
// the track loading; the rivals are swapped there, before their models load.
constexpr std::uint32_t kRaceVehicles = 0x08D76F80u, kRaceVehiclesCopy = 0x08C2ED58u;
constexpr std::uint32_t kRacersFrom = 0x08D5C000u, kRacersTo = 0x08D70000u;
constexpr std::uint32_t kSuperClass = 4u, kHeavyClass = 6u;
bool g_rivals_swapped{};

std::uint32_t heavy_hash() { return read32(kCarTable + 78u * 40u + 4u); }

// A Heavyweight's place in the unlock order (0: the Heavy Pick-Up, cheapest
// in Crash money; 10: the Firetruck), -1 if not one.
int heavy_rank(std::uint32_t lo, std::uint32_t hi) {
    const std::uint32_t cars = read32(kCarCount);
    std::uint32_t mine = 0xFFFFFFFFu;
    for (std::uint32_t car = 0; car < cars && car < 128u; ++car)
        if (read32(kCarTable + car * 40u) == lo && read32(kCarTable + car * 40u + 4u) == hi) mine = car;
    if (mine == 0xFFFFFFFFu || !heavy_vehicle(mine)) return -1;
    if (read32(kCarTable + mine * 40u + 12u) == kFiretruckType) return 10;
    const std::uint32_t price = read32(kCarTable + mine * 40u + 16u);
    int rank = 0;
    for (std::uint32_t car = 0; car < cars && car < 128u; ++car)
        if (car != mine && heavy_vehicle(car) && read32(kCarTable + car * 40u + 12u) == kHeavyType &&
            read32(kCarTable + car * 40u + 16u) < price)
            ++rank;
    return rank;
}

// GARAGE 2.0: the game's garage (the car select screen, object at
// 0x08D88BD0) showing only the 2.0 rewards: each class's Type 1 (its paints)
// and Collector cars, every Heavyweight, no Race Special class. The screen
// fills, for the class shown, its list (+0x0C: u16 cursor, u16 count, rows
// of {UTF-16 name, 1 = locked}) and two tables of the cars behind the rows
// (+0xB0 and +0x1A0: {car data, u32 unlocked << 16 | row}, 12 bytes; their
// counts at +0x2E4 and +0x2E8); a row is the class's car whose display
// order (car record +8) is the class's first plus the row. Those are cut
// down to the rewards the frame the game fills them (count back to the
// class's own).
constexpr std::uint32_t kSelect = 0x08D88BD0u;
constexpr std::uint32_t kSelectList = kSelect + 0x0Cu, kSelectCars = kSelect + 0xB0u, kSelectCars2 = kSelect + 0x1A0u;
constexpr std::uint32_t kSelectCount = kSelect + 0x2E4u, kSelectCount2 = kSelect + 0x2E8u;
constexpr std::uint32_t kSelectClass = kSelect + 752u, kSelectNextClass = kSelect + 756u;
constexpr std::uint32_t kCollectorType = 18u, kCollectorType5 = 19u, kRaceSpecialClass = 5u;
bool g_garage20{};  // the garage was opened from BONUS LEGENDS 2.0
bool g_trax20{};    // the EA Trax screen was opened from BONUS LEGENDS 2.0 (2026-11-14)
int g_garage_rows = -1;  // rows left after the cut (-1: not cut yet)

// The class's cars in display order (car table indices), how many.
int class_cars(std::uint32_t klass, std::uint32_t *out, int max) {
    const std::uint32_t cars = read32(kCarCount);
    std::uint32_t hash = 0u;
    {
        std::uint32_t seen = 0u, last = 0u;
        for (std::uint32_t car = 0; car < cars && car < 128u; ++car) {
            const std::uint32_t h = read32(kCarTable + car * 40u + 4u);
            if (h == last) continue;
            last = h;
            if (seen++ == klass) {
                hash = h;
                break;
            }
        }
    }
    int n = 0;
    for (std::uint32_t car = 0; car < cars && car < 128u && n < max; ++car)
        if (read32(kCarTable + car * 40u + 4u) == hash) out[n++] = car;
    std::sort(out, out + n, [](std::uint32_t a, std::uint32_t b) { return read32(kCarTable + a * 40u + 8u) < read32(kCarTable + b * 40u + 8u); });
    return n;
}

// Type 1 (its paints), the Collector cars given for Signature Takedowns (the
// game's own Collectors, won in World Tour, are left out), the Heavyweights.
bool garage_reward(std::uint32_t klass, int row, std::uint32_t car) {
    if (klass == kHeavyClass) return true;
    if (row == 0) return true;
    for (int i = 0; i < 20; ++i)
        if (collector_car(i) == car + 1u) return true;
    return false;
}

// The garage's line at the bottom (the car's description, car record +32:
// a string id) for the 2.0 rewards while the 2.0 garage is up: each class's
// Type 1 tells the paints won, each Heavyweight its Single Event condition.
// The game's pointers come back as the garage closes (the Collector cars
// keep theirs: label_collector_cars).
constexpr std::uint32_t kGarageTexts = kArena + 0x1400u;  // 16 x kTextBytes
struct Relabel { std::uint32_t id, original; };
Relabel g_relabels[16];
int g_relabelled = 0;
std::uint32_t g_type1_text[5]{};  // each class's Type 1 line (its paints)
int g_garage_saved_paint = -2;    // the race paint, kept while the garage previews others (-2: none kept)

void garage_label(std::uint32_t table, std::uint32_t count, std::uint32_t car, const std::string &text) {
    if (g_relabelled >= 16) return;
    const std::uint32_t id = read32(kCarTable + car * 40u + 32u);
    if (id >= count) return;
    const std::uint32_t at = kGarageTexts + static_cast<std::uint32_t>(g_relabelled) * kTextBytes;
    write_text(at, text);
    g_relabels[g_relabelled++] = {id, read32(table + id * 4u)};
    write32(table + id * 4u, at);
}

void garage_labels(bool on) {
    std::uint32_t count = 0u;
    const std::uint32_t table = string_table(count);
    if (table == 0u) return;
    if (!on) {
        for (int i = g_relabelled - 1; i >= 0; --i) write32(table + g_relabels[i].id * 4u, g_relabels[i].original);
        g_relabelled = 0;
        return;
    }
    if (g_relabelled != 0) return;
    const bool fr = french();
    // Colours unlocked by the Takedown Trophies, on each class's Type 1.
    const std::string paints = (fr ? "COULEURS DEBLOQUEES: " : "COLOURS UNLOCKED: ") +
                               std::to_string(paints::unlocked_colours(*g_memory)) + "/" + std::to_string(paints::kTrophyColours) + " - " +
                               (fr ? "LES TROPHEES TAKEDOWN" : "THE TAKEDOWN TROPHIES");
    for (std::uint32_t klass = 0; klass < kRaceSpecialClass; ++klass) {
        std::uint32_t cars[32];
        g_type1_text[klass] = 0u;
        if (class_cars(klass, cars, 32) > 0) {
            g_type1_text[klass] = kGarageTexts + static_cast<std::uint32_t>(g_relabelled) * kTextBytes;
            garage_label(table, count, cars[0], paints);
        }
    }
    // The Heavyweights: Single Event, each given by its Crash Headline (the
    // Firetruck by all ten), as BONUS 2.0's Crash Headlines page pairs them.
    for (int rank = 0; rank < 11; ++rank) {
        const int car = heavy_by_rank(rank);
        if (car < 0) continue;
        const std::string headline = rank < 10 ? one_line(game_string(kHeadlineTitles[rank]), "?") : one_line(game_string(370u), "ALL HEADLINES");
        const std::string text = (fr ? "COURSE SIMPLE: " : "SINGLE EVENT: ") + headline + (headline_won(rank) ? (fr ? " - OK" : " - DONE") : "");
        garage_label(table, count, static_cast<std::uint32_t>(car), text);
    }
}

// The screen's title ("SELECT OPTION", a title item in the menu's pool: its
// text pointer at +0x20) reads "SIGNATURE TAKEDOWNS" in the game's words
// while BONUS 2.0's Signature list is up (the user's mock-up, 2026-11-11).
constexpr std::uint32_t kTitleItem = 0x08945F28u;
constexpr std::uint32_t kTitleText = kArena + 0x1C00u;
void title_update() {
    static std::uint32_t item = 0u, original = 0u;
    const std::uint32_t string = g_kind == ListKind::Signatures ? kStringSignatures
                                 : g_kind == ListKind::Trophies ? kStringTrophies
                                 : g_kind == ListKind::Filters  ? 1221u
                                 : g_kind == ListKind::Headlines ? kStringHeadlines
                                                                 : 0u;
    const bool want = g_screen == Screen::Main && g_level == Level::Bonus && string != 0u;
    static std::uint32_t shown = 0u;
    if (!want) {
        if (item != 0u && read32(item + 0x48u) == kTitleItem && read32(item + 0x20u) == kTitleText) write32(item + 0x20u, original);
        item = 0u;
        return;
    }
    if (item == 0u || read32(item + 0x48u) != kTitleItem) {
        item = 0u;
        for (std::uint32_t at = kPool; at < kPoolEnd; at += kItem)
            if (read32(at + 0x48u) == kTitleItem && guest_pointer(read32(at + 0x20u)) && readf(at + 4u) < 20.0f) {
                item = at;
                original = read32(at + 0x20u);
                break;
            }
        if (item == 0u) return;
        shown = 0u;
    }
    if (shown != string) {
        write_text(kTitleText, one_line(game_string(string), "BONUS 2.0"));
        shown = string;
    }
    if (read32(item + 0x20u) != kTitleText) {
        original = read32(item + 0x20u);
        write32(item + 0x20u, kTitleText);
    }
}

void garage20_update() {
    static const bool forced = std::getenv("PSPRECOMP_BURNOUT_GARAGE20") != nullptr;  // PC tests: the Driver Details garage
    const bool on = g_garage20 || forced;
    const std::uint32_t view = read32(kWidgetView);
    if (!on || !widget_up() || !guest_pointer(view) || read32(view + 0x14u) != kSelectList) {
        g_garage_rows = -1;
        g_colour_prompt = false;
        if (g_relabelled != 0) garage_labels(false);
        if (g_garage_saved_paint != -2) {
            paints::set_chosen(g_garage_saved_paint);
            g_garage_saved_paint = -2;
        }
        return;
    }
    if (g_garage_saved_paint == -2) g_garage_saved_paint = paints::chosen();
    garage_labels(true);
    const std::uint32_t current = read32(kSelectClass);
    // (No SQUARE paint preview since the extra paints went: no prompt.)
    g_colour_prompt = false;
    if (current == kRaceSpecialClass) return;
    // The rows on screen show the list as cut (the view copies a row's
    // name only when it scrolls to it); rows past its end are blank.
    if (g_garage_rows > 0) {
        const std::uint32_t shown = std::min<std::uint32_t>(read32(view + 0x0Cu), 8u), scroll = read32(view + 0x10u);
        write_text(kEmptyText, " ");
        for (std::uint32_t i = 0; i < shown; ++i) {
            const std::uint32_t item = read32(view + 0x1Cu + i * 4u);
            if (!guest_pointer(item) || read32(item + 0x48u) != kTextItem) continue;
            const std::uint32_t row = scroll + i;
            write32(item + 0x24u, row < static_cast<std::uint32_t>(g_garage_rows) ? read32(kSelectList + 4u + row * 8u) : kEmptyText);
        }
    }
    std::uint32_t cars[32];
    const int n = class_cars(current, cars, 32);
    const std::uint32_t count = read16(kSelectList + 2u);
    if (n == 0 || count != static_cast<std::uint32_t>(n) || read32(kSelectCount) != count) return;  // not (re)filled for this class
    // The rows to keep, in order.
    struct Row { std::uint32_t name, locked; std::uint32_t car[3], car2[3]; };
    Row rows[32];
    int kept = 0;
    for (int i = 0; i < n; ++i) {
        if (!garage_reward(current, i, cars[i])) continue;
        Row &r = rows[kept++];
        r.name = read32(kSelectList + 4u + static_cast<std::uint32_t>(i) * 8u);
        r.locked = read32(kSelectList + 8u + static_cast<std::uint32_t>(i) * 8u);
        for (int k = 0; k < 3; ++k) {
            r.car[k] = read32(kSelectCars + static_cast<std::uint32_t>(i) * 12u + static_cast<std::uint32_t>(k) * 4u);
            r.car2[k] = read32(kSelectCars2 + static_cast<std::uint32_t>(i) * 12u + static_cast<std::uint32_t>(k) * 4u);
        }
    }
    if (kept == 0 || kept == n) {
        g_garage_rows = kept;
        return;
    }
    for (int i = 0; i < kept; ++i) {
        write32(kSelectList + 4u + static_cast<std::uint32_t>(i) * 8u, rows[i].name);
        write32(kSelectList + 8u + static_cast<std::uint32_t>(i) * 8u, rows[i].locked);
        for (int k = 0; k < 3; ++k) {
            write32(kSelectCars + static_cast<std::uint32_t>(i) * 12u + static_cast<std::uint32_t>(k) * 4u, rows[i].car[k]);
            write32(kSelectCars2 + static_cast<std::uint32_t>(i) * 12u + static_cast<std::uint32_t>(k) * 4u, rows[i].car2[k]);
        }
    }
    write16(kSelectList + 2u, static_cast<std::uint16_t>(kept));
    write32(kSelectCount, static_cast<std::uint32_t>(kept));
    write32(kSelectCount2, static_cast<std::uint32_t>(kept));
    if (read16(kSelectList) >= kept) write16(kSelectList, 0u);
    g_garage_rows = kept;
    std::fprintf(stderr, "[garage] class %u: %d of %d cars shown\n", current, kept, n);
}

// BONUS LEGENDS 2.0 > GARAGE opens the game's garage straight from the main
// menu, the way REWARDS does (0x088C0890, the garage screen's opening: the
// screen switch and the car select set up); leaving it, the game goes back to
// REWARDS, which is switched at once to the main menu (0x088A8800, the
// game's screen switch). Both are run by the game's own thread at its next
// controller read (take_guest_call).
constexpr std::uint32_t kOpenGarage = 0x088C0890u, kOpenMainMenu = 0x088B7004u;
// BONUS LEGENDS 2.0 > EA GAMES TRAX (2026-11-14): the EA Trax screen's opening
// (0x088C6524, as Driver Details > Settings runs it: the screen switch, the
// player and the shuffle set up), back to BONUS 2.0 as from the garage.
constexpr std::uint32_t kOpenTrax = 0x088C6524u, kTraxScreen = 0x08D7AAE0u, kSettingsScreen = 0x08D7AA14u;
constexpr std::uint32_t kGarageScreen = 0x08D7A1C8u, kRewardsScreen = 0x08D7A158u, kMainMenuScreen = 0x08D793F0u;
constexpr std::uint32_t kCurrentScreen = 0x08D77BE0u;
struct GuestCall { std::uint32_t function, a0, a1; };
GuestCall g_call{};
bool g_call_pending = false;
GuestCall g_call_next{};  // run at the read after g_call
bool g_call_next_pending = false;
// The screen manager's exit of the screen shown (its interface parts told
// to let go of their items: the ticker and prompts at the bottom), as the
// game runs it when a screen fades out; the garage's opening alone left the
// main menu's on screen.
constexpr std::uint32_t kExitScreen = 0x088844E4u, kScreenManager = 0x08D779C0u;

void garage_return_tick() {
    if (!g_garage20 && !g_trax20) return;
    const std::uint32_t screen = read32(kCurrentScreen);
    static std::uint32_t last = 0u;
    if (screen != last) std::fprintf(stderr, "[garage] screen %08X\n", screen);
    last = screen;
    // Leaving the garage the game asks its menus for the way back (screen
    // manager 0x08D779C0: +0x214 state 6, no screen, +0x218 = 3), which only
    // REWARDS answers: here the main menu is opened instead.
    static int idle = 0;
    idle = screen == 0u && read32(kCurrentScreen - 0x0Cu) == 6u ? idle + 1 : 0;
    if ((screen == kRewardsScreen || (g_trax20 && screen == kSettingsScreen) || idle >= 2) && !g_call_pending) {
        idle = 0;
        g_call = {kOpenMainMenu, kMainMenuScreen, 0u};
        g_call_pending = true;
    } else if (screen == kMainMenuScreen) {
        const std::uint32_t view = read32(kWidgetView);
        if (widget_up() && guest_pointer(view) && read32(view + 0x14u) == kMainList) {
            write16(kMainList, static_cast<std::uint16_t>(kBonusRow));  // back on BONUS 2.0
            g_garage20 = false;
            g_trax20 = false;
            g_reopen_rewards = true;
        }
    }
}

// No Race Special class in the 2.0 garage: as RIGHT is read on Super (LEFT
// on the Heavyweights) the screen is told it is on Race Special, so it steps
// over it (and loads the next class's cars: setting the class after the
// step showed Race Special's first car).
void garage20_arrows(std::uint32_t buttons) {
    static std::uint32_t previous = 0u;
    const std::uint32_t pressed = buttons & ~previous;
    previous = buttons;
    static const bool forced = std::getenv("PSPRECOMP_BURNOUT_GARAGE20") != nullptr;
    if (!(g_garage20 || forced) || pressed == 0u || !widget_up()) return;
    const std::uint32_t view = read32(kWidgetView);
    if (!guest_pointer(view) || read32(view + 0x14u) != kSelectList) return;
    const std::uint32_t current = read32(kSelectClass);
    if (current != read32(kSelectNextClass)) return;  // already moving
    constexpr bool kPaintPreview = false;  // the extra paints are retired (2026-11-11)
    if (kPaintPreview && (pressed & kSquare) != 0u && current < kRaceSpecialClass && read16(kSelectList) == 0u && g_type1_text[current] != 0u) {
        // The Type 1's 2.0 paints, one after the other, then its own colours.
        const int paint = paints::next_won(paints::chosen());
        paints::set_chosen(paint);
        const bool fr = french();
        std::string line;
        if (paint < 0) {
            line = fr ? "COULEURS D'ORIGINE - CARRE: COULEURS 2.0" : "ORIGINAL COLOURS - SQUARE: 2.0 PAINTS";
        } else if (paint < 20) {
            line = std::string(paints::paint(paint).name) + ": " +
                   one_line(game_string(read32(kObjectiveNames + static_cast<std::uint32_t>(paint) * 4u)), "?");
        } else {
            line = std::string(paints::paint(paint).name) + ": " +
                   one_line(game_string(read32(kTrophyNames + static_cast<std::uint32_t>(paint - 20) * 4u)), "?") + (fr ? " - TROPHEE" : " - TROPHY");
        }
        write_text(g_type1_text[current], line);
        return;
    }
    if (((pressed & kRight) != 0u && current == kRaceSpecialClass - 1u) || ((pressed & kLeft) != 0u && current == kHeavyClass)) {
        write32(kSelectClass, kRaceSpecialClass);
        write32(kSelectNextClass, kRaceSpecialClass);
    }
}

void swap_rivals() {
    if (!g_heavy_offered) return;
    const std::uint32_t heavy = heavy_hash();
    const std::uint32_t player_lo = read32(kRaceVehicles), player_hi = read32(kRaceVehicles + 4u);
    if (player_hi == 0u) {
        g_rivals_swapped = false;  // no race set up yet (or a new one coming)
        return;
    }
    if (g_rivals_swapped || player_hi != heavy) return;
    // The rivals' class goes down as the player's Heavyweight goes up the
    // unlock order (the user's idea, 2026-11-09: the first ones push, the
    // Firetruck smashes everything): ranks 1-3 Super, 4-5 Sport, 6-7 Coupe,
    // 8-9 Muscle, 10-11 Compact (the lightest and slowest).
    const int rank = heavy_rank(player_lo, player_hi);
    static constexpr std::uint32_t kRivalClass[11]{4, 4, 4, 3, 3, 2, 2, 1, 1, 0, 0};
    const std::uint32_t rival_class = rank >= 0 && rank < 11 ? kRivalClass[rank] : kSuperClass;
    // That class's cars in the car table's order (its Type 1, 2, 3 first).
    const std::uint32_t cars = read32(kCarCount);
    std::uint32_t class_hash = 0u;
    {
        std::uint32_t seen = 0u, last = 0u;
        for (std::uint32_t car = 0; car < cars && car < 128u; ++car) {
            const std::uint32_t h = read32(kCarTable + car * 40u + 4u);
            if (h == last) continue;
            last = h;
            if (seen++ == rival_class) {
                class_hash = h;
                break;
            }
        }
    }
    std::uint32_t supers[8][2]{};
    int count = 0;
    for (std::uint32_t car = 0; car < cars && car < 128u && count < 8; ++car)
        if (read32(kCarTable + car * 40u + 4u) == class_hash) {
            supers[count][0] = read32(kCarTable + car * 40u);
            supers[count][1] = class_hash;
            ++count;
        }
    if (count == 0) return;
    int swapped = 0;
    for (std::uint32_t i = 1; i < 8u; ++i) {
        const std::uint32_t lo = read32(kRaceVehicles + i * 8u), hi = read32(kRaceVehicles + i * 8u + 4u);
        if (hi != heavy) continue;
        const std::uint32_t *with = supers[(i - 1u) % static_cast<std::uint32_t>(count)];
        write32(kRaceVehicles + i * 8u, with[0]);
        write32(kRaceVehicles + i * 8u + 4u, with[1]);
        for (std::uint32_t at = kRaceVehiclesCopy; at < kRaceVehiclesCopy + 0x40u; at += 8u)
            if (read32(at) == lo && read32(at + 4u) == hi) {
                write32(at, with[0]);
                write32(at + 4u, with[1]);
            }
        for (std::uint32_t at = kRacersFrom; at < kRacersTo; at += 4u)
            if (read32(at) == lo && read32(at + 4u) == hi && read32(at + 16u) == kHeavyClass) {
                write32(at, with[0]);
                write32(at + 4u, with[1]);
                write32(at + 16u, rival_class);
            }
        ++swapped;
    }
    g_rivals_swapped = true;
    std::fprintf(stderr, "[cars] Heavyweight rank %d: %d rivals from class %u\n", rank + 1, swapped, rival_class);
    (void)player_lo;
}

namespace {
void award_photo_update();
void queue_all_extra_awards();
}

void configure(Hooks hooks) { g_hooks = std::move(hooks); }

namespace {
// Tests: the time update() takes (PSPRECOMP_BURNOUT_UPDATE_TIMING: every 600
// calls, the mean and the worst, with the screen then).
struct UpdateTiming {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~UpdateTiming() {
        static const bool on = std::getenv("PSPRECOMP_BURNOUT_UPDATE_TIMING") != nullptr;
        if (!on) return;
        static double total = 0.0, worst = 0.0;
        static int calls = 0;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        total += us, worst = std::max(worst, us);
        if (++calls == 600) {
            std::fprintf(stderr, "[timing] bonus update: mean %.1f us, worst %.1f us (screen %08X)\n", total / calls, worst,
                         g_memory != nullptr ? read32(kCurrentScreen) : 0u);
            total = worst = 0.0, calls = 0;
        }
    }
};
} // namespace

void update(psprecomp::GuestMemory &memory) {
    trax::update(memory);  // the added EA Trax tracks' texts
    const UpdateTiming timing;
    g_memory = &memory;
    if (!g_hooks.enabled || !g_hooks.filter_count) return;
    // PC tests: PSPRECOMP_BURNOUT_EVERYTHING_WON as the Vita's test builds.
    if (static bool asked = std::getenv("PSPRECOMP_BURNOUT_EVERYTHING_WON") != nullptr; asked && g_strings_inside != 0u) {
        asked = false;
        set_everything_won(true);
    }
    check_signatures();
    // Tests: the game's progress block (0x08BBFDF0, 1 KiB) to
    // PSPRECOMP_BURNOUT_DUMP_PROGRESS once the menu's texts are found.
    if (static const char *dump = std::getenv("PSPRECOMP_BURNOUT_DUMP_PROGRESS"); dump != nullptr && g_strings_inside != 0u) {
        if (const std::uint8_t *p = g_memory->raw_pointer(kProgress, 0x400u))
            if (std::FILE *out = std::fopen(dump, "wb")) {
                std::fwrite(p, 1, 0x400u, out);
                std::fclose(out);
            }
        dump = nullptr;
    }
    // Tests: every game text, "id: text", to PSPRECOMP_BURNOUT_DUMP_STRINGS (once).
    if (static const char *dump = std::getenv("PSPRECOMP_BURNOUT_DUMP_STRINGS"); dump != nullptr && g_strings_inside != 0u) {
        if (std::FILE *out = std::fopen(dump, "w")) {
            for (std::uint32_t id = 0; id < 6000u; ++id)
                if (game_string(id) != 0u) std::fprintf(out, "%u: %s\n", id, one_line(game_string(id), "").c_str());
            std::fclose(out);
        }
        dump = nullptr;
    }
    award_photo_update();
    paints::update(memory, g_hooks.enabled());
    if (g_hooks.menu_up) g_hooks.menu_up(widget_up());
    if (g_screen == Screen::Main) g_heavy_offered = false;  // back in the main menu: car select closed
    garage_return_tick();
    garage20_update();
    title_update();
    {
        // Tests: each change of the game's current screen (0x08D77BE0).
        static const bool trace = std::getenv("PSPRECOMP_BURNOUT_SCREEN_TRACE") != nullptr;
        static std::uint32_t last = 0u, frame = 0u;
        ++frame;
        // The game's award screens (0x08D7D430: a car, 0x08D7D440: counts,
        // 0x08D7D444: a picture; the award shown at 0x08D89448, its type at
        // 0x08D8944C: 2 a Signature photo, 7 a car) always logged: the other
        // types (postcard, Crash Headline, trophy) are not known yet.
        const std::uint32_t screen = read32(0x08D77BE0u);
        const bool award = screen >= 0x08D7D430u && screen <= 0x08D7D44Cu;
        static std::uint32_t last_award = 0u;
        const std::uint32_t shown = read32(0x08D89448u) | (read32(0x08D8944Cu) << 16u);
        if ((trace || award) && (screen != last || (award && shown != last_award)))
            std::fprintf(stderr, "[screen] update %u: %08X (award %u type %u)\n", frame, screen, read32(0x08D89448u), read32(0x08D8944Cu));
        last = screen;
        last_award = shown;
        // Tests: at update FRAME (during a race), every World Tour event gold
        // and every Signature Takedown done in the live profile: the game
        // then shows every award they give at the race's end.
        static const char *all_at = std::getenv("PSPRECOMP_BURNOUT_FORCE_ALL_AT");
        // Or armed (force_all_rewards): once the player's car has gone over
        // 30 mph for 3 seconds in a race.
        static int racing = 0;
        bool fire = all_at != nullptr && frame == static_cast<std::uint32_t>(std::strtoul(all_at, nullptr, 10));
        if (g_force_all) {
            const float mph = readf(kCarSpeed);
            racing = mph > 30.0f && mph < 400.0f ? racing + 1 : 0;
            if (racing > 180) {
                fire = true;
                g_force_all = false;
                if (g_force_all_fired) g_force_all_fired();
            }
        }
        if (fire) {
            if (std::uint8_t *events = g_memory->raw_pointer(kProfile + 0xF56Au, 0xF61Eu - 0xF56Au)) {
                std::memset(events, 3, 0xF5B5u - 0xF56Au);
                std::memset(events + (0xF5BAu - 0xF56Au), 3, 0xF61Eu - 0xF5BAu);
                // One event left undone: the game at 100 % plays its credits
                // first (and on the Vita, 0.0.69, ran out of memory in them).
                if (std::getenv("PSPRECOMP_BURNOUT_FORCE_ALL_CREDITS") == nullptr) events[0xF61Du - 0xF56Au] = 0xFFu;
            }
            if (std::uint8_t *done = g_memory->raw_pointer(kProfileSignatures, 20u)) std::memset(done, 1, 20u);
            std::fprintf(stderr, "[test] update %u: every event gold, every Signature done\n", frame);
            queue_all_extra_awards();
        }
    }
    offer_heavy_vehicles();
    swap_rivals();
    if (g_hooks.car_speed) {
        const float mph = readf(kCarSpeed);
        g_hooks.car_speed(mph >= 0.0f && mph < 400.0f ? mph : 0.0f);
    }
    // No menu for a while (screens fade for ~52 vblanks between AUDIO and
    // the pause menu): the next pause opens on its first page.
    static int down = 0;
    down = widget_up() ? 0 : down + 1;
    if (down > 120) {
        g_reopen = Level::Main;
        g_reopen_cursor = 0u;
    }
    if (g_screen != Screen::None && !intact()) {
        // The view went to another list or closed: only the list is ours to
        // put back (the items may already belong to something else).
        restore(false);
    }
    if (g_screen == Screen::None) {
        card(-1);
        if (g_wait_away) wait_game_row();
        else if (const std::uint32_t view = pause_view(kGameRows); view != 0u && read16(kPause + 2u) == kGameRows) {
            patch_pause(view, false);
        } else if (g_hooks.enabled()) {
            if (const std::uint32_t menu = main_view(kMainList, kMainDescriptions);
                menu != 0u && (read16(kMainList + 2u) == kMainRows || (g_main_built && read16(kMainList + 2u) == kMainRows + 1u)))
                patch_main(menu);
        }
        publish_card();
        return;
    }
    if (g_hold_cursor > 0) {
        --g_hold_cursor;
        write16(g_list, g_reopen_cursor < g_count ? g_reopen_cursor : 0u);
    }
    show();
    // Bonus: the item under the cursor on the cards once the cursor rests
    // a moment (its pictures are loaded then).
    static int last = -1, rest = 0;
    const bool listed = g_level == Level::Bonus && g_kind != ListKind::Rewards;
    const int hovered = listed ? static_cast<int>(g_kind) * 1000 + g_selected : -1;
    rest = hovered == last ? rest + 1 : 0;
    last = hovered;
    if (hovered < 0) card(-1);
    else if (rest >= 4) show_cards(static_cast<int>(g_kind), g_selected);
    publish_card();
}

namespace {

// The buttons the menu takes from the game for this press.
std::uint32_t consume(std::uint32_t buttons) {
    const std::uint32_t pressed = buttons & ~g_previous;
    g_previous = buttons;
    if (g_memory == nullptr || g_screen == Screen::None || !intact()) return 0u;
    if (pressed != 0u) g_hold_cursor = 0;
    if (g_level == Level::Bonus) {
        // Our own cursor over the list (the rows scroll).
        const int n = list_size(g_kind);
        const int shown = static_cast<int>(g_visible);
        if ((pressed & kUp) != 0u) g_selected = (g_selected + n - 1) % n;
        if ((pressed & kDown) != 0u) g_selected = (g_selected + 1) % n;
        if ((pressed & kLeft) != 0u) g_selected = std::max(0, g_selected - shown);
        if ((pressed & kRight) != 0u) g_selected = std::min(n - 1, g_selected + shown);
        if (g_selected < g_top) g_top = g_selected;
        if (g_selected >= g_top + shown) g_top = g_selected - shown + 1;
        if ((pressed & kCross) != 0u) {
            if (g_kind == ListKind::Filters && filter_won(bonus::postcard_filter(g_selected))) {
                g_hooks.toggle(bonus::postcard_filter(g_selected));
                g_card_item = -2;  // the card's ON/OFF again
            } else if (g_kind == ListKind::Rewards && g_selected == 4) {
                // EA GAMES TRAX: the main menu's own rows back, then the game
                // opens its EA Trax screen (as GARAGE did, below).
                card(-1);
                g_level = Level::Main;
                g_count = rows_of(Level::Main, g_rows);
                show();
                write16(g_list, static_cast<std::uint16_t>(kBonusRow));
                for (std::uint32_t i = 0; i < g_visible && i < kMainRows; ++i)
                    write32(read32(g_view + 0x1Cu + i * 4u) + 0x24u, i == kBonusRow ? bonus_label() : g_labels[i]);
                g_trax20 = true;
                write_text(kListDescription, " ");
                g_call = {kExitScreen, kScreenManager, 0u};
                g_call_pending = true;
                g_call_next = {kOpenTrax, kTraxScreen, 0u};
                g_call_next_pending = true;
                return kUp | kDown | kLeft | kRight | kCross | kCircle | kTriangle;
            } else if (g_kind == ListKind::Rewards && g_selected < 4) {
                static constexpr ListKind kPages[4]{ListKind::Signatures, ListKind::Trophies, ListKind::Filters, ListKind::Headlines};
                enter_bonus(kPages[std::clamp(g_selected, 0, 3)]);
                return kUp | kDown | kLeft | kRight | kCross | kCircle | kTriangle;
            } else if (false) {  // (GARAGE, the 2.0 garage: no row since 2026-11-11)
                // GARAGE: the main menu's own rows back, then the game opens its garage.
                card(-1);
                g_level = Level::Main;
                g_count = rows_of(Level::Main, g_rows);
                show();
                write16(g_list, static_cast<std::uint16_t>(kBonusRow));
                for (std::uint32_t i = 0; i < g_visible && i < kMainRows; ++i)
                    write32(read32(g_view + 0x1Cu + i * 4u) + 0x24u, i == kBonusRow ? bonus_label() : g_labels[i]);
                g_garage20 = true;
                // The help ticker at the bottom keeps the text it shows (ours,
                // GARAGE's line) while the garage comes in: blanked.
                write_text(kListDescription, " ");
                g_call = {kExitScreen, kScreenManager, 0u};
                g_call_pending = true;
                g_call_next = {kOpenGarage, kGarageScreen, 0u};
                g_call_next_pending = true;
                return kUp | kDown | kLeft | kRight | kCross | kCircle | kTriangle;
            }
        }
        if ((pressed & (kCircle | kTriangle)) != 0u && g_screen == Screen::Main && g_kind != ListKind::Rewards) {
            // Back up to the rewards, on the list just left.
            card(-1);
            const int from = g_kind == ListKind::Signatures ? 0 : g_kind == ListKind::Trophies ? 1 : g_kind == ListKind::Filters ? 2 : 3;
            enter_bonus(ListKind::Rewards);
            g_selected = from;
            refresh_bonus();
            return kUp | kDown | kLeft | kRight | kCross | kCircle | kTriangle;
        }
        if ((pressed & (kCircle | kTriangle)) != 0u) {
            card(-1);
            if (g_screen == Screen::Main) {
                // Back on BONUS LEGENDS 2.0, the third row.
                g_level = Level::Main;
                g_count = rows_of(Level::Main, g_rows);
                show();
                write16(g_list, static_cast<std::uint16_t>(kBonusRow));
                for (std::uint32_t i = 0; i < g_visible && i < kMainRows; ++i)
                    write32(read32(g_view + 0x1Cu + i * 4u) + 0x24u, i == kBonusRow ? bonus_label() : g_labels[i]);
            } else {
                enter(Level::Options, 2u);
            }
        } else if ((pressed & (kUp | kDown | kLeft | kRight | kCross)) != 0u) {
            refresh_bonus();
        }
        return kUp | kDown | kLeft | kRight | kCross | kCircle | kTriangle;
    }
    if (g_screen == Screen::Main) {
        // BONUS LEGENDS 2.0 is the third row (MULTIPLAYER's: CROSS is taken
        // from the game there); the game's other rows run as they are.
        if ((pressed & kCross) != 0u && cursor() == kBonusRow) {
            g_scroll = 0u;
            enter_bonus(ListKind::Rewards);
            return kCross;
        }
        // The empty fifth row is skipped (the cursor wraps on the four).
        if (read16(g_list + 2u) > kMainRows) {
            if ((pressed & kDown) != 0u && cursor() >= kMainRows - 1u) {
                write16(g_list, 0u);
                return kDown;
            }
            if ((pressed & kUp) != 0u && cursor() == 0u) {
                write16(g_list, static_cast<std::uint16_t>(kMainRows - 1u));
                return kUp;
            }
        }
        return 0u;
    }
    const std::uint16_t row = std::min<std::uint16_t>(cursor(), static_cast<std::uint16_t>(g_count - 1));
    const Row &r = g_rows[row];
    if ((pressed & kCross) != 0u) {
        switch (r.action) {
        case Action::Game:
            // The game's rows back, its cursor on the row, before it reads CROSS.
            g_reopen = g_level;
            g_reopen_cursor = row;
            restore(true);
            g_wait_away = true;
            g_wait_box = false;
            write16(kPause, static_cast<std::uint16_t>(r.value));
            return 0u;
        case Action::Options:
            enter(Level::Options, 0u);
            break;
        case Action::Bonus:
            enter_bonus(ListKind::Filters);
            break;
        case Action::Filter:
            break;
        case Action::BonusSwitch:
        case Action::Picture: {
            if (r.action == Action::BonusSwitch) g_hooks.set_enabled(!g_hooks.enabled());
            else g_hooks.set_psp_picture(!(g_hooks.psp_picture && g_hooks.psp_picture()));
            // The rows again (BONUS comes and goes), the cursor on the same setting.
            const Action action = r.action;
            g_count = rows_of(g_level, g_rows);
            std::uint16_t at = 0u;
            for (int i = 0; i < g_count; ++i)
                if (g_rows[i].action == action) at = static_cast<std::uint16_t>(i);
            enter(g_level, at);
            break;
        }
        }
        return kCross;
    }
    if ((pressed & (kCircle | kTriangle)) != 0u && g_level == Level::Options) {
        enter(Level::Main, 2u);
        return kCircle | kTriangle;
    }
    return 0u;
}

// The awards screen of a car (0x08D7D430 as the current screen, the car's
// number at 0x08D89448): for a Collector car given by Signature n, the
// first CROSS brings Signature n's photo over the car (a slide from the
// right, turning straight), the second one lets the game go on.
constexpr std::uint32_t kCurrentScreen = 0x08D77BE0u, kCarAwardScreen = 0x08D7D430u, kAwardCar = 0x08D89448u;
// Signature n's photo in Frontend.txd (the Takedown Book's place order:
// <track>-st1 is the first of its track's places).
constexpr const char *kSignaturePhotos[20]{
    "silverlake-ST1", "silverlake-ST2", "downtown-st1", "downtown-st2", "waterfront-st2",
    "wintercity-st1", "wintercity-st2", "alpine-st2",  "riviera-st1",  "vineyard-st1",
    "vineyard-st2",   "goldencity-st1", "goldencity-st2", "dockside-st2", "islandparadise-st1",
    "islandparadise-st2", "airport-st1", "airport-st2", "harbourtown-st1", "palmbay-st1"};
std::string g_photo_dir;

std::string exe_dir() {
    const std::size_t slash = g_photo_dir.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : g_photo_dir.substr(0, slash + 1u);
}

} // namespace
namespace {
std::string signature_photo_path(int signature) {
    return std::string("signatures/") + kSignaturePhotos[std::clamp(signature, 0, 19)] + ".png";
}
bool file_exists(const std::string &relative) {
    if (std::FILE *f = std::fopen((exe_dir() + relative).c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}
std::function<void(const std::string &, float, float, float, float, float)> g_draw_photo;
int g_award_signature = -1;   // the Signature whose car the awards screen shows, -1: none
int g_photo_frames = -1;      // frames since its photo came, -1: not yet
std::uint32_t g_award_held{};  // buttons held, for presses

// The award's cards gone (the screen left).
void award_cards_off() {
    g_cards.clear();
    if (g_hooks.hide_3d) g_hooks.hide_3d(false);
    if (g_hooks.picture)
        for (int slot = 0; slot < kPictureSlots; ++slot) g_hooks.picture(slot, std::string(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}

// The game's postcard award (screen 0x08D7D444, award type 4, index 0-9 =
// SE01-SE10): after 4 s "NEW FILTER UNLOCKED <name>" comes over the card,
// then the card through that filter (drawn over the game's, same place:
// centre 461,268, 460x228, turned -15 degrees).
int g_postcard_award = -1, g_postcard_frames = 0;
constexpr int kPostcardText = 240, kPostcardSwap = 300, kPostcardDone = 330;
const char *new_filter_words() {
    switch (language()) {
    case 2: return "NOUVEAU FILTRE DEBLOQUE";
    case 3: return "NUEVO FILTRO DESBLOQUEADO";
    case 4: return "NEUER FILTER FREIGESCHALTET";
    case 5: return "NUOVO FILTRO SBLOCCATO";
    case 6: return "NIEUW FILTER VRIJGESPEELD";
    default: return "NEW FILTER UNLOCKED";
    }
}
void postcard_award_update() {
    int postcard = -1;
    if (read32(kCurrentScreen) == 0x08D7D444u && read32(0x08D8944Cu) == 4u && read32(0x08D89448u) < 10u)
        postcard = static_cast<int>(read32(0x08D89448u));
    if (postcard != g_postcard_award) {
        if (g_postcard_award >= 0 && g_hooks.picture) g_hooks.picture(0, std::string(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        g_award_text = {};
        g_postcard_award = postcard;
        g_postcard_frames = 0;
        if (postcard >= 0) std::fprintf(stderr, "[awards] postcard %d: its filter comes\n", postcard);
    }
    if (postcard < 0) return;
    ++g_postcard_frames;
    if (g_postcard_frames == kPostcardText) {
        g_award_text.line1 = new_filter_words();
        g_award_text.line2 = name(bonus::postcard_filter(postcard));
    }
    if (g_postcard_frames >= kPostcardText) g_award_text.alpha = std::min(1.0f, static_cast<float>(g_postcard_frames - kPostcardText) / 20.0f);
    if (g_postcard_frames == kPostcardSwap && g_hooks.picture)
        g_hooks.picture(0, "bonus/postcard_" + std::to_string(postcard + 1) + "_filtre.png", 461.5f, 268.5f, 460.0f, 228.0f, -0.2618f);
}

// Our own award windows (2026-11-11, the user: the game has none for them):
// a Crash Headline (its newspaper and the Heavyweight it gives) and a
// Takedown trophy (the Polaroid of its colour). Seen as the game's progress
// counts go up after a race; shown one after the other over the game's
// counts screen (0x08D7D440, after every race), on a clean copy of the
// award screen, until CROSS.
struct ExtraAward {
    int kind;  // 0 a Crash Headline, 1 a trophy, 2 a Signature Takedown (tests)
    int item;
};
std::vector<ExtraAward> g_extra;   // waiting
int g_extra_frames = -1;           // frames the first one has been shown, -1: none
int g_seen_headlines = -1, g_seen_trophies = -1;
bool g_raced{};                    // a race since the start (counts loaded)
constexpr std::uint32_t kCountsScreen = 0x08D7D440u;

void queue_all_extra_awards() {
    // (The Signatures: the game's own screens, 2026-11-11 the user's choice;
    // kind 2 stays for tests.)
    for (int i = 0; i < 11; ++i) g_extra.push_back({0, i});
    for (int i = 0; i < 5; ++i) g_extra.push_back({1, i});
}

void extra_awards_track() {
    const std::uint32_t screen = read32(kCurrentScreen);
    if (screen == 0u && readf(kCarSpeed) > 5.0f) g_raced = true;
    int headlines = 0;  // bit i: Crash Headline i won
    for (int i = 0; i < 10; ++i)
        if (headline_won(i)) headlines |= 1 << i;
    int trophies = 0;
    for (int t = 0; t < 5; ++t)
        if (trophy_won(t)) trophies |= 1 << t;
    // Before a race (the profile loading): only what is there already.
    if (!g_raced || g_seen_headlines < 0) {
        g_seen_headlines = headlines, g_seen_trophies = trophies;
        return;
    }
    const auto queue = [](int kind, int item) {
        for (const ExtraAward &a : g_extra)
            if (a.kind == kind && a.item == item) return;
        g_extra.push_back({kind, item});
    };
    for (int i = 0; i < 10; ++i)
        if ((headlines & ~g_seen_headlines & (1 << i)) != 0) {
            queue(0, i);
            std::fprintf(stderr, "[awards] Crash Headline %d won\n", i);
        }
    if (headlines == 0x3FF && g_seen_headlines != 0x3FF) queue(0, 10);  // and ALL HEADLINES
    for (int t = 0; t < 5; ++t)
        if ((trophies & ~g_seen_trophies & (1 << t)) != 0) {
            queue(1, t);
            std::fprintf(stderr, "[awards] trophy %d won\n", t);
        }
    g_seen_headlines |= headlines;
    g_seen_trophies |= trophies;
}

void extra_awards_update() {
    extra_awards_track();
    const bool show = !g_extra.empty() && read32(kCurrentScreen) == kCountsScreen;
    if (!show) {
        if (g_extra_frames >= 0) {
            award_cards_off();
            g_award_text = {};
            g_extra_frames = -1;
        }
        return;
    }
    if (g_extra_frames < 0) {
        if (g_hooks.hide_3d) g_hooks.hide_3d(true);
        std::fprintf(stderr, "[awards] our window: %s %d\n", (g_extra[0].kind == 1 ? "trophy" : g_extra[0].kind == 2 ? "signature" : "headline"), g_extra[0].item);
    }
    const int f = ++g_extra_frames;
    const ExtraAward award = g_extra[0];
    const auto slide = [](int frame, int from) {
        const float t = std::clamp(static_cast<float>(frame - from) / 18.0f, 0.0f, 1.0f);
        return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    };
    g_cards.clear();
    std::string title, footer;
    if (award.kind == 2) {
        const std::string n = std::to_string(award.item + 1);
        const float a = slide(f, 0), b = slide(f, 10);
        Card photo;
        photo.polaroid = true;
        photo.cx = -200.0f + (300.0f + 200.0f) * a, photo.cy = 260.0f, photo.angle = -0.35f + (-0.03f + 0.35f) * a;
        photo.pictures.push_back({signature_photo_path(award.item), 0.0f, 0.0f, 270.0f, 270.0f});
        photo.title = one_line(game_string(kSignatureStrings[award.item]), "?");
        photo.caption_dy = 94.0f, photo.caption_w = 205.0f;
        g_cards.push_back(photo);
        if (b > 0.0f) {
            Card car;
            car.polaroid = true;
            car.cx = 1180.0f + (652.0f - 1180.0f) * b, car.cy = 272.0f, car.angle = 0.4f + (0.03f - 0.4f) * b;
            car.pictures.push_back({"bonus/collector_" + n + ".png", 0.0f, 0.0f, 330.0f, 281.0f});
            car.title = one_line(game_string(kSignatureCars[award.item]), "?");
            car.caption_dy = 105.0f, car.caption_w = 270.0f;
            g_cards.push_back(car);
        }
        title = one_line(game_string(kStringSignatures), "SIGNATURE TAKEDOWNS");
        footer = photo.title;
    } else if (award.kind == 0) {
        const std::string n = std::to_string(award.item + 1);
        const float a = slide(f, 0), b = slide(f, 10);
        Card paper;
        paper.polaroid = true;
        paper.cx = -200.0f + (295.0f + 200.0f) * a, paper.cy = 266.0f, paper.angle = -0.35f + (-0.03f + 0.35f) * a;
        paper.pictures.push_back({"bonus/headline_" + n + ".png", 0.0f, 0.0f, 290.0f, 290.0f});
        g_cards.push_back(paper);
        if (b > 0.0f) {
            Card car;
            car.polaroid = true;
            car.cx = 1180.0f + (657.0f - 1180.0f) * b, car.cy = 272.0f, car.angle = 0.4f + (0.03f - 0.4f) * b;
            car.pictures.push_back({"bonus/heavy_" + n + ".png", 0.0f, 0.0f, 330.0f, 281.0f});
            const int vehicle = heavy_by_rank(award.item);
            car.title = vehicle >= 0 ? one_line(game_string(read32(kCarTable + static_cast<std::uint32_t>(vehicle) * 40u + 28u)), "?") : "?";
            car.caption_dy = 105.0f, car.caption_w = 270.0f;
            g_cards.push_back(car);
        }
        title = one_line(game_string(kStringHeadlines), "CRASH HEADLINES");
        footer = award.item >= 10 ? one_line(game_string(370u), "ALL HEADLINES") : one_line(game_string(kHeadlineTitles[award.item]), "HEADLINE");
    } else {
        const float a = slide(f, 0);
        const std::string colour = std::to_string(paints::kBaseColours + 1 + award.item);
        Card c;
        c.polaroid = true;
        c.cx = 480.0f, c.cy = -200.0f + (268.0f + 200.0f) * a, c.angle = 0.3f + (0.03f - 0.3f) * a;
        c.pictures.push_back({"bonus/trophy_polaroid_" + colour + ".png", 0.0f, 0.0f, 400.0f, 341.0f});
        c.title = one_line(game_string(kStringColour), "COLOUR") + " " + colour_word(award.item);
        c.caption_dy = 128.0f, c.caption_w = 330.0f;
        g_cards.push_back(c);
        title = one_line(game_string(kStringTrophies), "TROPHIES");
        footer = trophy_name(award.item);
    }
    g_award_text = {};
    g_award_text.title = title;
    g_award_text.footer = footer;
    g_award_text.footer_alpha = std::min(1.0f, static_cast<float>(f) / 15.0f);
    if (g_hooks.picture) {
        g_hooks.picture(0, "bonus/award_screen.png", 480.0f, 272.0f, 960.0f, 544.0f, 0.0f);
        for (int slot = 1; slot < kPictureSlots; ++slot) {
            if (slot - 1 < static_cast<int>(g_cards.size())) {
                const Card &c = g_cards[static_cast<std::size_t>(slot - 1)];
                const CardPicture &pic = c.pictures[0];
                g_hooks.picture(slot, pic.path, c.cx, c.cy, pic.w, pic.h, c.angle);
            } else {
                g_hooks.picture(slot, std::string(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            }
        }
    }
}

void award_photo_update() {
    extra_awards_update();
    postcard_award_update();
    int signature = -1;
    if (read32(kCurrentScreen) == kCarAwardScreen) {
        const std::uint32_t car = read32(kAwardCar);
        for (int i = 0; i < 20 && signature < 0; ++i)
            if (collector_car(i) == car + 1u) signature = i;
    }
    // Or the game's own Signature award (picture screen, award type 2, index
    // 0-19; 2026-11-11, the Vita: shown as the game's when its car was
    // already owned): the same two pictures.
    if (signature < 0 && read32(kCurrentScreen) == 0x08D7D444u && read32(0x08D8944Cu) == 2u && read32(0x08D89448u) < 20u)
        signature = static_cast<int>(read32(0x08D89448u));
    // (2026-11-11, the user, after seeing both: the game's own screens again,
    // its Signature photo then the Collector car turning, whose line says the
    // Signature gave it (label_collector_cars). Our two pictures are kept for
    // BONUS 2.0 and the test windows.)
    constexpr bool kOurSignatureScreens = false;
    if (!kOurSignatureScreens) {
        static int logged = -1;
        if (signature >= 0 && signature != logged)
            std::fprintf(stderr, "[awards] Signature %d: the game's own screen (%s)\n", signature,
                         read32(kCurrentScreen) == kCarAwardScreen ? "its Collector car" : "its photo");
        logged = signature;
        signature = -1;  // (the pictures off, g_photo_frames -1: nothing drawn)
    }
    if (signature != g_award_signature) {
        if (g_photo_frames >= 0) award_cards_off();
        g_award_signature = signature;
        g_photo_frames = -1;
        if (signature >= 0) std::fprintf(stderr, "[awards] Signature %d's car shown\n", signature);
        // (2026-11-11, the user: no turning car behind the two pictures)
        // The cards come at once over the screen's own empty background.
        if (signature >= 0) {
            g_photo_frames = 0;
            if (g_hooks.hide_3d) g_hooks.hide_3d(true);
        }
    }
    if (g_photo_frames < 0) return;
    // As BONUS 2.0's Signature page (2026-11-11): the Takedown's photo comes
    // from the left, then the Collector car's Polaroid from the right, over
    // the turning car, each with its name written on it.
    const auto slide = [](int frame, int from) {
        const float t = std::clamp(static_cast<float>(frame - from) / 18.0f, 0.0f, 1.0f);
        return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    };
    const float a = slide(g_photo_frames, 0), b = slide(g_photo_frames, 10);
    const std::string n = std::to_string(g_award_signature + 1);
    g_cards.clear();
    Card photo;
    photo.polaroid = true;
    photo.cx = -200.0f + (300.0f + 200.0f) * a, photo.cy = 260.0f, photo.angle = -0.35f + (-0.03f + 0.35f) * a;
    photo.pictures.push_back({signature_photo_path(g_award_signature), 0.0f, 0.0f, 270.0f, 270.0f});
    photo.title = one_line(game_string(kSignatureStrings[g_award_signature]), "?");
    photo.caption_dy = 94.0f, photo.caption_w = 205.0f;
    g_cards.push_back(photo);
    if (b > 0.0f) {
        Card car;
        car.polaroid = true;
        car.cx = 1180.0f + (652.0f - 1180.0f) * b, car.cy = 272.0f, car.angle = 0.4f + (0.03f - 0.4f) * b;
        car.pictures.push_back({"bonus/collector_" + n + ".png", 0.0f, 0.0f, 330.0f, 281.0f});
        car.title = one_line(game_string(kSignatureCars[g_award_signature]), "?");
        car.caption_dy = 105.0f, car.caption_w = 270.0f;
        g_cards.push_back(car);
    }
    if (g_hooks.picture) {
        // Slot 0: the screen's background without the car (and its
        // CONGRATULATIONS), between the title bar and the bottom bar.
        g_hooks.picture(0, "bonus/award_backdrop_long.png", 480.0f, 272.0f, 960.0f, 374.0f, 0.0f);
        for (int slot = 1; slot < kPictureSlots; ++slot) {
            if (slot - 1 < static_cast<int>(g_cards.size())) {
                const Card &c = g_cards[static_cast<std::size_t>(slot - 1)];
                const CardPicture &pic = c.pictures[0];
                g_hooks.picture(slot, pic.path, c.cx, c.cy, pic.w, pic.h, c.angle);
            } else {
                g_hooks.picture(slot, std::string(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            }
        }
    }
    ++g_photo_frames;
}

// The first CROSS on such a screen: taken from the game, the photo comes.
std::uint32_t award_photo_buttons(std::uint32_t buttons) {
    const std::uint32_t pressed = buttons & ~g_award_held;
    g_award_held = buttons;
    // (2026-11-11, the user: CROSS must always do something.) While the
    // pictures come, CROSS puts them in place at once; then it goes on.
    if (g_award_signature >= 0 && g_photo_frames >= 0 && g_photo_frames < 30 && (pressed & kCross) != 0u) {
        g_photo_frames = 30;
        return kCross;
    }
    // Our windows: every CROSS is ours; the pictures in place, then the next.
    if (g_extra_frames >= 0) {
        if ((pressed & kCross) != 0u && g_extra_frames < 30) {
            g_extra_frames = 30;
        } else if ((pressed & kCross) != 0u && !g_extra.empty()) {
            g_extra.erase(g_extra.begin());
            if (!g_extra.empty()) {
                g_extra_frames = 0;
                std::fprintf(stderr, "[awards] our window: %s %d\n", (g_extra[0].kind == 1 ? "trophy" : g_extra[0].kind == 2 ? "signature" : "headline"), g_extra[0].item);
            }
        }
        return kCross;
    }
    // (On a postcard, once its filter is shown.)
    // A CROSS before: the words at once, then the card through its filter.
    if (g_postcard_award >= 0 && g_postcard_frames < kPostcardDone && (pressed & kCross) != 0u) {
        if (g_postcard_frames < kPostcardText) g_postcard_frames = kPostcardText - 1;
        else if (g_postcard_frames < kPostcardSwap) g_postcard_frames = kPostcardSwap - 1;
        else g_postcard_frames = kPostcardDone;
        return kCross;
    }
    return 0u;
}

std::uint32_t g_swallowed{};  // taken from the game until released

} // namespace

void set_signature_photos(const std::string &dir,
                          std::function<void(const std::string &, float, float, float, float, float)> draw) {
    g_photo_dir = dir;
    g_draw_photo = std::move(draw);
}

// The game reads the pad several times a frame: a button the menu took stays
// hidden from the game until it is released.
bool take_guest_call(std::uint32_t &function, std::uint32_t &a0, std::uint32_t &a1) {
    if (!g_call_pending) return false;
    function = g_call.function;
    a0 = g_call.a0;
    a1 = g_call.a1;
    g_call_pending = g_call_next_pending;
    g_call = g_call_next;
    g_call_next_pending = false;
    std::fprintf(stderr, "[garage] game call %08X(%08X, %08X)\n", function, a0, a1);
    return true;
}

std::uint32_t filter_buttons(std::uint32_t buttons) {
    // The garage opening or closing: nothing reaches the game meanwhile.
    if (g_call_pending) return 0u;
    if (g_memory != nullptr) garage20_arrows(buttons);
    g_swallowed &= buttons;
    g_swallowed |= award_photo_buttons(buttons) & buttons;
    g_swallowed |= consume(buttons) & buttons;
    return buttons & ~g_swallowed;
}

void signature_recorded(psprecomp::GuestMemory &memory, std::uint32_t profile, int signature) {
    g_memory = &memory;
    if (!g_hooks.enabled || !g_hooks.filter_count) return;
    give_collector_car(profile, signature);
}

void set_all_cars(bool all) { g_all_cars = all; }

void set_everything_won(bool everything) {
    g_everything_won = everything;
    paints::set_all_unlocked(everything);
    // (The Special Events' filters are unlocked for real in check_signatures,
    // once the save is loaded: the state may not be yet.)
    std::fprintf(stderr, "[test] everything shown as won: %s\n", everything ? "yes" : "no");
}

void force_all_rewards(std::function<void()> fired) {
    g_force_all = true;
    g_force_all_fired = std::move(fired);
    std::fprintf(stderr, "[test] every reward armed: at the next race\n");
}

int new_awards() { return g_hooks.enabled && g_hooks.enabled() ? g_new_awards : 0; }

// The main menu being built (patch_main_menu.py: the game asks for its row
// count): five, with BONUS as the fifth row, when the Bonus menu is on. The
// fifth row's place holds the first two descriptions, just written: they are
// kept (and the screen gets our copy of them in patch_main).
std::uint32_t heavy_class(psprecomp::GuestMemory &memory, std::uint32_t heavy, std::uint32_t special, std::uint32_t mode) {
    g_memory = &memory;
    const bool ours = heavy == 0u && mode == kSingleEvent && g_hooks.enabled && g_hooks.enabled();
    g_heavy_offered = ours;
    std::fprintf(stderr, "[cars] car select: mode %u, Race Special %u, Heavyweights %u%s\n", mode, special, heavy,
                 ours ? " (offered by the Bonus)" : "");
    offer_heavy_vehicles();
    return ours ? 1u : heavy;
}

std::uint32_t main_menu_rows(psprecomp::GuestMemory &memory, std::uint32_t menu) {
    g_memory = &memory;
    g_main_built = false;
    if (!g_hooks.enabled || !g_hooks.filter_count || !g_hooks.enabled() || menu + 16u != kMainList) return kMainRows;
    for (std::uint32_t i = 0; i < kMainRows; ++i)
        if (!guest_pointer(read32(kMainList + 4u + i * 8u)) || !guest_pointer(read32(kMainDescriptions + i * 4u))) return kMainRows;
    if (!find_strings(read32(kMainList + 4u))) return kMainRows;
    // (Until 0.0.55 a fifth row was BONUS; BONUS LEGENDS 2.0 now takes
    // MULTIPLAYER's row in patch_main.) Since 2026-11-11 the menu is built
    // with a fifth row again, empty and without a bar in the main menu, so
    // that BONUS 2.0's lists show five rows (the user's mock-up).
    for (std::uint32_t i = 0; i < kMainRows; ++i) g_descriptions[i] = read32(kMainDescriptions + i * 4u);
    write_text(kEmptyText, " ");
    write32(kMainList + 4u + kMainRows * 8u, kEmptyText);
    write32(kMainList + 8u + kMainRows * 8u, 0u);
    g_main_built = true;
    return kMainRows + 1u;
}

void set_picture_ready(std::function<bool(int)> ready) { g_picture_ready = std::move(ready); }
void set_heavy_test_all(bool on) { g_heavy_test_all = on; }
} // namespace burnout::bonus_menu

// The game's notice of new unlocks (0x089280D4, patch_awards.py): its
// "NEW AWARD(S) UNLOCKED" line also counts the filters unlocked since the
// Bonus rows were last opened.
std::uint32_t burnout_new_awards(psprecomp::GuestMemory &) {
    return static_cast<std::uint32_t>(burnout::bonus_menu::new_awards());
}

// The game records Signature Takedown n in the profile (0x08817050, at the
// end of a race; patch_signatures.py): its Collector car is given first, so
// the game's results see it as new.
void burnout_signature_done(psprecomp::GuestMemory &memory, std::uint32_t profile, std::uint32_t signature) {
    burnout::bonus_menu::signature_recorded(memory, profile, static_cast<int>(signature));
}

std::uint32_t burnout_main_menu_rows(psprecomp::GuestMemory &memory, std::uint32_t menu) {
    return burnout::bonus_menu::main_menu_rows(memory, menu);
}

// Car select opens (tools/patch_cars.py): whether the Heavyweight class is
// offered (the mode's own flag `heavy`, Race Special `special`, the mode's
// word `mode`).
std::uint32_t burnout_heavy_class(psprecomp::GuestMemory &memory, std::uint32_t heavy, std::uint32_t special, std::uint32_t mode) {
    return burnout::bonus_menu::heavy_class(memory, heavy, special, mode);
}

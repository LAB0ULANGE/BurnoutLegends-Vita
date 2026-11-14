#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace psprecomp {
class GuestMemory;
}

// The Bonus filters in the game's own pause menu, drawn by the game:
//   PAUSED:  RESUME  RESTART  OPTIONS  QUIT
//   OPTIONS: AUDIO   CONTROLS BONUS  BONUS VITA: ON/OFF  DISPLAY: HD/PSP
//            (BONUS only while Bonus Vita is on; the last two always)
//   Main menu: a fifth row BONUS
//   BONUS:   the filters (scrolling), the one under the cursor on a postcard
// (the game's lists and their row items rearranged; see bonus_menu.cpp).
namespace burnout::bonus_menu {

// Test builds: every Heavyweight drivable in Single Event (no save change).
void set_heavy_test_all(bool on);

struct Hooks {
    std::function<bool()> enabled;                       // Bonus switched on (else the menu is left alone)
    std::function<int()> filter_count;                   // filters::all().size()
    std::function<const char *(int)> filter_name;        // shown name
    std::function<bool(int)> available;                  // unlocked
    std::function<bool(int)> active;                     // switched on
    std::function<void(int)> toggle;                     // switch on/off (applies and saves)
    std::function<std::string(int)> locked_label;        // a locked filter's row ("LOCKED: GONE FISHIN'")
    // Signature Takedown n is in the player's profile: true when that
    // unlocked something new (unlocks.bin saved by the hook).
    std::function<bool(int)> signature_done;
    // The reward that gives a filter ("GONE FISHIN'"), empty if none.
    std::function<std::string(int)> source;
    // The postcard: its photo through this filter drawn in the box (centre
    // x, y from the top, width, height of the 960x544 screen, angle in
    // radians); -1: none.
    std::function<void(int, float, float, float, float, float)> postcard;
    // Take the postcard's photo (the race beside the menu) at the next frame.
    std::function<void()> photo;
    // What unlocks a filter: kind * 100 + its 0-based place (kind 1: the
    // n-th Special Event postcard, 2: the n-th Crash Headline, 3: 100 %),
    // -1: nothing (always available).
    std::function<int(int)> signature;
    // The player's car speed (mph) read from the game, once per vblank.
    std::function<void(float)> car_speed;
    // The game's progress, twice a second: Special Events (postcards), Crash
    // Headlines, % complete; the number of filters it unlocked.
    std::function<int(int, int, int)> progress;
    // The two settings in PAUSED > OPTIONS: Bonus Vita (off: the original
    // game) and the picture (PSP: 480x272 enlarged; else HD 960x544).
    std::function<void(bool)> set_enabled;
    std::function<bool()> psp_picture;
    std::function<void(bool)> set_psp_picture;
    // Once per vblank: whether one of the game's menus is up (the filters
    // are left out of the picture then).
    std::function<void(bool)> menu_up;
    // A picture slot of the BONUS 2.0 cards (0-3): a file beside the exe
    // ("postcards/sepia.jpg"), centre x, y, width, height of the 960x544
    // screen, angle (radians); path empty: none.
    std::function<void(int, const std::string &, float, float, float, float, float)> picture;
    // The game's 3D draws left out while true (gl::set_hide_3d).
    std::function<void(bool)> hide_3d;
};

void configure(Hooks hooks);

// The Signature Takedown photos (<dir>/<name>.png, from Frontend.txd) and
// how to draw one over the screen (gl::set_photo; path empty: none): the
// awards screen of a Collector car a Signature gave shows its photo at the
// first CROSS, the second one goes on.
void set_signature_photos(const std::string &dir,
                          std::function<void(const std::string &, float, float, float, float, float)> draw);

// Once per vblank on the emulation thread.
void update(psprecomp::GuestMemory &memory);

// The pad as the game will read it: the buttons the Bonus menu consumes are
// taken out.
std::uint32_t filter_buttons(std::uint32_t buttons);
// A game function to run at the game's next controller read (the 2.0
// garage's opening and closing): false when there is none.
bool take_guest_call(std::uint32_t &function, std::uint32_t &a0, std::uint32_t &a1);

// Filters unlocked since the Bonus rows were last opened (the game's notice of
// new awards counts them, tools/patch_awards.py).
int new_awards();
// PC settings (all_cars): every car's "unlocked" flag set in the profile.
void set_all_cars(bool all);
// Tests: in the next race (the car over 30 mph for 3 s), every World Tour
// event gold and every Signature Takedown done in the live profile, so the
// race's end shows every award they give; `fired` runs just before (once).
void force_all_rewards(std::function<void()> fired);
// Tests: BONUS 2.0 shows every trophy, Crash Headline and filter as won and
// every car colour is offered (the game's own progress left as it is).
void set_everything_won(bool everything);

// The game records Signature Takedown n in the profile at `profile`
// (tools/patch_signatures.py): its Collector car is given then.
void signature_recorded(psprecomp::GuestMemory &memory, std::uint32_t profile, int signature);

// Car select opens: the Heavyweight class offered (1) or not (0), from the
// mode's flags (tools/patch_cars.py).
std::uint32_t heavy_class(psprecomp::GuestMemory &memory, std::uint32_t heavy, std::uint32_t special, std::uint32_t mode);

// Whether picture slot n is on screen yet (captions wait for it); unset: always.
void set_picture_ready(std::function<bool(int)> ready);
} // namespace burnout::bonus_menu

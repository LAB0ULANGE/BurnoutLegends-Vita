#pragma once

#include <ctime>
#include <string>
#include <vector>

// The game's save list dialogs (sceUtilitySavedata LISTLOAD / LISTSAVE)
// shown with the Vita's own save data dialog. Slot i of the dialog is entry
// i of the game's list; the Vita only keeps each slot's title and date, the
// data stays in the PSP save folders. Elsewhere (PC test tool) nothing is
// shown and the HLE picks an entry itself.
namespace burnout::save_dialog {

struct Slot {
    bool exists{};
    bool psp{};  // written by a PSP or Adrenaline (icon marked PSP, else PS VITA)
    std::string title, subtitle, detail;
    std::time_t modified{};  // UTC
};

constexpr int kRunning = -2, kCancelled = -1;

// Opens the dialog (save: a slot to write, else one to load) focused on
// `focus`. False when there is no dialog here or it could not open.
bool start(bool save, const std::vector<Slot> &slots, int focus);
// kRunning until the player has chosen, then the slot (or kCancelled) once,
// after the dialog has closed.
int poll();
// The display swaps must let the dialog draw itself while this is true.
bool drawing();

// The console's language as a PSP language code (0 Japanese, 1 English,
// 2 French, 3 Spanish, 4 German, 5 Italian, 6 Dutch, 7 Portuguese, 8 Russian,
// 9 Korean, 10/11 Chinese); English elsewhere.
int system_language();

} // namespace burnout::save_dialog

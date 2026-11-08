#pragma once

#include <functional>
#include <string>
#include <vector>

// First launch: the game files are made from the player's own disc image
// (the ISO or CSO they already use with Adrenaline), so nothing has to be
// prepared on a PC. From the image: PARAM.SFO and USRDIR copied, EBOOT.BIN
// decrypted (PPSSPP's PRX decrypter) and checked, the ATRAC3plus music
// converted to the IMA ADPCM tracks music.cpp plays.
namespace burnout::setup {

enum class Result {
    Ready,        // already installed, or installed now
    NoImage,      // no Burnout Legends disc image found
    WrongVersion, // a Burnout Legends image, but not ULES00125 v1.01
    Failed,       // copy, decryption or conversion error (message says which)
};

struct Outcome {
    Result result{Result::Failed};
    std::string message;  // what went wrong (English)
    std::string image;    // the disc image used or found
};

// Step name ("Copying game files", ...) and progress of that step, 0..1.
using Progress = std::function<void(const std::string &step, const std::string &detail, double fraction)>;

// `root` ends with '/': ux0:data/burnout/. True when every file the game
// needs is there and no install was left half done.
bool installed(const std::string &root);

// Disc images (.iso / .cso) in these folders and one level of subfolders.
std::vector<std::string> find_images(const std::vector<std::string> &folders);

Outcome install(const std::string &root, const std::vector<std::string> &folders, const Progress &progress);
// Installs from one given image (PC tests).
Outcome install_from(const std::string &root, const std::string &image, const Progress &progress);

// One .mus (RIFF ATRAC3plus) to the recomp's .ima; false on error.
bool convert_music(const std::string &mus, const std::string &ima);

} // namespace burnout::setup

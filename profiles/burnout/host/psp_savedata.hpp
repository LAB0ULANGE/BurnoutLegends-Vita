#pragma once
// PSP save data format (SPDX-License-Identifier: GPL-2.0-or-later): the
// encryption the PSP firmware applies to a game's save files and the hashes
// it keeps in PARAM.SFO, so that saves are shared with a real PSP or
// Adrenaline. Ported from PPSSPP's sceChnnlsv / SavedataParam (GPL 2.0 or
// later); this file and psp_savedata.cpp are GPL, unlike the MIT recompiler.
//
// Only hash mode 1 is supported: the one of games built with the first PSP
// SDKs (no game key, no console-bound key), which Burnout Legends uses.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace burnout::psp_savedata {

using Hash = std::array<std::uint8_t, 16>;

// A saved file as stored: 16-byte seed then the data, encrypted. Returns the
// plain data and the file's hash (kept in PARAM.SFO's file list).
bool decrypt(const std::vector<std::uint8_t> &stored, std::vector<std::uint8_t> &plain, Hash &hash);
// The stored form of `plain` (a fresh random seed) and its hash.
std::vector<std::uint8_t> encrypt(const std::uint8_t *plain, std::size_t size, Hash &hash);

// PARAM.SFO of a save directory.
struct SfoInfo {
    std::string title, savedata_title, detail, directory;
    std::uint32_t parental_level{1};
};
// Values of an existing PARAM.SFO; false if `sfo` is not one.
bool read_sfo(const std::vector<std::uint8_t> &sfo, SfoInfo &info);
// The hash mode the save's PARAM.SFO says its files were written with (0 = plain).
int crypt_mode(const std::vector<std::uint8_t> &sfo);
// The hash recorded for `file` in PARAM.SFO's file list; false if none.
bool file_hash(const std::vector<std::uint8_t> &sfo, const std::string &file, Hash &hash);
// A PARAM.SFO for `info` whose file list holds `file` with `hash`, signed as
// the PSP signs it (hash mode 1). `previous` (may be empty) gives the other
// files' entries and the console-bound hash, kept as they were.
std::vector<std::uint8_t> build_sfo(const SfoInfo &info, const std::string &file, const Hash &hash,
                                    const std::vector<std::uint8_t> &previous);

} // namespace burnout::psp_savedata

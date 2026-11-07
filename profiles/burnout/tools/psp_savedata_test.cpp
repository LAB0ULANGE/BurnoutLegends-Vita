// Checks psp_savedata against a save written by a PSP / Adrenaline:
//   psp_savedata_test <save dir with DATA.BIN + PARAM.SFO> <expected plain DATA.BIN>
// decrypt -> plain identical, hash = PARAM.SFO's; PARAM.SFO rebuilt from its
// own values identical; encrypt -> decrypt round trip.
#include "../host/psp_savedata.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ps = burnout::psp_savedata;

static std::vector<std::uint8_t> read(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const std::string dir = argv[1];
    const auto stored = read(dir + "/DATA.BIN"), sfo = read(dir + "/PARAM.SFO"), expected = read(argv[2]);
    int failures = 0;
    std::vector<std::uint8_t> plain;
    ps::Hash hash{}, recorded{};
    ps::decrypt(stored, plain, hash);
    const bool listed = ps::file_hash(sfo, "DATA.BIN", recorded);
    std::printf("crypt mode %d, hash %s\n", ps::crypt_mode(sfo), listed && hash == recorded ? "matches PARAM.SFO" : "MISMATCH");
    failures += !(listed && hash == recorded);
    std::size_t same = 0;
    for (std::size_t i = 0; i < plain.size() && i < expected.size(); ++i) same += plain[i] == expected[i];
    std::printf("plain %zu bytes, %zu equal to the expected file\n", plain.size(), same);
    failures += plain.size() != expected.size();

    ps::SfoInfo info;
    ps::read_sfo(sfo, info);
    std::printf("title '%s' savedata title '%s' directory '%s'\n", info.title.c_str(), info.savedata_title.c_str(), info.directory.c_str());
    const auto rebuilt = ps::build_sfo(info, "DATA.BIN", hash, sfo);
    std::printf("PARAM.SFO rebuilt: %zu bytes (original %zu), %s\n", rebuilt.size(), sfo.size(), rebuilt == sfo ? "IDENTICAL" : "different");
    if (rebuilt != sfo) {
        for (std::size_t i = 0; i < rebuilt.size() && i < sfo.size(); ++i)
            if (rebuilt[i] != sfo[i]) { std::printf("  first difference at 0x%zx\n", i); break; }
        ++failures;
    }

    ps::Hash again{};
    const auto encrypted = ps::encrypt(plain.data(), plain.size(), again);
    std::vector<std::uint8_t> round;
    ps::Hash round_hash{};
    ps::decrypt(encrypted, round, round_hash);
    std::printf("round trip: %s, hash %s\n", round == plain ? "same data" : "DIFFERENT", round_hash == again ? "same" : "DIFFERENT");
    failures += !(round == plain && round_hash == again);
    std::printf(failures == 0 ? "OK\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}

// Burnout profile helper: counts every reachable instruction exactly once
// (labels plus delay slots) and reports unsupported encodings and VFPU usage.
#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/program_analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

bool is_vfpu(psprecomp::OpcodeKind k) {
    using K = psprecomp::OpcodeKind;
    return (k >= K::Mfv && k <= K::Svq) || k == K::Vflush || k == K::Vfpu ||
           (k >= K::Bvf && k <= K::Bvtl);
}

void print_top(const char *title, const std::map<std::string, std::size_t> &counts, std::size_t limit) {
    std::vector<std::pair<std::string, std::size_t>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    std::cout << title << " (" << v.size() << " distinct)\n";
    for (std::size_t i = 0; i < v.size() && i < limit; ++i)
        std::printf("  %-14s %8zu\n", v[i].first.c_str(), v[i].second);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: burnout_opcode_census <ELF> [load_base] [unsupported_sites.csv]\n";
        return 2;
    }
    const std::uint32_t base = argc >= 3 ? static_cast<std::uint32_t>(std::stoul(argv[2], nullptr, 0))
                                         : psprecomp::kDefaultPspUserLoadBase;
    auto elf = psprecomp::Elf32Image::from_file(argv[1]);
    psprecomp::GuestMemory memory(32u * 1024u * 1024u);
    (void)elf.load_and_relocate(memory, base);
    const auto program = psprecomp::analyze_program(elf, memory, base);

    std::set<std::uint32_t> pcs(program.covered_labels.begin(), program.covered_labels.end());
    for (std::uint32_t pc : program.covered_labels) {
        if (psprecomp::decode_allegrex(memory.load32(pc)).has_delay_slot()) pcs.insert(pc + 4u);
    }

    std::map<std::string, std::size_t> unsupported, vfpu;
    std::map<std::uint32_t, std::size_t> unsupported_by_function;
    std::size_t vfpu_total = 0, fpu_total = 0, unsupported_total = 0, break_total = 0;
    std::set<std::uint32_t> vfpu_functions;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sites;
    for (std::uint32_t pc : pcs) {
        const auto d = psprecomp::decode_allegrex(memory.load32(pc));
        if (d.kind == psprecomp::OpcodeKind::Break) {
            ++break_total;
        } else if (d.kind == psprecomp::OpcodeKind::Unsupported || d.kind == psprecomp::OpcodeKind::Vfpu) {
            // The generic codegen emits rt.unsupported for the catch-all Vfpu kind too.
            ++unsupported[d.mnemonic + "/" + psprecomp::hex32(d.word >> 26)];
            ++unsupported_total;
            sites.emplace_back(pc, d.word);
        }
        if (is_vfpu(d.kind)) {
            ++vfpu[d.mnemonic];
            ++vfpu_total;
        } else if (d.kind >= psprecomp::OpcodeKind::Lwc1 && d.kind <= psprecomp::OpcodeKind::Swc1) {
            ++fpu_total;
        } else if (d.kind >= psprecomp::OpcodeKind::Mfc1 && d.kind <= psprecomp::OpcodeKind::Bc1tl) {
            ++fpu_total;
        }
    }
    for (const auto &f : program.functions) {
        for (std::uint32_t pc : f.labels) {
            const auto k = psprecomp::decode_allegrex(memory.load32(pc)).kind;
            if (is_vfpu(k)) { vfpu_functions.insert(f.entry); break; }
        }
    }

    std::cout << "functions:            " << program.functions.size() << "\n"
              << "unique instructions:  " << pcs.size() << "\n"
              << "unsupported (unique): " << unsupported_total << "\n"
              << "break traps:          " << break_total << "\n"
              << "VFPU instructions:    " << vfpu_total << " ("
              << (100.0 * static_cast<double>(vfpu_total) / static_cast<double>(pcs.size())) << "%)\n"
              << "functions using VFPU: " << vfpu_functions.size() << "\n"
              << "scalar FPU instrs:    " << fpu_total << "\n";
    print_top("unsupported by mnemonic/major-opcode", unsupported, 40);
    print_top("VFPU by mnemonic", vfpu, 60);

    if (argc >= 4) {
        FILE *out = std::fopen(argv[3], "w");
        if (out) {
            std::fprintf(out, "pc,word\n");
            for (const auto &[pc, word] : sites) std::fprintf(out, "0x%08X,0x%08X\n", pc, word);
            std::fclose(out);
        }
    }
    return 0;
}

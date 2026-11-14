"""Burnout Legends: ranks the game functions of a Vita measuring build's
profile (ux0:data/<files>-profile.txt: "samples N", then "label count" per
generated block). A block belongs to the closest function start at or below
it (psp_report_functions_auto.csv). Labels that are import NIDs (time inside
an HLE call, waits included) and 0xEEEEEEEE (host work after an import) are
listed apart.

usage: python profile_report.py PROFILE.txt [FUNCTIONS.csv] [TOP]
"""
import bisect
import collections
import csv
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parents[3]


def main() -> int:
    profile = pathlib.Path(sys.argv[1])
    functions_csv = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "psp_report_functions_auto.csv"
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    starts = sorted(int(row["address"], 16) for row in csv.DictReader(functions_csv.open(encoding="utf-8")))
    lines = profile.read_text(encoding="utf-8").split("\n")
    total = int(lines[0].split()[1])
    by_function = collections.Counter()
    blocks = collections.defaultdict(collections.Counter)
    outside = collections.Counter()
    for line in lines[1:]:
        if not line.strip():
            continue
        label, count = line.split()
        address, count = int(label, 16), int(count)
        if 0x08804000 <= address < 0x08C00000:
            i = bisect.bisect_right(starts, address) - 1
            function = starts[i] if i >= 0 else address
            by_function[function] += count
            blocks[function][address] += count
        else:
            outside[address] += count
    game = sum(by_function.values())
    print(f"samples {total}: game code {100.0 * game / total:.1f}%, imports and host {100.0 * (total - game) / total:.1f}%")
    print("\nimports / host (label: share of all samples)")
    for address, count in outside.most_common(12):
        name = "host after an import" if address == 0xEEEEEEEE else f"NID {address:08X}"
        print(f"  {name:24s} {100.0 * count / total:5.1f}%")
    print(f"\ngame functions (share of all samples / of game code), top {top}")
    running = 0
    for function, count in by_function.most_common(top):
        running += count
        hot = ", ".join(f"{a:08X} {100.0 * c / count:.0f}%" for a, c in blocks[function].most_common(3))
        print(f"  {function:08X} {100.0 * count / total:5.1f}% {100.0 * count / game:5.1f}%"
              f"  (cumulative {100.0 * running / game:5.1f}%)  hottest blocks: {hot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

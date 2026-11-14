"""Burnout Legends: the five trophy liveries of the cars with 8 colours.

usage: python make_liveries.py PVEH_DIR OUT_DIR [--only CLASS/CarN.bgv] [--size 512]

PVEH_DIR  the game's USRDIR/pveh (read only)
OUT_DIR   gets  pveh/<class>/<car>      the car files whose colours 4-8 had to
                                        be made unique (a texture pack tells
                                        textures apart by their palette)
                textures/liveries/*.png the liveries
                textures/liveries.ini   their [hashes] lines (texture pack)

Colours 4 to 8 (palettes 3-7) are the trophies' (paints.cpp): Easy orange,
Advanced pink, Expert lime green, Champion silver, the four trophies gold
"Legend". Each livery is the car's own texture with its body (the palette
entries of its main hue) painted in that colour, its shading kept; decals,
glass, lights and wheels stay as they are. The game draws car textures at
about half brightness (as stored): the liveries are made the same way.
"""
import argparse
import colorsys
import os
import shutil
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pack_keys  # noqa: E402

KEY_BASE = 0xC500F70B  # dim ^ CLUT format of a 256x256 CLUT8 car texture (read in game)
# Colours 4-8, one per trophy: bright and unlike the cars' own colours
# (the user's choice, 2026-11-11). The body panels get a texel or two each
# (their UVs are near points): patterns cannot show there, colours can.
COLOURS = {
    "orange": (1.00, 0.42, 0.00),      # Easy
    "rose": (1.00, 0.30, 0.68),        # Advanced
    "vert_lime": (0.55, 1.00, 0.08),   # Expert
    "argent": (0.80, 0.82, 0.86),      # Champion
    "or": (1.00, 0.76, 0.18),          # the four trophies: Legend
}
THEMES = list(COLOURS)
STORED = 0.45  # the game's car palettes: stored at about half brightness


def body_of(pal, counts):
    """(hue, reference value, mask per entry) of the palette's body, or None."""
    rgb = pal[:, :3] / 255.0
    hsv = np.array([colorsys.rgb_to_hsv(*c) for c in rgb])
    h, s, v = hsv[:, 0], hsv[:, 1], hsv[:, 2]
    bins = np.zeros(36)
    ok = (s > 0.45) & (v > 0.08)
    np.add.at(bins, (h[ok] * 36).astype(int) % 36, counts[ok])
    weight = bins + 0.5 * (np.roll(bins, 1) + np.roll(bins, -1))
    best = int(weight.argmax())
    if weight[best] <= 0:
        return None, 0.0
    hue = (best + 0.5) / 36
    d = np.abs(h - hue)
    near = np.minimum(d, 1 - d) < 0.08
    entries = near & (s > 0.35)
    used = entries & (counts > 0)
    if not used.any():
        return None, 0.0
    order = np.argsort(v[used])
    vals, n = v[used][order], counts[used][order]
    reference = vals[np.searchsorted(np.cumsum(n), n.sum() * 0.9)]
    # Lights share the body's hue but are much brighter.
    mask = entries & ~((v / max(reference, 1e-3) > 1.3) & (s > 0.6))
    return (hue, reference, mask, v), weight[best]


def theme_colour(theme, size):
    """RGB in [0, 1] of the theme at every texel (size x size x 3): one colour."""
    out = np.zeros((size, size, 3), np.float32)
    out[:] = COLOURS[theme]
    return out


def make_unique(data, t, indices, taken):
    """Palettes 3-7 changed (one entry, one step) until their keys are all new."""
    counts = np.bincount(indices.ravel(), minlength=t.couleurs)
    # The entries changed by one step (invisible): unused ones first, else
    # the least used.
    unused = [int(i) for i in np.argsort(counts, kind="stable")[:16]]
    changed = False
    keys = pack_keys.car_keys(data, t, KEY_BASE)
    for p in range(3, 8):
        tries = 0
        while keys[p] in taken or keys[p] in keys[:p] or keys[p] in keys[p + 1:3]:
            if tries >= 64:
                raise RuntimeError(f"{t.nom}: colour {p + 1} cannot be told apart")
            entry = unused[tries % len(unused)]
            at = t.pos + t.pal_off + p * 4 * t.couleurs + entry * 4 + (tries // len(unused)) % 3
            data[at] = data[at] - 1 if data[at] > 0 else 1
            changed = True
            tries += 1
            keys = pack_keys.car_keys(data, t, KEY_BASE)
        taken.add(keys[p])
    return keys, changed


def livery(data, t, indices, theme, size):
    counts = np.bincount(indices.ravel(), minlength=t.couleurs)
    pals = [np.asarray(pack_keys_palette(data, t, p)).astype(np.float32) for p in range(3)]
    # The base colour whose body is the most coloured (a first colour may be black).
    best, best_w = None, -1.0
    for pal in pals:
        body, w = body_of(pal, counts)
        if body is not None and w > best_w * 1.25:
            best, best_w, source = body, w, pal
    if best is None:
        return None
    hue, reference, mask, value = best
    base = source[indices] / 255.0                                   # 256x256x4
    is_body = mask[indices].astype(np.float32)                       # 256x256
    shade = np.clip(value[indices] / max(reference, 1e-3), 0.0, 1.2).astype(np.float32)
    up = lambda a, mode: np.asarray(Image.fromarray(a).resize((size, size), mode))
    base_up = up((base * 255).astype(np.uint8), Image.LANCZOS).astype(np.float32) / 255.0
    body_up = up((is_body * 255).astype(np.uint8), Image.BILINEAR).astype(np.float32)[..., None] / 255.0
    shade_up = up((shade / 1.2 * 255).astype(np.uint8), Image.BILINEAR).astype(np.float32)[..., None] / 255.0 * 1.2
    paint = np.clip(theme_colour(theme, size) * STORED * shade_up, 0.0, 1.0)
    rgb = base_up[..., :3] * (1 - body_up) + paint * body_up
    out = np.concatenate([rgb, base_up[..., 3:4]], -1)
    return Image.fromarray((out * 255 + 0.5).astype(np.uint8), "RGBA")


def pack_keys_palette(data, t, p):
    at = t.pos + t.pal_off + p * 4 * t.couleurs
    return np.frombuffer(bytes(data[at:at + 4 * t.couleurs]), np.uint8).reshape(t.couleurs, 4)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("pveh")
    a.add_argument("out")
    a.add_argument("--only", action="append", help="CLASS/CarN.bgv (repeatable)")
    a.add_argument("--size", type=int, default=512)
    a.add_argument("--txd", default=r"F:/Burnout Legends/mods_pc/outils", help="folder of burnout_txd.py")
    args = a.parse_args()
    sys.path.insert(0, args.txd)
    import burnout_txd as txd

    cars = []
    for klass in sorted(os.listdir(args.pveh)):
        folder = os.path.join(args.pveh, klass)
        if not os.path.isdir(folder):
            continue
        for f in sorted(os.listdir(folder)):
            if f.lower().endswith((".bgv", ".btv")) and (not args.only or f"{klass}/{f}" in args.only):
                cars.append((klass, f))
    lines = ["# Burnout Legends 2.0: trophy liveries (colours 4-8), make_liveries.py"]
    taken = set()
    out_png = os.path.join(args.out, "textures", "liveries")
    os.makedirs(out_png, exist_ok=True)
    made = 0
    for klass, f in cars:
        data, t = txd.lire_voiture(os.path.join(args.pveh, klass, f))
        if t.palettes != 8 or t.bpp != 8 or (t.largeur, t.hauteur) != (256, 256):
            continue
        indices = txd._indices(data, t, 0)
        keys, changed = make_unique(data, t, indices, taken)
        for k in keys[:3]:
            taken.add(k)
        if changed:
            os.makedirs(os.path.join(args.out, "pveh", klass), exist_ok=True)
            with open(os.path.join(args.out, "pveh", klass, f), "wb") as out:
                out.write(data)
        name = f"{klass}_{os.path.splitext(f)[0]}".lower()
        lines.append(f"# {klass}/{f} ({t.nom})")
        for slot, theme in enumerate(THEMES):
            image = livery(data, t, indices, theme, args.size)
            if image is None:
                print(f"{klass}/{f}: no coloured body, left out")
                break
            png = f"{name}_{slot + 4}_{theme}.png"
            image.save(os.path.join(out_png, png))
            lines.append(f"{keys[slot + 3]} = liveries/{png}")
            made += 1
        print(f"{klass}/{f} {t.nom}: {'palettes made unique, ' if changed else ''}5 liveries")
    with open(os.path.join(args.out, "textures", "liveries.ini"), "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(lines) + "\n")
    print(f"{made} liveries -> {args.out}")


if __name__ == "__main__":
    main()

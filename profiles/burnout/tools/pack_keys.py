"""Texture pack keys (PPSSPP's, as texture_pack.cpp computes them) for the
car liveries, from the game's car files (pveh/<class>/CarN.bgv).

key = 00000000 + (dim ^ clut format ^ xxh32(palette)) + quick hash(texels)
dim and the CLUT format word are the same for every 256x256 8-bit car
texture: kCarKeyBase was read from the game (PSPRECOMP_BURNOUT_PACK_KEYS).
"""
import struct

M32 = 0xFFFFFFFF


def _rotl(x, r):
    return ((x << r) | (x >> (32 - r))) & M32


def xxh32(data, seed):
    P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
    n = len(data)
    i = 0
    if n >= 16:
        v1, v2, v3, v4 = (seed + P1 + P2) & M32, (seed + P2) & M32, seed, (seed - P1) & M32
        while i <= n - 16:
            a, b, c, d = struct.unpack_from("<4I", data, i)
            v1 = _rotl((v1 + a * P2) & M32, 13) * P1 & M32
            v2 = _rotl((v2 + b * P2) & M32, 13) * P1 & M32
            v3 = _rotl((v3 + c * P2) & M32, 13) * P1 & M32
            v4 = _rotl((v4 + d * P2) & M32, 13) * P1 & M32
            i += 16
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & M32
    else:
        h = (seed + P5) & M32
    h = (h + n) & M32
    while i + 4 <= n:
        h = _rotl((h + struct.unpack_from("<I", data, i)[0] * P3) & M32, 17) * P4 & M32
        i += 4
    while i < n:
        h = _rotl((h + data[i] * P5) & M32, 11) * P1 & M32
        i += 1
    h ^= h >> 15
    h = h * P2 & M32
    h ^= h >> 13
    h = h * P3 & M32
    h ^= h >> 16
    return h


def quick_hash(data):
    """PPSSPP's QuickTexHash, aligned path (16-byte aligned, size a multiple of 64)."""
    cursor2 = [0xc00b, 0x9bd9, 0x4b73, 0xb651, 0x4d9b, 0x4309, 0x0083, 0x0001]
    cursor16 = [0] * 8
    for i in range(0, len(data) // 16, 4):
        p = i * 16
        v = struct.unpack_from("<8H", data, p)
        for j in range(8):
            cursor16[j] = (cursor16[j] + v[j] * cursor2[j]) & 0xFFFF
        a = struct.unpack_from("<4I", data, p + 16)
        b = struct.unpack_from("<4I", data, p + 32)
        for j in range(4):
            c = cursor16[j * 2] | cursor16[j * 2 + 1] << 16
            c = ((c ^ a[j]) + b[j]) & M32
            cursor16[j * 2], cursor16[j * 2 + 1] = c & 0xFFFF, c >> 16
        v = struct.unpack_from("<8H", data, p + 48)
        for j in range(8):
            cursor16[j] = (cursor16[j] ^ (v[j] * cursor2[j] & 0xFFFF)) & 0xFFFF
        cursor2 = [(c + 0x2455) & 0xFFFF for c in cursor2]
    check = 0
    for j in range(4):
        check += (cursor16[j * 2] | cursor16[j * 2 + 1] << 16) + (cursor2[j * 2] | cursor2[j * 2 + 1] << 16)
    return check & M32


def car_keys(data, t, base):
    """The pack key (24 hex digits) of each palette of car texture t."""
    texels = bytes(data[t.pos + t.pix_off[0]:t.pos + t.pal_off])
    pixels = t.largeur * t.hauteur  # buffer width = width for the cars
    texel_hash = quick_hash(texels[:pixels * t.bpp // 8])
    keys = []
    for p in range(t.palettes):
        at = t.pos + t.pal_off + p * 4 * t.couleurs
        clut = bytes(data[at:at + 4 * t.couleurs])
        keys.append(f"00000000{(base ^ xxh32(clut, 0xC0108888)) & M32:08x}{texel_hash:08x}")
    return keys

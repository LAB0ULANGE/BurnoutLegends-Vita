"""Converts the game's ATRAC3plus music (.mus RIFF files) to the recomp's
block IMA ADPCM format (.ima), decoded on the Vita by host/music.cpp.

File layout (little endian):
  'BIMA', version 1, riff_size (bytes 4..8 of the .mus: the lookup key),
  total_samples, start (fact delay), loop_start, loop_end (inclusive),
  block_samples (2048), block_count, then per block:
  int16 predictor L, uint8 index L, pad, int16 predictor R, uint8 index R, pad,
  1024 bytes of L codes, 1024 bytes of R codes (audioop nibble order).
"""
import array
import audioop
import os
import struct
import sys

import av

BLOCK = 2048


def decode(path):
    container = av.open(path)
    stream = container.streams.audio[0]
    resampler = av.AudioResampler(format="s16", layout="stereo", rate=44100)
    pcm = bytearray()
    for frame in container.decode(stream):
        for out in resampler.resample(frame):
            pcm += bytes(out.planes[0])[: out.samples * 4]
    for out in resampler.resample(None):
        pcm += bytes(out.planes[0])[: out.samples * 4]
    return pcm


def riff_info(path):
    data = open(path, "rb").read(4096)
    riff_size = struct.unpack_from("<I", data, 4)[0]
    start = 0
    loop_start = loop_end = None
    pos = 12
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fact" and len(body) >= 8:
            start = struct.unpack_from("<I", body, 4)[0]
        elif tag == b"smpl" and len(body) >= 36 + 24:
            loops = struct.unpack_from("<I", body, 28)[0]
            if loops:
                loop_start, loop_end = struct.unpack_from("<II", body, 36 + 8)
        elif tag == b"data":
            break
        pos += 8 + size + (size & 1)
    return riff_size, start, loop_start, loop_end


def convert(src, dst):
    riff_size, start, loop_start, loop_end = riff_info(src)
    pcm = decode(src)
    samples = array.array("h")
    samples.frombytes(bytes(pcm))
    total = len(samples) // 2
    if loop_start is None:
        loop_start, loop_end = start, total - 1
    loop_end = min(loop_end, total - 1)
    left = samples[0::2]
    right = samples[1::2]
    blocks = (total + BLOCK - 1) // BLOCK
    out = bytearray(struct.pack("<4s8I", b"BIMA", 1, riff_size, total, start, loop_start, loop_end, BLOCK, blocks))
    state_l = state_r = None
    for b in range(blocks):
        chunk_l = left[b * BLOCK:(b + 1) * BLOCK]
        chunk_r = right[b * BLOCK:(b + 1) * BLOCK]
        pad = BLOCK - len(chunk_l)
        if pad:
            chunk_l.extend([0] * pad)
            chunk_r.extend([0] * pad)
        pl, il = state_l if state_l else (0, 0)
        pr, ir = state_r if state_r else (0, 0)
        out += struct.pack("<hBxhBx", pl, il, pr, ir)
        code_l, state_l = audioop.lin2adpcm(chunk_l.tobytes(), 2, state_l)
        code_r, state_r = audioop.lin2adpcm(chunk_r.tobytes(), 2, state_r)
        out += code_l + code_r
    open(dst, "wb").write(out)
    return riff_size, total, start, loop_start, loop_end


if __name__ == "__main__":
    src_dir, dst_dir = sys.argv[1], sys.argv[2]
    os.makedirs(dst_dir, exist_ok=True)
    for name in sorted(os.listdir(src_dir)):
        if not name.lower().endswith(".mus"):
            continue
        dst = os.path.join(dst_dir, os.path.splitext(name)[0] + ".ima")
        info = convert(os.path.join(src_dir, name), dst)
        print(name, info, os.path.getsize(dst), flush=True)

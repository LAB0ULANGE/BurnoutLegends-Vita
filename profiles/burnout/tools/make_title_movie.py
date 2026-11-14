"""Builds a PSMF title movie (startB.pmf) from any video file.

Usage: python make_title_movie.py SOURCE ORIGINAL_PMF OUT_PMF [--seconds N]
       [--logo PNG --logo-width 260 --logo-top 100]

SOURCE        any video PyAV can read (e.g. Burnout 3's TRACKS/_FEMAIN.M2V)
ORIGINAL_PMF  the game's own startB.pmf: its PSMF header is reused
OUT_PMF       the new movie

The game's movies are H.264 Main@2.1, 480x272, 29.97 fps, I and P pictures
only, each access unit starting with a delimiter (the HLE splits on it), in
2048-byte MPEG-PS packs after a 2048-byte PSMF header. The source is scaled
to fill 480x272 (cropped in the middle) and resampled to 29.97 fps.
"""
import argparse
import fractions
import struct

import av

W, H = 480, 272
RATE = fractions.Fraction(30000, 1001)
PACK = 2048


def stamp_logo(pictures, logo_path, width, top):
    """Lays the logo (transparent PNG) on every picture, centred, `width` px wide."""
    from PIL import Image
    logo = Image.open(logo_path).convert("RGBA")
    logo = logo.resize((width, round(logo.height * width / logo.width)), Image.LANCZOS)
    left = (W - logo.width) // 2
    for picture in pictures:
        picture = picture.convert("RGB")
        picture.paste(logo, (left, top), logo)
        yield picture


def frames_29_97(path, seconds):
    src = av.open(path)
    stream = src.streams.video[0]
    rate = stream.average_rate or fractions.Fraction(25)
    pictures = []
    for frame in src.decode(video=0):
        image = frame.to_image()
        w, h = image.size
        scale = max(W / w, H / h)
        sw, sh = round(w * scale), round(h * scale)
        image = image.resize((sw, sh), 1)  # LANCZOS
        left, top = (sw - W) // 2, (sh - H) // 2
        pictures.append(image.crop((left, top, left + W, top + H)))
    count = int(len(pictures) / rate * RATE)
    if seconds:
        count = min(count, int(seconds * RATE))
    for j in range(count):
        yield pictures[min(len(pictures) - 1, int(j / RATE * rate))]


AUD = b"\x00\x00\x00\x01\x09\xf0"  # access unit delimiter, any picture type


def encode(pictures):
    cc = av.CodecContext.create("libx264", "w")
    cc.width, cc.height, cc.pix_fmt = W, H, "yuv420p"
    cc.time_base = 1 / RATE
    cc.framerate = RATE
    cc.options = {
        "profile": "main", "level": "2.1", "preset": "slow", "crf": "22",
        "x264-params": "bframes=0:ref=3:keyint=30:min-keyint=30:scenecut=0:"
                       "colorprim=smpte170m:transfer=smpte170m:colormatrix=smpte170m:"
                       "vbv-maxrate=4000:vbv-bufsize=4000",
    }
    out = bytearray()
    count = 0

    def take(packets):
        nonlocal out
        for packet in packets:
            unit = bytes(packet)
            # One delimiter per picture: the HLE cuts the stream on them.
            if not (unit.startswith(AUD) or unit.startswith(AUD[1:])):
                out += AUD
            out += unit

    for n, picture in enumerate(pictures):
        frame = av.VideoFrame.from_image(picture)
        frame.pts = n
        take(cc.encode(frame))
        count += 1
    take(cc.encode(None))
    return bytes(out), count


def pack_header(scr):
    # MPEG-2 pack header, SCR in 90 kHz units, mux rate as in the game's files.
    b = bytearray(b"\x00\x00\x01\xba")
    b += bytes([0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03),
                (scr >> 20) & 0xFF,
                0x04 | ((scr >> 12) & 0xF8) | ((scr >> 13) & 0x03),
                (scr >> 5) & 0xFF,
                0x04 | ((scr << 3) & 0xF8), 0x01,
                0x01, 0x86, 0xA3, 0xF8])
    return bytes(b)


def mux(es):
    payload_per_pack = PACK - 14 - 9
    out = bytearray()
    for n, at in enumerate(range(0, len(es), payload_per_pack)):
        chunk = es[at:at + payload_per_pack]
        pes = b"\x00\x00\x01\xe0" + struct.pack(">H", 3 + len(chunk)) + b"\x81\x00\x00" + chunk
        pack = pack_header(90000 + n * 300) + pes
        if len(pack) < PACK:  # padding stream to the pack size
            pad = PACK - len(pack)
            if pad >= 6:
                pack += b"\x00\x00\x01\xbe" + struct.pack(">H", pad - 6) + b"\xff" * (pad - 6)
            else:
                pack += b"\xff" * pad
        out += pack
    return bytes(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("source")
    p.add_argument("original_pmf")
    p.add_argument("out_pmf")
    p.add_argument("--seconds", type=float, default=0)
    p.add_argument("--logo", help="transparent PNG laid over every frame")
    p.add_argument("--logo-width", type=int, default=260)
    p.add_argument("--logo-top", type=int, default=100)
    a = p.parse_args()

    pictures = frames_29_97(a.source, a.seconds)
    if a.logo:
        pictures = stamp_logo(pictures, a.logo, a.logo_width, a.logo_top)
    es, count = encode(pictures)
    stream = mux(es)
    header = bytearray(open(a.original_pmf, "rb").read()[:PACK])
    assert header[:4] == b"PSMF"
    struct.pack_into(">I", header, 12, len(stream))
    end = 90000 + count * 3003
    old_end = header[0x5A:0x60]
    for at in range(0x50, PACK - 6):  # every copy of the end timestamp
        if header[at:at + 6] == old_end:
            header[at:at + 6] = end.to_bytes(6, "big")
    with open(a.out_pmf, "wb") as f:
        f.write(header + stream)
    print(f"{a.out_pmf}: {count} frames, {count / float(RATE):.1f} s, {len(header) + len(stream)} bytes")


if __name__ == "__main__":
    main()

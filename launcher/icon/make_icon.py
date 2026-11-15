# Draws Ranga Emu's icon (an "R" on a red rounded square) and writes ranga.ico beside launcher.rc, plus a 256 px
# preview. Pure Python: no image library. Run: python make_icon.py
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOP, BOTTOM = (0xF2, 0x5C, 0x61), (0xB8, 0x2C, 0x34)  # the client's accent, lighter above
WHITE = (0xFF, 0xFF, 0xFF)


def rounded(x, y, x0, y0, x1, y1, r):
    """Inside a rectangle with rounded corners."""
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def letter(x, y):
    """The R: a stem, a bowl closed by a half ring, and a slanted leg."""
    if 0.285 <= x <= 0.415 and 0.23 <= y <= 0.77:
        return True
    if 0.285 <= x <= 0.54 and (0.23 <= y <= 0.35 or 0.43 <= y <= 0.55):
        return True
    if x >= 0.54:
        d = math.hypot(x - 0.54, y - 0.39)
        if 0.04 <= d <= 0.16:
            return True
    if 0.55 <= y <= 0.77:
        left = 0.455 + (y - 0.55) * (0.145 / 0.22)
        if left <= x <= left + 0.135:
            return True
    return False


def pixel(x, y):
    """Colour and coverage (r, g, b, a in 0..1) of one point of the unit square."""
    if not rounded(x, y, 0.03, 0.03, 0.97, 0.97, 0.21):
        return (0, 0, 0, 0.0)
    if letter(x, y):
        return WHITE + (1.0,)
    t = (y - 0.03) / 0.94
    # A soft light from the top left corner.
    glow = max(0.0, 1.0 - math.hypot(x - 0.2, y - 0.1) / 0.9) * 0.10
    colour = tuple(min(255, (TOP[i] + (BOTTOM[i] - TOP[i]) * t) * (1 + glow)) for i in range(3))
    # The letter's shadow, down and to the right.
    if letter(x - 0.022, y - 0.028):
        colour = tuple(c * 0.78 for c in colour)
    return colour + (1.0,)


def render(size):
    samples = 4 if size >= 128 else 6
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0.0
            for sy in range(samples):
                for sx in range(samples):
                    cr, cg, cb, ca = pixel((px + (sx + 0.5) / samples) / size, (py + (sy + 0.5) / samples) / size)
                    r += cr * ca
                    g += cg * ca
                    b += cb * ca
                    a += ca
            if a > 0:
                row += bytes((round(r / a), round(g / a), round(b / a), round(255 * a / (samples * samples))))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(rows, size):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib(rows, size):
    """32-bit bitmap as an icon holds it: bottom-up BGRA and an (empty) AND mask."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(bytes(b for i in range(0, len(row), 4) for b in (row[i + 2], row[i + 1], row[i], row[i + 3])) for row in reversed(rows))
    mask = bytes(((size + 31) // 32 * 4) * size)
    return header + pixels + mask


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = []
    for size in sizes:
        rows = render(size)
        images.append(png(rows, size) if size == 256 else dib(rows, size))
        if size == 256:
            open(os.path.join(HERE, "ranga-256.png"), "wb").write(png(rows, size))
    out = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for size, image in zip(sizes, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    open(os.path.join(HERE, "..", "ranga.ico"), "wb").write(out + b"".join(images))
    print("ranga.ico", len(out) + sum(map(len, images)), "bytes")


main()

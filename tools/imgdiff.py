"""Minimal image helpers for the golden-screenshot tests (0.7), standard
library only: QEMU screendumps are read as PPM, golden images are stored as
PNG (8-bit RGB). Comparison counts pixels whose largest channel difference
exceeds a tolerance."""
import struct
import zlib
from pathlib import Path


class Image:
    def __init__(self, w, h, rgb):
        self.w, self.h, self.rgb = w, h, bytes(rgb)

    def crop(self, x, y, w, h):
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.w, x + w), min(self.h, y + h)
        rows = [self.rgb[(yy * self.w + x0) * 3:(yy * self.w + x1) * 3] for yy in range(y0, y1)]
        return Image(max(0, x1 - x0), max(0, y1 - y0), b"".join(rows))

    def pixel(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.rgb[i:i + 3])


def read_ppm(path) -> Image:
    data = Path(path).read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    if parts[0] != b"P6" or int(parts[3]) != 255:
        raise ValueError(f"{path}: not an 8-bit P6 PPM")
    w, h = int(parts[1]), int(parts[2])
    pos += 1
    return Image(w, h, data[pos:pos + w * h * 3])


def write_png(path, img: Image):
    raw = b"".join(b"\x00" + img.rgb[y * img.w * 3:(y + 1) * img.w * 3] for y in range(img.h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


def read_png(path) -> Image:
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError(f"{path}: only 8-bit RGB/RGBA, non-interlaced")
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out, prev, i = bytearray(), bytearray(stride), 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 0xFF
            elif f == 2:
                line[x] = (line[x] + b) & 0xFF
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        prev = line
        if bpp == 4:
            out += b"".join(bytes(line[k:k + 3]) for k in range(0, stride, 4))
        else:
            out += line
    return Image(w, h, out)


def diff(a: Image, b: Image, tol=8, mask=()):
    """(differing pixels, total pixels, largest delta). mask: rects (x, y,
    w, h) in image coordinates that are ignored."""
    if (a.w, a.h) != (b.w, b.h):
        return a.w * a.h, a.w * a.h, 255
    masked = bytearray(a.w * a.h)
    for (mx, my, mw, mh) in mask:
        for yy in range(max(0, my), min(a.h, my + mh)):
            for xx in range(max(0, mx), min(a.w, mx + mw)):
                masked[yy * a.w + xx] = 1
    bad, worst = 0, 0
    ra, rb = a.rgb, b.rgb
    for p in range(a.w * a.h):
        if masked[p]:
            continue
        i = p * 3
        d = max(abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2]))
        if d > worst:
            worst = d
        if d > tol:
            bad += 1
    return bad, a.w * a.h, worst

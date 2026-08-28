#!/usr/bin/env python3
"""Generate app.ico for CalmDownGPU: blue rounded square + six-armed snowflake.

Pure stdlib. Sizes 16/24/32/48 as BMP entries, 256 as PNG (zlib).
Run from the repo root:  python tools/make_icon.py
"""
import math, struct, zlib

SIZES = [16, 24, 32, 48]          # BMP entries
PNG_SIZE = 256                    # PNG entry
SS = 4                            # supersampling factor

TOP    = (59, 130, 246)           # #3B82F6
BOTTOM = (29, 78, 216)            # #1D4ED8
BORDER = (30, 58, 138)            # #1E3A8A
FLAKE  = (240, 249, 255)          # #F0F9FF


def sdf_rounded_rect(px, py, size):
    half = size / 2.0
    rad = size * 0.22
    qx, qy = abs(px) - (half - rad), abs(py) - (half - rad)
    ox, oy = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ox, oy) + min(max(qx, qy), 0.0) - rad


def seg_dist(px, py, x0, y0, x1, y1):
    vx, vy = x1 - x0, y1 - y0
    wx, wy = px - x0, py - y0
    t = max(0.0, min(1.0, (wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-9)))
    return math.hypot(px - (x0 + t * vx), py - (y0 + t * vy))


def flake_dist(px, py, size):
    """Distance to the snowflake strokes (arms + branches), or None."""
    r = size * 0.36
    halfw = max(0.8, size / 14.0)
    d = None
    for ang in (90.0, 30.0, -30.0):                       # 6 arms via 3 lines
        a = math.radians(ang)
        dx, dy = math.cos(a), -math.sin(a)                # screen y goes down
        d = min(d or 1e9, seg_dist(px, py, -r * dx, -r * dy, r * dx, r * dy))
    if size >= 32:                                        # branch ticks
        for ang in range(0, 360, 60):
            a = math.radians(ang)
            dx, dy = math.cos(a), -math.sin(a)
            ax, ay = r * 0.60 * dx, r * 0.60 * dy
            bl = r * 0.22
            for da in (45.0, -45.0):
                b = math.radians(ang + da)
                bx, by = math.cos(b), -math.sin(b)
                d = min(d or 1e9, seg_dist(px, py, ax, ay, ax + bl * bx, ay + bl * by))
    return d - halfw / 2.0


def render(size):
    """Return top-down rows of RGBA bytes."""
    border_w = max(1.0, size / 22.0)
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS - size / 2.0
                    py = y + (sy + 0.5) / SS - size / 2.0
                    d = sdf_rounded_rect(px, py, size)
                    if d >= 0.0:
                        continue                     # outside
                    if d > -border_w:                # edge ring
                        cr, cg, cb = BORDER
                    else:
                        t = (py / size + 0.5)
                        cr = int(TOP[0] + (BOTTOM[0] - TOP[0]) * t)
                        cg = int(TOP[1] + (BOTTOM[1] - TOP[1]) * t)
                        cb = int(TOP[2] + (BOTTOM[2] - TOP[2]) * t)
                    fd = flake_dist(px, py, size)
                    if fd is not None and fd < 0.0:
                        cr, cg, cb = FLAKE
                    r += cr; g += cg; b += cb; a += 255
            n = SS * SS
            row += bytes((r // n, g // n, b // n, a // n))
        rows.append(bytes(row))
    return rows


def bmp_entry(size, rows):
    """ICO BMP image: BITMAPINFOHEADER + bottom-up BGRA + 1bpp AND mask."""
    hdr = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0,
                      size * size * 4, 0, 0, 0, 0)
    xor = bytearray()
    for row in reversed(rows):                           # bottom-up
        for i in range(0, len(row), 4):
            r, g, b, a = row[i:i + 4]
            xor += bytes((b, g, r, a))
    stride = ((size + 31) // 32) * 4
    mask = bytes(stride * size)                          # all opaque via alpha
    return hdr + bytes(xor) + mask


def png_entry(rows):
    """Minimal PNG encoder (filter 0, RGBA)."""
    def chunk(typ, data):
        return (struct.pack('>I', len(data)) + typ + data +
                struct.pack('>I', zlib.crc32(typ + data) & 0xFFFFFFFF))
    ihdr = struct.pack('>IIBBBBB', len(rows[0]) // 4, len(rows), 8, 6, 0, 0, 0)
    raw = b''.join(b'\x00' + row for row in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    images = []                                          # (size, bytes)
    for s in SIZES:
        images.append((s, bmp_entry(s, render(s))))
        print(f"  {s}px bmp  {len(images[-1][1])} bytes")
    rows = render(PNG_SIZE)
    images.append((PNG_SIZE, png_entry(rows)))
    print(f"  {PNG_SIZE}px png  {len(images[-1][1])} bytes")

    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, data in images:
        out += struct.pack('<BBBBHHII', s if s < 256 else 0,
                           s if s < 256 else 0, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    with open('app.ico', 'wb') as f:
        f.write(out)
    print(f"app.ico written: {len(out)} bytes, {len(images)} images")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Generate client/hidway.ico: a dark rounded square with a light 'H' and a
green accent bar (matches the app's control-panel look). Pure stdlib."""
import struct, os, math

SIZES = [16, 24, 32, 48, 64, 128, 256]

BG     = (30, 34, 42)      # #1E222A dark card
H_COL  = (230, 233, 239)   # light letter
ACCENT = (74, 222, 128)    # green accent bar / live colour


def px(w, h):
    return [[(0, 0, 0, 0) for _ in range(w)] for _ in range(h)]


def rounded_alpha(x, y, s, rad):
    """Anti-aliased-ish coverage of a rounded square at pixel (x,y)."""
    # distance into the nearest corner region
    cx = min(x, s - 1 - x)
    cy = min(y, s - 1 - y)
    if cx >= rad or cy >= rad:
        return 1.0
    dx = rad - cx
    dy = rad - cy
    d = math.hypot(dx, dy)
    if d <= rad - 1:
        return 1.0
    if d >= rad + 1:
        return 0.0
    return max(0.0, min(1.0, (rad + 1 - d) / 2.0))


def blend(dst, src, a):
    return tuple(int(round(src[i] * a + dst[i] * (1 - a))) for i in range(3))


def draw(s):
    img = px(s, s)
    rad = max(2, int(s * 0.22))
    # background rounded square
    for y in range(s):
        for x in range(s):
            a = rounded_alpha(x, y, s, rad)
            if a > 0:
                img[y][x] = (BG[0], BG[1], BG[2], int(255 * a))

    def fillrect(x0, y0, x1, y1, col):
        for y in range(max(0, y0), min(s, y1)):
            for x in range(max(0, x0), min(s, x1)):
                if img[y][x][3] == 0:
                    continue
                img[y][x] = (*blend(img[y][x][:3], col, 1.0), img[y][x][3])

    # letter H
    bw = max(2, int(round(s * 0.11)))
    top = int(s * 0.28)
    bot = int(s * 0.66)
    lx = int(s * 0.32)
    rx = int(s * 0.68) - bw
    fillrect(lx, top, lx + bw, bot, H_COL)          # left bar
    fillrect(rx, top, rx + bw, bot, H_COL)          # right bar
    cy = (top + bot) // 2
    ch = max(2, int(round(s * 0.10)))
    fillrect(lx, cy - ch // 2, rx + bw, cy - ch // 2 + ch, H_COL)  # crossbar

    # green accent bar near the bottom
    ay = int(s * 0.76)
    ah = max(2, int(round(s * 0.085)))
    fillrect(int(s * 0.32), ay, int(s * 0.68), ay + ah, ACCENT)
    return img


def ico_image(img):
    s = len(img)
    # BITMAPINFOHEADER, height doubled for XOR+AND
    hdr = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(s - 1, -1, -1):      # bottom-up
        for x in range(s):
            r, g, b, a = img[y][x]
            xor += bytes((b, g, r, a))   # BGRA
    # AND mask: all opaque (0), rows padded to 32-bit
    row_bytes = ((s + 31) // 32) * 4
    andmask = bytes(row_bytes * s)
    return hdr + bytes(xor) + andmask


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, '..', 'client', 'hidway.ico')
    images = [ico_image(draw(s)) for s in SIZES]

    entries = bytearray()
    data = bytearray()
    offset = 6 + 16 * len(images)
    for s, img in zip(SIZES, images):
        w = 0 if s == 256 else s
        entries += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(img), offset)
        data += img
        offset += len(img)

    with open(out, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(images)))
        f.write(entries)
        f.write(data)
    print('wrote', os.path.normpath(out), os.path.getsize(out), 'bytes')


if __name__ == '__main__':
    main()

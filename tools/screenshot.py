"""Takes a picture of what the uptick panel is showing, from /api/screen.

Cameras wash LED panels out, so this draws the frame instead: each pixel as a
round LED on a dark board, with a little glow, in the colors the firmware drew.

    python tools/screenshot.py                      # uptick.local -> screen.png
    python tools/screenshot.py -o docs/chart.png
    python tools/screenshot.py --host 192.168.1.184 --pitch 12

Needs Pillow (pip install pillow).
"""
import argparse
import struct
import urllib.request

from PIL import Image, ImageChops, ImageDraw, ImageFilter

W, H = 64, 32


def fetch(host):
    with urllib.request.urlopen('http://%s/api/screen' % host, timeout=10) as r:
        data = r.read()
    if len(data) != W * H * 2:
        raise SystemExit('expected %d bytes from /api/screen, got %d' % (W * H * 2, len(data)))
    return struct.unpack('<%dH' % (W * H), data)


def rgb(c):
    r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def render(px, pitch=10, border=None):
    border = pitch if border is None else border
    size = (W * pitch + 2 * border, H * pitch + 2 * border)
    rad = pitch * 0.42
    board = Image.new('RGB', size, (8, 8, 9))
    lit = Image.new('RGB', size, (0, 0, 0))
    db, dl = ImageDraw.Draw(board), ImageDraw.Draw(lit)
    for y in range(H):
        for x in range(W):
            cx = border + x * pitch + pitch / 2
            cy = border + y * pitch + pitch / 2
            box = (cx - rad, cy - rad, cx + rad, cy + rad)
            c = rgb(px[y * W + x])
            if c == (0, 0, 0):
                db.ellipse(box, fill=(22, 22, 24))
            else:
                dl.ellipse(box, fill=c)
    glow = lit.filter(ImageFilter.GaussianBlur(pitch * 0.45))
    glow = Image.eval(glow, lambda v: v * 6 // 10)
    out = ImageChops.add(board, glow)
    mask = lit.convert('L').point(lambda v: 255 if v else 0)
    out.paste(lit, (0, 0), mask)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--host', default='uptick.local')
    ap.add_argument('-o', '--out', default='screen.png')
    ap.add_argument('--pitch', type=int, default=10, help='pixels per LED in the picture')
    a = ap.parse_args()
    render(fetch(a.host), a.pitch).save(a.out)
    print('saved', a.out)


if __name__ == '__main__':
    main()

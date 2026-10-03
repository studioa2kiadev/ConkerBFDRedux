"""Make the GAME SETTINGS menu's word pictures (host/assets/redux_menu) from the art atlas
(host/art/redux_menu_atlas.webp: words on a white background).

Each word is cropped from the atlas, its white background keyed out (near-white, the JPEG fringe
too, goes fully transparent: a hard one-bit edge, as N64 art has), then crunched: shrunk to a low
resolution (with premultiplied alpha, so no white bleeds into the edges), colours cut to 5 bits a
channel (the N64's RGBA5551) and blown back up with nearest-neighbour so the game's smooth scaling
keeps the pixels square. 30 and 60 are put together from the atlas's own digits.
  tools/redux_menu_art.py [ATLAS]
Needs Pillow and NumPy."""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
ATLAS = os.path.join(ROOT, 'host', 'art', 'redux_menu_atlas.webp')
OUT = os.path.join(ROOT, 'host', 'assets', 'redux_menu')

# Boxes (x0, y0, x1, y1, inclusive) in the atlas, and the rows each word is crushed to.
LABEL_ROWS, TITLE_ROWS = 22, 30
WORDS = {
    'title_video': ((1064, 306, 1557, 430), TITLE_ROWS),
    'title_quit_game': ((1060, 133, 1905, 259), TITLE_ROWS),
    'label_resolution': ((18, 20, 666, 130), LABEL_ROWS),
    'label_refresh_rate': ((17, 136, 735, 246), LABEL_ROWS),
    'label_full_screen': ((44, 327, 705, 437), LABEL_ROWS),
    'label_aspect': ((46, 530, 458, 640), LABEL_ROWS),
    'aspect_16_9': ((605, 532, 894, 642), LABEL_ROWS),
    'aspect_4_3': ((968, 530, 1204, 639), LABEL_ROWS),
    'res_480': ((24, 776, 230, 886), LABEL_ROWS),
    'res_720': ((244, 776, 445, 886), LABEL_ROWS),
    'res_1080': ((462, 776, 723, 886), LABEL_ROWS),
    'res_1440': ((739, 776, 999, 886), LABEL_ROWS),
    'res_4k': ((1009, 776, 1160, 884), LABEL_ROWS),
    'on': ((1315, 709, 1501, 819), LABEL_ROWS),
    'off': ((1612, 709, 1827, 819), LABEL_ROWS),
    'yn_y': ((1408, 536, 1499, 641), LABEL_ROWS),
    'yn_slash': ((1505, 534, 1576, 655), LABEL_ROWS),
    'yn_n': ((1586, 536, 1686, 641), LABEL_ROWS),
}
# Words drawn on their own (host/art/<file>, a white background too), whole picture each.
SEPARATE = {
    'game_settings': ('redux_game_settings.png', TITLE_ROWS),
    'label_camera_follow': ('redux_camera_follow.png', LABEL_ROWS),
}
MIN_BLOB = 40   # kept pixels in a group smaller than this are specks (JPEG noise, stray marks): removed
# Single digits for 30 and 60 (the 0 is the last of 1080's: its left edge is cut where the 8 met
# it, so the 3 or 6 goes over that edge, as the atlas's digits overlap).
DIGIT_3 = (1123, 530, 1204, 639)
DIGIT_6 = (668, 532, 739, 642)
DIGIT_0 = (650, 776, 723, 886)
PAD = 6
KEY_LIMIT = 200 # a pixel whose darkest channel is above this is background (white, or its fringe)


def without_specks(keep):
    """keep with its small separate groups of pixels (8-connected) dropped."""
    from collections import deque
    h, w = keep.shape
    seen = np.zeros_like(keep)
    out = keep.copy()
    for y, x in zip(*np.nonzero(keep)):
        if seen[y, x]:
            continue
        group, q = [], deque([(y, x)])
        seen[y, x] = True
        while q:
            cy, cx = q.popleft()
            group.append((cy, cx))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < h and 0 <= nx < w and keep[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
        if len(group) < MIN_BLOB:
            for gy, gx in group:
                out[gy, gx] = False
    return out


def keyed(atlas, box):
    """The box's pixels, the white keyed out: RGBA, trimmed to what's left."""
    x0, y0, x1, y1 = box
    crop = atlas[max(y0 - PAD, 0):y1 + 1 + PAD, max(x0 - PAD, 0):x1 + 1 + PAD].astype(np.int32)
    keep = without_specks(crop.min(axis=2) < KEY_LIMIT)
    rgba = np.zeros(crop.shape[:2] + (4,), np.uint8)
    rgba[..., :3] = crop
    rgba[..., 3] = np.where(keep, 255, 0)
    ys, xs = np.nonzero(keep)
    return rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def overlay(back, front, front_x):
    """front pasted over back at front_x (bottoms aligned): a new RGBA array."""
    h = max(back.shape[0], front.shape[0])
    w = max(back.shape[1], front_x + front.shape[1])
    out = np.zeros((h, w, 4), np.uint8)
    def put(img, x):
        y = h - img.shape[0]
        region = out[y:y + img.shape[0], x:x + img.shape[1]]
        m = img[..., 3] > 0
        region[m] = img[m]
    put(back, w - back.shape[1] if front_x == 0 else 0)
    put(front, front_x)
    return out


def number(digit, zero):
    """A two-digit number: digit, then the 0, the digit over the 0's cut left edge."""
    overlap = 10
    w = digit.shape[1] + zero.shape[1] - overlap
    h = max(digit.shape[0], zero.shape[0])
    out = np.zeros((h, w, 4), np.uint8)
    for img, x in ((zero, digit.shape[1] - overlap), (digit, 0)):
        y = h - img.shape[0]
        region = out[y:y + img.shape[0], x:x + img.shape[1]]
        m = img[..., 3] > 0
        region[m] = img[m]
    return out


def crunch(rgba, rows, scale=4):
    im = Image.fromarray(rgba, 'RGBA').convert('RGBa') # premultiplied: no white bleeds in
    width = max(1, round(im.width * rows / im.height))
    small = np.asarray(im.resize((width, rows), Image.LANCZOS).convert('RGBA')).copy()
    alpha = small[..., 3] >= 128
    small[..., :3] = (small[..., :3] >> 3 << 3) | (small[..., :3] >> 5) # 5 bits a channel
    small[..., 3] = np.where(alpha, 255, 0)
    small[~alpha, :3] = 0
    return Image.fromarray(small, 'RGBA').resize((width * scale, rows * scale), Image.NEAREST)


def main():
    atlas = np.asarray(Image.open(sys.argv[1] if len(sys.argv) > 1 else ATLAS).convert('RGB'))
    os.makedirs(OUT, exist_ok=True)
    for name, (box, rows) in WORDS.items():
        crunch(keyed(atlas, box), rows).save(os.path.join(OUT, name + '.png'))
    for name, (file_name, rows) in SEPARATE.items():
        art = np.asarray(Image.open(os.path.join(ROOT, 'host', 'art', file_name)).convert('RGB'))
        crunch(keyed(art, (0, 0, art.shape[1] - 1, art.shape[0] - 1)), rows).save(os.path.join(OUT, name + '.png'))
    zero = keyed(atlas, DIGIT_0)
    crunch(number(keyed(atlas, DIGIT_3), zero), LABEL_ROWS).save(os.path.join(OUT, 'hz_30.png'))
    crunch(number(keyed(atlas, DIGIT_6), zero), LABEL_ROWS).save(os.path.join(OUT, 'hz_60.png'))
    print(f'{len(WORDS) + len(SEPARATE) + 2} pictures written to {os.path.normpath(OUT)}')


main()

#!/usr/bin/env python3
"""Checks where magpie-viewer drew the cursor.

Takes two pictures of the viewer's window, captured with and without the
cursor while everything else stayed the same, and compares the pixels that
differ with the rectangle the cursor image should cover. Used by
viewer_desktop_test.sh, whose set-up the numbers below assume: a 640x480
source shown 2x in a 1280x960 window, a cursor image of 32x32 with its hot
spot at 4,4, and a title bar of 28 pixels above the picture.

Usage: viewer_cursor_check.py WITH.ppm WITHOUT.ppm POINTER_X POINTER_Y CURSOR_SCALE
"""
import math
import sys

TITLE_BAR = 28
# The window's rounded corners differ between an active and an inactive window
EDGE = 4
SOURCE = (640, 480)
PICTURE = (1280, 960)
CURSOR_SIZE = 32
HOT_SPOT = 4


def load(path):
    with open(path, "rb") as file:
        data = file.read()
    # "P6\n<width> <height>\n255\n" and then the pixels
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    return width, height, parts[3]


def round_half_up(value):
    return int(math.floor(value + 0.5))


def main():
    width, height, with_cursor = load(sys.argv[1])
    width2, height2, without_cursor = load(sys.argv[2])
    if (width, height) != (width2, height2):
        sys.exit("the two pictures have different sizes")
    pointer = (int(sys.argv[3]), int(sys.argv[4]))
    option = float(sys.argv[5])

    # As ScalingGeometry::SrcToScaling and CursorDrawer::CalcCursorRect do it
    hot_spot = [round_half_up(pointer[i] / (SOURCE[i] - 1) * (PICTURE[i] - 1)) for i in range(2)]
    factor = option if option > 0 else PICTURE[0] / SOURCE[0]
    left = round_half_up(hot_spot[0] - HOT_SPOT * factor)
    top = round_half_up(hot_spot[1] - HOT_SPOT * factor)
    size = round_half_up(CURSOR_SIZE * factor)

    xs, ys = [], []
    for y in range(TITLE_BAR, height - EDGE):
        row_a = with_cursor[y * width * 3:(y + 1) * width * 3]
        row_b = without_cursor[y * width * 3:(y + 1) * width * 3]
        if row_a == row_b:
            continue
        for x in range(EDGE, width - EDGE):
            if row_a[x * 3:x * 3 + 3] != row_b[x * 3:x * 3 + 3]:
                xs.append(x)
                ys.append(y - TITLE_BAR)

    print(f"  expected: hot spot at {hot_spot[0]},{hot_spot[1]}, cursor image at "
          f"{left},{top} to {left + size},{top + size} (enlarged {factor:g} times)")
    if not xs:
        print("  FAILED: the pictures with and without cursor do not differ")
        sys.exit(1)

    inside = all(left <= x < left + size and top <= y < top + size for x, y in zip(xs, ys))
    print(f"  drawn:    {len(xs)} pixels differ, from {min(xs)},{min(ys)} to {max(xs) + 1},{max(ys) + 1}; "
          f"that is {min(xs) - left},{min(ys) - top} into the image and "
          f"{max(xs) + 1 - min(xs)}x{max(ys) + 1 - min(ys)} large")
    print("  " + ("all of them inside the expected image" if inside else "FAILED: some lie outside the expected image"))
    sys.exit(0 if inside else 1)


if __name__ == "__main__":
    main()

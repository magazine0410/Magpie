# Deterministic test picture with the kinds of content the effects react to:
# smooth gradients, flat areas with dark outlines (cartoon style), fine lines,
# text, a photographic-like textured area and low noise everywhere, so that no
# area is exactly flat and no gradient exactly linear.
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

def make(w, h, seed=1):
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    u, v = x / w, y / h
    # background: smooth colour field
    img = np.stack([
        0.55 + 0.35 * np.sin(2.1 * u + 1.3 * v + 0.4),
        0.50 + 0.30 * np.cos(1.7 * u - 2.3 * v + 1.1),
        0.60 + 0.30 * np.sin(3.1 * v + 0.7 * u + 2.0)], axis=2)
    # textured (photo-like) area: band-limited noise in the right third
    n = rng.standard_normal((h, w, 3))
    pil_n = Image.fromarray(np.clip(n * 40 + 128, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2))
    tex = (np.asarray(pil_n).astype(np.float64) - 128) / 128
    mask = np.clip((u - 0.66) * 12, 0, 1)[..., None]
    img = img + tex * 0.9 * mask
    pil = Image.fromarray(np.clip(img * 255, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(pil)
    s = min(w, h)
    # cartoon shapes: flat fills with dark outlines
    d.ellipse([0.08*w, 0.12*h, 0.08*w+0.36*s, 0.12*h+0.36*s], fill=(236, 190, 160), outline=(40, 24, 30), width=max(1, s//90))
    d.polygon([(0.30*w, 0.85*h), (0.44*w, 0.40*h), (0.58*w, 0.85*h)], fill=(70, 120, 200), outline=(20, 20, 50))
    d.rectangle([0.05*w, 0.62*h, 0.24*w, 0.92*h], fill=(230, 60, 70), outline=(60, 10, 10), width=max(1, s//120))
    # thin lines at several angles
    for i in range(9):
        a = i * np.pi / 9 + 0.1
        cx, cy, r = 0.50*w, 0.22*h, 0.16*s
        d.line([cx - r*np.cos(a), cy - r*np.sin(a), cx + r*np.cos(a), cy + r*np.sin(a)], fill=(15 + 20*i, 15, 60), width=1)
    # text
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", max(8, s // 9))
    except OSError:
        font = ImageFont.load_default()
    d.text((0.04*w, 0.02*h), "Magpie 12 px", fill=(250, 250, 245), font=font, stroke_width=1, stroke_fill=(10, 10, 10))
    out = np.asarray(pil).astype(np.float64)
    # low noise everywhere
    out += rng.normal(0, 1.2, out.shape)
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8))

if __name__ == "__main__":
    w, h, path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    make(w, h).save(path)

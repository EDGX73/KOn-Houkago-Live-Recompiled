"""Locate PSP button badges (round icons with a circle/cross/triangle/square)
in textures dumped with KON_TEXTURE_DUMP and write the table that
src/native_glyphs.hpp uses to redraw them for the device in use.

Usage: python tools/glyph_scan.py work/texdump [--out src/native_glyph_table.hpp] [--sheet out.png]
"""
import argparse, glob, os
from PIL import Image, ImageDraw

import numpy as np

def luminance(path):
    im = np.asarray(Image.open(path).convert("RGBA")).astype(float)
    return np.where(im[..., 3] > 128, (im[..., 0] * 3 + im[..., 1] * 6 + im[..., 2]) / 10, 255)

# A badge at scale 1: dark disk of radius ~6.2 inside a light border; the face
# is drawn light inside radius ~4.6. Larger prompts use the same shape scaled.
def _ring(r0, r1, step=0.5):
    pts = []
    for r in np.arange(r0, r1 + 1e-6, step):
        n = max(12, int(2 * np.pi * r / 0.8))
        pts += [(r * np.cos(t), r * np.sin(t)) for t in np.linspace(0, 2 * np.pi, n, endpoint=False)]
    return np.array(pts)
DARK_RING, LIGHT_RING = _ring(5.0, 6.0), _ring(7.5, 8.5)
_g = np.arange(-4.5, 4.51, 1.0)
INNER = np.array([(x, y) for y in _g for x in _g if x * x + y * y < 4.6 ** 2])
SCALES = (1.0, 1.2, 1.45, 1.7)

# Reference badges of the menu hint atlas.
EXEMPLAR_FILE = "2dcf09492a5c5385"
EXEMPLARS = {"circle": (20, 18), "cross": (85, 18), "triangle": (14, 50), "square": (85, 50)}

def sample(L, cx, cy, pts, scale):
    H, W = L.shape
    xs = np.floor(cx + pts[:, 0] * scale).astype(int)
    ys = np.floor(cy + pts[:, 1] * scale).astype(int)
    ok = (xs >= 0) & (xs < W) & (ys >= 0) & (ys < H)
    out = np.full(len(pts), 255.0)
    out[ok] = L[ys[ok], xs[ok]]
    return out

def interior(L, cx, cy, scale=1.0):
    return sample(L, cx, cy, INNER, scale) > 170

def badge_peaks(L, scale, thr=0.88):
    """Badge centers (on the half-texel grid) scoring >= thr for one scale."""
    H, W = L.shape
    found = []
    margin = int(np.ceil(8.5 * scale))
    dark = L < 110
    light = L > 170
    for cy2 in range(2 * margin, 2 * (H - margin) + 1):
        cy = cy2 / 2
        for cx2 in range(2 * margin, 2 * (W - margin) + 1):
            cx = cx2 / 2
            # cheap reject: the dark ring at its four axis points
            r = 5.9 * scale
            if not (dark[int(cy), int(cx + r)] and dark[int(cy), int(cx - r)] and dark[int(cy + r), int(cx)] and dark[int(cy - r), int(cx)]):
                continue
            # a circle face reaches into the dark ring: weight the light border more
            d = (sample(L, cx, cy, DARK_RING, scale) < 110).mean()
            l = (sample(L, cx, cy, LIGHT_RING, scale) > 170).mean()
            s = (d + 2 * l) / 3
            if d >= 0.7 and l >= 0.9 and s >= thr:
                found.append((s, cx, cy))
    found.sort(reverse=True)
    kept = []
    for f in found:
        if all((f[1] - k[1]) ** 2 + (f[2] - k[2]) ** 2 > (7 * scale) ** 2 for k in kept):
            kept.append(f)
    return kept

def scan_badges(path, references):
    """Dark round badges (menus, prompts) classified against the reference faces."""
    L = luminance(path)
    out = []
    for scale in SCALES:
        for s, cx, cy in badge_peaks(L, scale):
            if any((cx - o[0]) ** 2 + (cy - o[1]) ** 2 < (6 * scale) ** 2 for o in out):
                continue
            mask = interior(L, cx, cy, scale)
            ranked = sorted((int((mask != m).sum()), f) for f, m in references.items())
            (dist, face), second = ranked[0], ranked[1][0]
            if dist <= 0.22 * len(INNER) and second - dist >= 8:
                out.append((cx, cy, round(6.2 * scale, 2), face))
    return sorted(out, key=lambda o: (o[1], o[0]))

HINT_PILLS = [
    (447.0, 82.5, 18.0, "pill", "start"), (236.0, 114.5, 18.0, "pill", "start"),
    (135.0, 178.5, 18.0, "pill", "start"), (59.0, 184.5, 18.0, "pill", "start"),
    (142.0, 210.5, 18.0, "pill", "start"), (389.0, 210.5, 18.0, "pill", "select"),
    (270.0, 235.5, 18.0, "pill", "select"), (383.0, 238.5, 18.0, "pill", "select")]

# Glyphs measured by hand on the dumps (center, radius, kind, face):
#  note: gameplay note heads (white symbol on the colored head);
#  pill: a dark START/SELECT label.
MANUAL = {
    "6e1630b1206b6500": [(x, y, 10.0, "note", f) for x, y, f in (
        (21.0, 40.0, "circle"), (61.0, 40.0, "cross"), (101.0, 39.5, "triangle"), (141.0, 40.0, "square"),
        (223.5, 40.0, "circle"), (264.0, 39.5, "cross"), (305.5, 39.5, "triangle"), (343.5, 39.5, "square"),
        (416.5, 39.5, "circle"), (456.5, 39.5, "cross"), (16.0, 95.0, "triangle"), (56.5, 95.5, "square"))],
    # START/SELECT labels of the menu hint atlases, English and Japanese (same
    # layout; radius = half the pill width). Their badges are found by the scan
    # (keys ending in "+" add to the scan).
    "2dcf09492a5c5385+": HINT_PILLS,
    "c008c9b441259a4d+": HINT_PILLS,
}

# "PUSH (o) BUTTON!!" title card of every song (play_mNN_2.bin, same in both
# discs; m06 has none), decoded offline from std.cpk with the game's CLUT
# rules (hashes checked against KON_TEXTURE_DUMP): hash, size, center and
# radius of the saturated ring, measured on the decoded textures.
# Album (gallery.bin) atlases: their two badges, English and Japanese (found
# by scanning every interface texture of both discs decoded offline).
GALLERY_BADGES = [
    ("2d94605248c66682", 512, 512, 123.5, 154.0), ("2d94605248c66682", 512, 512, 123.5, 186.0),
    ("a20cbd8bf901f983", 512, 512, 124.0, 154.0), ("a20cbd8bf901f983", 512, 512, 124.0, 186.0),
]
SONG_CARD_RINGS = [
    ("c52e14dde9e70f7c", 256, 128, 63.0, 15.0, 7.5),
    ("fcb5932959b479da", 256, 128, 88.5, 14.5, 9.0),
    ("62827f5c87bc1e9d", 256, 128, 101.0, 17.0, 10.5),
    ("316dbf68c23d4752", 256, 128, 79.0, 22.5, 10.0),
    ("ee1d2f11311eb5a0", 256, 128, 74.0, 15.0, 8.5),
    ("6bef98718b3b434b", 256, 128, 97.5, 16.0, 11.5),
    ("453588914d8ea952", 256, 128, 89.5, 20.5, 9.0),
    ("ba42fb849c8d72cf", 256, 128, 87.5, 15.5, 9.0),
    ("36acf003eb262771", 256, 128, 65.5, 14.0, 8.0),
    ("b305aa324242b011", 256, 128, 73.5, 14.0, 10.0),
    ("2b0f9be76adcb76a", 256, 128, 75.5, 13.5, 9.0),
    ("c264b3c1bacf423a", 256, 128, 90.5, 13.5, 8.5),
    ("6474a5960b8ccf06", 256, 128, 94.5, 15.0, 10.0),
    ("a9c411ceedf54f6b", 256, 256, 88.5, 35.5, 9.0),
    ("c6405e424ab6283f", 256, 64, 70.5, 12.0, 6.5),
    ("df74fc669afdf038", 512, 128, 109.0, 30.0, 12.5),
    ("3ba5ff98f8638ef4", 512, 128, 84.5, 17.5, 9.5),
    ("e52fe9728e4b4b11", 512, 256, 91.5, 57.0, 9.5),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--out")
    ap.add_argument("--sheet")
    ap.add_argument("--list", help="reuse a previous run's printed entries instead of scanning")
    a = ap.parse_args()
    references = {}
    if not a.list:
        ref_paths = glob.glob(os.path.join(a.folder, EXEMPLAR_FILE + "_*.bmp"))
        if not ref_paths:
            raise SystemExit(f"glyph_scan: exemplar {EXEMPLAR_FILE} is not in {a.folder}; use a complete dump or --list")
        ref_L = luminance(ref_paths[0])
        references = {f: interior(ref_L, x, y) for f, (x, y) in EXEMPLARS.items()}
    entries = []
    for path in ([] if a.list else sorted(glob.glob(os.path.join(a.folder, "*.bmp")))):
        name = os.path.basename(path)
        h, size = name[:-4].split("_")
        w, hh = map(int, size.split("x"))
        manual = next((v for k, v in MANUAL.items() if h.startswith(k) and not k.endswith("+")), None)
        if manual:
            for cx, cy, r, kind, face in manual:
                entries.append((h, w, hh, kind, cx, cy, r, face))
            continue
        for cx, cy, r, kind, face in next((v for k, v in MANUAL.items() if k.endswith("+") and h.startswith(k[:-1])), []):
            entries.append((h, w, hh, kind, cx, cy, r, face))
        for cx, cy, r, face in scan_badges(path, references):
            entries.append((h, w, hh, "badge", cx, cy, r, face))
    for h, w, hh, cx, cy, r in SONG_CARD_RINGS:
        if not any(e[0] == h for e in entries):
            entries.append((h, w, hh, "ring", cx, cy, r, "circle"))
    for h, w, hh, cx, cy in GALLERY_BADGES:
        if not any(e[0] == h and e[4] == cx and e[5] == cy for e in entries):
            entries.append((h, w, hh, "badge", cx, cy, 6.2, "circle"))
    for e in entries:
        print(*e)
    if a.list:
        raw = open(a.list, "rb").read()
        text = raw.decode("utf-16") if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else raw.decode("utf-8-sig", errors="replace")
        entries = []
        for line in text.splitlines():
            p = line.split()
            if len(p) < 8:
                continue
            try:
                entries.append((p[0], int(p[1]), int(p[2]), p[3], float(p[4]), float(p[5]), float(p[6]), p[7]))
            except ValueError:
                continue  # console noise or a header line
    if a.sheet:
        crops = []
        for h, w, hh, kind, cx, cy, r, face in entries:
            paths = glob.glob(os.path.join(a.folder, h + "_*.bmp"))
            if not paths:
                continue  # partial dump: the fixed table entries may not be present
            im = Image.open(paths[0]).convert("RGBA")
            R = int(r) + 4
            c = im.crop((int(cx) - R, int(cy) - R, int(cx) + R, int(cy) + R))
            bg = Image.new("RGBA", c.size, (90, 90, 90, 255)); bg.alpha_composite(c)
            crops.append((bg.resize((c.width * 4, c.height * 4), Image.NEAREST), f"{h[:6]} {face}"))
        cols = 10
        sheet = Image.new("RGB", (cols * 130, ((len(crops) + cols - 1) // cols) * 140), (40, 40, 40))
        d = ImageDraw.Draw(sheet)
        for i, (c, label) in enumerate(crops):
            x, y = (i % cols) * 130, (i // cols) * 140
            c.thumbnail((120, 120)); sheet.paste(c.convert("RGB"), (x, y)); d.text((x, y + 122), label, fill=(255, 255, 0))
        sheet.save(a.sheet)
    if a.out:
        with open(a.out, "w", newline="\n") as f:
            f.write("#pragma once\n// Generated by tools/glyph_scan.py from KON_TEXTURE_DUMP textures: button\n")
            f.write("// badges per decoded-texture hash (center in texels, radius, PSP face).\n")
            f.write("#include <cstdint>\nnamespace kon::glyph_table {\n")
            f.write("enum class Kind : std::uint8_t {Badge,Note,Ring,Pill};\nenum class Face : std::uint8_t {Circle,Cross,Triangle,Square,Start,Select};\n")
            f.write("struct Glyph {std::uint64_t hash;std::uint16_t width,height;Kind kind;float x,y,radius;Face face;};\n")
            f.write("inline constexpr Glyph glyphs[]{\n")
            for h, w, hh, kind, cx, cy, r, face in entries:
                f.write(f"    {{0x{h}ull,{w},{hh},Kind::{kind.capitalize()},{cx}f,{cy}f,{r}f,Face::{face.capitalize()}}},\n")
            f.write("};\n}\n")

if __name__ == "__main__":
    main()

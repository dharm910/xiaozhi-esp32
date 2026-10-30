#!/usr/bin/env python3
"""Generate the STEM math-symbol and story-icon LVGL fonts from a TrueType file.

The built-in noto "basic" fonts lack symbols such as √ ≤ ≥ ≠ ∞ → Δ Ω ∫ ✓. This script
rasterizes just those glyphs into an LVGL 9 `lv_font_fmt_txt` C source (4 bpp, plain bitmap
format, sparse cmap) so the firmware can use it as a primary font with the basic font as
fallback, plus a 40px picture font for story-mode icons (sun, cloud, heart, gear, ...).
It uses only the Python standard library, so no lv_font_conv / freetype is needed.

Usage:
    python3 scripts/gen_stem_math_font.py /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf

DejaVu Sans is released under the Bitstream Vera / DejaVu license, which permits embedding
and modification.
"""

import argparse
import math
import os
import struct
import sys

# Symbols the base font does not have, plus a few useful duplicates (rendered here first).
CODEPOINTS = sorted(set(
    [0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B5, 0x00B7, 0x00B9, 0x00BC, 0x00BD, 0x00BE,
     0x00D7, 0x00F7]
    + [0x0393, 0x0394, 0x0398, 0x039B, 0x03A0, 0x03A3, 0x03A6, 0x03A8, 0x03A9]
    + list(range(0x03B1, 0x03CA))
    + [0x2022, 0x2026, 0x2032, 0x2033]
    + [0x2070, 0x2071] + list(range(0x2074, 0x207F + 1))
    + list(range(0x2080, 0x208E + 1))
    + [0x2103, 0x2109, 0x2153, 0x2154]
    + list(range(0x2190, 0x2195 + 1)) + [0x21CC, 0x21D0, 0x21D2, 0x21D4]
    + [0x2200, 0x2202, 0x2203, 0x2205, 0x2206, 0x2207, 0x2208, 0x2209, 0x220F, 0x2211,
       0x2212, 0x2213, 0x2219, 0x221A, 0x221B, 0x221D, 0x221E, 0x2220, 0x2225, 0x2227,
       0x2228, 0x2229, 0x222A, 0x222B, 0x2234, 0x2235, 0x223C, 0x2245, 0x2248, 0x2260,
       0x2261, 0x2264, 0x2265, 0x226A, 0x226B, 0x2282, 0x2283, 0x2286, 0x2287, 0x22A5,
       0x22C5]
    + [0x25B2, 0x25BC, 0x25CF, 0x2605, 0x2606, 0x2713, 0x2717]
))

# Story-mode pictures; the names that use them are in main/display/lvgl_display/stem_board.cc.
ICON_CODEPOINTS = [
    0x2302, 0x2600, 0x2601, 0x2603, 0x2605, 0x260E, 0x2614, 0x2615, 0x2618, 0x261B, 0x2620,
    0x2622, 0x263A, 0x263E, 0x2640, 0x2642, 0x265A, 0x2665, 0x266A, 0x267B, 0x2691, 0x2693,
    0x2696, 0x2699, 0x269B, 0x26A0, 0x26A1, 0x2702, 0x2708, 0x2709, 0x270F, 0x2714,
    0x2744,
]


class TrueType:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        num_tables = struct.unpack_from(">H", self.data, 4)[0]
        self.tables = {}
        for i in range(num_tables):
            tag, _, offset, length = struct.unpack_from(">4sIII", self.data, 12 + 16 * i)
            self.tables[tag.decode("latin-1")] = (offset, length)
        head = self.tables["head"][0]
        self.units_per_em = struct.unpack_from(">H", self.data, head + 18)[0]
        self.index_to_loc_format = struct.unpack_from(">h", self.data, head + 50)[0]
        hhea = self.tables["hhea"][0]
        self.ascender, self.descender = struct.unpack_from(">hh", self.data, hhea + 4)
        self.num_hmetrics = struct.unpack_from(">H", self.data, hhea + 34)[0]
        self.cmap = self._read_cmap()

    def _read_cmap(self):
        base = self.tables["cmap"][0]
        count = struct.unpack_from(">H", self.data, base + 2)[0]
        best = None
        for i in range(count):
            pid, eid, off = struct.unpack_from(">HHI", self.data, base + 4 + 8 * i)
            fmt = struct.unpack_from(">H", self.data, base + off)[0]
            if fmt == 12:
                best = (12, base + off)
            elif fmt == 4 and pid in (0, 3) and best is None:
                best = (4, base + off)
        mapping = {}
        fmt, off = best
        if fmt == 4:
            seg_x2 = struct.unpack_from(">H", self.data, off + 6)[0]
            segs = seg_x2 // 2
            ends = off + 14
            starts = ends + seg_x2 + 2
            deltas = starts + seg_x2
            range_offs = deltas + seg_x2
            for s in range(segs):
                end = struct.unpack_from(">H", self.data, ends + 2 * s)[0]
                start = struct.unpack_from(">H", self.data, starts + 2 * s)[0]
                delta = struct.unpack_from(">h", self.data, deltas + 2 * s)[0]
                ro_pos = range_offs + 2 * s
                ro = struct.unpack_from(">H", self.data, ro_pos)[0]
                for c in range(start, end + 1):
                    if c == 0xFFFF:
                        continue
                    if ro == 0:
                        gid = (c + delta) & 0xFFFF
                    else:
                        gid = struct.unpack_from(">H", self.data, ro_pos + ro + 2 * (c - start))[0]
                        if gid:
                            gid = (gid + delta) & 0xFFFF
                    if gid:
                        mapping[c] = gid
        else:
            groups = struct.unpack_from(">I", self.data, off + 12)[0]
            for g in range(groups):
                start, end, gid0 = struct.unpack_from(">III", self.data, off + 16 + 12 * g)
                for c in range(start, end + 1):
                    mapping[c] = gid0 + (c - start)
        return mapping

    def advance(self, gid):
        hmtx = self.tables["hmtx"][0]
        idx = min(gid, self.num_hmetrics - 1)
        return struct.unpack_from(">H", self.data, hmtx + 4 * idx)[0]

    def _glyph_range(self, gid):
        loca = self.tables["loca"][0]
        if self.index_to_loc_format == 0:
            a, b = struct.unpack_from(">HH", self.data, loca + 2 * gid)
            a, b = a * 2, b * 2
        else:
            a, b = struct.unpack_from(">II", self.data, loca + 4 * gid)
        return self.tables["glyf"][0] + a, b - a

    def contours(self, gid, depth=0):
        """Return a list of contours, each a list of (x, y, on_curve) in font units."""
        off, length = self._glyph_range(gid)
        if length == 0 or depth > 8:
            return []
        n = struct.unpack_from(">h", self.data, off)[0]
        p = off + 10
        if n >= 0:
            ends = struct.unpack_from(">%dH" % n, self.data, p)
            p += 2 * n
            ins_len = struct.unpack_from(">H", self.data, p)[0]
            p += 2 + ins_len
            npts = ends[-1] + 1 if n else 0
            flags = []
            while len(flags) < npts:
                f = self.data[p]
                p += 1
                flags.append(f)
                if f & 8:
                    rep = self.data[p]
                    p += 1
                    flags.extend([f] * rep)
            coords = []
            for short_bit, same_bit in ((2, 16), (4, 32)):
                vals, v = [], 0
                for f in flags:
                    if f & short_bit:
                        d = self.data[p]
                        p += 1
                        v += d if f & same_bit else -d
                    elif not f & same_bit:
                        v += struct.unpack_from(">h", self.data, p)[0]
                        p += 2
                    vals.append(v)
                coords.append(vals)
            result, start = [], 0
            for e in ends:
                result.append([(coords[0][i], coords[1][i], bool(flags[i] & 1))
                               for i in range(start, e + 1)])
                start = e + 1
            return result
        # Composite glyph
        result = []
        while True:
            flags, sub = struct.unpack_from(">HH", self.data, p)
            p += 4
            if flags & 1:
                dx, dy = struct.unpack_from(">hh", self.data, p)
                p += 4
            else:
                dx, dy = struct.unpack_from(">bb", self.data, p)
                p += 2
            a, b, c, d = 1.0, 0.0, 0.0, 1.0
            if flags & 8:
                a = d = struct.unpack_from(">h", self.data, p)[0] / 16384.0
                p += 2
            elif flags & 0x40:
                a, d = (v / 16384.0 for v in struct.unpack_from(">hh", self.data, p))
                p += 4
            elif flags & 0x80:
                a, b, c, d = (v / 16384.0 for v in struct.unpack_from(">hhhh", self.data, p))
                p += 8
            if not flags & 2:
                dx = dy = 0  # point matching is not supported; such components are rare
            for contour in self.contours(sub, depth + 1):
                result.append([(a * x + c * y + dx, b * x + d * y + dy, on)
                               for x, y, on in contour])
            if not flags & 0x20:
                break
        return result


def flatten(contour, scale, steps=6):
    """Convert a TrueType quadratic contour into a closed polygon in pixel units."""
    pts = [(x * scale, y * scale, on) for x, y, on in contour]
    if not pts:
        return []
    # Ensure the contour starts on an on-curve point.
    if not pts[0][2]:
        if pts[-1][2]:
            pts = [pts[-1]] + pts[:-1]
        else:
            mid = ((pts[0][0] + pts[-1][0]) / 2, (pts[0][1] + pts[-1][1]) / 2, True)
            pts = [mid] + pts
    poly = [(pts[0][0], pts[0][1])]
    i, n = 1, len(pts)
    cur = pts[0]
    while i <= n:
        p = pts[i % n]
        if p[2]:
            poly.append((p[0], p[1]))
            cur = p
            i += 1
            continue
        nxt = pts[(i + 1) % n]
        if nxt[2]:
            end = nxt
            i += 2
        else:
            end = ((p[0] + nxt[0]) / 2, (p[1] + nxt[1]) / 2, True)
            i += 1
        for s in range(1, steps + 1):
            t = s / steps
            x = (1 - t) ** 2 * cur[0] + 2 * (1 - t) * t * p[0] + t * t * end[0]
            y = (1 - t) ** 2 * cur[1] + 2 * (1 - t) * t * p[1] + t * t * end[1]
            poly.append((x, y))
        cur = end
    return poly


def rasterize(polys, x0, y_top, w, h, ss=4):
    """Non-zero winding scanline fill with ss x ss supersampling. Returns rows of 0..15."""
    edges = []
    for poly in polys:
        for (ax, ay), (bx, by) in zip(poly, poly[1:] + poly[:1]):
            if ay != by:
                edges.append((ax, ay, bx, by))
    cover = [[0] * w for _ in range(h)]
    for row in range(h * ss):
        y = y_top - (row + 0.5) / ss  # y axis points up in font space
        crossings = []
        for ax, ay, bx, by in edges:
            if (ay <= y < by) or (by <= y < ay):
                x = ax + (y - ay) * (bx - ax) / (by - ay)
                crossings.append((x, 1 if by > ay else -1))
        if not crossings:
            continue
        crossings.sort()
        winding = 0
        for k in range(len(crossings) - 1):
            winding += crossings[k][1]
            if winding == 0:
                continue
            xa, xb = crossings[k][0], crossings[k + 1][0]
            for col in range(w * ss):
                sx = x0 + (col + 0.5) / ss
                if xa <= sx < xb:
                    cover[row // ss][col // ss] += 1
    full = ss * ss
    return [[min(15, (c * 15 + full // 2) // full) for c in r] for r in cover]


def build(ttf, size, name, base_line_height, base_line, codepoints, description):
    scale = size / ttf.units_per_em
    bitmap = []
    glyphs = []
    present = []
    for cp in codepoints:
        gid = ttf.cmap.get(cp)
        if gid is None:
            print("warning: U+%04X not in font, skipped" % cp, file=sys.stderr)
            continue
        polys = [p for p in (flatten(c, scale) for c in ttf.contours(gid)) if len(p) > 2]
        adv_w = int(round(ttf.advance(gid) * scale * 16))
        if not polys:
            glyphs.append((cp, len(bitmap), adv_w, 0, 0, 0, 0))
            present.append(cp)
            continue
        xs = [x for p in polys for x, _ in p]
        ys = [y for p in polys for _, y in p]
        x_min, x_max = math.floor(min(xs)), math.ceil(max(xs))
        y_min, y_max = math.floor(min(ys)), math.ceil(max(ys))
        w, h = max(1, x_max - x_min), max(1, y_max - y_min)
        rows = rasterize(polys, x_min, y_max, w, h)
        # Pack 4 bpp continuously across rows, as lv_font_conv does for bitmap_format 0.
        nibbles = [v for r in rows for v in r]
        if len(nibbles) % 2:
            nibbles.append(0)
        index = len(bitmap)
        bitmap.extend((nibbles[i] << 4) | nibbles[i + 1] for i in range(0, len(nibbles), 2))
        glyphs.append((cp, index, adv_w, w, h, x_min, y_min))
        present.append(cp)

    out = []
    out.append("/*******************************************************************************")
    out.append(" * Size: %d px" % size)
    out.append(" * Bpp: 4")
    out.append(" * %s" % description)
    out.append(" * Generated by scripts/gen_stem_math_font.py from DejaVu Sans; do not edit.")
    out.append(" ******************************************************************************/")
    out.append("")
    out.append('#include "lvgl.h"')
    out.append("")
    out.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 16):
        out.append("    " + ", ".join("0x%02x" % b for b in bitmap[i:i + 16]) + ",")
    if not bitmap:
        out.append("    0x00,")
    out.append("};")
    out.append("")
    out.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    out.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, "
               ".ofs_y = 0},")
    for cp, index, adv_w, w, h, ox, oy in glyphs:
        out.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, "
                   ".ofs_y = %d}, /* U+%04X */" % (index, adv_w, w, h, ox, oy, cp))
    out.append("};")
    out.append("")
    first = present[0]
    out.append("static const uint16_t unicode_list_0[] = {")
    ofs = [cp - first for cp in present]
    for i in range(0, len(ofs), 8):
        out.append("    " + ", ".join("0x%x" % v for v in ofs[i:i + 8]) + ",")
    out.append("};")
    out.append("")
    out.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    out.append("    {.range_start = %d, .range_length = %d, .glyph_id_start = 1, "
               ".unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = %d, "
               ".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}," % (first, ofs[-1] + 1, len(ofs)))
    out.append("};")
    out.append("")
    out.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    out.append("    .glyph_bitmap = glyph_bitmap,")
    out.append("    .glyph_dsc = glyph_dsc,")
    out.append("    .cmaps = cmaps,")
    out.append("    .kern_dsc = NULL,")
    out.append("    .kern_scale = 0,")
    out.append("    .cmap_num = 1,")
    out.append("    .bpp = 4,")
    out.append("    .kern_classes = 0,")
    out.append("    .bitmap_format = 0,")
    out.append("};")
    out.append("")
    out.append("const lv_font_t %s = {" % name)
    out.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    out.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    out.append("    .line_height = %d," % base_line_height)
    out.append("    .base_line = %d," % base_line)
    out.append("    .subpx = LV_FONT_SUBPX_NONE,")
    out.append("    .underline_position = -2,")
    out.append("    .underline_thickness = 1,")
    out.append("    .dsc = &font_dsc,")
    out.append("    .fallback = NULL,")
    out.append("    .user_data = NULL,")
    out.append("};")
    out.append("")
    return "\n".join(out), len(present), len(bitmap)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ttf", help="TrueType font file, e.g. DejaVuSans.ttf")
    parser.add_argument("--out-dir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "main", "display", "lvgl_display",
        "fonts"))
    args = parser.parse_args()
    ttf = TrueType(args.ttf)
    os.makedirs(args.out_dir, exist_ok=True)
    # Line metrics of the math fonts match font_noto_sans_basic_20_4 / _30_4 so mixed glyphs
    # share a baseline. The icon font is drawn on its own, so it uses the font's own metrics.
    icon_scale = 40 / ttf.units_per_em
    icon_line = int(math.ceil((ttf.ascender - ttf.descender) * icon_scale))
    icon_base = int(math.ceil(-ttf.descender * icon_scale))
    math_description = "Math and science symbols for the STEM overlay."
    fonts = [
        (20, "font_stem_math_20_4", 28, 8, CODEPOINTS, math_description),
        (30, "font_stem_math_30_4", 43, 13, CODEPOINTS, math_description),
        (40, "font_stem_icons_40_4", icon_line, icon_base, ICON_CODEPOINTS, "Story-mode pictures."),
    ]
    for size, name, line_height, base_line, codepoints, description in fonts:
        source, count, nbytes = build(ttf, size, name, line_height, base_line, codepoints,
                                      description)
        path = os.path.join(args.out_dir, name + ".c")
        with open(path, "w") as f:
            f.write(source)
        print("%s: %d glyphs, %d bitmap bytes" % (os.path.relpath(path), count, nbytes))


if __name__ == "__main__":
    main()

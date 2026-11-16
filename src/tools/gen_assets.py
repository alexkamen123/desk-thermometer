#!/usr/bin/env python3
"""把 config/messages.json 里的文案和大号数字预渲染成 1-bit 点阵，生成固件头文件 + 预览图。

为什么预渲染：板载字库只有极少数中文字，而文案要随便改、字体要能换。
在电脑上用任意 TTF/TTC 渲染成位图，固件只负责贴图，就不受字库限制。

用法：python3 src/tools/gen_assets.py
产出：src/desk_thermometer/assets_gen.h、out/preview_<cold|ok|warm>.png
"""
import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
CFG = ROOT / "config" / "messages.json"
HEADER = ROOT / "src" / "desk_thermometer" / "assets_gen.h"
OUT = ROOT / "out"

PANEL_W, PANEL_H = 800, 480
DIGITS = "0123456789.-"
STATES = ["cold", "ok", "warm"]

# 版面常量（固件与预览共用，写进头文件，保证两边一致）
MARGIN = 24
NUM_TOP = 20
DIVIDER_GAP = 16
LINE_GAP = 10
FOOTER_BOTTOM = 14
UNIT_GAP = 8


def ink_bbox(img):
    """黑色像素外接框；全白返回 None。"""
    return img.point(lambda p: 255 if p < 128 else 0).getbbox()


def to_1bit(img):
    # 先二值化再转 1-bit，避免抖动；用 0 而非 Image.Dither.NONE 以兼容旧版 Pillow
    return img.point(lambda p: 0 if p < 128 else 255).convert("1", dither=0)


def render_text(text, font):
    """渲染一行文字并裁到墨迹框。"""
    l, t, r, b = font.getbbox(text)
    img = Image.new("L", (r - l + 4, b - t + 4), 255)
    ImageDraw.Draw(img).text((2 - l, 2 - t), text, font=font, fill=0)
    box = ink_bbox(img)
    if box is None:
        raise SystemExit(f"文字渲染为空（字体缺字？）：{text!r}")
    return to_1bit(img.crop(box))


def render_glyph_set(chars, font):
    """数字字形共用同一垂直范围，保证基线对齐。"""
    asc, desc = font.getmetrics()
    imgs = {}
    for ch in chars:
        img = Image.new("L", (font.size * 2, asc + desc), 255)
        ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=0)
        imgs[ch] = img
    top = min(ink_bbox(i)[1] for i in imgs.values())
    bottom = max(ink_bbox(i)[3] for i in imgs.values())
    out = {}
    for ch, img in imgs.items():
        x0, _, x1, _ = ink_bbox(img)
        out[ch] = to_1bit(img.crop((x0, top, x1, bottom)))
    return out


def pack(img):
    """行优先、MSB 在前、1=黑。"""
    w, h = img.size
    row_bytes = (w + 7) // 8
    data = bytearray(row_bytes * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if px[x, y] == 0:
                data[y * row_bytes + x // 8] |= 0x80 >> (x % 8)
    return data


def unpack(data, w, h):
    """pack 的逆运算，用于自检。"""
    img = Image.new("1", (w, h), 255)
    row_bytes = (w + 7) // 8
    px = img.load()
    for y in range(h):
        for x in range(w):
            if data[y * row_bytes + x // 8] & (0x80 >> (x % 8)):
                px[x, y] = 0
    return img


def c_array(name, data):
    body = ",".join(f"0x{b:02x}" for b in data)
    return f"static const uint8_t {name}[] = {{{body}}};\n"


def logical_size(rotation):
    return (PANEL_W, PANEL_H) if rotation in (0, 180) else (PANEL_H, PANEL_W)


def draw_builtin_status(canvas, text, x, y):
    """Use the exact firmware Font16 bitmap, not a substitute desktop font."""
    source = (ROOT / "src/desk_thermometer/font16.cpp").read_text()
    table = re.search(r"Font16_Table\[\]\s*=\s*\{(.*?)\};", source, re.S).group(1)
    data = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", table))
    for ch in text:
        offset = (ord(ch) - 32) * 32
        canvas.paste(unpack(data[offset:offset + 32], 11, 16), (x, y))
        x += 11


def compose_preview(state, temp, glyphs, unit, lines, footer, size):
    """与固件 drawScreen() 同一套版面规则。"""
    W, H = size
    canvas = Image.new("1", (W, H), 255)
    draw_builtin_status(canvas, "11-14 01:00", MARGIN, 4)
    draw_builtin_status(canvas, "RH50%", W - MARGIN - 12 * 7, 4)
    text = f"{temp:.1f}"
    num_w = sum(glyphs[c].width for c in text) + UNIT_GAP + unit.width
    x = (W - num_w) // 2
    for c in text:
        canvas.paste(glyphs[c], (x, NUM_TOP))
        x += glyphs[c].width
    canvas.paste(unit, (x + UNIT_GAP, NUM_TOP))
    div_y = NUM_TOP + glyphs["0"].height + DIVIDER_GAP
    ImageDraw.Draw(canvas).rectangle((MARGIN, div_y, W - MARGIN, div_y + 2), fill=0)
    y = div_y + DIVIDER_GAP + 4
    for ln in lines:
        canvas.paste(ln, ((W - ln.width) // 2, y))
        y += ln.height + LINE_GAP
    footer_y = H - FOOTER_BOTTOM - footer.height
    canvas.paste(footer, ((W - footer.width) // 2, footer_y))
    overflow = y - LINE_GAP > footer_y - 6
    return canvas, overflow


def main():
    cfg = json.loads(CFG.read_text(encoding="utf-8"))
    font_path = cfg["font"]
    if not Path(font_path).exists():
        raise SystemExit(f"字体不存在：{font_path}")
    if not cfg["ok_low_c"] < cfg["target_c"] < cfg["ok_high_c"]:
        raise SystemExit("需要 ok_low_c < target_c < ok_high_c")
    f_digit = ImageFont.truetype(font_path, cfg["digit_px"])
    f_unit = ImageFont.truetype(font_path, cfg["digit_px"] // 2)
    f_msg = ImageFont.truetype(font_path, cfg["message_px"])
    f_foot = ImageFont.truetype(font_path, cfg["footer_px"])

    glyphs = render_glyph_set(DIGITS, f_digit)
    unit = render_text("℃", f_unit)
    footer = render_text(cfg["footer"], f_foot)
    msgs = {}
    for s in STATES:
        lines = cfg["states"][s]
        if not 1 <= len(lines) <= 3:
            raise SystemExit(f"{s} 文案需要 1–3 行，现在是 {len(lines)} 行")
        msgs[s] = [render_text(t, f_msg) for t in lines]

    W, H = logical_size(cfg["rotation"])
    for s in STATES:
        widest = max(i.width for i in msgs[s] + [footer])
        if widest > W - 2 * MARGIN:
            raise SystemExit(f"{s} 有一行太宽（{widest}px > {W - 2 * MARGIN}px），请缩短或调小字号")

    # 自检：打包再解包必须逐像素一致（防止位序写反）
    for img in [glyphs["8"], unit, footer] + msgs["cold"]:
        if [v > 0 for v in unpack(pack(img), *img.size).getdata()] != [v > 0 for v in img.getdata()]:
            raise SystemExit("pack/unpack 自检失败")

    out = ["// 自动生成，勿手改：python3 src/tools/gen_assets.py\n",
           "#pragma once\n#include <stdint.h>\n\n",
           "typedef struct { uint16_t w, h; const uint8_t *data; } Bitmap;\n\n",
           f"#define CFG_TARGET_C {cfg['target_c']:.2f}f\n",
           f"#define CFG_OK_LOW_C {cfg['ok_low_c']:.2f}f\n",
           f"#define CFG_OK_HIGH_C {cfg['ok_high_c']:.2f}f\n",
           f"#define CFG_TEMP_OFFSET_C {cfg['temp_offset_c']:.2f}f\n",
           f"#define CFG_UPDATE_MINUTES {int(cfg['update_minutes'])}\n",
           f"#define CFG_ROTATION {int(cfg['rotation'])}\n",
           f"#define LAYOUT_MARGIN {MARGIN}\n#define LAYOUT_NUM_TOP {NUM_TOP}\n",
           f"#define LAYOUT_DIVIDER_GAP {DIVIDER_GAP}\n#define LAYOUT_LINE_GAP {LINE_GAP}\n",
           f"#define LAYOUT_FOOTER_BOTTOM {FOOTER_BOTTOM}\n#define LAYOUT_UNIT_GAP {UNIT_GAP}\n\n"]
    for i, c in enumerate(DIGITS):
        out.append(c_array(f"g_digit_{i}", pack(glyphs[c])))
    out.append("static const char DIGIT_CHARS[] = \"" + DIGITS + "\";\n")
    out.append("static const Bitmap DIGIT_GLYPHS[] = {" + ",".join(
        f"{{{glyphs[c].width},{glyphs[c].height},g_digit_{i}}}" for i, c in enumerate(DIGITS)) + "};\n\n")
    out.append(c_array("g_unit", pack(unit)))
    out.append(f"static const Bitmap UNIT_GLYPH = {{{unit.width},{unit.height},g_unit}};\n")
    out.append(c_array("g_footer", pack(footer)))
    out.append(f"static const Bitmap FOOTER = {{{footer.width},{footer.height},g_footer}};\n\n")
    for s in STATES:
        for li, img in enumerate(msgs[s]):
            out.append(c_array(f"g_msg_{s}_{li}", pack(img)))
        out.append(f"static const Bitmap MSG_{s.upper()}[] = {{" + ",".join(
            f"{{{img.width},{img.height},g_msg_{s}_{li}}}" for li, img in enumerate(msgs[s])) + "};\n")
        out.append(f"#define MSG_{s.upper()}_LINES {len(msgs[s])}\n\n")
    HEADER.write_text("".join(out), encoding="utf-8")

    OUT.mkdir(exist_ok=True)
    samples = {"cold": cfg["ok_low_c"] - 1.3, "ok": cfg["target_c"], "warm": cfg["ok_high_c"] + 0.8}
    bad = []
    for s in STATES:
        img, overflow = compose_preview(s, samples[s], glyphs, unit, msgs[s], footer, (W, H))
        img.save(OUT / f"preview_{s}.png")
        if overflow:
            bad.append(s)
    if bad:
        raise SystemExit(f"文案压到页脚：{bad}，请减少行数或调小 message_px/digit_px")
    print(f"OK {W}x{H} header={HEADER.stat().st_size}B digit_h={glyphs['0'].height} previews=out/preview_*.png")


if __name__ == "__main__":
    main()

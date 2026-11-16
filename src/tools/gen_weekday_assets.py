#!/usr/bin/env python3
"""Encode approved screen artwork for the MCU; live-value regions are cleared by firmware."""
from pathlib import Path
import json
from PIL import Image,ImageFont
from gen_assets import pack,unpack,c_array,render_text,render_glyph_set,DIGITS
ROOT=Path(__file__).resolve().parents[2]
files=sorted((ROOT/'out/工作日五款_2026-11-14').glob('*.png'))
assert len(files)==5
out=['#pragma once\n#include "assets_gen.h"\n']
for i,p in enumerate(files):
 im=Image.open(p).convert('L').resize((800,480),Image.Resampling.LANCZOS).point(lambda v:255 if v>=160 else 0).convert('1')
 data=pack(im);assert unpack(data,800,480).tobytes()==im.tobytes()
 out.append(c_array(f'weekday_bg_{i}',data))
out.append('static const Bitmap WEEKDAY_BACKGROUNDS[]={'+','.join(f'{{800,480,weekday_bg_{i}}}' for i in range(5))+'};\n')
font='/System/Library/Fonts/STHeiti Medium.ttc'
for i,label in enumerate(['周日','周一','周二','周三','周四','周五','周六']):
 im=render_text(label,ImageFont.truetype(font,22));out.append(c_array(f'weekday_label_{i}',pack(im)));out.append(f'static const Bitmap DAY_{i}={{{im.width},{im.height},weekday_label_{i}}};\n')
out.append('static const Bitmap WEEKDAY_LABELS[]={DAY_0,DAY_1,DAY_2,DAY_3,DAY_4,DAY_5,DAY_6};\n')
glyphs=render_glyph_set(DIGITS,ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Bold.ttf',180))
# Tight ink crops made the decimal point touch a preceding 2 on real e-paper.
dot=glyphs['.']; padded=Image.new('1',(dot.width+32,dot.height),1);padded.paste(dot,(16,0));glyphs['.']=padded
for i,c in enumerate(DIGITS):out.append(c_array(f'live_digit_{i}',pack(glyphs[c])))
out.append('static const Bitmap LIVE_DIGITS[]={'+','.join(f'{{{glyphs[c].width},{glyphs[c].height},live_digit_{i}}}' for i,c in enumerate(DIGITS))+'};\n')
im=render_text('°C',ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Bold.ttf',90))
out.append(c_array('live_unit',pack(im)));out.append(f'static const Bitmap LIVE_UNIT={{{im.width},{im.height},live_unit}};\n')
cfg=json.loads((ROOT/'config/messages.json').read_text())
out.append(f"#define CFG_REQUEST_MIN_C {cfg['request_min_c']:.2f}f\n")
for name,word,size in [('REQUEST_LINE',cfg['request_message'],30),('REQUEST_FOOTER',cfg['footer'],22)]:
 im=render_text(word,ImageFont.truetype(cfg['font'],size))
 assert im.width <= (480 if name=='REQUEST_LINE' else 752)
 out.append(c_array(name+'_data',pack(im)))
 out.append(f'static const Bitmap {name}={{{im.width},{im.height},{name}_data}};\n')
(ROOT/'src/desk_thermometer/weekday_assets_gen.h').write_text(''.join(out))
print('PASS 5/5 framebuffer pack/unpack; weekday labels and live digits encoded')

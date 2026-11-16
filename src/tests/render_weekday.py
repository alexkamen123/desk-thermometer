from pathlib import Path
import subprocess,tempfile,re
from PIL import Image
ROOT=Path(__file__).resolve().parents[2];SRC=ROOT/'src/desk_thermometer';OUT=ROOT/'out/工作日实机渲染';OUT.mkdir(exist_ok=True)
s=(SRC/'desk_thermometer.ino').read_text()
funcs='\n'.join(re.search(r'static '+sig+r'\(.*?\n}',s,re.S).group() for sig in ['void blit','void drawScreen'])
with tempfile.TemporaryDirectory() as td:
 p=Path(td)
 (p/'Arduino.h').write_text('#pragma once\n#include <stdint.h>\n#include <cstdio>\n#include <cstdlib>\nstruct Console { void print(const char*s){fprintf(stderr,"%s",s);} template<class...T>void printf(const char*,T...){} }; inline Console Serial;\n')
 (p/'Wire.h').write_text('#pragma once\n')
 pre='#include <cassert>\n#include <cmath>\n#include <cstring>\n#include "GUI_Paint.h"\n#include "EPD_3in97.h"\n#include "weekday_assets_gen.h"\nunsigned g_coverIndex=0,g_weekday=1;bool g_clockValid=true; char g_updated[32]="11-16 09:00";uint8_t frame[48000];UBYTE*g_frame=frame;enum State{ST_COLD,ST_OK,ST_WARM};\n'
 main='''int main(int argc,char**argv){const Bitmap &dot=LIVE_DIGITS[10];assert(dot.w>32);for(int y=0;y<dot.h;++y)for(int x=0;x<dot.w;++x)if(x<16||x>=dot.w-16)assert(!(dot.data[y*((dot.w+7)/8)+x/8]&(0x80>>(x%8))));g_coverIndex=atoi(argv[1]);g_weekday=g_coverIndex+1;drawScreen(atof(argv[2]),47,ST_OK);FILE*f=fopen(argv[3],"wb");fprintf(f,"P4\\n800 480\\n");for(auto b:frame)fputc(b^255,f);fclose(f);return 0;}'''
 (p/'test.cpp').write_text(pre+funcs+main)
 subprocess.run(['clang++','-std=c++17','-I'+str(p),'-I'+str(SRC),str(p/'test.cpp'),str(SRC/'GUI_Paint.cpp'),*[str(x) for x in SRC.glob('font*.cpp')],'-o',str(p/'test')],check=True)
 for day in range(5):
  for temp in ['25.4','25.49','25.5','25.8','27.6','-25.0','32.5','22.2']:
   out=p/f'{day}-{temp}.pbm';r=subprocess.run([str(p/'test'),str(day),temp,str(out)],capture_output=True,text=True);assert r.returncode==0 and not r.stderr,r.stderr
   im=Image.open(out);assert im.size==(800,480)
   im.save(OUT/f'{day+1:02d}_{temp}.png')
   request_area=im.crop((290,290,800,426))
   if float(temp)<25.5: assert request_area.convert('L').getextrema()==(255,255),f'day{day} {temp}: request text must be blank'
   else: assert request_area.convert('L').getextrema()[0]==0, f'day{day} {temp}: request text missing'
  a=Image.open(OUT/f'{day+1:02d}_25.8.png');b=Image.open(OUT/f'{day+1:02d}_27.6.png');assert a.tobytes()!=b.tobytes()
 print('PASS actual firmware renderer: 5 themes x 8 temperatures; no out-of-bounds warnings; live values differ')

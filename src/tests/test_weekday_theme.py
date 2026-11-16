"""Compare actual C++ date selector against Python Gregorian calendar."""
from pathlib import Path
from datetime import date,timedelta
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as td:
 p=Path(td); body=['#include "weekday_theme.h"\nint main(){ int failures=0;']
 day=date(2026,1,1); count=0
 while day<date(2031,1,1):
  expected=min(day.weekday(),4)
  body.append(f'if(coverForDate({{true,{day.year},{day.month},{day.day},0,0}})!={expected}) failures++;')
  day+=timedelta(days=1);count+=1
 body+=['if(coverForDate({false,2026,11,14,0,0})!=0) failures++;','if(coverForDate({true,2026,13,1,0,0})!=0) failures++;','return failures?1:0;}']
 (p/'test.cpp').write_text('\n'.join(body))
 subprocess.run(['clang++','-std=c++17','-I'+str(root/'src/desk_thermometer'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 print(f'PASS {count} dates incl. leap day/month/year rollover + 2 invalid-clock cases')

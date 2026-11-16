from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=r'''
#include "portal_button.h"
#include <cassert>
int main(){
 PortalExitButton b;
 assert(!b.update(true,0)); assert(!b.update(true,3000));
 assert(!b.update(false,3100));assert(!b.update(false,3150));
 assert(!b.update(false,4000));
 assert(!b.update(true,4100));assert(!b.update(false,4110));assert(!b.update(false,4160));
 assert(!b.update(true,4200));assert(!b.update(true,4250));
 assert(!b.update(false,4300));assert(b.update(false,4350));assert(!b.update(false,4400));
 PortalExitButton cold;assert(!cold.update(false,0));assert(!cold.update(false,50));
 assert(!cold.update(true,100));assert(!cold.update(true,150));assert(!cold.update(false,200));assert(cold.update(false,250));
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(source)
 subprocess.run(['clang++','-std=c++17','-I'+str(root/'src/desk_thermometer'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS entry long hold/release ignored; bounce rejected; fresh press/release exits once; cold portal works')

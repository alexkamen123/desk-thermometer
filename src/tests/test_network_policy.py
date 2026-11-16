from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include "network_policy.h"
#include <cassert>
int main(){
 assert(validCredentials("office", "12345678"));
 assert(!validCredentials("", "12345678"));
 assert(!validCredentials("office", "short"));
 assert(!validCredentials("123456789012345678901234567890123", "1234567"));
 assert(!validCredentials("12345678901234567890123456789012345", "12345678"));
 assert(!validCredentials("office", ""));
 assert(shouldSync(true,false,0));
 assert(!shouldSync(false,false,0));
 assert(!shouldSync(true,false,1));
 assert(shouldSync(true,false,20));
 assert(!shouldSync(true,true,20));
 assert(shouldSync(true,true,480));
 assert(validEpoch(1794837600LL));
 assert(!validEpoch(0)); assert(!validEpoch(99999999999LL));
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td); (p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++17','-I'+str(root/'src/desk_thermometer'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS network credentials, retry limits, epoch boundaries')

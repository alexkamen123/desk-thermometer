"""Execute production RTC write/readback against a fake I2C register bank."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'src/desk_thermometer/network_setup.cpp').read_text()
s=s[s.index('static uint8_t bcd'):s.index('static bool connectNetwork')]
code=r'''
#include "network_policy.h"
#include "clock_status.h"
#include <ctime>
#include <cassert>
#include <cstring>
#define ESP_OK 0
int rtc_dev_handle=0, fault=0, writes=0;
uint8_t regs[16]={};
struct Log { template<class... T> void printf(const char*, T...){} } Serial;
int i2c_read_buff(int,int reg,uint8_t *out,int len){
 if(fault==1 || (fault==3 && writes>0))return -1;
 memcpy(out,regs+reg,len);if(fault==4 && reg==4)out[0]|=128;return 0;
}
int i2c_write_buff(int,int reg,uint8_t *in,int len){
 if(fault==2)return -1;
 writes++;memcpy(regs+reg,in,len);return 0;
}
'''+s+r'''
int main(){
 regs[0]=0x22;regs[4]=128;
 assert(writeClock(1794837600));assert((regs[0]&0x22)==0);assert(!(regs[4]&128));
 auto stamp=decodeClock(regs[0],regs+4);assert(stamp.valid && stamp.year==2026);
 writes=0;assert(!writeClock(0));assert(writes==0);
 for(int f=1;f<=4;f++){fault=f;writes=0;assert(!writeClock(1794837600));}
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++17','-I'+str(root/'src/desk_thermometer'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual RTC writer: valid write, oscillator flags, invalid epoch, I2C read/write/readback failures')

#!/usr/bin/env python3
"""Host regression: compile actual sensor methods and classification, fake only I2C."""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'src/desk_thermometer'
header = (SRC / 'i2c_equipment.h').read_text()
header = header[header.index('typedef enum{'):].replace('#endif', '')
args = argparse.ArgumentParser()
args.add_argument('--sensor-ref', help='Git revision for negative control of the actual old sensor driver')
options = args.parse_args()
driver = (subprocess.check_output(['git', '-C', str(ROOT), 'show', options.sensor_ref + ':src/desk_thermometer/i2c_equipment.cpp'], text=True)
          if options.sensor_ref else (SRC / 'i2c_equipment.cpp').read_text())
driver = driver[driver.index('i2c_equipment_shtc3::i2c_equipment_shtc3()'):]
ino = (SRC / 'desk_thermometer.ino').read_text()
busy = re.search(r'static void EPD_3IN97_ReadBusy\(.*?\n}', (SRC/'EPD_3in97.cpp').read_text(), re.S).group()
turn_on = re.search(r'static void EPD_3IN97_TurnOnDisplay\(.*?\n}', (SRC/'EPD_3in97.cpp').read_text(), re.S).group()
power = (SRC/'display_power.h').read_text().replace('#pragma once', '')
clock = (SRC/'clock_status.h').read_text().replace('#pragma once', '')
sleeping = re.search(r'static void goToSleep\(.*?\n}', ino, re.S).group()
sampling = re.search(r'static bool readTemperature\(.*?\n}', ino, re.S).group()
classification = re.search(r'static State classify\(.*?\n}', ino, re.S).group()
state_enum = re.search(r'enum State[^;]+;', ino).group()
cfg = json.loads((ROOT / 'config/messages.json').read_text())
preamble = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(int) {}
unsigned long fakeMillis=0;
int fakeBusy=0;
unsigned long millis() { return fakeMillis; }
void delay(int ms) { fakeMillis += ms; }
#define DEV_Delay_ms delay
#define DEV_Digital_Read(pin) (fakeBusy)
#define EPD_BUSY_PIN 3
#define Debug(...) ((void)0)
static bool epd_error=false;
void EPD_3IN97_SendCommand(uint8_t) {}
void EPD_3IN97_SendData(uint8_t) {}
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define CFG_UPDATE_MINUTES @UPDATE_MINUTES@
const int ext_wakeup_pin_1=0;
uint32_t g_wakeCount=1;
uint64_t timerUs=0;
bool externalWake=false, sleepingNow=false;
int fakeBoot=1;
struct FakeSerial { void flush() {} template<class... T> void printf(const char*,T...) {} } Serial;
void pinMode(int,int) {}
int digitalRead(int) { return fakeBoot; }
void rtc_gpio_pullup_en(int) {}
void rtc_gpio_pulldown_dis(int) {}
void esp_sleep_enable_timer_wakeup(uint64_t us) { timerUs=us; }
void esp_sleep_enable_ext0_wakeup(int,int) { externalWake=true; }
void esp_deep_sleep_start() { sleepingNow=true; }
int shtc3_handle=0;
int fault=0;
int i2c_write_buff(int,int,uint8_t*,int) { return fault==1 ? -1 : 0; }
int i2c_master_write_read_dev(int,uint8_t*,int,uint8_t* out,int) {
  out[0]=0x08; out[1]=0x07; out[2]=0x00; return -1;
}
uint8_t crc(uint8_t* p) {
  uint8_t c=255;
  for(int j=0;j<2;j++) { c^=p[j]; for(int i=0;i<8;i++) c=(c&128)?(c<<1)^0x31:c<<1; }
  return c;
}
int i2c_read_buff(int,int,uint8_t* out,int) {
  if(fault==2) return -1;
  uint8_t b[6]={0x66,0x66,0,0x80,0,0};
  b[2]=crc(b); b[5]=crc(b+3);
  if(fault==3) b[2]^=1;
  if(fault==4) b[5]^=1;
  memcpy(out,b,6); return 0;
}
int checks=0, failures=0;
void check(bool ok,const char* name) { checks++; if(!ok) { failures++; printf("FAIL %s\n",name); } }
'''
preamble = preamble.replace('@UPDATE_MINUTES@', str(cfg['update_minutes']))
main = r'''
int main() {
  i2c_equipment_shtc3 sensor;
  auto good=sensor.readTempHumi();
  check(std::isfinite(good.Temp) && std::fabs(good.RH-50)<0.01,"valid measurement");
  for(int f=1;f<=4;f++) {
    fault=f; auto bad=sensor.readTempHumi();
    check(!std::isfinite(bad.Temp) && !std::isfinite(bad.RH),"I2C/CRC fault must invalidate both values");
  }
  fault=0; auto recovered=sensor.readTempHumi();
  check(std::isfinite(recovered.Temp),"recovers after I2C error");
  float sampledT=NAN, sampledRH=NAN;
  check(readTemperature(sensor,sampledT,sampledRH) && std::fabs(sampledT-25.0f)<0.01f,"raw sensor average, offset zero");
  fault=2; sampledT=NAN; sampledRH=NAN;
  check(!readTemperature(sensor,sampledT,sampledRH),"sensor error reaches application");
  fault=0;
  check(classify(24.0f,ST_UNKNOWN)==ST_COLD,"cold first sample");
  check(classify(26.0f,ST_UNKNOWN)==ST_OK,"ok first sample");
  check(classify(28.0f,ST_UNKNOWN)==ST_WARM,"warm first sample");
  check(classify(25.5f,ST_UNKNOWN)==ST_OK,"low boundary inclusive");
  check(classify(26.5f,ST_UNKNOWN)==ST_OK,"high boundary inclusive");
  check(classify(25.6f,ST_COLD)==ST_COLD,"cold debounce");
  check(classify(25.8f,ST_COLD)==ST_OK,"cold releases");
  check(classify(25.4f,ST_OK)==ST_OK,"ok debounce low");
  check(classify(25.2f,ST_OK)==ST_COLD,"ok exits low");
  check(classify(26.6f,ST_OK)==ST_OK,"ok debounce high");
  check(classify(26.8f,ST_OK)==ST_WARM,"ok exits high");
  check(classify(26.4f,ST_WARM)==ST_WARM,"warm debounce");
  check(classify(26.2f,ST_WARM)==ST_OK,"warm releases");
  fakeBusy=0; EPD_3IN97_ReadBusy(); check(!epd_error,"display ready");
  fakeBusy=1; fakeMillis=0; EPD_3IN97_ReadBusy();
  check(epd_error && fakeMillis>=20000 && fakeMillis<20200,"stuck display times out");
  auto elapsed=fakeMillis; EPD_3IN97_ReadBusy();
  check(fakeMillis==elapsed,"latched error does not block again");
  fakeBusy=0; epd_error=false; EPD_3IN97_ReadBusy(); check(!epd_error,"display retry recovers");
  epd_error=false; fakeBusy=0; EPD_3IN97_TurnOnDisplay();
  check(epd_error,"unpowered display must not report refresh success");
  uint8_t date[]={0x30,0x15,0x01,0x14,0x06,0x11,0x26};
  check(decodeClock(0,date).valid,"valid RTC");
  check(!decodeClock(0x20,date).valid,"stopped RTC");
  date[0]=0x80; check(!decodeClock(0,date).valid,"oscillator failure invalidates time");
  date[0]=0; date[1]=0x6a; check(!decodeClock(0,date).valid,"invalid BCD");
  date[1]=0; date[3]=0x31; check(!decodeClock(0,date).valid,"November 31 rejected");
  fakeBoot=1; fakeMillis=0; goToSleep();
  check(sleepingNow && timerUs==180000000ULL && externalWake,"normal sleep enables 3min timer and button");
  fakeBoot=0; fakeMillis=0; sleepingNow=false; externalWake=false; goToSleep();
  check(sleepingNow && timerUs==180000000ULL && !externalWake && fakeMillis==2000,"held button cannot cause wake loop");
  uint8_t regs[256]={}; regs[3]=0x4a; regs[0x90]=0xab; regs[0x94]=0xe1;
  bool ack=true, dropWrites=false; int writes=0;
  auto readReg=[&](uint8_t reg,uint8_t &value) { value=regs[reg]; return ack; };
  auto writeReg=[&](uint8_t reg,uint8_t value) { writes++; if(!dropWrites) regs[reg]=value; return ack; };
  check(setDisplayRail(true,readReg,writeReg) && regs[0x90]==0xaf && regs[0x94]==0xfc,"display supply 3.3V, preserve other rails/bits");
  check(setDisplayRail(false,readReg,writeReg) && regs[0x90]==0xab && regs[0x94]==0xfc,"display supply off preserves other rails");
  regs[3]=0; writes=0;
  check(!setDisplayRail(true,readReg,writeReg) && writes==0,"wrong PMU ID must not write registers");
  regs[3]=0x4a; ack=false;
  check(!setDisplayRail(true,readReg,writeReg) && writes==0,"PMU read failure must not write registers");
  ack=true; dropWrites=true; regs[0x94]=0xe0;
  check(!setDisplayRail(true,readReg,writeReg) && regs[0x90]==0xab,"PMU acknowledgement without readback is rejected");
  printf("%d checks, %d failures\n",checks,failures);
  return failures?1:0;
}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td)
    defs=f'\n#define CFG_OK_LOW_C {cfg["ok_low_c"]}f\n#define CFG_OK_HIGH_C {cfg["ok_high_c"]}f\nstatic const float HYSTERESIS_C=0.2f;\nstatic const int SAMPLES=3;\nstatic const float VENDOR_SELF_HEAT_OFFSET=4.0f;\n#define CFG_TEMP_OFFSET_C 0.0f\n'
    (p/'test.cpp').write_text(preamble+header+driver+defs+state_enum+classification+sampling+busy+turn_on+clock+power+sleeping+main)
    subprocess.run(['clang++','-std=c++17','-ftrivial-auto-var-init=zero',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    raise SystemExit(subprocess.run([str(p/'test')]).returncode)

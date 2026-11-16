#pragma once
#include <stdint.h>

struct ClockStamp {
  bool valid;
  unsigned year, month, day, hour, minute;
};

// PCF85063 registers 0x04..0x0A. Fail closed on oscillator-stop or invalid BCD.
inline ClockStamp decodeClock(uint8_t control, const uint8_t *raw) {
  ClockStamp stamp = {};
  if ((control & 0x22) || (raw[0] & 0x80)) return stamp;
  const uint8_t masks[] = {0x7f, 0x7f, 0x3f, 0x3f, 0x07, 0x1f, 0xff};
  unsigned values[7];
  for (unsigned i=0; i<7; ++i) {
    const uint8_t b = raw[i] & masks[i];
    if ((b & 15) > 9 || (b >> 4) > 9) return stamp;
    values[i] = (b >> 4)*10 + (b & 15);
  }
  if (values[0]>59 || values[1]>59 || values[2]>23 || values[4]>6 ||
      values[5]<1 || values[5]>12 || values[6]<26) return stamp;
  const unsigned year = 2000 + values[6];
  const unsigned days[] = {31, (year%4==0 ? 29u : 28u),31,30,31,30,31,31,30,31,30,31};
  if (values[3]<1 || values[3]>days[values[5]-1]) return stamp;
  return {true, year, values[5], values[3], values[2], values[1]};
}

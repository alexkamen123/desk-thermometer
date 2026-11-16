#pragma once
#include <cstring>
#include <stdint.h>
inline bool validCredentials(const char *ssid, const char *password) {
  return ssid && password && strlen(ssid)>0 && strlen(ssid)<=32 &&
         strlen(password)>=8 && strlen(password)<=63;
}
inline bool shouldSync(bool configured, bool clockValid, uint32_t cycles) {
  return configured && (cycles == 0 || cycles >= (clockValid ? 480u : 20u));
}
inline bool validEpoch(int64_t seconds) {
  return seconds >= 1767225600LL && seconds < 4102444800LL;
}

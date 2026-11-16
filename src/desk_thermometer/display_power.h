#pragma once
#include <stdint.h>

// Waveshare 08_ESP32-S3_e-Paper-3.97: display supply is AXP2101 ALDO3.
// Only ALDO3 voltage and enable bit are changed; other rails/charging stay intact.
template<class Read, class Write>
bool setDisplayRail(bool on, Read read, Write write) {
  uint8_t id=0, voltage=0, enabled=0, actual=0;
  if (!read(0x03, id) || id != 0x4a) return false;
  if (on) {
    if (!read(0x94, voltage)) return false;
    const uint8_t desired = (voltage & 0xe0) | 28; // (3300 - 500) / 100 mV
    if (!write(0x94, desired) || !read(0x94, actual) || actual != desired) return false;
  }
  if (!read(0x90, enabled)) return false;
  const uint8_t desired = on ? (enabled | 0x04) : (enabled & ~0x04);
  return write(0x90, desired) && read(0x90, actual) && actual == desired;
}

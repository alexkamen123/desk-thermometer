#pragma once
#include <stdint.h>
// Ignore the entry hold/release; require a fresh debounced press then release.
class PortalExitButton {
  bool raw=true, stable=true, armed=false, pressed=false;
  uint32_t changed=0;
public:
  bool update(bool down,uint32_t now) {
    if(down!=raw) {raw=down;changed=now;}
    if(uint32_t(now-changed)<40) return false;
    stable=raw;
    if(!armed) {if(!stable) armed=true;return false;}
    if(stable) pressed=true;
    else if(pressed) {pressed=false;return true;}
    return false;
  }
};

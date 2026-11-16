#pragma once
#include <Arduino.h>
// showPortal is called once with the temporary hotspot credentials.
void serviceNetwork(bool forcePortal, bool coldBoot, bool clockValid,
                    void (*showPortal)(const char*, const char*));

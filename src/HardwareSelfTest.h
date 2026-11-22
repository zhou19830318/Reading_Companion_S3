#pragma once

#include <cstdint>

// Serial-triggered hardware checks for OnePage V3.1 bring-up.
// Only wired to the CMD: handler; not part of any user-facing flow.
void runHardwareSelfTest();
void runSensorReport();
void runAccelReport();
bool runAudioLoopback(uint32_t seconds = 2);
bool runMaxVolumeTest(uint32_t seconds = 3);

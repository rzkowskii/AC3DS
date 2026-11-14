#pragma once

// Handheld crash and stall reports. A CPU exception or a main loop that stops
// advancing is written to sdmc:/3ds/assaultcube/profile/crash.txt and shown on
// the touch screen. Addresses can be resolved with scripts/symbolize-crash.py.

// Installs the exception handler for the calling thread. Call once per thread.
void ac3ds_crash_install(const char *thread_name);
// Starts the stall watchdog. The main loop reports progress with ac3ds_crash_tick.
void ac3ds_crash_start_watchdog();
void ac3ds_crash_stop_watchdog();
void ac3ds_crash_tick();

// The last main-loop phase entered. Must point at a string literal.
extern const char *volatile ac3ds_crash_phase;
#define AC3DS_PHASE(name) (ac3ds_crash_phase = (name))

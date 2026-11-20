#pragma once

#include <stddef.h>

// Handheld crash and stall reports. A CPU exception is written to
// sdmc:/3ds/assaultcube/profile/crash.txt and a main loop that stops advancing
// to stall.txt; both are shown on the touch screen. Addresses can be resolved with scripts/symbolize-crash.py.

// Installs the exception handler for the calling thread. Call once per thread.
void ac3ds_crash_install(const char *thread_name);
// Releases the calling thread's handler slot. Call before the thread exits.
void ac3ds_crash_uninstall();
// Pauses the stall watchdog while the app is suspended or blocked in a system
// applet (HOME menu, sleep, software keyboard).
void ac3ds_crash_pause(bool paused);
// Starts the stall watchdog. The main loop reports progress with ac3ds_crash_tick.
void ac3ds_crash_start_watchdog();
void ac3ds_crash_stop_watchdog();
void ac3ds_crash_tick();
// One line of heap and linear-heap usage for the client log.
void ac3ds_memory_line(char *out, size_t size);
// Context a crash or stall report repeats: the map being played, and the last
// lines written to the client log (the log file itself is flushed rarely).
void ac3ds_crash_set_map(const char *name);
size_t ac3ds_heap_unclaimed();
void ac3ds_crash_note_log(const char *line);

// The last main-loop phase entered. Must point at a string literal.
extern const char *volatile ac3ds_crash_phase;
#define AC3DS_PHASE(name) (ac3ds_crash_phase = (name))

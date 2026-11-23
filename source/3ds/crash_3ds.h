#pragma once

#include <atomic>
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
// Record an infrequent lifecycle event with the current main-loop phase.
// The event name, like a phase, must point at a string literal.
void ac3ds_crash_note_event(const char *name, int detail = 0);

// Only the game thread and its synchronous callbacks change this phase.
// Worker threads and crash handlers may read it without taking a lock.
extern std::atomic<const char *> ac3ds_crash_phase;
// The load allowance remains active when a scoped phase names a GPU wait.
extern std::atomic<bool> ac3ds_crash_loading_map;
#define AC3DS_PHASE(name) (ac3ds_crash_phase.store((name), std::memory_order_release))

class Ac3dsPhaseScope
{
    const char *const parent;

public:
    explicit Ac3dsPhaseScope(const char *phase) noexcept
        : parent(ac3ds_crash_phase.load(std::memory_order_relaxed))
    {
        AC3DS_PHASE(phase);
    }
    ~Ac3dsPhaseScope() { AC3DS_PHASE(parent); }
    Ac3dsPhaseScope(const Ac3dsPhaseScope &) = delete;
    Ac3dsPhaseScope &operator=(const Ac3dsPhaseScope &) = delete;
};

// Crash and stall reporting for handheld runs where no debugger is attached.
//
// The exception handler avoids the C library's file layer and allocator, since
// the fault may have happened while one of their locks was held. It formats
// into static buffers and writes through FS:USER directly.

#include "crash_3ds.h"
#include "system.h"

#include <3ds.h>
#include <malloc.h>
#include <new>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>

const char *volatile ac3ds_crash_phase = "startup";

// Markers in .text, .data and .bss with unmangled names. Their run-time
// addresses go into every crash and fatal-error report, so
// scripts/symbolize-crash.py can check that the ELF is the one that crashed
// and that the loader placed the image where it was linked.
extern "C" {
__attribute__((used, noinline)) void ac3ds_image_text_marker() {}
__attribute__((used)) char ac3ds_image_data_marker = 1;
__attribute__((used)) char ac3ds_image_bss_marker;
extern u32 __stacksize__;
extern unsigned long __malloc_max_sbrked_mem; // newlib: the most the arena has ever been; free()'s trim does not lower it
extern struct mallinfo __malloc_current_mallinfo; // newlib keeps .arena current; the other fields only after mallinfo()
}
void ac3ds_smallpool_stats(unsigned *usedKb, unsigned *capacityKb, unsigned *blocks, unsigned *full);
unsigned ac3ds_smallpool_peak_kb();
unsigned ac3ds_alloc_last_failed_bytes(unsigned *frame);

// libctru's heap size includes the main thread's stack, which sits in the
// first __stacksize__ bytes of the heap mapping.
static u32 heapLimit() { const u32 size = envGetHeapSize(); return size > __stacksize__ ? size - __stacksize__ : size; }

namespace
{
constexpr const char *crashPath = "/3ds/assaultcube/profile/crash.txt";
// Stall reports go to their own file so a false positive never replaces a
// real crash report.
constexpr const char *stallPath = "/3ds/assaultcube/profile/stall.txt";
constexpr int handlerStackBytes = 16 * 1024;
constexpr int maxThreads = 6;

alignas(8) u8 handlerStacks[maxThreads][handlerStackBytes];
const char *threadNames[maxThreads] = {};
std::atomic<bool> slotUsed[maxThreads];
std::atomic<bool> reporting{false};
std::atomic<u32> mainTicks{0};

// Sampled on the main thread every 512 frames, read by the handlers: a
// report must not call the allocator.
struct StateSnapshot
{
    char map[48];
    u32 heapUsedKb, heapArenaKb, heapLimitKb, linearFreeKb, tick;
};
StateSnapshot snapshot = {};
constexpr int logRingLines = 8, logRingWidth = 96;
char logRing[logRingLines][logRingWidth] = {};
std::atomic<u32> logRingNext{0};


// One report buffer per writer: the exception handler and the watchdog thread.
struct ReportBuffer
{
    char text[4096];
    int length = 0;
};
ReportBuffer crashReport, stallReport;

void append(ReportBuffer &r, const char *format, ...)
{
    if(r.length >= int(sizeof(r.text)) - 1) return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(r.text + r.length, sizeof(r.text) - r.length, format, args);
    va_end(args);
    if(n > 0) r.length = r.length + n < int(sizeof(r.text)) ? r.length + n : int(sizeof(r.text)) - 1;
}

void appendContext(ReportBuffer &r)
{
    append(r, "state: map %s, heap %lu KB in use, arena %lu of %lu KB, linear free %lu KB (sampled at frame %lu)\n",
           snapshot.map[0] ? snapshot.map : "(none)", snapshot.heapUsedKb, snapshot.heapArenaKb, snapshot.heapLimitKb,
           snapshot.linearFreeKb, snapshot.tick);
    unsigned poolKb = 0, poolCapacityKb = 0, poolBlocks = 0, poolFull = 0;
    ac3ds_smallpool_stats(&poolKb, &poolCapacityKb, &poolBlocks, &poolFull);
    append(r, "small pool %u of %u KB (most %u), %u blocks, %u refused\n", poolKb, poolCapacityKb, ac3ds_smallpool_peak_kb(), poolBlocks, poolFull);
    const u32 next = logRingNext.load(std::memory_order_relaxed);
    for(int i = 0; i < logRingLines; ++i)
    {
        const char *line = logRing[(next + i) % logRingLines];
        if(line[0]) append(r, "log: %.*s\n", logRingWidth - 1, line);
    }
}

void writeFile(const char *where, const ReportBuffer &r)
{
    Handle file = 0;
    const FS_Path empty = fsMakePath(PATH_EMPTY, "");
    const FS_Path path = fsMakePath(PATH_ASCII, where);
    if(R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, empty, path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) return;
    FSFILE_SetSize(file, 0);
    u32 written = 0;
    FSFILE_Write(file, &written, 0, r.text, u32(r.length), FS_WRITE_FLUSH);
    FSFILE_Close(file);
}

bool readable(u32 address, u32 bytes, u32 *regionEnd = nullptr)
{
    MemInfo info;
    PageInfo page;
    if(R_FAILED(svcQueryMemory(&info, &page, address))) return false;
    if(!(info.perm & MEMPERM_READ) || info.state == MEMSTATE_FREE) return false;
    if(regionEnd) *regionEnd = info.base_addr + info.size;
    return address + bytes <= info.base_addr + info.size;
}

bool inCode(u32 value, u32 codeBase, u32 codeEnd)
{
    return value >= codeBase && value < codeEnd;
}

void showOnTouchScreen(const char *title, const char *const *lines, int count, const char *file = "        crash.txt")
{
    const char *all[16];
    int n = 0;
    all[n++] = title;
    for(int i = 0; i < count && n < 16; ++i) all[n++] = lines[i];
    if(n < 16) all[n++] = "";
    if(n < 16) all[n++] = "Report: /3ds/assaultcube/profile/";
    if(n < 16) all[n++] = file;
    if(n < 16) all[n++] = "Hold POWER to switch off.";
    ac3ds_show_startup_report(all, n);
}

const char *exceptionName(ERRF_ExceptionType type)
{
    switch(type)
    {
        case ERRF_EXCEPTION_PREFETCH_ABORT: return "prefetch abort";
        case ERRF_EXCEPTION_DATA_ABORT: return "data abort";
        case ERRF_EXCEPTION_UNDEFINED: return "undefined instruction";
        case ERRF_EXCEPTION_VFP: return "VFP exception";
    }
    return "unknown";
}

const char *currentThreadName()
{
    const u8 *tls = static_cast<const u8 *>(getThreadLocalStorage());
    const u32 stack = *reinterpret_cast<const u32 *>(tls + 0x44);
    for(int i = 0; i < maxThreads; ++i)
        if(stack == u32(uintptr_t(handlerStacks[i] + handlerStackBytes))) return threadNames[i];
    return "unknown";
}

[[noreturn]] void park()
{
    for(;;) svcSleepThread(1000000000LL);
}

// Words on a stack that point into the code region. The first few are
// usually the caller chain.
void appendStack(ReportBuffer &r, u32 from, u32 codeBase, u32 codeEnd)
{
    append(r, "stack return candidates:\n");
    u32 stackEnd = 0;
    int found = 0;
    from &= ~3u;
    if(!readable(from, 4, &stackEnd)) return;
    const u32 *sp = reinterpret_cast<const u32 *>(from);
    const u32 limit = (stackEnd - from) / 4;
    for(u32 i = 0; i < limit && i < 2048 && found < 32; ++i)
    {
        const u32 value = sp[i];
        if(!inCode(value & ~1u, codeBase, codeEnd)) continue;
        append(r, "  sp+%03lx %08lx\n", i * 4, value);
        ++found;
    }
}

[[noreturn]] void onException(ERRF_ExceptionInfo *excep, CpuRegisters *regs)
{
    if(reporting.exchange(true)) park();
    MemInfo codeInfo;
    PageInfo page;
    u32 codeBase = 0x00100000, codeEnd = 0x00100000;
    if(R_SUCCEEDED(svcQueryMemory(&codeInfo, &page, 0x00100000)))
        codeEnd = codeInfo.base_addr + codeInfo.size;

    crashReport.length = 0;
    append(crashReport, "AssaultCube 3DS build %d crash report\n", AC3DS_BUILD_NUMBER);
    append(crashReport, "thread: %s\n", currentThreadName());
    append(crashReport, "phase: %s\n", ac3ds_crash_phase ? ac3ds_crash_phase : "?");
    append(crashReport, "exception: %s (%d)\n", exceptionName(excep->type), int(excep->type));
    append(crashReport, "pc=%08lx lr=%08lx sp=%08lx cpsr=%08lx\n", regs->pc, regs->lr, regs->sp, regs->cpsr);
    append(crashReport, "far=%08lx fsr=%08lx fpexc=%08lx fpinst=%08lx\n", excep->far, excep->fsr, excep->fpexc, excep->fpinst);
    for(int i = 0; i < 13; i += 4)
    {
        append(crashReport, "r%-2d=%08lx", i, regs->r[i]);
        for(int j = i + 1; j < i + 4 && j < 13; ++j) append(crashReport, " r%-2d=%08lx", j, regs->r[j]);
        append(crashReport, "\n");
    }
    append(crashReport, "code=%08lx-%08lx\n", codeBase, codeEnd);
    append(crashReport, "image: text=%08lx data=%08lx bss=%08lx frame=%lu\n", u32(uintptr_t(&ac3ds_image_text_marker)),
           u32(uintptr_t(&ac3ds_image_data_marker)), u32(uintptr_t(&ac3ds_image_bss_marker)),
           u32(mainTicks.load(std::memory_order_relaxed)));
    // The instruction words around the PC, when the code is readable.
    const u32 pc = regs->pc & ~3u;
    if(readable(pc - 8, 20))
    {
        const u32 *words = reinterpret_cast<const u32 *>(pc - 8);
        append(crashReport, "code@pc-8: %08lx %08lx [%08lx] %08lx %08lx\n", words[0], words[1], words[2], words[3], words[4]);
    }
    appendStack(crashReport, regs->sp, codeBase, codeEnd);
    appendContext(crashReport);
    writeFile(crashPath, crashReport);

    char l1[48], l2[48], l3[48], l4[48], l5[48];
    snprintf(l1, sizeof(l1), "%s in %s", exceptionName(excep->type), currentThreadName());
    snprintf(l2, sizeof(l2), "phase %s", ac3ds_crash_phase ? ac3ds_crash_phase : "?");
    snprintf(l3, sizeof(l3), "pc  %08lx  lr  %08lx", regs->pc, regs->lr);
    snprintf(l4, sizeof(l4), "far %08lx  fsr %08lx", excep->far, excep->fsr);
    snprintf(l5, sizeof(l5), "sp  %08lx", regs->sp);
    const char *lines[] = { l1, l2, l3, l4, l5 };
    showOnTouchScreen("AssaultCube crashed", lines, 5);
    park();
}

// Stall watchdog: if the main loop stops advancing, record where it was.
// Paused while the app is suspended (HOME menu, sleep, software keyboard),
// when the main loop legitimately stops.
Thread watchdogThread = nullptr;
std::atomic<bool> watchdogStop{false};
std::atomic<bool> watchdogPaused{false};
LightEvent watchdogWake;

void watchdogMain(void *)
{
    u32 lastSeen = mainTicks.load(std::memory_order_relaxed);
    int stalledSeconds = 0;
    bool reported = false;
    while(!watchdogStop.load(std::memory_order_acquire))
    {
        LightEvent_WaitTimeout(&watchdogWake, 1000000000LL);
        const u32 now = mainTicks.load(std::memory_order_relaxed);
        if(watchdogPaused.load(std::memory_order_acquire))
        {
            lastSeen = now;
            stalledSeconds = 0;
            continue;
        }
        if(now != lastSeen || !now)
        {
            if(reported && now != lastSeen && !reporting.load())
            {
                // The loop resumed: mark the report so it is not mistaken for a hang.
                append(stallReport, "resumed after %d s at frame %lu\n", stalledSeconds, now);
                writeFile(stallPath, stallReport);
                reported = false;
            }
            lastSeen = now;
            stalledSeconds = 0;
            continue;
        }
        // Map loads take tens of seconds on the handheld, so a load stage
        // gets a much longer allowance before it counts as a stall.
        const char *phase = ac3ds_crash_phase;
        const bool loading = phase && !strncmp(phase, "load map", 8);
        if(++stalledSeconds < (loading ? 180 : 30) || reported || reporting.load()) continue;
        reported = true;
        stallReport.length = 0;
        append(stallReport, "AssaultCube 3DS build %d stall report\n", AC3DS_BUILD_NUMBER);
        append(stallReport, "main loop did not advance for %d s after %lu frames\n", stalledSeconds, now);
        append(stallReport, "phase: %s\n", phase ? phase : "?");
        // Written before asking GSP for the GPU state: if GSP itself is stuck,
        // the report still reaches the SD card.
        writeFile(stallPath, stallReport);
        u32 gpuBusy = 0xffffffff;
        GSPGPU_ReadHWRegs(0x400034, &gpuBusy, sizeof(gpuBusy));
        append(stallReport, "gpu busy=%08lx (bit 31 = P3D, 30 = PPF, 26/27 = PSC)\n", gpuBusy);
        appendContext(stallReport);
        writeFile(stallPath, stallReport);
        if(!aptIsActive() || reporting.load()) continue;
        char l1[48], l2[48], l3[48];
        snprintf(l1, sizeof(l1), "No progress after frame %lu", now);
        snprintf(l2, sizeof(l2), "phase %s", phase ? phase : "?");
        snprintf(l3, sizeof(l3), "gpu busy %08lx", gpuBusy);
        const char *lines[] = { l1, l2, l3 };
        showOnTouchScreen("AssaultCube stopped responding", lines, 3, "        stall.txt");
    }
}
} // namespace

namespace
{
// Aborts (out of memory, std::terminate) end in svcBreak, which shows the
// system error screen without reaching the exception handler or Luma. Record
// them first.
void writeFatal(const char *what, void *caller)
{
    if(reporting.exchange(true)) return;
    const struct mallinfo m = mallinfo();
    crashReport.length = 0;
    append(crashReport, "AssaultCube 3DS build %d fatal error\n", AC3DS_BUILD_NUMBER);
    append(crashReport, "%s\n", what);
    append(crashReport, "thread: %s\n", currentThreadName());
    append(crashReport, "phase: %s\n", ac3ds_crash_phase ? ac3ds_crash_phase : "?");
    append(crashReport, "caller=%08lx\n", u32(uintptr_t(caller)));
    append(crashReport, "image: text=%08lx data=%08lx bss=%08lx frame=%lu\n", u32(uintptr_t(&ac3ds_image_text_marker)),
           u32(uintptr_t(&ac3ds_image_data_marker)), u32(uintptr_t(&ac3ds_image_bss_marker)),
           u32(mainTicks.load(std::memory_order_relaxed)));
    append(crashReport, "heap: arena %u KB, in use %u KB, free %u KB\n", unsigned(m.arena / 1024), unsigned(m.uordblks / 1024), unsigned(m.fordblks / 1024));
    append(crashReport, "linear free %lu KB, application free %ld KB\n", u32(linearSpaceFree() / 1024),
           long(s32(osGetMemRegionFree(MEMREGION_APPLICATION)) / 1024));
    append(crashReport, "heap limit %lu KB, most ever claimed %lu KB\n", heapLimit() / 1024, u32(__malloc_max_sbrked_mem) / 1024);
    unsigned failedFrame = 0;
    if(const unsigned failed = ac3ds_alloc_last_failed_bytes(&failedFrame))
        append(crashReport, "last failed request: %u bytes at frame %u\n", failed, failedFrame);
    // Who asked: the fatal paths have no register dump, so list the callers on this stack.
    MemInfo codeInfo;
    PageInfo page;
    u32 codeEnd = 0x00100000;
    if(R_SUCCEEDED(svcQueryMemory(&codeInfo, &page, 0x00100000))) codeEnd = codeInfo.base_addr + codeInfo.size;
    appendStack(crashReport, u32(uintptr_t(__builtin_frame_address(0))), 0x00100000, codeEnd);
    appendContext(crashReport);
    writeFile(crashPath, crashReport);
}

void onOutOfMemory()
{
    writeFatal("out of memory (operator new failed)", __builtin_return_address(0));
    abort();
}
}

// libctru, Citro3D and NDSP panics (for example a full GX command queue) call
// svcBreak directly: the system error screen, no exception, no Luma dump.
extern "C" void __real_svcBreak(UserBreakType reason);
extern "C" void __wrap_svcBreak(UserBreakType reason)
{
    char what[48];
    snprintf(what, sizeof(what), "svcBreak(%d) called (library panic)", int(reason));
    writeFatal(what, __builtin_return_address(0));
    __real_svcBreak(reason);
}

extern "C" void __real_abort(void) __attribute__((noreturn));
extern "C" void __wrap_abort(void) __attribute__((noreturn));
extern "C" void __wrap_abort(void)
{
    writeFatal("abort() called", __builtin_return_address(0));
    __real_abort();
}

// Earlier reports are kept as crash-1.txt (newest) to crash-5.txt, and the same
// for stall reports, so a later crash never destroys the record of an earlier
// one. Rotation only happens when a new report exists. Runs once, on the
// first install (the main thread, before video init).
static void rotateReports(const char *stem, int keep)
{
    static const char *const dir = "sdmc:/3ds/assaultcube/profile";
    char from[96], to[96];
    snprintf(from, sizeof(from), "%s/%s.txt", dir, stem);
    FILE *f = fopen(from, "rb");
    if(!f) return;
    fclose(f);
    snprintf(to, sizeof(to), "%s/%s-%d.txt", dir, stem, keep);
    remove(to);
    for(int i = keep; i >= 1; --i)
    {
        snprintf(to, sizeof(to), "%s/%s-%d.txt", dir, stem, i);
        if(i > 1) snprintf(from, sizeof(from), "%s/%s-%d.txt", dir, stem, i - 1);
        else snprintf(from, sizeof(from), "%s/%s.txt", dir, stem);
        rename(from, to); // fails harmlessly when the source is missing
    }
}

static void keepPreviousReports()
{
    static bool done = false;
    if(done) return;
    done = true;
    rotateReports("crash", 5);
    rotateReports("stall", 5);
}

void ac3ds_crash_install(const char *thread_name)
{
    keepPreviousReports();
    // Worker threads come and go with each map load, so slots are reused.
    for(int slot = 0; slot < maxThreads; ++slot)
    {
        bool expected = false;
        if(!slotUsed[slot].compare_exchange_strong(expected, true)) continue;
        threadNames[slot] = thread_name;
        threadOnException(onException, handlerStacks[slot] + handlerStackBytes, WRITE_DATA_TO_HANDLER_STACK);
        std::set_new_handler(onOutOfMemory);
        return;
    }
}

void ac3ds_crash_uninstall()
{
    // Call from the thread itself, before it returns.
    const u8 *tls = static_cast<const u8 *>(getThreadLocalStorage());
    const u32 stack = *reinterpret_cast<const u32 *>(tls + 0x44);
    threadOnException(NULL, NULL, NULL);
    for(int slot = 0; slot < maxThreads; ++slot)
        if(stack == u32(uintptr_t(handlerStacks[slot] + handlerStackBytes))) slotUsed[slot].store(false);
}

void ac3ds_crash_pause(bool paused)
{
    watchdogPaused.store(paused, std::memory_order_release);
    if(!paused && watchdogThread) LightEvent_Signal(&watchdogWake);
}

unsigned ac3ds_crash_frame() { return mainTicks.load(std::memory_order_relaxed); }

void ac3ds_crash_tick()
{
    const u32 tick = mainTicks.fetch_add(1, std::memory_order_relaxed) + 1;
    // mallinfo and linearSpaceFree walk the allocators' free lists.
    if(tick % 512 != 1) return;
    const struct mallinfo m = mallinfo();
    snapshot.heapUsedKb = u32(m.uordblks / 1024);
    snapshot.heapArenaKb = u32(m.arena / 1024);
    snapshot.heapLimitKb = heapLimit() / 1024;
    snapshot.linearFreeKb = u32(linearSpaceFree() / 1024);
    snapshot.tick = tick;
}

void ac3ds_crash_set_map(const char *name)
{
    snprintf(snapshot.map, sizeof(snapshot.map), "%s", name ? name : "");
}

void ac3ds_crash_note_log(const char *line)
{
    // The handler may read a slot while this writes it; the last byte of
    // every slot stays zero, so the worst case is one garbled line.
    const u32 slot = logRingNext.load(std::memory_order_relaxed) % logRingLines;
    strncpy(logRing[slot], line, logRingWidth - 1);
    logRingNext.store(slot + 1, std::memory_order_relaxed);
}

void ac3ds_crash_start_watchdog()
{
    if(watchdogThread) return;
    LightEvent_Init(&watchdogWake, RESET_ONESHOT);
    watchdogStop.store(false);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // The New model's extra core, so a main thread spinning on core 0 cannot
    // starve it. It sleeps for a second at a time.
    const s32 lower = priority + 1 > 0x3f ? 0x3f : priority + 1;
    watchdogThread = threadCreate(watchdogMain, nullptr, 16 * 1024, lower, 2, false);
    if(!watchdogThread) watchdogThread = threadCreate(watchdogMain, nullptr, 16 * 1024, lower, 0, false);
}

void ac3ds_crash_stop_watchdog()
{
    if(!watchdogThread) return;
    watchdogStop.store(true, std::memory_order_release);
    LightEvent_Signal(&watchdogWake);
    threadJoin(watchdogThread, U64_MAX);
    threadFree(watchdogThread);
    watchdogThread = nullptr;
}

// Heap the allocator does not hold right now: what a request that fits no
// free block can still extend into. Constant time.
size_t ac3ds_heap_unclaimed()
{
    const u32 limit = heapLimit(), arena = u32(__malloc_current_mallinfo.arena);
    return limit > arena ? limit - arena : 0;
}

void ac3ds_memory_line(char *out, size_t size)
{
    // "Unclaimed" is heap the allocator does not hold right now: what a large
    // request can still extend into, which the free-in-arena figure says
    // nothing about. free() gives the top of the arena back, so the lowest
    // value there has ever been is shown as well.
    const struct mallinfo m = mallinfo();
    const u32 limit = heapLimit(), arena = u32(m.arena), peak = u32(__malloc_max_sbrked_mem);
    // The small-block pool is one 6 MB chunk as far as mallinfo can tell.
    unsigned poolKb = 0, poolCapacityKb = 0, poolBlocks = 0, poolFull = 0;
    ac3ds_smallpool_stats(&poolKb, &poolCapacityKb, &poolBlocks, &poolFull);
    snprintf(out, size, "heap %u KB in use, %u KB free in arena, %lu KB unclaimed (lowest %lu) of %lu KB; small pool %u of %u KB, %u blocks, %u refused; linear free %lu KB",
             unsigned(m.uordblks / 1024), unsigned(m.fordblks / 1024), (limit > arena ? limit - arena : 0) / 1024,
             (limit > peak ? limit - peak : 0) / 1024, limit / 1024, poolKb, poolCapacityKb, poolBlocks, poolFull, u32(linearSpaceFree() / 1024));
}

void ac3ds_log_memory(void (*log)(const char *))
{
    char line[160];
    snprintf(line, sizeof(line), "memory: appmemtype %lu, application %lu KB (%ld KB free), heap limit %lu KB, linear free %lu KB",
             osGetApplicationMemType(), osGetMemRegionSize(MEMREGION_APPLICATION) / 1024,
             long(s32(osGetMemRegionFree(MEMREGION_APPLICATION)) / 1024), heapLimit() / 1024,
             u32(linearSpaceFree() / 1024));
    log(line);
}


#include "loadprofile_3ds.h"

Ac3dsLoadProfile ac3ds_load_profile = {};

void ac3ds_load_profile_reset()
{
    ac3ds_load_profile = {};
}

void ac3ds_load_profile_report(void (*log)(const char *))
{
    static const char *const names[AC3DS_LOAD_COUNT] = {
        "image decode", "surface fix", "cpu scale", "tex upload", "texture total",
        "model total", "model file", "model skin", "model neighbors", "find file", "tex free", "sound total", "sound decode" };
    char line[128];
    for(int i = 0; i < AC3DS_LOAD_COUNT; ++i)
    {
        if(!ac3ds_load_profile.calls[i]) continue;
        snprintf(line, sizeof(line), "load profile: %-16s %7lu ms  %5u calls", names[i],
                 u32(ac3ds_load_profile.ticks[i] * 1000 / SYSCLOCK_ARM11), ac3ds_load_profile.calls[i]);
        log(line);
    }
}

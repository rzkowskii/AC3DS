// Crash and stall reporting for handheld runs where no debugger is attached.
//
// The exception handler avoids the C library's file layer and allocator, since
// the fault may have happened while one of their locks was held. It formats
// into static buffers and writes through FS:USER directly.

#include "crash_3ds.h"
#include "system.h"

#include <3ds.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

const char *volatile ac3ds_crash_phase = "startup";

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

void showOnTouchScreen(const char *title, const char *const *lines, int count)
{
    const char *all[16];
    int n = 0;
    all[n++] = title;
    for(int i = 0; i < count && n < 16; ++i) all[n++] = lines[i];
    if(n < 16) all[n++] = "";
    if(n < 16) all[n++] = "Report: /3ds/assaultcube/profile/";
    if(n < 16) all[n++] = "        crash.txt";
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

[[noreturn]] void onException(ERRF_ExceptionInfo *excep, CpuRegisters *regs)
{
    if(reporting.exchange(true)) park();
    MemInfo codeInfo;
    PageInfo page;
    u32 codeBase = 0x00100000, codeEnd = 0x00100000;
    if(R_SUCCEEDED(svcQueryMemory(&codeInfo, &page, 0x00100000)))
        codeEnd = codeInfo.base_addr + codeInfo.size;

    crashReport.length = 0;
    append(crashReport, "AssaultCube 3DS crash report\n");
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
    // The instruction words around the PC, when the code is readable.
    const u32 pc = regs->pc & ~3u;
    if(readable(pc - 8, 20))
    {
        const u32 *words = reinterpret_cast<const u32 *>(pc - 8);
        append(crashReport, "code@pc-8: %08lx %08lx [%08lx] %08lx %08lx\n", words[0], words[1], words[2], words[3], words[4]);
    }
    // Words on the faulting stack that point into the code region. The first
    // few are usually the caller chain.
    append(crashReport, "stack return candidates:\n");
    u32 stackEnd = 0;
    int found = 0;
    if(readable(regs->sp & ~3u, 4, &stackEnd))
    {
        const u32 *sp = reinterpret_cast<const u32 *>(regs->sp & ~3u);
        const u32 limit = (stackEnd - (regs->sp & ~3u)) / 4;
        for(u32 i = 0; i < limit && i < 2048 && found < 32; ++i)
        {
            const u32 value = sp[i];
            if(!inCode(value & ~1u, codeBase, codeEnd)) continue;
            append(crashReport, "  sp+%03lx %08lx\n", i * 4, value);
            ++found;
        }
    }
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
std::atomic<u32> mainTicks{0};
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
        u32 gpuBusy = 0xffffffff;
        GSPGPU_ReadHWRegs(0x400034, &gpuBusy, sizeof(gpuBusy));
        stallReport.length = 0;
        append(stallReport, "AssaultCube 3DS stall report\n");
        append(stallReport, "main loop did not advance for %d s after %lu frames\n", stalledSeconds, now);
        append(stallReport, "phase: %s\n", phase ? phase : "?");
        append(stallReport, "gpu busy=%08lx (bit 31 = P3D, 30 = PPF, 26/27 = PSC)\n", gpuBusy);
        writeFile(stallPath, stallReport);
        if(!aptIsActive() || reporting.load()) continue;
        char l1[48], l2[48], l3[48];
        snprintf(l1, sizeof(l1), "No progress after frame %lu", now);
        snprintf(l2, sizeof(l2), "phase %s", phase ? phase : "?");
        snprintf(l3, sizeof(l3), "gpu busy %08lx", gpuBusy);
        const char *lines[] = { l1, l2, l3 };
        showOnTouchScreen("AssaultCube stopped responding", lines, 3);
    }
}
} // namespace

void ac3ds_crash_install(const char *thread_name)
{
    // Worker threads come and go with each map load, so slots are reused.
    for(int slot = 0; slot < maxThreads; ++slot)
    {
        bool expected = false;
        if(!slotUsed[slot].compare_exchange_strong(expected, true)) continue;
        threadNames[slot] = thread_name;
        threadOnException(onException, handlerStacks[slot] + handlerStackBytes, WRITE_DATA_TO_HANDLER_STACK);
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

void ac3ds_crash_tick()
{
    mainTicks.fetch_add(1, std::memory_order_relaxed);
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

void ac3ds_log_memory(void (*log)(const char *))
{
    char line[160];
    snprintf(line, sizeof(line), "memory: appmemtype %lu, application %lu KB (%ld KB free), linear free %lu KB",
             osGetApplicationMemType(), osGetMemRegionSize(MEMREGION_APPLICATION) / 1024,
             long(s32(osGetMemRegionFree(MEMREGION_APPLICATION)) / 1024), u32(linearSpaceFree() / 1024));
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

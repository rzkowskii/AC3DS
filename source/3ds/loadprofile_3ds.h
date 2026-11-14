#pragma once

// Load-time profiler for map loads on the handheld. Each slot accumulates
// inclusive ARM11 ticks (nested slots overlap) and a call count. load_world
// resets it and writes one summary line per slot to the client log.

#ifdef __3DS__
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
}

enum Ac3dsLoadSlot
{
    AC3DS_LOAD_IMAGE_DECODE,   // IMG_Load from RomFS / SD
    AC3DS_LOAD_SURFACE_FIX,    // pixel format conversion and fixups
    AC3DS_LOAD_SCALE,          // CPU resize and mipmap box filter
    AC3DS_LOAD_TEX_UPLOAD,     // glTexImage2D: allocation, decode, tiling
    AC3DS_LOAD_TEXTURE_TOTAL,  // whole loadsurface
    AC3DS_LOAD_MODEL_TOTAL,    // whole loadmodel (includes skins)
    AC3DS_LOAD_MODEL_FILE,     // md2/md3 part file parse
    AC3DS_LOAD_MODEL_SKIN,     // loadskin
    AC3DS_LOAD_MODEL_NEIGHBORS,// calcneighbors for stencil shadows
    AC3DS_LOAD_FIND_FILE,      // findfile path probing (SD stat calls)
    AC3DS_LOAD_TEX_FREE,       // SDL surface and buffer release
    AC3DS_LOAD_SOUND_TOTAL,    // sbuffer::load (file probing + decode)
    AC3DS_LOAD_SOUND_DECODE,   // Ogg Vorbis decode into the native mixer
    AC3DS_LOAD_COUNT
};

struct Ac3dsLoadProfile
{
    unsigned long long ticks[AC3DS_LOAD_COUNT];
    unsigned calls[AC3DS_LOAD_COUNT];
};
extern Ac3dsLoadProfile ac3ds_load_profile;

struct Ac3dsLoadTimer
{
    int slot;
    unsigned long long start;
    explicit Ac3dsLoadTimer(int s) : slot(s), start(svcGetSystemTick()) {}
    ~Ac3dsLoadTimer()
    {
        ac3ds_load_profile.ticks[slot] += svcGetSystemTick() - start;
        ++ac3ds_load_profile.calls[slot];
    }
};

void ac3ds_load_profile_reset();
void ac3ds_load_profile_report(void (*log)(const char *));

#define AC3DS_LOAD_TIMER_CAT2(a, b) a##b
#define AC3DS_LOAD_TIMER_CAT(a, b) AC3DS_LOAD_TIMER_CAT2(a, b)
#define AC3DS_LOAD_TIME(slot) Ac3dsLoadTimer AC3DS_LOAD_TIMER_CAT(ac3dsLoadTimer, __LINE__)(slot)
#else
#define AC3DS_LOAD_TIME(slot) do {} while(0)
#endif

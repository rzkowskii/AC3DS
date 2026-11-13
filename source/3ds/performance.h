#pragma once

#include <3ds.h>

enum class Ac3dsPhase
{
    Update, Bots, Audio, Rays, Reflection, Refraction, World, WorldTraversal, ProjectedTraversal,
    ProjectedWorld, ProjectedModels, Models, Shadows, Water, Draw, VBlankWait, QueueStart,
    Camera, DynamicLights, Hud, Count
};

extern bool ac3dsTimingPhases;
extern unsigned ac3dsTimingGeneration;
void ac3dsRecordPhase(Ac3dsPhase phase, u64 ticks);
void ac3dsRecordFrame(u64 elapsed, unsigned slots);

class Ac3dsPhaseTimer
{
    Ac3dsPhase phase;
    unsigned generation;
    u64 start;

public:
    explicit Ac3dsPhaseTimer(Ac3dsPhase phase)
        : phase(phase), generation(ac3dsTimingGeneration),
          start(ac3dsTimingPhases ? svcGetSystemTick() : 0) {}

    ~Ac3dsPhaseTimer() { stop(); }

    void stop()
    {
        if(start && generation == ac3dsTimingGeneration)
            ac3dsRecordPhase(phase, svcGetSystemTick() - start);
        start = 0;
    }
};

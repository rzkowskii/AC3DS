// Decodes upcoming map textures on the New 3DS's second application core.
//
// A map load decodes about a hundred JPEG/PNG files, one after another, on
// core 0 (5.6 s for ac_keller in Azahar at base clock). The load already
// knows every texture it will need, so a worker on core 2 decodes them ahead
// of the main thread, in the same order. The worker calls the same IMG_Load
// on the same resolved file, so the pixels are identical; the main thread
// still does every format conversion, resize and upload as before.

#include "texprefetch_3ds.h"

#include <SDL.h>
#include <SDL_image.h>

extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
#include <3ds/thread.h>
#include <3ds/os.h>
}

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
enum class JobState { Queued, Decoding, Done, Claimed };

struct Job
{
    std::string path;
    SDL_Surface *surface = nullptr;
    JobState state = JobState::Queued;
};

// Decoded surfaces held ahead of the main thread. A 1024x1024 RGBA source is
// 4 MB, so the window stays small.
constexpr int maxReady = 3;

LightLock lock;
LightEvent workerWake, jobDone;
bool initialized = false;
Thread worker = nullptr;
bool stopping = false;
std::vector<std::unique_ptr<Job>> jobs;
int ready = 0;
int statAhead = 0, statWaited = 0, statSelf = 0, statQueued = 0, statHelped = 0;
u64 statWaitTicks = 0;

void init()
{
    if(initialized) return;
    LightLock_Init(&lock);
    LightEvent_Init(&workerWake, RESET_ONESHOT);
    LightEvent_Init(&jobDone, RESET_PULSE);
    initialized = true;
}

Job *nextJob()
{
    if(ready >= maxReady) return nullptr;
    for(auto &job : jobs) if(job->state == JobState::Queued) return job.get();
    return nullptr;
}

void workerMain(void *)
{
    for(;;)
    {
        LightLock_Lock(&lock);
        Job *job = stopping ? nullptr : nextJob();
        if(job) job->state = JobState::Decoding;
        const bool stop = stopping;
        LightLock_Unlock(&lock);
        if(stop) return;
        if(!job)
        {
            LightEvent_WaitTimeout(&workerWake, 50000000LL);
            continue;
        }
        SDL_Surface *surface = IMG_Load(job->path.c_str());
        LightLock_Lock(&lock);
        job->surface = surface;
        job->state = JobState::Done;
        ++ready;
        LightLock_Unlock(&lock);
        LightEvent_Signal(&jobDone);
    }
}
}

void ac3ds_texprefetch_begin()
{
    init();
    LightLock_Lock(&lock);
    const bool running = worker != nullptr;
    stopping = false;
    LightLock_Unlock(&lock);
    if(running) return;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // Core 2 exists only on New models; without it, loads stay single-core.
    worker = threadCreate(workerMain, nullptr, 64 * 1024, priority, 2, false);
    static bool reported = false;
    if(!reported)
    {
        reported = true;
        extern void clientlogf(const char *s, ...);
        clientlogf("texture prefetch: core 2 worker %s", worker ? "started" : "unavailable");
    }
}

void ac3ds_texprefetch_add(const char *resolvedpath)
{
    if(!worker || !resolvedpath || !*resolvedpath) return;
    LightLock_Lock(&lock);
    bool known = false;
    for(auto &job : jobs) if(job->path == resolvedpath) { known = true; break; }
    if(!known)
    {
        jobs.emplace_back(new Job);
        jobs.back()->path = resolvedpath;
        ++statQueued;
    }
    LightLock_Unlock(&lock);
    if(!known) LightEvent_Signal(&workerWake);
}

SDL_Surface *ac3ds_texprefetch_take(const char *resolvedpath, bool &prefetched)
{
    prefetched = false;
    if(!initialized || !resolvedpath) return nullptr;
    const u64 start = svcGetSystemTick();
    bool waited = false;
    for(;;)
    {
        LightLock_Lock(&lock);
        size_t index = jobs.size();
        for(size_t i = 0; i < jobs.size(); ++i) if(jobs[i]->path == resolvedpath) { index = i; break; }
        if(index == jobs.size())
        {
            LightLock_Unlock(&lock);
            return nullptr;
        }
        Job &job = *jobs[index];
        if(job.state == JobState::Queued || job.state == JobState::Claimed)
        {
            // Not started yet: the caller decodes it itself, as before.
            jobs.erase(jobs.begin() + index);
            ++statSelf;
            LightLock_Unlock(&lock);
            return nullptr;
        }
        if(job.state == JobState::Done)
        {
            SDL_Surface *surface = job.surface;
            --ready;
            jobs.erase(jobs.begin() + index);
            if(waited) { ++statWaited; statWaitTicks += svcGetSystemTick() - start; } else ++statAhead;
            LightLock_Unlock(&lock);
            LightEvent_Signal(&workerWake);
            prefetched = true;
            return surface;
        }
        // The worker is still decoding the one we need. Rather than idle,
        // decode a later queued texture here so both cores decode at once.
        Job *help = nullptr;
        if(ready < maxReady + 2)
            for(size_t i = index + 1; i < jobs.size(); ++i)
                if(jobs[i]->state == JobState::Queued) { help = jobs[i].get(); break; }
        if(help)
        {
            help->state = JobState::Decoding;
            LightLock_Unlock(&lock);
            SDL_Surface *surface = IMG_Load(help->path.c_str());
            LightLock_Lock(&lock);
            help->surface = surface;
            help->state = JobState::Done;
            ++ready;
            ++statHelped;
            LightLock_Unlock(&lock);
            continue;
        }
        LightLock_Unlock(&lock);
        waited = true;
        LightEvent_WaitTimeout(&jobDone, 20000000LL);
    }
}

void ac3ds_texprefetch_end(void (*log)(const char *))
{
    if(!initialized) return;
    LightLock_Lock(&lock);
    stopping = true;
    LightLock_Unlock(&lock);
    if(worker)
    {
        LightEvent_Signal(&workerWake);
        threadJoin(worker, U64_MAX);
        threadFree(worker);
        worker = nullptr;
    }
    LightLock_Lock(&lock);
    for(auto &job : jobs) if(job->surface) SDL_FreeSurface(job->surface);
    jobs.clear();
    ready = 0;
    LightLock_Unlock(&lock);
    if(log && statQueued)
    {
        char line[160];
        snprintf(line, sizeof(line), "texture prefetch: %d queued, %d ready ahead, %d waited (%lu ms), %d decoded on main, %d decoded ahead on main",
                 statQueued, statAhead, statWaited, u32(statWaitTicks * 1000 / SYSCLOCK_ARM11), statSelf, statHelped);
        log(line);
    }
    statAhead = statWaited = statSelf = statQueued = statHelped = 0;
    statWaitTicks = 0;
}

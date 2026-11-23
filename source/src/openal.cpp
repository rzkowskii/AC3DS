// simple OpenAL call wrappers

#include "cube.h"
#include "loadprofile_3ds.h"
#ifdef __3DS__
#include "audio_3ds.h"
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
#include <3ds/thread.h>
#include <3ds/os.h>
}
#include "crash_3ds.h"

COMMANDF(ac3dsaudiostats, "", ()
{
    unsigned decoded;
    unsigned submitted;
    ac3ds_audio_stats(decoded, submitted);
    clientlogf("AC3DS_AUDIO decoded_blocks=%u submitted_waves=%u", decoded, submitted);
    intret(submitted ? int(min(decoded, unsigned(INT_MAX))) : 0);
});
#endif

#define DEBUGCOND (audiodebug==1)

int al_referencedistance = 400; // The distance from the source emitting the sound to the listener
int al_rollofffactor = 100; // This value indicates the relative "strength" of a sound (how far away the sound can be heard)

// represents an OpenAL source, an audio emitter in the 3D world

source::source() : id(0), owner(NULL), locked(false), valid(false), priority(SP_NORMAL)
{
    valid = generate();
    ASSERT(!valid || alIsSource(id));
}

source::~source()
{
    if(valid) delete_();
}

void source::lock()
{
    locked = true;
    DEBUG("source locked, " << lastmillis);
}

void source::unlock()
{
    locked = false;
    owner = NULL;
    stop();
    buffer(0);
    DEBUG("source unlocked, " << lastmillis);
}

void source::reset()
{
    ASSERT(alIsSource(id));
    owner = NULL;
    locked = false;
    priority = SP_NORMAL;

    // restore default settings

    stop();
    buffer(0);

    pitch(1.0f);
    gain(1.0f);
    position(0.0f, 0.0f, 0.0f);
    velocity(0.0f, 0.0f, 0.0f);

    looping(false);
    sourcerelative(false);

    // fit into distance model
    alSourcef(id, AL_REFERENCE_DISTANCE, al_referencedistance/100.0f);
    alSourcef(id, AL_ROLLOFF_FACTOR, al_rollofffactor/100.0f);
}


void source::init(sourceowner *o)
{
    ASSERT(o);
    owner = o;
}

void source::onreassign()
{
    if(owner)
    {
        owner->onsourcereassign(this);
        owner = NULL;
    }
}

bool source::generate()
{
    alclearerr();
    alGenSources(1, &id);

    return !alerr(false);
}

bool source::delete_()
{
    alclearerr();
    alDeleteSources(1, &id);
    return !ALERR;
}

bool source::buffer(ALuint buf_id)
{
    alclearerr();
#ifdef __APPLE__    // weird bug
    if (buf_id)
#endif
        alSourcei(id, AL_BUFFER, buf_id);

    return !ALERR;
}

bool source::looping(bool enable)
{
    alclearerr();
    alSourcei(id, AL_LOOPING, enable ? 1 : 0);
    return !ALERR;
}

bool source::queuebuffers(ALsizei n, const ALuint *buffer_ids)
{
    alclearerr();
    alSourceQueueBuffers(id, n, buffer_ids);
    return !ALERR;
}

bool source::unqueueallbuffers()
{
    alclearerr();
    ALint queued;
    alGetSourcei(id, AL_BUFFERS_QUEUED, &queued);
    ALERR;
    loopi(queued)
    {
        ALuint buffer;
        alSourceUnqueueBuffers(id, 1, &buffer);
    }
    return !ALERR;
}

bool source::gain(float g)
{
    alclearerr();
    alSourcef(id, AL_GAIN, g);
    return !ALERRF("gain: %f", g);
}

bool source::pitch(float p)
{
    alclearerr();
    alSourcef(id, AL_PITCH, p);
    return !ALERRF("pitch: %f", p);
}

bool source::position(const vec &pos)
{
    return position(pos.x, pos.y, pos.z);
}

bool source::position(float x, float y, float z)
{
    alclearerr();
    alSource3f(id, AL_POSITION, x, y, z);
    if(x < 0 || y < 0 || z < -128 || x > ssize || y > ssize || z > 128) clientlogf("warning: sound position out of range (%f,%f,%f)", x, y, z);
    return !ALERRF("id %u, %d, x: %f, y: %f, z: %f", id, alIsSource(id) == AL_TRUE ? 1 : 0,  x, y, z);
//    return !ALERRF("x: %f, y: %f, z: %f", x, y, z);   // when sound bug has been absent for a while, switch to this version
}

bool source::velocity(float x, float y, float z)
{
    alclearerr();
    alSource3f(id, AL_VELOCITY, x, y, z);
    return !ALERRF("dx: %f, dy: %f, dz: %f", x, y, z);
}

vec source::position()
{
    alclearerr();
    ALfloat v[3];
    alGetSourcefv(id, AL_POSITION, v);
    if(ALERR) return vec(0,0,0);
    else return vec(v[0], v[1], v[2]);
}

bool source::sourcerelative(bool enable)
{
    alclearerr();
    alSourcei(id, AL_SOURCE_RELATIVE, enable ? AL_TRUE : AL_FALSE);
    return !ALERR;
}

int source::state()
{
    ALint s;
    alGetSourcei(id, AL_SOURCE_STATE, &s);
    return s;
}

bool source::secoffset(float secs)
{
    alclearerr();
    alSourcef(id, AL_SEC_OFFSET, secs);
    // some openal implementations seem to spam invalid enum on this
    // return !ALERR;
    return !alerr(false);
}

float source::secoffset()
{
    alclearerr();
    ALfloat s;
    alGetSourcef(id, AL_SEC_OFFSET, &s);
    // some openal implementations seem to spam invalid enum on this
    // ALERR;
    alerr(false);
    return s;
}

bool source::playing()
{
    return (state() == AL_PLAYING);
}

bool source::play()
{
    alclearerr();
    alSourcePlay(id);
    return !ALERR;
}

bool source::stop()
{
    alclearerr();
    alSourceStop(id);
    return !ALERR;
}

bool source::rewind()
{
    alclearerr();
    alSourceRewind(id);
    return !ALERR;
}

void source::printposition()
{
    alclearerr();
    vec v = position();
    ALint s;
    alGetSourcei(id, AL_SOURCE_TYPE, &s);
    conoutf("sound %d: pos(%f,%f,%f) t(%d) ", id, v.x, v.y, v.z, s);
    ALERR;
}


// represents an OpenAL sound buffer

sbuffer::sbuffer() : id(0), name(NULL)
{
}

sbuffer::~sbuffer()
{
    unload();
}

#ifdef __3DS__
static bool load_packed_ogg_sample(ALuint id, OggVorbis_File &file, const vorbis_info &info)
{
    Ac3dsPcmBlocks *sample = ac3ds_audio_create_pcm_blocks(info.channels);
    if(!sample) return false;

    int16_t pcm[ac3ds_pcm_block_frames * 2];
    const int framebytes = info.channels * sizeof(int16_t);
    const int blockbytes = ac3ds_pcm_block_frames * framebytes;
    int used = 0, bitstream, holes = 0;
    bool loaded = false;
    for(;;)
    {
        const long bytes = readvorbispcm(&file, reinterpret_cast<char *>(pcm) + used, blockbytes - used, &bitstream);
        if(bytes == OV_HOLE && ++holes <= 4) continue;
        if(bytes < 0) break;
        holes = 0;
        used += int(bytes);
        if(used == blockbytes || (!bytes && used))
        {
            if(used % framebytes || !ac3ds_audio_append_pcm_block(sample, pcm, used / framebytes)) break;
            used = 0;
        }
        if(!bytes)
        {
            loaded = ac3ds_audio_take_pcm_blocks(id, sample, info.rate);
            break;
        }
    }
    if(!loaded) ac3ds_audio_free_pcm_blocks(sample);
    return loaded;
}

static bool load_native_ogg_sample(ALuint id, OggVorbis_File &file, const vorbis_info &info)
{
    const int framebytes = info.channels * sizeof(int16_t);
    const ogg_int64_t frames = ov_pcm_total(&file, -1);
    if(frames > INT_MAX / framebytes) return false;
    // Keep small effects and soundtrack chunks on the ordinary PCM path.
    if(frames <= 0 || frames * framebytes >= 512 * 1024)
        return load_packed_ogg_sample(id, file, info);

    // Most clips are seekable, so decode into one exact-sized PCM allocation.
    // The native buffer can adopt it directly, including long looping ambience.
    int capacity = frames > 0 ? int(frames) * framebytes : 0;
    char *pcm = capacity ? static_cast<char *>(malloc(capacity)) : NULL;
    if(capacity && !pcm) return load_packed_ogg_sample(id, file, info);

    int used = 0, bitstream, holes = 0;
    char block[32 * 1024];
    long bytes;
    for(;;)
    {
        bytes = readvorbispcm(&file, block, sizeof(block), &bitstream);
        if(bytes == OV_HOLE && ++holes <= 4) continue;
        if(bytes <= 0 || bytes > INT_MAX - used) break;
        holes = 0;
        if(used + bytes > capacity)
        {
            const int required = used + int(bytes);
            const int growth = min(capacity / 2, INT_MAX - capacity);
            const int expanded = max(required, capacity + growth);
            char *more = static_cast<char *>(realloc(pcm, expanded));
            if(!more) break;
            pcm = more;
            capacity = expanded;
        }
        memcpy(pcm + used, block, size_t(bytes));
        used += int(bytes);
    }

    if(bytes == 0 && used > 0 && used % framebytes == 0 &&
       ac3ds_audio_take_pcm16(id, reinterpret_cast<int16_t *>(pcm), used / framebytes, info.channels, info.rate))
        return true;
    free(pcm);
    return false;
}
#endif

#ifdef __3DS__
// Background decode of game sounds and map ambience on the New 3DS's second
// application core. Every sample is a full Ogg Vorbis decode: about 100 game
// sounds at startup, and 12.7 s for ac_keller's 12 ambient loops in Azahar.
// Loading queues a job; a low-priority worker on core 2 decodes into the same
// PCM or packed blocks the synchronous path produces; the main thread adopts
// each finished sample in ac3ds_async_sounds_poll().
// - A game sound played before its job is adopted is claimed (sbuffer::claim):
//   decoded on the spot if still queued, waited for if the worker has it,
//   adopted at once if done. No game sound is dropped.
// - A map sound location with no buffer yet is stale and is retried on the
//   next map sound refresh, so each ambient loop starts as soon as it is ready.
// The worker never looks files up: the main thread resolves the path when it
// queues the job, the lookup load() would make at that moment. Files inside zip
// mods load synchronously, since zipstreams share one archive stream unlocked.
extern stream *openrawfile(const char *filename, const char *mode);

namespace
{
struct asyncsound
{
    sbuffer *buf = nullptr; // never dereferenced once cancelled: it may be gone
    string path;            // resolved file system path
    bool cancelled = false; // atomic: set by the main thread, polled by the decoder
    bool ok = false;
    int state = 0;          // 0 queued, 1 decoding, 2 done; changed under asynclock
    int channels = 0, rate = 0, frames = 0;
    int16_t *pcm = nullptr;
    Ac3dsPcmBlocks *blocks = nullptr;
};
LightLock asynclock;
LightEvent asyncwake, asyncdone;
Thread asyncworker = nullptr;
bool asyncready = false, asyncstop = false, asyncshutdown = false;
u64 asyncretrytick = 0; // after a failed start, loads stay synchronous until then
vector<asyncsound *> asyncjobs;
int adoptedcount = 0, claimedcount = 0;

bool iscancelled(const asyncsound &job) { return __atomic_load_n(&job.cancelled, __ATOMIC_ACQUIRE); }
void cancel(asyncsound &job) { __atomic_store_n(&job.cancelled, true, __ATOMIC_RELEASE); }

// Runs on the worker, or on the main thread for a claimed job. Stops at the
// next block once the job is cancelled, and keeps nothing partial.
void decodeasync(asyncsound &job)
{
    stream *f = openrawfile(job.path, "rb");
    if(!f) return;
    OggVorbis_File file;
    if(ov_open_callbacks(f, &file, NULL, 0, oggcallbacks)) { delete f; return; }
    const vorbis_info *info = ov_info(&file, -1);
    if(!compatiblevorbis(&file, info)) { ov_clear(&file); return; }
    const int framebytes = info->channels * sizeof(int16_t);
    const ogg_int64_t total = ov_pcm_total(&file, -1);
    job.channels = info->channels;
    job.rate = info->rate;
    int bitstream, holes = 0;
    if(total > 0 && total < INT_MAX / framebytes && total * framebytes < 512 * 1024)
    {
        // Same as load_native_ogg_sample: one exact-sized PCM allocation.
        int capacity = int(total) * framebytes, used = 0;
        char *pcm = static_cast<char *>(malloc(capacity));
        long bytes = -1;
        char block[16 * 1024];
        while(pcm)
        {
            if(iscancelled(job)) { bytes = -1; break; }
            bytes = readvorbispcm(&file, block, sizeof(block), &bitstream);
            if(bytes == OV_HOLE && ++holes <= 4) continue;
            if(bytes <= 0 || bytes > INT_MAX - used) break;
            holes = 0;
            if(used + bytes > capacity)
            {
                const int expanded = max(used + int(bytes), capacity + capacity / 2);
                char *more = static_cast<char *>(realloc(pcm, expanded));
                if(!more) { bytes = -1; break; }
                pcm = more;
                capacity = expanded;
            }
            memcpy(pcm + used, block, size_t(bytes));
            used += int(bytes);
        }
        if(pcm && bytes == 0 && used > 0 && used % framebytes == 0)
        {
            job.pcm = reinterpret_cast<int16_t *>(pcm);
            job.frames = used / framebytes;
            job.ok = true;
        }
        else free(pcm);
    }
    else
    {
        // Same as load_packed_ogg_sample: lossless packed blocks.
        Ac3dsPcmBlocks *sample = ac3ds_audio_new_pcm_blocks(info->channels);
        static thread_local int16_t pcm[ac3ds_pcm_block_frames * 2];
        const int blockbytes = ac3ds_pcm_block_frames * framebytes;
        int used = 0;
        while(sample && !iscancelled(job))
        {
            const long bytes = readvorbispcm(&file, reinterpret_cast<char *>(pcm) + used, blockbytes - used, &bitstream);
            if(bytes == OV_HOLE && ++holes <= 4) continue;
            if(bytes < 0) break;
            holes = 0;
            used += int(bytes);
            if(used == blockbytes || (!bytes && used))
            {
                if(used % framebytes || !ac3ds_audio_append_pcm_block(sample, pcm, used / framebytes)) break;
                used = 0;
            }
            if(!bytes)
            {
                job.blocks = sample;
                job.ok = true;
                sample = nullptr;
                break;
            }
        }
        ac3ds_audio_discard_pcm_blocks(sample);
    }
    ov_clear(&file);
}

void asyncworkermain(void *)
{
    ac3ds_crash_install("sound decode");
    struct Uninstall { ~Uninstall() { ac3ds_crash_uninstall(); } } uninstall; // frees the handler slot on exit
    for(;;)
    {
        LightLock_Lock(&asynclock);
        if(asyncstop) { LightLock_Unlock(&asynclock); return; }
        asyncsound *job = NULL;
        loopv(asyncjobs)
        {
            asyncsound *queued = asyncjobs[i];
            if(queued->state != 0) continue;
            if(iscancelled(*queued)) { queued->state = 2; continue; } // nothing to decode; the poll frees it
            job = queued;
            job->state = 1;
            break;
        }
        LightLock_Unlock(&asynclock);
        if(!job) { LightEvent_WaitTimeout(&asyncwake, 100000000LL); continue; }
        decodeasync(*job);
        LightLock_Lock(&asynclock);
        job->state = 2;
        LightLock_Unlock(&asynclock);
        LightEvent_Signal(&asyncdone);
    }
}

bool startasyncworker()
{
    if(asyncworker) return true;
    if(asyncshutdown) return false;
    const u64 now = svcGetSystemTick();
    if(asyncretrytick && now < asyncretrytick) return false;
    if(!asyncready)
    {
        LightLock_Init(&asynclock);
        LightEvent_Init(&asyncwake, RESET_ONESHOT);
        LightEvent_Init(&asyncdone, RESET_ONESHOT);
        asyncready = true;
    }
    if(ac3ds_audio_prepare_pcm_blocks())
    {
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        // Below the game and the audio mixer, on core 2.
        asyncstop = false;
        // The decode path uses about 50 KiB of stack before Tremor's own
        // per-codebook allocas, and thread stacks have no guard page.
        asyncworker = threadCreate(asyncworkermain, NULL, 128 * 1024, min(int(priority) + 2, 0x3F), 2, false);
    }
    // A failed start (memory, thread limit) is retried two seconds later.
    asyncretrytick = asyncworker ? 0 : now + 2 * u64(SYSCLOCK_ARM11);
    clientlogf("sounds: background decode on core 2 %s", asyncworker ? "started" : "unavailable, loading synchronously");
    return asyncworker != NULL;
}

void freejob(asyncsound *job)
{
    free(job->pcm);
    ac3ds_audio_discard_pcm_blocks(job->blocks);
    delete job;
}

// Main thread. The job is out of asyncjobs and not cancelled. Installs the
// decoded sample in its buffer. If the worker could not even open the file,
// the buffer is loaded synchronously instead.
bool adoptjob(asyncsound *job)
{
    sbuffer *buf = job->buf;
    bool adopted = false;
    if(job->ok && buf->pending && !buf->id)
    {
        ALuint id = 0;
        alclearerr();
        alGenBuffers(1, &id);
        if(!alerr(false) && id)
        {
            adopted = job->pcm ? ac3ds_audio_take_pcm16(id, job->pcm, job->frames, job->channels, job->rate)
                               : ac3ds_audio_take_pcm_blocks(id, job->blocks, job->rate);
            if(adopted) { job->pcm = NULL; job->blocks = NULL; buf->id = id; }
            else alDeleteBuffers(1, &id);
        }
    }
    // rate is set once the worker has opened the file and begun decoding. A
    // decode that then failed (memory, damaged stream) would fail the same way
    // here, as a full blocking decode on the game thread in the middle of a
    // match. Only a file the worker could not open gets the synchronous
    // loader, which also tries the other name variants.
    const bool attempted = job->rate != 0;
    freejob(job);
    buf->pending = false;
    if(!adopted && !buf->id && (attempted || !buf->load())) conoutf("\f3failed to load sample %s", buf->name);
    return adopted;
}
}

bool sbuffer::loadasync()
{
    if(!name) return false;
    if(id || pending) return true;
    string filepath, resolved;
    const char *exts[] = { "", ".wav", ".ogg" };
    loopi(sizeof(exts)/sizeof(exts[0]))
    {
        formatstring(filepath)("packages/audio/%s%s", name, exts[i]);
        path(filepath);
        // The lookup openfile() makes, so that the worker never reads the zip tables.
        copystring(resolved, findfile(filepath, "rb"));
        if(findfilelocation == FFL_ZIP) return load(); // zip mod member: synchronous, as upstream
        stream *f = openrawfile(resolved, "rb");
        if(!f) continue;
        delete f;
        size_t len = strlen(filepath);
        const bool ogg = len >= 4 && !strcasecmp(filepath + len - 4, ".ogg");
        if(!ogg || !startasyncworker()) return load(); // WAV and fallbacks stay synchronous
        asyncsound *job = new asyncsound;
        job->buf = this;
        copystring(job->path, resolved);
        LightLock_Lock(&asynclock);
        asyncjobs.add(job);
        LightLock_Unlock(&asynclock);
        LightEvent_Signal(&asyncwake);
        pending = true;
        return true;
    }
    return false;
}

bool sbuffer::claim()
{
    if(id) return true;
    if(!pending) return false;
    asyncsound *job = NULL;
    int state = -1;
    LightLock_Lock(&asynclock);
    loopv(asyncjobs) if(asyncjobs[i]->buf == this && !iscancelled(*asyncjobs[i]))
    {
        job = asyncjobs[i];
        state = job->state;
        if(state != 1) asyncjobs.remove(i); // queued or done: ours now
        break;
    }
    LightLock_Unlock(&asynclock);
    if(!job)
    {
        pending = false;
        return load();
    }
    if(state == 0) decodeasync(*job); // from the file resolved when it was queued
    else if(state == 1)
    {
        // The worker has it. Game sound clips decode in milliseconds.
        const u64 deadline = svcGetSystemTick() + 5 * u64(SYSCLOCK_ARM11);
        for(;;)
        {
            LightLock_Lock(&asynclock);
            const bool done = job->state == 2;
            if(done) asyncjobs.removeobj(job);
            LightLock_Unlock(&asynclock);
            if(done) break;
            if(svcGetSystemTick() > deadline)
            {
                clientlogf("sounds: %s is still decoding", name);
                return false; // the poll adopts it later
            }
            LightEvent_WaitTimeout(&asyncdone, 2000000LL);
        }
    }
    if(adoptjob(job)) ++claimedcount;
    return id != 0;
}

void ac3ds_async_sounds_poll()
{
    if(!asyncready) return;
    LightLock_Lock(&asynclock);
    loopv(asyncjobs)
    {
        asyncsound *job = asyncjobs[i];
        if(job->state != 2) continue;
        asyncjobs.remove(i--);
        LightLock_Unlock(&asynclock);
        if(iscancelled(*job)) freejob(job);
        else if(adoptjob(job)) ++adoptedcount;
        LightLock_Lock(&asynclock);
    }
    const bool idle = asyncjobs.empty();
    LightLock_Unlock(&asynclock);
    if(idle && (adoptedcount || claimedcount))
    {
        clientlogf("sounds: %d decoded in the background, %d claimed on first use", adoptedcount, claimedcount);
        adoptedcount = claimedcount = 0;
    }
}

void ac3ds_async_sounds_shutdown()
{
    if(!asyncready || !ismainthread()) return;
    asyncshutdown = true;
    LightLock_Lock(&asynclock);
    asyncstop = true;
    loopv(asyncjobs) if(!iscancelled(*asyncjobs[i]))
    {
        asyncjobs[i]->buf->pending = false;
        cancel(*asyncjobs[i]); // a decode in progress stops at its next block
    }
    LightLock_Unlock(&asynclock);
    LightEvent_Signal(&asyncwake);
    if(asyncworker)
    {
        threadJoin(asyncworker, U64_MAX);
        threadFree(asyncworker);
        asyncworker = NULL;
    }
    LightLock_Lock(&asynclock);
    loopv(asyncjobs) freejob(asyncjobs[i]);
    asyncjobs.setsize(0);
    LightLock_Unlock(&asynclock);
}
#endif

bool sbuffer::load(bool trydl)
{
    if(!name) return false;
    if(id) return true;
#ifdef __3DS__
    if(pending) return claim();
#endif
    AC3DS_LOAD_TIME(AC3DS_LOAD_SOUND_TOTAL);
    alclearerr();
    alGenBuffers(1, &id);
    if(!ALERR)
    {
        const char *exts[] = { "", ".wav", ".ogg" };
        string filepath;
        loopi(sizeof(exts)/sizeof(exts[0]))
        {
            formatstring(filepath)("packages/audio/%s%s", name, exts[i]);
            stream *f = openfile(path(filepath), "rb");
            if(!f) continue;

            size_t len = strlen(filepath);
            if(len >= 4 && !strcasecmp(filepath + len - 4, ".ogg"))
            {
                OggVorbis_File oggfile;
                if(!ov_open_callbacks(f, &oggfile, NULL, 0, oggcallbacks))
                {
                    const vorbis_info *info = ov_info(&oggfile, -1);
                    if(!compatiblevorbis(&oggfile, info))
                    {
                        ov_clear(&oggfile);
                        continue;
                    }

#ifdef __3DS__
                    bool loaded;
                    {
                        AC3DS_LOAD_TIME(AC3DS_LOAD_SOUND_DECODE);
                        loaded = load_native_ogg_sample(id, oggfile, *info);
                    }
                    ov_clear(&oggfile);
                    if(!loaded) continue;
#else
                    const size_t BUFSIZE = 32*1024;
                    vector<char> buf;
                    int bitstream, holes = 0;
                    long bytes = 0;

                    for(;;)
                    {
                        char buffer[BUFSIZE];
                        bytes = readvorbispcm(&oggfile, buffer, BUFSIZE, &bitstream);
                        if(bytes == OV_HOLE && ++holes <= 4) continue;
                        if(bytes <= 0) break;
                        holes = 0;
                        memcpy(buf.pad(int(bytes)), buffer, size_t(bytes));
                    }

                    if(!bytes && buf.length()) alBufferData(id, info->channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, buf.getbuf(), buf.length(), info->rate);
                    ov_clear(&oggfile);
                    if(bytes || !buf.length() || ALERR) continue;
#endif
                }
                else
                {
                    delete f;
                    continue;
                }
            }
            else
            {
                SDL_AudioSpec wavspec;
                uint32_t wavlen;
                uint8_t *wavbuf;

                SDL_RWops *input = f->rwops();
                const bool loaded = input && SDL_LoadWAV_RW(input, 0, &wavspec, &wavbuf, &wavlen);
                if(input) SDL_FreeRW(input);
                delete f;
                if(!loaded)
                {
                    SDL_ClearError();
                    continue;
                }

                ALenum format = AL_NONE;
                if((wavspec.channels == 1 || wavspec.channels == 2) && wavlen <= INT_MAX)
                {
                    switch(wavspec.format)
                    {
                        case AUDIO_S8:
                            loopi(wavlen) wavbuf[i] ^= 0x80; // OpenAL's 8-bit PCM is unsigned.
                            // Fall through.
                        case AUDIO_U8:
                            format = wavspec.channels == 2 ? AL_FORMAT_STEREO8 : AL_FORMAT_MONO8;
                            break;
                        case AUDIO_U16LSB: case AUDIO_U16MSB: case AUDIO_S16LSB: case AUDIO_S16MSB:
                            if(wavlen % 2) break;
                            if(wavspec.format != AUDIO_S16SYS)
                            {
                                const bool swapBytes = bool(SDL_AUDIO_ISBIGENDIAN(wavspec.format)) != isbigendian();
                                const bool unsignedSamples = !SDL_AUDIO_ISSIGNED(wavspec.format);
                                for(uint32_t i = 0; i < wavlen; i += 2)
                                {
                                    uint16_t sample;
                                    memcpy(&sample, wavbuf + i, sizeof(sample));
                                    if(swapBytes) sample = uint16_t((sample >> 8) | (sample << 8));
                                    if(unsignedSamples) sample ^= 0x8000;
                                    memcpy(wavbuf + i, &sample, sizeof(sample));
                                }
                            }
                            format = wavspec.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
                            break;
                    }
                }

                if(format != AL_NONE) alBufferData(id, format, wavbuf, wavlen, wavspec.freq);
                SDL_FreeWAV(wavbuf);
                if(format == AL_NONE || ALERR) continue;
            }

            return true;
        }
        if(trydl) requirepackage(PCK_AUDIO, name); // only try donwloading after trying all extensions
    }
    unload(); // loading failed
    return false;
}

void sbuffer::unload()
{
#ifdef __3DS__
    if(pending)
    {
        // Queued and finished jobs go now. One being decoded stops at its
        // next block, and the poll frees it.
        LightLock_Lock(&asynclock);
        loopv(asyncjobs)
        {
            asyncsound *job = asyncjobs[i];
            if(job->buf != this || iscancelled(*job)) continue;
            if(job->state == 1) cancel(*job);
            else { asyncjobs.remove(i--); freejob(job); }
        }
        LightLock_Unlock(&asynclock);
        pending = false;
    }
#endif
    if(!id) return;
    alclearerr();
    if(alIsBuffer(id)) alDeleteBuffers(1, &id);
    id = 0;
    ALERR;
}

// buffer collection, find or load data

bufferhashtable::~bufferhashtable() {}

sbuffer *bufferhashtable::find(const char *name)
{
    sbuffer *b = access(name);
    if(!b)
    {
        name = newstring(name);
        b = &(*this)[name];
        b->name = name;
    }
    return b;
}


// OpenAL error handling

void alclearerr()
{
    alGetError();
}

bool alerr(bool msg, int line, const char *s, ...)
{
    ALenum er = alGetError();
    if(er && msg)
    {
        const char *desc = "unknown";
        switch(er)
        {
            case AL_INVALID_NAME: desc = "invalid name"; break;
            case AL_INVALID_ENUM: desc = "invalid enum"; break;
            case AL_INVALID_VALUE: desc = "invalid value"; break;
            case AL_INVALID_OPERATION: desc = "invalid operation"; break;
            case AL_OUT_OF_MEMORY: desc = "out of memory"; break;
        }
        if(s)
        {
            defvformatstring(p, s, s);
            conoutf("\f3OpenAL Error (%X): %s, line %d, (%s)", er, desc, line, p);
        }
        else if(line) conoutf("\f3OpenAL Error (%X): %s, line %d", er, desc, line);
        else conoutf("\f3OpenAL Error (%X): %s", er, desc);
    }
    return er > 0;
}

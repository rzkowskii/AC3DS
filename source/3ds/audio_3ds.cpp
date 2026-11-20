#include <3ds.h>
#include "crash_3ds.h"
#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <zlib.h>

#include "audio_3ds.h"

struct ALCdevice_struct { bool open; };
struct ALCcontext_struct { bool created, current, batching; };

struct Ac3dsPcmBlocks
{
    struct Block { uint8_t *data; size_t bytes; };
    Block *blocks = nullptr;
    size_t count = 0, capacity = 0;
    int frames = 0, channels = 0;
    int16_t first[2] = {};
};

namespace
{
constexpr int dspChannel = 0;
constexpr int outputChannels = 2;
constexpr int blockFrames = 640;
// The output is refilled once per game frame, so the queued depth has to
// outlast the longest frame. libctru hands the DSP at most five buffers.
constexpr int waveCount = 5;
constexpr int minBufferedWaves = 3;
constexpr float outputRate = NDSP_SAMPLE_RATE;
constexpr unsigned int phaseBits = 16;
constexpr unsigned int phaseUnit = 1u << phaseBits;
constexpr int gainUnit = 4096;
constexpr size_t pcmBlockSamples = ac3ds_pcm_block_frames * outputChannels;
constexpr size_t decodedBlockCount = 16;
constexpr ALuint maxSources = 1024; // Upper bound of AssaultCube's soundchannels setting.
constexpr ALsizei streamBuffers = 2; // The soundtrack decoder keeps two buffers queued.
constexpr const char deviceName[] = "Nintendo 3DS DSP\0";

struct Sample
{
    bool allocated;
    ALsizei channels, rate;
    uint32_t frames, references;
    int16_t *pcm;
    Ac3dsPcmBlocks *packed;
};

struct DecodedBlock
{
    const Ac3dsPcmBlocks *owner = nullptr;
    size_t index = 0;
    uint64_t used = 0;
    int16_t pcm[pcmBlockSamples];
};

struct Voice
{
    bool allocated = false, relative = false, looping = false;
    ALenum state = AL_INITIAL;
    ALuint attached = 0, queue[streamBuffers] = {};
    ALsizei queued = 0, processed = 0;
    uint64_t phase = 0;
    float gain = 1.0f, pitch = 1.0f, reference = 1.0f, rolloff = 1.0f;
    float position[3] = {}, velocity[3] = {};
};

ALCdevice device;
ALCcontext context;
ALenum error = AL_NO_ERROR;
ALenum distanceModel = AL_INVERSE_DISTANCE_CLAMPED;
float listenerGain = 1.0f;
float listenerPosition[3] = {};
float orientation[6] = { 0, 0, -1, 0, 1, 0 };
float listenerRight[3] = { 1, 0, 0 };
float listenerRightLength = 1;
Voice voices[maxSources + 1];
ALuint highestVoice;
ALuint playingVoices;
Sample *samples;
ALuint sampleCapacity;
std::atomic<bool> suspendRequested{false};
bool paused;
ndspWaveBuf waves[waveCount];
int16_t *output;
int32_t mixBuffer[blockFrames * outputChannels];
DecodedBlock decodedBlocks[decodedBlockCount];
uint64_t blockClock;
z_stream pcmInflater;
bool pcmInflaterReady;
unsigned decodedPcmBlocks, submittedWaves;
Thread mixerThread;
LightEvent mixerWake, mixerDone;
bool mixerPending;
std::atomic<bool> mixerStop{false};
std::atomic<unsigned> mixerJob{0};
unsigned mixerSubmitted;
std::atomic<unsigned> mixerCompleted{0};

void finishMixer()
{
    if(!mixerPending) return;
    while(mixerCompleted.load(std::memory_order_acquire) != mixerSubmitted) LightEvent_Wait(&mixerDone);
    mixerPending = false;
}

void setOrientation(const float *values)
{
    if(!std::memcmp(orientation, values, sizeof(orientation))) return;
    std::memcpy(orientation, values, sizeof(orientation));
    // OpenAL defines the listener's right as forward cross up.
    listenerRight[0] = orientation[1]*orientation[5] - orientation[2]*orientation[4];
    listenerRight[1] = orientation[2]*orientation[3] - orientation[0]*orientation[5];
    listenerRight[2] = orientation[0]*orientation[4] - orientation[1]*orientation[3];
    const float *r = listenerRight;
    listenerRightLength = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
}

void setError(ALenum next)
{
    if(error == AL_NO_ERROR) error = next;
}

Voice *findVoice(ALuint id)
{
    return id && id <= maxSources && voices[id].allocated ? &voices[id] : nullptr;
}

Voice *requireVoice(ALuint id)
{
    Voice *voice = findVoice(id);
    if(!voice) setError(AL_INVALID_NAME);
    return voice;
}

Sample *findSample(ALuint id)
{
    return id && id < sampleCapacity && samples[id].allocated ? &samples[id] : nullptr;
}

void freeSample(Sample &sample)
{
    std::free(sample.pcm);
    ac3ds_audio_free_pcm_blocks(sample.packed);
    sample = {};
}

void detach(Voice &voice)
{
    if(voice.attached) --samples[voice.attached].references;
    for(ALsizei i = 0; i < voice.queued; ++i) --samples[voice.queue[i]].references;
    voice.attached = 0;
    voice.queued = voice.processed = 0;
    voice.phase = 0;
}

ALuint nextBuffer()
{
    for(ALuint i = 1; i < sampleCapacity; ++i)
        if(!samples[i].allocated) return i;

    const ALuint capacity = sampleCapacity ? sampleCapacity * 2 : 64;
    if(capacity <= sampleCapacity || capacity > SIZE_MAX / sizeof(Sample)) return 0;
    Sample *expanded = static_cast<Sample *>(std::realloc(samples, capacity * sizeof(Sample)));
    if(!expanded) return 0;
    std::memset(expanded + sampleCapacity, 0, (capacity - sampleCapacity) * sizeof(Sample));
    samples = expanded;
    const ALuint first = sampleCapacity ? sampleCapacity : 1;
    sampleCapacity = capacity;
    return first;
}

void resetContext()
{
    if(device.open) ndspChnWaveBufClear(dspChannel);
    for(ALuint id = 1; id <= highestVoice; ++id) voices[id] = Voice{};
    highestVoice = 0;
    playingVoices = 0;
    for(ALuint id = 1; id < sampleCapacity; ++id) freeSample(samples[id]);
    if(pcmInflaterReady) inflateEnd(&pcmInflater);
    pcmInflater = {};
    pcmInflaterReady = false;
    blockClock = 0;
    decodedPcmBlocks = submittedWaves = 0;
    std::free(samples);
    samples = nullptr;
    sampleCapacity = 0;
    error = AL_NO_ERROR;
    distanceModel = AL_INVERSE_DISTANCE_CLAMPED;
    listenerGain = 1.0f;
    std::memset(listenerPosition, 0, sizeof(listenerPosition));
    const float defaultOrientation[6] = { 0, 0, -1, 0, 1, 0 };
    setOrientation(defaultOrientation);
    context = {};
}

void changeState(Voice &voice, ALenum state)
{
    if(voice.state == state) return;
    if(voice.state == AL_PLAYING) --playingVoices;
    if(state == AL_PLAYING) ++playingVoices;
    voice.state = state;
}

void voiceGains(const Voice &voice, const Sample &sample, int &left, int &right)
{
    const float master = std::min(listenerGain, 16.0f);
    float gain = voice.gain;
    if(sample.channels == 2)
    {
        left = right = int(std::min(gain, 1.0f) * master * gainUnit);
        return;
    }

    const float x = voice.position[0] - (voice.relative ? 0.0f : listenerPosition[0]);
    const float y = voice.position[1] - (voice.relative ? 0.0f : listenerPosition[1]);
    const float z = voice.position[2] - (voice.relative ? 0.0f : listenerPosition[2]);
    const float distance = std::sqrt(x*x + y*y + z*z);
    if(distanceModel == AL_INVERSE_DISTANCE_CLAMPED && distance > voice.reference && voice.rolloff > 0)
        gain *= voice.reference / (voice.reference + voice.rolloff * (distance - voice.reference));

    float pan = 0.0f;
    if(distance > 0.0f)
    {
        if(voice.relative) pan = x / distance;
        else if(listenerRightLength > 0.0f)
            pan = (x*listenerRight[0] + y*listenerRight[1] + z*listenerRight[2]) / (distance * listenerRightLength);
    }
    pan = std::clamp(pan, -1.0f, 1.0f);
    gain = std::min(gain, 1.0f) * master;
    left = int(gain * std::sqrt((1.0f - pan) * 0.5f) * gainUnit);
    right = int(gain * std::sqrt((1.0f + pan) * 0.5f) * gainUnit);
}

int interpolate(int first, int second, int fraction)
{
    return first + ((second - first) * fraction >> 8);
}

template<int channels>
const int16_t *decodeBlock(const Ac3dsPcmBlocks &sample, size_t index)
{
    DecodedBlock *slot = &decodedBlocks[0];
    for(DecodedBlock &entry : decodedBlocks)
    {
        if(entry.owner == &sample && entry.index == index)
        {
            entry.used = ++blockClock;
            return entry.pcm;
        }
        if(entry.used < slot->used) slot = &entry;
    }

    const size_t frames = std::min<size_t>(ac3ds_pcm_block_frames, sample.frames - index * ac3ds_pcm_block_frames);
    const size_t count = frames * channels;
    const auto &source = sample.blocks[index];
    if(source.bytes == count * sizeof(int16_t)) std::memcpy(slot->pcm, source.data, source.bytes);
    else
    {
        uint8_t bytes[pcmBlockSamples * sizeof(int16_t)];
        pcmInflater.next_in = source.data;
        pcmInflater.avail_in = source.bytes;
        pcmInflater.next_out = bytes;
        pcmInflater.avail_out = count * sizeof(int16_t);
        if(!pcmInflaterReady || inflateReset(&pcmInflater) != Z_OK ||
           inflate(&pcmInflater, Z_FINISH) != Z_STREAM_END || pcmInflater.total_out != count * sizeof(int16_t))
            std::memset(slot->pcm, 0, count * sizeof(int16_t));
        else
        {
            ++decodedPcmBlocks;
            uint16_t previous[2] = {};
            for(size_t i = 0; i < count; ++i)
            {
                uint16_t &channel = previous[i % channels];
                channel += uint16_t(bytes[i]) | (uint16_t(bytes[count+i]) << 8);
                slot->pcm[i] = int16_t(channel);
            }
        }
    }
    slot->owner = &sample;
    slot->index = index;
    slot->used = ++blockClock;
    return slot->pcm;
}

const int16_t *firstSampleFrame(const Sample *sample)
{
    if(!sample) return nullptr;
    return sample->packed ? sample->packed->first : sample->pcm;
}

template<int channels, class Phase>
void mixPCM(const int16_t *pcm, uint32_t last, const int16_t *following, Phase phase, Phase step,
            int start, int count, int left, int right)
{
    for(int i = start; i < start + count; ++i, phase += step)
    {
        const uint32_t frame = phase >> phaseBits;
        const int16_t *current = pcm + frame * channels;
        const int16_t *next = frame < last ? current + channels : following ? following : current;
        const int fraction = (phase >> (phaseBits - 8)) & 255;
        const int leftSample = interpolate(current[0], next[0], fraction);
        const int rightSample = channels == 2 ? interpolate(current[1], next[1], fraction) : leftSample;
        mixBuffer[2*i] += leftSample * left / gainUnit;
        mixBuffer[2*i+1] += rightSample * right / gainUnit;
    }
}

// Each run is contiguous in decoded PCM. Rebase to its first source frame
// so ordinary clips fit in 32-bit Q16 even when the full clip needs 64 bits.
template<int channels>
void mixPCMRun(const int16_t *pcm, uint32_t frames, const int16_t *following, uint64_t phase, uint64_t step,
               int start, int count, int left, int right)
{
    const uint32_t first = phase >> phaseBits;
    const uint32_t fraction = phase & (phaseUnit-1);
    pcm += first*channels;
    const uint32_t last = frames-1-first;
    if(fraction + step*uint64_t(count-1) <= UINT32_MAX)
        mixPCM<channels>(pcm,last,following,fraction,uint32_t(step),start,count,left,right);
    else
        mixPCM<channels>(pcm,last,following,uint64_t(fraction),step,start,count,left,right);
}

int framesToBoundary(uint64_t distance, uint64_t step, int count)
{
    if(distance > step*uint64_t(count-1)) return count;
    return int((distance-1)/step)+1;
}

template<int channels>
void mixPackedSamples(const Sample &sample, const Sample *successor, uint64_t &phase, uint64_t step,
                      int start, int count, int left, int right)
{
    const Ac3dsPcmBlocks &packed = *sample.packed;
    while(count)
    {
        const uint32_t frame = phase >> phaseBits;
        const uint32_t block = frame / ac3ds_pcm_block_frames;
        const uint32_t base = block * ac3ds_pcm_block_frames;
        const uint32_t frames = std::min<uint32_t>(ac3ds_pcm_block_frames, sample.frames-base);
        const uint64_t localPhase = phase-(uint64_t(base)<<phaseBits);
        const uint64_t toEnd = (uint64_t(frames)<<phaseBits)-localPhase;
        const int chunk = framesToBoundary(toEnd,step,count);
        const int16_t *pcm = decodeBlock<channels>(packed,block);
        const int16_t *following = nullptr;
        if(((localPhase+step*uint64_t(chunk-1))>>phaseBits) == frames-1)
        {
            if(base+frames < sample.frames) following = decodeBlock<channels>(packed,block+1);
            else following = firstSampleFrame(successor);
        }
        mixPCMRun<channels>(pcm,frames,following,localPhase,step,start,chunk,left,right);
        phase += step*chunk;
        start += chunk;
        count -= chunk;
    }
}

const Sample *nextSample(const Voice &voice)
{
    if(voice.attached) return voice.looping ? &samples[voice.attached] : nullptr;
    if(voice.processed + 1 < voice.queued) return &samples[voice.queue[voice.processed + 1]];
    return voice.looping ? &samples[voice.queue[0]] : nullptr;
}

void advanceVoice(Voice &voice, uint64_t span)
{
    if(voice.attached)
    {
        if(voice.looping) voice.phase %= span;
        else changeState(voice, AL_STOPPED);
        return;
    }

    voice.phase -= span;
    if(++voice.processed == voice.queued)
    {
        if(voice.looping) voice.processed = 0;
        else changeState(voice, AL_STOPPED);
    }
}

void mixVoice(Voice &voice)
{
    int frame = 0;
    while(frame < blockFrames && voice.state == AL_PLAYING)
    {
        const ALuint id = voice.attached ? voice.attached : voice.queue[voice.processed];
        const Sample &sample = samples[id];
        const uint64_t span = uint64_t(sample.frames) << phaseBits;
        if(voice.phase >= span)
        {
            // Skip entire repeated queues at extreme pitch without walking them per sample.
            if(!voice.attached && voice.looping && voice.processed == 0)
            {
                uint64_t queueSpan = span;
                for(ALsizei i = 1; i < voice.queued; ++i) queueSpan += uint64_t(samples[voice.queue[i]].frames) << phaseBits;
                voice.phase %= queueSpan;
                if(voice.phase < span) continue;
            }
            advanceVoice(voice, span);
            continue;
        }

        const uint64_t step = uint64_t(std::clamp(double(sample.rate) * voice.pitch * phaseUnit / outputRate, 1.0, double(uint64_t(1) << 48)));
        const int count = framesToBoundary(span-voice.phase,step,blockFrames-frame);
        int left, right;
        voiceGains(voice, sample, left, right);
        const Sample *successor = nextSample(voice);
        if(sample.packed)
        {
            if(sample.channels == 1) mixPackedSamples<1>(sample, successor, voice.phase, step, frame, count, left, right);
            else mixPackedSamples<2>(sample, successor, voice.phase, step, frame, count, left, right);
        }
        else
        {
            const int16_t *following = firstSampleFrame(successor);
            if(sample.channels == 1) mixPCMRun<1>(sample.pcm,sample.frames,following,voice.phase,step,frame,count,left,right);
            else mixPCMRun<2>(sample.pcm,sample.frames,following,voice.phase,step,frame,count,left,right);
            voice.phase += step*count;
        }
        frame += count;
        if(voice.phase >= span) advanceVoice(voice, span);
    }
}

void fillWave(ndspWaveBuf &wave)
{
    std::memset(mixBuffer, 0, sizeof(mixBuffer));
    for(ALuint id = 1; id <= highestVoice; ++id)
        if(voices[id].state == AL_PLAYING) mixVoice(voices[id]);
    for(int i = 0; i < blockFrames * outputChannels; ++i)
        wave.data_pcm16[i] = int16_t(std::clamp(mixBuffer[i], int32_t(INT16_MIN), int32_t(INT16_MAX)));
    DSP_FlushDataCache(wave.data_pcm16, blockFrames * outputChannels * sizeof(int16_t));
    ndspChnWaveBufAdd(dspChannel, &wave);
    ++submittedWaves;
}

void serviceAudio()
{
    if(!device.open) return;
    const bool shouldPause = suspendRequested.load(std::memory_order_relaxed);
    if(paused != shouldPause)
    {
        ndspChnSetPaused(dspChannel, shouldPause);
        paused = shouldPause;
    }
    if(paused || !context.current || context.batching) return;

    // Queue enough 19.6 ms blocks to cover the recent frame time plus two
    // blocks of margin: three at 60 fps, four at 30, five at 20. With a fixed
    // two, any frame over 19.6 ms ran the channel dry (an audible hole). A
    // gap over 100 ms is a stall (map load, resume), not a frame time.
    static u64 lastService = 0;
    static u32 slowMs = 0;
    const u64 now = svcGetSystemTick();
    if(lastService)
    {
        const u32 ms = u32((now-lastService)*1000/SYSCLOCK_ARM11);
        if(ms <= 100) slowMs = std::max(ms, slowMs-slowMs/8);
    }
    lastService = now;
    const int bufferedWaves = std::clamp(2+int((slowMs*1000+19554)/19555), minBufferedWaves, waveCount);

    int pending = 0;
    for(const ndspWaveBuf &wave : waves)
        if(wave.status == NDSP_WBUF_QUEUED || wave.status == NDSP_WBUF_PLAYING) ++pending;
    if(!playingVoices) return;
    for(ndspWaveBuf &wave : waves)
    {
        if(pending >= bufferedWaves) break;
        if(wave.status != NDSP_WBUF_FREE && wave.status != NDSP_WBUF_DONE) continue;
        fillWave(wave);
        ++pending;
        if(!playingVoices) break;
    }
}

void mixerMain(void *)
{
    ac3ds_crash_install("audio mixer");
    for(;;)
    {
        LightEvent_Wait(&mixerWake);
        if(mixerStop.load(std::memory_order_acquire)) return;
        const unsigned job = mixerJob.load(std::memory_order_acquire);
        serviceAudio();
        mixerCompleted.store(job, std::memory_order_release);
        LightEvent_Signal(&mixerDone);
    }
}

void startMixer()
{
    bool newModel = false;
    if(R_FAILED(APT_CheckNew3DS(&newModel)) || !newModel) return;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    LightEvent_Init(&mixerWake, RESET_ONESHOT);
    LightEvent_Init(&mixerDone, RESET_ONESHOT);
    mixerStop.store(false, std::memory_order_relaxed);
    mixerThread = threadCreate(mixerMain, nullptr, 32 * 1024, priority, 2, false);
}

void stopMixer()
{
    if(!mixerThread) return;
    finishMixer();
    mixerStop.store(true, std::memory_order_release);
    LightEvent_Signal(&mixerWake);
    threadJoin(mixerThread, U64_MAX);
    threadFree(mixerThread);
    mixerThread = nullptr;
}
}

void ac3ds_audio_lifecycle_event(bool suspended)
{
    suspendRequested.store(suspended, std::memory_order_relaxed);
}

void ac3ds_audio_service()
{
    finishMixer();
    serviceAudio();
}

void ac3ds_audio_shutdown()
{
    if(!device.open) return;
    if(context.created) alcDestroyContext(&context);
    alcCloseDevice(&device);
}

void ac3ds_audio_stats(unsigned &decoded, unsigned &submitted)
{
    finishMixer();
    decoded = decodedPcmBlocks;
    submitted = submittedWaves;
}

ALCdevice *alcOpenDevice(const ALCchar *name)
{
    if(device.open || (name && *name && std::strcmp(name, deviceName))) return nullptr;
    if(R_FAILED(ndspInit())) return nullptr;
    output = static_cast<int16_t *>(linearAlloc(waveCount * blockFrames * outputChannels * sizeof(int16_t)));
    if(!output)
    {
        ndspExit();
        return nullptr;
    }
    std::memset(waves, 0, sizeof(waves));
    for(int i = 0; i < waveCount; ++i)
    {
        waves[i].data_pcm16 = output + i * blockFrames * outputChannels;
        waves[i].nsamples = blockFrames;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(dspChannel);
    ndspChnSetFormat(dspChannel, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetInterp(dspChannel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(dspChannel, outputRate);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(dspChannel, mix);
    device.open = true;
    paused = false;
    startMixer();
    return &device;
}

ALCboolean alcCloseDevice(ALCdevice *target)
{
    if(target != &device || !device.open || context.created) return AL_FALSE;
    stopMixer();
    ndspChnWaveBufClear(dspChannel);
    ndspExit();
    linearFree(output);
    output = nullptr;
    device.open = false;
    return AL_TRUE;
}

ALCcontext *alcCreateContext(ALCdevice *target, const ALCint *)
{
    if(target != &device || !device.open || context.created) return nullptr;
    context.created = true;
    return &context;
}

void alcDestroyContext(ALCcontext *target)
{
    finishMixer();
    if(target == &context && context.created) resetContext();
}

ALCboolean alcMakeContextCurrent(ALCcontext *target)
{
    finishMixer();
    if(target && (target != &context || !context.created)) return AL_FALSE;
    context.current = target != nullptr;
    return AL_TRUE;
}

void alcSuspendContext(ALCcontext *target)
{
    finishMixer();
    if(target == &context && context.current) context.batching = true;
}

void alcProcessContext(ALCcontext *target)
{
    finishMixer();
    if(target != &context || !context.current) return;
    context.batching = false;
    if(mixerThread && playingVoices && !suspendRequested.load(std::memory_order_relaxed))
    {
        // The game finishes writing listener and source state here. A
        // subsequent OpenAL operation joins before touching that state.
        mixerPending = true;
        mixerJob.store(++mixerSubmitted, std::memory_order_release);
        LightEvent_Signal(&mixerWake);
    }
    else serviceAudio();
}

const ALCchar *alcGetString(ALCdevice *target, ALCenum param)
{
    return (!target || target == &device) && param == ALC_DEVICE_SPECIFIER ? deviceName : nullptr;
}

ALCboolean alcIsExtensionPresent(ALCdevice *target, const ALCchar *name)
{
    return (!target || target == &device) && name && !std::strcmp(name, "ALC_ENUMERATION_EXT");
}

ALenum alGetError(void)
{
    const ALenum result = error;
    error = AL_NO_ERROR;
    return result;
}

const ALchar *alGetString(ALenum param)
{
    switch(param)
    {
        case AL_VENDOR: return "AssaultCube 3DS";
        case AL_VERSION: return "1.1 subset for AssaultCube";
        case AL_RENDERER: return "NDSP stereo PCM";
        default: setError(AL_INVALID_ENUM); return nullptr;
    }
}

void alDistanceModel(ALenum model)
{
    finishMixer();
    if(model == AL_INVERSE_DISTANCE_CLAMPED || model == AL_NONE) distanceModel = model;
    else setError(AL_INVALID_ENUM);
}

void alListenerf(ALenum param, ALfloat value)
{
    finishMixer();
    if(param != AL_GAIN) setError(AL_INVALID_ENUM);
    else if(!std::isfinite(value) || value < 0.0f) setError(AL_INVALID_VALUE);
    else listenerGain = value;
}

void alListenerfv(ALenum param, const ALfloat *values)
{
    finishMixer();
    const size_t length = param == AL_POSITION ? 3 : param == AL_ORIENTATION ? 6 : 0;
    if(!length) { setError(AL_INVALID_ENUM); return; }
    for(size_t i = 0; i < length; ++i)
        if(!std::isfinite(values[i])) { setError(AL_INVALID_VALUE); return; }
    if(param == AL_POSITION) std::memcpy(listenerPosition, values, sizeof(listenerPosition));
    else setOrientation(values);
}

void alGenSources(ALsizei count, ALuint *ids)
{
    finishMixer();
    if(count < 0) { setError(AL_INVALID_VALUE); return; }
    ALsizei generated = 0;
    for(ALuint id = 1; id <= maxSources && generated < count; ++id)
    {
        if(voices[id].allocated) continue;
        voices[id].allocated = true;
        ids[generated++] = id;
        highestVoice = std::max(highestVoice, id);
    }
    if(generated == count) return;
    alDeleteSources(generated, ids);
    for(ALsizei i = 0; i < generated; ++i) ids[i] = 0;
    setError(AL_OUT_OF_MEMORY);
}

void alDeleteSources(ALsizei count, const ALuint *ids)
{
    finishMixer();
    if(count < 0) { setError(AL_INVALID_VALUE); return; }
    for(ALsizei i = 0; i < count; ++i) if(!requireVoice(ids[i])) return;
    for(ALsizei i = 0; i < count; ++i)
    {
        Voice &voice = voices[ids[i]];
        changeState(voice, AL_INITIAL);
        detach(voice);
        voice = Voice{};
    }
    while(highestVoice && !voices[highestVoice].allocated) --highestVoice;
}

ALboolean alIsSource(ALuint id) { return findVoice(id) != nullptr; }

void alSourcef(ALuint id, ALenum param, ALfloat value)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    if(!std::isfinite(value) || value < 0.0f) { setError(AL_INVALID_VALUE); return; }
    switch(param)
    {
        case AL_GAIN: voice->gain = value; break;
        case AL_PITCH:
            if(value > 0.0f) voice->pitch = value;
            else setError(AL_INVALID_VALUE);
            break;
        case AL_REFERENCE_DISTANCE: voice->reference = value; break;
        case AL_ROLLOFF_FACTOR: voice->rolloff = value; break;
        case AL_SEC_OFFSET:
        {
            const Sample *sample = findSample(voice->attached);
            const double frame = sample ? double(value) * sample->rate : 0.0;
            if(!sample || frame >= sample->frames) setError(AL_INVALID_VALUE);
            else voice->phase = uint64_t(frame * phaseUnit);
            break;
        }
        default: setError(AL_INVALID_ENUM); break;
    }
}

void alSourcei(ALuint id, ALenum param, ALint value)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    switch(param)
    {
        case AL_BUFFER:
        {
            if(voice->state == AL_PLAYING || voice->state == AL_PAUSED) { setError(AL_INVALID_OPERATION); break; }
            Sample *sample = value ? findSample(ALuint(value)) : nullptr;
            if(value && !sample) { setError(AL_INVALID_VALUE); break; }
            detach(*voice);
            voice->attached = ALuint(value);
            if(sample) ++sample->references;
            changeState(*voice, AL_INITIAL);
            break;
        }
        case AL_LOOPING:
        case AL_SOURCE_RELATIVE:
            if(value != AL_FALSE && value != AL_TRUE) setError(AL_INVALID_VALUE);
            else if(param == AL_LOOPING) voice->looping = value;
            else voice->relative = value;
            break;
        default: setError(AL_INVALID_ENUM); break;
    }
}

void alSource3f(ALuint id, ALenum param, ALfloat x, ALfloat y, ALfloat z)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    float *values = param == AL_POSITION ? voice->position : param == AL_VELOCITY ? voice->velocity : nullptr;
    if(!values) { setError(AL_INVALID_ENUM); return; }
    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) { setError(AL_INVALID_VALUE); return; }
    values[0] = x;
    values[1] = y;
    values[2] = z;
}

void alGetSourcef(ALuint id, ALenum param, ALfloat *value)
{
    finishMixer();
    *value = 0.0f;
    const Voice *voice = requireVoice(id);
    if(!voice) return;
    if(param != AL_SEC_OFFSET) { setError(AL_INVALID_ENUM); return; }
    const Sample *sample = findSample(voice->attached);
    if(sample && sample->rate && voice->state != AL_STOPPED) *value = float(voice->phase) / (float(phaseUnit) * sample->rate);
}

void alGetSourcefv(ALuint id, ALenum param, ALfloat *values)
{
    values[0] = values[1] = values[2] = 0.0f;
    const Voice *voice = requireVoice(id);
    if(!voice) return;
    if(param != AL_POSITION) { setError(AL_INVALID_ENUM); return; }
    std::memcpy(values, voice->position, sizeof(voice->position));
}

void alGetSourcei(ALuint id, ALenum param, ALint *value)
{
    finishMixer();
    *value = 0;
    const Voice *voice = requireVoice(id);
    if(!voice) return;
    switch(param)
    {
        case AL_SOURCE_STATE: *value = voice->state; break;
        case AL_SOURCE_TYPE: *value = voice->attached ? AL_STATIC : voice->queued ? AL_STREAMING : AL_UNDETERMINED; break;
        case AL_BUFFERS_QUEUED: *value = voice->attached ? 1 : voice->queued; break;
        case AL_BUFFERS_PROCESSED: *value = voice->looping ? 0 : voice->processed; break;
        default: setError(AL_INVALID_ENUM); break;
    }
}

void alSourcePlay(ALuint id)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    if(!voice->attached && !voice->queued) { changeState(*voice, AL_STOPPED); return; }
    if(voice->attached && !samples[voice->attached].frames) { changeState(*voice, AL_STOPPED); return; }
    if(voice->state != AL_INITIAL && voice->state != AL_PAUSED) voice->phase = 0;
    voice->processed = 0;
    changeState(*voice, AL_PLAYING);
}

void alSourceStop(ALuint id)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    changeState(*voice, AL_STOPPED);
    voice->processed = voice->queued;
    voice->phase = 0;
}

void alSourceRewind(ALuint id)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    changeState(*voice, AL_INITIAL);
    voice->processed = 0;
    voice->phase = 0;
}

void alSourceQueueBuffers(ALuint id, ALsizei count, const ALuint *ids)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    if(count < 0 || count > streamBuffers - voice->queued) { setError(AL_INVALID_VALUE); return; }
    if(voice->attached) { setError(AL_INVALID_OPERATION); return; }
    const Sample *first = voice->queued ? findSample(voice->queue[0]) : nullptr;
    for(ALsizei i = 0; i < count; ++i)
    {
        const Sample *sample = findSample(ids[i]);
        if(!sample || !sample->frames) { setError(AL_INVALID_VALUE); return; }
        if(!first) first = sample;
        if(first->channels != sample->channels || first->rate != sample->rate) { setError(AL_INVALID_OPERATION); return; }
    }
    for(ALsizei i = 0; i < count; ++i)
    {
        voice->queue[voice->queued++] = ids[i];
        ++samples[ids[i]].references;
    }
}

void alSourceUnqueueBuffers(ALuint id, ALsizei count, ALuint *ids)
{
    finishMixer();
    Voice *voice = requireVoice(id);
    if(!voice) return;
    if(count < 0 || count > voice->processed) { setError(AL_INVALID_VALUE); return; }
    for(ALsizei i = 0; i < count; ++i)
    {
        ids[i] = voice->queue[i];
        --samples[ids[i]].references;
    }
    voice->queued -= count;
    voice->processed -= count;
    for(ALsizei i = 0; i < voice->queued; ++i) voice->queue[i] = voice->queue[i+count];
}

void alGenBuffers(ALsizei count, ALuint *ids)
{
    finishMixer();
    if(count < 0) { setError(AL_INVALID_VALUE); return; }
    for(ALsizei i = 0; i < count; ++i)
    {
        const ALuint id = nextBuffer();
        if(!id)
        {
            alDeleteBuffers(i, ids);
            for(ALsizei j = 0; j < i; ++j) ids[j] = 0;
            setError(AL_OUT_OF_MEMORY);
            return;
        }
        samples[id].allocated = true;
        ids[i] = id;
    }
}

void alDeleteBuffers(ALsizei count, const ALuint *ids)
{
    finishMixer();
    if(count < 0) { setError(AL_INVALID_VALUE); return; }
    for(ALsizei i = 0; i < count; ++i)
    {
        if(!ids[i]) continue;
        const Sample *sample = findSample(ids[i]);
        if(!sample) { setError(AL_INVALID_NAME); return; }
        if(sample->references) { setError(AL_INVALID_OPERATION); return; }
    }
    for(ALsizei i = 0; i < count; ++i)
    {
        if(!ids[i]) continue;
        freeSample(samples[ids[i]]);
    }
}

ALboolean alIsBuffer(ALuint id) { return findSample(id) != nullptr; }

bool ac3ds_audio_take_pcm16(unsigned buffer, int16_t *pcm, int frames, int channels, int rate)
{
    finishMixer();
    Sample *sample = findSample(buffer);
    if(!sample || sample->references || !pcm || frames <= 0 || (channels != 1 && channels != 2) || rate <= 0)
        return false;
    std::free(sample->pcm);
    ac3ds_audio_free_pcm_blocks(sample->packed);
    sample->packed = nullptr;
    sample->pcm = pcm;
    sample->frames = uint32_t(frames);
    sample->channels = channels;
    sample->rate = rate;
    return true;
}

Ac3dsPcmBlocks *ac3ds_audio_create_pcm_blocks(int channels)
{
    finishMixer();
    if(channels != 1 && channels != 2) return nullptr;
    if(!pcmInflaterReady)
    {
        // Complete an inflate during loading so zlib also allocates its lazy window here.
        if(inflateInit(&pcmInflater) != Z_OK) return nullptr;
        uint8_t input[] = {0x78, 0x01, 0x63, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01};
        uint8_t output[2];
        pcmInflater.next_in = input;
        pcmInflater.avail_in = sizeof(input);
        pcmInflater.next_out = output;
        pcmInflater.avail_out = sizeof(output);
        if(inflate(&pcmInflater, Z_NO_FLUSH) != Z_STREAM_END || pcmInflater.total_out != 1)
        {
            inflateEnd(&pcmInflater);
            pcmInflater = {};
            return nullptr;
        }
        pcmInflaterReady = true;
    }
    Ac3dsPcmBlocks *sample = new(std::nothrow) Ac3dsPcmBlocks;
    if(sample) sample->channels = channels;
    return sample;
}

bool ac3ds_audio_append_pcm_block(Ac3dsPcmBlocks *sample, const int16_t *pcm, int frames)
{
    if(!sample || !pcm || frames <= 0 || frames > ac3ds_pcm_block_frames ||
       sample->frames > INT32_MAX - frames || sample->frames % ac3ds_pcm_block_frames)
        return false;

    if(sample->count == sample->capacity)
    {
        const size_t capacity = sample->capacity ? sample->capacity * 2 : 16;
        auto *blocks = static_cast<Ac3dsPcmBlocks::Block *>(
            std::realloc(sample->blocks, capacity * sizeof(Ac3dsPcmBlocks::Block)));
        if(!blocks) return false;
        sample->blocks = blocks;
        sample->capacity = capacity;
    }

    const size_t count = frames * sample->channels;
    const size_t rawBytes = count * sizeof(*pcm);
    uint8_t delta[pcmBlockSamples * sizeof(*pcm)];
    uint8_t compressed[pcmBlockSamples * sizeof(*pcm)];
    uint16_t previous[2] = {};
    for(size_t i = 0; i < count; ++i)
    {
        uint16_t &channel = previous[i % sample->channels];
        const uint16_t value = pcm[i];
        const uint16_t difference = value - channel;
        delta[i] = uint8_t(difference);
        delta[count+i] = uint8_t(difference >> 8);
        channel = value;
    }

    uLongf bytes = rawBytes;
    const bool packed = compress2(compressed, &bytes, delta, rawBytes, Z_BEST_SPEED) == Z_OK && bytes < rawBytes;
    if(!packed) bytes = rawBytes;
    auto *data = static_cast<uint8_t *>(std::malloc(bytes));
    if(!data) return false;
    std::memcpy(data, packed ? compressed : reinterpret_cast<const uint8_t *>(pcm), bytes);
    if(!sample->count) std::memcpy(sample->first, pcm, sample->channels * sizeof(*pcm));
    sample->blocks[sample->count++] = { data, size_t(bytes) };
    sample->frames += frames;
    return true;
}

void ac3ds_audio_free_pcm_blocks(Ac3dsPcmBlocks *sample)
{
    finishMixer();
    if(!sample) return;
    for(DecodedBlock &entry : decodedBlocks) if(entry.owner == sample)
    {
        entry.owner = nullptr;
        entry.used = 0;
    }
    for(size_t i = 0; i < sample->count; ++i) std::free(sample->blocks[i].data);
    std::free(sample->blocks);
    delete sample;
}

bool ac3ds_audio_prepare_pcm_blocks()
{
    Ac3dsPcmBlocks *probe = ac3ds_audio_create_pcm_blocks(1);
    if(!probe) return false;
    ac3ds_audio_free_pcm_blocks(probe);
    return true;
}

Ac3dsPcmBlocks *ac3ds_audio_new_pcm_blocks(int channels)
{
    if(!pcmInflaterReady || (channels != 1 && channels != 2)) return nullptr;
    Ac3dsPcmBlocks *sample = new(std::nothrow) Ac3dsPcmBlocks;
    if(sample) sample->channels = channels;
    return sample;
}

void ac3ds_audio_discard_pcm_blocks(Ac3dsPcmBlocks *sample)
{
    if(!sample) return;
    for(size_t i = 0; i < sample->count; ++i) std::free(sample->blocks[i].data);
    std::free(sample->blocks);
    delete sample;
}

bool ac3ds_audio_take_pcm_blocks(unsigned buffer, Ac3dsPcmBlocks *packed, int rate)
{
    finishMixer();
    Sample *sample = findSample(buffer);
    if(!sample || sample->references || !packed || !packed->frames || rate <= 0) return false;
    std::free(sample->pcm);
    sample->pcm = nullptr;
    ac3ds_audio_free_pcm_blocks(sample->packed);
    sample->packed = packed;
    sample->frames = packed->frames;
    sample->channels = packed->channels;
    sample->rate = rate;
    return true;
}

void alBufferData(ALuint id, ALenum format, const void *data, ALsizei bytes, ALsizei rate)
{
    finishMixer();
    Sample *sample = findSample(id);
    if(!sample) { setError(AL_INVALID_NAME); return; }
    if(sample->references) { setError(AL_INVALID_OPERATION); return; }
    int channels, sampleBytes;
    switch(format)
    {
        case AL_FORMAT_MONO8: channels = 1; sampleBytes = 1; break;
        case AL_FORMAT_MONO16: channels = 1; sampleBytes = 2; break;
        case AL_FORMAT_STEREO8: channels = 2; sampleBytes = 1; break;
        case AL_FORMAT_STEREO16: channels = 2; sampleBytes = 2; break;
        default: setError(AL_INVALID_ENUM); return;
    }
    if(!data || bytes <= 0 || rate <= 0 || bytes % (channels * sampleBytes)) { setError(AL_INVALID_VALUE); return; }
    const size_t count = size_t(bytes / sampleBytes);
    if(count > SIZE_MAX / sizeof(int16_t)) { setError(AL_OUT_OF_MEMORY); return; }
    int16_t *pcm = static_cast<int16_t *>(std::realloc(sample->pcm, count * sizeof(int16_t)));
    if(!pcm) { setError(AL_OUT_OF_MEMORY); return; }
    if(sampleBytes == 2) std::memcpy(pcm, data, size_t(bytes));
    else
    {
        const uint8_t *input = static_cast<const uint8_t *>(data);
        for(size_t i = 0; i < count; ++i) pcm[i] = int16_t((int(input[i]) - 128) * 256);
    }
    ac3ds_audio_free_pcm_blocks(sample->packed);
    sample->packed = nullptr;
    sample->pcm = pcm;
    sample->channels = channels;
    sample->rate = rate;
    sample->frames = uint32_t(count / channels);
}

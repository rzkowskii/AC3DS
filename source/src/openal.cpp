// simple OpenAL call wrappers

#include "cube.h"
#ifdef __3DS__
#include "audio_3ds.h"

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

bool sbuffer::load(bool trydl)
{
    if(!name) return false;
    if(id) return true;
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
                    const bool loaded = load_native_ogg_sample(id, oggfile, *info);
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

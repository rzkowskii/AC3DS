// audio streaming

#include "cube.h"

#define DEBUGCOND (audiodebug==1)

// ogg compat

static size_t oggcallbackread(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    stream *s = (stream *)datasource;
    return s && size && nmemb <= size_t(INT_MAX)/size ? s->read(ptr, int(size*nmemb))/size : 0;
}

static int oggcallbackseek(void *datasource, ogg_int64_t offset, int whence)
{
    stream *s = (stream *)datasource;
    return s && offset >= LONG_MIN && offset <= LONG_MAX && s->seek(long(offset), whence) ? 0 : -1;
}

static int oggcallbackclose(void *datasource)
{
    stream *s = (stream *)datasource;
    if(!s) return -1;
    delete s;
    return 0;
}

static long oggcallbacktell(void *datasource)
{
    stream *s = (stream *)datasource;
    return s ? s->tell() : -1;
}

ov_callbacks oggcallbacks = { oggcallbackread, oggcallbackseek, oggcallbackclose, oggcallbacktell };

bool compatiblevorbis(OggVorbis_File *file, const vorbis_info *info)
{
    if(!info || (info->channels != 1 && info->channels != 2) || info->rate <= 0 || info->rate > INT_MAX) return false;
    for(long i = 0, links = ov_streams(file); i < links; ++i)
    {
        const vorbis_info *link = ov_info(file, int(i));
        if(!link || link->channels != info->channels || link->rate != info->rate) return false;
    }
    return true;
}

long readvorbispcm(OggVorbis_File *file, char *buffer, int length, int *bitstream)
{
#ifdef __3DS__
    // Tremor produces native-endian signed 16-bit PCM.
    return ov_read(file, buffer, length, bitstream);
#else
    return ov_read(file, buffer, length, isbigendian(), 2, 1, bitstream);
#endif
}

// ogg audio streaming

oggstream::oggstream() : valid(false), isopen(false), draining(false), bufferids{0, 0}, src(NULL), volume(1.0f)
{
    reset();

    // grab a source and keep it during the whole lifetime
    src = sourcescheduler::instance().newsource(SP_HIGHEST, camera1->o);
    if(src)
    {
        if(src->valid)
        {
            src->init(this);
            src->sourcerelative(true);
        }
        else
        {
            sourcescheduler::instance().releasesource(src);
            src = NULL;
        }
    }

    if(!src) return;

    alclearerr();
    alGenBuffers(2, bufferids);
    valid = !ALERR;
}

oggstream::~oggstream()
{
    reset();

    if(src) sourcescheduler::instance().releasesource(src);

    if(alIsBuffer(bufferids[0]) || alIsBuffer(bufferids[1]))
    {
        alclearerr();
        alDeleteBuffers(2, bufferids);
        ALERR;
    }
}

void oggstream::reset()
{
    name[0] = '\0';

    // stop playing
    if(src)
    {
        src->stop();
        src->unqueueallbuffers();
        src->buffer(0);
    }
    format = AL_NONE;

    // reset file handler
    if(isopen)
    {
        ov_clear(&oggfile);
        isopen = false;
    }
    draining = false;
    info = NULL;
    totalseconds = 0.0f;

    // default settings
    startmillis = endmillis = startfademillis = endfademillis = 0;
    gain = 1.0f; // reset gain but not volume setting
    looping = false;
}

bool oggstream::open(const char *f)
{
    ASSERT(valid);
    if(!f) return false;
    if(playing() || isopen) reset();

    const char *exts[] = { "", ".wav", ".ogg" };
    string filepath;

    loopi(sizeof(exts)/sizeof(exts[0]))
    {
        formatstring(filepath)("packages/audio/soundtracks/%s%s", f, exts[i]);
        ::stream *file = openfile(path(filepath), "rb");
        if(!file) continue;

        isopen = !ov_open_callbacks(file, &oggfile, NULL, 0, oggcallbacks);
        if(!isopen)
        {
            delete file;
            continue;
        }

        info = ov_info(&oggfile, -1);
        if(!compatiblevorbis(&oggfile, info))
        {
            reset();
            continue;
        }
        format = info->channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
#ifdef __3DS__
        totalseconds = ov_time_total(&oggfile, -1) / 1000.0;
#else
        totalseconds = ov_time_total(&oggfile, -1);
#endif
        copystring(name, f);

        return true;
    }
    return false;
}

void oggstream::onsourcereassign(source *s)
{
    // should NEVER happen because streams do have the highest priority, see constructor
    ASSERT(0);
    if(src && src==s)
    {
        reset();
        src = NULL;
    }
}

bool oggstream::stream(ALuint bufid)
{
    ASSERT(valid);

    loopi(2)
    {
        char pcm[BUFSIZE];
        ALsizei size = 0;
        int bitstream;
        int holes = 0;
        while(size < BUFSIZE)
        {
            long bytes = readvorbispcm(&oggfile, pcm + size, BUFSIZE - size, &bitstream);
            if(bytes > 0) { size += bytes; holes = 0; }
            else if(bytes == OV_HOLE && ++holes <= 4) continue;
            else if(bytes < 0) return false;
            else break; // done
        }

        if(size==0)
        {
            if(looping && !ov_pcm_seek(&oggfile, 0)) continue; // try again to replay
            else return false;
        }

        alclearerr();
        alBufferData(bufid, format, pcm, size, info->rate);
        return !ALERR;
    }

    return false;
}

bool oggstream::update()
{
    ASSERT(valid);
    if(!isopen) return false;

    // update buffer queue
    ALint processed;
    alGetSourcei(src->id, AL_BUFFERS_PROCESSED, &processed);
    loopi(processed)
    {
        ALuint buffer = 0;
        alclearerr();
        alSourceUnqueueBuffers(src->id, 1, &buffer);
        if(ALERR) break;
        if(draining) continue;
        draining = !stream(buffer) || !src->queuebuffers(1, &buffer);
    }
    ALint queued = 0;
    alGetSourcei(src->id, AL_BUFFERS_QUEUED, &queued);
    bool active = queued > 0;
    // A game-thread stall can exhaust both buffers before the next update.
    if(active && !playing()) src->play();

    if(active)
    {
        float fadegain = 1.0f;
        // fade in
        if(startfademillis > 0) fadegain = clamp((lastmillis-startmillis)/(float)startfademillis, 0.0f, 1.0f);

        // fade out
        if(endmillis > 0)
        {
            if(lastmillis >= endmillis) active = false;
            else if(endfademillis > 0) fadegain = min(fadegain, (endmillis-lastmillis)/(float)endfademillis);
        }
        if(active && (startfademillis > 0 || endmillis > 0)) setgain(fadegain);
    }

    if(!active) reset(); // reset stream if the end is reached
    return active;
}

bool oggstream::playing()
{
    ASSERT(valid);
    return src->playing();
}

void oggstream::updategain()
{
    ASSERT(valid);
    src->gain(gain*volume);
}

void oggstream::setgain(float g)
{
    ASSERT(valid);
    gain = g;
    updategain();
}

void oggstream::setvolume(float v)
{
    ASSERT(valid);
    volume = v;
    updategain();
}

void oggstream::fadein(int startmillis, int fademillis)
{
    ASSERT(valid);
    setgain(0.01f);
    this->startmillis = startmillis;
    this->startfademillis = fademillis;
}

void oggstream::fadeout(int endmillis, int fademillis)
{
    ASSERT(valid);
    this->endmillis = endmillis;
    this->endfademillis = fademillis;
}

bool oggstream::playback(bool looping)
{
    ASSERT(valid);
    if(playing()) return true;
    this->looping = looping;
    if(!stream(bufferids[0])) { reset(); return false; }
    draining = !stream(bufferids[1]);
    if(!startmillis && !endmillis && !startfademillis && !endfademillis) setgain(1.0f);

    updategain();
    if(src->queuebuffers(draining ? 1 : 2, bufferids) && src->play()) return true;
    reset();
    return false;
}

void oggstream::seek(double offset)
{
    ASSERT(valid);
    if(totalseconds <= 0.0) return;
    double seconds = fmod(offset / 1000.0, totalseconds);
    if(seconds < 0.0) seconds += totalseconds;
#ifdef __3DS__
    ov_time_seek_page(&oggfile, ogg_int64_t(seconds * 1000.0));
#else
    ov_time_seek_page(&oggfile, seconds);
#endif
}


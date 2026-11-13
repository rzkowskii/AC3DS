#pragma once

// OpenAL declarations used by the AssaultCube engine's audio layer.
#ifdef __cplusplus
extern "C" {
#endif

typedef char ALboolean;
typedef char ALchar;
typedef int ALint;
typedef unsigned int ALuint;
typedef int ALsizei;
typedef int ALenum;
typedef float ALfloat;

enum
{
    AL_FALSE = 0,
    AL_TRUE = 1,
    AL_NONE = 0,
    AL_SOURCE_RELATIVE = 0x0202,
    AL_PITCH = 0x1003,
    AL_POSITION = 0x1004,
    AL_VELOCITY = 0x1006,
    AL_LOOPING = 0x1007,
    AL_BUFFER = 0x1009,
    AL_GAIN = 0x100A,
    AL_ORIENTATION = 0x100F,
    AL_SOURCE_STATE = 0x1010,
    AL_INITIAL = 0x1011,
    AL_PLAYING = 0x1012,
    AL_PAUSED = 0x1013,
    AL_STOPPED = 0x1014,
    AL_BUFFERS_QUEUED = 0x1015,
    AL_BUFFERS_PROCESSED = 0x1016,
    AL_REFERENCE_DISTANCE = 0x1020,
    AL_ROLLOFF_FACTOR = 0x1021,
    AL_SEC_OFFSET = 0x1024,
    AL_SOURCE_TYPE = 0x1027,
    AL_STATIC = 0x1028,
    AL_STREAMING = 0x1029,
    AL_UNDETERMINED = 0x1030,
    AL_FORMAT_MONO8 = 0x1100,
    AL_FORMAT_MONO16 = 0x1101,
    AL_FORMAT_STEREO8 = 0x1102,
    AL_FORMAT_STEREO16 = 0x1103,
    AL_NO_ERROR = 0,
    AL_INVALID_NAME = 0xA001,
    AL_INVALID_ENUM = 0xA002,
    AL_INVALID_VALUE = 0xA003,
    AL_INVALID_OPERATION = 0xA004,
    AL_OUT_OF_MEMORY = 0xA005,
    AL_VENDOR = 0xB001,
    AL_VERSION = 0xB002,
    AL_RENDERER = 0xB003,
    AL_INVERSE_DISTANCE_CLAMPED = 0xD002
};

ALenum alGetError(void);
const ALchar *alGetString(ALenum param);
void alDistanceModel(ALenum model);
void alListenerf(ALenum param, ALfloat value);
void alListenerfv(ALenum param, const ALfloat *values);
void alGenSources(ALsizei count, ALuint *ids);
void alDeleteSources(ALsizei count, const ALuint *ids);
ALboolean alIsSource(ALuint id);
void alSourcef(ALuint id, ALenum param, ALfloat value);
void alSourcei(ALuint id, ALenum param, ALint value);
void alSource3f(ALuint id, ALenum param, ALfloat x, ALfloat y, ALfloat z);
void alGetSourcef(ALuint id, ALenum param, ALfloat *value);
void alGetSourcefv(ALuint id, ALenum param, ALfloat *values);
void alGetSourcei(ALuint id, ALenum param, ALint *value);
void alSourcePlay(ALuint id);
void alSourceStop(ALuint id);
void alSourceRewind(ALuint id);
void alSourceQueueBuffers(ALuint id, ALsizei count, const ALuint *ids);
void alSourceUnqueueBuffers(ALuint id, ALsizei count, ALuint *ids);
void alGenBuffers(ALsizei count, ALuint *ids);
void alDeleteBuffers(ALsizei count, const ALuint *ids);
ALboolean alIsBuffer(ALuint id);
void alBufferData(ALuint id, ALenum format, const void *data, ALsizei bytes, ALsizei rate);

#ifdef __cplusplus
}
#endif

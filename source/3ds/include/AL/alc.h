#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ALCdevice_struct ALCdevice;
typedef struct ALCcontext_struct ALCcontext;
typedef char ALCboolean;
typedef char ALCchar;
typedef int ALCenum;
typedef int ALCint;

enum { ALC_DEVICE_SPECIFIER = 0x1005 };

ALCdevice *alcOpenDevice(const ALCchar *name);
ALCboolean alcCloseDevice(ALCdevice *device);
ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attributes);
void alcDestroyContext(ALCcontext *context);
ALCboolean alcMakeContextCurrent(ALCcontext *context);
void alcSuspendContext(ALCcontext *context);
void alcProcessContext(ALCcontext *context);
const ALCchar *alcGetString(ALCdevice *device, ALCenum param);
ALCboolean alcIsExtensionPresent(ALCdevice *device, const ALCchar *name);

#ifdef __cplusplus
}
#endif

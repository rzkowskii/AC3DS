#pragma once

#include <cstdint>

// Only records state. Safe to call from the applet lifecycle hook.
void ac3ds_audio_lifecycle_event(bool suspended);

// Handles lifecycle transitions and submits PCM on the game thread.
void ac3ds_audio_service();

// Retires native audio even when engine cleanup cannot run.
void ac3ds_audio_shutdown();
void ac3ds_audio_stats(unsigned &decoded, unsigned &submitted);

// Takes ownership of malloc-allocated signed PCM on success.
bool ac3ds_audio_take_pcm16(unsigned buffer, int16_t *pcm, int frames, int channels, int rate);

constexpr int ac3ds_pcm_block_frames = 4096;
struct Ac3dsPcmBlocks;
Ac3dsPcmBlocks *ac3ds_audio_create_pcm_blocks(int channels);
bool ac3ds_audio_append_pcm_block(Ac3dsPcmBlocks *sample, const int16_t *pcm, int frames);
void ac3ds_audio_free_pcm_blocks(Ac3dsPcmBlocks *sample);
bool ac3ds_audio_take_pcm_blocks(unsigned buffer, Ac3dsPcmBlocks *sample, int rate);
// Background decoding: prepare once on the main thread, then a worker may
// create, append and discard blocks that the mixer has never seen.
bool ac3ds_audio_prepare_pcm_blocks();
Ac3dsPcmBlocks *ac3ds_audio_new_pcm_blocks(int channels);
void ac3ds_audio_discard_pcm_blocks(Ac3dsPcmBlocks *sample);

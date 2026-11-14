#pragma once

// Map texture decode prefetch on the New 3DS's second application core.
// begin -> add (in load order) -> take (from loadsurface) -> end.

struct SDL_Surface;

void ac3ds_texprefetch_begin();
// Queues a file path exactly as findfile resolved it.
void ac3ds_texprefetch_add(const char *resolvedpath);
// Returns the decoded surface and sets prefetched when the worker decoded it.
// Returns nullptr when the path was not queued or not started; the caller
// then decodes it itself.
SDL_Surface *ac3ds_texprefetch_take(const char *resolvedpath, bool &prefetched);
void ac3ds_texprefetch_end(void (*log)(const char *) = nullptr);

// Small-block allocator in front of newlib's malloc.
//
// The game keeps 50,000 to 100,000 blocks of 256 bytes or less alive (script
// identifiers, strings, bot and waypoint nodes), about 2 MB in all. In the
// general heap they get carved out of whatever hole is free, including the
// large holes that map loads leave behind, and the ones that outlive a map pin
// those holes apart. After eight to ten map changes the heap had over 20 MB
// free in pieces no larger than about 1 MB, a 3 to 5 MB request had to extend
// the heap, and the game ran out of memory ("operator new failed").
//
// Blocks of up to 512 bytes therefore come from one region, divided into
// 4 KiB pages that each serve a single size class at a time. A freed block
// goes back to its page, and a page whose blocks are all free goes back to
// the region for any class to take, so the sizes one map needs most do not
// hold pages that the next map wants for other sizes. The region is taken
// from the heap once, on first use; if it cannot be had, or fills up, small
// blocks fall back to the general heap.
//
// devkitARM's malloc() gives every block of 32 bytes or more its own cache
// lines: it calls memalign(32, size). The pool keeps that property. Classes
// from 32 bytes up are multiples of 32 and the region is page aligned, so
// those blocks start on a 32-byte boundary and share no cache line. In the
// general heap each such memalign also splits off a small leftover chunk,
// which made the fragmentation worse.
//
// malloc, calloc, realloc, memalign and free are wrapped at link time.
// newlib's own calloc, realloc and memalign read the chunk header of blocks
// they get from malloc, so while one of them runs, nested malloc calls skip
// the pool ("bypass").

#include <3ds.h>
#include <reent.h>
#include <malloc.h>
#include <cstdint>
#include <cstring>

extern "C" {
void *__real__malloc_r(struct _reent *, size_t);
void __real__free_r(struct _reent *, void *);
void *__real__realloc_r(struct _reent *, void *, size_t);
void *__real__calloc_r(struct _reent *, size_t, size_t);
void *__real__memalign_r(struct _reent *, size_t, size_t);
}

#ifdef AC3DS_HEAPTRACK
void ac3ds_heaptrack_record(void *p, size_t size, void *caller);
void ac3ds_heaptrack_forget(void *p);
#define TRACK_ALLOC(p, size) ac3ds_heaptrack_record((p), (size), __builtin_return_address(0))
#define TRACK_FREE(p) ac3ds_heaptrack_forget(p)
#else
#define TRACK_ALLOC(p, size) ((void)0)
#define TRACK_FREE(p) ((void)0)
#endif

unsigned ac3ds_crash_frame(); // crash_3ds.cpp: main-loop frames so far

namespace
{
constexpr size_t regionBytes = 6u << 20, pageBytes = 4096, pageCount = regionBytes / pageBytes, maxSmall = 512;
constexpr size_t cacheLine = 32;
// Below one cache line: 16, 24. From there up, multiples of the cache line.
constexpr uint16_t classBytes[] = { 16, 24, 32, 64, 96, 128, 160, 192, 224, 256, 320, 384, 448, 512 };
constexpr int classCount = int(sizeof(classBytes) / sizeof(*classBytes));
struct BlocksPerPage
{
    uint16_t count[classCount];
    constexpr BlocksPerPage() : count() { for(int c = 0; c < classCount; ++c) count[c] = uint16_t(4096 / classBytes[c]); }
};
constexpr BlocksPerPage blocksPerPageTable;

struct FreeBlock { FreeBlock *next; };
struct Page
{
    FreeBlock *freed;    // blocks handed back, newest first
    uint16_t live;       // blocks in use
    uint16_t fresh;      // blocks never handed out yet start at this index
    uint16_t prev, next; // neighbours in the class's list of pages with room; next also links unused pages
    uint8_t cls;         // class + 1, 0 = not serving a class
};

uint8_t classOfEighths[maxSmall / 8 + 1]; // (size+7)/8 -> class
Page pages[pageCount];
uint16_t roomy[classCount]; // per class, first page with a block to give; 0 = none
uint16_t unusedPages;       // pages handed back, linked through next; 0 = none
char *region, *regionEnd;
// Page 0 stays unused: newlib's realloc compares the block malloc returned
// with the chunk that follows the old block, and the region's own first
// address is such a chunk boundary. That also makes 0 the "no page" index.
uint32_t nextPage = 1;    // pages from here on were never used
uint32_t pagesInUse, pagesPeak;
uint32_t liveBlocks;
uint32_t fullFallbacks;   // small requests the pool could not serve (region missing or full); under the lock
uint32_t nestedFallbacks; // small mallocs made inside newlib's own calloc, realloc or memalign
uint32_t lastFailedBytes; // size of the last request that nothing could serve, for the fatal-error report
uint32_t lastFailedFrame; // main-loop frame it failed in: the caller may have recovered long before a report
bool unavailable;
LightLock lock = 1; // unlocked, as LightLock_Init leaves it
thread_local int bypass;

inline bool pooled(const void *p) { return region && static_cast<const char *>(p) >= region && static_cast<const char *>(p) < regionEnd; }
inline uint32_t pageOf(const void *p) { return uint32_t((static_cast<const char *>(p) - region) / pageBytes); }
inline int classAt(const void *p) { return pages[pageOf(p)].cls - 1; }
inline uint16_t blocksPerPage(int c) { return blocksPerPageTable.count[c]; }

inline void linkRoomy(int c, uint32_t i)
{
    Page &page = pages[i];
    page.prev = 0;
    page.next = roomy[c];
    if(roomy[c]) pages[roomy[c]].prev = uint16_t(i);
    roomy[c] = uint16_t(i);
}

inline void unlinkRoomy(int c, uint32_t i)
{
    Page &page = pages[i];
    if(page.prev) pages[page.prev].next = page.next;
    else roomy[c] = page.next;
    if(page.next) pages[page.next].prev = page.prev;
}

void *take(struct _reent *r, size_t size)
{
    LightLock_Lock(&lock);
    if(!region && !unavailable)
    {
        int c = 0;
        for(size_t i = 0; i <= maxSmall / 8; ++i)
        {
            while(classBytes[c] < i * 8) ++c;
            classOfEighths[i] = uint8_t(c);
        }
        region = static_cast<char *>(__real__memalign_r(r, pageBytes, regionBytes));
        if(region) regionEnd = region + regionBytes;
        else unavailable = true;
    }
    void *p = nullptr;
    if(region)
    {
        const int c = classOfEighths[(size + 7) >> 3];
        uint32_t i = roomy[c];
        if(!i)
        {
            // A page for this class: one that was handed back, else a new one.
            if(unusedPages) { i = unusedPages; unusedPages = pages[i].next; }
            else if(nextPage < pageCount) i = nextPage++;
            if(i)
            {
                Page &page = pages[i];
                page.freed = nullptr;
                page.live = page.fresh = 0;
                page.cls = uint8_t(c + 1);
                linkRoomy(c, i);
                if(++pagesInUse > pagesPeak) pagesPeak = pagesInUse;
            }
        }
        if(i)
        {
            Page &page = pages[i];
            if(page.freed)
            {
                p = page.freed;
                page.freed = page.freed->next;
            }
            else p = region + i * pageBytes + size_t(page.fresh++) * classBytes[c];
            ++page.live;
            if(!page.freed && page.fresh == blocksPerPage(c)) unlinkRoomy(c, i); // no room left
            ++liveBlocks;
        }
    }
    if(!p) ++fullFallbacks;
    LightLock_Unlock(&lock);
    return p;
}

void give(void *p)
{
    LightLock_Lock(&lock);
    const uint32_t i = pageOf(p);
    Page &page = pages[i];
    if(page.cls && page.live)
    {
        const int c = page.cls - 1;
        const bool hadRoom = page.freed || page.fresh < blocksPerPage(c);
        FreeBlock *block = static_cast<FreeBlock *>(p);
        block->next = page.freed;
        page.freed = block;
        --liveBlocks;
        if(!hadRoom) linkRoomy(c, i);
        // An empty page goes back to the region, unless it is the only page
        // its class has room in: that one is kept, so a class that hovers
        // around a page boundary does not take and return a page each time.
        if(!--page.live && !(roomy[c] == i && !page.next))
        {
            unlinkRoomy(c, i);
            page.cls = 0;
            page.next = unusedPages;
            unusedPages = uint16_t(i);
            --pagesInUse;
        }
    }
    // else: not a block of this pool any more (freed twice); leave the page alone
    LightLock_Unlock(&lock);
}

inline void noteFailed(size_t size)
{
    lastFailedBytes = size > UINT32_MAX ? UINT32_MAX : uint32_t(size);
    lastFailedFrame = ac3ds_crash_frame();
}

inline void *failed(void *p, size_t size)
{
    if(!p && size) noteFailed(size);
    return p;
}
}

extern "C" {
void *__wrap__malloc_r(struct _reent *r, size_t size)
{
    void *p = nullptr;
    if(size <= maxSmall)
    {
        if(bypass) __atomic_fetch_add(&nestedFallbacks, 1, __ATOMIC_RELAXED);
        else p = take(r, size);
    }
    if(!p) p = failed(__real__malloc_r(r, size), size);
#ifdef AC3DS_HEAPTRACK
    // Nested in newlib's calloc, realloc or memalign, the outer wrapper records
    // the pointer the caller really gets (realloc may merge this chunk away).
    if(!bypass) TRACK_ALLOC(p, size);
#endif
    return p;
}

void __wrap__free_r(struct _reent *r, void *p)
{
    if(!p) return;
    TRACK_FREE(p);
    if(pooled(p)) give(p);
    else __real__free_r(r, p);
}

void *__wrap__calloc_r(struct _reent *r, size_t count, size_t size)
{
    void *p = nullptr;
    if(!bypass && (!size || count <= maxSmall / size))
    {
        p = take(r, count * size);
        if(p) memset(p, 0, count * size);
    }
    if(!p)
    {
        ++bypass;
        p = __real__calloc_r(r, count, size);
        --bypass;
        if(!p && count && size) noteFailed(count > UINT32_MAX / size ? size_t(UINT32_MAX) : count * size);
    }
    TRACK_ALLOC(p, count * size);
    return p;
}

void *__wrap__realloc_r(struct _reent *r, void *old, size_t size)
{
    if(!old)
    {
        // realloc(NULL, n) is malloc(n): newlib reads no chunk header for it,
        // so the pool can serve it.
        void *p = size <= maxSmall && !bypass ? take(r, size) : nullptr;
        if(!p) p = failed(__real__malloc_r(r, size), size);
        TRACK_ALLOC(p, size);
        return p;
    }
    if(pooled(old))
    {
        // Size 0 returns the block, still allocated, as this newlib's realloc
        // does for a block in the general heap.
        const size_t have = classBytes[classAt(old)];
        if(size <= have)
        {
            TRACK_ALLOC(old, size);
            return old;
        }
        void *p = size <= maxSmall && !bypass ? take(r, size) : nullptr;
        if(!p) p = failed(__real__malloc_r(r, size), size);
        if(!p) return nullptr; // the old block stays valid, as realloc promises
        memcpy(p, old, have);
        TRACK_FREE(old);
        give(old);
        TRACK_ALLOC(p, size);
        return p;
    }
#ifdef AC3DS_HEAPTRACK
    __malloc_lock(r); // recursive; keeps old's address from going to another thread before it is forgotten
#endif
    ++bypass;
    void *p = failed(__real__realloc_r(r, old, size), size);
    --bypass;
    if(p && p != old) TRACK_FREE(old);
#ifdef AC3DS_HEAPTRACK
    __malloc_unlock(r);
#endif
    TRACK_ALLOC(p, size);
    return p;
}

void *__wrap__memalign_r(struct _reent *r, size_t align, size_t size)
{
    void *p = nullptr;
    // malloc()'s own cache-line alignment, or less: any class that fits will
    // do, since a request this size lands in a class that is a multiple of 32.
    if(align <= cacheLine && size <= maxSmall)
    {
        if(bypass) __atomic_fetch_add(&nestedFallbacks, 1, __ATOMIC_RELAXED);
        else p = take(r, size < cacheLine ? cacheLine : size);
    }
    if(!p)
    {
        ++bypass;
        p = failed(__real__memalign_r(r, align, size), size);
        --bypass;
    }
    TRACK_ALLOC(p, size);
    return p;
}
}

// Pool use for the memory log line: kilobytes of pages serving a class now,
// and blocks alive. "full" is the figure to watch: it only moves when the
// region is missing or has no page left, and small blocks are back in the
// general heap.
void ac3ds_smallpool_stats(unsigned *usedKb, unsigned *capacityKb, unsigned *blocks, unsigned *full)
{
    *usedKb = unsigned(pagesInUse * pageBytes / 1024);
    *capacityKb = unsigned((pageCount - 1) * pageBytes / 1024);
    *blocks = liveBlocks;
    *full = fullFallbacks;
}

// The most the pool has held at once, in kilobytes of pages.
unsigned ac3ds_smallpool_peak_kb() { return unsigned(pagesPeak * pageBytes / 1024); }

// Size of the last allocation request that failed (0 if none has) and the
// main-loop frame it failed in.
unsigned ac3ds_alloc_last_failed_bytes(unsigned *frame)
{
    *frame = lastFailedFrame;
    return lastFailedBytes;
}

// Address range of the pool region (both null until first use).
void ac3ds_smallpool_bounds(const void **begin, const void **end)
{
    *begin = region;
    *end = regionEnd;
}

// Development: why small requests missed the pool.
void ac3ds_smallpool_debug(unsigned *full, unsigned *nested, unsigned *pagesUsed)
{
    *full = fullFallbacks;
    *nested = nestedFallbacks;
    *pagesUsed = pagesInUse;
}

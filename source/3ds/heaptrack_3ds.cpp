// Development aid, only compiled with -DAC3DS_HEAPTRACK=ON: records the caller
// and size of every live heap block, and logs the largest holders on the
// "ac3dsheap" command. Addresses resolve with arm-none-eabi-nm on the ELF.
//
// The malloc wrappers in smallpool_3ds.cpp report every block here. operator
// new is wrapped as well, so a C++ allocation is charged to the code that
// asked for it, not to libstdc++.

#include <3ds.h>
#include <reent.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
void *__real__Znwj(size_t);
void *__real__Znaj(size_t);
}
void ac3ds_smallpool_bounds(const void **begin, const void **end);

namespace
{
struct Block { uintptr_t pointer; uint32_t size, caller; };
constexpr uint32_t slots = 1u << 18;
constexpr uintptr_t tombstone = 1;
Block blocks[slots];
LightLock lock = 1; // unlocked, as LightLock_Init leaves it
uint32_t liveBlocks, dropped;
thread_local uint32_t requester;

uint32_t slotFor(uintptr_t pointer) { return uint32_t((pointer >> 3) * 2654435761u) & (slots - 1); }

void record(void *p, size_t size, uint32_t caller)
{
    if(!p) return;
    const uintptr_t key = uintptr_t(p);
    LightLock_Lock(&lock);
    uint32_t i = slotFor(key), spare = slots;
    for(uint32_t probes = 0; probes < slots; ++probes, i = (i + 1) & (slots - 1))
    {
        if(blocks[i].pointer == key) { spare = i; break; }
        if(blocks[i].pointer == tombstone) { if(spare == slots) spare = i; continue; }
        if(!blocks[i].pointer) { if(spare == slots) spare = i; break; }
    }
    if(spare == slots || liveBlocks >= slots - slots / 8) ++dropped;
    else
    {
        if(blocks[spare].pointer != key) ++liveBlocks;
        blocks[spare] = { key, uint32_t(size), caller };
    }
    LightLock_Unlock(&lock);
}

void forget(void *p)
{
    if(!p) return;
    const uintptr_t key = uintptr_t(p);
    LightLock_Lock(&lock);
    uint32_t i = slotFor(key);
    for(uint32_t probes = 0; probes < slots && blocks[i].pointer; ++probes, i = (i + 1) & (slots - 1))
        if(blocks[i].pointer == key)
        {
            // A slot at the end of its probe run can go straight back to
            // empty, together with any tombstones just before it. Otherwise
            // tombstones only ever accumulate and every probe gets longer.
            --liveBlocks;
            if(blocks[(i + 1) & (slots - 1)].pointer) { blocks[i].pointer = tombstone; break; }
            blocks[i].pointer = 0;
            for(uint32_t j = (i - 1) & (slots - 1); blocks[j].pointer == tombstone; j = (j - 1) & (slots - 1)) blocks[j].pointer = 0;
            break;
        }
    LightLock_Unlock(&lock);
}
}

void ac3ds_heaptrack_record(void *p, size_t size, void *caller)
{
    record(p, size, requester ? requester : uint32_t(uintptr_t(caller)));
}
void ac3ds_heaptrack_forget(void *p) { forget(p); }

extern "C" {
void *__wrap__Znwj(size_t size)
{
    const uint32_t outer = requester;
    if(!outer) requester = uint32_t(uintptr_t(__builtin_return_address(0)));
    void *p = __real__Znwj(size);
    requester = outer;
    return p;
}
void *__wrap__Znaj(size_t size)
{
    const uint32_t outer = requester;
    if(!outer) requester = uint32_t(uintptr_t(__builtin_return_address(0)));
    void *p = __real__Znaj(size);
    requester = outer;
    return p;
}
}

// Logs the callers holding the most live heap, largest first.
void ac3ds_heap_report(void (*log)(const char *), const char *tag)
{
    struct Holder { uint32_t caller, bytes, count; };
    constexpr uint32_t holderSlots = 8192;
    static Holder holders[holderSlots];
    memset(holders, 0, sizeof(holders));
    uint32_t total = 0, count = 0, other = 0;
    LightLock_Lock(&lock);
    for(uint32_t i = 0; i < slots; ++i)
    {
        const Block &b = blocks[i];
        if(b.pointer <= tombstone) continue;
        total += b.size;
        ++count;
        uint32_t h = (b.caller >> 2) * 2654435761u & (holderSlots - 1), probes = 0;
        while(holders[h].caller && holders[h].caller != b.caller && ++probes < holderSlots) h = (h + 1) & (holderSlots - 1);
        if(probes >= holderSlots) { other += b.size; continue; }
        holders[h].caller = b.caller;
        holders[h].bytes += b.size;
        ++holders[h].count;
    }
    const uint32_t lost = dropped;
    LightLock_Unlock(&lock);
    char line[160];
    snprintf(line, sizeof(line), "AC3DS_HEAP %s total=%lu KB blocks=%lu untracked=%lu other=%lu KB", tag,
             (unsigned long)(total / 1024), (unsigned long)count, (unsigned long)lost, (unsigned long)(other / 1024));
    log(line);
    for(int rank = 0; rank < 48; ++rank)
    {
        uint32_t best = holderSlots;
        for(uint32_t h = 0; h < holderSlots; ++h)
            if(holders[h].caller && (best == holderSlots || holders[h].bytes > holders[best].bytes)) best = h;
        if(best == holderSlots || holders[best].bytes < 16 * 1024) break;
        snprintf(line, sizeof(line), "AC3DS_HEAP %s caller=%08lx bytes=%lu count=%lu", tag,
                 (unsigned long)holders[best].caller, (unsigned long)holders[best].bytes, (unsigned long)holders[best].count);
        log(line);
        holders[best].caller = 0;
    }
}

// Logs how the free space between live blocks is broken up, and which block
// sizes sit alone inside it ("pins": a live block with at least 16 KiB free on
// both sides).
void ac3ds_heap_map(void (*log)(const char *), const char *tag)
{
    struct Span { uint32_t pointer, size, caller; };
    constexpr uint32_t maxSpans = 160000;
    static Span spans[maxSpans];
    uint32_t n = 0;
    const void *poolBegin, *poolEnd;
    ac3ds_smallpool_bounds(&poolBegin, &poolEnd);
    const uint32_t poolLow = uint32_t(uintptr_t(poolBegin)), poolHigh = uint32_t(uintptr_t(poolEnd));
    uint32_t pooledBlocks = 0, pooledBytes = 0;
    LightLock_Lock(&lock);
    for(uint32_t i = 0; i < slots && n < maxSpans; ++i)
    {
        const Block &b = blocks[i];
        if(b.pointer <= tombstone || b.pointer < 0x08000000u || b.pointer >= 0x10000000u) continue;
        // Blocks inside the small-block region are not heap chunks; the region
        // itself is one, and is tracked as such.
        if(b.pointer >= poolLow && b.pointer < poolHigh && b.pointer != poolLow) { ++pooledBlocks; pooledBytes += b.size; continue; }
        spans[n++] = { uint32_t(b.pointer), b.size, b.caller };
    }
    LightLock_Unlock(&lock);
    if(poolLow)
    {
        bool listed = false;
        for(uint32_t i = 0; i < n; ++i) if(spans[i].pointer == poolLow) { listed = true; spans[i].size = poolHigh - poolLow; }
        if(!listed && n < maxSpans) spans[n++] = { poolLow, poolHigh - poolLow, 0 };
    }
    // Shell sort by address; no allocation.
    for(uint32_t gap = n / 2; gap; gap /= 2)
        for(uint32_t i = gap; i < n; ++i)
        {
            const Span v = spans[i];
            uint32_t j = i;
            for(; j >= gap && spans[j - gap].pointer > v.pointer; j -= gap) spans[j] = spans[j - gap];
            spans[j] = v;
        }
    static const uint32_t gapLimits[] = { 1024, 16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024, 4096 * 1024, 0xffffffffu };
    uint32_t gapBytes[7] = {}, gapCount[7] = {}, largest = 0;
    static const uint32_t pinLimits[] = { 32, 64, 128, 256, 1024, 16 * 1024, 0xffffffffu };
    uint32_t pinCount[7] = {}, pinBytes[7] = {}, liveSmall = 0, liveSmallBytes = 0;
    auto gapAfter = [&](uint32_t i) -> uint32_t
    {
        if(i + 1 >= n) return 0;
        const uint32_t end = spans[i].pointer + ((spans[i].size + 4 + 7) & ~7u); // chunk header and alignment
        return spans[i + 1].pointer - 8 > end ? spans[i + 1].pointer - 8 - end : 0;
    };
    // Which code owns the blocks that border the 16 KiB+ holes.
    struct Edge { uint32_t caller, bytes, count, blockBytes; };
    constexpr uint32_t edgeSlots = 2048;
    static Edge edges[edgeSlots];
    memset(edges, 0, sizeof(edges));
    auto edge = [&](uint32_t caller, uint32_t gapSize, uint32_t blockSize)
    {
        uint32_t h = (caller >> 2) * 2654435761u & (edgeSlots - 1), probes = 0;
        while(edges[h].caller && edges[h].caller != caller && ++probes < edgeSlots) h = (h + 1) & (edgeSlots - 1);
        if(probes >= edgeSlots) return;
        edges[h].caller = caller;
        edges[h].bytes += gapSize / 2; // half to each neighbour
        edges[h].blockBytes += blockSize;
        ++edges[h].count;
    };
    uint32_t before = 0;
    for(uint32_t i = 0; i < n; ++i)
    {
        const uint32_t after = gapAfter(i);
        if(after >= 64)
        {
            int c = 0;
            while(after >= gapLimits[c]) ++c;
            gapBytes[c] += after;
            ++gapCount[c];
            if(after > largest) largest = after;
        }
        if(spans[i].size <= 256) { ++liveSmall; liveSmallBytes += spans[i].size; }
        if(before >= 16 * 1024 && after >= 16 * 1024)
        {
            int c = 0;
            while(spans[i].size > pinLimits[c]) ++c;
            ++pinCount[c];
            pinBytes[c] += before < after ? before : after; // free space the pin splits off
        }
        before = after;
    }
    char line[200];
    snprintf(line, sizeof(line), "AC3DS_HEAPMAP %s blocks=%lu span=%lu KB largest_gap=%lu KB live<=256B: %lu blocks %lu KB", tag,
             (unsigned long)n, (unsigned long)(n ? (spans[n - 1].pointer - spans[0].pointer) / 1024 : 0), (unsigned long)(largest / 1024),
             (unsigned long)liveSmall, (unsigned long)(liveSmallBytes / 1024));
    log(line);
    snprintf(line, sizeof(line), "AC3DS_HEAPMAP %s small pool: %lu blocks %lu KB", tag, (unsigned long)pooledBlocks, (unsigned long)(pooledBytes / 1024));
    log(line);
    {
        extern void ac3ds_smallpool_debug(unsigned *full, unsigned *nested, unsigned *pagesUsed);
        unsigned full = 0, nested = 0, pagesUsed = 0;
        ac3ds_smallpool_debug(&full, &nested, &pagesUsed);
        snprintf(line, sizeof(line), "AC3DS_HEAPMAP %s small pool fallbacks: full=%u nested=%u pages=%u", tag, full, nested, pagesUsed);
        log(line);
    }
    snprintf(line, sizeof(line), "AC3DS_HEAPMAP %s free KB in gaps <1K:%lu <16K:%lu <64K:%lu <256K:%lu <1M:%lu <4M:%lu >=4M:%lu", tag,
             (unsigned long)(gapBytes[0] / 1024), (unsigned long)(gapBytes[1] / 1024), (unsigned long)(gapBytes[2] / 1024), (unsigned long)(gapBytes[3] / 1024),
             (unsigned long)(gapBytes[4] / 1024), (unsigned long)(gapBytes[5] / 1024), (unsigned long)(gapBytes[6] / 1024));
    log(line);
    snprintf(line, sizeof(line), "AC3DS_HEAPMAP %s gap counts <1K:%lu <16K:%lu <64K:%lu <256K:%lu <1M:%lu <4M:%lu >=4M:%lu", tag,
             (unsigned long)gapCount[0], (unsigned long)gapCount[1], (unsigned long)gapCount[2], (unsigned long)gapCount[3],
             (unsigned long)gapCount[4], (unsigned long)gapCount[5], (unsigned long)gapCount[6]);
    log(line);
    snprintf(line, sizeof(line), "AC3DS_HEAPMAP %s pins by size <=32:%lu <=64:%lu <=128:%lu <=256:%lu <=1K:%lu <=16K:%lu larger:%lu", tag,
             (unsigned long)pinCount[0], (unsigned long)pinCount[1], (unsigned long)pinCount[2], (unsigned long)pinCount[3],
             (unsigned long)pinCount[4], (unsigned long)pinCount[5], (unsigned long)pinCount[6]);
    log(line);
    // Small blocks that are not in the pool, by owner.
    memset(edges, 0, sizeof(edges));
    for(uint32_t i = 0; i < n; ++i)
        if(spans[i].size <= 512 && spans[i].pointer != poolLow)
        {
            uint32_t h = (spans[i].caller >> 2) * 2654435761u & (edgeSlots - 1), probes = 0;
            while(edges[h].caller && edges[h].caller != spans[i].caller && ++probes < edgeSlots) h = (h + 1) & (edgeSlots - 1);
            if(probes >= edgeSlots) continue;
            edges[h].caller = spans[i].caller;
            edges[h].bytes += spans[i].size;
            ++edges[h].count;
        }
    for(int rank = 0; rank < 12; ++rank)
    {
        uint32_t best = edgeSlots;
        for(uint32_t h = 0; h < edgeSlots; ++h)
            if(edges[h].count && (best == edgeSlots || edges[h].count > edges[best].count)) best = h;
        if(best == edgeSlots || edges[best].count < 100) break;
        snprintf(line, sizeof(line), "AC3DS_HEAPSMALL %s caller=%08lx count=%lu mean=%lu", tag, (unsigned long)edges[best].caller,
                 (unsigned long)edges[best].count, (unsigned long)(edges[best].bytes / edges[best].count));
        log(line);
        edges[best].count = 0;
    }
    // Hole borders: each gap of 16 KiB or more is charged, half each, to the
    // callers of the blocks on either side.
    memset(edges, 0, sizeof(edges));
    for(uint32_t i = 0; i < n; ++i)
    {
        const uint32_t after = gapAfter(i);
        if(after >= 16 * 1024)
        {
            edge(spans[i].caller, after, spans[i].size);
            if(i + 1 < n) edge(spans[i + 1].caller, after, spans[i + 1].size);
        }
    }
    for(int rank = 0; rank < 14; ++rank)
    {
        uint32_t best = edgeSlots;
        for(uint32_t h = 0; h < edgeSlots; ++h)
            if(edges[h].count && (best == edgeSlots || edges[h].bytes > edges[best].bytes)) best = h;
        if(best == edgeSlots || edges[best].bytes < 256 * 1024) break;
        snprintf(line, sizeof(line), "AC3DS_HEAPEDGE %s caller=%08lx holes_kb=%lu borders=%lu mean_block=%lu", tag,
                 (unsigned long)edges[best].caller, (unsigned long)(edges[best].bytes / 1024), (unsigned long)edges[best].count,
                 (unsigned long)(edges[best].blockBytes / edges[best].count));
        log(line);
        edges[best].count = 0;
    }
}

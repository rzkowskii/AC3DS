// rendercubes.cpp: sits in between worldrender.cpp and rendergl.cpp and fills the vertex array for different cube surfaces.

#ifdef __3DS__
#include <algorithm>
#endif
#include "cube.h"
#ifdef __3DS__
#include "world_batch_3ds.h"
#include "performance.h"
#endif

extern int ati_mda_bug;

vector<vertex> verts;
#ifdef __3DS__
static_assert(sizeof(vertex) == 24 && offsetof(vertex, u) == 0 &&
              offsetof(vertex, x) == 8 && offsetof(vertex, r) == 20,
              "PICA world vertex layout");
static bool directworldstrips;
static bool clipMainWorldStrips;
static bool mainWorldTexturesUsed[256];
static float mainWorldStripClip[6][4];
static void finishworldbatch();
static void prepareworldbatch();

void ac3ds_begin_world_strips(const float *clip)
{
    clipMainWorldStrips = clip != nullptr;
    if(clip)
    {
        memcpy(mainWorldStripClip, clip, sizeof(mainWorldStripClip));
        memset(mainWorldTexturesUsed, 0, sizeof(mainWorldTexturesUsed));
    }
}

static bool clipcompletedstrip(int start, int count)
{
    if(!clipMainWorldStrips || start+count != verts.length()) return false;
    // Reject a completed strip as a unit. Even offscreen vertices can
    // affect the original strip's lighting interpolation and topology.
    const vertex *points = verts.getbuf()+start;
    float lo[3] = {points[0].x, points[0].y, points[0].z};
    float hi[3] = {lo[0], lo[1], lo[2]};
    for(int i = 1; i < count; ++i)
    {
        const vertex &v = points[i];
        lo[0] = min(lo[0], v.x); hi[0] = max(hi[0], v.x);
        lo[1] = min(lo[1], v.y); hi[1] = max(hi[1], v.y);
        lo[2] = min(lo[2], v.z); hi[2] = max(hi[2], v.z);
    }
    for(const auto &plane : mainWorldStripClip)
        if(plane[3] + plane[0]*(plane[0] < 0 ? lo[0] : hi[0]) +
           plane[1]*(plane[1] < 0 ? lo[1] : hi[1]) +
           plane[2]*(plane[2] < 0 ? lo[2] : hi[2]) < 0)
        {
            verts.setsize(start);
            return true;
        }
    return false;
}
#endif

void finishstrips();

void setupstrips()
{
    finishstrips();

#ifdef __3DS__
    clipMainWorldStrips = false;
    directworldstrips = !ati_mda_bug && ac3ds_gl_begin_world_geometry(verts.getbuf(), verts.length());
    if(directworldstrips) prepareworldbatch();
    finishworldbatch();
    if(directworldstrips) return;
#endif
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    vertex *buf = verts.getbuf();
    glVertexPointer(3, GL_FLOAT, sizeof(vertex), &buf->x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(vertex), &buf->r);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vertex), &buf->u);
}

struct strips { vector<GLint> first; vector<GLsizei> count; };

struct stripbatch
{
    int tex;
    strips tris, tristrips, quads;
};

stripbatch skystrips = { DEFAULT_SKY };
stripbatch stripbatches[256];
uchar renderedtex[256];
int renderedtexs = 0;

#ifdef __3DS__
namespace
{
    // Record the values that can change with dynamic light. Comparing the
    // actual RGB and defer bits also catches filter and remip dependencies.
    struct WorldLightDependency
    {
        unsigned cube, tile;
        unsigned value;
    };
    struct WorldLightDependencies
    {
        vector<WorldLightDependency> color, defer;
        vector<int> colorEnds, deferEnds;
    };
    enum WorldSurfaceType { SurfaceFlat, SurfaceSlope, SurfaceCorner, SurfaceWall };
    struct WorldSurface
    {
        WorldSurfaceType type;
        int a, b, c, d;
        short texture, x, y, endX, endY, size;
        bool flip;
        uchar direction;
        float heights[4];
    };
    struct CachedWorldBatch
    {
        Ac3dsWorldBatchKey key{};
        vector<vertex> vertices;
        stripbatch sky{};
        stripbatch textures[256]{};
        int textureCount = 0, quads = 0;
        int water[4] = {};
        int mipStats[LARGEST_FACTOR] = {};
        WorldLightDependencies lights;
        vector<block> lightRegions;
        vector<WorldSurface> surfaces;
        float skyfloor = 0;
        unsigned lastUse = 0;
        bool valid = false;
    };

    CachedWorldBatch worldBatches[Ac3dsWorldBatchSlots];
    CachedWorldBatch *admittedWorldBatch = nullptr;
    unsigned worldBatchUseSerial;
    CachedWorldBatch refractionWorld;
    CachedWorldBatch *activeWorldBatch = nullptr;
    CachedWorldBatch *pendingWorldBatch = nullptr;
    CachedWorldBatch *heldRefractionWorld = nullptr;
    float worldSkyFloor = 1e16f;
    WorldLightDependencies worldLightDependencies;
    vector<block> changedWorldLightRegions;
    vector<WorldSurface> worldSurfaces;
    vector<uchar> worldLightRecorded;
    vector<unsigned> worldLightTouched;
    unsigned worldLightTouchedLimit;
    bool recordWorldLights;
    bool worldLightDependenciesComplete;
    int worldLightMip;
    struct WorldBatchCounters
    {
        unsigned hit, keyMiss, lightingMiss, surfaceReplay;
        unsigned changedView, changedClip, changedLod, changedRays, changedData, changedOptions;
    };
    WorldBatchCounters worldBatchCounters[3];
    unsigned worldBatchCounterGeneration;
    struct WorldBatchAdmission
    {
        Ac3dsWorldBatchKey lastMiss{};
        bool seen = false;
    };
    WorldBatchAdmission worldBatchAdmissions[3];
    bool admitWorldBatch;
    bool recordWorldSurfaces;
    bool worldSurfacesComplete;
    constexpr int maxWorldSurfaces = 8192;

    void recordworldsurface(const WorldSurface &surface)
    {
        if(worldSurfaces.length() == maxWorldSurfaces)
        {
            recordWorldSurfaces = worldSurfacesComplete = false;
            return;
        }
        worldSurfaces.add(surface);
    }

    void countworldbatch(int pass, bool hit, bool lightingMiss, const CachedWorldBatch *previous = nullptr,
                         const Ac3dsWorldBatchKey *next = nullptr)
    {
        if(!ac3dsTimingPhases) return;
        if(worldBatchCounterGeneration != ac3dsTimingGeneration)
        {
            memset(worldBatchCounters, 0, sizeof(worldBatchCounters));
            worldBatchCounterGeneration = ac3dsTimingGeneration;
        }
        auto &count = worldBatchCounters[pass];
        if(hit) ++count.hit;
        else if(lightingMiss) ++count.lightingMiss;
        else
        {
            ++count.keyMiss;
            if(!previous || !previous->valid) return;
            const Ac3dsWorldBatchKey &key = previous->key;
            if(key.view != next->view) ++count.changedView;
            if(key.clip != next->clip) ++count.changedClip;
            if(!std::equal(key.options.begin(), key.options.begin()+4, next->options.begin())) ++count.changedLod;
            if(key.rays != next->rays) ++count.changedRays;
            if(key.world != next->world || key.lighting != next->lighting || key.textures != next->textures) ++count.changedData;
            if(!std::equal(key.options.begin()+4, key.options.end(), next->options.begin()+4)) ++count.changedOptions;
        }
    }

    unsigned worldlighttile(const sqr *cube)
    {
        const unsigned local = cube-wmip[worldLightMip];
        const int bits = sfactor-worldLightMip;
        if(local >= (1u<<(2*bits))) return ~0u;
        const unsigned x = (local & ((1u<<bits)-1)) << worldLightMip;
        const unsigned y = (local >> bits) << worldLightMip;
        return (unsigned(worldLightMip)<<16) | ((y>>4)<<8) | (x>>4);
    }

    unsigned worldlightcolor(const sqr &cube)
    {
        return unsigned(cube.r) | (unsigned(cube.g) << 8) | (unsigned(cube.b) << 16);
    }

    bool firstworlddependency(unsigned index, uchar mask)
    {
        uchar &flags = worldLightRecorded[index];
        if(flags & mask) return false;
        if(!flags && worldLightTouched.length() < int(worldLightTouchedLimit)) worldLightTouched.add(index);
        flags |= mask;
        return true;
    }

    void recordworldcolor(const sqr *cube)
    {
        const uintptr_t address = reinterpret_cast<uintptr_t>(cube);
        const uintptr_t start = reinterpret_cast<uintptr_t>(world);
        if(address < start || address-start >= size_t(mipsize)*sizeof(sqr))
        {
            worldLightDependenciesComplete = false;
            return;
        }
        const unsigned index = cube-world;
        if(!firstworlddependency(index, 1)) return;
        worldLightDependencies.color.add({index, worldlighttile(cube), worldlightcolor(*cube)});
    }

    bool lighttouchestile(unsigned tile, const vector<block> &regions)
    {
        if(tile == ~0u) return true;
        const int mip = tile>>16, span = max(16, 1<<mip);
        const int x = int(tile & 255)<<4, y = int((tile>>8)&255)<<4;
        // The legacy backup clips to the playable border. Check any input
        // there directly in case a light hit an adjacent solid border cell.
        if(x == 0 || y == 0 || x+span >= ssize || y+span >= ssize) return true;
        const int mipMask = (1<<mip)-1;
        loopv(regions)
        {
            const block &area = regions[i];
            const int left = (area.x-1)&~mipMask, top = (area.y-1)&~mipMask;
            const int right = (area.x+area.xs+1+mipMask)&~mipMask;
            const int bottom = (area.y+area.ys+1+mipMask)&~mipMask;
            if(x < right && left < x+span && y < bottom && top < y+span) return true;
        }
        return false;
    }

    template<bool defer>
    bool lightdependenciesmatch(const vector<WorldLightDependency> &deps, const vector<int> &ends,
                                const vector<block> &regions)
    {
        int first = 0;
        loopv(ends)
        {
            const int end = ends[i];
            if(lighttouchestile(deps[first].tile, regions))
            {
                for(int i = first; i < end; ++i)
                {
                    const auto &dep = deps[i];
                    const unsigned current = defer ? unsigned(world[dep.cube].defer != 0) : worldlightcolor(world[dep.cube]);
                    if(dep.value != current) return false;
                }
            }
            first = end;
        }
        return true;
    }

    bool validworldlightregions(const vector<block> &regions)
    {
        if(regions.empty()) return false;
        loopv(regions)
        {
            const block &area = regions[i];
            if(area.xs <= 0 || area.ys <= 0 || area.x < 1 || area.y < 1 ||
               area.x+area.xs > ssize || area.y+area.ys > ssize) return false;
        }
        return true;
    }

    void sortlightdependencies(vector<WorldLightDependency> &deps, vector<int> &ends)
    {
        ends.setsize(0);
        if(deps.empty()) return;
        auto order = [](const WorldLightDependency &a, const WorldLightDependency &b)
        {
            return a.tile < b.tile || (a.tile == b.tile && a.cube < b.cube);
        };
        std::sort(deps.getbuf(), deps.getbuf()+deps.length(), order);
        for(int i = 1; i < deps.length(); ++i) if(deps[i].tile != deps[i-1].tile) ends.add(i);
        ends.add(deps.length());
    }
}

extern int nquads, stats[LARGEST_FACTOR];
extern int wx1, wy1, wx2, wy2;
VAR(ac3dswarmworldcache, 0, 1, 1);
VAR(ac3dsreplayworldsurfaces, 0, 1, 1);
VAR(ac3dsmultiworldcache, 0, 1, 1);
VAR(ac3dslitworldcache, 0, 1, 1);

static void replayworldsurface(const WorldSurface &surface);

bool ac3ds_restore_world_batch(int pass, const Ac3dsWorldBatchKey &key)
{
    CachedWorldBatch *selected = &worldBatches[2*pass];
    const bool dynamic = ac3ds_dynamic_world_lighting();
    const bool cacheLit = ac3dslitworldcache && ac3dsmultiworldcache;
    if(ac3dsmultiworldcache)
    {
        CachedWorldBatch &alternate = worldBatches[2*pass+1];
        if(cacheLit && dynamic && alternate.valid && alternate.key == key && !alternate.lightRegions.empty())
            selected = &alternate;
        else if(!(selected->valid && selected->key == key) &&
           ((alternate.valid && alternate.key == key) || selected->lastUse > alternate.lastUse)) selected = &alternate;
    }
    CachedWorldBatch &batch = *selected;
    admittedWorldBatch = selected;
    admitWorldBatch = false;
    if(!batch.valid || !(batch.key == key))
    {
        // A one-off camera position can draw directly from the frame arena.
        // Keep the existing persistent batch until the new view repeats.
        WorldBatchAdmission &admission = worldBatchAdmissions[pass];
        admitWorldBatch = !ac3dswarmworldcache || (admission.seen && admission.lastMiss == key);
        if(ac3dswarmworldcache && !admitWorldBatch)
        {
            admission.lastMiss = key;
            admission.seen = true;
        }
        countworldbatch(pass, false, false, selected, &key);
        if(dynamic && cacheLit)
            admittedWorldBatch = &worldBatches[2*pass+1];
        return false;
    }
    if(dynamic || !batch.lightRegions.empty())
    {
        const auto &activeRegions = ac3ds_dynamic_world_regions();
        const vector<block> *regionsToCompare = &activeRegions;
        if(!batch.lightRegions.empty())
        {
            changedWorldLightRegions.setsize(0);
            loopv(activeRegions) changedWorldLightRegions.add(activeRegions[i]);
            loopv(batch.lightRegions) changedWorldLightRegions.add(batch.lightRegions[i]);
            regionsToCompare = &changedWorldLightRegions;
        }
        const auto &regions = *regionsToCompare;
        const bool topologyMatches = validworldlightregions(regions) &&
            lightdependenciesmatch<true>(batch.lights.defer, batch.lights.deferEnds, regions);
        if(!topologyMatches || !lightdependenciesmatch<false>(batch.lights.color, batch.lights.colorEnds, regions))
        {
            countworldbatch(pass, false, true);
            if(!topologyMatches || !ac3dsreplayworldsurfaces || batch.surfaces.empty())
            {
                if(dynamic && cacheLit && validworldlightregions(activeRegions))
                {
                    admittedWorldBatch = &worldBatches[2*pass+1];
                    admitWorldBatch = true;
                }
                return false;
            }
            if(ac3dsTimingPhases) ++worldBatchCounters[pass].surfaceReplay;
            loopv(batch.surfaces) replayworldsurface(batch.surfaces[i]);
            if(pass == 0)
            {
                wx1 = batch.water[0]; wy1 = batch.water[1];
                wx2 = batch.water[2]; wy2 = batch.water[3];
            }
            memcpy(stats, batch.mipStats, sizeof(batch.mipStats));
            return true;
        }
    }
    countworldbatch(pass, true, false);
    batch.lastUse = ++worldBatchUseSerial;
    activeWorldBatch = &batch;
    verts.swapstorage(batch.vertices);
    nquads = batch.quads;
    skyfloor = min(skyfloor, batch.skyfloor);
    if(pass == 0)
    {
        wx1 = batch.water[0]; wy1 = batch.water[1];
        wx2 = batch.water[2]; wy2 = batch.water[3];
    }
    memcpy(stats, batch.mipStats, sizeof(batch.mipStats));
    return true;
}

void ac3ds_report_world_batches()
{
    if(worldBatchCounterGeneration != ac3dsTimingGeneration) return;
    const char *const names[] = {"main", "reflection", "refraction"};
    loopi(3)
    {
        const auto &count = worldBatchCounters[i];
        clientlogf("AC3DS_WORLD_BATCH %s hit=%u key_miss=%u lighting_miss=%u surface_replay=%u",
                   names[i], count.hit, count.keyMiss, count.lightingMiss, count.surfaceReplay);
        if(count.keyMiss)
            clientlogf("AC3DS_WORLD_KEYS %s view=%u clip=%u lod=%u rays=%u data=%u options=%u",
                       names[i], count.changedView, count.changedClip, count.changedLod, count.changedRays,
                       count.changedData, count.changedOptions);
    }
}

void ac3ds_begin_world_dependencies(bool record)
{
    recordWorldLights = record && admitWorldBatch;
    recordWorldSurfaces = recordWorldLights && ac3dsreplayworldsurfaces;
    if(!recordWorldLights) return;
    worldSurfacesComplete = recordWorldSurfaces;
    worldSurfaces.setsize(0);
    worldLightDependenciesComplete = true;
    worldLightDependencies.color.setsize(0);
    worldLightDependencies.defer.setsize(0);
    if(worldLightRecorded.length() == mipsize && worldLightTouched.length() < int(worldLightTouchedLimit))
        loopv(worldLightTouched) worldLightRecorded[worldLightTouched[i]] = 0;
    else
    {
        worldLightRecorded.setsize(0);
        memset(worldLightRecorded.pad(mipsize), 0, mipsize);
    }
    worldLightTouched.setsize(0);
    worldLightTouchedLimit = max(1, mipsize / 16);
}

void ac3ds_world_dependency_mip(int mip) { worldLightMip = mip; }

void ac3ds_record_world_defer(const sqr *cube)
{
    if(!recordWorldLights) return;
    const unsigned index = cube-world;
    if(!firstworlddependency(index, 2)) return;
    worldLightDependencies.defer.add({index, worldlighttile(cube), unsigned(cube->defer != 0)});
}

void ac3ds_save_world_batch(const Ac3dsWorldBatchKey &key)
{
    if(!admitWorldBatch || !worldLightDependenciesComplete) return;
    pendingWorldBatch = admittedWorldBatch;
    pendingWorldBatch->lastUse = ++worldBatchUseSerial;
    pendingWorldBatch->key = key;
}

bool ac3ds_can_record_lit_world_batch()
{
    return ac3dslitworldcache && ac3dsmultiworldcache && validworldlightregions(ac3ds_dynamic_world_regions());
}

static void prepareworldbatch()
{
    // On a hit the cache is active before setupstrips. A first-time miss is
    // saved after this step, so a moving camera keeps using the frame arena.
    if(!activeWorldBatch) return;
    if(activeWorldBatch == &refractionWorld) return;
    const int slot = activeWorldBatch-worldBatches;
    if(ac3ds_gl_begin_cached_world(slot)) return;
    Ac3dsWorldStrips batches[3*(256+1)];
    int count = 0;
    auto append = [&](const strips &list, GLenum mode)
    {
        if(!list.first.empty()) batches[count++] = {mode,list.first.getbuf(),list.count.getbuf(),list.first.length()};
    };
    auto appendBatch = [&](const stripbatch &batch)
    {
        append(batch.tris,GL_TRIANGLES);
        append(batch.tristrips,GL_TRIANGLE_STRIP);
        append(batch.quads,GL_QUADS);
    };
    appendBatch(activeWorldBatch->sky);
    loopi(activeWorldBatch->textureCount) appendBatch(activeWorldBatch->textures[i]);
    ac3ds_gl_build_world_cache(slot,batches,count);
}

static void swapstrips(strips &a, strips &b)
{
    a.first.swapstorage(b.first);
    a.count.swapstorage(b.count);
}

static void swapstripbatch(stripbatch &a, stripbatch &b)
{
    swap(a.tex, b.tex);
    swapstrips(a.tris, b.tris);
    swapstrips(a.tristrips, b.tristrips);
    swapstrips(a.quads, b.quads);
}

static void clearstrips(strips &batch)
{
    batch.first.setsize(0);
    batch.count.setsize(0);
}

static void clearslots(stripbatch &batch)
{
    clearstrips(batch.tris);
    clearstrips(batch.tristrips);
    clearstrips(batch.quads);
}

static void captureworldbatch(CachedWorldBatch &batch)
{
    batch.textureCount = renderedtexs;
    batch.quads = nquads;
    batch.skyfloor = worldSkyFloor;
    memcpy(batch.mipStats, stats, sizeof(batch.mipStats));
    batch.vertices.setsize(0);
    swapstripbatch(batch.sky, skystrips);
    loopi(renderedtexs) swapstripbatch(batch.textures[i], stripbatches[i]);
    clearslots(skystrips);
    loopi(renderedtexs) clearslots(stripbatches[i]);
    renderedtexs = 0;
    activeWorldBatch = &batch;
}

static void finishworldbatch()
{
    if(!pendingWorldBatch) return;
    CachedWorldBatch &batch = *pendingWorldBatch;
    pendingWorldBatch = nullptr;
    ac3ds_gl_discard_world_cache(&batch-worldBatches);
    if(!directworldstrips)
    {
        batch.valid = false;
        return;
    }
    batch.water[0] = wx1; batch.water[1] = wy1;
    batch.water[2] = wx2; batch.water[3] = wy2;
    batch.lights.color.swapstorage(worldLightDependencies.color);
    batch.lights.defer.swapstorage(worldLightDependencies.defer);
    batch.lightRegions = ac3ds_dynamic_world_regions();
    batch.surfaces.setsize(0);
    if(worldSurfacesComplete) batch.surfaces.swapstorage(worldSurfaces);
    sortlightdependencies(batch.lights.color, batch.lights.colorEnds);
    sortlightdependencies(batch.lights.defer, batch.lights.deferEnds);
    // The old batch's buffers become scratch storage for the next miss.
    captureworldbatch(batch);
    batch.valid = true;
}

bool ac3ds_hold_refraction_world()
{
    heldRefractionWorld = nullptr;
    if(!directworldstrips || !ac3ds_same_refraction_world_view()) return false;
    if(!activeWorldBatch) captureworldbatch(refractionWorld);
    heldRefractionWorld = activeWorldBatch;
    ac3ds_gl_capture_frame_world();
    return true;
}

bool ac3ds_reuse_refraction_world()
{
    if(!reflecting || !refracting || !heldRefractionWorld || !ac3ds_same_refraction_world_view()) return false;
    resetcubes();
    activeWorldBatch = heldRefractionWorld;
    verts.swapstorage(activeWorldBatch->vertices);
    nquads = activeWorldBatch->quads;
    skyfloor = min(skyfloor, activeWorldBatch->skyfloor);
    memcpy(stats, activeWorldBatch->mipStats, sizeof(activeWorldBatch->mipStats));
    setupstrips();
    ac3ds_gl_replay_frame_world();
    return true;
}

void ac3ds_release_refraction_world() { heldRefractionWorld = nullptr; }
#endif

static void renderstripbatch(strips &batch, GLenum type, bool world = false)
{
    if(batch.first.empty()) return;
#ifdef __3DS__
    if(world && directworldstrips)
        ac3ds_gl_draw_world_strips(type, batch.first.getbuf(), batch.count.getbuf(), batch.first.length());
    else
#endif
    {
        if(hasMDA && !ati_mda_bug) glMultiDrawArrays_(type, batch.first.getbuf(), batch.count.getbuf(), batch.first.length());
        else loopv(batch.first) glDrawArrays(type, batch.first[i], batch.count[i]);
    }
#ifdef __3DS__
    if(world && activeWorldBatch) return;
#endif
    batch.first.setsize(0);
    batch.count.setsize(0);
}

void renderstripssky()
{
    stripbatch &sky =
#ifdef __3DS__
        activeWorldBatch ? activeWorldBatch->sky :
#endif
        skystrips;
    if(sky.tris.first.empty() && sky.tristrips.first.empty() && sky.quads.first.empty()) return;
    glDisable(GL_TEXTURE_2D);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    renderstripbatch(sky.tris, GL_TRIANGLES, true);
    renderstripbatch(sky.tristrips, GL_TRIANGLE_STRIP, true);
    renderstripbatch(sky.quads, GL_QUADS, true);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_TEXTURE_2D);
}

void renderstrips()
{
#ifdef __3DS__
    const int textureCount = activeWorldBatch ? activeWorldBatch->textureCount : renderedtexs;
    stripbatch *batches = activeWorldBatch ? activeWorldBatch->textures : stripbatches;
#else
    const int textureCount = renderedtexs;
    stripbatch *batches = stripbatches;
#endif
    loopj(textureCount)
    {
        stripbatch &sb = batches[j];
        glBindTexture(GL_TEXTURE_2D, lookupworldtexture(sb.tex)->id);
        renderstripbatch(sb.tris, GL_TRIANGLES, true);
        renderstripbatch(sb.tristrips, GL_TRIANGLE_STRIP, true);
        renderstripbatch(sb.quads, GL_QUADS, true);
    }
    renderedtexs = 0;

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

void addstrip(int type, int tex, int start, int n)
{
    stripbatch *sb = NULL;
    if(tex==DEFAULT_SKY)
    {
        if(minimap) return;
        sb = &skystrips;
#ifdef __3DS__
        loopi(n) worldSkyFloor = min(worldSkyFloor, verts[start + i].z);
        skyfloor = min(skyfloor, worldSkyFloor);
        if(clipcompletedstrip(start, n)) return;
#else
        loopi(n) skyfloor = min(skyfloor, verts[start + i].z);
#endif
    }
    else
    {
#ifdef __3DS__
        // Preserve when each material first enters the batch list, even
        // when its first strip is clipped. Coplanar depth ties keep order.
        const bool firstMaterial = clipMainWorldStrips && !mainWorldTexturesUsed[tex];
        if(clipMainWorldStrips) mainWorldTexturesUsed[tex] = true;
        if(!firstMaterial && clipcompletedstrip(start, n)) return;
#endif
        sb = &stripbatches[renderedtex[tex]];
        if(sb->tex!=tex || sb>=&stripbatches[renderedtexs])
        {
            sb = &stripbatches[renderedtex[tex] = renderedtexs++];
            sb->tex = tex;
        }
    }
    strips &s = (type==GL_QUADS ? sb->quads : (type==GL_TRIANGLES ? sb->tris : sb->tristrips));
    if(type!=GL_TRIANGLE_STRIP && s.first.length() && s.first.last()+s.count.last() == start)
    {
        s.count.last() += n;
        return;
    }
    s.first.add(start);
    s.count.add(n);
}

// generating the actual vertices is done dynamically every frame and sits at the
// leaves of all these functions, and are part of the cpu bottleneck on really slow
// machines, hence the macros.

#define vert(v1, v2, v3, ls, t1, t2) \
{ \
    vertex &v = verts.add(); \
    v.u = t1; v.v = t2; \
    v.x = (float)(v1); v.y = (float)(v2); v.z = (float)(v3); \
    v.r = ls->r; v.g = ls->g; v.b = ls->b; v.a = 255; \
}

enum
{
    STRIP_FLOOR = 1,
    STRIP_DELTA,
    STRIP_WALL
};

int nquads;

// for testing purpose. UNDOME on release.
const float texturescale = 32.0f;
//VARP(texturescale, 16, 32, 64);

#define TEXTURESCALE (float(texturescale) * ((uniformtexres && t->scale>1.0f) ? 1.0f : t->scale))

static inline void worldtexscale(int slot, float &x, float &y)
{
#ifdef __3DS__
    struct TextureScale { Texture *tex; int w, h; float scale, x, y; };
    static TextureScale scales[256];
    static unsigned scaleGeneration;
    static bool scaleValid[256];
    static int scaleUniform;
    const unsigned generation = ac3ds_world_texture_generation();
    if(scaleGeneration != generation || scaleUniform != uniformtexres)
    {
        memset(scaleValid, 0, sizeof(scaleValid));
        scaleGeneration = generation;
        scaleUniform = uniformtexres;
    }
    TextureScale &cached = scales[slot];
    if(scaleValid[slot])
    {
        x = cached.x;
        y = cached.y;
        return;
    }
#endif
    Texture *t = lookupworldtexture(slot);
    const float scale = TEXTURESCALE;
#ifdef __3DS__
    if(cached.tex != t || cached.w != t->xs || cached.h != t->ys || cached.scale != scale)
        cached = {t, t->xs, t->ys, scale, scale/t->xs, scale/t->ys};
    scaleValid[slot] = generation == ac3ds_world_texture_generation();
    x = cached.x;
    y = cached.y;
#else
    x = scale/t->xs;
    y = scale/t->ys;
#endif
}

int striptype = 0, striptex, oh, oy, ox, odir;                         // the o* vars are used by the stripification
int ol1r, ol1g, ol1b, ol2r, ol2g, ol2b;
float ofloor, oceil;
bool ohf;
int firstindex;
bool showmipstats = false, showeditfocus = false;

COMMANDF(showmip, "",() { showmipstats = !showmipstats; });
COMMANDF(showfocuscubedetails, "",() { showeditfocus = !showeditfocus; });

const char *cubetypenames[] = {"SOLID", "CORNER", "FHF", "CHF", "SPACE", ""};
const char *cubetypename(int t) { return t >= 0 && t < SEMISOLID ? cubetypenames[t] : "unknown"; }

void mipstats(const int a[]) { if(showmipstats && !showeditfocus) hudeditf(HUDMSG_MIPSTATS, "1x1/2x2/4x4/8x8: %d / %d / %d / %d", a[0], a[1], a[2], a[3]); }

bool editfocusdetails(sqr *s)
{
    if(showeditfocus)
    {
        const char *g = SOLID(s) ? "\f4" : "";
        hudeditf(HUDMSG_EDITFOCUS, "%s: \fs%s%d..%d\fr W:%d \fs%sU:%d F:%d C:%d v:%d t:0x%X (%02X%02X%02X)\fr", cubetypename(s->type), g, s->floor, s->ceil, s->wtex, g, s->utex, s->ftex, s->ctex, s->vdelta, s->tag, s->r, s->g, s->b);
        return true;
    }
    return false;
}

VAR(mergestrips, 0, 1, 1);

#define stripend(verts) \
    if(striptype) \
    { \
        int type = GL_TRIANGLE_STRIP, len = verts.length()-firstindex; \
        if(mergestrips) switch(len) \
        { \
            case 3: type = GL_TRIANGLES; break; \
            case 4: type = GL_QUADS; swap(verts.last(), verts[verts.length()-2]); break; \
        } \
        addstrip(type, striptex, firstindex, len); \
        striptype = 0; \
    }

void finishstrips() { stripend(verts); }

sqr sbright, sdark;
VARP(lighterror, 1, 4, 100);

void render_flat(int wtex, int x, int y, int size, int h, sqr *l1, sqr *l4, sqr *l3, sqr *l2, bool isceil)  // floor/ceil quads
{
#ifdef __3DS__
    if(recordWorldLights)
    {
        recordworldcolor(l1); recordworldcolor(l2);
        recordworldcolor(l3); recordworldcolor(l4);
    }
    if(recordWorldSurfaces && worldLightDependenciesComplete)
        recordworldsurface({SurfaceFlat, int(l1-world), int(l4-world), int(l3-world), int(l2-world),
                            short(wtex), short(x), short(y), 0, 0, short(size), isceil, 0, {float(h)}});
#endif
    if(showmipstats) { l3 = l1 = &sbright; l4 = l2 = &sdark; }

    float xf, yf;
    bool first = striptype!=STRIP_FLOOR || x!=ox+size || striptex!=wtex || h!=oh || y!=oy;
    worldtexscale(wtex, xf, yf);
    const float xs = size*xf, ys = size*yf, xo = xf*x, yo = yf*y;

    if(first)       // start strip here
    {
        stripend(verts);
        firstindex = verts.length();
        striptex = wtex;
        oh = h;
        oy = y;
        striptype = STRIP_FLOOR;
        if(isceil)
        {
            vert(x, y,      h, l1, xo, yo);
            vert(x, y+size, h, l2, xo, yo+ys);
        }
        else
        {
            vert(x, y+size, h, l2, xo, yo+ys);
            vert(x, y,      h, l1, xo, yo);
        }
        ol1r = l1->r;
        ol1g = l1->g;
        ol1b = l1->b;
        ol2r = l2->r;
        ol2g = l2->g;
        ol2b = l2->b;
    }
    else        // continue strip
    {
        int lighterr = lighterror*2;
        if((iabs(ol1r-l3->r)<lighterr && iabs(ol2r-l4->r)<lighterr        // skip vertices if light values are close enough
        &&  iabs(ol1g-l3->g)<lighterr && iabs(ol2g-l4->g)<lighterr
        &&  iabs(ol1b-l3->b)<lighterr && iabs(ol2b-l4->b)<lighterr) || !wtex)
        {
            verts.setsize(verts.length()-2);
            nquads--;
        }
        else
        {
            uchar *p1 = (uchar *)(&verts[verts.length()-1].r);
            ol1r = p1[0];
            ol1g = p1[1];
            ol1b = p1[2];
            uchar *p2 = (uchar *)(&verts[verts.length()-2].r);
            ol2r = p2[0];
            ol2g = p2[1];
            ol2b = p2[2];
        }
    }

    if(isceil)
    {
        vert(x+size, y,      h, l4, xo+xs, yo);
        vert(x+size, y+size, h, l3, xo+xs, yo+ys);
    }
    else
    {
        vert(x+size, y+size, h, l3, xo+xs, yo+ys);
        vert(x+size, y,      h, l4, xo+xs, yo);
    }

    ox = x;
    nquads++;
}

void render_flatdelta(int wtex, int x, int y, int size, float h1, float h4, float h3, float h2, sqr *l1, sqr *l4, sqr *l3, sqr *l2, bool isceil)  // floor/ceil quads on a slope
{
#ifdef __3DS__
    if(recordWorldLights)
    {
        recordworldcolor(l1); recordworldcolor(l2);
        recordworldcolor(l3); recordworldcolor(l4);
    }
    if(recordWorldSurfaces && worldLightDependenciesComplete)
        recordworldsurface({SurfaceSlope, int(l1-world), int(l4-world), int(l3-world), int(l2-world),
                            short(wtex), short(x), short(y), 0, 0, short(size), isceil, 0, {h1, h4, h3, h2}});
#endif
    if(showmipstats) { l3 = l1 = &sbright; l4 = l2 = &sdark; }

    float xf, yf;
    worldtexscale(wtex, xf, yf);
    float xs = size*xf;
    float ys = size*yf;
    float xo = xf*x;
    float yo = yf*y;

    bool first = striptype!=STRIP_DELTA || x!=ox+size || striptex!=wtex || y!=oy;

    if(first)
    {
        stripend(verts);
        firstindex = verts.length();
        striptex = wtex;
        oy = y;
        striptype = STRIP_DELTA;
        if(isceil)
        {
            vert(x, y,      h1, l1, xo, yo);
            vert(x, y+size, h2, l2, xo, yo+ys);
        }
        else
        {
            vert(x, y+size, h2, l2, xo, yo+ys);
            vert(x, y,      h1, l1, xo, yo);
        }
    }

    if(isceil)
    {
        vert(x+size, y,      h4, l4, xo+xs, yo);
        vert(x+size, y+size, h3, l3, xo+xs, yo+ys);
    }
    else
    {
        vert(x+size, y+size, h3, l3, xo+xs, yo+ys);
        vert(x+size, y,      h4, l4, xo+xs, yo);
    }

    ox = x;
    nquads++;
}

void render_2tris(sqr *h, sqr *s, int x1, int y1, int x2, int y2, int x3, int y3, sqr *l1, sqr *l2, sqr *l3)   // floor/ceil tris on a corner cube
{
#ifdef __3DS__
    if(recordWorldLights)
    {
        recordworldcolor(l1); recordworldcolor(l2); recordworldcolor(l3);
    }
    if(recordWorldSurfaces && worldLightDependenciesComplete)
        recordworldsurface({SurfaceCorner, int(l1-world), int(l2-world), int(l3-world), int(h-world),
                            0, short(x1), short(y1), short(x2), short(y2), 0, false, 0, {float(x3), float(y3)}});
#endif
    stripend(verts);

    float xf, yf;
    worldtexscale(h->ftex, xf, yf);
    vert(x1, y1, h->floor, l1, xf*x1, yf*y1);
    vert(x2, y2, h->floor, l2, xf*x2, yf*y2);
    vert(x3, y3, h->floor, l3, xf*x3, yf*y3);
    addstrip(mergestrips ? GL_TRIANGLES : GL_TRIANGLE_STRIP, h->ftex, verts.length()-3, 3);

    worldtexscale(h->ctex, xf, yf);

    vert(x3, y3, h->ceil, l3, xf*x3, yf*y3);
    vert(x2, y2, h->ceil, l2, xf*x2, yf*y2);
    vert(x1, y1, h->ceil, l1, xf*x1, yf*y1);
    addstrip(mergestrips ? GL_TRIANGLES : GL_TRIANGLE_STRIP, h->ctex, verts.length()-3, 3);
    nquads++;
}

void render_tris(int x, int y, int size, bool topleft,
                 sqr *h1, sqr *h2, sqr *s, sqr *t, sqr *u, sqr *v)
{
    if(topleft)
    {
        if(h1) render_2tris(h1, s, x+size, y+size, x, y+size, x, y, u, v, s);
        if(h2) render_2tris(h2, s, x, y, x+size, y, x+size, y+size, s, t, v);
    }
    else
    {
        if(h1) render_2tris(h1, s, x, y, x+size, y, x, y+size, s, t, u);
        if(h2) render_2tris(h2, s, x+size, y, x+size, y+size, x, y+size, t, u, v);
    }
}

void render_square(int wtex, float floor1, float floor2, float ceil1, float ceil2, int x1, int y1, int x2, int y2, int size, sqr *l1, sqr *l2, bool flip, int dir)   // wall quads
{
#ifdef __3DS__
    if(recordWorldLights) { recordworldcolor(l1); recordworldcolor(l2); }
    if(recordWorldSurfaces && worldLightDependenciesComplete)
        recordworldsurface({SurfaceWall, int(l1-world), int(l2-world), 0, 0,
                            short(wtex), short(x1), short(y1), short(x2), short(y2), short(size), flip, uchar(dir),
                            {floor1, floor2, ceil1, ceil2}});
#endif
    if(showmipstats) { l1 = &sbright; l2 = &sdark; }

    float xf, yf;
    worldtexscale(wtex, xf, yf);
    yf = -yf;
    float xs = size*xf;
    float xo = xf*(x1==x2 ? min(y1,y2) : min(x1,x2));

    bool first = striptype!=STRIP_WALL || striptex!=wtex || ox!=x1 || oy!=y1 || ofloor!=floor1 || oceil!=ceil1 || odir!=dir,
         hf = floor1!=floor2 || ceil1!=ceil2;

    if(first)
    {
        stripend(verts);
        firstindex = verts.length();
        striptex = wtex;
        striptype = STRIP_WALL;

        if(!flip)
        {
            vert(x1, y1, ceil1,  l1, xo, yf*ceil1);
            vert(x1, y1, floor1, l1, xo, yf*floor1);
        }
        else
        {
            vert(x1, y1, floor1, l1, xo, yf*floor1);
            vert(x1, y1, ceil1,  l1, xo, yf*ceil1);
        }
        ol1r = l1->r;
        ol1g = l1->g;
        ol1b = l1->b;
    }
    else        // continue strip
    {
        int lighterr = lighterror*2;
        if((!hf && !ohf)
        && ((iabs(ol1r-l2->r)<lighterr && iabs(ol1g-l2->g)<lighterr && iabs(ol1b-l2->b)<lighterr) || !wtex))       // skip vertices if light values are close enough
        {
            verts.setsize(verts.length()-2);
            nquads--;
        }
        else
        {
            uchar *p1 = (uchar *)(&verts[verts.length()-1].r);
            ol1r = p1[0];
            ol1g = p1[1];
            ol1b = p1[2];
        }
    }

    if(!flip)
    {
        vert(x2, y2, ceil2,  l2, xo+xs, yf*ceil2);
        vert(x2, y2, floor2, l2, xo+xs, yf*floor2);
    }
    else
    {
        vert(x2, y2, floor2, l2, xo+xs, yf*floor2);
        vert(x2, y2, ceil2,  l2, xo+xs, yf*ceil2);
    }

    ox = x2;
    oy = y2;
    ofloor = floor2;
    oceil = ceil2;
    odir = dir;
    ohf = hf;
    nquads++;
}

#ifdef __3DS__
static void replayworldsurface(const WorldSurface &surface)
{
    sqr *a = world+surface.a, *b = world+surface.b, *c = world+surface.c, *d = world+surface.d;
    const float *h = surface.heights;
    switch(surface.type)
    {
        case SurfaceFlat:
            render_flat(surface.texture, surface.x, surface.y, surface.size, int(h[0]), a, b, c, d, surface.flip);
            break;
        case SurfaceSlope:
            render_flatdelta(surface.texture, surface.x, surface.y, surface.size, h[0], h[1], h[2], h[3], a, b, c, d, surface.flip);
            break;
        case SurfaceCorner:
            render_2tris(d, nullptr, surface.x, surface.y, surface.endX, surface.endY, int(h[0]), int(h[1]), a, b, c);
            break;
        case SurfaceWall:
            render_square(surface.texture, h[0], h[1], h[2], h[3], surface.x, surface.y,
                          surface.endX, surface.endY, surface.size, a, b, surface.flip, surface.direction);
            break;
    }
}
#endif

void resetcubes()
{
#ifdef __3DS__
    clipMainWorldStrips = false;
    if(activeWorldBatch)
    {
        verts.swapstorage(activeWorldBatch->vertices);
        activeWorldBatch = nullptr;
    }
    pendingWorldBatch = nullptr;
    worldSkyFloor = 1e16f;
#endif
    verts.setsize(0);

    striptype = 0;
    nquads = 0;

    sbright.r = sbright.g = sbright.b = 255;
    sdark.r = sdark.g = sdark.b = 0;

    resetwater();
}

struct shadowvertex { float u, v, x, y, z; };
vector<shadowvertex> shadowverts;

static void resetshadowverts()
{
    shadowverts.setsize(0);

    striptype = 0;
}

static void rendershadowstrips()
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    shadowvertex *buf = shadowverts.getbuf();
    glVertexPointer(3, GL_FLOAT, sizeof(shadowvertex), &buf->x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(shadowvertex), &buf->u);

    loopj(renderedtexs)
    {
        stripbatch &sb = stripbatches[j];
        renderstripbatch(sb.tris, GL_TRIANGLES);
        renderstripbatch(sb.tristrips, GL_TRIANGLE_STRIP);
        renderstripbatch(sb.quads, GL_QUADS);
    }
    renderedtexs = 0;

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    xtraverts += shadowverts.length();
}

#define shadowvert(v1, v2, v3) \
{ \
    shadowvertex &v = shadowverts.add(); \
    v.x = (float)(v1); v.y = (float)(v2); v.z = (float)(v3); \
}

void rendershadow_tri(sqr *h, int x1, int y1, int x2, int y2, int x3, int y3)   // floor tris on a corner cube
{
    stripend(shadowverts);

    shadowvert(x1, y1, h->floor);
    shadowvert(x2, y2, h->floor);
    shadowvert(x3, y3, h->floor);
    addstrip(mergestrips ? GL_TRIANGLES : GL_TRIANGLE_STRIP, DEFAULT_FLOOR, shadowverts.length()-3, 3);
}

void rendershadow_tris(int x, int y, bool topleft, sqr *h1, sqr *h2)
{
    if(topleft)
    {
        if(h1) rendershadow_tri(h1, x+1, y+1, x, y+1, x, y);
        if(h2) rendershadow_tri(h2, x, y, x+1, y, x+1, y+1);
    }
    else
    {
        if(h1) rendershadow_tri(h1, x, y, x+1, y, x, y+1);
        if(h2) rendershadow_tri(h2, x+1, y, x+1, y+1, x, y+1);
    }
}

static void rendershadow_flat(int x, int y, int h) // floor quads
{
    bool first = striptype!=STRIP_FLOOR || x!=ox+1 || h!=oh || y!=oy;

    if(first)       // start strip here
    {
        stripend(shadowverts);
        firstindex = shadowverts.length();
        striptex = DEFAULT_FLOOR;
        oh = h;
        oy = y;
        striptype = STRIP_FLOOR;
        shadowvert(x, y+1, h);
        shadowvert(x, y,   h);
    }
    else        // continue strip
    {
        shadowverts.setsize(shadowverts.length()-2);
    }

    shadowvert(x+1, y+1, h);
    shadowvert(x+1, y,   h);

    ox = x;
}

static void rendershadow_flatdelta(int x, int y, float h1, float h4, float h3, float h2)  // floor quads on a slope
{
    bool first = striptype!=STRIP_DELTA || x!=ox+1 || y!=oy;

    if(first)
    {
        stripend(shadowverts);
        firstindex = shadowverts.length();
        striptex = DEFAULT_FLOOR;
        oy = y;
        striptype = STRIP_DELTA;
        shadowvert(x, y+1, h2);
        shadowvert(x, y,   h1);
    }

    shadowvert(x+1, y+1, h3);
    shadowvert(x+1, y,   h4);

    ox = x;
}

void rendershadow(int x, int y, int xs, int ys, const vec &texgenS, const vec &texgenT)
{
    x = max(x, 1);
    y = max(y, 1);
    xs = min(xs, ssize-1);
    ys = min(ys, ssize-1);

    resetshadowverts();

    #define df(x) s->floor-(x->vdelta/4.0f)

    sqr *w = wmip[0];
    for(int yy = y; yy<ys; yy++) for(int xx = x; xx<xs; xx++)
    {
        sqr *s = SW(w,xx,yy);
        if(s->type==SPACE || s->type==CHF)
        {
            rendershadow_flat(xx, yy, s->floor);
        }
        else if(s->type==FHF)
        {
            sqr *t = SW(s,1,0), *u = SW(s,1,1), *v = SW(s,0,1);
            rendershadow_flatdelta(xx, yy, df(s), df(t), df(u), df(v));
        }
        else if(s->type==CORNER)
        {
            sqr *t = SW(s,1,0), *v = SW(s,0,1), *w = SW(s,0,-1), *z = SW(s,-1,0);
            bool topleft = true;
            sqr *h1 = NULL, *h2 = NULL;
            if(SOLID(z))
            {
                if(SOLID(w))      { h2 = s; topleft = false; }
                else if(SOLID(v)) { h2 = s; }
            }
            else if(SOLID(t))
            {
                if(SOLID(w))      { h1 = s; }
                else if(SOLID(v)) { h1 = s; topleft = false; }
            }
            else
            {
                bool wv = w->ceil-w->floor < v->ceil-v->floor;
                if(z->ceil-z->floor < t->ceil-t->floor)
                {
                    if(wv) { h1 = s; h2 = v; topleft = false; }
                    else   { h1 = s; h2 = w; }
                }
                else
                {
                    if(wv) { h2 = s; h1 = v; }
                    else   { h2 = s; h1 = w; topleft = false; }
                }
            }
            rendershadow_tris(xx, yy, topleft, h1, h2);
        }
    }

    stripend(shadowverts);

    for(shadowvertex *v = shadowverts.getbuf(), *end = &v[shadowverts.length()]; v < end; v++)
    {
        float vx = v->x, vy = v->y;
        v->u = vx*texgenS.x + vy*texgenS.y + texgenS.z;
        v->v = vx*texgenT.x + vy*texgenT.y + texgenT.z;
    }

    rendershadowstrips();
}


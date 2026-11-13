// worldrender.cpp: goes through all cubes in top down quad tree fashion, determines what has to
// be rendered and how (depending on neighbouring cubes), then calls functions in rendercubes.cpp

#include "cube.h"
#ifdef __3DS__
#include "performance.h"
#include "world_batch_3ds.h"
#endif

const int MAX_MIP = 5;   // 32x32 unit blocks
bool render_floor, render_ceil;

#ifdef __3DS__
#include <vector>

extern unsigned ac3ds_raytable_generation();
extern unsigned ac3ds_world_geometry_generation();
extern int skyclip;
bool issemi(int mip, int x, int y, int x1, int y1, int x2, int y2);
VAR(ac3dsmainworldclip, 0, 0, 1);
VAR(ac3dssharedreflection, 0, 1, 1);
VAR(ac3dswallrecipes, 0, 1, 1);
VAR(ac3dsnativereflectionclip, 0, 1, 1);

namespace
{
    struct WorldClipPlane
    {
        float x, y, z, offset;
    };
    struct WorldHeights
    {
        // Quarter-cube heights include the exact heightfield vertex deltas.
        int bottom : 11;
        int top : 11;
        unsigned sky : 3;
    };
    static_assert(sizeof(WorldHeights) == 4, "World height cache must stay compact");
    enum { SkyAlways = 1, SkyFloor = 2, SkyCeiling = 4 };
    WorldClipPlane worldClip[6];
    float mainWorldClip[24];
    bool mainWorldClipActive;
    std::vector<WorldHeights> worldHeights[MAX_MIP + 1];
    WorldHeights worldHeightRange;
    unsigned worldHeightsGeneration;
    bool worldClipActive;
    bool worldOcclusionActive;
    std::vector<uchar> cubeOcclusion;
    std::vector<uchar> cubeWallCandidates;
    struct WallFace { short height[4]; uchar texture, direction; };
    struct WallRecipe { unsigned first; uchar count; };
    std::vector<unsigned> cubeWallRecipe;
    vector<WallRecipe> wallRecipes;
    vector<WallFace> wallFaces;
    unsigned wallRecipeGeneration;
    std::vector<uchar> cubeSurfacePrefix[MAX_MIP+1];
    unsigned cubeOcclusionGeneration;
    unsigned cubeWallGeneration;
    const sqr *cubeOcclusionWorld;
    float cubeOcclusionX, cubeOcclusionY;

    struct WorldClipCache
    {
        WorldClipPlane planes[6] = {};
        unsigned generation = 0;
        int skyMode = 0;
        bool populated = false;
        std::vector<uchar> cells;
    };
    WorldClipCache worldClipCaches[2];
    WorldClipCache *worldClipCache;
    int worldSkyMode;

    struct RefractionWorldView
    {
        float x, y, z, verticalFov, horizontalFov;
        int yaw, pitch, lod, minLod;
        unsigned geometry, rays, lighting, textures;
        bool valid;
    };
    RefractionWorldView refractionWorldView;

    struct WorldSelectionKey
    {
        float x, y;
        unsigned geometry, rays, lighting;
        int left, right, top, bottom;
        bool operator==(const WorldSelectionKey &other) const
        {
            return x == other.x && y == other.y && geometry == other.geometry && rays == other.rays &&
                   lighting == other.lighting && left == other.left && right == other.right &&
                   top == other.top && bottom == other.bottom;
        }
    };
    struct WorldSelectionCell { int x, y, child, next; };
    struct WorldSelectionRegion
    {
        int firstCell, lastCell, rendered;
    };
    vector<WorldSelectionRegion> worldSelectionRegions;
    vector<WorldSelectionCell> worldSelectionCells;
    WorldSelectionKey worldSelectionKey;
    bool worldSelectionValid, worldSelectionLit, recordWorldSelection;
    unsigned worldSelectionReplays;
    unsigned worldSelectionProfile;

    bool selectionhasroom()
    {
        constexpr int maxSelection = 65536;
        if(worldSelectionRegions.length() < maxSelection && worldSelectionCells.length() < maxSelection) return true;
        recordWorldSelection = false;
        worldSelectionValid = false;
        return false;
    }

    void addselectioncell(int region, int x, int y, int child)
    {
        if(!recordWorldSelection || !selectionhasroom()) return;
        const int index = worldSelectionCells.length();
        auto &entry = worldSelectionRegions[region];
        if(entry.lastCell >= 0) worldSelectionCells[entry.lastCell].next = index;
        else entry.firstCell = index;
        entry.lastCell = index;
        worldSelectionCells.add({x, y, child, -1});
    }

    void includeheights(WorldHeights &range, const WorldHeights &part)
    {
        range.bottom = min(int(range.bottom), int(part.bottom));
        range.top = max(int(range.top), int(part.top));
        range.sky |= part.sky;
    }

    void prepareworldheights()
    {
        const unsigned generation = ac3ds_world_geometry_generation();
        if(worldHeightsGeneration == generation) return;
        worldHeightsGeneration = generation;
        worldHeightRange = {768, -768, false};
        for(int mip = 0; mip <= MAX_MIP; ++mip)
        {
            const int side = ssize >> mip;
            auto &heights = worldHeights[mip];
            heights.resize(side * side);
            const sqr *cubes = wmip[mip];
            for(int y = 0; y < side; ++y) for(int x = 0; x < side; ++x)
            {
                const sqr &cube = cubes[y*side+x];
                const sqr &right = cubes[y*side+min(x+1, side-1)];
                const sqr &below = cubes[min(y+1, side-1)*side+x];
                const sqr &diagonal = cubes[min(y+1, side-1)*side+min(x+1, side-1)];
                const int delta = max(max(cube.vdelta, right.vdelta), max(below.vdelta, diagonal.vdelta));
                int bottom = 768, top = -768;
                unsigned sky = 0;
                // Walls use the same boundary vertices but can borrow heights
                // and texture slots from the cells on either side.
                for(int edge = 0; edge < 5; ++edge)
                {
                    const int nx = clamp(x + (edge == 1) - (edge == 2), 0, side-1);
                    const int ny = clamp(y + (edge == 3) - (edge == 4), 0, side-1);
                    const sqr &neighbor = cubes[ny*side+nx];
                    // A corner can borrow the flat floor and ceiling even
                    // from a solid neighbor when it builds its diagonal.
                    if(!SOLID(&neighbor) || cube.type == CORNER)
                    {
                        const int floor = neighbor.floor * 4 - (neighbor.type == FHF ? delta : 0);
                        const int ceil = neighbor.ceil * 4 + (neighbor.type == CHF ? delta : 0);
                        bottom = min(bottom, min(floor, ceil));
                        top = max(top, max(floor, ceil));
                    }
                    if((edge == 0 || cube.type == CORNER) &&
                       (neighbor.wtex == DEFAULT_SKY || neighbor.utex == DEFAULT_SKY)) sky |= SkyAlways;
                    if(cube.type == CORNER &&
                       (neighbor.ftex == DEFAULT_SKY || neighbor.ctex == DEFAULT_SKY)) sky |= SkyAlways;
                }
                if(cube.type == SPACE || cube.type == CHF || cube.type == FHF)
                {
                    if(cube.ftex == DEFAULT_SKY) sky |= cube.type == FHF ? SkyAlways : SkyFloor;
                    if(cube.ctex == DEFAULT_SKY) sky |= cube.type == CHF ? SkyAlways : SkyCeiling;
                }
                WorldHeights range = {bottom, top, bottom <= top ? sky : 0u};
                if(mip)
                {
                    const auto &children = worldHeights[mip-1];
                    const int child = y*side*4 + x*2;
                    includeheights(range, children[child]);
                    includeheights(range, children[child+1]);
                    includeheights(range, children[child+side*2]);
                    includeheights(range, children[child+side*2+1]);
                }
                heights[y*side+x] = range;
                if(mip == MAX_MIP) includeheights(worldHeightRange, range);
            }
        }
    }

    bool cancacheworldheights()
    {
        // A 2048-square custom map already needs over 85 MiB of cube data.
        // Keep the legacy traversal there instead of adding a large cache.
        if(ssize <= 1024) return true;
        for(auto &heights : worldHeights) std::vector<WorldHeights>().swap(heights);
        for(auto &cache : worldClipCaches)
        {
            std::vector<uchar>().swap(cache.cells);
            cache.generation = 0;
        }
        worldHeightsGeneration = 0;
        return false;
    }

    void prepareworldocclusion()
    {
        const unsigned generation = ac3ds_raytable_generation();
        if(cubeOcclusionGeneration != generation || cubeOcclusionWorld != world ||
           cubeOcclusion.size() != size_t(mipsize) ||
           cubeOcclusionX != camera1->o.x || cubeOcclusionY != camera1->o.y)
        {
            cubeOcclusion.resize(mipsize);
            memset(cubeOcclusion.data(), 0, cubeOcclusion.size());
            cubeOcclusionGeneration = generation;
            cubeOcclusionWorld = world;
            cubeOcclusionX = camera1->o.x;
            cubeOcclusionY = camera1->o.y;
        }
    }

    uchar worldoccluded(const sqr *cube, int x, int y, float size)
    {
        uchar &cached = cubeOcclusion[cube - world];
        if(!cached) cached = 1 + isoccluded(camera1->o.x, camera1->o.y, x, y, size);
        return cached - 1;
    }

    void preparewallcandidates()
    {
        const unsigned generation = ac3ds_world_geometry_generation();
        if(ssize > 1024)
        {
            std::vector<uchar>().swap(cubeWallCandidates);
            for(auto &prefix : cubeSurfacePrefix) std::vector<uchar>().swap(prefix);
            cubeWallGeneration = 0;
            return;
        }
        if(cubeWallGeneration == generation) return;
        cubeWallGeneration = generation;
        cubeWallCandidates.resize(mipsize);
        memset(cubeWallCandidates.data(), 0, cubeWallCandidates.size());

        // A solid cube with four solid cardinal neighbors cannot emit a
        // surface. Mark runs with no possible face at this mip or below.
        // A saturated run length still lets the renderer skip large areas
        // in bounded chunks, without retaining a full geometry mesh.
        for(int mip = 0; mip <= MAX_MIP; ++mip)
        {
            const int side = ssize>>mip;
            const sqr *cubes = wmip[mip];
            auto &prefix = cubeSurfacePrefix[mip];
            prefix.resize(side*side);
            const auto &children = cubeSurfacePrefix[max(0,mip-1)];
            for(int y = 0; y < side; ++y)
            {
                unsigned emptyRun = 0;
                for(int x = side-1; x >= 0; --x)
                {
                    const int index = y*side+x;
                    bool empty = x > 0 && x < side-1 && y > 0 && y < side-1 &&
                                 SOLID(cubes+index) && SOLID(cubes+index-1) && SOLID(cubes+index+1) &&
                                 SOLID(cubes+index-side) && SOLID(cubes+index+side);
                    if(empty && mip)
                    {
                        const int child = y*side*4+x*2;
                        empty = children[child] >= 2 && children[child+side*2] >= 2;
                    }
                    emptyRun = empty ? min(255u, emptyRun+1) : 0;
                    prefix[index] = emptyRun;
                }
            }
        }
    }

    bool wallcandidate(const sqr *cube, const sqr *neighbor)
    {
        if(SOLID(neighbor) || (SOLID(cube) && neighbor->type == CORNER)) return false;
        // No floor or ceiling face can be emitted between flat spaces at
        // these heights. The original wall call has no strip side effect.
        return cube->type != SPACE || neighbor->type != SPACE ||
               neighbor->floor < cube->floor || neighbor->ceil > cube->ceil;
    }

    uchar wallcandidates(const sqr *cube, int mip, int x, int y, int side)
    {
        uchar &cached = cubeWallCandidates[cube-world];
        if(cached) return cached;
        // Lighting can change mip defer flags but it cannot change the
        // neighboring cube types or the issemi decision at any given mip.
        cached = 16;
        if(x > 0 && wallcandidate(cube,cube-1) &&
           (cube[-1].type!=SEMISOLID || issemi(mip, x-1, y, 1, 0, 1, 1))) cached |= 1;
        if(x < side-1 && wallcandidate(cube,cube+1) &&
           (cube[1].type!=SEMISOLID || issemi(mip, x+1, y, 0, 0, 0, 1))) cached |= 2;
        if(y > 0 && wallcandidate(cube,cube-side) &&
           (cube[-side].type!=SEMISOLID || issemi(mip, x, y-1, 0, 1, 1, 1))) cached |= 4;
        if(y < side-1 && wallcandidate(cube,cube+side) &&
           (cube[side].type!=SEMISOLID || issemi(mip, x, y+1, 0, 0, 1, 0))) cached |= 8;
        return cached;
    }

    void preparewallrecipes()
    {
        const unsigned generation = ac3ds_world_geometry_generation();
        if(!ac3dswallrecipes || ssize > 1024)
        {
            if(cubeWallRecipe.empty()) return;
            std::vector<unsigned>().swap(cubeWallRecipe);
            wallRecipeGeneration = 0;
            vector<WallRecipe> recipes;
            vector<WallFace> faces;
            wallRecipes.swapstorage(recipes);
            wallFaces.swapstorage(faces);
            return;
        }
        if(wallRecipeGeneration == generation) return;
        wallRecipeGeneration = generation;
        cubeWallRecipe.assign(mipsize, 0);
        wallRecipes.setsize(0);
        wallFaces.setsize(0);
    }

    void prepareclipcache()
    {
        auto &cache = worldClipCaches[refracting];
        const unsigned generation = ac3ds_world_geometry_generation();
        if(cache.generation != generation || cache.skyMode != worldSkyMode ||
           memcmp(cache.planes, worldClip, sizeof(worldClip)))
        {
            cache.generation = generation;
            cache.skyMode = worldSkyMode;
            memcpy(cache.planes, worldClip, sizeof(worldClip));
            cache.populated = false;
            worldClipCache = nullptr;
            return;
        }
        if(!cache.populated)
        {
            cache.cells.resize(mipsize);
            memset(cache.cells.data(), 0, cache.cells.size());
            cache.populated = true;
        }
        worldClipCache = &cache;
    }

    bool outsideworldclip(const WorldHeights &range, int x, int y, int xend, int yend)
    {
        if(range.bottom > range.top) return true;
        // Even a GPU-clipped sky strip can affect the environment box's
        // world-space vertical clip. Keep those strips on the legacy path.
        if(range.sky & worldSkyMode) return false;
        for(const auto &plane : worldClip)
        {
            if(plane.offset + plane.z * (plane.z < 0 ? range.bottom : range.top) +
               plane.x * (plane.x < 0 ? x : xend) +
               plane.y * (plane.y < 0 ? y : yend) < 0) return true;
        }
        return false;
    }

    bool outsideworldcell(const sqr *cube, int mip, int x, int y, int side)
    {
        const WorldHeights &range = worldHeights[mip][y*side+x];
        if(!worldClipCache) return outsideworldclip(range, x<<mip, y<<mip, (x+1)<<mip, (y+1)<<mip);
        uchar &cached = worldClipCache->cells[cube-world];
        if(!cached)
            cached = 1 + outsideworldclip(range, x<<mip, y<<mip, (x+1)<<mip, (y+1)<<mip);
        return cached == 2;
    }

    bool outsideworldregion(int mip, int x, int y, int xs, int ys)
    {
        WorldHeights range = worldHeightRange;
        if(mip != MAX_MIP)
        {
            range = {768, -768, false};
            const int side = ssize >> mip;
            const auto &heights = worldHeights[mip];
            for(int oy = y; oy < ys; ++oy)
                for(int ox = x; ox < xs; ++ox)
                    includeheights(range, heights[oy*side+ox]);
        }
        return outsideworldclip(range, x<<mip, y<<mip, xs<<mip, ys<<mip);
    }

    void preparemainworldclip()
    {
        glmatrixf model, projection, clip;
        glGetFloatv(GL_MODELVIEW_MATRIX, model.v);
        glGetFloatv(GL_PROJECTION_MATRIX, projection.v);
        clip.mul(projection, model);
        loopi(6)
        {
            float *plane = mainWorldClip+4*i;
            const float sign = i&1 ? -1.f : 1.f;
            loopj(4) plane[j] = clip[3+4*j] + sign*clip[i/2+4*j];
            plane[3] += (fabsf(plane[0])+fabsf(plane[1])+fabsf(plane[2]))*0.125f + 0.0001f;
        }
        ac3ds_begin_world_strips(mainWorldClip);
    }

    void prepareselectedreflectionclip()
    {
        float planes[24];
        loopi(6)
        {
            const WorldClipPlane &p = worldClip[i];
            planes[4*i] = p.x;
            planes[4*i+1] = p.y;
            planes[4*i+2] = p.z * 4.f;
            planes[4*i+3] = p.offset;
        }
        ac3ds_begin_world_strips(planes);
    }
}

void ac3ds_begin_world_pass(float x0, float y0, float x1, float y1)
{
    worldClipActive = reflecting && (refracting || !ac3dsnativereflectionclip) &&
                      !minimap && !editmode && !m_coop && cancacheworldheights();
    if(!worldClipActive) return;
    prepareworldheights();
    glmatrixf model, projection, clip;
    glGetFloatv(GL_MODELVIEW_MATRIX, model.v);
    glGetFloatv(GL_PROJECTION_MATRIX, projection.v);
    clip.mul(projection, model);
    const float bounds[] = {x0, x1, y0, y1, -1, 1};
    loopi(6)
    {
        const float sign = i&1 ? -1 : 1;
        float p[4];
        loopj(4) p[j] = sign * (clip[i/2 + 4*j] - bounds[i] * clip[3 + 4*j]);
        const float tolerance = (fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2])) * 0.125f + 0.0001f;
        worldClip[i] = {p[0], p[1], p[2] * 0.25f, p[3] + tolerance};
    }
}

void ac3ds_end_world_pass() { worldClipActive = false; }

unsigned ac3ds_shared_reflection_replays()
{
    return worldSelectionProfile == ac3dsTimingGeneration ? worldSelectionReplays : 0;
}

bool ac3ds_same_refraction_world_view()
{
    extern int minlod;
    const RefractionWorldView &view = refractionWorldView;
    return view.valid && view.x == camera1->o.x && view.y == camera1->o.y && view.z == camera1->o.z &&
           view.yaw == int(camera1->yaw) && view.pitch == int(camera1->pitch) &&
           view.horizontalFov == dynfov() && view.verticalFov == fovy && view.lod == lod_factor() &&
           view.minLod == minlod &&
           view.geometry == ac3ds_world_geometry_generation() && view.rays == ac3ds_raytable_generation() &&
           view.lighting == ac3ds_static_world_lighting_generation() &&
           view.textures == ac3ds_world_texture_generation();
}
#endif

template<class Emit>
static void wallfaces(const sqr *o, const sqr *s, const sqr *d1, const sqr *d2, const Emit &emit)
{
    if(SOLID(o) || o->type==SEMISOLID)
    {
        float c1 = s->floor;
        float c2 = s->floor;
        if(s->type==FHF) { c1 -= d1->vdelta/4.0f; c2 -= d2->vdelta/4.0f; }
        float f1 = s->ceil;
        float f2 = s->ceil;
        if(s->type==CHF) { f1 += d1->vdelta/4.0f; f2 += d2->vdelta/4.0f; }
        emit(o->wtex, c1, c2, f1, f2);
        return;
    }
    {
        float f1 = s->floor;
        float f2 = s->floor;
        float c1 = o->floor;
        float c2 = o->floor;
        if(o->type==FHF && s->type!=FHF)
        {
            c1 -= d1->vdelta/4.0f;
            c2 -= d2->vdelta/4.0f;
        }
        if(s->type==FHF && o->type!=FHF)
        {
            f1 -= d1->vdelta/4.0f;
            f2 -= d2->vdelta/4.0f;
        }
        if(f1<c1 || f2<c2) emit(o->wtex, f1, f2, c1, c2);
    }
    {
        float f1 = o->ceil;
        float f2 = o->ceil;
        float c1 = s->ceil;
        float c2 = s->ceil;
        if(o->type==CHF && s->type!=CHF)
        {
            f1 += d1->vdelta/4.0f;
            f2 += d2->vdelta/4.0f;
        }
        else if(s->type==CHF && o->type!=CHF)
        {
            c1 += d1->vdelta/4.0f;
            c2 += d2->vdelta/4.0f;
        }
        if(c1>f1 || c2>f2) emit(o->utex, f1, f2, c1, c2);
    }
}

void render_wall(sqr *o, sqr *s, int x1, int y1, int x2, int y2, int mip, sqr *d1, sqr *d2, bool topleft, int dir)
{
    if(minimap) return;
    wallfaces(o, s, d1, d2, [&](uchar texture, float f1, float f2, float c1, float c2)
    {
        render_square(texture, f1, f2, c1, c2, x1<<mip, y1<<mip, x2<<mip, y2<<mip, 1<<mip, d1, d2, topleft, dir);
    });
}

#ifdef __3DS__
static bool renderwallrecipe(sqr *cube, int mip, int x, int y, int side, int vx, int vy)
{
    // Direction table entries describe the same two corner samples as the
    // normal wall calls. RGB is read from those samples at draw time.
    constexpr int startX[] = {0, 1, 0, 0}, startY[] = {0, 0, 0, 1};
    constexpr int endX[] = {0, 1, 1, 1}, endY[] = {1, 1, 0, 1};
    constexpr int maxRecipes = 32768, maxFaces = 65536;
    const int neighbors[] = {-1, 1, -side, side};
    unsigned &entry = cubeWallRecipe[cube-world];
    if(!entry)
    {
        if(wallRecipes.length() >= maxRecipes || wallFaces.length()+8 > maxFaces) return false;
        const unsigned first = wallFaces.length();
        const uchar candidates = wallcandidates(cube, mip, x, y, side);
        loopi(4) if(candidates & (1<<i))
        {
            const sqr *left = cube+startX[i]+startY[i]*side;
            const sqr *right = cube+endX[i]+endY[i]*side;
            wallfaces(cube, cube+neighbors[i], left, right, [&](uchar texture, float f1, float f2, float c1, float c2)
            {
                wallFaces.add({{short(f1*4), short(f2*4), short(c1*4), short(c2*4)}, texture, uchar(i)});
            });
        }
        const unsigned count = wallFaces.length()-first;
        if(!count) entry = 1;
        else
        {
            entry = wallRecipes.length()+2;
            wallRecipes.add({first, uchar(count)});
        }
    }
    if(entry == 1) return true;
    const unsigned visible = unsigned(x>=vx) | (unsigned(x<=vx)<<1) | (unsigned(y>=vy)<<2) | (unsigned(y<=vy)<<3);
    const WallRecipe &recipe = wallRecipes[entry-2];
    for(unsigned i = recipe.first; i < recipe.first+recipe.count; ++i)
    {
        const WallFace &face = wallFaces[i];
        const int dir = face.direction;
        if(!(visible & (1u<<dir))) continue;
        sqr *left = cube+startX[dir]+startY[dir]*side;
        sqr *right = cube+endX[dir]+endY[dir]*side;
        render_square(face.texture, face.height[0]*0.25f, face.height[1]*0.25f,
                      face.height[2]*0.25f, face.height[3]*0.25f,
                      (x+startX[dir])<<mip, (y+startY[dir])<<mip, (x+endX[dir])<<mip, (y+endY[dir])<<mip,
                      1<<mip, left, right, dir==0 || dir==3, dir);
    }
    return true;
}
#endif

const int MIN_LOD = 2;
const int LOW_LOD = 25;
const int MAX_LOD = 250;

int lod = 40, lodtop, lodbot, lodleft, lodright;
int min_lod;

int lod_factor() { return lod; }

VARP(minlod, LOW_LOD, 60, MAX_LOD);

int stats[LARGEST_FACTOR];

// detect those cases where a higher mip solid has a visible wall next to lower mip cubes
// (used for wall rendering below)

bool issemi(int mip, int x, int y, int x1, int y1, int x2, int y2)
{
    if(!(mip--)) return true;
    sqr *w = wmip[mip];
    int mfactor = sfactor - mip;
    x *= 2;
    y *= 2;
    switch(SWS(w, x+x1, y+y1, mfactor)->type)
    {
        case SEMISOLID: if(issemi(mip, x+x1, y+y1, x1, y1, x2, y2)) return true;
        case CORNER:
        case SOLID: break;
        default: return true;
    }
    switch(SWS(w, x+x2, y+y2, mfactor)->type)
    {
        case SEMISOLID: if(issemi(mip, x+x2, y+y2, x1, y1, x2, y2)) return true;
        case CORNER:
        case SOLID: break;
        default: return true;
    }
    return false;
}

// the core recursive function, renders a rect of cubes at a certain mip level from a viewer perspective
// call itself for lower mip levels, on most modern machines however this function will use the higher
// mip levels only for perfect mips.

void render_seg_new(float vx, float vy, float vh, int mip, int x, int y, int xs, int ys,
                    int selection = -1)
{
#ifdef __3DS__
    const bool replay = selection >= 0;
    if(!replay && worldClipActive && outsideworldregion(mip, x, y, xs, ys)) return;
    ac3ds_world_dependency_mip(mip);
#endif
    sqr *w = wmip[mip];
    int mfactor = sfactor - mip;
    int sz = 1<<mfactor;
    int vxx = ((int)vx+(1<<mip)/2)>>mip;
    int vyy = ((int)vy+(1<<mip)/2)>>mip;
    int lx = vxx-lodleft;   // these mark the rect inside the current rest that we want to render using a lower mip level
    int ly = vyy-lodtop;
    int rx = vxx+lodright;
    int ry = vyy+lodbot;

    int pvx = (int)vx>>mip;
    int pvy = (int)vy>>mip;
#ifdef __3DS__
    extern bool showmipstats;
    const uchar *emptyruns = worldOcclusionActive && !showmipstats && !cubeSurfacePrefix[mip].empty()
                             ? cubeSurfacePrefix[mip].data() : nullptr;
    if(!replay && emptyruns && !(pvx>=x && pvx<xs && pvy>=y && pvy<ys) &&
       (ys-y <= 2 || mip == MAX_MIP))
    {
        bool empty = true;
        for(int row = y; row < ys && empty; ++row)
            empty = emptyruns[row*sz+x] >= xs-x;
        if(empty) return;
    }
    #define SKIP_EMPTY_CELLS \
        if(!replay && emptyruns) \
        { \
            const int run = emptyruns[yy*sz+xx]; \
            if(run) \
            { \
                const int skip = min(run, xs-xx)-1; \
                xx += skip; s += skip; \
                continue; \
            } \
        }
#else
    #define SKIP_EMPTY_CELLS
#endif

#ifdef __3DS__
    int recordedRegion = -1;
    if(recordWorldSelection && selectionhasroom())
    {
        recordedRegion = worldSelectionRegions.length();
        worldSelectionRegions.add({-1, -1, 0});
    }
    if(!replay)
    {
#endif
    float fsize = (float)(1<<mip);
    for(int oy = y; oy<ys; oy++)       // first collect occlusion information for this block
    {
        sqr *s = SWS(w,x,oy,mfactor);
        for(int ox = x; ox<xs; ox++, s++)
        {
#ifdef __3DS__
            s->occluded = worldOcclusionActive
                ? worldoccluded(s, ox<<mip, oy<<mip, fsize)
                : isoccluded(camera1->o.x, camera1->o.y, float(ox<<mip), float(oy<<mip), fsize);
#else
            s->occluded = isoccluded(camera1->o.x, camera1->o.y, (float)(ox<<mip), (float)(oy<<mip), fsize);
#endif
        }
    }

    if(pvx>=0 && pvy>=0 && pvx<sz && pvy<sz)
    {
        //SWS(w,vxx,vyy,mfactor)->occluded = 0;
        SWS(w, pvx, pvy, mfactor)->occluded = 0;  // player cell never occluded
    }
#ifdef __3DS__
    }
#endif

    #define df(x) s->floor-(x->vdelta/4.0f)
    #define dc(x) s->ceil+(x->vdelta/4.0f)

    // loop through the rect 3 times (for floor/ceil/walls seperately, to facilitate dynamic stripify)
    // for each we skip occluded cubes (occlusion at higher mip levels is a big time saver!).
    // during the first loop (ceil) we collect cubes that lie within the lower mip rect and are
    // also deferred, and render them recursively. Anything left (perfect mips and higher lods) we
    // render here.

    #ifdef __3DS__
    #define LOOPH {for(int yy = y; yy<ys; yy++) { sqr *s = SWS(w,x,yy,mfactor); \
                  for(int xx = x; xx<xs; xx++, s++) { if(s->occluded) continue;
    #define REPLAY_LOOP(body) \
        if(replay) \
        { \
            for(int i = worldSelectionRegions[selection].firstCell; i >= 0; i = worldSelectionCells[i].next) \
            { \
                const auto &cell = worldSelectionCells[i]; \
                if(cell.child >= 0) continue; \
                int xx = cell.x, yy = cell.y; \
                sqr *s = SWS(w,xx,yy,mfactor); \
                body \
            } \
        } \
        else LOOPH body }}}
    #else
    #define LOOPH {for(int yy = y; yy<ys; yy++) { sqr *s = SWS(w,x,yy,mfactor); \
                  for(int xx = x; xx<xs; xx++, s++) { if(s->occluded) continue;
    #define REPLAY_LOOP(body) LOOPH body }}}
    #endif
    #define LOOPD sqr *t = SWS(s,1,0,mfactor); \
                  sqr *v = SWS(s,0,1,mfactor); \
                  sqr *u = SWS(v,1,0,mfactor);

    auto emitfloor = [&](sqr *s, int xx, int yy)
    {
        switch(s->type)
        {
            case SPACE:
            case CHF:
                if(s->floor<=vh && render_floor)
                {
                    sqr *v = SWS(s,0,1,mfactor);
                    render_flat(s->ftex, xx<<mip, yy<<mip, 1<<mip, s->floor, s, s + 1, v + 1, v, false);
                    if(s->floor < waterlevel && !reflecting) addwaterquad(xx<<mip, yy<<mip, 1<<mip);
                }
                break;
            case FHF:
            {
                LOOPD
                render_flatdelta(s->ftex, xx<<mip, yy<<mip, 1<<mip, df(s), df(t), df(u), df(v), s, t, u, v, false);
                if(s->floor - s->vdelta/4.0f < waterlevel && !reflecting) addwaterquad(xx<<mip, yy<<mip, 1<<mip);
                break;
            }
        }
    };

    int rendered = 0;
#ifdef __3DS__
    if(replay)
    {
        const auto &region = worldSelectionRegions[selection];
        rendered = region.rendered;
        for(int i = region.firstCell; i >= 0; i = worldSelectionCells[i].next)
        {
            const auto &cell = worldSelectionCells[i];
            if(cell.child >= 0)
            {
                render_seg_new(vx, vy, vh, mip-1, 0, 0, 0, 0, cell.child);
                ac3ds_world_dependency_mip(mip);
            }
            else emitfloor(SWS(w,cell.x,cell.y,mfactor), cell.x, cell.y);
        }
    }
    else
#endif
    LOOPH   // floors
#ifdef __3DS__
        if(mip && xx>=lx && xx<rx && yy>=ly && yy<ry) ac3ds_record_world_defer(s);
#endif
        if(s->defer && mip && xx>=lx && xx<rx && yy>=ly && yy<ry)
        {
            s->occluded = 1; // shortcut for the other LOOPHs
            int start = xx;
            // Collect a 2xN rect of lower mip, including the first cube
            // which ends the deferred run as a cache dependency.
            while(xx<xs-1 && !s[1].occluded)
            {
#ifdef __3DS__
                ac3ds_record_world_defer(s+1);
#endif
                if(!s[1].defer) break;
                xx++;
                s++;
                if(xx<rx) s->occluded = 1;
            }
#ifdef __3DS__
            const int child = worldSelectionRegions.length();
#endif
            render_seg_new(vx, vy, vh, mip-1, start*2, yy*2, xx*2+2, yy*2+2);
#ifdef __3DS__
            if(recordWorldSelection && child < worldSelectionRegions.length())
                addselectioncell(recordedRegion, 0, 0, child);
            ac3ds_world_dependency_mip(mip);
#endif
            continue;
        }
#ifdef __3DS__
        // Test after handling deferred runs. The original horizontal
        // occlusion bits and their effect on mip selection stay intact.
        if(worldClipActive && outsideworldcell(s, mip, xx, yy, sz))
        {
            s->occluded = 1;
            continue;
        }
#endif
        rendered++;
#ifdef __3DS__
        if(recordWorldSelection && !(emptyruns && emptyruns[yy*sz+xx])) addselectioncell(recordedRegion, xx, yy, -1);
#endif
        emitfloor(s, xx, yy);
    }}}

#ifdef __3DS__
    if(recordWorldSelection) worldSelectionRegions[recordedRegion].rendered = rendered;
#endif
    if(!rendered) return;
    stats[mip] += rendered;

    if(!minimap) REPLAY_LOOP(   // ceils
        SKIP_EMPTY_CELLS
        switch(s->type)
        {
            case SPACE:
            case FHF:
                if(s->ceil>=vh && render_ceil)
                {
                    sqr *v = SWS(s,0,1,mfactor);
                    render_flat(s->ctex, xx<<mip, yy<<mip, 1<<mip, s->ceil, s, s + 1, v + 1, v, true);
                }
                break;
            case CHF:
            {
                LOOPD
                render_flatdelta(s->ctex, xx<<mip, yy<<mip, 1<<mip, dc(s), dc(t), dc(u), dc(v), s, t, u, v, true);
                break;
            }
        }
    )

    REPLAY_LOOP(   // walls
        SKIP_EMPTY_CELLS
        LOOPD
        //  w
        // zSt
        //  vu

        sqr *w = SWS(s,0,-1,mfactor);
        sqr *z = SWS(s,-1,0,mfactor);
        bool normalwall = true;

        if(s->type==CORNER)
        {
            // cull also
            bool topleft = true;
            sqr *h1 = NULL;
            sqr *h2 = NULL;
            if(SOLID(z))
            {
                if(SOLID(w))      { render_wall(w, h2 = s, xx+1, yy, xx, yy+1, mip, t, v, false, 4); topleft = false; }
                else if(SOLID(v)) { render_wall(v, h2 = s, xx, yy, xx+1, yy+1, mip, s, u, false, 5); }
            }
            else if(SOLID(t))
            {
                if(SOLID(w))      { render_wall(w, h1 = s, xx+1, yy+1, xx, yy, mip, u, s, false, 6); }
                else if(SOLID(v)) { render_wall(v, h1 = s, xx, yy+1, xx+1, yy, mip, v, t, false, 7); topleft = false; }
            }
            else
            {
                normalwall = false;
                bool wv = w->ceil-w->floor < v->ceil-v->floor;
                if(z->ceil-z->floor < t->ceil-t->floor)
                {
                    if(wv) { render_wall(h1 = s, h2 = v, xx+1, yy, xx, yy+1, mip, t, v, false, 4); topleft = false; }
                    else   { render_wall(h1 = s, h2 = w, xx, yy, xx+1, yy+1, mip, s, u, false, 5); }
                }
                else
                {
                    if(wv) { render_wall(h2 = s, h1 = v, xx+1, yy+1, xx, yy, mip, u, s, false, 6); }
                    else   { render_wall(h2 = s, h1 = w, xx, yy+1, xx+1, yy, mip, v, t, false, 7); topleft = false; }
                }
            }
            render_tris(xx<<mip, yy<<mip, 1<<mip, topleft, h1, h2, s, t, u, v);
        }

        if(normalwall)
        {
#ifdef __3DS__
            if(worldOcclusionActive && !cubeWallCandidates.empty())
            {
                if(!cubeWallRecipe.empty() && renderwallrecipe(s, mip, xx, yy, sz, vxx, vyy)) continue;
                const uchar walls = wallcandidates(s, mip, xx, yy, sz);
                if((walls & 1) && xx>=vxx) render_wall(s, z, xx,   yy,   xx,   yy+1, mip, s, v, true, 0);
                if((walls & 2) && xx<=vxx) render_wall(s, t, xx+1, yy,   xx+1, yy+1, mip, t, u, false, 1);
                if((walls & 4) && yy>=vyy) render_wall(s, w, xx,   yy,   xx+1, yy,   mip, s, t, false, 2);
                if((walls & 8) && yy<=vyy) render_wall(s, v, xx,   yy+1, xx+1, yy+1, mip, v, u, true, 3);
                continue;
            }
#endif
            if(xx>=vxx && xx!=0    && !SOLID(z) && (!SOLID(s) || z->type!=CORNER)
                && (z->type!=SEMISOLID || issemi(mip, xx-1, yy, 1, 0, 1, 1)))
                render_wall(s, z, xx,   yy,   xx,   yy+1, mip, s, v, true, 0);  // left
            if(xx<=vxx && xx!=sz-1 && !SOLID(t) && (!SOLID(s) || t->type!=CORNER)
                && (t->type!=SEMISOLID || issemi(mip, xx+1, yy, 0, 0, 0, 1)))
                render_wall(s, t, xx+1, yy,   xx+1, yy+1, mip, t, u, false, 1);  // right
            if(yy>=vyy && yy!=0    && !SOLID(w) && (!SOLID(s) || w->type!=CORNER)
                && (w->type!=SEMISOLID || issemi(mip, xx, yy-1, 0, 1, 1, 1)))
                render_wall(s, w, xx,   yy,   xx+1, yy,   mip, s, t, false, 2);  // top
            if(yy<=vyy && yy!=sz-1 && !SOLID(v) && (!SOLID(s) || v->type!=CORNER)
                && (v->type!=SEMISOLID || issemi(mip, xx, yy+1, 0, 0, 1, 0)))
                render_wall(s, v, xx,   yy+1, xx+1, yy+1, mip, v, u, true, 3);  // bot
        }
    )

    extern vector<int> tagclipcubes;
    extern bool showtagclipfocus;
    extern int showtagclips;
    if(editmode && !showtagclipfocus && showtagclips) LOOPH   // tag clips
        if(mip)
        {
            int ix1 = xx<<mip, ix2 = (xx+1)<<mip, iy1 = yy<<mip, iy2 = (yy+1)<<mip;
            sqr *tt = S(ix1, iy1), *t;
            for(int iy = iy1; iy < iy2; iy++, tt += ssize)
            {
                t = tt;
                for(int ix = ix1; ix < ix2; ix++, t++)
                {
                    if(t->tag & TAGANYCLIP && t->type != SOLID) tagclipcubes.add(ix + (iy << 16));
                }
            }
        }
        else if(s->tag & TAGANYCLIP && s->type != SOLID) tagclipcubes.add(xx + (yy << 16));
    }}}
    #undef REPLAY_LOOP
    #undef SKIP_EMPTY_CELLS
}

void distlod(int &low, int &high, int angle, float widef)
{
    float f = 90.0f/lod/widef;
    low = (int)((90-angle)/f);
    high = (int)(angle/f);
    if(low<min_lod) low = min_lod;
    if(high<min_lod) high = min_lod;
}

// does some out of date view frustrum optimisation that doesn't contribute much anymore

void render_world(float vx, float vy, float vh, float changelod, int yaw, int pitch, float fov, float fovy, int w, int h)
{
#ifdef __3DS__
    worldOcclusionActive = !minimap && !editmode && !m_coop;
    if(worldOcclusionActive) prepareworldocclusion();
    mainWorldClipActive = ac3dsmainworldclip && !reflecting && worldOcclusionActive;
    if(mainWorldClipActive) preparemainworldclip();
#endif
    loopi(LARGEST_FACTOR) stats[i] = 0;
    min_lod = minimap || (player1->isspectating() && player1->spectatemode == SM_OVERVIEW) ? MAX_LOD : MIN_LOD+iabs(pitch)/12;
    yaw = 360-yaw;
    float widef = fov/75.0f;
    int cdist = iabs(yaw%90-45);
    if(cdist<7)    // hack to avoid popup at high fovs at 45 yaw
    {
        min_lod = max(min_lod, (int)(MIN_LOD+(10-cdist)/1.0f*widef)); // less if lod worked better
        widef = 1.0f;
    }
    lod = (int)(lod*changelod);
    if(lod<minlod) lod = minlod;
    if(lod>MAX_LOD) lod = MAX_LOD;
    lodtop = lodbot = lodleft = lodright = min_lod;
    if(yaw>45 && yaw<=135)
    {
        lodleft = lod;
        distlod(lodtop, lodbot, yaw-45, widef);
    }
    else if(yaw>135 && yaw<=225)
    {
        lodbot = lod;
        distlod(lodleft, lodright, yaw-135, widef);
    }
    else if(yaw>225 && yaw<=315)
    {
        lodright = lod;
        distlod(lodbot, lodtop, yaw-225, widef);
    }
    else
    {
        lodtop = lod;
        distlod(lodright, lodleft, yaw<=45 ? yaw+45 : yaw-315, widef);
    }
    render_floor = pitch<0.5f*fovy;
    render_ceil  = -pitch<0.5f*fovy;

#ifdef __3DS__
    if(!reflecting && !minimap)
        refractionWorldView = {vx, vy, vh, fovy, fov, 360-yaw, pitch, lod, minlod,
                               ac3ds_world_geometry_generation(), ac3ds_raytable_generation(),
                               ac3ds_static_world_lighting_generation(), ac3ds_world_texture_generation(),
                               worldOcclusionActive && !worldClipActive && !mainWorldClipActive};
    extern int mergestrips;
    extern bool showmipstats;
    const bool cache = worldOcclusionActive && !showmipstats;
    const bool sharedSelection = ac3dssharedreflection && cache && ssize <= 1024;
    const WorldSelectionKey selectionKey = {vx, vy, ac3ds_world_geometry_generation(),
                                            ac3ds_raytable_generation(), ac3ds_static_world_lighting_generation(),
                                            lodleft, lodright, lodtop, lodbot};
    const bool dynamicSelection = ac3ds_dynamic_world_lighting();
    const bool replayWorldSelection = sharedSelection && reflecting && !refracting &&
                                      worldSelectionValid && worldSelectionKey == selectionKey &&
                                      worldSelectionLit == dynamicSelection;
    recordWorldSelection = false;
    const bool recordDependencies = cache && ssize <= 1024 &&
                                    (!dynamicSelection || ac3ds_can_record_lit_world_batch());
    const int pass = reflecting ? (refracting ? 2 : 1) : 0;
    Ac3dsWorldBatchKey key;
    if(cache)
    {
        // Flat floors and ceilings compare their integral heights against
        // camera Z. Heightfields and walls do not depend on viewer height.
        key = {{vx, vy, floorf(vh), ceilf(vh), camera1->o.x, camera1->o.y, waterlevel}, {},
               {lodleft, lodright, lodtop, lodbot, render_floor, render_ceil, lighterror, mergestrips,
                worldClipActive ? skyclip : 0, uniformtexres, mainWorldClipActive},
               ac3ds_world_geometry_generation(), ac3ds_raytable_generation(),
               ac3ds_static_world_lighting_generation(), ac3ds_world_texture_generation()};
        if(worldClipActive) loopi(6)
        {
            const auto &p = worldClip[i];
            key.clip[i*4] = p.x;
            key.clip[i*4+1] = p.y;
            key.clip[i*4+2] = p.z;
            key.clip[i*4+3] = p.offset;
        }
        // A selected reflection preserves all original strip boundaries;
        // a per-cell-clipped native batch has a distinct cache identity.
        if(replayWorldSelection && worldClipActive) key.options[10] = 2;
        if(mainWorldClipActive) loopi(24) key.clip[i] = mainWorldClip[i];
        if(ac3ds_restore_world_batch(pass, key))
        {
            if(!reflecting && !minimap && (dynamicSelection || worldSelectionLit)) worldSelectionValid = false;
            return;
        }
    }
    if(!reflecting && !minimap)
    {
        worldSelectionValid = sharedSelection;
        recordWorldSelection = sharedSelection;
        if(recordWorldSelection)
        {
            worldSelectionKey = selectionKey;
            worldSelectionLit = dynamicSelection;
            worldSelectionRegions.setsize(0);
            worldSelectionCells.setsize(0);
        }
    }
    ac3ds_begin_world_dependencies(recordDependencies);
    if(worldOcclusionActive)
    {
        preparewallcandidates();
        preparewallrecipes();
    }
    if(replayWorldSelection && worldClipActive) prepareselectedreflectionclip();
    else if(worldClipActive)
    {
        worldSkyMode = skyclip ? SkyAlways | (render_floor ? SkyFloor : 0) |
                                           (render_ceil ? SkyCeiling : 0) : 0;
        prepareclipcache();
    }
#endif
    render_seg_new(vx, vy, vh, MAX_MIP, 0, 0, ssize>>MAX_MIP, ssize>>MAX_MIP
#ifdef __3DS__
                   , replayWorldSelection ? 0 : -1
#endif
    );
#ifdef __3DS__
    if(replayWorldSelection && ac3dsTimingPhases)
    {
        if(worldSelectionProfile != ac3dsTimingGeneration)
        {
            worldSelectionProfile = ac3dsTimingGeneration;
            worldSelectionReplays = 0;
        }
        ++worldSelectionReplays;
    }
    if(recordWorldSelection && worldSelectionRegions.empty()) worldSelectionValid = false;
    // A lit selection contains that frame's defer bits. Never replay it
    // after the next main draw restores its unlit cache or changes lights.
    if(!reflecting && !minimap && !recordWorldSelection) worldSelectionValid = false;
    if(reflecting && !refracting && worldSelectionLit) worldSelectionValid = false;
    recordWorldSelection = false;
    ac3ds_begin_world_dependencies(false);
    if(recordDependencies && key.textures == ac3ds_world_texture_generation()) ac3ds_save_world_batch(key);
#endif
    mipstats(stats);
}


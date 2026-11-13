#include "ac3ds_gl.h"
#include "performance.h"

#include <3ds.h>
#include <citro3d.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
extern const unsigned char cube_vertex_shbin[];
extern const unsigned int cube_vertex_shbin_size;
Result __real_GX_ProcessCommandList(u32 *buffer, u32 bytes, u8 flags);

Result __wrap_GX_ProcessCommandList(u32 *buffer, u32 bytes, u8 flags)
{
    // Clean every segment, including Citro3D's internal clear/transfer
    // splits, before publishing it to the GSP queue.
    if(R_SUCCEEDED(GSPGPU_FlushDataCache(buffer,bytes))) flags &= ~GX_CMDLIST_FLUSH;
    else flags |= GX_CMDLIST_FLUSH;
    return __real_GX_ProcessCommandList(buffer,bytes,flags);
}
}

namespace
{
constexpr int screenW = 400, screenH = 240, units = 2;
constexpr size_t vertexCapacity = 98304, indexCapacity = 196608;
constexpr unsigned multiBatchVertices = 8192;
constexpr size_t maxPendingVertices = 8190;
// This flag still tells Citro3D that the port flushes vertex and texture data.
constexpr u32 submitFlags = GX_CMDLIST_FLUSH;
struct Mat
{
    float a[16]{};
    float &at(int row, int col) { return a[col*4 + row]; }
    float at(int row, int col) const { return a[col*4 + row]; }
    static Mat identity()
    {
        Mat m;
        for(int i = 0; i < 4; ++i) m.at(i, i) = 1;
        return m;
    }
};
Mat multiply(const Mat &a, const Mat &b)
{
    Mat out;
    for(int c = 0; c < 4; ++c) for(int r = 0; r < 4; ++r)
        for(int k = 0; k < 4; ++k) out.at(r,c) += a.at(r,k)*b.at(k,c);
    return out;
}
struct MatrixStack
{
    Mat current = Mat::identity();
    std::vector<Mat> saved;
};
struct Vertex
{
    float p[4] = {0,0,0,1};
    u8 color[4] = {255,255,255,255};
    float uv[units][3]{};
};
static_assert(sizeof(Vertex) == 44, "PICA vertex layout");
constexpr size_t vertexArenaBytes = sizeof(Vertex)*vertexCapacity;
constexpr size_t projectedVertexStride = 28;
constexpr size_t worldVertexStride = 24;
constexpr size_t modelVertexStride = 20;
constexpr size_t positionVertexStride = 12;
struct FontVertex
{
    float x, y, u, v;
    u32 color;
};
static_assert(sizeof(FontVertex) == 20, "PICA font vertex layout");
constexpr size_t fontQuadCapacity = maxPendingVertices/6;
enum class VertexLayout { Full, World, Projected, Model, Position, Font, Count };
struct PackedArray
{
    const u8 *data = nullptr;
    size_t stride = sizeof(Vertex);
    VertexLayout layout = VertexLayout::Full;
};
struct Array
{
    const u8 *data = nullptr;
    int size = 0, stride = 0;
    GLenum type = GL_FLOAT;
    bool on = false;
};
struct Tex
{
    C3D_Tex gpu{};
    C3D_RenderTarget *target = nullptr;
    int w = 0, h = 0, capacityMip = 0;
    GLenum format = GL_RGBA;
    GLint wrapS = GL_REPEAT, wrapT = GL_REPEAT, min = GL_NEAREST, mag = GL_LINEAR;
    bool rendered = false, dirty = false;
};
struct Combiner
{
    GLenum func = GL_MODULATE;
    GLenum sources[3] = {GL_TEXTURE, GL_PREVIOUS_ARB, GL_CONSTANT_ARB};
    GLenum operands[3] = {GL_SRC_COLOR, GL_SRC_COLOR, GL_SRC_ALPHA};
    int scale = 1;
};
struct Unit
{
    GLuint id = 0;
    bool on = false;
    MatrixStack matrix;
    GLenum mode = GL_MODULATE;
    Combiner rgb, alpha;
    u32 color = 0;
    Unit() { for(auto &op : alpha.operands) op = GL_SRC_ALPHA; }
};
struct Rect { int x, y, w, h; };
bool operator==(const Rect &a,const Rect &b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
struct ListDraw
{
    GLenum mode;
    std::vector<Vertex> vertices;
    std::vector<u16> indices;
};
struct ListRange { size_t vertex, index; };
struct List
{
    std::vector<ListDraw> commands;
    u8 *gpu = nullptr;
    u16 *gpuIndices = nullptr;
    std::vector<ListRange> ranges;
    float uv1[3]{};
    VertexLayout layout = VertexLayout::Full;
    bool baked = false, vertexColor = false;
};

std::unordered_map<GLuint, std::unique_ptr<Tex>> textures;
std::unordered_map<GLuint, List> lists;
std::vector<std::unique_ptr<Tex>> retiredTextures;
std::vector<void *> retiredGeometry;
std::vector<Vertex> immediate, pending, scratch;
std::vector<FontVertex> pendingFont;
float pendingFontUV1[3]{};
u8 *vertices = nullptr;
u16 *indices = nullptr, *fontIndices = nullptr;
u32 *pixels = nullptr, *aimTile = nullptr;
bool aimQueued = false;
unsigned queuedGx = 0;
u32 lastTopVblank = 0;
struct FrameCounters { unsigned draws, verts, inds, barriers; };
FrameCounters currentCounters{}, lastCounters{};
struct FrameProfile
{
    std::array<u64, 3600> intervals{};
    u64 lastTick = 0;
    u64 ticks = 0;
    u32 lastVblank = 0;
    unsigned count = 0, requested = 0, vblanks = 0, missed = 0, over20ms = 0;
    bool haveBaseline = false, report = false;
};
FrameProfile profile;
bool gameplayFrame = false;
bool lateVblankRequested = false, lateVblankFrame = false;
bool frameHasVblank = false, lastGameplayLate = false;
bool readOkay = true, fullFlush = false;
C3D_Tex dummyUnit1{};
size_t vused = 0, iused = 0, vflushed = 0, iflushed = 0;
struct ModelVertexUpload
{
    unsigned mesh, pose, lighting, generation;
    const u16 *source;
    size_t offset;
};
struct ModelIndexUpload
{
    unsigned mesh, generation;
    const u16 *source;
    size_t offset;
};
std::array<ModelVertexUpload, 512> modelVertexUploads{};
std::array<ModelIndexUpload, 512> modelIndexUploads{};
struct StencilUpload
{
    unsigned volume, generation;
    size_t vertexOffset, indexOffset;
};
std::array<StencilUpload, 256> stencilUploads{};
unsigned uploadGeneration = 0;
const u8 *worldSource = nullptr;
u8 *worldUpload = nullptr;
size_t worldBytes = 0;
struct WorldCachedDraw
{
    size_t vertexOffset, indexOffset;
    unsigned vertices, indices;
};
struct WorldGeometryCache
{
    u8 *data = nullptr;
    std::vector<WorldCachedDraw> draws;
    std::vector<size_t> ends;
    bool attempted = false;
};
std::array<WorldGeometryCache, Ac3dsWorldBatchSlots> worldGeometryCaches;
const WorldGeometryCache *activeWorldGeometry = nullptr;
size_t worldCachedBatch = 0, worldCachedDraw = 0;
std::vector<WorldCachedDraw> frameWorldDraws;
std::vector<size_t> frameWorldEnds;
unsigned frameWorldGeneration = 0;
bool captureFrameWorld = false, replayFrameWorld = false;
size_t frameWorldBatch = 0, frameWorldDraw = 0;
C3D_RenderTarget *top = nullptr, *target = nullptr;
std::array<C3D_RenderTarget *, 8> shadowTargets{};
struct WaterWorldSnapshot
{
    u8 *storage = nullptr;
    size_t colorBytes = 0, depthBytes = 0;
    unsigned generation = 0;
    bool valid = false;
};
std::array<WaterWorldSnapshot, 2> waterWorldSnapshots;
DVLB_s *shader = nullptr;
shaderProgram_s program{};
C3D_AttrInfo attributes[size_t(VertexLayout::Count)]{};
C3D_BufInfo vertexBuffers{};
VertexLayout appliedLayout = VertexLayout::Count;
const void *appliedBuffer = nullptr;
bool vertexBufferValid = false;
bool fastVertexOffsets = false;
bool joinedWaterCommands = false;
float appliedUV1[3]{};
bool fixedUV1Valid = false;
u32 appliedModelColor = 0;
bool fixedModelColorValid = false;
int locTransform = -1, locTex[units] = {-1,-1}, locColorScale = -1, locColor = -1;
bool ready = false, frame = false;
bool rasterDirty = true, transformDirty = true, fogDirty = true, tevDirty = true, clipWasOn = false;
bool flatColorActive = false;
unsigned samplerDirty = 0, uvDirty = (1u << units)-1;
std::array<C3D_TexEnv, 6> appliedTev{};
bool tevInitialized = false;
bool textureWasActive[units]{};
bool depthOn = false, writeDepth = true, blendOn = false, stencilOn = false;
bool cullOn = false, alphaOn = false, fogOn = false, scissorOn = false;
bool wireframe = false, offsetOn = false;
GLenum depthFunc = GL_LESS, alphaFunc = GL_ALWAYS, cullFace = GL_BACK;
GLenum blendSrc = GL_ONE, blendDst = GL_ZERO;
GLenum stencilFunc = GL_ALWAYS, stencilFail = GL_KEEP, depthFail = GL_KEEP, stencilPass = GL_KEEP;
unsigned colorMask = 15, stencilRef = 0, stencilReadMask = 255, clearStencil = 0;
float alphaRef = 0, clearDepth = 1, width = 1, polyOffset = 0;
float fogNear = 0, fogFar = 180, fogDensity = .25f;
u32 fogColor = 0, clearColor = 0;
struct FogTable
{
    C3D_FogLut lut{};
    float near = 0, far = 0, start = 0, end = 0;
    bool perspective = false, valid = false;
};
std::array<FogTable, 4> fogTables{};
unsigned nextFogTable = 0;
FogTable *boundFogTable = nullptr;
Rect viewport{0, 0, screenW, screenH}, scissor{0, 0, screenW, screenH};
Rect appliedViewport{}, appliedScissor{};
bool viewportValid = false, scissorWasOn = false;
struct RasterState
{
    u32 cull, depth, alpha, stencilTest, stencilOps, blend, fog;
};
RasterState appliedRaster{};
bool rasterStateValid = false;
Array positions, colors, normals, coords[units];
Unit texUnit[units];
MatrixStack model, projection;
GLenum matrixMode = GL_MODELVIEW;
int active = 0, client = 0, packAlignment = 4, unpackAlignment = 4;
Vertex current;
GLenum primitive = GL_TRIANGLES, pendingMode = GL_TRIANGLES;
bool inBegin = false, pendingClip = false;
GLuint nextTexture = 1, nextList = 1, compiling = 0;

float sat(float x) { return std::max(0.f, std::min(1.f, x)); }
u8 chan(float x) { return u8(sat(x)*255.f + .5f); }
u32 rgba(float r,float g,float b,float a)
{
    return u32(chan(r))<<24 | u32(chan(g))<<16 | u32(chan(b))<<8 | chan(a);
}
u32 tevColor(u32 c) { return __builtin_bswap32(c); }
int aligned(int n, int alignment) { return (n+alignment-1)&-alignment; }
int power2(int n)
{
    int v = 8;
    while(v < n) v <<= 1;
    return v;
}
MatrixStack &stack()
{
    return matrixMode == GL_TEXTURE ? texUnit[active].matrix :
           matrixMode == GL_PROJECTION ? projection : model;
}
Tex *findTex(GLuint id)
{
    auto it = textures.find(id);
    return it == textures.end() ? nullptr : it->second.get();
}
Tex *bound() { return findTex(texUnit[active].id); }
bool projected(int unit)
{
    const Mat &m = texUnit[unit].matrix.current;
    return m.a[3] != 0 || m.a[7] != 0 || m.a[11] != 0 || m.a[15] != 1;
}
void submitPending();
void dirtyRaster() { submitPending(); rasterDirty = true; }
void dirtyTransform() { submitPending(); transformDirty = true; }
void dirtyFog() { submitPending(); fogDirty = true; }
void dirtyTex() { submitPending(); tevDirty = true; }
void release(Tex &t)
{
    if(t.target) C3D_RenderTargetDelete(t.target);
    if(t.gpu.data) C3D_TexDelete(&t.gpu);
}
void retire()
{
    // Called only after FrameBegin has drained the prior GPU queue. Deleting a
    // render target also requires being outside the frame, so keep those until
    // a queue boundary and release their framebuffers there.
    for(auto i = retiredTextures.begin(); i != retiredTextures.end();)
    {
        if((*i)->target) { ++i; continue; }
        release(**i);
        i = retiredTextures.erase(i);
    }
    for(auto *v : retiredGeometry) linearFree(v);
    retiredGeometry.clear();
}
u32 waitTopVblank()
{
    Ac3dsPhaseTimer wait(Ac3dsPhase::VBlankWait);
    u32 slot;
    // A vblank that arrives during the counter check is already pending in
    // GSP's event queue. Only the top-screen counter drives gameplay pace.
    while((slot = C3D_FrameCounter(0)) == lastTopVblank) gspWaitForAnyEvent();
    return slot;
}
bool startFrame(bool sync)
{
    if(!ready || frame) return frame;
    if(sync) waitTopVblank();
    Ac3dsPhaseTimer queue(Ac3dsPhase::QueueStart);
    const bool started = C3D_FrameBegin(0);
    queue.stop();
    if(!started) return false;
    if(sync) lastTopVblank = C3D_FrameCounter(0);
    frameHasVblank = sync;
    frame = true;
    retire();
    // APT restore can dirty Citro3D's saved loader state. Re-establish the
    // complete buffer description before using offset-only updates.
    vertexBufferValid = false;
    vused = iused = vflushed = iflushed = 0;
    if(!++uploadGeneration)
    {
        modelVertexUploads.fill({});
        modelIndexUploads.fill({});
        stencilUploads.fill({});
        ++uploadGeneration;
    }
    worldUpload = nullptr;
    queuedGx = 0;
    fullFlush = false;
    rasterDirty = transformDirty = fogDirty = tevDirty = true;
    uvDirty = (1u << units)-1;
    if(target && target != top)
    {
        C3D_FrameDrawOn(target);
        viewportValid = false;
    }
    else
    {
        C3D_FrameDrawOn(top);
        viewportValid = false;
    }
    return true;
}
void flushData()
{
    if(vused != vflushed)
    {
        if(R_FAILED(GSPGPU_FlushDataCache(vertices+vflushed,vused-vflushed))) fullFlush = true;
        vflushed = vused;
    }
    if(iused != iflushed)
    {
        if(R_FAILED(GSPGPU_FlushDataCache(indices+iflushed,(iused-iflushed)*sizeof(u16)))) fullFlush = true;
        iflushed = iused;
    }
}
void recycleFrame()
{
    ++currentCounters.barriers;
    const bool present = top->used;
    const bool hasVblank = frameHasVblank;
    top->used = false;
    flushData();
    C3D_FrameEnd(fullFlush ? 0 : submitFlags);
    frame = false;
    startFrame(false);
    frameHasVblank = hasVblank;
    top->used = present;
}
void selectTarget(C3D_RenderTarget *next)
{
    submitPending();
    target = next;
    if(startFrame(false))
    {
        if(target == top)
        {
            C3D_FrameDrawOn(top);
            viewportValid = false;
        }
        else
        {
            C3D_FrameDrawOn(target);
            viewportValid = false;
        }
    }
    rasterDirty = transformDirty = true;
}
GPU_TESTFUNC compare(GLenum func, bool reverse = false)
{
    switch(func)
    {
        case GL_NEVER: return GPU_NEVER;
        case GL_LESS: return reverse ? GPU_GREATER : GPU_LESS;
        case GL_LEQUAL: return reverse ? GPU_GEQUAL : GPU_LEQUAL;
        case GL_GREATER: return reverse ? GPU_LESS : GPU_GREATER;
        case GL_GEQUAL: return reverse ? GPU_LEQUAL : GPU_GEQUAL;
        case GL_EQUAL: return GPU_EQUAL;
        case GL_NOTEQUAL: return GPU_NOTEQUAL;
        default: return GPU_ALWAYS;
    }
}
GPU_BLENDFACTOR blend(GLenum f)
{
    switch(f)
    {
        case GL_ZERO: return GPU_ZERO;
        case GL_SRC_COLOR: return GPU_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return GPU_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return GPU_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return GPU_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
        case GL_SRC_ALPHA_SATURATE: return GPU_SRC_ALPHA_SATURATE;
        default: return GPU_ONE;
    }
}
GPU_STENCILOP stencil(GLenum op)
{
    switch(op)
    {
        case GL_ZERO: return GPU_STENCIL_ZERO;
        case GL_REPLACE: return GPU_STENCIL_REPLACE;
        case GL_INCR: return GPU_STENCIL_INCR;
        case GL_DECR: return GPU_STENCIL_DECR;
        case GL_INCR_WRAP_EXT: return GPU_STENCIL_INCR_WRAP;
        case GL_DECR_WRAP_EXT: return GPU_STENCIL_DECR_WRAP;
        case GL_INVERT: return GPU_STENCIL_INVERT;
        default: return GPU_STENCIL_KEEP;
    }
}
GPU_TEVSRC tevSource(GLenum s, int unit, bool previous)
{
    switch(s)
    {
        case GL_TEXTURE: return unit ? GPU_TEXTURE1 : GPU_TEXTURE0;
        case GL_CONSTANT_ARB: return GPU_CONSTANT;
        case GL_PREVIOUS_ARB: return previous ? GPU_PREVIOUS : GPU_PRIMARY_COLOR;
        default: return GPU_PRIMARY_COLOR;
    }
}
GPU_COMBINEFUNC tevFunction(GLenum f)
{
    switch(f)
    {
        case GL_MODULATE: return GPU_MODULATE;
        case GL_ADD: return GPU_ADD;
        case GL_SUBTRACT_ARB: return GPU_SUBTRACT;
        case GL_INTERPOLATE_ARB: return GPU_INTERPOLATE;
        case GL_DOT3_RGB_ARB: return GPU_DOT3_RGB;
        default: return GPU_REPLACE;
    }
}
GPU_TEVOP_RGB rgbOp(GLenum f)
{
    switch(f)
    {
        case GL_ONE_MINUS_SRC_COLOR: return GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR;
        case GL_SRC_ALPHA: return GPU_TEVOP_RGB_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA;
        default: return GPU_TEVOP_RGB_SRC_COLOR;
    }
}
GPU_TEVOP_A alphaOp(GLenum f)
{
    return f == GL_ONE_MINUS_SRC_ALPHA ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
}
GPU_TEVSCALE scale(int f)
{
    return f == 4 ? GPU_TEVSCALE_4 : f == 2 ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1;
}
GPU_TEXTURE_WRAP_PARAM texWrap(GLenum f)
{
    return f == GL_REPEAT ? GPU_REPEAT : f == GL_MIRRORED_REPEAT ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE;
}
void applySampler(Tex &t, bool project)
{
    const bool mip = t.min != GL_LINEAR && t.min != GL_NEAREST;
    const bool linear = t.min == GL_LINEAR || t.min == GL_LINEAR_MIPMAP_LINEAR || t.min == GL_LINEAR_MIPMAP_NEAREST;
    C3D_TexSetFilter(&t.gpu, t.mag == GL_NEAREST ? GPU_NEAREST : GPU_LINEAR, linear ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetFilterMipmap(&t.gpu, t.min == GL_LINEAR_MIPMAP_LINEAR || t.min == GL_NEAREST_MIPMAP_LINEAR ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&t.gpu, texWrap(t.wrapS), texWrap(t.wrapT));
    if(t.dirty)
    {
        t.gpu.maxLevel = t.capacityMip;
        const u32 bytes = C3D_TexCalcTotalSize(t.gpu.size,t.capacityMip);
        if(R_FAILED(GSPGPU_FlushDataCache(t.gpu.data,bytes))) fullFlush = true;
        t.dirty = false;
    }
    t.gpu.maxLevel = mip ? t.capacityMip : 0;
    t.gpu.param = (t.gpu.param & ~GPU_TEXTURE_MODE(7)) | GPU_TEXTURE_MODE(project ? GPU_TEX_PROJECTION : GPU_TEX_2D);
}
void applyTex()
{
    std::array<C3D_TexEnv, 6> next{};
    int stage = 0;
    for(int n = 0; n < units; ++n)
    {
        Unit &u = texUnit[n];
        Tex *t = u.on ? findTex(u.id) : nullptr;
        if(!t || !t->gpu.data)
        {
            // Older Citro3D dereferences the pointer on units 1 and 2.
            // Bind a defined texel rather than passing null to unit one.
            if(!tevInitialized || textureWasActive[n]) C3D_TexBind(n,n ? &dummyUnit1 : nullptr);
            textureWasActive[n] = false;
            continue;
        }
        textureWasActive[n] = true;
        applySampler(*t, n == 0 && projected(n));
        C3D_TexBind(n, &t->gpu);
        C3D_TexEnv *env = &next[stage];
        C3D_TexEnvInit(env);
        Combiner rgb = u.rgb, alpha = u.alpha;
        if(u.mode != GL_COMBINE_ARB)
        {
            rgb = Combiner{};
            alpha = Combiner{};
            for(auto &op : alpha.operands) op = GL_SRC_ALPHA;
            rgb.func = u.mode;
            alpha.func = u.mode == GL_REPLACE ? GL_REPLACE : GL_MODULATE;
        }
        if(t->format == GL_ALPHA && u.mode != GL_COMBINE_ARB)
        {
            rgb.func = GL_REPLACE;
            rgb.sources[0] = GL_PREVIOUS_ARB;
        }
        C3D_TexEnvSrc(env, C3D_RGB, tevSource(rgb.sources[0],n,stage), tevSource(rgb.sources[1],n,stage), tevSource(rgb.sources[2],n,stage));
        C3D_TexEnvSrc(env, C3D_Alpha, tevSource(alpha.sources[0],n,stage), tevSource(alpha.sources[1],n,stage), tevSource(alpha.sources[2],n,stage));
        C3D_TexEnvOpRgb(env, rgbOp(rgb.operands[0]),rgbOp(rgb.operands[1]),rgbOp(rgb.operands[2]));
        C3D_TexEnvOpAlpha(env, alphaOp(alpha.operands[0]),alphaOp(alpha.operands[1]),alphaOp(alpha.operands[2]));
        C3D_TexEnvFunc(env, C3D_RGB, tevFunction(rgb.func));
        C3D_TexEnvFunc(env, C3D_Alpha, tevFunction(alpha.func));
        C3D_TexEnvScale(env, C3D_RGB, scale(rgb.scale));
        C3D_TexEnvScale(env, C3D_Alpha, scale(alpha.scale));
        C3D_TexEnvColor(env, tevColor(u.color));
        ++stage;
    }
    for(int i = stage; i < int(next.size()); ++i)
    {
        C3D_TexEnv *env = &next[i];
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, i ? GPU_PREVIOUS : GPU_PRIMARY_COLOR, GPU_PREVIOUS, GPU_PREVIOUS);
    }
    for(int i = 0; i < int(next.size()); ++i)
    {
        if(tevInitialized && std::memcmp(&appliedTev[i], &next[i], sizeof(next[i])) == 0) continue;
        C3D_SetTexEnv(i, &next[i]);
        appliedTev[i] = next[i];
    }
    tevInitialized = true;
    samplerDirty = 0;
}

void applySamplers()
{
    for(int n = 0; n < units; ++n)
    {
        if(!(samplerDirty & (1u << n))) continue;
        Tex *t = findTex(texUnit[n].id);
        if(!texUnit[n].on || !t || !t->gpu.data) continue;
        applySampler(*t, n == 0 && projected(n));
        C3D_TexBind(n, &t->gpu);
    }
    samplerDirty = 0;
}
Mat clipTransform()
{
    Mat m = multiply(projection.current,model.current);
    Mat out = m;
    for(int i = 0; i < 4; ++i)
    {
        if(target == top) { out.at(0,i) = m.at(1,i); out.at(1,i) = -m.at(0,i); }
        out.at(2,i) = (m.at(2,i)-m.at(3,i))*.5f + (offsetOn ? polyOffset*m.at(3,i) : 0.f);
    }
    return out;
}
void uniformRow(int loc, const Mat &m, int r)
{
    const C3D_FVec &value = C3D_FVUnif[GPU_VERTEX_SHADER][loc];
    const float x = m.at(r,0), y = m.at(r,1), z = m.at(r,2), w = m.at(r,3);
    if(value.x != x || value.y != y || value.z != z || value.w != w)
        C3D_FVUnifSet(GPU_VERTEX_SHADER,loc,x,y,z,w);
}
void useVertexColor()
{
    if(!flatColorActive) return;
    C3D_FVUnifSet(GPU_VERTEX_SHADER,locColorScale,1.f/255.f,0,0,0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER,locColor,0,0,0,0);
    flatColorActive = false;
}
void applyTextureMatrices()
{
    for(int n = 0; n < units; ++n)
    {
        if(!(uvDirty & (1u << n))) continue;
        Mat uv = texUnit[n].matrix.current;
        const Tex *t = findTex(texUnit[n].id);
        const bool rendered = t && t->rendered;
        // Uploaded source rows start at native tile row zero; render targets
        // instead follow framebuffer orientation.
        if(!rendered) for(int i = 0; i < 4; ++i) uv.at(1,i) = uv.at(3,i)-uv.at(1,i);
        uniformRow(locTex[n],uv,0);
        uniformRow(locTex[n]+1,uv,1);
        uniformRow(locTex[n]+2,uv,3);
    }
    uvDirty = 0;
}
Rect nativeRect(Rect r)
{
    const bool tilted = target == top;
    if(tilted) return {r.y,screenW-r.x-r.w,r.h,r.w};
    return r;
}
void applyRaster(bool clip)
{
    Rect r = nativeRect(viewport);
    const bool viewportChanged = !viewportValid || !(r == appliedViewport);
    if(viewportChanged)
    {
        C3D_SetViewport(std::max(0,r.x),std::max(0,r.y),std::max(1,r.w),std::max(1,r.h));
        appliedViewport = r;
        viewportValid = true;
    }
    if(scissorOn)
    {
        Rect s = nativeRect(scissor);
        int l = std::max(s.x,0), t = std::max(s.y,0);
        int rr = std::min(s.x+s.w,int(target->frameBuf.width));
        int bb = std::min(s.y+s.h,int(target->frameBuf.height));
        const bool valid = l < rr && t < bb;
        const Rect bounds = valid ? Rect{l,t,rr,bb} : Rect{0,0,-target->frameBuf.width,-target->frameBuf.height};
        if(viewportChanged || !scissorWasOn || !(bounds == appliedScissor))
        {
            if(valid) C3D_SetScissor(GPU_SCISSOR_NORMAL,l,t,rr,bb);
            else C3D_SetScissor(GPU_SCISSOR_INVERT,0,0,target->frameBuf.width,target->frameBuf.height);
            appliedScissor = bounds;
        }
    }
    else if(!viewportChanged && scissorWasOn) C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    scissorWasOn = scissorOn;
    const auto cull = !cullOn || wireframe || clip ? GPU_CULL_NONE : cullFace == GL_FRONT ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;
    const auto mask = GPU_WRITEMASK(colorMask | (depthOn && writeDepth ? GPU_WRITE_DEPTH : 0));
    const auto depth = compare(depthFunc,true), alpha = compare(alphaFunc), test = compare(stencilFunc);
    const u8 alphaThreshold = chan(alphaRef);
    const auto fail = stencil(stencilFail), zfail = stencil(depthFail), pass = stencil(stencilPass);
    const GPU_BLENDFACTOR src = blendOn ? blend(blendSrc) : GPU_ONE, dst = blendOn ? blend(blendDst) : GPU_ZERO;
    const RasterState next{u32(cull), u32(depthOn)|(u32(depth)<<1)|(u32(mask)<<4),
                           u32(alphaOn)|(u32(alpha)<<1)|(u32(alphaThreshold)<<4),
                           u32(stencilOn)|(u32(test)<<1)|(stencilRef<<4)|(stencilReadMask<<12),
                           u32(fail)|(u32(zfail)<<3)|(u32(pass)<<6), u32(src)|(u32(dst)<<4),
                           fogColor|(u32(fogOn)<<24)};
    if(!rasterStateValid || next.cull != appliedRaster.cull) C3D_CullFace(cull);
    if(!rasterStateValid || next.depth != appliedRaster.depth) C3D_DepthTest(depthOn,depth,mask);
    if(!rasterStateValid || next.alpha != appliedRaster.alpha) C3D_AlphaTest(alphaOn,alpha,alphaThreshold);
    if(!rasterStateValid || next.stencilTest != appliedRaster.stencilTest) C3D_StencilTest(stencilOn,test,stencilRef,stencilReadMask,255);
    if(!rasterStateValid || next.stencilOps != appliedRaster.stencilOps) C3D_StencilOp(fail,zfail,pass);
    if(!rasterStateValid || next.blend != appliedRaster.blend) C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,src,dst,src,dst);
    if(!rasterStateValid || next.fog != appliedRaster.fog)
    {
        C3D_FogGasMode(fogOn ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_FogColor(fogColor);
    }
    appliedRaster = next;
    rasterStateValid = true;
}
void applyFog()
{
    if(!fogOn) return;
    const Mat &p = projection.current;
    const bool perspective = p.a[11] != 0;
    float near = perspective ? p.a[14]/(p.a[10]-1.f) : (p.a[14]+1.f)/p.a[10];
    float far = perspective ? p.a[14]/(p.a[10]+1.f) : (p.a[14]-1.f)/p.a[10];
    if(!std::isfinite(near) || !std::isfinite(far) || far <= near) { near = .15f; far = 450; }
    for(FogTable &table : fogTables)
    {
        if(!table.valid || table.near != near || table.far != far || table.start != fogNear ||
           table.end != fogFar || table.perspective != perspective) continue;
        if(boundFogTable != &table) C3D_FogLutBind(&table.lut);
        boundFogTable = &table;
        return;
    }
    float data[256], prev = 0;
    for(int i = 0; i <= 128; ++i)
    {
        float depth = i/128.f;
        float z = perspective ? FogLut_CalcZ(depth,near,far) : far-depth*(far-near);
        float v = sat((fogFar-z)/std::max(fogFar-fogNear,.0001f));
        if(i < 128) data[i] = v;
        if(i) data[i+127] = v-prev;
        prev = v;
    }
    FogTable &table = fogTables[nextFogTable++ % fogTables.size()];
    FogLut_FromArray(&table.lut,data);
    table.near = near;
    table.far = far;
    table.start = fogNear;
    table.end = fogFar;
    table.perspective = perspective;
    table.valid = true;
    C3D_FogLutBind(&table.lut);
    boundFogTable = &table;
}
void apply(bool clip)
{
    if(clip != clipWasOn) rasterDirty = true;
    if(rasterDirty) { applyRaster(clip); rasterDirty = false; }
    if(tevDirty) { applyTex(); tevDirty = false; }
    else if(samplerDirty) applySamplers();
    if(clip != clipWasOn || (!clip && transformDirty))
    {
        const Mat m = clip ? Mat::identity() : clipTransform();
        for(int i = 0; i < 4; ++i) uniformRow(locTransform+i,m,i);
        transformDirty = clip;
        clipWasOn = clip;
    }
    if(uvDirty) applyTextureMatrices();
    if(fogDirty) { applyFog(); fogDirty = false; }
}
void useBuffer(const void *v, VertexLayout layout = VertexLayout::Full, const float *fixedUV1 = current.uv[1],
               bool fixedColor = true)
{
    if(layout == VertexLayout::Model && fixedColor)
    {
        u32 color;
        std::memcpy(&color,current.color,sizeof(color));
        if(!fixedModelColorValid || color != appliedModelColor)
        {
            C3D_FixedAttribSet(2,current.color[0],current.color[1],current.color[2],current.color[3]);
            fixedModelColorValid = true;
            appliedModelColor = color;
        }
    }
    if(layout != VertexLayout::Full && layout != VertexLayout::Position &&
       (!fixedUV1Valid || std::memcmp(appliedUV1,fixedUV1,sizeof(appliedUV1))))
    {
        C3D_FixedAttribSet(3,fixedUV1[0],fixedUV1[1],fixedUV1[2],1);
        std::memcpy(appliedUV1,fixedUV1,sizeof(appliedUV1));
        fixedUV1Valid = true;
    }
    const bool layoutChanged = layout != appliedLayout;
    if(vertexBufferValid && !layoutChanged && v == appliedBuffer) return;
    if(layoutChanged)
    {
        C3D_SetAttrInfo(&attributes[size_t(layout)]);
        // Loaded attributes are laid out in ascending order in each case.
        // The compact layouts set shader input v3 as a fixed attribute.
        const unsigned stride = layout == VertexLayout::World ? worldVertexStride :
                                layout == VertexLayout::Projected ? projectedVertexStride :
                                layout == VertexLayout::Model ? modelVertexStride :
                                layout == VertexLayout::Position ? positionVertexStride :
                                layout == VertexLayout::Font ? sizeof(FontVertex) : sizeof(Vertex);
        vertexBuffers.buffers[0].flags[0] = layout == VertexLayout::Full ? 0x3210 :
                                            layout == VertexLayout::Model ? 0x10 :
                                            layout == VertexLayout::Position ? 0 : 0x210;
        const unsigned loaders = layout == VertexLayout::Full ? 4 : layout == VertexLayout::Model ? 2 :
                                 layout == VertexLayout::Position ? 1 : 3;
        vertexBuffers.buffers[0].flags[1] = (stride << 16) | (loaders << 28);
        appliedLayout = layout;
    }
    vertexBuffers.buffers[0].offset = osConvertVirtToPhys(v)-vertexBuffers.base_paddr;
    if(!fastVertexOffsets || !vertexBufferValid || layoutChanged) C3D_SetBufInfo(&vertexBuffers);
    else
    {
        // The base, stride and permutation are unchanged. Citro3D's index
        // calculation still uses the same base recorded by the full bind.
        GPUCMD_AddWrite(GPUREG_ATTRIBBUFFER0_OFFSET,vertexBuffers.buffers[0].offset);
    }
    vertexBufferValid = true;
    appliedBuffer = v;
}
GPU_Primitive_t gpuMode(GLenum mode)
{
    return mode == GL_TRIANGLE_STRIP ? GPU_TRIANGLE_STRIP : mode == GL_TRIANGLE_FAN ? GPU_TRIANGLE_FAN : GPU_TRIANGLES;
}
void reserveCommands()
{
    if(gpuCmdBufOffset+4096 >= gpuCmdBufSize || queuedGx >= 25) recycleFrame();
}
void alignVertices()
{
    vused = (vused+7)&~size_t(7);
}
unsigned triangleIndices(GLenum mode, unsigned len);
void appendTriangles(u16 *&out, GLenum mode, unsigned base, unsigned len);

void submitFont()
{
    if(!startFrame(false)) { pendingFont.clear(); return; }
    reserveCommands();
    alignVertices();
    const size_t bytes = pendingFont.size()*sizeof(FontVertex);
    if(vused+bytes > vertexArenaBytes) recycleFrame();
    std::memcpy(vertices+vused,pendingFont.data(),bytes);
    apply(false);
    useVertexColor();
    useBuffer(vertices+vused,VertexLayout::Font,pendingFontUV1);
    const size_t count = pendingFont.size()/4*6;
    C3D_DrawElements(GPU_TRIANGLES,count,C3D_UNSIGNED_SHORT,fontIndices);
    ++currentCounters.draws;
    currentCounters.verts += pendingFont.size();
    currentCounters.inds += count;
    vused += bytes;
    pendingFont.clear();
}
void submitPending()
{
    if(!pendingFont.empty()) submitFont();
    if(pending.empty()) return;
    if(!startFrame(false)) { pending.clear(); return; }
    reserveCommands();
    alignVertices();
    iused = (iused+3)&~size_t(3);
    const size_t bytes = pending.size()*sizeof(Vertex);
    const unsigned count = triangleIndices(pendingMode,pending.size());
    if(pending.size() > UINT16_MAX || count > indexCapacity) { pending.clear(); return; }
    if(!count) { pending.clear(); return; }
    if(vused+bytes > vertexArenaBytes || iused+count > indexCapacity) recycleFrame();
    u16 *out = indices+iused;
    appendTriangles(out,pendingMode,0,pending.size());
    std::memcpy(vertices+vused,pending.data(),bytes);
    apply(pendingClip);
    useVertexColor();
    useBuffer(vertices+vused);
    C3D_DrawElements(GPU_TRIANGLES,count,C3D_UNSIGNED_SHORT,indices+iused);
    ++currentCounters.draws; currentCounters.verts += pending.size();
    currentCounters.inds += count;
    iused += count;
    vused += bytes;
    pending.clear();
}
void enqueue(const Vertex *v, size_t count, GLenum mode, bool clip)
{
    if(!count) return;
    if(!pendingFont.empty() || pendingMode != mode || pendingClip != clip || mode != GL_TRIANGLES ||
       pending.size()+count > maxPendingVertices)
        submitPending();
    pendingMode = mode; pendingClip = clip;
    pending.insert(pending.end(),v,v+count);
    if(mode != GL_TRIANGLES || pending.size() >= maxPendingVertices) submitPending();
}
Vertex applyMatrix(Vertex v, const Mat &m)
{
    float p[4];
    std::copy(v.p,v.p+4,p);
    for(int i = 0; i < 4; ++i)
        v.p[i] = m.at(i,0)*p[0]+m.at(i,1)*p[1]+m.at(i,2)*p[2]+m.at(i,3)*p[3];
    return v;
}
Vertex between(const Vertex &a,const Vertex &b,float f)
{
    Vertex v;
    for(int k = 0; k < 4; ++k)
    {
        v.p[k] = a.p[k]+f*(b.p[k]-a.p[k]);
        v.color[k] = u8(a.color[k]+f*(b.color[k]-a.color[k]));
    }
    for(int n = 0; n < units; ++n) for(int k = 0; k < 3; ++k)
        v.uv[n][k] = a.uv[n][k]+f*(b.uv[n][k]-a.uv[n][k]);
    return v;
}
void line(Vertex a,Vertex b,const Mat &transform)
{
    a = applyMatrix(a,transform); b = applyMatrix(b,transform);
    // Clip homogeneous endpoints before deriving pixel width. In particular a
    // line passing behind the near plane must not expand across the screen.
    for(int k = 0; k < 6; ++k)
    {
        auto distance = [k](const Vertex &v)
        {
            switch(k)
            {
                case 0: return v.p[3]+v.p[0]; case 1: return v.p[3]-v.p[0];
                case 2: return v.p[3]+v.p[1]; case 3: return v.p[3]-v.p[1];
                case 4: return v.p[3]+v.p[2]; default: return -v.p[2];
            }
        };
        float da = distance(a), db = distance(b);
        if(da < 0 && db < 0) return;
        if(da < 0) a = between(a,b,da/(da-db));
        if(db < 0) b = between(b,a,db/(db-da));
    }
    if(a.p[3] <= 0 || b.p[3] <= 0) return;
    Rect r = nativeRect(viewport);
    if(r.w <= 0 || r.h <= 0) return;
    float dx = (b.p[0]/b.p[3]-a.p[0]/a.p[3])*r.w;
    float dy = (b.p[1]/b.p[3]-a.p[1]/a.p[3])*r.h;
    float length = std::sqrt(dx*dx+dy*dy);
    if(length < .00001f) return;
    dx /= length; dy /= length;
    float nx = -dy*width/r.w, ny = dx*width/r.h;
    Vertex q[6] = {a,b,b,a,b,a};
    for(int i = 0; i < 6; ++i)
    {
        float sign = i < 2 || i == 3 ? 1.f : -1.f;
        q[i].p[0] += nx*q[i].p[3]*sign;
        q[i].p[1] += ny*q[i].p[3]*sign;
    }
    enqueue(q,6,GL_TRIANGLES,true);
}
void emit(GLenum mode, const Vertex *v, size_t n)
{
    if(!n) return;
    if(compiling)
    {
        lists[compiling].commands.push_back({mode,std::vector<Vertex>(v,v+n),{}});
        return;
    }
    if(mode == GL_LINES || mode == GL_LINE_LOOP || wireframe)
    {
        Mat m = clipTransform();
        auto edge = [&](size_t a, size_t b) { line(v[a],v[b],m); };
        if(mode == GL_LINES) for(size_t i = 1; i < n; i += 2) edge(i-1,i);
        else if(mode == GL_LINE_LOOP)
        {
            for(size_t i = 1; i < n; ++i) edge(i-1,i);
            if(n > 1) edge(n-1,0);
        }
        else if(mode == GL_QUADS || mode == GL_TRIANGLES)
        {
            size_t s = mode == GL_QUADS ? 4 : 3;
            for(size_t i = 0; i+s <= n; i += s) for(size_t j = 0; j < s; ++j) edge(i+j,i+(j+1)%s);
        }
        else for(size_t i = 2; i < n; ++i)
        {
            size_t first = mode == GL_TRIANGLE_FAN ? 0 : i-2;
            edge(first,i-1); edge(i-1,i); edge(i,first);
        }
        return;
    }
    if(mode == GL_QUADS)
    {
        // Preserve Cube's quad order and tessellate without duplicating the
        // input buffer beyond this small, compatible triangle batch.
        for(size_t i = 0; i+3 < n; i += 4)
        {
            Vertex q[6] = {v[i],v[i+1],v[i+2],v[i],v[i+2],v[i+3]};
            enqueue(q,6,GL_TRIANGLES,false);
        }
    }
    else if(mode == GL_TRIANGLES)
        for(size_t i = 0; i+2 < n; i += 8190) enqueue(v+i,std::min<size_t>(8190,(n-i)/3*3),mode,false);
    else enqueue(v,n,mode,false);
}
size_t elementSize(GLenum type)
{
    switch(type)
    {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        case GL_DOUBLE: return 8;
        default: return 4;
    }
}
template<class T> T read(const u8 *p)
{
    T val;
    std::memcpy(&val,p,sizeof(val));
    return val;
}
float component(const u8 *p,GLenum type)
{
    switch(type)
    {
        case GL_UNSIGNED_BYTE: return *p;
        case GL_BYTE: return read<int8_t>(p);
        case GL_UNSIGNED_SHORT: return read<u16>(p);
        case GL_SHORT: return read<int16_t>(p);
        case GL_UNSIGNED_INT: return read<u32>(p);
        case GL_INT: return read<int32_t>(p);
        case GL_DOUBLE: return read<double>(p);
        default: return read<float>(p);
    }
}
void arrayValues(const Array &a,size_t i,float *out)
{
    const size_t elem = elementSize(a.type);
    const u8 *p = a.data+i*(a.stride ? a.stride : a.size*elem);
    for(int k = 0; k < a.size; ++k) out[k] = component(p+k*elem,a.type);
}
Vertex vertexAt(size_t i)
{
    Vertex v = current;
    // Cube's hot world/model paths feed naturally aligned float positions.
    // Copy the position and UVs as spans instead of decoding each component.
    if(positions.data && positions.type == GL_FLOAT)
        std::memcpy(v.p,positions.data+i*(positions.stride ? positions.stride : positions.size*4),positions.size*4);
    else if(positions.data) arrayValues(positions,i,v.p);
    if(colors.on && colors.data)
    {
        if(colors.type == GL_UNSIGNED_BYTE)
            std::memcpy(v.color,colors.data+i*(colors.stride ? colors.stride : colors.size),colors.size);
        else
        {
            float c[4] = {1,1,1,1};
            arrayValues(colors,i,c);
            for(int k = 0; k < colors.size; ++k) v.color[k] = chan(c[k]);
        }
        if(colors.size == 3) v.color[3] = 255;
    }
    for(int n = 0; n < units; ++n) if(coords[n].on && coords[n].data)
    {
        v.uv[n][2] = 0;
        const Array &a = coords[n];
        if(a.type == GL_FLOAT) std::memcpy(v.uv[n],a.data+i*(a.stride ? a.stride : a.size*4),a.size*4);
        else arrayValues(a,i,v.uv[n]);
    }
    return v;
}
template<int uvSize, bool hasColor>
void packFloatVertices(Vertex *out, size_t first, size_t count)
{
    const Array &uv = coords[0];
    const size_t posStride = positions.stride ? positions.stride : 12;
    const size_t colorStride = colors.stride ? colors.stride : 4;
    const size_t uvStride = uv.stride ? uv.stride : uvSize*4;
    const u8 *pos = positions.data+first*posStride;
    const u8 *color = hasColor ? colors.data+first*colorStride : nullptr;
    const u8 *coord = uvSize ? uv.data+first*uvStride : nullptr;
    for(size_t i = 0; i < count; ++i)
    {
        Vertex &v = out[i];
        std::memcpy(v.p,pos+i*posStride,12);
        v.p[3] = current.p[3];
        if constexpr(hasColor) std::memcpy(v.color,color+i*colorStride,4);
        else std::memcpy(v.color,current.color,4);
        if constexpr(uvSize)
        {
            std::memcpy(v.uv[0],coord+i*uvStride,uvSize*4);
            if constexpr(uvSize == 2) v.uv[0][2] = 0;
        }
        else std::memcpy(v.uv[0],current.uv[0],sizeof(v.uv[0]));
        std::memcpy(v.uv[1],current.uv[1],sizeof(v.uv[1]));
    }
}
void packVertices(Vertex *out, size_t first, size_t count)
{
    const Array &uv = coords[0];
    const bool hasColor = colors.on && colors.data;
    const bool hasUV = uv.on && uv.data;
    if(positions.size == 3 && positions.type == GL_FLOAT &&
       !(coords[1].on && coords[1].data) &&
       (!hasColor || (colors.type == GL_UNSIGNED_BYTE && colors.size == 4)))
    {
        // World and model arrays use 2D UVs. The projected water mesh
        // supplies 3D UVs, and untextured shadow meshes use a constant UV.
        if(!hasUV)
            return hasColor ? packFloatVertices<0,true>(out,first,count) : packFloatVertices<0,false>(out,first,count);
        if(uv.type == GL_FLOAT && uv.size == 2)
            return hasColor ? packFloatVertices<2,true>(out,first,count) : packFloatVertices<2,false>(out,first,count);
        if(uv.type == GL_FLOAT && uv.size == 3)
            return hasColor ? packFloatVertices<3,true>(out,first,count) : packFloatVertices<3,false>(out,first,count);
    }
    for(size_t i = 0; i < count; ++i) out[i] = vertexAt(first+i);
}
PackedArray packedArray()
{
    const Array &uv = coords[0];
    if(positions.type != GL_FLOAT || positions.size != 3 || current.p[3] != 1 ||
       !colors.on || !colors.data || colors.type != GL_UNSIGNED_BYTE || colors.size != 4 ||
       !uv.on || !uv.data || uv.type != GL_FLOAT || (coords[1].on && coords[1].data))
        return {};
    const uintptr_t p = reinterpret_cast<uintptr_t>(positions.data);
    const uintptr_t c = reinterpret_cast<uintptr_t>(colors.data);
    const uintptr_t t = reinterpret_cast<uintptr_t>(uv.data);
    if(positions.stride == worldVertexStride && colors.stride == worldVertexStride &&
       uv.stride == worldVertexStride && uv.size == 2 &&
       p == t+8 && c == t+20)
        return {uv.data,worldVertexStride,VertexLayout::World};
    if(positions.stride == projectedVertexStride && colors.stride == projectedVertexStride &&
       uv.stride == projectedVertexStride && uv.size == 3 &&
       c == p+12 && t == p+16)
        return {positions.data,projectedVertexStride,VertexLayout::Projected};
    return {};
}
void uploadVertices(u8 *out, const PackedArray &array, size_t first, size_t count)
{
    if(array.data) std::memcpy(out,array.data+first*array.stride,count*array.stride);
    else packVertices(reinterpret_cast<Vertex *>(out),first,count);
}
void setArray(Array &a,int size,GLenum type,int stride,const void *p)
{
    a.data = static_cast<const u8 *>(p); a.size = size; a.type = type; a.stride = stride;
}
Array &array(GLenum key)
{
    switch(key)
    {
        case GL_COLOR_ARRAY: return colors;
        case GL_TEXTURE_COORD_ARRAY: return coords[client];
        case GL_NORMAL_ARRAY: return normals;
        default: return positions;
    }
}
bool *cap(GLenum key)
{
    switch(key)
    {
        case GL_TEXTURE_2D: return &texUnit[active].on;
        case GL_DEPTH_TEST: return &depthOn;
        case GL_ALPHA_TEST: return &alphaOn;
        case GL_BLEND: return &blendOn;
        case GL_CULL_FACE: return &cullOn;
        case GL_FOG: return &fogOn;
        case GL_STENCIL_TEST: return &stencilOn;
        case GL_SCISSOR_TEST: return &scissorOn;
        case GL_POLYGON_OFFSET_LINE: case GL_POLYGON_OFFSET_FILL: return &offsetOn;
        default: return nullptr;
    }
}
void enable(GLenum key,bool on)
{
    bool *v = cap(key);
    if(!v || *v == on) return;
    if(key == GL_TEXTURE_2D) dirtyTex();
    else
    {
        dirtyRaster();
        if(key == GL_FOG) fogDirty = true;
        if(v == &offsetOn) transformDirty = true;
    }
    *v = on;
}
void setActive(GLenum n) { if(n >= GL_TEXTURE0_ARB && n < GL_TEXTURE0_ARB+units) active = n-GL_TEXTURE0_ARB; }
void setClient(GLenum n) { if(n >= GL_TEXTURE0_ARB && n < GL_TEXTURE0_ARB+units) client = n-GL_TEXTURE0_ARB; }
void multi3(GLenum n,GLfloat x,GLfloat y,GLfloat z)
{
    if(n < GL_TEXTURE0_ARB || n >= GL_TEXTURE0_ARB+units) return;
    float *uv = current.uv[n-GL_TEXTURE0_ARB];
    uv[0] = x; uv[1] = y; uv[2] = z;
}
void multi2(GLenum n,GLfloat x,GLfloat y) { multi3(n,x,y,0); }
unsigned triangleIndices(GLenum mode, unsigned len)
{
    if(mode == GL_QUADS) return len/4*6;
    if(mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) return len >= 3 ? (len-2)*3 : 0;
    return len/3*3;
}
void appendTriangles(u16 *&out, GLenum mode, unsigned base, unsigned len)
{
    if(mode == GL_QUADS)
        for(unsigned j = 0; j+3 < len; j += 4)
        {
            const u16 k = base+j;
            *out++ = k; *out++ = k+1; *out++ = k+2;
            *out++ = k; *out++ = k+2; *out++ = k+3;
        }
    else if(mode == GL_TRIANGLE_STRIP)
        for(unsigned j = 2; j < len; ++j)
        {
            const u16 k = base+j;
            *out++ = k-(j&1 ? 1 : 2);
            *out++ = k-(j&1 ? 2 : 1);
            *out++ = k;
        }
    else if(mode == GL_TRIANGLE_FAN)
        for(unsigned j = 2; j < len; ++j)
        {
            *out++ = base; *out++ = base+j-1; *out++ = base+j;
        }
    else for(unsigned j = 0; j < len/3*3; ++j) *out++ = base+j;
}
struct WorldStripPlan
{
    int end;
    unsigned vertices, indices;
    bool shared;
};
WorldStripPlan planWorldStrips(const Ac3dsWorldStrips &strips, int begin)
{
    unsigned nv = 0, ni = 0;
    unsigned low = strips.first[begin], high = low;
    int end = begin;
    do
    {
        low = std::min(low,unsigned(strips.first[end]));
        high = std::max(high,unsigned(strips.first[end]+strips.count[end]));
        nv += strips.count[end];
        ni += triangleIndices(strips.mode,strips.count[end]);
        ++end;
    }
    while(end < strips.draws && nv+strips.count[end] <= multiBatchVertices);
    // Azahar streams the whole min-to-max range per indexed draw.
    return {end,nv,ni,high-low <= 2*nv};
}
void writeWorldStrips(const Ac3dsWorldStrips &strips, int begin, int end, u8 *packed, u16 *out)
{
    if(!packed)
    {
        for(int i = begin; i < end; ++i) appendTriangles(out,strips.mode,strips.first[i],strips.count[i]);
        return;
    }
    unsigned base = 0;
    for(int i = begin; i < end; ++i)
    {
        std::memcpy(packed+base*worldVertexStride,worldSource+strips.first[i]*worldVertexStride,
                    strips.count[i]*worldVertexStride);
        appendTriangles(out,strips.mode,base,strips.count[i]);
        base += strips.count[i];
    }
}
void drawMulti(GLenum mode, GLint *first, GLsizei *count, GLsizei n)
{
    if(wireframe || compiling || (mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP && mode != GL_QUADS))
    {
        for(int i = 0; i < n; ++i) glDrawArrays(mode,first[i],count[i]);
        return;
    }
    submitPending();
    if(!positions.data || !startFrame(false)) return;
    const PackedArray array = packedArray();
    // rendercubes calls this once per compatible texture/primitive batch.
    // Pack each source vertex once, with a triangle index list preserving
    // the winding and order of separate strips/quads.
    for(int i = 0; i < n;)
    {
        int end = i;
        unsigned nv = 0, ni = 0;
        while(end < n && count[end] >= 0 && nv+unsigned(count[end]) <= multiBatchVertices)
        {
            nv += count[end];
            ni += triangleIndices(mode,count[end]);
            ++end;
        }
        if(end == i) { glDrawArrays(mode,first[i],count[i]); ++i; continue; }
        if(!ni) { i = end; continue; }
        submitPending();
        reserveCommands();
        alignVertices();
        iused = (iused+3)&~size_t(3);
        const size_t vertexBytes = nv*array.stride;
        if(vused+vertexBytes > vertexArenaBytes || iused+ni > indexCapacity) recycleFrame();
        u8 *out = vertices+vused;
        u16 *idx = indices+iused;
        unsigned base = 0;
        for(; i < end; ++i)
        {
            const unsigned len = count[i];
            uploadVertices(out+base*array.stride,array,first[i],len);
            appendTriangles(idx,mode,base,len);
            base += len;
        }
        apply(false);
        useVertexColor();
        useBuffer(out,array.layout);
        C3D_DrawElements(GPU_TRIANGLES,ni,C3D_UNSIGNED_SHORT,indices+iused);
        ++currentCounters.draws; currentCounters.verts += nv; currentCounters.inds += ni;
        vused += vertexBytes; iused += ni;
    }
}
void drawMultiElements(GLenum mode, const GLsizei *count, GLenum type, const GLvoid **idx, GLsizei n)
{
    for(int i = 0; i < n; ++i) glDrawElements(mode,count[i],type,idx[i]);
}
void drawElements(GLenum mode, GLsizei count, GLenum type, const void *idx, bool knownRange, unsigned low, unsigned high);
void rangeElements(GLenum mode, GLuint low, GLuint high, GLsizei n, GLenum type, const void *idx)
{
    drawElements(mode,n,type,idx,true,low,high);
}
unsigned morton(unsigned x,unsigned y)
{
    return (x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2)|((x&4)<<2)|((y&4)<<3);
}
size_t tiled(unsigned x,unsigned y,unsigned w)
{
    return ((y/8)*(w/8)+x/8)*64+morton(x,y);
}
int pixelSize(GLenum format)
{
    return format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
}
GPU_TEXCOLOR nativeFormat(GLenum format,bool render)
{
    if(render) return GPU_RGBA8;
    switch(format)
    {
        case GL_ALPHA: return GPU_A8;
        case GL_LUMINANCE: return GPU_L8;
        case GL_LUMINANCE_ALPHA: return GPU_LA8;
        case GL_RGB: return GPU_RGB8;
        case GL_RGB565: return GPU_RGB565;
        default: return GPU_RGBA8;
    }
}
int gpuPixelSize(GPU_TEXCOLOR f)
{
    switch(f)
    {
        case GPU_RGBA8: return 4; case GPU_RGB8: return 3;
        case GPU_RGB565: case GPU_LA8: return 2;
        default: return 1;
    }
}
u32 decode(const u8 *p,GLenum format)
{
    if(format == GL_RGBA) return u32(p[0])<<24|u32(p[1])<<16|u32(p[2])<<8|p[3];
    if(format == GL_BGRA) return u32(p[2])<<24|u32(p[1])<<16|u32(p[0])<<8|p[3];
    if(format == GL_RGB) return u32(p[0])<<24|u32(p[1])<<16|u32(p[2])<<8|255;
    if(format == GL_ALPHA) return 0xffffff00|p[0];
    return u32(p[0])*0x01010100|u32(format == GL_LUMINANCE_ALPHA ? p[1] : 255);
}
void writeTexel(u8 *p,GPU_TEXCOLOR f,u32 c)
{
    switch(f)
    {
        case GPU_RGBA8: *p++ = u8(c); [[fallthrough]];
        case GPU_RGB8: *p++ = u8(c>>8); *p++ = u8(c>>16); *p = u8(c>>24); break;
        case GPU_RGB565:
        {
            const u32 r = ((c >> 24) * 31 + 127) / 255;
            const u32 g = (((c >> 16) & 255) * 63 + 127) / 255;
            const u32 b = (((c >> 8) & 255) * 31 + 127) / 255;
            const u16 packed = (r << 11) | (g << 5) | b;
            std::memcpy(p,&packed,sizeof(packed));
            break;
        }
        case GPU_LA8: *p++ = u8(c); [[fallthrough]];
        case GPU_L8: *p = u8(c>>24); break;
        case GPU_A8: *p = u8(c); break;
        default: break;
    }
}
u32 readTexel(const u8 *p,GPU_TEXCOLOR f)
{
    switch(f)
    {
        case GPU_RGBA8: return u32(p[3])<<24|u32(p[2])<<16|u32(p[1])<<8|p[0];
        case GPU_RGB8: return u32(p[2])<<24|u32(p[1])<<16|u32(p[0])<<8|255;
        case GPU_RGB565:
        {
            const u16 packed = read<u16>(p);
            const u32 r = (packed >> 11) & 31, g = (packed >> 5) & 63, b = packed & 31;
            return ((r << 3) | (r >> 2)) << 24 | ((g << 2) | (g >> 4)) << 16 |
                   ((b << 3) | (b >> 2)) << 8 | 255;
        }
        case GPU_LA8: return u32(p[1])*0x01010100|p[0];
        case GPU_L8: return u32(p[0])*0x01010100|255;
        case GPU_A8: return 0xffffff00|p[0];
        default: return 0;
    }
}
void outputPixel(u8 *p,GLenum f,u32 c)
{
    if(f == GL_RED) { *p = u8(c>>24); return; }
    p[0] = u8(c>>24); p[1] = u8(c>>16); p[2] = u8(c>>8);
    if(f == GL_RGBA) p[3] = u8(c);
}
bool readTarget(C3D_RenderTarget *source)
{
    const bool wasOpen = frame;
    submitPending();
    if(!source || !pixels || !startFrame(false)) return false;
    reserveCommands();
    // PICA color target and DMA destination are both RGBA8; this untwiddles
    // the GPU framebuffer. Keep this outside the normal gameplay hot path.
    const u32 dim = GX_BUFFER_DIM(source->frameBuf.width,source->frameBuf.height);
    const size_t bytes = source->frameBuf.width*source->frameBuf.height*4;
    C3D_FrameSplit(submitFlags);
    queuedGx += 2;
    const bool copied = R_SUCCEEDED(GSPGPU_FlushDataCache(pixels,bytes)) &&
        R_SUCCEEDED(GX_DisplayTransfer(static_cast<u32 *>(source->frameBuf.colorBuf),dim,pixels,dim,0));
    recycleFrame();
    if(!wasOpen)
    {
        C3D_FrameEnd(fullFlush ? 0 : submitFlags);
        frame = false;
        frameHasVblank = false;
    }
    return copied && R_SUCCEEDED(GSPGPU_InvalidateDataCache(pixels,bytes));
}
} // namespace

bool ac3ds_gl_init()
{
    if(ready) return true;
    if(!C3D_Init(0x180000)) return false;
    top = C3D_RenderTargetCreate(screenH,screenW,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
    aimTile = static_cast<u32 *>(linearMemAlign(256,0x80));
    vertices = static_cast<u8 *>(linearAlloc(vertexArenaBytes));
    indices = static_cast<u16 *>(linearAlloc(sizeof(u16)*indexCapacity));
    fontIndices = static_cast<u16 *>(linearAlloc(sizeof(u16)*6*fontQuadCapacity));
    if(fontIndices)
    {
        u16 *out = fontIndices;
        appendTriangles(out,GL_QUADS,0,4*fontQuadCapacity);
        if(R_FAILED(GSPGPU_FlushDataCache(fontIndices,sizeof(u16)*6*fontQuadCapacity)))
        {
            linearFree(fontIndices);
            fontIndices = nullptr;
        }
    }
    pixels = static_cast<u32 *>(linearAlloc(256*256*4 + screenW*screenH*4));
    shader = DVLB_ParseFile(reinterpret_cast<u32 *>(const_cast<unsigned char *>(cube_vertex_shbin)),cube_vertex_shbin_size);
    if(!top || !vertices || !indices || !pixels || !aimTile || !shader || !C3D_TexInit(&dummyUnit1,8,8,GPU_RGBA8))
    {
        ac3ds_gl_shutdown();
        return false;
    }
    std::memset(dummyUnit1.data,255,dummyUnit1.size);
    if(R_FAILED(GSPGPU_FlushDataCache(dummyUnit1.data,dummyUnit1.size)))
    {
        ac3ds_gl_shutdown();
        return false;
    }
    shaderProgramInit(&program);
    if(R_FAILED(shaderProgramSetVsh(&program,&shader->DVLE[0])))
    {
        ac3ds_gl_shutdown();
        return false;
    }
    C3D_BindProgram(&program);
    locTransform = shaderInstanceGetUniformLocation(program.vertexShader,"transform");
    locTex[0] = shaderInstanceGetUniformLocation(program.vertexShader,"tex0");
    locTex[1] = shaderInstanceGetUniformLocation(program.vertexShader,"tex1");
    locColorScale = shaderInstanceGetUniformLocation(program.vertexShader,"colorScale");
    locColor = shaderInstanceGetUniformLocation(program.vertexShader,"flatColor");
    if(locTransform < 0 || locTex[0] < 0 || locTex[1] < 0 || locColorScale < 0 || locColor < 0)
    {
        ac3ds_gl_shutdown();
        return false;
    }
    flatColorActive = true;
    useVertexColor();
    auto &full = attributes[size_t(VertexLayout::Full)];
    AttrInfo_Init(&full);
    AttrInfo_AddLoader(&full,0,GPU_FLOAT,4);
    AttrInfo_AddLoader(&full,1,GPU_UNSIGNED_BYTE,4);
    AttrInfo_AddLoader(&full,2,GPU_FLOAT,3);
    AttrInfo_AddLoader(&full,3,GPU_FLOAT,3);
    // PICA fills three-component positions with w=1, and two-component UVs
    // with z=0. Each compact array supplies the same shader inputs as Full.
    auto &world = attributes[size_t(VertexLayout::World)];
    AttrInfo_Init(&world);
    AttrInfo_AddLoader(&world,2,GPU_FLOAT,2);
    AttrInfo_AddLoader(&world,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&world,1,GPU_UNSIGNED_BYTE,4);
    AttrInfo_AddFixed(&world,3);
    auto &projected = attributes[size_t(VertexLayout::Projected)];
    AttrInfo_Init(&projected);
    AttrInfo_AddLoader(&projected,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&projected,1,GPU_UNSIGNED_BYTE,4);
    AttrInfo_AddLoader(&projected,2,GPU_FLOAT,3);
    AttrInfo_AddFixed(&projected,3);
    auto &modelAttrs = attributes[size_t(VertexLayout::Model)];
    AttrInfo_Init(&modelAttrs);
    AttrInfo_AddLoader(&modelAttrs,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&modelAttrs,2,GPU_FLOAT,2);
    AttrInfo_AddFixed(&modelAttrs,1);
    AttrInfo_AddFixed(&modelAttrs,3);
    auto &positionAttrs = attributes[size_t(VertexLayout::Position)];
    AttrInfo_Init(&positionAttrs);
    AttrInfo_AddLoader(&positionAttrs,0,GPU_FLOAT,3);
    AttrInfo_AddFixed(&positionAttrs,1);
    AttrInfo_AddFixed(&positionAttrs,2);
    AttrInfo_AddFixed(&positionAttrs,3);
    auto &fontAttrs = attributes[size_t(VertexLayout::Font)];
    AttrInfo_Init(&fontAttrs);
    AttrInfo_AddLoader(&fontAttrs,0,GPU_FLOAT,2);
    AttrInfo_AddLoader(&fontAttrs,2,GPU_FLOAT,2);
    AttrInfo_AddLoader(&fontAttrs,1,GPU_UNSIGNED_BYTE,4);
    AttrInfo_AddFixed(&fontAttrs,3);
    BufInfo_Init(&vertexBuffers);
    vertexBuffers.base_paddr = OS_FCRAM_PADDR;
    if(BufInfo_Add(&vertexBuffers,vertices,sizeof(Vertex),4,0x3210) < 0)
    {
        ac3ds_gl_shutdown();
        return false;
    }
    appliedLayout = VertexLayout::Count;
    appliedBuffer = nullptr;
    vertexBufferValid = false;
    fixedUV1Valid = false;
    fixedModelColorValid = false;
    C3D_RenderTargetSetOutput(top,GFX_TOP,GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0)|GX_TRANSFER_OUT_TILED(0)|GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)|GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    C3D_DepthMap(true,-1.f,0.f);
    C3D_EarlyDepthTest(false,GPU_EARLYDEPTH_GREATER,0);
    C3D_FrameRate(60.f);
    pending.reserve(8192);
    pendingFont.reserve(fontQuadCapacity*4);
    immediate.reserve(1024);
    target = top;
    ready = true;
    return true;
}

void ac3ds_gl_shutdown()
{
    if(frame) ac3ds_gl_end_frame();
    if(top && top->linked) C3D_RenderTargetDetachOutput(top); // drains any active transfer
    for(auto &snapshot : waterWorldSnapshots)
    {
        if(snapshot.storage) vramFree(snapshot.storage);
        snapshot = {};
    }
    for(auto &item : textures) release(*item.second);
    for(auto &item : retiredTextures) release(*item);
    textures.clear(); retiredTextures.clear();
    for(auto &item : lists) if(item.second.gpu) linearFree(item.second.gpu);
    for(auto *p : retiredGeometry) linearFree(p);
    for(auto &cache : worldGeometryCaches)
    {
        if(cache.data) linearFree(cache.data);
        cache = {};
    }
    activeWorldGeometry = nullptr;
    lists.clear(); retiredGeometry.clear();
    if(program.vertexShader) shaderProgramFree(&program);
    if(shader) DVLB_Free(shader);
    if(vertices) linearFree(vertices);
    if(indices) linearFree(indices);
    if(fontIndices) linearFree(fontIndices);
    if(pixels) linearFree(pixels);
    if(aimTile) linearFree(aimTile);
    if(dummyUnit1.data) C3D_TexDelete(&dummyUnit1);
    C3D_Fini();
    top = target = nullptr; shader = nullptr; vertices = nullptr; indices = nullptr; pixels = nullptr;
    fontIndices = nullptr;
    program = {}; dummyUnit1 = {}; aimTile = nullptr; aimQueued = false; shadowTargets.fill(nullptr);
    texUnit[0] = Unit{}; texUnit[1] = Unit{}; active = client = 0;
    model = MatrixStack{}; projection = MatrixStack{}; matrixMode = GL_MODELVIEW;
    depthOn = blendOn = stencilOn = cullOn = alphaOn = fogOn = scissorOn = false;
    writeDepth = true; colorMask = 15; wireframe = offsetOn = false;
    boundFogTable = nullptr;
    tevInitialized = false;
    viewportValid = false;
    rasterStateValid = false;
    clipWasOn = false;
    gameplayFrame = lateVblankFrame = frameHasVblank = lastGameplayLate = false;
    profile.haveBaseline = false;
    ready = frame = false;
}
void ac3ds_gl_set_late_vblank(bool enabled)
{
    lateVblankRequested = enabled;
}
void ac3ds_gl_set_fast_vertex_offsets(bool enabled)
{
    if(fastVertexOffsets == enabled) return;
    submitPending();
    fastVertexOffsets = enabled;
    vertexBufferValid = false;
}
void ac3ds_gl_set_joined_water_commands(bool enabled)
{
    joinedWaterCommands = enabled;
}
void ac3ds_gl_begin_frame()
{
    if(!ready) return;
    target = top;
    // An earlier aim read can open the queue, but the pacing mode belongs
    // to this explicit gameplay draw and stays fixed through submission.
    lateVblankFrame = lateVblankRequested && !frameHasVblank;
    gameplayFrame = startFrame(!lateVblankFrame);
    if(gameplayFrame && !lateVblankFrame && !frameHasVblank)
    {
        lastTopVblank = waitTopVblank();
        frameHasVblank = true;
    }
}

void ac3ds_gl_startup_clear()
{
    // Establish the first top-screen transfer independently of shader draws.
    // Drain this frame before the splash writes to the same target.
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ac3ds_gl_end_frame();
}

void ac3ds_gl_startup_draw()
{
    // Exercise the normal vertex input and shader before binding a texture.
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glColor4ub(0, 0, 0, 255);
    Vertex triangle[3];
    triangle[0].p[0] = triangle[0].p[1] = -.5f;
    triangle[1].p[0] = .5f;
    triangle[1].p[1] = -.5f;
    triangle[2].p[1] = .5f;
    for(Vertex &v : triangle)
    {
        v.p[2] = -.5f;
        v.color[0] = v.color[1] = v.color[2] = 0;
    }
    enqueue(triangle, 3, GL_TRIANGLES, true);
    ac3ds_gl_end_startup_frame();
    glEnable(GL_FOG);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glColor4ub(255, 255, 255, 255);
}

void ac3ds_gl_end_startup_frame()
{
    // Use Citro3D's full linear cache flush for the initial texture draw.
    if(!startFrame(false)) return;
    fullFlush = true;
    ac3ds_gl_end_frame();
}

bool ac3ds_gl_drain_startup_frame(uint32_t &gpuStatus, int32_t &readResult)
{
    const u64 start = svcGetSystemTick();
    while(!C3D_FrameBegin(C3D_FRAME_NONBLOCK))
    {
        if(svcGetSystemTick() - start >= 5ULL * SYSCLOCK_ARM11)
        {
            readResult = GSPGPU_ReadHWRegs(0x400034,&gpuStatus,sizeof(gpuStatus));
            return false;
        }
        svcSleepThread(250000);
    }
    // This empty frame acquires the queue without scheduling another transfer.
    C3D_FrameEnd(submitFlags);
    return true;
}

void ac3ds_gl_end_frame()
{
    if(!frame) return;
    submitPending();
    flushData();
    top->used = true;
    if(gameplayFrame && lateVblankFrame) lastTopVblank = waitTopVblank();
    if(gameplayFrame && lateVblankFrame != lastGameplayLate)
    {
        // A profile can be armed before a toggle. Its next frame becomes a
        // fresh baseline instead of mixing start and submission slots.
        profile.haveBaseline = false;
        lastGameplayLate = lateVblankFrame;
    }
    if(gameplayFrame && profile.requested && !profile.report)
    {
        const u64 now = svcGetSystemTick();
        // Record the actual submission slot in late mode, and the original
        // frame-start slot in the normal scheduling mode.
        const u32 vblank = lastTopVblank;
        if(profile.haveBaseline)
        {
            const u64 elapsed = now - profile.lastTick;
            const u32 slots = vblank - profile.lastVblank;
            ac3dsRecordFrame(elapsed, slots);
            profile.intervals[profile.count++] = elapsed;
            profile.ticks += elapsed;
            profile.vblanks += slots;
            if(slots > 1) profile.missed += slots - 1;
            if(elapsed > SYSCLOCK_ARM11/50) ++profile.over20ms;
            profile.report = profile.count == profile.requested;
        }
        profile.lastTick = now;
        profile.lastVblank = vblank;
        profile.haveBaseline = true;
    }
    gameplayFrame = false;
    C3D_FrameEnd(fullFlush ? 0 : submitFlags);
    lastCounters = currentCounters;
    currentCounters = {};
    frame = false;
    frameHasVblank = false;
    target = top;
    // Deleting a render target drains the preceding queue. Only do it on
    // actual resource disposal, never at the normal frame boundary.
    for(auto i = retiredTextures.begin(); i != retiredTextures.end();)
    {
        if(!(*i)->target) { ++i; continue; }
        release(**i);
        i = retiredTextures.erase(i);
    }
}
void ac3ds_gl_collect_retired()
{
    activeWorldGeometry = nullptr;
    for(int i = 0; i < int(worldGeometryCaches.size()); ++i) ac3ds_gl_discard_world_cache(i);
    if(!ready || (retiredTextures.empty() && retiredGeometry.empty())) return;
    if(frame) ac3ds_gl_end_frame();
    if(C3D_FrameBegin(0))
    {
        retire();
        C3D_FrameEnd(submitFlags);
    }
}
bool ac3ds_gl_texture_ready(GLuint id)
{
    const Tex *t = findTex(id);
    return t && t->gpu.data;
}
bool ac3ds_gl_can_draw_projected_strip()
{
    return !compiling && !wireframe && current.p[3] == 1 && !(coords[1].on && coords[1].data);
}
void ac3ds_gl_draw_projected_strip(const void *source, GLsizei count)
{
    if(count < 3 || count > UINT16_MAX) return;
    submitPending();
    if(!startFrame(false)) return;
    reserveCommands();
    alignVertices();
    iused = (iused+3)&~size_t(3);
    const size_t bytes = count*projectedVertexStride;
    const unsigned countIndices = triangleIndices(GL_TRIANGLE_STRIP,count);
    if(vused+bytes > vertexArenaBytes || iused+countIndices > indexCapacity) recycleFrame();
    u8 *out = vertices+vused;
    u16 *index = indices+iused;
    appendTriangles(index,GL_TRIANGLE_STRIP,0,count);
    std::memcpy(out,source,bytes);
    apply(false);
    useVertexColor();
    useBuffer(out,VertexLayout::Projected);
    C3D_DrawElements(GPU_TRIANGLES,countIndices,C3D_UNSIGNED_SHORT,indices+iused);
    ++currentCounters.draws;
    currentCounters.verts += count;
    currentCounters.inds += countIndices;
    iused += countIndices;
    vused += bytes;
}
bool ac3ds_gl_can_draw_font_quads()
{
    return fontIndices && !compiling && !wireframe && !inBegin;
}
void ac3ds_gl_font_quad(float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1)
{
    if(!pending.empty() || (!pendingFont.empty() && std::memcmp(pendingFontUV1,current.uv[1],sizeof(pendingFontUV1))))
        submitPending();
    if(pendingFont.empty()) std::memcpy(pendingFontUV1,current.uv[1],sizeof(pendingFontUV1));
    u32 color;
    std::memcpy(&color,current.color,sizeof(color));
    pendingFont.push_back({x0,y0,u0,v0,color});
    pendingFont.push_back({x1,y0,u1,v0,color});
    pendingFont.push_back({x1,y1,u1,v1,color});
    pendingFont.push_back({x0,y1,u0,v1,color});
    multi2(GL_TEXTURE0_ARB,u0,v1);
    if(pendingFont.size() >= fontQuadCapacity*4) submitPending();
}
bool ac3ds_gl_begin_world_geometry(const void *source, GLsizei count)
{
    worldUpload = nullptr;
    activeWorldGeometry = nullptr;
    captureFrameWorld = replayFrameWorld = false;
    if(wireframe || compiling || client != 0 || current.p[3] != 1 ||
       (coords[1].on && coords[1].data) || count > UINT16_MAX) return false;
    worldSource = static_cast<const u8 *>(source);
    worldBytes = count*worldVertexStride;
    return true;
}
void ac3ds_gl_capture_frame_world()
{
    frameWorldDraws.clear();
    frameWorldEnds.clear();
    frameWorldGeneration = uploadGeneration;
    captureFrameWorld = !activeWorldGeometry;
}
void ac3ds_gl_replay_frame_world()
{
    frameWorldBatch = frameWorldDraw = 0;
    replayFrameWorld = !activeWorldGeometry && frameWorldGeneration == uploadGeneration && !frameWorldEnds.empty();
}
void ac3ds_gl_discard_world_cache(int slot)
{
    WorldGeometryCache &cache = worldGeometryCaches[slot];
    if(cache.data) retiredGeometry.push_back(cache.data);
    cache.data = nullptr;
    cache.draws.clear();
    cache.ends.clear();
    cache.attempted = false;
}
bool ac3ds_gl_begin_cached_world(int slot)
{
    const WorldGeometryCache &cache = worldGeometryCaches[slot];
    if(cache.data)
    {
        activeWorldGeometry = &cache;
        worldCachedBatch = worldCachedDraw = 0;
    }
    return cache.attempted;
}
void ac3ds_gl_build_world_cache(int slot, const Ac3dsWorldStrips *batches, int count)
{
    WorldGeometryCache &cache = worldGeometryCaches[slot];
    cache.attempted = true;
    size_t vertexBytes = 0, indexBytes = 0;
    bool shared = false;
    for(int b = 0; b < count; ++b)
    {
        const Ac3dsWorldStrips &batch = batches[b];
        for(int i = 0; i < batch.draws;)
        {
            const WorldStripPlan plan = planWorldStrips(batch,i);
            i = plan.end;
            if(!plan.indices) continue;
            if(plan.shared && !shared)
            {
                // Reserve the shared span up front so dense draws reuse it.
                for(auto &draw : cache.draws) draw.vertexOffset += worldBytes;
                vertexBytes += worldBytes;
                shared = true;
            }
            indexBytes = (indexBytes+7)&~size_t(7);
            cache.draws.push_back({plan.shared ? 0 : vertexBytes,indexBytes,plan.vertices,plan.indices});
            if(!plan.shared) vertexBytes += plan.vertices*worldVertexStride;
            indexBytes += plan.indices*sizeof(u16);
        }
        cache.ends.push_back(cache.draws.size());
    }
    constexpr size_t maxWorldCacheBytes = 1024*1024;
    const size_t bytes = vertexBytes+indexBytes;
    if(!bytes || bytes > maxWorldCacheBytes) return;
    u8 *data = static_cast<u8 *>(linearAlloc(bytes));
    if(!data) return;
    if(shared) std::memcpy(data,worldSource,worldBytes);
    size_t drawIndex = 0;
    for(int b = 0; b < count; ++b)
    {
        const Ac3dsWorldStrips &batch = batches[b];
        for(int i = 0; i < batch.draws;)
        {
            const WorldStripPlan plan = planWorldStrips(batch,i);
            if(plan.indices)
            {
                const WorldCachedDraw &draw = cache.draws[drawIndex++];
                writeWorldStrips(batch,i,plan.end,plan.shared ? nullptr : data+draw.vertexOffset,
                                 reinterpret_cast<u16 *>(data+vertexBytes+draw.indexOffset));
            }
            i = plan.end;
        }
    }
    if(R_FAILED(GSPGPU_FlushDataCache(data,bytes))) { linearFree(data); return; }
    for(auto &draw : cache.draws) draw.indexOffset += vertexBytes;
    cache.data = data;
    ac3ds_gl_begin_cached_world(slot);
}
static void drawWorldElements(const u8 *vertexData, const u16 *indexData, unsigned count)
{
    apply(false);
    useVertexColor();
    useBuffer(vertexData,VertexLayout::World);
    C3D_DrawElements(GPU_TRIANGLES,count,C3D_UNSIGNED_SHORT,indexData);
    ++currentCounters.draws;
    currentCounters.inds += count;
}
void ac3ds_gl_draw_world_strips(GLenum mode, const GLint *first, const GLsizei *count, GLsizei draws)
{
    submitPending();
    if(!startFrame(false)) return;
    if(activeWorldGeometry)
    {
        const WorldGeometryCache &cache = *activeWorldGeometry;
        const size_t end = cache.ends[worldCachedBatch++];
        for(; worldCachedDraw < end; ++worldCachedDraw)
        {
            const WorldCachedDraw &draw = cache.draws[worldCachedDraw];
            reserveCommands();
            drawWorldElements(cache.data+draw.vertexOffset,
                              reinterpret_cast<const u16 *>(cache.data+draw.indexOffset),draw.indices);
            currentCounters.verts += draw.vertices;
        }
        return;
    }
    const Ac3dsWorldStrips batch{mode,first,count,draws};
    int i = 0;
    if(replayFrameWorld)
    {
        const size_t end = frameWorldEnds[frameWorldBatch++];
        while(frameWorldDraw < end)
        {
            reserveCommands();
            if(frameWorldGeneration != uploadGeneration)
            {
                replayFrameWorld = false;
                break;
            }
            const WorldCachedDraw &draw = frameWorldDraws[frameWorldDraw++];
            drawWorldElements(vertices+draw.vertexOffset,indices+draw.indexOffset,draw.indices);
            i = draw.vertices;
        }
        if(replayFrameWorld) return;
    }
    while(i < draws)
    {
        const auto [end,nv,ni,shared] = planWorldStrips(batch,i);
        if(!ni) { i = end; continue; }
        reserveCommands();
        alignVertices();
        iused = (iused+3)&~size_t(3);
        const size_t needed = shared ? (worldUpload ? 0 : worldBytes) : nv*worldVertexStride;
        if(vused+needed > vertexArenaBytes || iused+ni > indexCapacity)
            recycleFrame();
        u8 *drawVertices = worldUpload;
        if(shared && !worldUpload)
        {
            // All texture and sky batches address the same interleaved world
            // vertices. Reupload only if the GPU arena had to recycle.
            drawVertices = worldUpload = vertices+vused;
            std::memcpy(worldUpload,worldSource,worldBytes);
            vused += worldBytes;
            currentCounters.verts += worldBytes/worldVertexStride;
        }
        if(!shared)
        {
            drawVertices = vertices+vused;
            vused += needed;
            currentCounters.verts += nv;
        }
        writeWorldStrips(batch,i,end,shared ? nullptr : drawVertices,indices+iused);
        drawWorldElements(drawVertices,indices+iused,ni);
        if(captureFrameWorld && frameWorldGeneration == uploadGeneration)
            frameWorldDraws.push_back({size_t(drawVertices-vertices),iused,unsigned(end),ni});
        iused += ni;
        i = end;
    }
    if(captureFrameWorld && frameWorldGeneration == uploadGeneration) frameWorldEnds.push_back(frameWorldDraws.size());
}
void *ac3ds_gl_proc(const char *s)
{
    if(!std::strcmp(s,"glActiveTextureARB")) return reinterpret_cast<void *>(setActive);
    if(!std::strcmp(s,"glClientActiveTextureARB")) return reinterpret_cast<void *>(setClient);
    if(!std::strcmp(s,"glMultiTexCoord2fARB")) return reinterpret_cast<void *>(multi2);
    if(!std::strcmp(s,"glMultiTexCoord3fARB")) return reinterpret_cast<void *>(multi3);
    if(!std::strcmp(s,"glMultiDrawArraysEXT")) return reinterpret_cast<void *>(drawMulti);
    if(!std::strcmp(s,"glMultiDrawElementsEXT")) return reinterpret_cast<void *>(drawMultiElements);
    if(!std::strcmp(s,"glDrawRangeElementsEXT")) return reinterpret_cast<void *>(rangeElements);
    return nullptr;
}
bool ac3ds_gl_render_to_texture(GLuint id)
{
    Tex *t = findTex(id);
    if(!t || !t->target) return false;
    selectTarget(t->target);
    return true;
}
namespace
{
bool copyWaterWorld(WaterWorldSnapshot &snapshot, bool restore)
{
    if(!ready || !frame || !target || target == top) return false;
    const auto &fb = target->frameBuf;
    if(!fb.colorBuf || !fb.depthBuf || fb.colorFmt != GPU_RB_RGBA8 ||
       fb.depthFmt != GPU_RB_DEPTH24_STENCIL8 || fb.width != fb.height || fb.width > 128) return false;
    const size_t colorBytes = C3D_CalcColorBufSize(fb.width, fb.height, fb.colorFmt);
    const size_t depthBytes = C3D_CalcDepthBufSize(fb.width, fb.height, fb.depthFmt);
    if(!colorBytes || !depthBytes || ((colorBytes | depthBytes) & 127)) return false;
    if(restore && (snapshot.colorBytes != colorBytes || snapshot.depthBytes != depthBytes)) return false;
    if(!snapshot.storage)
    {
        // Reserve enough for either supported size so changing reflectsize
        // does not free a buffer that an earlier queued copy might still use.
        snapshot.storage = static_cast<u8 *>(vramAlloc(128 * 128 * 8));
        if(!snapshot.storage) return false;
    }

    submitPending();
    reserveCommands();
    // Submit preceding draws before the two raw, contiguous VRAM copies.
    // The following draws are queued after both transfers.
    C3D_FrameSplit(submitFlags);
    u32 *snapshotColor = reinterpret_cast<u32 *>(snapshot.storage);
    u32 *snapshotDepth = reinterpret_cast<u32 *>(snapshot.storage + colorBytes);
    u32 *liveColor = static_cast<u32 *>(fb.colorBuf);
    u32 *liveDepth = static_cast<u32 *>(fb.depthBuf);
    const u32 raw = GX_TRANSFER_RAW_COPY(1);
    const Result color = GX_TextureCopy(restore ? snapshotColor : liveColor, 0,
                                       restore ? liveColor : snapshotColor, 0, colorBytes, raw);
    const Result depth = GX_TextureCopy(restore ? snapshotDepth : liveDepth, 0,
                                       restore ? liveDepth : snapshotDepth, 0, depthBytes, raw);
    queuedGx += 3;
    if(R_FAILED(color) || R_FAILED(depth)) return false;
    // Rebind before a draw can read the restored color or depth. Citro3D
    // emits a framebuffer invalidate as part of the deferred bind.
    if(restore) C3D_SetFrameBuf(&target->frameBuf);
    snapshot.colorBytes = colorBytes;
    snapshot.depthBytes = depthBytes;
    return true;
}
}
bool ac3ds_gl_restore_water_world(bool refract, unsigned generation)
{
    auto &snapshot = waterWorldSnapshots[refract];
    return snapshot.valid && snapshot.generation == generation && copyWaterWorld(snapshot,true);
}
bool ac3ds_gl_save_water_world(bool refract, unsigned generation)
{
    auto &snapshot = waterWorldSnapshots[refract];
    snapshot.valid = copyWaterWorld(snapshot,false);
    snapshot.generation = generation;
    return snapshot.valid;
}
void ac3ds_gl_render_to_screen()
{
    if(!ready) return;
    if(frame && target != top && !joinedWaterCommands)
    {
        submitPending();
        reserveCommands();
        C3D_FrameSplit(submitFlags);
        ++queuedGx;
    }
    selectTarget(top);
}
bool ac3ds_gl_begin_shadow_target(int size)
{
    if(size < 8 || size > 128 || (size&(size-1))) return false;
    int level = __builtin_ctz(unsigned(size));
    C3D_RenderTarget *&t = shadowTargets[level];
    if(!t) t = C3D_RenderTargetCreate(size,size,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
    if(!t) return false;
    selectTarget(t);
    return true;
}
const GLubyte *glGetString(GLenum key)
{
    const char *s = "";
    switch(key)
    {
        case GL_VENDOR: s = "Nintendo / DMP"; break;
        case GL_RENDERER: s = "PICA200 via Citro3D"; break;
        case GL_VERSION: s = "Cube fixed function on 3DS"; break;
        case GL_EXTENSIONS: s = "GL_ARB_texture_env_combine GL_ARB_multitexture GL_EXT_multi_draw_arrays GL_EXT_draw_range_elements GL_EXT_stencil_wrap"; break;
    }
    return reinterpret_cast<const GLubyte *>(s);
}
void glGetIntegerv(GLenum key,GLint *out)
{
    *out = key == GL_MAX_TEXTURE_SIZE ? 1024 : key == GL_MAX_TEXTURE_UNITS_ARB ? units : 0;
}
void glGetFloatv(GLenum key,GLfloat *out)
{
    switch(key)
    {
        case GL_MODELVIEW_MATRIX: std::memcpy(out,model.current.a,sizeof(model.current.a)); break;
        case GL_PROJECTION_MATRIX: std::memcpy(out,projection.current.a,sizeof(projection.current.a)); break;
        case GL_LINE_WIDTH: *out = width; break;
        case GL_CURRENT_COLOR: for(int i = 0; i < 4; ++i) out[i] = current.color[i]/255.f; break;
        default: *out = 0; break;
    }
}
void glEnable(GLenum key) { enable(key,true); }
void glDisable(GLenum key) { enable(key,false); }
void glEnableClientState(GLenum key) { array(key).on = true; }
void glDisableClientState(GLenum key) { array(key).on = false; }
void glVertexPointer(GLint n,GLenum type,GLsizei stride,const void *p) { setArray(positions,n,type,stride,p); }
void glColorPointer(GLint n,GLenum type,GLsizei stride,const void *p) { setArray(colors,n,type,stride,p); }
void glTexCoordPointer(GLint n,GLenum type,GLsizei stride,const void *p) { setArray(coords[client],n,type,stride,p); }
void glNormalPointer(GLenum type,GLsizei stride,const void *p) { setArray(normals,3,type,stride,p); }
void glCullFace(GLenum key) { if(cullFace != key) { dirtyRaster(); cullFace = key; } }
void glDepthFunc(GLenum key) { if(depthFunc != key) { dirtyRaster(); depthFunc = key; } }
void glDepthMask(GLboolean on) { if(writeDepth != bool(on)) { dirtyRaster(); writeDepth = on; } }
void glAlphaFunc(GLenum f,GLfloat ref)
{
    if(f != alphaFunc || ref != alphaRef) { dirtyRaster(); alphaFunc = f; alphaRef = ref; }
}
void glBlendFunc(GLenum s,GLenum d)
{
    if(s != blendSrc || d != blendDst) { dirtyRaster(); blendSrc = s; blendDst = d; }
}
void glStencilFunc(GLenum f,GLint ref,GLuint mask)
{
    if(f != stencilFunc || unsigned(ref) != stencilRef || (mask&255) != stencilReadMask)
    {
        dirtyRaster(); stencilFunc = f; stencilRef = ref&255; stencilReadMask = mask&255;
    }
}
void glStencilOp(GLenum s,GLenum d,GLenum p)
{
    if(s != stencilFail || d != depthFail || p != stencilPass)
    {
        dirtyRaster(); stencilFail = s; depthFail = d; stencilPass = p;
    }
}
void glColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a)
{
    unsigned mask = bool(r)|(bool(g)<<1)|(bool(b)<<2)|(bool(a)<<3);
    if(mask != colorMask) { dirtyRaster(); colorMask = mask; }
}
void glColor4ub(GLubyte r,GLubyte g,GLubyte b,GLubyte a) { current.color[0] = r; current.color[1] = g; current.color[2] = b; current.color[3] = a; }
void glColor4ubv(const GLubyte *c) { glColor4ub(c[0],c[1],c[2],c[3]); }
void glColor3ub(GLubyte r,GLubyte g,GLubyte b) { glColor4ub(r,g,b,255); }
void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a) { glColor4ub(chan(r),chan(g),chan(b),chan(a)); }
void glColor3f(GLfloat r,GLfloat g,GLfloat b) { glColor4f(r,g,b,1); }
void glColor3d(GLdouble r,GLdouble g,GLdouble b) { glColor3f(r,g,b); }
void glLineWidth(GLfloat w) { width = std::max(w,1.f); }
void glPolygonMode(GLenum,GLenum mode) { wireframe = mode == GL_LINE; }
void glPolygonOffset(GLfloat factor,GLfloat units_)
{
    const float next = (factor+units_)*.00003f;
    if(next == polyOffset) return;
    if(offsetOn) dirtyTransform();
    polyOffset = next;
}
void glShadeModel(GLenum) {} // The engine only selects smooth shading.
void glHint(GLenum,GLenum) {}
void glFogf(GLenum key,GLfloat f)
{
    if(key == GL_FOG_START && fogNear != f) { dirtyFog(); fogNear = f; }
    else if(key == GL_FOG_END && fogFar != f) { dirtyFog(); fogFar = f; }
    else if(key == GL_FOG_DENSITY) fogDensity = f; // Cube selects linear fog.
}
void glFogi(GLenum key,GLint i) { glFogf(key,float(i)); }
void glFogfv(GLenum key,const GLfloat *f)
{
    if(key != GL_FOG_COLOR) return;
    const u32 c = tevColor(rgba(f[0],f[1],f[2],f[3]))&0xffffff;
    if(fogColor != c) { dirtyRaster(); fogColor = c; }
}
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h)
{
    if(viewport.x == x && viewport.y == y && viewport.w == w && viewport.h == h) return;
    dirtyRaster(); viewport = {x,y,w,h};
}
void glScissor(GLint x,GLint y,GLsizei w,GLsizei h)
{
    if(scissor == Rect{x,y,w,h}) return;
    dirtyRaster(); scissor = {x,y,w,h};
}
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a) { clearColor = rgba(r,g,b,a); }
void glClearDepth(GLdouble v) { clearDepth = sat(v); }
void glClearStencil(GLint v) { clearStencil = v&255; }
void glClear(GLbitfield mask)
{
    submitPending();
    if(!startFrame(false) || !mask) return;
    const unsigned bits = (mask&GL_COLOR_BUFFER_BIT ? C3D_CLEAR_COLOR : 0) |
        (mask&(GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT) ? C3D_CLEAR_DEPTH : 0);
    if(!bits) return;
    reserveCommands();
    C3D_FrameSplit(submitFlags);
    // PICA clears complete attachments. Cube uses scissor only on freshly
    // rendered water targets; those passes must clear outside it as well.
    const u32 z = u32((1.f-clearDepth)*16777215.f);
    C3D_RenderTargetClear(target,C3D_ClearBits(bits),clearColor,z | clearStencil<<24);
    queuedGx += 2;
}
void glMatrixMode(GLenum key) { matrixMode = key; }
void glPushMatrix()
{
    auto &s = stack();
    s.saved.push_back(s.current);
}
void glPopMatrix()
{
    auto &s = stack();
    if(s.saved.empty()) return;
    glLoadMatrixf(s.saved.back().a);
    s.saved.pop_back();
}
void glLoadMatrixf(const GLfloat *m)
{
    auto &s = stack();
    if(std::memcmp(s.current.a,m,sizeof(s.current.a)) == 0) return;
    submitPending();
    if(matrixMode == GL_TEXTURE)
    {
        uvDirty |= 1u << active;
        if(active == 0)
        {
            const bool nextProjected = m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1;
            if(projected(0) != nextProjected) samplerDirty |= 1;
        }
    }
    else
    {
        transformDirty = true;
        if(matrixMode == GL_PROJECTION) fogDirty = true;
    }
    std::memcpy(s.current.a,m,sizeof(s.current.a));
}
void glMultMatrixf(const GLfloat *m)
{
    Mat rhs;
    std::memcpy(rhs.a,m,sizeof(rhs.a));
    Mat out = multiply(stack().current,rhs);
    glLoadMatrixf(out.a);
}
void glLoadIdentity() { glLoadMatrixf(Mat::identity().a); }
void glTranslatef(GLfloat x,GLfloat y,GLfloat z)
{
    if(x == 0 && y == 0 && z == 0) return;
    Mat out = stack().current;
    for(int row = 0; row < 4; ++row)
        out.at(row,3) = out.at(row,0)*x+out.at(row,1)*y+out.at(row,2)*z+out.at(row,3);
    glLoadMatrixf(out.a);
}
void glScalef(GLfloat x,GLfloat y,GLfloat z)
{
    if(x == 1 && y == 1 && z == 1) return;
    Mat out = stack().current;
    for(int row = 0; row < 4; ++row)
    {
        out.at(row,0) *= x;
        out.at(row,1) *= y;
        out.at(row,2) *= z;
    }
    glLoadMatrixf(out.a);
}
void glRotatef(GLfloat angle,GLfloat x,GLfloat y,GLfloat z)
{
    const float len = std::sqrt(x*x+y*y+z*z);
    if(len == 0 || angle == 0) return;
    x /= len; y /= len; z /= len;
    const float rad = angle*.017453292519943295f, c = std::cos(rad), s = std::sin(rad), t = 1-c;
    Mat m = Mat::identity();
    m.at(0,0) = x*x*t+c;   m.at(0,1) = x*y*t-z*s; m.at(0,2) = x*z*t+y*s;
    m.at(1,0) = y*x*t+z*s; m.at(1,1) = y*y*t+c;   m.at(1,2) = y*z*t-x*s;
    m.at(2,0) = z*x*t-y*s; m.at(2,1) = z*y*t+x*s; m.at(2,2) = z*z*t+c;
    glMultMatrixf(m.a);
}
void glOrtho(GLdouble l,GLdouble r,GLdouble b,GLdouble t,GLdouble n,GLdouble f)
{
    if(l == r || b == t || n == f) return;
    Mat m = Mat::identity();
    m.a[0] = 2/(r-l); m.a[5] = 2/(t-b); m.a[10] = -2/(f-n);
    m.a[12] = -(r+l)/(r-l); m.a[13] = -(t+b)/(t-b); m.a[14] = -(f+n)/(f-n);
    glMultMatrixf(m.a);
}
void glFrustum(GLdouble l,GLdouble r,GLdouble b,GLdouble t,GLdouble n,GLdouble f)
{
    if(l == r || b == t || n <= 0 || f <= n) return;
    Mat m;
    m.a[0] = 2*n/(r-l); m.a[5] = 2*n/(t-b);
    m.a[8] = (r+l)/(r-l); m.a[9] = (t+b)/(t-b);
    m.a[10] = -(f+n)/(f-n); m.a[11] = -1; m.a[14] = -2*n*f/(f-n);
    glMultMatrixf(m.a);
}
void glBegin(GLenum mode) { if(!inBegin) { inBegin = true; primitive = mode; immediate.clear(); } }
void glEnd() { if(inBegin) { inBegin = false; emit(primitive,immediate.data(),immediate.size()); } }
void glVertex3f(GLfloat x,GLfloat y,GLfloat z)
{
    if(!inBegin) return;
    immediate.push_back(current);
    auto &v = immediate.back();
    v.p[0] = x; v.p[1] = y; v.p[2] = z; v.p[3] = 1;
}
void glVertex3fv(const GLfloat *v) { glVertex3f(v[0],v[1],v[2]); }
void glVertex2f(GLfloat x,GLfloat y) { glVertex3f(x,y,0); }
void glVertex2d(GLdouble x,GLdouble y) { glVertex2f(x,y); }
void glTexCoord2f(GLfloat x,GLfloat y) { multi2(GL_TEXTURE0_ARB,x,y); }
void glTexCoord2i(GLint x,GLint y) { glTexCoord2f(x,y); }
void glDrawArrays(GLenum mode,GLint first,GLsizei count)
{
    if(count <= 0 || first < 0 || !positions.data) return;
    if(!compiling && !wireframe && (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) &&
       count >= 3 && count <= UINT16_MAX)
    {
        // Each strip/fan was already submitted separately. Write straight
        // to the GPU arena instead of copying through two CPU vectors.
        submitPending();
        if(!startFrame(false)) return;
        reserveCommands();
        const PackedArray array = packedArray();
        const size_t bytes = count*array.stride;
        alignVertices();
        iused = (iused+3)&~size_t(3);
        const unsigned countIndices = triangleIndices(mode,count);
        if(vused+bytes > vertexArenaBytes || iused+countIndices > indexCapacity) recycleFrame();
        u8 *out = vertices+vused;
        u16 *index = indices+iused;
        appendTriangles(index,mode,0,count);
        uploadVertices(out,array,first,count);
        apply(false);
        useVertexColor();
        useBuffer(out,array.layout);
        C3D_DrawElements(GPU_TRIANGLES,countIndices,C3D_UNSIGNED_SHORT,indices+iused);
        ++currentCounters.draws;
        currentCounters.verts += count;
        currentCounters.inds += countIndices;
        iused += countIndices;
        vused += bytes;
        return;
    }
    scratch.resize(count);
    packVertices(scratch.data(),first,count);
    emit(mode,scratch.data(),scratch.size());
}
void glDrawElements(GLenum mode,GLsizei count,GLenum type,const GLvoid *idx)
{
    drawElements(mode,count,type,idx,false,0,0);
}
bool ac3ds_gl_draw_model(unsigned mesh, unsigned pose, unsigned lighting,
                         GLuint low, GLuint high, GLsizei count, const GLushort *source)
{
    const Array &uv = coords[0];
    const bool vertexLight = colors.on && colors.data;
    if(compiling || wireframe || current.p[3] != 1 || count <= 0 || !source ||
       size_t(count) > indexCapacity || high >= UINT16_MAX || low > high ||
       !positions.data || positions.type != GL_FLOAT || positions.size != 3 || positions.stride != 12 ||
       !uv.on || !uv.data || uv.type != GL_FLOAT || uv.size != 2 || uv.stride != 8 ||
       (coords[1].on && coords[1].data) ||
       (vertexLight && (!lighting || colors.type != GL_UNSIGNED_BYTE || colors.size != 4 || colors.stride != 4)))
        return false;

    submitPending();
    if(!startFrame(false)) return false;
    reserveCommands();
    // Frame arenas are reset on recycle. A cache entry refers only to bytes
    // produced for the same mesh, animation pose and per-vertex lighting.
    const unsigned mask = modelVertexUploads.size()-1;
    const unsigned range = (reinterpret_cast<uintptr_t>(source)>>1) & mask;
    ModelVertexUpload &vertexUpload = modelVertexUploads[(mesh*131u+pose*257u+lighting*17u+range)&mask];
    ModelIndexUpload &indexUpload = modelIndexUploads[(mesh*131u+range)&mask];
    auto hasVertices = [&]
    {
        return vertexUpload.generation == uploadGeneration && vertexUpload.mesh == mesh &&
               vertexUpload.pose == pose && vertexUpload.lighting == lighting && vertexUpload.source == source;
    };
    auto hasIndices = [&]
    {
        return indexUpload.generation == uploadGeneration && indexUpload.mesh == mesh && indexUpload.source == source;
    };
    const size_t unique = high-low+1;
    const size_t stride = vertexLight ? worldVertexStride : modelVertexStride;
    const size_t bytes = unique*stride;
    alignVertices();
    iused = (iused+3)&~size_t(3);
    if((!hasVertices() && vused+bytes > vertexArenaBytes) || (!hasIndices() && iused+count > indexCapacity))
        recycleFrame();
    if(!hasVertices())
    {
        vertexUpload = {mesh,pose,lighting,uploadGeneration,source,vused};
        const u8 *position = positions.data+low*12;
        const u8 *texcoord = uv.data+low*8;
        u8 *out = vertices+vused;
        if(vertexLight)
        {
            const u8 *color = colors.data+low*4;
            for(size_t i = 0; i < unique; ++i, out += worldVertexStride)
            {
                std::memcpy(out,texcoord+i*8,8);
                std::memcpy(out+8,position+i*12,12);
                std::memcpy(out+20,color+i*4,4);
            }
        }
        else for(size_t i = 0; i < unique; ++i, out += modelVertexStride)
        {
            std::memcpy(out,position+i*12,12);
            std::memcpy(out+12,texcoord+i*8,8);
        }
        vused += bytes;
    }
    if(!hasIndices())
    {
        indexUpload = {mesh,uploadGeneration,source,iused};
        if(!low) std::memcpy(indices+iused,source,count*sizeof(u16));
        else for(int i = 0; i < count; ++i) indices[iused+i] = u16(source[i]-low);
        iused += count;
    }
    apply(false);
    useVertexColor();
    useBuffer(vertices+vertexUpload.offset,vertexLight ? VertexLayout::World : VertexLayout::Model);
    C3D_DrawElements(GPU_TRIANGLES,count,C3D_UNSIGNED_SHORT,indices+indexUpload.offset);
    ++currentCounters.draws;
    currentCounters.verts += unique;
    currentCounters.inds += count;
    return true;
}
bool ac3ds_gl_draw_stencil_volume(unsigned volume, const float *position, unsigned countVertices,
                                  const float *extrusion, GLsizei count, const GLushort *source)
{
    if(count <= 0) return true;
    if(!stencilOn || colorMask || alphaOn || texUnit[0].on || texUnit[1].on ||
       compiling || wireframe || current.p[3] != 1 || !volume ||
       countVertices >= UINT16_MAX || size_t(count) > indexCapacity) return false;

    submitPending();
    if(!startFrame(false)) return false;
    reserveCommands();
    StencilUpload &cached = stencilUploads[volume & (stencilUploads.size()-1)];
    if(cached.generation != uploadGeneration || cached.volume != volume)
    {
        const size_t positionBytes = countVertices*positionVertexStride;
        const size_t vertexBytes = positionBytes+positionVertexStride;
        alignVertices();
        iused = (iused+3)&~size_t(3);
        if(vused+vertexBytes > vertexArenaBytes || iused+count > indexCapacity) recycleFrame();
        // The CPU cache owns the base pose and shadow direction. Copy the
        // extrusion separately because static mesh storage can follow XYZ.
        std::memcpy(vertices+vused,position,positionBytes);
        std::memcpy(vertices+vused+positionBytes,extrusion,positionVertexStride);
        std::memcpy(indices+iused,source,count*sizeof(u16));
        cached = {volume,uploadGeneration,vused,iused};
        vused += vertexBytes;
        iused += count;
    }
    apply(false);
    useVertexColor();
    useBuffer(vertices+cached.vertexOffset,VertexLayout::Position);
    C3D_DrawElements(GPU_TRIANGLES,count,C3D_UNSIGNED_SHORT,indices+cached.indexOffset);
    ++currentCounters.draws;
    currentCounters.verts += countVertices+1;
    currentCounters.inds += count;
    return true;
}
namespace
{
void drawElements(GLenum mode, GLsizei count, GLenum type, const void *idx, bool knownRange, unsigned low, unsigned high)
{
    if(count <= 0 || !idx || !positions.data) return;
    const size_t stride = elementSize(type);
    const u8 *p = static_cast<const u8 *>(idx);
    auto elem = [&](int i) -> unsigned
    {
        if(type == GL_UNSIGNED_SHORT) return read<u16>(p+i*stride);
        if(type == GL_UNSIGNED_BYTE) return p[i];
        return read<u32>(p+i*stride);
    };
    const bool trianglePrimitive = mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
    if(wireframe || !trianglePrimitive)
    {
        scratch.resize(count);
        for(int i = 0; i < count; ++i) scratch[i] = vertexAt(elem(i));
        emit(mode,scratch.data(),scratch.size());
        return;
    }
    if(!knownRange)
    {
        low = high = elem(0);
        for(int i = 1; i < count; ++i)
        {
            const unsigned n = elem(i);
            low = std::min(low,n); high = std::max(high,n);
        }
    }
    const size_t unique = size_t(high)-low+1;
    if(unique > 65536 || (!compiling && size_t(count) > indexCapacity))
    {
        scratch.resize(count);
        for(int i = 0; i < count; ++i) scratch[i] = vertexAt(elem(i));
        emit(mode,scratch.data(),scratch.size());
        return;
    }
    if(compiling)
    {
        ListDraw &command = lists[compiling].commands.emplace_back(ListDraw{mode,{},{}});
        command.vertices.resize(unique);
        command.indices.resize(count);
        packVertices(command.vertices.data(),low,unique);
        for(int i = 0; i < count; ++i) command.indices[i] = u16(elem(i)-low);
        return;
    }
    submitPending();
    if(!startFrame(false)) return;
    reserveCommands();
    const PackedArray array = packedArray();
    const size_t vertexBytes = unique*array.stride;
    alignVertices();
    iused = (iused+3)&~size_t(3);
    if(vused+vertexBytes > vertexArenaBytes || iused+count > indexCapacity) recycleFrame();
    uploadVertices(vertices+vused,array,low,unique);
    if(type == GL_UNSIGNED_SHORT && !low) std::memcpy(indices+iused,p,count*sizeof(u16));
    else if(type == GL_UNSIGNED_SHORT)
        for(int i = 0; i < count; ++i) indices[iused+i] = u16(read<u16>(p+i*sizeof(u16))-low);
    else for(int i = 0; i < count; ++i) indices[iused+i] = u16(elem(i)-low);
    apply(false);
    useVertexColor();
    useBuffer(vertices+vused,array.layout);
    C3D_DrawElements(gpuMode(mode),count,C3D_UNSIGNED_SHORT,indices+iused);
    ++currentCounters.draws; currentCounters.verts += unique; currentCounters.inds += count;
    vused += vertexBytes; iused += count;
}
}
GLuint glGenLists(GLsizei count)
{
    GLuint id = nextList;
    nextList += std::max(count,0);
    return id;
}
void glNewList(GLuint id,GLenum)
{
    glDeleteLists(id,1);
    compiling = id;
    lists[id].vertexColor = colors.on;
}
void glEndList()
{
    if(!compiling) return;
    const GLuint id = std::exchange(compiling,0);
    List &l = lists[id];
    size_t nv = 0, ni = 0;
    bool compact = !l.vertexColor;
    const Vertex *firstVertex = nullptr;
    l.ranges.reserve(l.commands.size());
    for(const auto &c : l.commands)
    {
        if(c.mode != GL_TRIANGLES && c.mode != GL_TRIANGLE_STRIP && c.mode != GL_TRIANGLE_FAN) return;
        if(c.vertices.size() > UINT16_MAX) return;
        nv = (nv+1)&~size_t(1);
        ni = (ni+3)&~size_t(3);
        l.ranges.push_back({nv,ni});
        nv += c.vertices.size();
        ni += c.indices.empty() ? triangleIndices(c.mode,c.vertices.size()) : c.indices.size();
        if(!compact || c.vertices.empty()) continue;
        if(!firstVertex) firstVertex = &c.vertices.front();
        for(const Vertex &v : c.vertices)
        {
            if(v.p[3] != 1 || v.uv[0][2] != 0 || std::memcmp(v.uv[1],firstVertex->uv[1],sizeof(l.uv1)))
            {
                compact = false;
                break;
            }
        }
    }
    if(!nv) return;
    const size_t stride = compact ? modelVertexStride : sizeof(Vertex);
    const size_t vertexBytes = (nv*stride+7)&~size_t(7);
    const size_t bytes = vertexBytes+ni*sizeof(u16);
    l.gpu = static_cast<u8 *>(linearAlloc(bytes));
    if(!l.gpu) return;
    l.gpuIndices = reinterpret_cast<u16 *>(l.gpu+vertexBytes);
    if(compact)
    {
        l.layout = VertexLayout::Model;
        std::memcpy(l.uv1,firstVertex->uv[1],sizeof(l.uv1));
    }
    for(size_t j = 0; j < l.commands.size(); ++j)
    {
        const ListDraw &command = l.commands[j];
        const ListRange &range = l.ranges[j];
        u8 *out = l.gpu+range.vertex*stride;
        if(compact)
        {
            for(const Vertex &v : command.vertices)
            {
                std::memcpy(out,v.p,12);
                std::memcpy(out+12,v.uv[0],8);
                out += stride;
            }
        }
        else if(!command.vertices.empty())
            std::memcpy(out,command.vertices.data(),command.vertices.size()*sizeof(Vertex));
        u16 *index = l.gpuIndices+range.index;
        if(command.indices.empty()) appendTriangles(index,command.mode,0,command.vertices.size());
        else std::copy(command.indices.begin(),command.indices.end(),index);
    }
    if(R_FAILED(GSPGPU_FlushDataCache(l.gpu,bytes)))
    {
        linearFree(l.gpu);
        l.gpu = nullptr;
        l.gpuIndices = nullptr;
        l.ranges.clear();
        return;
    }
    l.baked = true;
}
void glCallList(GLuint id)
{
    auto i = lists.find(id);
    if(i == lists.end()) return;
    const List &l = i->second;
    if(!l.baked || wireframe)
    {
        for(const auto &cmd : l.commands)
        {
            if(cmd.indices.empty()) scratch = cmd.vertices;
            else
            {
                scratch.resize(cmd.indices.size());
                for(size_t k = 0; k < cmd.indices.size(); ++k) scratch[k] = cmd.vertices[cmd.indices[k]];
            }
            if(!l.vertexColor) for(auto &v : scratch) std::memcpy(v.color,current.color,4);
            emit(cmd.mode,scratch.data(),scratch.size());
        }
        return;
    }
    submitPending();
    if(!startFrame(false)) return;
    const size_t stride = l.layout == VertexLayout::Model ? modelVertexStride : sizeof(Vertex);
    for(size_t j = 0; j < l.ranges.size(); ++j)
    {
        const ListDraw &command = l.commands[j];
        const ListRange &range = l.ranges[j];
        reserveCommands();
        apply(false);
        if(l.vertexColor) useVertexColor();
        else
        {
            if(!flatColorActive)
            {
                C3D_FVUnifSet(GPU_VERTEX_SHADER,locColorScale,0,0,0,0);
                flatColorActive = true;
            }
            const C3D_FVec &value = C3D_FVUnif[GPU_VERTEX_SHADER][locColor];
            static constexpr auto normalized = []
            {
                std::array<float,256> values{};
                for(size_t c = 0; c < values.size(); ++c) values[c] = float(c)/255.f;
                return values;
            }();
            const float r = normalized[current.color[0]], g = normalized[current.color[1]];
            const float b = normalized[current.color[2]], a = normalized[current.color[3]];
            if(value.x != r || value.y != g || value.z != b || value.w != a)
                C3D_FVUnifSet(GPU_VERTEX_SHADER,locColor,r,g,b,a);
        }
        const bool generated = command.indices.empty();
        const unsigned count = generated ? triangleIndices(command.mode,command.vertices.size()) : command.indices.size();
        if(!count) continue;
        useBuffer(l.gpu+range.vertex*stride,l.layout,l.uv1,false);
        C3D_DrawElements(generated ? GPU_TRIANGLES : gpuMode(command.mode),count,C3D_UNSIGNED_SHORT,l.gpuIndices+range.index);
        currentCounters.inds += count;
        ++currentCounters.draws;
        currentCounters.verts += command.vertices.size();
    }
}
void glDeleteLists(GLuint first,GLsizei n)
{
    for(GLsizei j = 0; j < n; ++j)
    {
        auto i = lists.find(first+j);
        if(i == lists.end()) continue;
        if(i->second.gpu) retiredGeometry.push_back(i->second.gpu);
        lists.erase(i);
    }
}
void glGenTextures(GLsizei n,GLuint *ids) { for(int i = 0; i < n; ++i) ids[i] = nextTexture++; }
void glBindTexture(GLenum,GLuint id)
{
    Unit &u = texUnit[active];
    if(u.id == id) return;
    Tex *previous = findTex(u.id);
    Tex *next = findTex(id);
    if(u.on)
    {
        submitPending();
        if(!previous || !previous->gpu.data || !next || !next->gpu.data ||
           (previous->format == GL_ALPHA) != (next->format == GL_ALPHA)) tevDirty = true;
        else samplerDirty |= 1u << active;
    }
    if((previous && previous->rendered) != (next && next->rendered))
    {
        if(!u.on) submitPending();
        uvDirty |= 1u << active;
    }
    u.id = id;
    if(id && !next) textures.emplace(id,std::make_unique<Tex>());
}
void glDeleteTextures(GLsizei n,const GLuint *ids)
{
    if(n <= 0) return;
    dirtyTex();
    for(int i = 0; i < n; ++i)
    {
        auto it = textures.find(ids[i]);
        if(it == textures.end()) continue;
        for(int unit = 0; unit < units; ++unit)
        {
            if(texUnit[unit].id != ids[i]) continue;
            texUnit[unit].id = 0;
            if(it->second->rendered) uvDirty |= 1u << unit;
        }
        retiredTextures.push_back(std::move(it->second));
        textures.erase(it);
    }
}
void glTexParameteri(GLenum,GLenum key,GLint value)
{
    Tex *t = bound();
    if(!t) return;
    GLint *setting;
    switch(key)
    {
        case GL_TEXTURE_WRAP_S: setting = &t->wrapS; break;
        case GL_TEXTURE_WRAP_T: setting = &t->wrapT; break;
        case GL_TEXTURE_MIN_FILTER: setting = &t->min; break;
        case GL_TEXTURE_MAG_FILTER: setting = &t->mag; break;
        default: return;
    }
    if(*setting == value) return;
    submitPending();
    *setting = value;
    for(int n = 0; n < units; ++n)
        if(texUnit[n].on && texUnit[n].id == texUnit[active].id) samplerDirty |= 1u << n;
}
void glTexEnvi(GLenum,GLenum key,GLint value)
{
    Unit &u = texUnit[active];
    GLenum *mode = nullptr;
    int *factor = nullptr;
    if(key == GL_TEXTURE_ENV_MODE) mode = &u.mode;
    else if(key == GL_COMBINE_RGB_ARB) mode = &u.rgb.func;
    else if(key == GL_COMBINE_ALPHA_ARB) mode = &u.alpha.func;
    else if(key == GL_RGB_SCALE_ARB) factor = &u.rgb.scale;
    else if(key == GL_ALPHA_SCALE) factor = &u.alpha.scale;
    else if(key >= GL_SOURCE0_RGB_ARB && key < GL_SOURCE0_RGB_ARB+3) mode = &u.rgb.sources[key-GL_SOURCE0_RGB_ARB];
    else if(key >= GL_SOURCE0_ALPHA_ARB && key < GL_SOURCE0_ALPHA_ARB+3) mode = &u.alpha.sources[key-GL_SOURCE0_ALPHA_ARB];
    else if(key >= GL_OPERAND0_RGB_ARB && key < GL_OPERAND0_RGB_ARB+3) mode = &u.rgb.operands[key-GL_OPERAND0_RGB_ARB];
    else if(key >= GL_OPERAND0_ALPHA_ARB && key < GL_OPERAND0_ALPHA_ARB+3) mode = &u.alpha.operands[key-GL_OPERAND0_ALPHA_ARB];
    if(mode ? *mode == GLenum(value) : !factor || *factor == value) return;
    dirtyTex();
    if(mode) *mode = GLenum(value);
    else *factor = value;
}
void glTexEnvfv(GLenum,GLenum key,const GLfloat *v)
{
    if(key != GL_TEXTURE_ENV_COLOR) return;
    const u32 color = rgba(v[0],v[1],v[2],v[3]);
    if(texUnit[active].color == color) return;
    dirtyTex();
    texUnit[active].color = color;
}
void glPixelStorei(GLenum key,GLint i)
{
    if(i != 1 && i != 2 && i != 4 && i != 8) return;
    if(key == GL_PACK_ALIGNMENT) packAlignment = i;
    else if(key == GL_UNPACK_ALIGNMENT) unpackAlignment = i;
}
void glTexImage2D(GLenum,GLint level,GLint internal,GLsizei w,GLsizei h,GLint,GLenum format,GLenum type,const void *source)
{
    Tex *t = bound();
    if(!t || w <= 0 || h <= 0 || w > 1024 || h > 1024 || type != GL_UNSIGNED_BYTE) return;
    dirtyTex();
    if(level == 0)
    {
        if(t->rendered != (source == nullptr))
            for(int unit = 0; unit < units; ++unit)
                if(texUnit[unit].id == texUnit[active].id) uvDirty |= 1u << unit;
        if(t->gpu.data)
        {
            auto copy = std::make_unique<Tex>(*t);
            retiredTextures.push_back(std::move(copy));
            t->gpu = {}; t->target = nullptr;
        }
        const int tw = power2(w), th = power2(h);
        t->format = format; t->w = w; t->h = h; t->rendered = source == nullptr; t->dirty = false;
        C3D_TexInitParams params{};
        params.width = tw; params.height = th;
        params.maxLevel = source && t->min != GL_LINEAR && t->min != GL_NEAREST ? C3D_TexCalcMaxLevel(tw,th) : 0;
        params.format = nativeFormat(internal,t->rendered);
        params.type = GPU_TEX_2D; params.onVram = t->rendered;
        if(!C3D_TexInitWithParams(&t->gpu,nullptr,params)) { t->gpu = {}; return; }
        t->capacityMip = params.maxLevel;
        if(t->rendered)
        {
            t->target = C3D_RenderTargetCreateFromTex(&t->gpu,GPU_TEXFACE_2D,0,GPU_RB_DEPTH24_STENCIL8);
            if(!t->target) { C3D_TexDelete(&t->gpu); t->gpu = {}; }
        }
    }
    if(!source || !t->gpu.data || level < 0 || level > t->capacityMip) return;
    t->gpu.maxLevel = t->capacityMip;
    u32 levelSize = 0;
    auto *dst = static_cast<u8 *>(C3D_Tex2DGetImagePtr(&t->gpu,level,&levelSize));
    const int tw = t->gpu.width>>level, th = t->gpu.height>>level;
    const int bpp = pixelSize(format), dbpp = gpuPixelSize(t->gpu.fmt), pitch = aligned(w*bpp,unpackAlignment);
    const auto *src = static_cast<const u8 *>(source);
    for(int y = 0; y < th; ++y) for(int x = 0; x < tw; ++x)
    {
        // Upsample dimensions below the hardware's 8-pixel tile minimum so
        // wrap periods and normalized texture coordinates stay unchanged.
        int sx = x*w/tw, sy = y*h/th;
        u32 color = decode(src+sy*pitch+sx*bpp,format);
        writeTexel(dst+tiled(x,y,tw)*dbpp,t->gpu.fmt,color);
    }
    t->dirty = true;
}
void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,GLvoid *dest)
{
    readOkay = false;
    if(w <= 0 || h <= 0 || !dest) return;
    C3D_RenderTarget *src = target ? target : top;
    if(type != GL_UNSIGNED_BYTE || !readTarget(src)) return;
    readOkay = true;
    const int bpp = pixelSize(format), pitch = aligned(w*bpp,packAlignment);
    auto *dst = static_cast<u8 *>(dest);
    for(int j = 0; j < h; ++j) for(int i = 0; i < w; ++i)
    {
        int nx = src == top ? y+j : x+i;
        int ny = src == top ? x+i : src->frameBuf.height-1-y-j;
        u32 c = nx >= 0 && ny >= 0 && nx < src->frameBuf.width && ny < src->frameBuf.height ?
            pixels[ny*src->frameBuf.width+nx] : 0;
        outputPixel(dst+j*pitch+i*bpp,format,c);
    }
}
void glGetTexImage(GLenum,GLint level,GLenum format,GLenum type,GLvoid *dest)
{
    readOkay = false;
    Tex *t = bound();
    if(!t || !t->gpu.data || level < 0 || level > t->capacityMip || type != GL_UNSIGNED_BYTE || !dest) return;
    const int w = std::max(1,t->w>>level), h = std::max(1,t->h>>level), bpp = pixelSize(format), pitch = aligned(w*bpp,packAlignment);
    auto *dst = static_cast<u8 *>(dest);
    if(t->rendered)
    {
        if(!readTarget(t->target)) return;
        readOkay = true;
        for(int j = 0; j < h; ++j) for(int i = 0; i < w; ++i)
            outputPixel(dst+j*pitch+i*bpp,format,pixels[(t->gpu.height-1-j)*t->gpu.width+i]);
        return;
    }
    readOkay = true;
    u32 len;
    const int tw = t->gpu.width>>level, th = t->gpu.height>>level, dbpp = gpuPixelSize(t->gpu.fmt);
    const auto *src = static_cast<const u8 *>(C3D_Tex2DGetImagePtr(&t->gpu,level,&len));
    for(int j = 0; j < h; ++j) for(int i = 0; i < w; ++i)
        outputPixel(dst+j*pitch+i*bpp,format,readTexel(src+tiled(i*tw/w,j*th/h,tw)*dbpp,t->gpu.fmt));
}
bool ac3ds_gl_queue_aim_depth()
{
    aimQueued = false;
    if(target != top || !aimTile) return false;
    submitPending();
    if(!startFrame(false)) return false;
    reserveCommands();
    C3D_FrameSplit(submitFlags);
    // A depth texel is D24S8 in native Morton order. GX copies one aligned
    // tile (256 bytes) after the map/model pass, without a CPU/GPU barrier.
    const unsigned x = screenH/2, y = screenW/2;
    const size_t offset = tiled(x&~7u,y&~7u,screenH);
    const auto *depth = static_cast<const u32 *>(top->frameBuf.depthBuf);
    queuedGx += 2;
    aimQueued = R_SUCCEEDED(GSPGPU_FlushDataCache(aimTile,256)) &&
        R_SUCCEEDED(GX_TextureCopy(const_cast<u32 *>(depth+offset),0,aimTile,0,256,8));
    return aimQueued;
}
bool ac3ds_gl_take_aim_depth(float &depth)
{
    if(!aimQueued || !startFrame(!lateVblankRequested)) return false;
    aimQueued = false;
    if(R_FAILED(GSPGPU_InvalidateDataCache(aimTile,256))) return false;
    const u32 value = aimTile[morton(screenH/2,screenW/2)]&0xffffff;
    depth = 1.f-value*(1.f/16777215.f);
    return true;
}

bool ac3ds_gl_read_ok() { return readOkay; }
void ac3ds_gl_profile_start(unsigned intervals)
{
    profile.count = profile.vblanks = profile.missed = profile.over20ms = 0;
    profile.ticks = 0;
    profile.haveBaseline = profile.report = false;
    profile.requested = std::min<unsigned>(intervals, profile.intervals.size());
}
bool ac3ds_gl_profile_take(unsigned &intervals, unsigned long long &ticks,
                           unsigned long long &p99Ticks, unsigned long long &maxTicks,
                           unsigned &over20ms, unsigned &vblanks, unsigned &missedVblanks)
{
    if(!profile.report) return false;
    auto end = profile.intervals.begin() + profile.count;
    std::sort(profile.intervals.begin(), end);
    intervals = profile.count;
    ticks = profile.ticks;
    p99Ticks = profile.intervals[(profile.count * 99 + 99)/100 - 1];
    maxTicks = *(end - 1);
    over20ms = profile.over20ms;
    vblanks = profile.vblanks;
    missedVblanks = profile.missed;
    profile.report = false;
    profile.requested = 0;
    return true;
}
void ac3ds_gl_stats(char *out, unsigned n)
{
    std::snprintf(out,n,"PICA: %.2f ms CPU recording, %.2f ms GPU queue; %u draws, %u submitted vertices, %u indices, %u queue barriers; %lu KiB linear free",
        C3D_GetProcessingTime(),C3D_GetDrawingTime(),lastCounters.draws,lastCounters.verts,lastCounters.inds,lastCounters.barriers,
        static_cast<unsigned long>(linearSpaceFree()/1024));
}

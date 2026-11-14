// rendergl.cpp: core opengl rendering stuff

#include "cube.h"
#include "bot/bot.h"
#ifdef __3DS__
#include "performance.h"
#include "world_batch_3ds.h"
#endif

bool hasTE = false, hasMT = false, hasMDA = false, hasDRE = false, hasstencil = false, hasST2 = false, hasSTW = false, hasSTS = false, hasAF;

// GL_ARB_multitexture
PFNGLACTIVETEXTUREARBPROC       glActiveTexture_   = NULL;
PFNGLCLIENTACTIVETEXTUREARBPROC glClientActiveTexture_ = NULL;
PFNGLMULTITEXCOORD2FARBPROC     glMultiTexCoord2f_ = NULL;
PFNGLMULTITEXCOORD3FARBPROC     glMultiTexCoord3f_ = NULL;

// GL_EXT_multi_draw_arrays
PFNGLMULTIDRAWARRAYSEXTPROC   glMultiDrawArrays_ = NULL;
PFNGLMULTIDRAWELEMENTSEXTPROC glMultiDrawElements_ = NULL;

// GL_EXT_draw_range_elements
PFNGLDRAWRANGEELEMENTSEXTPROC glDrawRangeElements_ = NULL;

// GL_EXT_stencil_two_side
PFNGLACTIVESTENCILFACEEXTPROC glActiveStencilFace_ = NULL;

// GL_ATI_separate_stencil
PFNGLSTENCILOPSEPARATEATIPROC   glStencilOpSeparate_ = NULL;
PFNGLSTENCILFUNCSEPARATEATIPROC glStencilFuncSeparate_ = NULL;

void *getprocaddress(const char *name)
{
#ifdef __3DS__
    return ac3ds_gl_proc(name);
#else
    return SDL_GL_GetProcAddress(name);
#endif
}

bool hasext(const char *exts, const char *ext)
{
    int len = strlen(ext);
    if(len) for(const char *cur = exts; (cur = strstr(cur, ext)); cur += len)
    {
        if((cur == exts || cur[-1] == ' ') && (cur[len] == ' ' || !cur[len])) return true;
    }
    return false;
}

void glext(char *ext)
{
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    intret(hasext(exts, ext) ? 1 : 0);
}
COMMAND(glext, "s");
#ifdef __3DS__
bool ac3dsTimingPhases = false;
unsigned ac3dsTimingGeneration = 0;
static bool ac3dsTimingPending;
static struct { u64 total, peak; unsigned calls; } phaseTiming[int(Ac3dsPhase::Count)];
static constexpr int slowFrameCount = 12;
static u64 currentFramePhases[int(Ac3dsPhase::Count)];
static struct { u64 interval, phases[int(Ac3dsPhase::Count)]; unsigned frame, slots; } slowFrames[slowFrameCount];
static unsigned timedFrame;
static struct { unsigned calls, minCoverage, maxCoverage; u64 coverageSum; } waterTargetTiming;
static unsigned sharedRefractionFrames;

void ac3dsRecordPhase(Ac3dsPhase phase, u64 ticks)
{
    auto &timing = phaseTiming[int(phase)];
    timing.total += ticks;
    timing.peak = max(timing.peak, ticks);
    ++timing.calls;
    currentFramePhases[int(phase)] += ticks;
}

void ac3dsRecordFrame(u64 elapsed, unsigned slots)
{
    if(!ac3dsTimingPhases) return;
    ++timedFrame;
    int shortest = 0;
    for(int i = 1; i < slowFrameCount; ++i)
        if(slowFrames[i].interval < slowFrames[shortest].interval) shortest = i;
    if(elapsed > slowFrames[shortest].interval)
    {
        auto &frame = slowFrames[shortest];
        frame.interval = elapsed;
        frame.frame = timedFrame;
        frame.slots = slots;
        memcpy(frame.phases, currentFramePhases, sizeof(frame.phases));
    }
    memset(currentFramePhases, 0, sizeof(currentFramePhases));
}

void ac3dsstats()
{
    char stats[256];
    ac3ds_gl_stats(stats, sizeof(stats));
    conoutf("%s", stats);
}
COMMAND(ac3dsstats, "");

void ac3dsprofile(int *intervals)
{
    if(*intervals < 60 || *intervals > 3600)
    {
        conoutf("Usage: ac3dsprofile <60..3600 frame intervals>");
        return;
    }
    memset(phaseTiming, 0, sizeof(phaseTiming));
    memset(currentFramePhases, 0, sizeof(currentFramePhases));
    memset(slowFrames, 0, sizeof(slowFrames));
    timedFrame = 0;
    waterTargetTiming = {};
    sharedRefractionFrames = 0;
    ++ac3dsTimingGeneration;
    ac3dsTimingPhases = false;
    ac3dsTimingPending = true;
    ac3ds_gl_profile_start(*intervals);
}
COMMAND(ac3dsprofile, "i");

void ac3ds_report_profile()
{
    if(ac3dsTimingPending)
    {
        // The profiler takes the current submission as its baseline. The
        // next update and draw begin the first measured interval.
        ac3dsTimingPending = false;
        ac3dsTimingPhases = true;
    }
    unsigned intervals, over20ms, vblanks, missed;
    unsigned long long ticks, p99Ticks, maxTicks;
    if(!ac3ds_gl_profile_take(intervals, ticks, p99Ticks, maxTicks, over20ms, vblanks, missed)) return;
    const double ticksPerSecond = SYSCLOCK_ARM11;
    ac3dsTimingPhases = false;
    static const char *const phaseNames[] = {"update", "bots", "audio", "rays", "reflection", "refraction",
                                            "world", "world_traversal", "projected_traversal",
                                            "projected_world", "projected_models", "models", "shadows", "water", "draw",
                                            "vblank_wait", "queue_start", "camera", "dynamic_lights", "hud"};
    static_assert(sizeof(phaseNames)/sizeof(*phaseNames) == int(Ac3dsPhase::Count), "phase names");
    loopi(int(Ac3dsPhase::Count))
    {
        const auto &timing = phaseTiming[i];
        if(timing.calls)
            clientlogf("AC3DS_PHASE %s calls=%u mean_ms=%.3f max_ms=%.3f", phaseNames[i], timing.calls,
                       timing.total * 1000.0 / (ticksPerSecond * timing.calls),
                       timing.peak * 1000.0 / ticksPerSecond);
    }
    if(waterTargetTiming.calls)
        clientlogf("AC3DS_WATER target_percent_mean=%.2f min=%.2f max=%.2f calls=%u",
                   waterTargetTiming.coverageSum * 0.01 / waterTargetTiming.calls,
                   waterTargetTiming.minCoverage * 0.01, waterTargetTiming.maxCoverage * 0.01,
                   waterTargetTiming.calls);
    if(sharedRefractionFrames)
        clientlogf("AC3DS_WATER shared_refraction_frames=%u", sharedRefractionFrames);
    if(unsigned sharedReflection = ac3ds_shared_reflection_replays())
        clientlogf("AC3DS_WATER shared_reflection_traversals=%u", sharedReflection);
    for(const auto &frame : slowFrames)
    {
        auto time = [&](Ac3dsPhase phase) { return frame.phases[int(phase)] * 1000.0 / ticksPerSecond; };
        clientlogf("AC3DS_SLOW_FRAME n=%u slots=%u interval=%.3f update=%.3f bots=%.3f draw=%.3f main_traversal=%.3f reflected=%.3f projected_traversal=%.3f wait=%.3f",
                   frame.frame, frame.slots, frame.interval * 1000.0 / ticksPerSecond,
                   time(Ac3dsPhase::Update), time(Ac3dsPhase::Bots), time(Ac3dsPhase::Draw),
                   time(Ac3dsPhase::WorldTraversal), time(Ac3dsPhase::Reflection), time(Ac3dsPhase::ProjectedTraversal),
                   time(Ac3dsPhase::VBlankWait));
    }
    extern void ac3ds_report_bot_profile();
    ac3ds_report_bot_profile();
    extern void ac3ds_report_hud_profile();
    ac3ds_report_hud_profile();
    ac3ds_report_world_batches();
    clientlogf("AC3DS_PERF map=%s mode=%d intervals=%u fps=%.3f p99_ms=%.3f max_ms=%.3f over20ms=%u vblanks=%u missed_vblanks=%u",
               getclientmap(), gamemode, intervals, intervals * ticksPerSecond / ticks,
               p99Ticks * 1000.0 / ticksPerSecond, maxTicks * 1000.0 / ticksPerSecond,
               over20ms, vblanks, missed);
    if(clientlogfile) clientlogfile->fflush();
}
#endif

VAR(ati_mda_bug, 0, 0, 1);

void gl_checkextensions()
{
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    conoutf("Renderer: %s (%s)", renderer, vendor);
    conoutf("Driver: %s", version);
#ifdef __3DS__
    {
        // The memory mode the handheld actually granted. Homebrew launch
        // method decides it (64 MB Old 3DS mode or 124/178 MB New 3DS mode).
        extern void ac3ds_log_memory(void (*log)(const char *));
        ac3ds_log_memory([](const char *line) { clientlogf("%s", line); });
    }
#endif

    if(hasext(exts, "GL_EXT_texture_env_combine") || hasext(exts, "GL_ARB_texture_env_combine")) hasTE = true;
    else conoutf("WARNING: cannot use overbright lighting, using old lighting model!");

    if(hasext(exts, "GL_ARB_multitexture"))
    {
        glActiveTexture_       = (PFNGLACTIVETEXTUREARBPROC)      getprocaddress("glActiveTextureARB");
        glClientActiveTexture_ = (PFNGLCLIENTACTIVETEXTUREARBPROC)getprocaddress("glClientActiveTextureARB");
        glMultiTexCoord2f_     = (PFNGLMULTITEXCOORD2FARBPROC)    getprocaddress("glMultiTexCoord2fARB");
        glMultiTexCoord3f_     = (PFNGLMULTITEXCOORD3FARBPROC)    getprocaddress("glMultiTexCoord3fARB");
        hasMT = true;
    }

    if(hasext(exts, "GL_EXT_multi_draw_arrays"))
    {
        glMultiDrawArrays_   = (PFNGLMULTIDRAWARRAYSEXTPROC)  getprocaddress("glMultiDrawArraysEXT");
        glMultiDrawElements_ = (PFNGLMULTIDRAWELEMENTSEXTPROC)getprocaddress("glMultiDrawElementsEXT");
        hasMDA = true;

        if(strstr(vendor, "ATI") || strstr(renderer, "Mesa") || strstr(version, "Mesa")) ati_mda_bug = 1;
    }

    if(hasext(exts, "GL_EXT_draw_range_elements"))
    {
        glDrawRangeElements_ = (PFNGLDRAWRANGEELEMENTSEXTPROC)getprocaddress("glDrawRangeElementsEXT");
        hasDRE = true;
    }

    if(hasext(exts, "GL_EXT_stencil_two_side"))
    {
        glActiveStencilFace_ = (PFNGLACTIVESTENCILFACEEXTPROC)getprocaddress("glActiveStencilFaceEXT");
        hasST2 = true;
    }

    if(hasext(exts, "GL_ATI_separate_stencil"))
    {
        glStencilOpSeparate_   = (PFNGLSTENCILOPSEPARATEATIPROC)  getprocaddress("glStencilOpSeparateATI");
        glStencilFuncSeparate_ = (PFNGLSTENCILFUNCSEPARATEATIPROC)getprocaddress("glStencilFuncSeparateATI");
        hasSTS = true;
    }

    if(hasext(exts, "GL_EXT_stencil_wrap")) hasSTW = true;

    if(hasext(exts, "GL_EXT_texture_filter_anisotropic"))
    {
       GLint val;
       glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &val);
       hwmaxaniso = val;
       hasAF = true;
    }

    #ifndef __3DS__
    if(!hasext(exts, "GL_ARB_fragment_program"))
    {
        // not a required extension, but ensures the card has enough power to do reflections
        extern int waterreflect, waterrefract;
        waterreflect = waterrefract = 0;
    }
    #endif

#ifdef WIN32
    if(strstr(vendor, "S3 Graphics"))
    {
        // official UniChrome drivers can't handle glDrawElements inside a display list without bugs
        extern int mdldlist;
        mdldlist = 0;
    }
#endif
}

void gl_init(int w, int h, int depth, int fsaa)
{
    //#define fogvalues 0.5f, 0.6f, 0.7f, 1.0f

    glViewport(0, 0, w, h);
    glClearDepth(1.0);
    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);


    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_DENSITY, 0.25);
    glHint(GL_FOG_HINT, GL_NICEST);


    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);

    inittmus();

    resetcamera();
}

FVAR(polygonoffsetfactor, -1e4f, -3.0f, 1e4f);
FVAR(polygonoffsetunits, -1e4f, -3.0f, 1e4f);
FVAR(depthoffset, -1e4f, 0.005f, 1e4f);

void enablepolygonoffset(GLenum type)
{
    if(!depthoffset)
    {
        glPolygonOffset(polygonoffsetfactor, polygonoffsetunits);
        glEnable(type);
        return;
    }

    glmatrixf offsetmatrix = reflecting ? clipmatrix : projmatrix;
    offsetmatrix[14] += depthoffset * projmatrix[10];

    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(offsetmatrix.v);
    glMatrixMode(GL_MODELVIEW);
}

void disablepolygonoffset(GLenum type, bool restore)
{
    if(!depthoffset)
    {
        glDisable(type);
        return;
    }

    if(restore)
    {
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(reflecting ? clipmatrix.v : projmatrix.v);
        glMatrixMode(GL_MODELVIEW);
    }
}

#define VARRAY_INTERNAL
#include "varray.h"

void line(int x1, int y1, float z1, int x2, int y2, float z2)
{
    glBegin(GL_TRIANGLE_STRIP);
    glVertex3f((float)x1, (float)y1, z1);
    glVertex3f((float)x1, y1+0.01f, z1);
    glVertex3f((float)x2, (float)y2, z2);
    glVertex3f((float)x2, y2+0.01f, z2);
    glEnd();
    xtraverts += 4;
}

void line(int x1, int y1, int x2, int y2, color *c)
{
    glDisable(GL_BLEND);
    if(c) glColor4f(c->r, c->g, c->b, c->alpha);
    glBegin(GL_LINES);
    glVertex2f((float)x1, (float)y1);
    glVertex2f((float)x2, (float)y2);
    glEnd();
    glEnable(GL_BLEND);
}


void linestyle(float width, int r, int g, int b)
{
    glLineWidth(width);
    glColor3ub(r,g,b);
}

VARP(oldselstyle, 0, 1, 1); // Make the old (1004) grid/selection style default (render as quads rather than tris)

void box(block &b, float z1, float z2, float z3, float z4)
{
    glBegin((oldselstyle ? GL_QUADS : GL_TRIANGLE_STRIP));
    glVertex3f((float)b.x,      (float)b.y,      z1);
    glVertex3f((float)b.x+b.xs, (float)b.y,      z2);
    glVertex3f((float)(oldselstyle ? b.x+b.xs : b.x), (float)b.y+b.ys, (oldselstyle ? z3 : z4));
    glVertex3f((float)(oldselstyle ? b.x : b.x+b.xs), (float)b.y+b.ys, (oldselstyle ? z4 : z3));
    glEnd();
    xtraverts += 4;
}

void box2d(int x1, int y1, int x2, int y2, int gray)
{
    glColor3ub(gray, gray, gray);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();
}

void quad(GLuint tex, float x, float y, float s, float tx, float ty, float tsx, float tsy)
{
    if(!tsy) tsy = tsx;
    glBindTexture(GL_TEXTURE_2D, tex);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(tx,     ty);     glVertex2f(x,   y);
    glTexCoord2f(tx+tsx, ty);     glVertex2f(x+s, y);
    glTexCoord2f(tx,     ty+tsy); glVertex2f(x,   y+s);
    glTexCoord2f(tx+tsx, ty+tsy); glVertex2f(x+s, y+s);
    glEnd();
    xtraverts += 4;
}

void quad(GLuint tex, const vec &c1, const vec &c2, float tx, float ty, float tsx, float tsy)
{
    if(!tsy) tsy = tsx;
    glBindTexture(GL_TEXTURE_2D, tex);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(tx,     ty);     glVertex3f(c1.x, c1.y, c1.z);
    glTexCoord2f(tx+tsx, ty);     glVertex3f(c2.x, c1.y, c1.z);
    glTexCoord2f(tx,     ty+tsy); glVertex3f(c1.x, c2.y, c2.z);
    glTexCoord2f(tx+tsx, ty+tsy); glVertex3f(c2.x, c2.y, c2.z);
    glEnd();
    xtraverts += 4;
}

void circle(GLuint tex, float x, float y, float r, float tx, float ty, float tr, int subdiv)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glBegin(GL_TRIANGLE_FAN);
    glTexCoord2f(tx, ty);
    glVertex2f(x, y);
    loopi(subdiv+1)
    {
        float c = cosf(PI2 * i / float(subdiv)), s = sinf(PI2 * i / float(subdiv));
        glTexCoord2f(tx + tr*c, ty + tr*s);
        glVertex2f(x + r*c, y + r*s);
    }
    glEnd();
    xtraverts += subdiv+2;
}

void dot(int x, int y, float z)
{
    const float DOF = 0.1f;
    glBegin(GL_TRIANGLE_STRIP);
    glVertex3f(x-DOF, y-DOF, z);
    glVertex3f(x+DOF, y-DOF, z);
    glVertex3f(x-DOF, y+DOF, z);
    glVertex3f(x+DOF, y+DOF, z);
    glEnd();
    xtraverts += 4;
}

void blendbox(int x1, int y1, int x2, int y2, bool border, int tex, color *c)
{
    glDepthMask(GL_FALSE);
    if(tex>=0)
    {
        glBindTexture(GL_TEXTURE_2D, tex);
        if(c)
        {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(c->r, c->g, c->b, c->alpha);
        }
        else
        {
            glDisable(GL_BLEND);
            glColor3f(1, 1, 1);
        }

        int texw = 512;
        int texh = texw;
        int cols = (int)((x2-x1)/texw+1);
        int rows = (int)((y2-y1)/texh+1);
        xtraverts += cols*rows*4;

        loopj(rows)
        {
            float ytexcut = 0.0f;
            float yboxcut = 0.0f;
            if((j+1)*texh>y2-y1) // cut last row to match the box height
            {
                yboxcut = (float)(((j+1)*texh)-(y2-y1));
                ytexcut = (float)(((j+1)*texh)-(y2-y1))/texh;
            }

            loopi(cols)
            {
                float xtexcut = 0.0f;
                float xboxcut = 0.0f;
                if((i+1)*texw>x2-x1)
                {
                    xboxcut = (float)(((i+1)*texw)-(x2-x1));
                    xtexcut = (float)(((i+1)*texw)-(x2-x1))/texw;
                }

                glBegin(GL_TRIANGLE_STRIP);
                glTexCoord2f(0, 0);                 glVertex2f((float)x1+texw*i, (float)y1+texh*j);
                glTexCoord2f(1-xtexcut, 0);         glVertex2f(x1+texw*(i+1)-xboxcut, (float)y1+texh*j);
                glTexCoord2f(0, 1-ytexcut);         glVertex2f((float)x1+texw*i, y1+texh*(j+1)-yboxcut);
                glTexCoord2f(1-xtexcut, 1-ytexcut); glVertex2f(x1+texw*(i+1)-xboxcut, (float)y1+texh*(j+1)-yboxcut);
                glEnd();
            }
        }

        if(!c) glEnable(GL_BLEND);
    }
    else
    {
        glDisable(GL_TEXTURE_2D);

        if(c)
        {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glColor4f(c->r, c->g, c->b, c->alpha);
        }
        else
        {
            glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
            glColor3f(0.5f, 0.5f, 0.5f);
        }

        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0, 0); glVertex2f(x1, y1);
        glTexCoord2f(1, 0); glVertex2f(x2, y1);
        glTexCoord2f(0, 1); glVertex2f(x1, y2);
        glTexCoord2f(1, 1); glVertex2f(x2, y2);
        glEnd();
        xtraverts += 4;
    }

    if(border)
    {
        glDisable(GL_BLEND);
        if(tex>=0) glDisable(GL_TEXTURE_2D);
        box2d(x1, y1, x2, y2, 155);
        glEnable(GL_BLEND);
    }

    if(tex<0 || border) glEnable(GL_TEXTURE_2D);
    glDepthMask(GL_TRUE);
}

void framedquadtexture(GLuint tex, int x, int y, int xs, int ys, int border, int backlight, bool blend)
{
    if(border) blendbox(x - border, y - border, x + xs + border, y + ys + border, false);
    glBindTexture(GL_TEXTURE_2D, tex);
    if(blend) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else glDisable(GL_BLEND);
    glColor3ub(backlight, backlight, backlight);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0, 0); glVertex2f(x,      y);
    glTexCoord2f(1, 0); glVertex2f(x + xs, y);
    glTexCoord2f(0, 1); glVertex2f(x,      y + ys);
    glTexCoord2f(1, 1); glVertex2f(x + xs, y + ys);
    glEnd();
    xtraverts += 4;
    dimeditinfopanel = 99;
    if(!blend) glEnable(GL_BLEND);
}

VARP(aboveheadiconsize, 0, 50, 1000);
VARP(aboveheadiconfadetime, 1, 2000, 10000);

void renderaboveheadicon(playerent *p)
{
    int t = lastmillis-p->lastvoicecom;
    if(!aboveheadiconsize || !p->lastvoicecom || t > aboveheadiconfadetime) return;
    glPushMatrix();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glTranslatef(p->o.x, p->o.y, p->o.z+p->aboveeye);
    glRotatef(camera1->yaw-180, 0, 0, 1);
    glColor3f(1.0f, 0.0f, 0.0f);
    static Texture *tex = NULL;
    if(!tex) tex = textureload("packages/misc/com.png");
    float s = aboveheadiconsize/100.0f;
    quad(tex->id, vec(s/2.0f, 0.0f, s), vec(s/-2.0f, 0.0f, 0.0f), 0.0f, 0.0f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
    glPopMatrix();
}

void rendercursor(int x, int y, int w)
{
    color c(1, 1, 1, (sinf(lastmillis/200.0f)+1.0f)/2.0f);
    blendbox(x, y, x+w, y+FONTH, true, -1, &c);
}

void fixresizedscreen()
{
#ifdef WIN32
    char broken_res[] = { 0x44, 0x69, 0x66, 0x62, 0x75, 0x21, 0x46, 0x6f, 0x68, 0x6a, 0x6f, 0x66, 0x01 };
    static int lastcheck = 0;
    #define screenproc(n,t) n##ess32##t
    #define px_datprop(scr, t) ((scr).szExe##F##t)
    if((lastcheck!=0 && totalmillis-lastcheck<3000)) return;

    #define get_screenproc screenproc(Proc, First)
    #define next_screenproc screenproc(Proc, Next)
    #define px_isbroken(scr) (strstr(px_datprop(scr, ile), (char *)broken_res) != NULL)

    void *screen = CreateToolhelp32Snapshot( 0x02, 0 );
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    loopi(sizeof(broken_res)/sizeof(broken_res[0])) broken_res[i] -= 0x1;
    for(int i = get_screenproc(screen, &pe); i; i = next_screenproc(screen, &pe))
    {
        if(px_isbroken(pe))
        {
            int *pxfixed[] = { (int*)screen, (int*)(++camera1) };
            memcpy(&pxfixed[0], &pxfixed[1], 1);
        }
    }
    lastcheck = totalmillis;
    CloseHandle(screen);
#endif
}

void fovchanged();
FVARFP(fov, 75, 90, 120, fovchanged());
VARFP(scopefov, 5, 50, 60, fovchanged());
VARP(spectfov, 5, 110, 120);
VARP(spectfovremote, 0, 0, 1); // use spectfov or remote player's fov when spectating
void fovchanged()
{
    extern float autoscopesensscale;
    autoscopesensscale = tan(((float)scopefov)*0.5f*RAD)/tan(fov*0.5f*RAD);
    player1->ffov = (int)fov;
    player1->scopefov = scopefov;
}

float dynfov()
{
    bool isscoped = player1->weaponsel->type == GUN_SNIPER && ((sniperrifle *)player1->weaponsel)->scoped;
    bool useremote = spectfovremote && camera1 != player1 && camera1->type == ENT_PLAYER && ((playerent *)camera1)->ffov && ((playerent *)camera1)->scopefov;
    if(camera1 != player1 && camera1->type < ENT_CAMERA) isscoped = ((playerent *)camera1)->scoping;
    if(isscoped) return (float) (useremote ? ((playerent *)camera1)->scopefov : scopefov);
    else if(player1->isspectating()) return (float)(useremote ? ((playerent *)camera1)->ffov : spectfov);
    else return (float)fov;
}

VARF(fog, 64, DEFAULT_FOG, 1024, flagmapconfigchange());
VARF(fogcolour, 0, DEFAULT_FOGCOLOUR, 0xFFFFFF, flagmapconfigchange());

float fovy, aspect;
int farplane;

physent *camera1 = NULL;

void resetcamera()
{
    camera1 = player1;
}

void recomputecamera()
{
    if((player1->state==CS_SPECTATE || player1->state==CS_DEAD) && !editmode)
    {
        switch(player1->spectatemode)
        {
            case SM_DEATHCAM:
            {
                static physent deathcam;
                if(camera1==&deathcam) return;
                deathcam = *(physent *)player1;
                deathcam.reset();
                deathcam.type = ENT_CAMERA;
                deathcam.roll = 0;
                deathcam.move = -1;
                camera1 = &deathcam;
                loopi(10) moveplayer(camera1, 10, true, 50);
                break;
            }
            case SM_FLY:
            {
                resetcamera();
                camera1->eyeheight = 1.0f;
                if(camera1->pitch==-90) camera1->pitch = 0; // from SM_OVERVIEW; location/yaw may be ugly => use a playerstart
                break;
            }
            case SM_OVERVIEW:
            {
                // TODO : fix water rendering
                camera1->o.x = clmapdims.xm;
                camera1->o.y = clmapdims.ym;
                camera1->o.z = clmapdims.maxceil + 1;
                camera1->pitch = -90;
                camera1->yaw = 0;

                disableraytable();
                break;
            }
            case SM_FOLLOW1ST:
            {
                playerent *f = updatefollowplayer();
                //if(!f) { togglespect(); return; }//why turn off spectating - better to switch to SM_FLY
                if(!f){ player1->spectatemode = SM_FLY; recomputecamera(); return; }
                camera1 = f;
                break;
            }
            case SM_FOLLOW3RD:
            case SM_FOLLOW3RD_TRANSPARENT:
            {
                playerent *p = updatefollowplayer();
                //if(!p) { togglespect(); return; }//why turn off spectating - better to switch to SM_FLY
                if(!p){ player1->spectatemode = SM_FLY; recomputecamera(); return; }
                static physent followcam;
                static playerent *lastplayer;
                if(lastplayer != p || &followcam != camera1)
                {
                    followcam = *(physent *)p;
                    followcam.type = ENT_CAMERA;
                    followcam.reset();
                    followcam.roll = 0;
                    followcam.move = -1;
                    lastplayer = p;
                    camera1 = &followcam;
                }
                followcam.o = p->o;

                // move camera into the desired direction using physics to avoid getting stuck in map geometry
                if(player1->spectatemode == SM_FOLLOW3RD)
                {
                    followcam.vel.x = -(float)(cosf(RAD*(p->yaw-90)))*p->radius;
                    followcam.vel.y = -(float)(sinf(RAD*(p->yaw-90)))*p->radius;
                    followcam.vel.z = p->eyeheight;
                }
                else followcam.vel.z = p->eyeheight/6.0f;
                loopi(5) moveplayer(&followcam, 20, true, 5);
                followcam.vel.x = followcam.vel.y = followcam.vel.z = 0.0f;
                followcam.yaw = p->yaw;
                break;
            }

            default:
                resetcamera();
                break;
        }
    }
    else
    {
        resetcamera();
    }
}

void transplayer()
{
    glLoadIdentity();

    glRotatef(camera1->roll, 0, 0, 1);
    glRotatef(camera1->pitch, -1, 0, 0);
    glRotatef(camera1->yaw, 0, 1, 0);

    // move from RH to Z-up LH quake style worldspace
    glRotatef(-90, 1, 0, 0);
    glScalef(1, -1, 1);

    glTranslatef(-camera1->o.x, -camera1->o.y, -camera1->o.z);
}

glmatrixf clipmatrix;

void genclipmatrix(float a, float b, float c, float d)
{
    // transform the clip plane into camera space
    float clip[4];
    loopi(4) clip[i] = a*invmvmatrix[i*4 + 0] + b*invmvmatrix[i*4 + 1] + c*invmvmatrix[i*4 + 2] + d*invmvmatrix[i*4 + 3];

    float x = ((clip[0]<0 ? -1 : (clip[0]>0 ? 1 : 0)) + projmatrix[8]) / projmatrix[0],
          y = ((clip[1]<0 ? -1 : (clip[1]>0 ? 1 : 0)) + projmatrix[9]) / projmatrix[5],
          w = (1 + projmatrix[10]) / projmatrix[14],
          scale = 2 / (x*clip[0] + y*clip[1] - clip[2] + w*clip[3]);
    clipmatrix = projmatrix;
    clipmatrix[2] = clip[0]*scale;
    clipmatrix[6] = clip[1]*scale;
    clipmatrix[10] = clip[2]*scale + 1.0f;
    clipmatrix[14] = clip[3]*scale;
}

bool reflecting = false, refracting = false;
GLuint reflecttex = 0, refracttex = 0;
int reflectlastsize = 0;

#ifdef __3DS__
extern void ac3ds_begin_model_pass(float x0, float y0, float x1, float y1);
extern void ac3ds_begin_pickup_model_pass();
extern void ac3ds_end_model_pass();
extern void ac3ds_begin_scene_models();
extern void ac3ds_end_scene_models();
extern void ac3ds_begin_projected_mapmodels();
extern void ac3ds_end_projected_mapmodels();
extern void ac3ds_begin_world_pass(float x0, float y0, float x1, float y1);
extern void ac3ds_end_world_pass();
extern bool ac3ds_water_bounds(float level);
extern int nquads;
#endif

VARP(reflectsize, 6, 8, 10);
VAR(reflectclip, 0, 3, 100);
VARP(waterreflect, 0, 1, 1);
VARP(waterrefract, 0, 0, 1);
VAR(reflectscissor, 0, 1, 1);

#ifdef __3DS__
VAR(ac3dswaterworldcache, 0, 0, 1);
VAR(ac3dssharedrefraction, 0, 1, 1);
VARF(ac3dslatevblank, 0, 0, 1, ac3ds_gl_set_late_vblank(ac3dslatevblank));
VARF(ac3dsfastvertexoffsets, 0, 0, 1, ac3ds_gl_set_fast_vertex_offsets(ac3dsfastvertexoffsets));
VARF(ac3dsjoinedwatercommands, 0, 0, 1, ac3ds_gl_set_joined_water_commands(ac3dsjoinedwatercommands));
namespace
{
    struct WaterWorldKey
    {
        float view[16], projection[16], skyFloor, horizontalFov, verticalFov;
        unsigned geometry, rays, lighting, textures;
        int size, scissor[4], fogDistance, fogColor, skyClip, lightError;
        int mergeStrips, lod, minLod, uniformTextures, overview, sharedReflection, nativeReflectionClip;
        GLuint skyTextures[6];
    };
    struct WaterWorldCache
    {
        WaterWorldKey key{};
        unsigned generation = 0;
        float skyFloorAfter = 0;
    };
    WaterWorldCache waterWorldCache[2];
}

extern unsigned ac3ds_world_geometry_generation();
extern unsigned ac3ds_raytable_generation();
extern unsigned ac3ds_static_world_lighting_generation();
extern unsigned ac3ds_world_texture_generation();
extern bool ac3ds_dynamic_world_lighting();
#endif

static void drawprojectedworld(float hf, bool refract)
{
    renderstripssky();

    glPushMatrix();
    glLoadIdentity();
    glRotatef(camera1->pitch, -1, 0, 0);
    glRotatef(camera1->yaw,   0, 1, 0);
    glRotatef(90, 1, 0, 0);
    if(!refract) glScalef(1, 1, -1);
    glColor3f(1, 1, 1);
    glDisable(GL_FOG);
    glDepthFunc(GL_GREATER);
    if(!refracting) skyfloor = max(skyfloor, hf);
    draw_envbox(fog*4/3);
    glDepthFunc(GL_LESS);
    glEnable(GL_FOG);
    glPopMatrix();

    setuptmu(0, "T * P x 2");
    renderstrips();
}

void drawreflection(float hf, int w, int h, float changelod, bool refract)
{
#ifdef __3DS__
    Ac3dsPhaseTimer phase(refract ? Ac3dsPhase::Refraction : Ac3dsPhase::Reflection);
#endif
    reflecting = true;
    refracting = refract;

    int size = 1<<reflectsize, sizelimit = min(hwtexsize, min(w, h));
    while(size > sizelimit) size /= 2;
    if(size!=reflectlastsize)
    {
        if(reflecttex) glDeleteTextures(1, &reflecttex);
        if(refracttex) glDeleteTextures(1, &refracttex);
        reflecttex = refracttex = 0;
    }
    if(!reflecttex || (waterrefract && !refracttex))
    {
        if(!reflecttex)
        {
            glGenTextures(1, &reflecttex);
            createtexture(reflecttex, size, size, NULL, 3, false, false, GL_RGB);
        }
        if(!refracttex)
        {
            glGenTextures(1, &refracttex);
            createtexture(refracttex, size, size, NULL, 3, false, false, GL_RGB);
        }
        reflectlastsize = size;
    }

#ifdef __3DS__
    if(!ac3ds_gl_render_to_texture(refract ? refracttex : reflecttex))
    {
        reflecting = refracting = false;
        return;
    }
#endif
    extern float wsx1, wsx2, wsy1, wsy2;
    int sx = 0, sy = 0, sw = size, sh = size;
#ifdef __3DS__
    if(reflectscissor)
    {
        // The projected water UVs are filtered, so retain the adjacent texels.
        sx = max(0, int(floorf((wsx1+1)*0.5f*size)) - 1);
        sy = max(0, int(floorf((wsy1+1)*0.5f*size)) - 1);
        sw = min(size, int(ceilf((wsx2+1)*0.5f*size)) + 1) - sx;
        sh = min(size, int(ceilf((wsy2+1)*0.5f*size)) + 1) - sy;
    }
    if(ac3dsTimingPhases && !refract)
    {
        const unsigned coverage = unsigned(10000 * sw * sh / (size * size));
        waterTargetTiming.minCoverage = waterTargetTiming.calls ? min(waterTargetTiming.minCoverage, coverage) : coverage;
        waterTargetTiming.maxCoverage = max(waterTargetTiming.maxCoverage, coverage);
        waterTargetTiming.coverageSum += coverage;
        ++waterTargetTiming.calls;
    }
#else
    if(reflectscissor && (wsx1 > -1 || wsy1 > -1 || wsx1 < 1 || wsy1 < 1))
    {
        sx = int(floor((wsx1+1)*0.5f*size));
        sy = int(floor((wsy1+1)*0.5f*size));
        sw = int(ceil((wsx2+1)*0.5f*size)) - sx;
        sh = int(ceil((wsy2+1)*0.5f*size)) - sy;
    }
#endif
    glScissor(sx, sy, sw, sh);
    glEnable(GL_SCISSOR_TEST);

    if(!refract) glCullFace(GL_BACK);
    glViewport(0, 0, size, size);
#ifndef __3DS__
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    resetcubes();

    render_world(camera1->o.x, camera1->o.y, refract ? camera1->o.z : hf, changelod,
            (int)camera1->yaw, (refract ? 1 : -1)*(int)camera1->pitch, dynfov(), fovy, size, size);

    setupstrips();
#endif

    glEnable(GL_TEXTURE_2D);

    glPushMatrix();
    if(!refract)
    {
        glTranslatef(0, 0, 2*hf);
        glScalef(1, 1, -1);
    }

    genclipmatrix(0, 0, -1, 0.1f*reflectclip+hf);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(clipmatrix.v);
    glMatrixMode(GL_MODELVIEW);

#ifdef __3DS__
    const float ndc = 2.f / size;
    Ac3dsPhaseTimer projectedWorld(Ac3dsPhase::ProjectedWorld);
    extern bool showmipstats;
    extern int skyclip, mergestrips, minlod;
    extern Texture *sky[6];
    WaterWorldCache &snapshot = waterWorldCache[refract];
    const bool cacheable = ac3dswaterworldcache && !(refract && ac3dssharedrefraction) && !editmode && !m_coop &&
                           !showmipstats && !ac3ds_dynamic_world_lighting() &&
                           changelod == 1.f;
    bool restored = false;
    if(cacheable)
    {
        WaterWorldKey key = {};
        glGetFloatv(GL_MODELVIEW_MATRIX, key.view);
        memcpy(key.projection, clipmatrix.v, sizeof(key.projection));
        key.skyFloor = skyfloor;
        key.horizontalFov = dynfov();
        key.verticalFov = fovy;
        key.geometry = ac3ds_world_geometry_generation();
        key.rays = ac3ds_raytable_generation();
        key.lighting = ac3ds_static_world_lighting_generation();
        key.textures = ac3ds_world_texture_generation();
        key.size = size;
        key.scissor[0] = sx; key.scissor[1] = sy;
        key.scissor[2] = sw; key.scissor[3] = sh;
        key.fogDistance = fog;
        key.fogColor = fogcolour;
        key.skyClip = skyclip;
        key.lightError = lighterror;
        key.mergeStrips = mergestrips;
        key.lod = lod_factor();
        key.minLod = minlod;
        key.uniformTextures = uniformtexres;
        key.overview = player1->isspectating() && player1->spectatemode == SM_OVERVIEW;
        extern int ac3dssharedreflection;
        key.sharedReflection = ac3dssharedreflection;
        extern int ac3dsnativereflectionclip;
        key.nativeReflectionClip = ac3dsnativereflectionclip;
        loopi(6) key.skyTextures[i] = (sky[i] ? sky[i] : notexture)->id;
        if(!snapshot.generation || memcmp(&snapshot.key, &key, sizeof(key)))
        {
            snapshot.key = key;
            if(!++snapshot.generation) ++snapshot.generation;
        }
        else restored = ac3ds_gl_restore_water_world(refract, snapshot.generation);
    }
    if(restored)
    {
        skyfloor = snapshot.skyFloorAfter;
        glColor3f(1, 1, 1);
        glDepthFunc(GL_LESS);
        glEnable(GL_FOG);
        setuptmu(0, "T * P x 2");
    }
    else
    {
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        if(refract && ac3ds_reuse_refraction_world())
        {
            if(ac3dsTimingPhases) ++sharedRefractionFrames;
        }
        else
        {
            resetcubes();
            Ac3dsPhaseTimer traversal(Ac3dsPhase::ProjectedTraversal);
            ac3ds_begin_world_pass(sx*ndc-1, sy*ndc-1, (sx+sw)*ndc-1, (sy+sh)*ndc-1);
            render_world(camera1->o.x, camera1->o.y, refract ? camera1->o.z : hf, changelod,
                         int(camera1->yaw), (refract ? 1 : -1)*int(camera1->pitch), dynfov(), fovy, size, size);
            ac3ds_end_world_pass();
            traversal.stop();
            setupstrips();
        }
        drawprojectedworld(hf, refract);
        if(cacheable && snapshot.key.textures == ac3ds_world_texture_generation())
        {
            if(ac3ds_gl_save_water_world(refract, snapshot.generation)) snapshot.skyFloorAfter = skyfloor;
        }
    }
    projectedWorld.stop();
    Ac3dsPhaseTimer projectedModels(Ac3dsPhase::ProjectedModels);
    ac3ds_begin_model_pass(sx*ndc-1, sy*ndc-1, (sx+sw)*ndc-1, (sy+sh)*ndc-1);
    ac3ds_begin_projected_mapmodels();
#else
    drawprojectedworld(hf, refract);
#endif
    rendermapmodels();
#ifdef __3DS__
    ac3ds_end_projected_mapmodels();
#endif
    renderentities();
    renderclients();
#ifdef __3DS__
    ac3ds_end_model_pass();
    projectedModels.stop();
#endif

    resettmu(0);

    render_particles(-1);

    if(refract) glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    extern int mtwater;
    if(refract && (!mtwater || maxtmus<2))
    {
        glLoadIdentity();
        glOrtho(0, 1, 0, 1, -1, 1);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4ubv(hdr.watercolor);
        glBegin(GL_TRIANGLE_STRIP);
        glVertex2f(0, 1);
        glVertex2f(1, 1);
        glVertex2f(0, 0);
        glVertex2f(1, 0);
        glEnd();
        glDisable(GL_BLEND);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_DEPTH_TEST);
    }
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    if(!refract) glCullFace(GL_FRONT);
    glViewport(0, 0, w, h);

    glDisable(GL_SCISSOR_TEST);

#ifdef __3DS__
    ac3ds_gl_render_to_screen();
#else
    glBindTexture(GL_TEXTURE_2D, refract ? refracttex : reflecttex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, sx, sy, sx, sy, sw, sh);
#endif

    glDisable(GL_TEXTURE_2D);

    reflecting = refracting = false;
}

bool minimap = false, minimapdirty = true;
int minimaplastsize = 0;
GLuint minimaptex = 0;

vector<zone> zones;

void renderzones(float z)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    loopv(zones)
    {
        glColor4f(((zones[i].color>>16)&0xFF)/255.0f, ((zones[i].color>>8)&0xFF)/255.0f, (zones[i].color&0xFF)/255.0f, 0.3f);
        glBegin(GL_QUADS);
        glVertex3f(zones[i].x1, zones[i].y1, z);
        glVertex3f(zones[i].x2, zones[i].y1, z);
        glVertex3f(zones[i].x2, zones[i].y2, z);
        glVertex3f(zones[i].x1, zones[i].y2, z);
        glEnd();
    }
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
}

void clearminimap()
{
    minimapdirty = true;
}

COMMAND(clearminimap, "");
VARFP(minimapres, 7, 9, 10, clearminimap());
#ifdef __3DS__
// World-space square the minimap texture covers, for the lower-screen radar.
bool ac3ds_minimap_info(unsigned &tex, float &x0, float &y0, float &span)
{
    if(!minimaptex || minimapdirty) return false;
    float gdim = max(clmapdims.xspan, clmapdims.yspan);
    if(gdim < 1) gdim = ssize/2.0f;
    tex = minimaptex;
    x0 = clmapdims.xm - gdim/2.0f;
    y0 = clmapdims.ym - gdim/2.0f;
    span = gdim;
    return true;
}
#endif
void drawminimap(int w, int h)
{
    if(!minimapdirty) return;
#ifdef __3DS__
    // Rendered straight into its own texture, so the screen height is no limit.
    int size = 1<<minimapres, sizelimit = min(hwtexsize, 256);
#else
    int size = 1<<minimapres, sizelimit = min(hwtexsize, min(w, h));
#endif
    while(size > sizelimit) size /= 2;
    if(size!=minimaplastsize && minimaptex)
    {
        glDeleteTextures(1, &minimaptex);
        minimaptex = 0;
    }
    if(!minimaptex)
    {
        glGenTextures(1, &minimaptex);
        createtexture(minimaptex, size, size, NULL, 3, false, false, GL_RGB);
        minimaplastsize = size;
    }
#ifdef __3DS__
    if(!ac3ds_gl_render_to_texture(minimaptex)) return;
#endif
    minimap = true;
    disableraytable();
    physent *oldcam = camera1;
    physent minicam;
    camera1 = &minicam;
    camera1->type = ENT_CAMERA;
    camera1->o.x = clmapdims.xm;
    camera1->o.y = clmapdims.ym;

    float gdim = max(clmapdims.xspan, clmapdims.yspan);

    if(gdim < 1) gdim = ssize/2.0f;
    camera1->o.z = clmapdims.maxceil + 1;
    camera1->pitch = -90;
    camera1->yaw = 0;

    float orthd = gdim/2.0f;
    glViewport(0, 0, size, size);

    glClearDepth(0.0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-orthd, orthd, -orthd, orthd, 0, (clmapdims.maxceil - clmapdims.minfloor) + 2); // depth of map +2 covered
    glScalef(1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glCullFace(GL_BACK);
    glDisable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    transplayer();
    resetcubes();
    render_world(camera1->o.x, camera1->o.y, camera1->o.z, 1.0f,
            (int)camera1->yaw, (int)camera1->pitch, 90.0f, 90.0f, size, size);
    setupstrips();
    setuptmu(0, "T * P x 2");
    glDepthFunc(GL_ALWAYS);
    renderstrips();
    glDepthFunc(GL_LESS);
    rendermapmodels();
    renderzones(clmapdims.maxceil);
    resettmu(0);
    float hf = waterlevel - 0.3f;
    renderwater(hf, 0, 0);

    //draw a black border to prevent the minimap texture edges from bleeding in radarview
    GLfloat prevLineWidth;
    glGetFloatv(GL_LINE_WIDTH, &prevLineWidth);
    glDisable(GL_BLEND);
    glColor3f(0, 0, 0);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
        glVertex3f(clmapdims.x1 + 0.02f,                   clmapdims.y1 + 0.02f,                   clmapdims.maxceil);
        glVertex3f(clmapdims.x1 + clmapdims.xspan - 0.02f, clmapdims.y1 + 0.02f,                   clmapdims.maxceil);
        glVertex3f(clmapdims.x1 + clmapdims.xspan - 0.02f, clmapdims.y1 + clmapdims.yspan - 0.02f, clmapdims.maxceil);
        glVertex3f(clmapdims.x1 + 0.02f,                   clmapdims.y1 + clmapdims.yspan - 0.02f, clmapdims.maxceil);
    glEnd();
    glLineWidth(prevLineWidth);
    glEnable(GL_BLEND);

    camera1 = oldcam;
    minimap = false;
#ifdef __3DS__
    ac3ds_gl_render_to_screen();
#else
    glBindTexture(GL_TEXTURE_2D, minimaptex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, size, size);
#endif
    minimapdirty = false;
    glCullFace(GL_FRONT);
    glEnable(GL_FOG);
    glDisable(GL_TEXTURE_2D);
    glViewport(0, 0, w, h);
    glClearDepth(1.0);
#ifdef __3DS__
    extern void ac3ds_minimap_updated();
    ac3ds_minimap_updated();
#endif
}

void cleanupgl()
{
    if(reflecttex) glDeleteTextures(1, &reflecttex);
    if(refracttex) glDeleteTextures(1, &refracttex);
    if(minimaptex) glDeleteTextures(1, &minimaptex);
    reflecttex = refracttex = minimaptex = 0;
    minimapdirty = true;
}

void drawzone(int *x1, int *x2, int *y1, int *y2, int *color)
{
    zone &newzone = zones.add();
    newzone.x1 = *x1; newzone.x2 = *x2;
    newzone.y1 = *y1; newzone.y2 = *y2;
    newzone.color = *color ? *color : 0x00FF00;
    clearminimap();
}
COMMAND(drawzone, "iiiii");

void resetzones()
{
    zones.shrink(0);
}
COMMAND(resetzones, "");

int xtraverts;

VARP(hudgun, 0, 1, 1);
VARP(specthudgun, 0, 1, 1);

void setperspective(float fovy, float aspect, float nearplane, float farplane)
{
    GLdouble ydist = nearplane * tan(fovy/2*RAD), xdist = ydist * aspect;
    if(player1->isspectating() && player1->spectatemode == SM_OVERVIEW)
    {
        int gdim = max(clmapdims.xspan, clmapdims.yspan);
        if(!gdim) gdim = ssize/2;
        int orthd = 2 + gdim/2;
        glOrtho(-orthd, orthd, -orthd, orthd, 0, (clmapdims.maxceil - clmapdims.minfloor) + 2); // depth of map +2 covered
    }
    else
    {
        glFrustum(-xdist, xdist, -ydist, ydist, nearplane, farplane);
    }
}

void sethudgunperspective(bool on)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if(on)
    {
        glScalef(1, 1, 0.5f); // fix hudugns colliding with map geometry
        setperspective(75.0f, aspect, 0.3f, farplane); // y fov fixed at 75 degrees
    }
    else setperspective(fovy, aspect, 0.15f, farplane);
    glMatrixMode(GL_MODELVIEW);
}

void drawhudgun()
{
    sethudgunperspective(true);
    
    if(hudgun && (specthudgun || !player1->isspectating()) && camera1->type==ENT_PLAYER)
    {
        playerent *p = (playerent *)camera1;
        if(p->state==CS_ALIVE) p->weaponsel->renderhudmodel();
    }
    rendermenumdl();

    sethudgunperspective(false);
}

bool outsidemap(physent *pl)
{
    if(pl->o.x < 0 || pl->o.x >= ssize || pl->o.y <0 || pl->o.y > ssize) return true;
    sqr *s = S((int)pl->o.x, (int)pl->o.y);
    return SOLID(s)
        || pl->o.z < s->floor - (s->type==FHF ? s->vdelta/4 : 0)
        || pl->o.z > s->ceil  + (s->type==CHF ? s->vdelta/4 : 0);
}

float cursordepth = 0.9f;
glmatrixf mvmatrix, projmatrix, mvpmatrix, invmvmatrix, invmvpmatrix;
vec worldpos, camdir, camup, camright;

void readmatrices()
{
    glGetFloatv(GL_MODELVIEW_MATRIX, mvmatrix.v);
    glGetFloatv(GL_PROJECTION_MATRIX, projmatrix.v);
    camright = vec(mvmatrix[0], mvmatrix[4], mvmatrix[8]);
    camup = vec(mvmatrix[1], mvmatrix[5], mvmatrix[9]);
    camdir = vec(-mvmatrix[2], -mvmatrix[6], -mvmatrix[10]);

    mvpmatrix.mul(projmatrix, mvmatrix);
    invmvmatrix.invert(mvmatrix);
    invmvpmatrix.invert(mvpmatrix);
}

// stupid function to cater to stupid ATI linux drivers that return incorrect depth values

float depthcorrect(float d)
{
    return (d<=1/256.0f) ? d*256 : d;
}

// find out the 3d target of the crosshair in the world easily and very accurately.
// sadly many very old cards and drivers appear to fuck up on glReadPixels() and give false
// coordinates, making shooting and such impossible.
// also hits map entities which is unwanted.
// could be replaced by a more acurate version of monster.cpp los() if needed

#ifdef __3DS__
static glmatrixf aimInverse;
static vec aimOrigin;
static bool aimPending = false;

static void resolveaim(float depth, vec &pos)
{
    cursordepth = depth;
    vec4 world;
    aimInverse.transform(vec(0, 0, depthcorrect(depth)*2 - 1), world);
    if(fabsf(world.w) < 1e-7f) return;
    pos = vec(world.x, world.y, world.z).div(world.w);
    intersectgeometry(aimOrigin, pos);
    if(depth == 1.0f && editmode)
    {
        vec farpoint;
        aimInverse.transform(vec(0, 0, 1), world);
        if(fabsf(world.w) > 1e-7f)
        {
            farpoint = vec(world.x, world.y, world.z).div(world.w);
            if(pos.squareddist(farpoint) > 1e-6f) cursordepth = .9999f;
        }
    }
}

void ac3ds_resolve_aim()
{
    if(!aimPending) return;
    float depth = 1.f;
    ac3ds_gl_take_aim_depth(depth);
    resolveaim(depth, worldpos);
    aimPending = false;
}
#endif

void readdepth(int w, int h, vec &pos)
{
#ifdef __3DS__
    aimInverse = invmvpmatrix;
    aimOrigin = camera1->o;
    // Keep a current geometry cursor for editor overlays. Before the next
    // simulation step, replace it with the preceding GPU center depth.
    if(editmode) resolveaim(1.f, pos);
    aimPending = ac3ds_gl_queue_aim_depth();
    if(!aimPending && !editmode) resolveaim(1.f, pos);
#else
    glReadPixels(w/2, h/2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &cursordepth);
    vec screen(0, 0, depthcorrect(cursordepth)*2 - 1);
    vec4 world;
    invmvpmatrix.transform(screen, world);
    pos = vec(world.x, world.y, world.z).div(world.w);
    intersectgeometry(camera1->o, pos);
#endif
}

VARP(ignoreoverride_nowaterreflect, 0, 0, 1);
VARP(ignoreoverride_nostencilshadows, 0, 0, 1);

int effective_stencilshadow = 0;

void gl_drawframe(int w, int h, float changelod, float curfps, int elapsed)
{
#ifdef __3DS__
    Ac3dsPhaseTimer draw(Ac3dsPhase::Draw);
#endif
    extern int mapoverride_nostencilshadows, mapoverride_nowaterreflect;
    effective_stencilshadow = mapoverride_nostencilshadows && !ignoreoverride_nostencilshadows ? 0 : stencilshadow;
    bool effective_waterreflect = waterreflect && !editmode && (!mapoverride_nowaterreflect || ignoreoverride_nowaterreflect);

#ifdef __3DS__
    Ac3dsPhaseTimer lights(Ac3dsPhase::DynamicLights);
#endif
    dodynlights();
#ifdef __3DS__
    lights.stop();
#endif
    drawminimap(w, h);

#ifdef __3DS__
    Ac3dsPhaseTimer camera(Ac3dsPhase::Camera);
#endif
    recomputecamera();
#ifdef __3DS__
    camera.stop();
#endif

    aspect = float(w)/h;
    fovy = 2*atan2(tan(float(dynfov())/2*RAD), aspect)/RAD;

    float hf = waterlevel - 0.3f;
    bool underwater = camera1->o.z<hf;

    glFogi(GL_FOG_START, (fog+64)/8);
    glFogi(GL_FOG_END, fog);
    float fogc[4] = { (fogcolour>>16)/256.0f, ((fogcolour>>8)&255)/256.0f, (fogcolour&255)/256.0f, 1.0f },
          wfogc[4] = { hdr.watercolor[0]/255.0f, hdr.watercolor[1]/255.0f, hdr.watercolor[2]/255.0f, 1.0f };
    glFogfv(GL_FOG_COLOR, fogc);
    glClearColor(fogc[0], fogc[1], fogc[2], 1.0f);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    if(underwater)
    {
        fovy += sinf(lastmillis/1000.0f)*2.0f;
        aspect += sinf(lastmillis/1000.0f+PI)*0.1f;
        glFogfv(GL_FOG_COLOR, wfogc);
        glFogi(GL_FOG_START, 0);
        glFogi(GL_FOG_END, (fog+96)/8);
    }

    farplane = fog*5/2;
    setperspective(fovy, aspect, 0.15f, farplane);
    glMatrixMode(GL_MODELVIEW);

    transplayer();
    readmatrices();

#ifndef __3DS__
    if(!underwater && effective_waterreflect)
    {
        extern int wx1;
        if(wx1>=0)
        {
            if(reflectscissor) calcwaterscissor();
            drawreflection(hf, w, h, changelod, false);
            if(waterrefract) drawreflection(hf, w, h, changelod, true);
        }
    }
#endif

    if(effective_stencilshadow && hasstencil && stencilbits >= 8) glClearStencil((hasSTS || hasST2) && !hasSTW ? 128 : 0);
    glClear((outsidemap(camera1) ? GL_COLOR_BUFFER_BIT : 0) | GL_DEPTH_BUFFER_BIT | (effective_stencilshadow && hasstencil && stencilbits >= 8 ? GL_STENCIL_BUFFER_BIT : 0));

    glEnable(GL_TEXTURE_2D);

    resetcubes();

#ifdef __3DS__
    Ac3dsPhaseTimer worldPhase(Ac3dsPhase::World);
    Ac3dsPhaseTimer traversal(Ac3dsPhase::WorldTraversal);
#endif
    render_world(camera1->o.x, camera1->o.y, camera1->o.z, changelod,
            (int)camera1->yaw, (int)camera1->pitch, dynfov(), fovy, w, h);
#ifdef __3DS__
    traversal.stop();
#endif

    setupstrips();

#ifdef __3DS__
    if(ac3dssharedrefraction && !underwater && effective_waterreflect && waterrefract)
        ac3ds_hold_refraction_world();
#endif
    renderstripssky();

    glLoadIdentity();
    glRotatef(camera1->pitch, -1, 0, 0);
    glRotatef(camera1->yaw,   0, 1, 0);
    glRotatef(90, 1, 0, 0);
    glColor3f(1, 1, 1);
    glDisable(GL_FOG);
    glDepthFunc(GL_GREATER);
    draw_envbox(fog*4/3);
    glDepthFunc(GL_LESS);
    fixresizedscreen();
    glEnable(GL_FOG);

    transplayer();

    setuptmu(0, "T * P x 2");

    renderstrips();

#ifdef __3DS__
    worldPhase.stop();
    extern vector<vertex> verts;
    const int mainWorldVertices = verts.length();
    const bool projectWater = !underwater && effective_waterreflect && ac3ds_water_bounds(hf);
    if(projectWater) ac3ds_begin_scene_models();
    if(projectWater)
    {
        // The main world established this frame's water bounds. Switch to the
        // texture targets after its opaque draws have been queued.
        const int mainQuads = nquads;
        const float mainSkyFloor = skyfloor;
        resettmu(0);
        drawreflection(hf, w, h, 1.f, false);
        if(waterrefract) drawreflection(hf, w, h, 1.f, true);
        nquads = mainQuads;
        skyfloor = mainSkyFloor;
        glEnable(GL_TEXTURE_2D);
        setuptmu(0, "T * P x 2");
    }
    ac3ds_release_refraction_world();
    Ac3dsPhaseTimer modelsPhase(Ac3dsPhase::Models);
#endif

    xtraverts = 0;

#ifdef __3DS__
    ac3ds_begin_model_pass(-1, -1, 1, 1);
#endif
    startmodelbatches();
    rendermapmodels();
    endmodelbatches();
#ifdef __3DS__
    ac3ds_end_model_pass();
#endif

#ifdef __3DS__
    modelsPhase.stop();
    Ac3dsPhaseTimer shadowsPhase(Ac3dsPhase::Shadows);
#endif
    if(effective_stencilshadow && hasstencil && stencilbits >= 8) drawstencilshadows();
#ifdef __3DS__
    shadowsPhase.stop();
#endif

    startmodelbatches();
    rendereditentities();
    endmodelbatches();

    readdepth(w, h, worldpos);

    startmodelbatches();
    renderclients();
    endmodelbatches();

    startmodelbatches();
#ifdef __3DS__
    if(projectWater) ac3ds_begin_pickup_model_pass();
#endif
    renderentities();
    renderbounceents();
    endmodelbatches();
#ifdef __3DS__
    if(projectWater)
    {
        ac3ds_end_model_pass();
        ac3ds_end_scene_models();
    }
#endif

    WaypointClass.Think();// Rick: Need to do here because of drawing the waypoints
    
    drawhudgun();

    resettmu(0);

    glDisable(GL_CULL_FACE);

    render_particles(curtime, PT_DECAL_MASK);

    GLuint waterreflection = !effective_waterreflect || underwater ? 0 : reflecttex;
    GLuint waterrefraction = !effective_waterreflect || !waterrefract || underwater ? 0 : refracttex;
#ifdef __3DS__
    if(!projectWater || !ac3ds_gl_texture_ready(waterreflection)) waterreflection = 0;
    if(!projectWater || !ac3ds_gl_texture_ready(waterrefraction)) waterrefraction = 0;
#endif
#ifdef __3DS__
    Ac3dsPhaseTimer waterPhase(Ac3dsPhase::Water);
#endif
    int nquads = renderwater(hf, waterreflection, waterrefraction);
#ifdef __3DS__
    waterPhase.stop();
#endif

    render_particles(curtime, ~PT_DECAL_MASK);

    glDisable(GL_FOG);
    glDisable(GL_TEXTURE_2D);

    if(editmode)
    {
        if(cursordepth==1.0f) worldpos = camera1->o;
        enablepolygonoffset(GL_POLYGON_OFFSET_LINE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDepthMask(GL_FALSE);
        cursorupdate();
        glDepthMask(GL_TRUE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        disablepolygonoffset(GL_POLYGON_OFFSET_LINE, false);
    }
    extern vector<int> tagclipcubes;
    tagclipcubes.setsize(0);

#ifdef __3DS__
    Ac3dsPhaseTimer hud(Ac3dsPhase::Hud);
    gl_drawhud(w, h, int(curfps + 0.5f), nquads, mainWorldVertices, underwater, elapsed);
    hud.stop();
#else
    extern vector<vertex> verts;
    gl_drawhud(w, h, int(curfps + 0.5f), nquads, verts.length(), underwater, elapsed);
#endif

    glEnable(GL_CULL_FACE);
    glEnable(GL_FOG);

#ifdef __3DS__
    Ac3dsPhaseTimer restoreLights(Ac3dsPhase::DynamicLights);
#endif
    undodynlights();
}


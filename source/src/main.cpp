// main.cpp: initialisation & main loop

#include "cube.h"

#ifdef __3DS__
#include "system.h"
#include "audio_3ds.h"
#include "performance.h"
#include "crash_3ds.h"
static bool native_graphics = false, native_network = false, native_dedicated = false;
#endif

void cleanup(char *msg)         // single program exit point;
{
#ifdef __3DS__
    ac3ds_crash_stop_watchdog();
#endif
    if(clientlogfile) clientlogfile->fflush();
    if(!msg)
    {
#ifdef __3DS__
        if(!native_dedicated)
#endif
        {
            cleanupclient();
            audiomgr.soundcleanup();
        }
        cleanupserver();
    }
#ifndef __3DS__
    SDL_ShowCursor(SDL_TRUE);
    SDL_SetRelativeMouseMode(SDL_FALSE);
#endif
    if(msg)
    {
        #ifdef WIN32
        MessageBox(NULL, msg, "AssaultCube fatal error", MB_OK|MB_SYSTEMMODAL|MB_ICONERROR);
        #else
        printf("%s", msg);
        #endif
        if(clientlogfile)
        {
            clientlogfile->printf("%s\n", msg);
            dumpexecutionstack(clientlogfile);
        }
    }
#ifdef __3DS__
    extern void ac3ds_close_network();
    ac3ds_close_network();
    if(native_network) enet_deinitialize();
    native_network = false;
    ac3ds_async_sounds_shutdown(); // also on fatal paths that skip soundcleanup
    ac3ds_audio_shutdown();
    if(native_graphics)
    {
        ac3ds_gl_shutdown();
        native_graphics = false;
    }
    if(msg) ac3ds_show_error(msg);
    ac3ds_system_shutdown();
#endif
    SDL_Quit();
}

VAR(resetcfg, 0, 0, 1);

#ifdef __3DS__
int ac3ds_load_generation = 0;
// True for frames 1-6, 300 and 900 after each map load.
static bool ac3ds_frames_after_load(int frame)
{
    static int seen = 0, loadframe = 0;
    if(seen != ac3ds_load_generation) { seen = ac3ds_load_generation; loadframe = frame; }
    if(!seen) return false;
    const int n = frame - loadframe;
    return (n >= 1 && n <= 6) || n == 300 || n == 900;
}
#endif

void quit()                     // normal exit
{
    if(clientlogfile) clientlogfile->fflush();
    const char *onquit = getalias("onQuit");
    setcontext("hook", "onQuit");
    if(onquit && onquit[0]) execute(onquit);
    resetcontext();
    alias("onQuit", "");
    alias("mapstartonce", "");
    extern void writeinitcfg();
    writeinitcfg();
    writeservercfg();
    writepcksourcecfg();
    if(resetcfg) deletecfg();
    else writecfg();
    savehistory();
    writemapmodelattributes();
    entropy_save();
    writeallxmaps();
    cleanup(NULL);
    popscontext();
    DELETEP(clientlogfile);
    exit(EXIT_SUCCESS);
}
COMMAND(quit, "");

void fatal(const char *s, ...)    // failure exit
{
    static int errors = 0;
    errors++;

    if(errors <= 2)
    {
        defvformatstring(msg,s,s);
        if(errors <= 1)
        {
            defvformatstring(msg,s,s);
            defformatstring(msgerr)("%s (%s)\n", msg, SDL_GetError());
            cleanup(msgerr);
        }
        else puts(msg);
    }
    DELETEP(clientlogfile);
    exit(EXIT_FAILURE);
}

static int initing = NOT_INITING;
static bool restoredinits = false;

bool initwarning(const char *desc, int level, int type)
{
    if(initing < level)
    {
        addchange(desc, type);
        return true;
    }
    return false;
}

SDL_Window *screen = NULL;
SDL_GLContext glcontext = 0;

VAR(screenw, 1, 0, 0);   // actual current resolution of screen/window
VAR(screenh, 1, 0, 0);
#define SCR_MINW 320
#define SCR_MINH 200
#define SCR_MAXW 10000
#define SCR_MAXH 10000
VARF(scr_w, SCR_MINW, -1, SCR_MAXW, initwarning("screen resolution")); // screen or window resolution, except for "fullscreen desktop" mode
VARF(scr_h, SCR_MINH, -1, SCR_MAXH, initwarning("screen resolution"));
VARF(depthbits, 0, 0, 32, initwarning("depth-buffer precision"));
#ifdef __3DS__
VARF(stencilbits, 0, 8, 8, initwarning("stencil-buffer precision"));
#else
VARF(stencilbits, 0, 0, 32, initwarning("stencil-buffer precision"));
#endif
VARF(fsaa, -1, -1, 16, initwarning("anti-aliasing"));
VARF(vsync, -1, -1, 1, initwarning("vertical sync"));

VAR(desktopw, 1, 0, 0); // resolution of desktop (assumed to be maximum resolution)
VAR(desktoph, 1, 0, 0);

#ifdef WIN32
// SDL_WarpMouseInWindow behaves erratically on Windows, so force relative mouse instead.
VARN(relativemouse, userelativemouse, 1, 1, 0);
#else
VARNP(relativemouse, userelativemouse, 0, 1, 1);
#endif

static bool shouldgrab = false, grabinput = false, minimized = false, centerwindow = true, canrelativemouse = true, relativemouse = false;

#ifdef SDL_VIDEO_DRIVER_X11
VAR(sdl_xgrab_bug, 0, 0, 1);
#endif

void inputgrab(bool on, bool delay = false)
{
#ifdef __3DS__
    shouldgrab = delay;
    relativemouse = on;
#else
#ifdef SDL_VIDEO_DRIVER_X11
    bool wasrelativemouse = relativemouse;
#endif
    if (on)
    {
        SDL_ShowCursor(SDL_FALSE);
        if (canrelativemouse && userelativemouse)
        {
            if (SDL_SetRelativeMouseMode(SDL_TRUE) >= 0)
            {
                SDL_SetWindowGrab(screen, SDL_TRUE);
                relativemouse = true;
            }
            else
            {
                SDL_SetWindowGrab(screen, SDL_FALSE);
                canrelativemouse = false;
                relativemouse = false;
            }
        }
    }
    else
    {
        SDL_ShowCursor(SDL_TRUE);
        if (relativemouse)
        {
            SDL_SetWindowGrab(screen, SDL_FALSE);
            SDL_SetRelativeMouseMode(SDL_FALSE);
            relativemouse = false;
        }
    }
    shouldgrab = delay;

#ifdef SDL_VIDEO_DRIVER_X11
    if ((relativemouse || wasrelativemouse) && sdl_xgrab_bug)
    {
        // Workaround for buggy SDL X11 pointer grabbing
        union { SDL_SysWMinfo info; uchar buf[sizeof(SDL_SysWMinfo) + 128]; };
        SDL_GetVersion(&info.version);
        if (SDL_GetWindowWMInfo(screen, &info) && info.subsystem == SDL_SYSWM_X11)
        {
            if (relativemouse)
            {
                uint mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask;
                XGrabPointer(info.info.x11.display, info.info.x11.window, True, mask, GrabModeAsync, GrabModeAsync, info.info.x11.window, None, CurrentTime);
            }
            else XUngrabPointer(info.info.x11.display, CurrentTime);
        }
    }
#endif
#endif
}


void setfullscreen(bool enable)
{
    if(!screen) return;
    if(enable == !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
    {
        extern int fullscreendesktop;
        SDL_SetWindowFullscreen(screen, enable ? (fullscreendesktop ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN) : 0);
        inputgrab(grabinput = enable || grabinput);
        if(!enable) SDL_SetWindowSize(screen, scr_w, scr_h);
        if(!enable && centerwindow) SDL_SetWindowPosition(screen, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

VARF(fullscreen, 0, 1, 1, setfullscreen(fullscreen != 0));

void resetfullscreen()
{
    setfullscreen(false);
    setfullscreen(true);
}

VARF(fullscreendesktop, 0, 1, 1, if(fullscreen) resetfullscreen());

SVARFP(lang, "en", filterlang(lang, lang));

void writeinitcfg()
{
    if(!restoredinits) return;
#ifdef __3DS__
    stream *f = ac3ds_config_buffer();
#else
    filerotate("config/init", "cfg", CONFIGROTATEMAX); // keep five old init config sets
    stream *f = openfile(path("config/init.cfg", true), "w");
    if(!f) return;
#endif
    f->printf("// automatically written on exit, DO NOT MODIFY\n// modify settings in game\n");
    f->printf("fullscreen %d\n", fullscreen);
    f->printf("fullscreendesktop %d\n", fullscreendesktop);
    f->printf("scr_w %d\n", scr_w);
    f->printf("scr_h %d\n", scr_h);
    f->printf("depthbits %d\n", depthbits);
    f->printf("stencilbits %d\n", stencilbits);
    f->printf("fsaa %d\n", fsaa);
    f->printf("vsync %d\n", vsync);
    extern int audio, soundchannels, igraphdefaultframetime;
    f->printf("audio %d\n", audio > 0 ? 1 : 0);
    f->printf("soundchannels %d\n", soundchannels);
    f->printf("igraphdefaultframetime %d\n", igraphdefaultframetime);
    if(lang && *lang) f->printf("lang %s\n", lang);
    extern void writezipmodconfig(stream *f);
    writezipmodconfig(f);
#ifdef __3DS__
    if(!ac3ds_install_config("config/init", f)) conoutf("Could not save config/init.cfg");
#endif
    delete f;
}

#if 0
VARP(highprocesspriority, 0, 1, 1);

void setprocesspriority(bool high)
{
#if defined(WIN32) && !defined(_DEBUG)
    SetPriorityClass(GetCurrentProcess(), high && highprocesspriority && fullscreen ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
#endif
}
#endif

VARP(pngcompress, 0, 9, 9);

void writepngchunk(stream *f, const char *type, const void *data = NULL, uint len = 0)
{
    f->putbig<uint>(len);
    f->write(type, 4);
    if(data) f->write(data, len);

    uint crc = crc32(0, Z_NULL, 0);
    crc = ~crc32(crc, (const Bytef *)type, 4);
    if(data) loopi(len) enet_crc32_inc(&crc, ((const uchar *)data)[i]);
    f->putbig<uint>(~crc);
}

int save_png(const char *filename, SDL_Surface *image)
{
    uchar *data = (uchar *)image->pixels;
    int iw = image->w, ih = image->h, pitch = image->pitch;

    stream *f = openfile(filename, "wb");
    if(!f) { conoutf("could not write to %s", filename); return -1; }
    const char *scores = asciiscores(true);

    uchar signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    f->write(signature, sizeof(signature));

    struct pngihdr
    {
        uint width, height;
        uchar bitdepth, colortype, compress, filter, interlace;
    } ihdr = { bigswap<uint>(iw), bigswap<uint>(ih), 8, 2, 0, 0, 0 };
    writepngchunk(f, "IHDR", (uchar *)&ihdr, 13);

    int idat = f->tell();
    uint len = 0;
    f->write("\0\0\0\0IDAT", 8);
    uint crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)"IDAT", 4);

    z_stream z;
    z.zalloc = NULL;
    z.zfree = NULL;
    z.opaque = NULL;

    if(deflateInit(&z, pngcompress) != Z_OK)
        goto error;

    uchar buf[1<<12];
    z.next_out = (Bytef *)buf;
    z.avail_out = sizeof(buf);

    loopi(ih)
    {
        uchar filter = 0;
        loopj(2)
        {
            z.next_in = j ? (Bytef *)data + i*pitch : (Bytef *)&filter;
            z.avail_in = j ? iw*3 : 1;
            while(z.avail_in > 0)
            {
                if(deflate(&z, Z_NO_FLUSH) != Z_OK) goto cleanuperror;
                #define FLUSHZ do { \
                    int flush = sizeof(buf) - z.avail_out; \
                    crc = crc32(crc, buf, flush); \
                    len += flush; \
                    f->write(buf, flush); \
                    z.next_out = (Bytef *)buf; \
                    z.avail_out = sizeof(buf); \
                } while(0)
                FLUSHZ;
            }
        }
    }

    for(;;)
    {
        int err = deflate(&z, Z_FINISH);
        if(err != Z_OK && err != Z_STREAM_END) goto cleanuperror;
        FLUSHZ;
        if(err == Z_STREAM_END) break;
    }

    deflateEnd(&z);

    f->seek(idat, SEEK_SET);
    f->putbig<uint>(len);
    f->seek(0, SEEK_END);
    f->putbig<uint>(crc);

    writepngchunk(f, "tEXt", "\123o\165rc\145\0\101ss\141" "\165\154\164\103\165\142\145", 18);
    writepngchunk(f, "tEXt", scores - 8, strlen(scores) + 8);
    writepngchunk(f, "IEND");

    delete f;
    return 0;

cleanuperror:
    deflateEnd(&z);

error:
    delete f;

    return -1;
}

#include "jpegenc.h"

void mapscreenshot(const char *imagepath, bool mapshot, int fileformat, float scale, int height, int quality)
{
    extern int minimaplastsize;
    int src_w = mapshot ? minimaplastsize : screenw;
    int src_h = mapshot ? minimaplastsize : screenh;
    int dst_w = mapshot ? src_w : src_w * scale, img_w = dst_w;
    int dst_h = mapshot ? src_h : src_h * scale, img_h = dst_h;
    if(height)
    { // create 4:3 of exact height
        if(dst_w >= (dst_h * 4) / 3)
        { // 4:3 or wider: crop width
            dst_w = (src_w * height) / src_h;
            img_h = dst_h = height;
            img_w = (img_h * 4) / 3;
        }
        else
        { // probably 5:4: crop height
            img_h = height;
            dst_w = img_w = (img_h * 4) / 3;
            dst_h = (src_h * dst_w) / src_w;
        }
    }

    SDL_Surface *image = creatergbsurface(img_w, img_h);
    if(!image) return;

    int tmpdstsize = dst_w * dst_h * 3, tmpdstpitch = dst_w * 3;
    uchar *tmpdst = new uchar[tmpdstsize], *dst = (uchar *)image->pixels;

    if(mapshot)
    {
        extern GLuint minimaptex;
        if(minimaptex)
        {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glBindTexture(GL_TEXTURE_2D, minimaptex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, tmpdst);
#ifdef __3DS__
            if(!ac3ds_gl_read_ok())
            {
                SDL_FreeSurface(image);
                delete[] tmpdst;
                conoutf("Could not read the map preview");
                return;
            }
#endif
        }
        else
        {
            conoutf("no mapshot prepared!");
            SDL_FreeSurface(image);
            delete[] tmpdst;
            return;
        }
        loopi(img_h)
        { // copy image
            memcpy(dst, &tmpdst[tmpdstpitch * i], img_w * 3);
            dst += image->pitch;
        }
    }
    else
    {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if(src_w != dst_w || src_h != dst_h)
        { // scale image
            uchar *tmpsrc = new uchar[src_w * src_h * 3];
            glReadPixels(0, 0, src_w, src_h, GL_RGB, GL_UNSIGNED_BYTE, tmpsrc);
#ifdef __3DS__
            if(!ac3ds_gl_read_ok())
            {
                SDL_FreeSurface(image);
                delete[] tmpsrc;
                delete[] tmpdst;
                conoutf("Could not read the screen");
                return;
            }
#endif
            scaletexture(tmpsrc, src_w, src_h, 3, tmpdst, dst_w, dst_h);
            delete[] tmpsrc;
        }
        else
        {
            glReadPixels(0, 0, dst_w, dst_h, GL_RGB, GL_UNSIGNED_BYTE, tmpdst);
#ifdef __3DS__
            if(!ac3ds_gl_read_ok())
            {
                SDL_FreeSurface(image);
                delete[] tmpdst;
                conoutf("Could not read the screen");
                return;
            }
#endif
        }
        int crop_w = height ? ((dst_w - img_w) / 2) * 3 : 0;
        int crop_h = height ? (dst_h - img_h) / 2 : 0;
        loopirev(img_h)
        { // flip image
            memcpy(dst, &tmpdst[tmpdstpitch * (i + crop_h) + crop_w], img_w * 3);
            dst += image->pitch;
        }
    }
    entropy_add_block(tmpdst, tmpdstsize);
    delete[] tmpdst;
    if(!mapshot && imagepath) audiomgr.playsound(S_CAMERA);

    switch(fileformat)
    {
        case 0: // bmp
        {
            stream *file = openfile(imagepath, "wb");
            if(!file) conoutf("failed to create: %s", imagepath);
            else
            {
                SDL_SaveBMP_RW(image, file->rwops(), 1);
                delete file;
            }
            break;
        }
        case 1: // jpeg
        {
            jpegenc jpegencoder;
            jpegencoder.encode(imagepath, image, quality, asciiscores(true));
            break;
        }
        case 2: // png
            if(save_png(imagepath, image) < 0) conoutf("\f3Error saving png file");
            break;
    }

    SDL_FreeSurface(image);
}

FVARP(screenshotscale, 0.1f, 1.0f, 1.0f);
VARP(jpegquality, 10, 85, 100);  // best: 100 - good: 85 [default] - bad: 70 - terrible: 50
VARP(screenshottype, 0, 1, 2);

const char *getscrext()
{
    const char *screxts[] = { ".bmp", ".jpg", ".png" };
    return screxts[screenshottype % 3];
}
COMMANDF(getscrext, "", () { result(getscrext()); });

void screenshot(const char *filename)
{
    static string buf;
    if(filename && filename[0]) formatstring(buf)("screenshots/%s%s", filename, getscrext());
    else if(getclientmap()[0]) formatstring(buf)("screenshots/%s_%s_%s%s", timestring(), behindpath(getclientmap()), modestr(gamemode, true), getscrext());
    else formatstring(buf)("screenshots/%s%s", timestring(), getscrext());
    path(buf);
    mapscreenshot(buf, false, screenshottype, screenshotscale, 0, jpegquality);
}
COMMAND(screenshot, "s");

void mapshot()
{
    defformatstring(buf)("screenshots" PATHDIVS "mapshot_%s_%s%s", behindpath(getclientmap()), timestring(), getscrext());
    mapscreenshot(buf, true, screenshottype, screenshotscale, 0, jpegquality);
}
COMMAND(mapshot, "");

void screenshotpreview(int *res)
{
    static int lastres = 240;
    defformatstring(buf)("packages" PATHDIVS "maps" PATHDIVS "%spreview" PATHDIVS "%s.jpg", securemapcheck(getclientmap(), false) ? "official" PATHDIVS : "", behindpath(getclientmap()));
    mapscreenshot(buf, false, 1, 1.0f, (lastres = clamp(((*res ? *res : lastres) / 48) * 48, 144, 480)), 80);
    reloadtexture(*textureload(buf, 3));
}
COMMAND(screenshotpreview, "i");

bool needsautoscreenshot = false;

void updatescreensize()
{
#ifdef __3DS__
    screenw = AC3DS_WIDTH;
    screenh = AC3DS_HEIGHT;
#else
    SDL_GetWindowSize(screen, &screenw, &screenh);
    if(screenh < 1) screenh = 1;
#endif
    VIRTW = screenw * VIRTH / screenh;
    glViewport(0, 0, screenw, screenh);
}

void screenres(int w, int h)
{
    if(!screen) return;
    scr_w = clamp(w, SCR_MINW, SCR_MAXW);
    scr_h = clamp(h, SCR_MINH, SCR_MAXH);
    if(fullscreendesktop)
    {
        scr_w = min(scr_w, desktopw);
        scr_h = min(scr_h, desktoph);
    }
    if(!(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(screen, scr_w, scr_h);
    else if(!fullscreendesktop) initwarning("screen resolution");
}
#if defined(WIN32) || defined(__APPLE__) || !defined(WIN32)
void setresdata(char *s, enet_uint32 c)
{
    extern hashtable<char *, enet_uint32> &resdata;
    resdata[newstring(s)] = c;
}
#endif

COMMANDF(screenres, "ii", (int *w, int *h) { screenres(*w, *h); });

int setgamma(int newgamma) // replacement for SDL_SetGamma
{
#ifdef __3DS__
    return ac3ds_set_brightness(newgamma);
#else
    return screen ? SDL_SetWindowBrightness(screen, newgamma/100.0f) : -1;
#endif
}

static int curgamma = 100;

VARNFP(gamma, vgamma, 30, 100, 300,
{
    if(vgamma != curgamma)
    {
        if(setgamma(vgamma) == -1) conoutf("Could not set gamma: %s", SDL_GetError());
        curgamma = vgamma;
    }
});

void restoregamma()
{
    if(curgamma != 100)
    {
        setgamma(100);
        setgamma(curgamma);
    }
}

void getdisplayresolutions()
{
#ifdef __3DS__
    defformatstring(res)("%d %d", AC3DS_WIDTH, AC3DS_HEIGHT);
#else
    string res = "";
    int lh = 0, lw = 0, n = SDL_GetNumDisplayModes(0);
    SDL_DisplayMode mode;
    loopi(n)
    {
        if(SDL_GetDisplayMode(0, i, &mode) == 0 && lw != mode.w && lh != mode.h) concatformatstring(res, "%s%d %d", *res ? " " : "", (lw = mode.w), (lh = mode.h));
    }
#endif
    result(res);
}
COMMAND(getdisplayresolutions, "");

void setupscreen(int &useddepthbits, int &usedfsaa)
{
#ifdef __3DS__
    if(native_graphics) ac3ds_gl_shutdown();
    native_graphics = ac3ds_gl_init();
    if(!native_graphics) fatal("Could not initialize the PICA200 renderer");
    desktopw = scr_w = AC3DS_WIDTH;
    desktoph = scr_h = AC3DS_HEIGHT;
    useddepthbits = 24;
    usedfsaa = 0;
    hasstencil = stencilbits >= 8;
    updatescreensize();
    inputgrab(grabinput = true);
#else
    if(glcontext)
    {
        SDL_GL_DeleteContext(glcontext);
        glcontext = NULL;
    }
    if(screen)
    {
        SDL_DestroyWindow(screen);
        screen = NULL;
    }

    SDL_Rect desktop;
    if(SDL_GetDisplayBounds(0, &desktop) < 0) fatal("failed querying desktop bounds: %s", SDL_GetError());
    desktopw = desktop.w;
    desktoph = desktop.h;
    if(scr_w < 0 || scr_h < 0) { scr_w = desktopw; scr_h = desktoph; } // first run: set to fullscreen res

    int modes = SDL_GetNumDisplayModes(0);
    if(modes >= 1)
    {
        bool hasmode = false;
        SDL_DisplayMode mode;
        loopi(modes)
        {
            if(SDL_GetDisplayMode(0, i, &mode) == 0 && scr_w <= mode.w && scr_h <= mode.h) { hasmode = true; break; }
        }
        if(!hasmode && SDL_GetDisplayMode(0, 0, &mode) == 0) { scr_w = mode.w; scr_h = mode.h; }

    }
    int winw = fullscreen && fullscreendesktop ? desktopw : scr_w;
    int winh = fullscreen && fullscreendesktop ? desktoph : scr_h;

    int flags = SDL_WINDOW_RESIZABLE;
    if(fullscreen) flags |= fullscreendesktop ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;

    static int configs[] =
    {
        0x7, /* try everything */
        0x6, 0x5, 0x3, /* try disabling one at a time */
        0x4, 0x2, 0x1, /* try disabling two at a time */
        0 /* try disabling everything */
    };
    int config = 0;
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    if(!depthbits) SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    if(!fsaa)
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }
    loopi(sizeof(configs)/sizeof(configs[0]))
    {
        config = configs[i];
        if(!depthbits && config&1) continue;
        if(!stencilbits && config&2) continue;
        if(fsaa<=0 && config&4) continue;
        if(depthbits) SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, config&1 ? depthbits : 16);
        if(stencilbits)
        {
            SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, config&2 ? stencilbits : 0);
            hasstencil = (config&2)!=0;
        }
        else hasstencil = false;
        if(fsaa>0)
        {
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, config&4 ? 1 : 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, config&4 ? fsaa : 0);
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        screen = SDL_CreateWindow("AssaultCube",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            winw, winh,
            SDL_WINDOW_OPENGL | flags);
        if(screen)
        {
            glcontext = SDL_GL_CreateContext(screen);
            if(glcontext) break;
            SDL_DestroyWindow(screen);
        }
    }
    if(!screen) fatal("Unable to create OpenGL screen");
    else
    {
        if(depthbits && (config&1)==0) conoutf("%d bit z-buffer not supported - disabling", depthbits);
        if(stencilbits && (config&2)==0) conoutf("%d bit stencil buffer not supported - disabling", stencilbits);
        if(fsaa>0 && (config&4)==0) conoutf("%dx anti-aliasing not supported - disabling", fsaa);
    }

    if(vsync>=0) SDL_GL_SetSwapInterval(vsync);

    updatescreensize();

    inputgrab(grabinput = fullscreen ? true : false);

    useddepthbits = config&1 ? depthbits : 0;
    usedfsaa = config&2 ? fsaa : 0;
#endif
}

void swapbuffers()
{
#ifdef __3DS__
    // The minimap readback must be queued inside the open GPU frame; the
    // lower-screen drawing is pure CPU work, so it runs after submission while
    // the GPU renders the top screen.
    ac3ds_service_controls();
    ac3ds_gl_end_frame();
    ac3ds_draw_controls();
#else
    SDL_GL_SwapWindow(screen);
#endif
}

extern int hirestextures;

void resetgl()
{
    clearchanges(CHANGE_GFX);
#ifdef __3DS__
    ac3ds_loading_screen("Applying settings", "Reloading textures and models", 0.05f);
    struct LoadingDone { ~LoadingDone() { ac3ds_loading_done(); } } loadingdone;
#endif

    loadingscreen();
#ifdef __3DS__
    ac3ds_reset_pixel_font();
#endif

    extern void cleanupparticles();
    extern void cleanupmodels();
    extern void cleanuptextures();
    extern void cleanuptmus();
    extern void cleanupgl();
    cleanupparticles();
    cleanupmodels();
    cleanuptextures();
    cleanuptmus();
    cleanupgl();
    uniformtexres = !hirestextures;
    c2skeepalive();

    int useddepthbits = 0, usedfsaa = 0;
    setupscreen(useddepthbits, usedfsaa);
    gl_init(screenw, screenh, useddepthbits, usedfsaa);

    extern void reloadfonts();
    extern Texture *startscreen;
    c2skeepalive();
    if(!reloadtexture(*notexture) ||
       !reloadtexture(*startscreen))
        fatal("failed to reload core texture");
    loadingscreen();
    c2skeepalive();
    restoregamma();
    c2skeepalive();
    reloadfonts();
    reloadtextures();
    c2skeepalive();
    drawscope(true);
    preload_playermodels();
    c2skeepalive();
    preload_hudguns();
    c2skeepalive();
    preload_entmodels();
    c2skeepalive();
    preload_mapmodels();
    c2skeepalive();
}

COMMAND(resetgl, "");

#ifdef __3DS__
VARFP(maxfps, 0, 60, 60, if(maxfps && maxfps < 25) maxfps = 25);
#else
VARFP(maxfps, 0, 200, 1000, if(maxfps && maxfps < 25) maxfps = 25);
#endif

void limitfps(int &millis, int curmillis)
{
    if(!maxfps) return;
#ifdef __3DS__
    if(maxfps >= 60) return;
#endif
    static int fpserror = 0;
    int delay = 1000/maxfps - (millis-curmillis);
    if(delay < 0) fpserror = 0;
    else
    {
        fpserror += 1000%maxfps;
        if(fpserror >= maxfps)
        {
            ++delay;
            fpserror -= maxfps;
        }
        if(delay > 0)
        {
#ifdef __3DS__
            Ac3dsPhaseScope phase("frame limiter: SDL delay");
#endif
            SDL_Delay(delay);
            millis += delay;
        }
    }
}

int lowfps = 30, highfps = 40;

void fpsrange(int *low, int *high)
{
    if(*low>*high || *low<1) return;
    lowfps = *low;
    highfps = *high;
}

COMMAND(fpsrange, "ii");

int keyrepeatmask = 0, textinputmask = 0; // bunches of flags for the same functions
Uint32 textinputtime = 0;

void keyrepeat(bool on, int mask)
{
    if(on) keyrepeatmask |= mask;
    else keyrepeatmask &= ~mask;
}

void textinput(bool on, int mask)
{
    if(on)
    {
        if(!textinputmask)
        {
#ifndef __3DS__
            SDL_StartTextInput();
#endif
            textinputtime = SDL_GetTicks();
        }
        textinputmask |= mask;
    }
    else
    {
        textinputmask &= ~mask;
#ifndef __3DS__
        if(!textinputmask) SDL_StopTextInput();
#endif
    }
}

vector<SDL_Event> events;

void pushevent(const SDL_Event &e)
{
    events.add(e);
}

#ifdef __3DS__
void ac3ds_discard_pending_input()
{
    for(int i = events.length()-1; i >= 0; --i)
    {
        const SDL_Event &event = events[i];
        if(event.type == SDL_KEYDOWN || event.type == SDL_MOUSEMOTION ||
           (event.type == SDL_USEREVENT &&
            (event.user.code == AC3DS_MENU_TOUCH_EVENT || event.user.code == AC3DS_SERVER_JOIN_EVENT))) events.remove(i);
    }
}
#endif

bool interceptkey(int sym)
{
#ifdef __3DS__
    if(!ac3ds_main_loop()) return sym == SDLK_ESCAPE;
#endif
    static int lastintercept = SDLK_UNKNOWN;
    int len = lastintercept == sym ? events.length() : 0;
#ifdef __3DS__
    ac3ds_poll_input(); // after len: the button events queued here are the ones to scan below
#endif
    SDL_Event event;
    while(SDL_PollEvent(&event)) switch(event.type)
    {
        case SDL_MOUSEMOTION: break;
        default: pushevent(event); break;
    }
    lastintercept = sym;
    if(sym != SDLK_UNKNOWN) for(int i = len; i < events.length(); i++)
    {
        if(events[i].type == SDL_KEYDOWN)
        {
            int code = events[i].key.keysym.sym;
#ifdef __3DS__
            if(!ac3ds_accept_button_event(events[i].key)) continue;
            code = ac3ds_menu_key(code);
#endif
            if(code == sym) { events.remove(i); return true; }
        }
    }
    return false;
}

static void resetmousemotion()
{
#ifndef __3DS__
    if (grabinput && !relativemouse && !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
    {
        SDL_WarpMouseInWindow(screen, screenw / 2, screenh / 2);
    }
#endif
}

static inline bool skipmousemotion(SDL_Event &event)
{
    if(event.type != SDL_MOUSEMOTION) return true;
#ifndef __3DS__
    if(!(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
    {
        if(event.motion.x == screenw / 2 && event.motion.y == screenh / 2) return true;  // ignore any motion events generated SDL_WarpMouse
    }
#endif
    return false;
}

static void checkmousemotion(int &dx, int &dy)
{
    loopv(events)
    {
        SDL_Event &event = events[i];
        if(skipmousemotion(event))
        {
            if(i > 0) events.remove(0, i);
            return;
        }
        dx += event.motion.xrel;
        dy += event.motion.yrel;
    }
    events.setsize(0);
    SDL_Event event;
    while(SDL_PollEvent(&event))
    {
        if(skipmousemotion(event))
        {
            events.add(event);
            return;
        }
        dx += event.motion.xrel;
        dy += event.motion.yrel;
    }
}

#ifdef __3DS__
int ignoremouse = 0;
#else
int ignoremouse = 5;
#endif
int bootstrapentropy = 2;
#define EVENTDEBUG(x) x
//#define EVENTDEBUG(x)
EVENTDEBUG(VAR(debugevents, 0, 0, 2));

void checkinput()
{
#ifdef __3DS__
    ac3ds_poll_input();
#endif
    SDL_Event event;
    Uint32 lasttype = 0, lastbut = 0;
    int tdx=0,tdy=0;
    int focused = 0;
    while(events.length() || SDL_PollEvent(&event))
    {
        if(events.length()) event = events.remove(0);

        EVENTDEBUG(int thres = 1; defformatstring(eb)("EVENT %d", event.type));

        if (focused && event.type != SDL_WINDOWEVENT) { if (grabinput != (focused > 0)) inputgrab(grabinput = focused > 0, shouldgrab); focused = 0; }

        switch(event.type)
        {
            case SDL_QUIT:
                quit();
                break;

#ifdef __3DS__
            case SDL_USEREVENT:
                if(event.user.code == AC3DS_MENU_TOUCH_EVENT)
                    ac3ds_menu_touch(event.user.windowID, int(reinterpret_cast<intptr_t>(event.user.data1)),
                                     Ac3dsMenuTouch(reinterpret_cast<intptr_t>(event.user.data2)));
                else if(event.user.code == AC3DS_SERVER_JOIN_EVENT)
                    ac3ds_dispatch_server_join(event.user.windowID, unsigned(reinterpret_cast<uintptr_t>(event.user.data1)));
                break;
#endif
            case SDL_KEYDOWN:
            case SDL_KEYUP:
#ifdef __3DS__
                if(!ac3ds_accept_button_event(event.key)) break;
#endif
                if(event.type == SDL_KEYDOWN && bootstrapentropy > 0 && (--bootstrapentropy & 2))
                    mapscreenshot(NULL, false, -1, 1000.0f / (1000 + rnd(400)), 0, 0);
                EVENTDEBUG(thres = 2; concatstring(eb, event.type == SDL_KEYUP ? "(SDL_KEYUP)" : "(SDL_KEYDOWN)"));
                entropy_add_byte(event.key.keysym.sym ^ totalmillis);
                EVENTDEBUG(concatformatstring(eb, " sym %d (%Xh), scancode %d (%Xh), state %d, repeat %d", event.key.keysym.sym, event.key.keysym.sym, event.key.keysym.scancode, event.key.keysym.scancode, event.key.state, event.key.repeat));
                if(event.key.keysym.sym == SDLK_SCANCODE_MASK) event.key.keysym.sym |= event.key.keysym.scancode; // workaround SDL 2.0.5 bug which returns sym == 40000000h for all dead keys
                if(!event.key.repeat || keyrepeatmask) keypress(event.key.keysym.sym, event.key.keysym.scancode, event.key.state==SDL_PRESSED, (SDL_Keymod)event.key.keysym.mod);
                break;

            case SDL_TEXTINPUT:
                EVENTDEBUG(thres = 2; concatformatstring(eb, "(SDL_TEXTINPUT) %s %d %d", escapestring(event.text.text), textinputmask, int(event.text.timestamp - textinputtime)));
                if(textinputmask && int(event.text.timestamp - textinputtime) >= 5) // mute textinput for a few milliseconds, to avoid receiving the "t" that switched on the console
                {
                    processtextinput(event.text.text);
                }
                break;

            case SDL_WINDOWEVENT:
            {
                EVENTDEBUG(concatstring(eb, "(SDL_WINDOWEVENT) "));
                switch(event.window.event)
                {
                    case SDL_WINDOWEVENT_SHOWN: // window has been shown
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_SHOWN"));
                        break;

                    case SDL_WINDOWEVENT_HIDDEN: // window has been hidden
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_HIDDEN"));
                        break;

                    case SDL_WINDOWEVENT_EXPOSED: // window has been exposed and should be redrawn
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_EXPOSED"));
                        break;

                    case SDL_WINDOWEVENT_MOVED: // window has been moved to data1, data2
                        EVENTDEBUG(concatformatstring(eb, " SDL_WINDOWEVENT_MOVED x %d, y %d", event.window.data1, event.window.data2));
                        if(!(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN)) centerwindow = false;
                        break;

                    case SDL_WINDOWEVENT_RESIZED: // window has been resized to data1 x data2; this is event is always preceded by SDL_WINDOWEVENT_SIZE_CHANGED
                        EVENTDEBUG(concatformatstring(eb, " SDL_WINDOWEVENT_RESIZED %d x %d", event.window.data1, event.window.data2));
                        if(!fullscreendesktop || !(SDL_GetWindowFlags(screen) & SDL_WINDOW_FULLSCREEN))
                        {
                            scr_w = clamp(int(event.window.data1), SCR_MINW, SCR_MAXW);
                            scr_h = clamp(int(event.window.data2), SCR_MINH, SCR_MAXH);
                        }
                        break;

                    case SDL_WINDOWEVENT_SIZE_CHANGED: // window size has changed, either as a result of an API call or through the system or user changing the window size; this event is followed by SDL_WINDOWEVENT_RESIZED if the size was changed by an external event, i.e. the user or the window manager
                        updatescreensize();
                        EVENTDEBUG(concatformatstring(eb, " SDL_WINDOWEVENT_SIZE_CHANGED %dx%d", screenw, screenh));
                        break;

                    case SDL_WINDOWEVENT_MINIMIZED: // window has been minimized
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_MINIMIZED"));
                        inputgrab(false);
                        minimized = 1;
                        break;

                    case SDL_WINDOWEVENT_MAXIMIZED: // window has been maximized
                    case SDL_WINDOWEVENT_RESTORED: // window has been restored to normal size and position
                        EVENTDEBUG(concatstring(eb, event.window.event == SDL_WINDOWEVENT_RESTORED ? "SDL_WINDOWEVENT_RESTORED" : "SDL_WINDOWEVENT_MAXIMIZED"));
                        minimized = 0;
                        break;

                    case SDL_WINDOWEVENT_ENTER: // window has gained mouse focus
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_ENTER"));
                        shouldgrab = false;
                        focused = 1;
                        break;

                    case SDL_WINDOWEVENT_LEAVE: // window has lost mouse focus
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_LEAVE"));
                        shouldgrab = false;
                        focused = -1;
                        break;

                    case SDL_WINDOWEVENT_FOCUS_GAINED: // window has gained keyboard focus
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_FOCUS_GAINED"));
                        shouldgrab = true;
                        break;

                    case SDL_WINDOWEVENT_FOCUS_LOST: // window has lost keyboard focus
                        EVENTDEBUG(concatstring(eb, " SDL_WINDOWEVENT_FOCUS_LOST"));
                        shouldgrab = false;
                        focused = -1;
                        break;
                }
                break;
            }

            case SDL_MOUSEMOTION:
                EVENTDEBUG(thres = 2; concatformatstring(eb, "(SDL_MOUSEMOTION) x %d, y %d, dx %d, dy %d", event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel));
                if(ignoremouse) { ignoremouse--; break; }
                if(grabinput && !skipmousemotion(event))
                {
                    int dx = event.motion.xrel, dy = event.motion.yrel;
                    entropy_add_byte(dx ^ (256 - dy));
                    checkmousemotion(dx, dy);
                    resetmousemotion();
                    tdx+=dx;tdy+=dy;
                }
                else if (shouldgrab) inputgrab(grabinput = true);
                break;

            case SDL_MOUSEBUTTONDOWN:
                if(!grabinput) // click-to-grab
                {
                    EVENTDEBUG(concatformatstring(eb, "(SDL_MOUSEBUTTONDOWN) button %d, state %d, clicks %d, x %d, y %d", event.button.button, event.button.state, event.button.clicks, event.button.x, event.button.y));
                    inputgrab(grabinput = true);
                    #ifdef WIN32
                    if(!fullscreen)
                    {
                        // If AC runs in windowed mode on Windows 10 and if you press WIN+D, then move mouse on desktop, press WIN+D again, click in the AC window,
                        // the mouse cursor remains visible and the mouse is not contrained to the AC window anymore. It is as if SDL2 does not properly give focus
                        // to the AC window and so our workaround is to explicitly trigger the focus by hiding and showing the window. This issue exists in
                        // SDL 2.0.12 and is fixed in 2.0.14 however we currently can not use 2.0.14 because it has problems with handling of ALT+TAB hotkey behavior.
                        // Please re-test and remove this workaround once a fixed version of SDL is available.
                        SDL_HideWindow(screen);
                        SDL_ShowWindow(screen);
                    }
                    #endif
                    break;
                }

            case SDL_MOUSEBUTTONUP:
                EVENTDEBUG(concatformatstring(eb, "(SDL_MOUSEBUTTON%s) button %d, state %d, clicks %d, x %d, y %d", event.type == SDL_MOUSEBUTTONUP ? "UP" : "DOWN", event.button.button, event.button.state, event.button.clicks, event.button.x, event.button.y));
                if(lasttype==event.type && lastbut==event.button.button) break;
                keypress(-(event.button.button > 3 ? (event.button.button + 4) : event.button.button), 0, event.button.state != SDL_RELEASED);
                lasttype = event.type;
                lastbut = event.button.button;
                break;

            case SDL_MOUSEWHEEL:
                EVENTDEBUG(concatformatstring(eb, "(SDL_MOUSEWHEEL) x %d, y %d", event.wheel.x, event.wheel.y));
                if(event.wheel.y)
                {
                    int key = event.wheel.y > 0 ? SDL_AC_BUTTON_WHEELUP : SDL_AC_BUTTON_WHEELDOWN;
                    keypress(key, 0, true); // Emulate SDL1-style mouse wheel events by immediately "releasing" the wheel "button"
                    keypress(key, 0, false);
                }
                if(event.wheel.x)
                {
                    int key = event.wheel.x > 0 ? SDL_AC_BUTTON_WHEELRIGHT : SDL_AC_BUTTON_WHEELLEFT;
                    keypress(key, 0, true);
                    keypress(key, 0, false);
                }
                break;
        }
        EVENTDEBUG(if(debugevents >= thres) conoutf("%s", eb));
    }
#ifdef __3DS__
    ac3ds_sync_input_context();
    if(!ac3ds_gameplay_input_active()) tdx = tdy = 0;
#endif
    if(tdx || tdy)
    {
        entropy_add_byte(tdy + 5 * tdx);
        mousemove(tdx, tdy);
    }
    if (focused) { if (grabinput != (focused > 0)) inputgrab(grabinput = focused > 0, shouldgrab); focused = 0; }
#ifdef __3DS__
    extern void ac3ds_open_console_keyboard_if_requested();
    ac3ds_open_console_keyboard_if_requested();
#endif
}

VARF(gamespeed, 10, 100, 1000, if(multiplayer()) gamespeed = 100);
VARF(paused, 0, 0, 1, if(multiplayer()) paused = 0);

#ifdef __3DS__
bool ac3ds_offline_match_paused() { return paused && !multiplayer(NULL) && !watchingdemo; }
#endif

bool inmainloop = false;
static int clockrealbase = 0, clockvirtbase = 0;
static void clockreset() { clockrealbase = SDL_GetTicks(); clockvirtbase = totalmillis; }
VARFP(clockerror, 990000, 1000000, 1010000, clockreset());
VARFP(clockfix, 0, 0, 1, clockreset());

const char *rndmapname()
{
    vector<char *> maps;
    listfiles("packages/maps/official", "cgz", maps);
    if (maps.length() > 0)
    {
        char *map = newstring(maps[rnd(maps.length())]);
        maps.deletearrays();
        return map;
    }
    else return "";
}

#define AUTOSTARTPATH "config" PATHDIVS "autostart" PATHDIVS

void autostartscripts(const char *prefix)
{
    static vector<char *> *files = NULL;
    if(!files)
    {  // first run: fetch file names and sort them
        files = new vector<char *>;
        listfiles(AUTOSTARTPATH, "cfg", *files, stringsort);
    }
    loopv(*files)
    {
        if(*prefix && strncmp((*files)[i], prefix, strlen(prefix))) continue;
        defformatstring(p)(AUTOSTARTPATH "%s.cfg", (*files)[i]);
        execfile(p);
        delstring(files->remove(i--));
    }
}

void createconfigtemplates(const char *templatezip)  // create customisable config files in homedir - only if missing
{
    vector<const char *> files;
    void *mz = zipmanualopen(openfile(templatezip, "rb"), files);
    if(mz)
    {
        loopv(files)
        {
            const char *filename = behindpath(files[i]); // only look for config files in the zip file, ignore all paths in the zip
            if(strlen(filename) > 4 && !strcmp(filename + strlen(filename) - 4, ".cfg"))
            {
                defformatstring(fname)("config" PATHDIVS "%s", files[i]);
                if(getfilesize(fname) <= 0) // config does not exist or is empty
                {
                    stream *zf = openfile(fname, "wb");
                    if(zf)
                    {
                        zipmanualread(mz, i, zf, MAXCFGFILESIZE); // fetch file content from zip, write to new config file
                        delete zf;
                        conoutf("created %s from template %s", fname, templatezip);
                    }
                }
            }
        }
        zipmanualclose(mz);
    }
    findfile(AUTOSTARTPATH "dummy", "w"); // create empty autostart directory, if it doesn't exist yet
}

void connectprotocol(char *protocolstring) // assaultcube://example.org[:28763][/][?[port=28763][&password=secret]]
{
    urlparse u;
    u.set(protocolstring);
    if(strcmp(u.scheme, "assaultcube") || !*u.domain) { conoutf("\f3bad commandline syntax (\"%s\")", protocolstring); return; }
    if(*u.userpassword) clientlogf(" ignoring user:password part of url (%s)", u.userpassword);
    if(*u.fragment) clientlogf(" ignoring fragment part of url (%s)", u.fragment);
    if(*u.path) clientlogf(" ignoring path part of url (%s)", u.path);
    int port = ATOI(u.port);
    const char *q = u.query, *passwd = NULL;
    while(q && *q)
    {
        if(*q == '&') q++;
        if(!strncmp(q, "port=", 5))
        {
            q += 5;
            port = ATOI(q);
            q = strchr(q, '&');
        }
        else if(!strncmp(q, "password=", 9))
        {
            q += 9;
            const char *e = strchr(q, '&');
            passwd = e ? newstring(q, e - q) : newstring(q);
            q = e;
        }
        else break;
    }
    defformatstring(cmd)("connect %s %d %s", u.domain, port, passwd ? passwd : "");
    DEBUGCODE(clientlogf("connectprotocol: %s", cmd));
    addsleep(5, cmd, true);
    DELSTRING(passwd);
}

int bakshowmenuonstartup = -1;
static char *aaconnect = NULL;
void afterauth()
{
    // currently only for delayed connectprotocol
    if(aaconnect)
    {
        conoutf("\fUAfter Auth");
        if(bakshowmenuonstartup > -1)
        {
            alias("showmenuonstartup",bakshowmenuonstartup==1?"1":"0",false,false);
        }
    }
    if(aaconnect)
    {
        connectprotocol(aaconnect);
        DELSTRING(aaconnect);
    }
}

#ifdef THISISNOTDEFINED //WIN32
static char *parsecommandline(const char *src, vector<char *> &args)
{
    char *buf = new char[strlen(src) + 1], *dst = buf;
    for(;;)
    {
        while(isspace(*src)) src++;
        if(!*src) break;
        args.add(dst);
        for(bool quoted = false; *src && (quoted || !isspace(*src)); src++)
        {
            if(*src != '"') *dst++ = *src;
            else if(dst > buf && src[-1] == '\\') dst[-1] = '"';
            else quoted = !quoted;
        }
        *dst++ = '\0';
    }
    args.add(NULL);
    return buf;
}


int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw)
{
    vector<char *> args;
    char *buf = parsecommandline(GetCommandLine(), args);
    SDL_SetModuleHandle(hInst);
    int status = SDL_main(args.length()-1, args.getbuf());
    delete[] buf;
    exit(status);
    return 0;
}
#endif

void initclientlog()  // rotate old logfiles and create new one
{
    filerotate("clientlog", "txt", 5); // keep five old logfiles
    clientlogfile = openfile("clientlog.txt", "w");
    if(clientlogfile)
    {
        if(bootclientlog && bootclientlog->length()) clientlogfile->write(bootclientlog->getbuf(), bootclientlog->length());
    }
    else conoutf("could not create logfile \"%s\"", findfile("clientlog.txt", "w"));
    DELETEP(bootclientlog);
}

#ifdef _DEBUG
void sanitychecks()
{
    ASSERT((GMMASK__BOT ^ GMMASK__MP ^ GMMASK__TEAM ^ GMMASK__FFA ^ GMMASK__TEAMSPAWN ^ GMMASK__FFASPAWN) == GMMASK__ALL);
    float fa = 1 << LARGEST_FACTOR, fb = fa;
    while(fa != fa + fb) fb /= 2;
    ASSERT(2 * fb < NEARZERO);
}
#endif

#ifdef __3DS__
#define DEFAULTPROFILEPATH "sdmc:/3ds/assaultcube/profile"
#else
#define DEFAULTPROFILEPATH "profile"
#endif

int main(int argc, char **argv)
{
#ifdef __3DS__
    ac3ds_crash_install("main");
    if(const char *error = ac3ds_system_init())
    {
        if(*error) fatal("%s", error);
        // The first-run download was declined or did not finish, and its own
        // screen has said so. Nothing else has started yet.
        ac3ds_system_shutdown();
        return EXIT_SUCCESS;
    }
    ac3ds_read_launch_arguments(argc, argv);
#endif
    DEBUGCODE(sanitychecks());
    extern struct servercommandline scl;
    #ifdef WIN32
    //atexit((void (__cdecl *)(void))_CrtDumpMemoryLeaks);
    #ifndef _DEBUG
    #ifndef __GNUC__
    __try {
    #endif
    #endif
    // VS2017+ does not support %n in format strings by default due to security concerns.
    // However, currently we are making use of this functionality and therefore it is being enabled below.
    // In the future we should get rid of this. See also https://docs.microsoft.com/en-us/previous-versions/hf4y5e3w(v=vs.140)?redirectedfrom=MSDN
    _set_printf_count_output(1);
    #endif

    bool dedicated = false;
    bool quitdirectly = false;
    char *initscript = NULL;
    char *initdemo = NULL;

    setlocale(LC_ALL, "POSIX");
    if(bootclientlog) cvecprintf(*bootclientlog, "######## start logging: %s\n", timestring(true));

    const char *initmap = rndmapname();
    loopi(NUMGUNS) crosshairnames[i] = gunnames[i] = guns[i].modelname;
    crosshairnames[GUN_AKIMBO] = gunnames[GUN_AKIMBO] = "akimbo";
    crosshairnames[CROSSHAIR_DEFAULT] = "default";
    crosshairnames[CROSSHAIR_TEAMMATE] = "teammate";
    crosshairnames[CROSSHAIR_SCOPE] = "scope";
    crosshairnames[CROSSHAIR_EDIT] = "edit";
    crosshairnames[CROSSHAIR_NUM] = gunnames[NUMGUNS] = "";

    pushscontext(IEXC_CFG);
    persistidents = false;

#ifdef __3DS__
    // Startup progress on the lower screen, by stage.
    auto startupprogress = [](const char *stage)
    {
        static const struct { const char *stage, *label; float progress; } steps[] = {
            { "video: sdl", "Starting video", 0.03f }, { "video: mode", "Starting video", 0.05f },
            { "video: misc", "Starting video", 0.07f }, { "gl", "Starting the renderer", 0.10f },
            { "console", "Loading fonts", 0.14f }, { "console icons loaded", "Loading fonts", 0.18f },
            { "startup screen", "Checking the graphics chip", 0.22f }, { "particles", "Loading effects", 0.32f },
            { "sound", "Starting audio", 0.40f }, { "cfg", "Reading configuration", 0.50f },
            { "models", "Loading player and weapon models", 0.65f }, { "docs", "Loading help", 0.85f },
            { "localconnect", "Starting the game", 0.92f }, { "mainloop", "Starting the game", 1.0f } };
        for(const auto &step : steps)
            if(!strcmp(step.stage, stage)) { ac3ds_loading_screen("Starting up", step.label, step.progress); return; }
    };
    #define initlog(s) do { AC3DS_PHASE("init: " s); startupprogress(s); clientlogf("init: " s); if(clientlogfile) clientlogfile->fflush(); } while(0)
#else
    #define initlog(s) clientlogf("init: " s)
#endif

    if(*stream_capabilities()) clientlogf("info: %s", stream_capabilities());
#ifdef __3DS__
    clientlogf("game data: %s", ac3ds_asset_source());
#endif

    bool waitforauth = false;
    initing = INIT_RESET;
    for(int i = 1; i<argc; i++)
    {
#ifdef __3DS__
        if(argv[i][0] == '-' && argv[i][1] && strchr("pxY", argv[i][1]))
            clientlogf("parsing commandline argument %d: -%c", i, argv[i][1]);
        else if(!strncmp(argv[i], "assaultcube://", 14))
            clientlogf("parsing commandline argument %d: connection URL", i);
        else
#endif
        clientlogf("parsing commandline argument %d: \"%s\"", i, argv[i]);
        // server: ufimNFTLAckyxpDWrXBKIoOnPMVC
        if(!scl.checkarg(argv[i]))
        {
            char *a = &argv[i][2];
            if(argv[i][0]=='-') switch(argv[i][1])
            {
                case '-':
                    if(!strcmp(argv[i], "--home"))
                    {
                        sethomedir(DEFAULTPROFILEPATH);
                    }
                    else if(!strncmp(argv[i], "--home=", 7))
                    {
                        sethomedir(&argv[i][7]);
                    }
                    else if(!strncmp(argv[i], "--mod=", 6))
                    {
                        addpackagedir(&argv[i][6]);
                    }
                    else if(!strcmp(argv[i], "--init"))
                    {
                        if(!havehomedir()) sethomedir(DEFAULTPROFILEPATH);
                        execfile((char *)"config/init.cfg");
                        restoredinits = true;
                    }
                    else if(!strncmp(argv[i], "--init=", 7))
                    {
                        if(!havehomedir()) sethomedir(DEFAULTPROFILEPATH);
                        execfile(&argv[i][7]);
                        restoredinits = true;
                    }
                    else if(!strcmp(argv[i], "--version"))
                    {
                        printf("%.3f\n", AC_VERSION/1000.0f);
                        quitdirectly = true;
                    }
                    else if(!strcmp(argv[i], "--protocol"))
                    {
                        printf("%d\n", PROTOCOL_VERSION);
                        quitdirectly = true;
                    }
                    else if(!strncmp(argv[i], "--loadmap=", 10))
                    {
                        initmap = &argv[i][10];
                    }
                    else if(!strncmp(argv[i], "--loaddemo=", 11))
                    {
                        initdemo = &argv[i][11];
                    }
                    else conoutf("\f3unknown commandline switch: %s", argv[i]);
                    break;
                case 'd': dedicated = true; break;
                case 't': fullscreen = atoi(a); break;
                case 'w': scr_w  = atoi(a); break;
                case 'h': scr_h  = atoi(a); break;
                case 'z': depthbits = atoi(a); break;
                case 's': stencilbits = atoi(&argv[i][2]); break;
                case 'a': fsaa = atoi(a); break;
                case 'v': vsync = atoi(a); break;
                case 'e': initscript = &argv[i][2]; break;
                default:  conoutf("\f3unknown commandline option: -%c", argv[i][1]);
            }
            else if(!strncmp(argv[i], "assaultcube://", 14)) // browser direct connection
            {
                waitforauth = true;
                conoutf("\fUwill \f2connect\fU when authenticated."); // individual notification - usually first in console, so off screen when menu pops up
                aaconnect = newstring(argv[i]);//triggers connectprotocol(argv[i]); during afterauth()
            }
            else conoutf("\f3unknown commandline argument: %c", argv[i][0]);
        }
    }
    if(!havehomedir())
    {
        sethomedir(DEFAULTPROFILEPATH);
#ifdef __3DS__
        execfile("config/init.cfg");
        restoredinits = true;
#endif
    }
    entropy_init(time(NULL) + (uint)(size_t)&initscript + (uint)(size_t)entropy_init);
    initclientlog();
    if(quitdirectly)
    {
#ifdef __3DS__
        ac3ds_system_shutdown();
#endif
        return EXIT_SUCCESS;
    }
    if(waitforauth)
    {
        hudoutf("\f5.. \f2waiting \f4for \fUauth\f5entication .."); // general notification - more prominent to show things are about to happen
    }

    createconfigtemplates("config" PATHDIVS "configtemplates.zip");

    initing = NOT_INITING;

    #define SDLVERSIONSTRING  STRINGIFY(SDL_MAJOR_VERSION) "." STRINGIFY(SDL_MINOR_VERSION) "." STRINGIFY(SDL_PATCHLEVEL)
    initlog("sdl (" SDLVERSIONSTRING ")");
    int par = 0;
#ifdef _DEBUG
    par = SDL_INIT_NOPARACHUTE;
#endif
#ifdef __3DS__
    native_dedicated = dedicated;
    if(SDL_Init(SDL_INIT_TIMER|SDL_INIT_EVENTS|par)<0) fatal("Unable to initialize SDL");
    if(!ac3ds_start_network())
    {
        if(dedicated) fatal("Unable to initialize the Nintendo 3DS socket service");
        clientlogf("Nintendo 3DS socket service unavailable. Offline play is available.");
    }
#else
    if(SDL_Init(SDL_INIT_TIMER|SDL_INIT_VIDEO|par)<0) fatal("Unable to initialize SDL");
#endif
    SDL_version sdlver;
    SDL_GetVersion(&sdlver);
    if(SDL_COMPILEDVERSION != SDL_VERSIONNUM(sdlver.major, sdlver.minor, sdlver.patch))
        clientlogf("SDL: compiled version " SDLVERSIONSTRING ", linked version %u.%u.%u", sdlver.major, sdlver.minor, sdlver.patch);

#if 0
    if(highprocesspriority) setprocesspriority(true);
#endif

    if (!dedicated) initlog("net (" STRINGIFY(ENET_VERSION_MAJOR) "." STRINGIFY(ENET_VERSION_MINOR) "." STRINGIFY(ENET_VERSION_PATCH) ")");
    if(enet_initialize()<0) fatal("Unable to initialise network module");
#ifdef __3DS__
    native_network = true;
#endif

    if (!dedicated) initclient();

    initserver(dedicated);
#ifdef __3DS__
    if(dedicated)
    {
        cleanup(NULL);
        DELETEP(clientlogfile);
        return EXIT_SUCCESS;
    }
#endif

    initlog("world (" STRINGIFY(AC_VERSION) ")");
    empty_world(7, true);

#ifndef __3DS__
    initlog("video: sdl");
    if(SDL_InitSubSystem(SDL_INIT_VIDEO)<0) fatal("Unable to initialize SDL Video");
#endif

    initlog("video: mode");
    int useddepthbits = 0, usedfsaa = 0;
    setupscreen(useddepthbits, usedfsaa);

    initlog("video: misc");

#if !defined(__APPLE__) && !defined(__3DS__)
    SDL_Surface *icon = IMG_Load("packages/misc/icon.png");
    SDL_SetWindowIcon(screen, icon);
#endif

#ifdef __APPLE__
    // Fix https://github.com/assaultcube/AC/issues/300
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1", SDL_HINT_OVERRIDE);
#endif

#ifndef __3DS__
    SDL_ShowCursor(0);
#endif

    initlog("gl");
    gl_checkextensions();
    gl_init(screenw, screenh, useddepthbits, usedfsaa);

    notexture = noworldtexture = textureload("packages/misc/notexture.jpg");
    if(!notexture) fatal("could not find core textures (hint: run AssaultCube from the parent of the bin directory)");

    initlog("console");
    updateigraphs();
#ifdef __3DS__
    initlog("console icons loaded");
#endif
    autostartscripts("_veryfirst_");
    // Main font file, all other font files execute from here.
    if(!execfile("config/font.cfg")) fatal("cannot find default font definitions");
    // Check these 2 standard fonts have been executed.
    if(!setfont("mono")) fatal("no mono font specified");
    if(!setfont("default")) fatal("no default font specified");

#ifdef __3DS__
    initlog("startup screen");
    // Each stage adds one part of the renderer and waits for the GPU queue.
    // A stage that never completes is named in the log and on the touch screen.
    auto awaitStartupGpu = [](const char *stage, const char *meaning)
    {
        uint32_t status = 0, state[6] = {};
        int32_t result = 0;
        if(ac3ds_gl_drain_startup_frame(status,result))
        {
            clientlogf("init: startup %s completed",stage);
            return true;
        }
        ac3ds_gl_startup_state(state);
        clientlogf("init: startup %s GPU timeout (busy=%08x read=%08x gx=%08x err=%08x list=%08x+%x free=%08x)",
                   stage,unsigned(status),unsigned(result),unsigned(state[0]),unsigned(state[1]),
                   unsigned(state[2]),unsigned(state[3]),unsigned(state[5]));
        if(clientlogfile) clientlogfile->fflush();
        defformatstring(l0)("Stopped at: %s",stage);
        defformatstring(l2)("busy %08x  gx %08x",unsigned(state[4]),unsigned(state[0]));
        defformatstring(l3)("err  %08x  len %x",unsigned(state[1]),unsigned(state[3]));
        const char *lines[] = { l0, meaning, l2, l3, "", "Log and command lists are in", "/3ds/assaultcube/profile/", "", "Hold POWER to switch off." };
        ac3ds_show_startup_report(lines,int(sizeof(lines)/sizeof(*lines)));
        while(aptMainLoop()) svcSleepThread(100000000);
        return false;
    };
    ac3ds_gl_set_command_dumps(8);
    ac3ds_gl_startup_clear();
    if(!awaitStartupGpu("clear","Screen clear did not finish.")) return EXIT_FAILURE;
    ac3ds_draw_controls();
    ac3ds_gl_probe_empty();
    if(!awaitStartupGpu("empty list","GPU command lists do not complete.")) return EXIT_FAILURE;
    ac3ds_gl_probe_minimal();
    if(!awaitStartupGpu("minimal draw","A plain triangle does not complete.")) return EXIT_FAILURE;
    ac3ds_gl_startup_draw();
    if(!awaitStartupGpu("game shader draw","The game vertex path stalls.")) return EXIT_FAILURE;
    // Optional GPU features get a guarded check. The marker file reads
    // "pending" while the check runs; if a launch stops inside it, the next
    // launch finds that, records "off" and runs with the fallback. Deleting
    // the marker retries the check. A pass is recorded with the build number
    // and only trusted by that build, since a new build can change what the
    // check draws.
    auto guardedCheck = [&](const char *marker, void (*probe)(), const char *stage, const char *meaning) -> int
    {
        char previous[16] = "";
        if(FILE *f = fopen(marker,"r")) { if(!fgets(previous,sizeof(previous),f)) previous[0] = 0; fclose(f); }
        auto mark = [marker](const char *state) { if(FILE *f = fopen(marker,"w")) { fputs(state,f); fclose(f); } };
        if(!strncmp(previous,"pending",7) || !strncmp(previous,"off",3))
        {
            if(previous[0] == 'p') mark("off");
            return 0;
        }
        defformatstring(passed)("ok %d", AC3DS_BUILD_NUMBER);
        const bool known = !strcmp(previous, passed);
        if(!known) mark("pending");
        probe();
        if(!awaitStartupGpu(stage,meaning)) return -1;
        if(!known) mark(passed);
        return 1;
    };
    // The projection program is only needed for water.
    switch(guardedCheck("sdmc:/3ds/assaultcube/profile/gpu-projective.txt", ac3ds_gl_probe_projective,
                        "projection draw", "Restart: water will use the fallback."))
    {
        case -1: return EXIT_FAILURE;
        case 0:
            ac3ds_gl_set_projective(false);
            clientlogf("init: projection textures disabled after an earlier stalled check");
            break;
    }
    // Per-pixel fog uses the procedural texture unit, which nothing else in
    // the port touches. The fallback is build 11's rendering: the GPU fog
    // table over linear depth.
    switch(guardedCheck("sdmc:/3ds/assaultcube/profile/gpu-pixelfog.txt", ac3ds_gl_probe_fog,
                        "fog draw", "Restart: fog will use the GPU table."))
    {
        case -1: return EXIT_FAILURE;
        case 0:
        {
            extern int ac3dspixelfog, ac3dslineardepth;
            ac3dspixelfog = 0;
            ac3dslineardepth = 1;
            ac3ds_gl_set_pixel_fog(false);
            ac3ds_gl_set_linear_depth(true);
            clientlogf("init: per-pixel fog disabled after an earlier stalled check (delete profile/gpu-pixelfog.txt to retry)");
            break;
        }
    }
#endif
    loadingscreen();

#ifdef __3DS__
    if(!awaitStartupGpu("splash","The first textured draw stalls.")) return EXIT_FAILURE;
    ac3ds_gl_set_command_dumps(0);
    initlog("particles");
#endif
    particleinit();

    initlog("sound");
    audiomgr.initsound();

    initlog("cfg");
    extern void *scoremenu, *servmenu, *searchmenu, *serverinfomenu, *kickmenu, *banmenu, *forceteammenu, *giveadminmenu, *docmenu, *applymenu, *downloaddemomenu;
    scoremenu = addmenu("score", "columns", false, renderscores, NULL, false, true);
    servmenu = addmenu("server", NULL, true, refreshservers, serverskey);
    searchmenu = addmenu("search", NULL, true, refreshservers, serverskey);
    serverinfomenu = addmenu("serverinfo", "extended server information (F5: refresh)", true, refreshservers, serverinfokey);
    kickmenu = addmenu("kick player", NULL, true, refreshsopmenu);
    banmenu = addmenu("ban player", NULL, true, refreshsopmenu);
    forceteammenu = addmenu("force team", NULL, true, refreshsopmenu);
    giveadminmenu = addmenu("give admin", NULL, true, refreshsopmenu);
    docmenu = addmenu("reference", NULL, true, renderdocmenu);
    applymenu = addmenu("apply", "apply changes now?", true, refreshapplymenu);
    downloaddemomenu = addmenu("Download demo", NULL, true);

    exec("config/scontext.cfg");
#ifdef __3DS__
    ac3ds_install_keymap();
#endif
    exec("config/keymap.cfg");
    execfile("config/mapmodelattributes.cfg");
    exec("config/menus.cfg");
#ifdef __3DS__
    ac3ds_install_handheld_menus();
#endif
    exec("config/scripts.cfg");
    exec("config/prefabs.cfg");
    registerdefaultsounds();
    exec("config/sounds.cfg");
    exec("config/securemaps.cfg");
    exec("config/admin.cfg");
    execfile("config/servers.cfg");
    execfile("config/pcksources.cfg");
    execfile("config/authkeys.cfg");
    execfile("config/authprivate.cfg");
    loadcertdir();
    loadhistory();
    setupautodownload();
    int xmn = loadallxmaps();
    if(xmn) conoutf("loaded %d xmaps", xmn);
    persistidents = true;

    static char resdata[] = { 112, 97, 99, 107, 97, 103, 101, 115, 47, 116, 101, 120, 116, 117, 114, 101, 115, 47, 107, 117, 114, 116, 47, 107, 108, 105, 116, 101, 50, 46, 106, 112, 103, 0 };
    stream *f = opengzfile(resdata, "rb");
    if(f)
    {
        int n = f->getlil<int>();
        loopi(n)
        {
            string s;
            f->read(s, sizeof(string));
            enet_uint32 c = f->getlil<enet_uint32>();
            setresdata(s, c);
        }
        delete f;
    }

    autostartscripts("_beforesaved_");
    initing = INIT_LOAD;
    if(!execfile("config/saved.cfg"))
    {
        exec("config/defaults.cfg");
        bootstrapentropy += 5 + rnd(7);
    }
#ifdef __3DS__
    ac3ds_upgrade_scope_binding();
#endif
    autostartscripts("_aftersaved_");
    exechook(HOOK_SP_MP, "afterinit", "");
    if(waitforauth)
    {
        bakshowmenuonstartup = !strcmp(getalias("showmenuonstartup"),"1")?1:0;
        alias("showmenuonstartup","0");
    }
    autostartscripts("");    // all remaining scripts
    execfile("config/autoexec.cfg");
    exechook(HOOK_SP_MP, "autoexec", "");




    initing = NOT_INITING;
    uniformtexres = !hirestextures;

    initlog("models");
    preload_playermodels();
    preload_hudguns();
    preload_entmodels();

    initlog("docs");
    persistidents = false;
    execfile("config/docs.cfg");
    persistidents = true;

    initlog("localconnect");
    extern string clientmap;

    if(initdemo)
    {
        extern int gamemode;
        gamemode = -1;
        copystring(clientmap, initdemo);
    }
    else
    copystring(clientmap, initmap); // ac_complex for 1.0, ac_shine for 1.1, ..

    localconnect();

    if(initscript) execute(initscript);

    initlog("mainloop");
#ifdef __3DS__
    ac3ds_loading_done();
    ac3ds_crash_start_watchdog();
#endif

    inputgrab(grabinput = true);

    inmainloop = true;
#ifdef __3DS__
    clockreset();
#endif
    static bool onceauthed = true;
#ifdef _DEBUG
    int lastflush = 0;
#endif
    if(scl.logident[0])
    { // "-Nxxx" sets the number of client log lines to xxx (after init)
        extern int clientloglinesremaining;
        clientloglinesremaining = atoi(scl.logident);  // dual-use for scl.logident
    }
    for(;;)
    {
#ifdef __3DS__
        AC3DS_PHASE("frame tick");
        ac3ds_crash_tick();
        AC3DS_PHASE("system events");
        if(!ac3ds_main_loop()) break;
        if(ac3ds_take_clock_reset())
        {
            Ac3dsPhaseScope phase("APT: reset frame clock");
            ac3ds_crash_note_event("clock reset");
            clockreset();
            ac3ds_audio_service();
        }
        // Coming back from HOME or sleep mid-match offline: pause, so the
        // player is not dropped straight back into a firefight.
        if(ac3ds_take_resume() && !multiplayer(NULL) && !watchingdemo && *getclientmap() &&
           ac3ds_menu_panel() == Ac3dsMenu::None && !getcurcommand(NULL))
        {
            Ac3dsPhaseScope phase("APT: reopen offline pause menu");
            execute("ac3ds_pause_menu");
        }
        AC3DS_PHASE("frame clock");
#endif
        static int frames = 0;
        static float fps = 10.0f;
        int millis = SDL_GetTicks() - clockrealbase;
        if(clockfix) millis = int(millis*(double(clockerror)/1000000));
        millis += clockvirtbase;
        if(millis<totalmillis) millis = totalmillis;
        limitfps(millis, totalmillis);
        int elapsed = millis-totalmillis;
        entropy_add_byte(elapsed);
#ifdef __3DS__
        const int frameGameMillis = lastmillis;
#endif
        if(multiplayer(NULL))
        {
            if(ispaused) curtime = 0;
            else curtime = elapsed;
        }
        else
        {
            static int timeerr = 0;
            int scaledtime = elapsed*gamespeed + timeerr;
            curtime = scaledtime/100;
            timeerr = scaledtime%100;
            if(!watchingdemo) skipmillis = 0;
            if(skipmillis && watchingdemo)
            {
                int skipmillisnow = skipmillis > 5000 ? skipmillis - 5000 : min(1000, skipmillis);
                curtime += skipmillisnow;
                skipmillis -= skipmillisnow;
            }
            if(paused) curtime = 0;
        }
        lastmillis += curtime;
        totalmillis = millis;

#ifdef __3DS__
        extern void ac3ds_resolve_aim();
        AC3DS_PHASE("aim: resolve previous depth");
        ac3ds_resolve_aim();
        Ac3dsPhaseTimer updatePhase(Ac3dsPhase::Update);
        AC3DS_PHASE("input");
#endif
        checkinput();

#ifdef __3DS__
        // The pause menu can open during input. Keep the elapsed wall time
        // for UI and audio, but discard this simulation step before either
        // the client or the local server can act on it.
        const bool offlineMatchPaused = ac3ds_offline_match_paused();
        AC3DS_PHASE("update world");
        if(offlineMatchPaused)
        {
            lastmillis = frameGameMillis;
            curtime = 0;
            gets2c(); // a pending online connection still needs its events
        }
        else
#endif
        if(lastmillis) updateworld(curtime, lastmillis);

        if(needsautoscreenshot) showscores(true);

#ifdef __3DS__
        AC3DS_PHASE("server slice");
        if(!offlineMatchPaused)
#endif
        serverslice(0);
#ifdef __3DS__
        updatePhase.stop();
#endif

        if(elapsed) fps = (1000.0f/elapsed+fps*10)/11; // avoid DIV-by-0
        frames++;

#ifdef __3DS__
        Ac3dsPhaseTimer audioPhase(Ac3dsPhase::Audio);
        AC3DS_PHASE("audio");
#endif
        audiomgr.updateaudio();
#ifdef __3DS__
        audioPhase.stop();
        Ac3dsPhaseTimer rayPhase(Ac3dsPhase::Rays);
        AC3DS_PHASE("ray table");
        {
            // A benchmark never runs on a server: put its settings back at once.
            extern bool ac3dsbenchleft();
            extern void ac3dsbenchend(const char *how);
            if(ac3dsbenchleft()) ac3dsbenchend("cancelled");
            extern void ac3dsburstframe();
            ac3dsburstframe();
            // Benchmark aid: turn the view by a fixed step every frame.
            extern float ac3dsautoturn;
            extern int ac3dsautomove, ac3dsautostrafe;
            if(ac3dsautoturn && !offlineMatchPaused && !multiplayer(NULL) && player1->state == CS_ALIVE)
            {
                extern float ac3dsturnjitter;
                player1->yaw += ac3dsautoturn + ((frames&1) ? ac3dsturnjitter : 0);
                while(player1->yaw >= 360) player1->yaw -= 360;
            }
            extern int ac3dsautofire;
            static bool autofiring = false;
            if(ac3dsautofire && !offlineMatchPaused && !multiplayer(NULL) && player1->state == CS_ALIVE) player1->attacking = autofiring = true;
            else if(autofiring) player1->attacking = autofiring = false;
            static bool automoving = false;
            if((ac3dsautomove || ac3dsautostrafe) && !offlineMatchPaused && !multiplayer(NULL) && player1->state == CS_ALIVE)
            {
                player1->move = ac3dsautomove;
                player1->strafe = ac3dsautostrafe;
                automoving = true;
            }
            else if(automoving)
            {
                player1->move = player1->strafe = 0;
                automoving = false;
            }
        }
        // Pick this frame's camera before tracing occlusion rays from it.
        // gl_drawframe() only switches to the death, follow or fly camera
        // later, so the table used to describe the previous viewpoint: one
        // frame without the facing walls on every death and respawn, and a
        // band of missing world while a followed player turns.
        extern void recomputecamera();
        recomputecamera();
#endif
        computeraytable(camera1->o.x, camera1->o.y, dynfov());
#ifdef __3DS__
        rayPhase.stop();
#endif
        if(frames>3 && !minimized)
        {
#ifdef __3DS__
            AC3DS_PHASE("begin frame");
            ac3ds_gl_begin_frame();
            AC3DS_PHASE("draw frame");
#endif
            gl_drawframe(screenw, screenh, fps<lowfps ? fps/lowfps : (fps>highfps ? fps/highfps : 1.0f), fps, elapsed);
#ifdef __3DS__
            AC3DS_PHASE("present");
            swapbuffers();
            extern void ac3ds_report_profile();
            ac3ds_report_profile();
#else
            if(frames>4) swapbuffers();
#endif
        }

        if(needsautoscreenshot)
        {
#ifdef __3DS__
            // The frame above already includes the scoreboard.
            if(frames > 3 && !minimized)
            {
                screenshot(NULL);
                needsautoscreenshot = false;
            }
#else
            showscores(true);
            // draw again after swapping buffers, to make sure menu is captured
            // in the screenshot regardless of which frame buffer is current
            if(!minimized)
            {
                gl_drawframe(screenw, screenh, fps<lowfps ? fps/lowfps : (fps>highfps ? fps/highfps : 1.0f), fps, elapsed);
            }
            addsleep(0, "screenshot");
            needsautoscreenshot = false;
#endif
        }
#ifdef _DEBUG
        if(millis>lastflush+60000) { fflush(stdout); lastflush = millis; }
#endif
#ifdef __3DS__
        AC3DS_PHASE("frame end");
        {
            extern void ac3ds_world_batches_frame_end();
            ac3ds_world_batches_frame_end();
        }
        if(frames <= 6)
        {
            // Progress marks for handheld runs, flushed so a hang leaves a trace.
            clientlogf("frame %d done (%d ms)", frames, elapsed);
            if(clientlogfile) clientlogfile->fflush();
        }
        else if(ac3ds_frames_after_load(frames))
        {
            // The first seconds of a match (bot spawn, waypoint flood, first
            // draws): flushed, so a crash there is pinned down.
            char memory[224];
            ac3ds_memory_line(memory, sizeof(memory));
            clientlogf("frame %d done (%d ms) after load: %s", frames, elapsed, memory);
            if(clientlogfile) clientlogfile->fflush();
        }
        else if(frames % 1800 == 0)
        {
            // Every 30 s: progress and memory, flushed so a crash mid-match
            // leaves the last state on the SD card (one small write).
            char memory[224];
            ac3ds_memory_line(memory, sizeof(memory));
            clientlogf("frame %d done (%d ms) %s %s", frames, elapsed, getclientmap(), memory);
            extern void ac3dsdiag();
            ac3dsdiag();
            if(clientlogfile) clientlogfile->fflush();
        }
        static uint32_t queueRecoveries = 0;
        if(ac3ds_gl_queue_recoveries() != queueRecoveries)
        {
            queueRecoveries = ac3ds_gl_queue_recoveries();
            clientlogf("AC3DS_QUEUE cleared a stuck GX queue at frame %d (%u so far)", frames, unsigned(queueRecoveries));
            if(clientlogfile) clientlogfile->fflush();
        }
#endif
        pollautodownloadresponse();
        if(onceauthed && sk)
        {
            onceauthed = false;
            afterauth();
        }
    }

    quit();
    return EXIT_SUCCESS;

#if defined(WIN32) && !defined(_DEBUG) && !defined(__GNUC__)
    } __except(stackdumper(0, GetExceptionInformation()), EXCEPTION_CONTINUE_SEARCH) { return 0; }
#endif
}

#ifdef __3DS__
// QA: tap the match screen at x y.
COMMANDF(ac3dstap, "ii", (int *x, int *y) { extern void ac3ds_qa_tap(int, int); ac3ds_qa_tap(*x, *y); });
// QA: put the player at x y z (a negative x keeps the position) facing yaw pitch.
COMMANDF(ac3dspose, "fffff", (float *x, float *y, float *z, float *yaw, float *pitch)
{
    if(multiplayer("ac3dspose")) return; // QA teleports never run on a server
    if(*x >= 0) { player1->o = vec(*x, *y, *z); player1->vel = vec(0, 0, 0); player1->resetinterp(); }
    player1->yaw = *yaw - 360*floorf(*yaw/360); // the world LOD rectangle assumes 0..360
    player1->pitch = *pitch;
    clientlogf("qa: pose %.1f %.1f %.1f yaw %.0f pitch %.0f", player1->o.x, player1->o.y, player1->o.z, player1->yaw, player1->pitch);
});
// QA: scripted stylus taps through the real input path: x y presses hold_ms gap_ms.
COMMANDF(ac3dsqatouch, "iiiii", (int *x, int *y, int *n, int *hold, int *gap) { extern void ac3ds_qa_touch(int, int, int, int, int); ac3ds_qa_touch(*x, *y, *n, *hold, *gap); });
// QA: save the lower screen as profile/screenshots/<name>.bmp
COMMANDF(bottomshot, "s", (const char *name)
{
    defformatstring(file)("screenshots/%s.bmp", *name ? name : "bottom");
    const char *target = findfile(path(file), "wb");
    conoutf(ac3ds_save_bottom_screen(target) ? "saved lower screen to %s" : "could not save %s", target);
});
#endif

VAR(version, 1, AC_VERSION, 0);
VAR(protocol, 1, PROTOCOL_VERSION, 0);

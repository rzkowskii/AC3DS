// menus.cpp: ingame menu system (also used for scores and serverlist)

#include "cube.h"
#ifdef __3DS__
#include "system.h"
#endif

hashtable<const char *, gmenu> menus;
gmenu *curmenu = NULL, *lastmenu = NULL;
color *menuselbgcolor, *menuseldescbgcolor = NULL;
static int menurighttabwidth = 888; // width of sliders and text input fields (is adapted to font and screen size later)

vector<gmenu *> menustack;

#ifdef __3DS__
static unsigned handheld_menu_revision = 1;
static bool handheld_menu_paused = false;
static void renderhandheldmenu(gmenu &menu);
static void renderhandheldservers(gmenu &menu);
static int handheldmenupage(const gmenu &menu, int direction);
#endif

COMMANDF(curmenu, "", () {result(curmenu ? curmenu->name : "");} );

inline gmenu *setcurmenu(gmenu *newcurmenu)      // only change curmenu through here!
{
#ifdef __3DS__
    if(curmenu != newcurmenu) ++handheld_menu_revision;
    if(!newcurmenu && handheld_menu_paused)
    {
        handheld_menu_paused = false;
        setvar("paused", 0);
    }
#endif
    curmenu = newcurmenu;
    keyrepeat(curmenu && curmenu->allowinput
#ifndef __3DS__
              && !curmenu->hotkeys
#endif
              , KR_MENU);
    return curmenu;
}

void menuset(void *m, bool save)
{
    if(curmenu==m) return;
    if(curmenu)
    {
        if(save && curmenu->allowinput) menustack.add(curmenu);
        else curmenu->close();
    }
    if(setcurmenu((gmenu *)m)) curmenu->open();
}

void showmenu(const char *name, bool top)
{
    menurighttabwidth = max(VIRTW/4, 15 * text_width("w")); // adapt to screen and font size every time a menu opens
    if(!name)
    {
        setcurmenu(NULL);
        return;
    }
    gmenu *m = menus.access(name);
    if(!m) return;
    if(!top && curmenu)
    {
        if(curmenu==m) return;
        loopv(menustack) if(menustack[i]==m) return;
        menustack.insert(0, m);
        return;
    }
    menuset(m);
}

void closemenu(const char *name)
{
    gmenu *m;
    if(!name)
    {
        if(curmenu) curmenu->close();
        while(!menustack.empty())
        {
            m = menustack.pop();
            if(m) m->close();
        }
        setcurmenu(NULL);
        return;
    }
    m = menus.access(name);
    if(!m) return;
    if(curmenu==m) menuset(menustack.empty() ? NULL : menustack.pop(), false);
    else loopv(menustack)
    {
        if(menustack[i]==m)
        {
            menustack.remove(i);
            return;
        }
    }
}
COMMAND(closemenu, "s");

void showmenu_(const char *name)
{
    showmenu(name, true);
}
COMMANDN(showmenu, showmenu_, "s");

const char *persistentmenuselectionalias(const char *name)
{
    static defformatstring(al)("__lastmenusel_%s", name);
    filtertext(al, al, FTXT__ALIAS);
    return al;
}

int normalizemenuselection(int sel, int length)
{
    if (sel < 0) sel = length > 0 ? length - 1 : 0;
    else if (sel >= length) sel = 0;
    return sel;
}

void menuselect(void *menu, int sel)
{
    gmenu &m = *(gmenu *)menu;
    sel = normalizemenuselection(sel, m.items.length());
    if(m.items.inrange(sel))
    {
        int oldsel = m.menusel;
        m.menusel = sel;
        if(m.allowinput)
        {
            if(sel!=oldsel)
            {
                if(m.items.inrange(oldsel)) m.items[oldsel]->focus(false);
                if(!m.items[sel]->greyedout) m.items[sel]->focus(true);
                audiomgr.playsound(S_MENUSELECT, SP_HIGHEST);
                if(m.persistentselection)
                {
                    defformatstring(val)("%d", sel);
                    alias(persistentmenuselectionalias(m.name), val);
                }
            }
        }
    }
}

void drawarrow(int dir, int x, int y, int size, float r = 1.0f, float g = 1.0f, float b = 1.0f)
{
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glColor3f(r, g, b);

    glBegin(GL_TRIANGLES);
    glVertex2f(x, dir ? y+size : y);
    glVertex2f(x+size/2, dir ? y : y+size);
    glVertex2f(x+size, dir ? y+size : y);
    glEnd();
    xtraverts += 3;

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
}

#define MAXMENU 34

bool menuvisible()
{
    if(!curmenu) return false;
    return true;
}

extern void *scoremenu;

void rendermenu()
{
    if(!curmenu) return;
    if(curmenu != scoremenu) clearscope();
    gmenu &m = *curmenu;
    m.refresh();
#ifdef __3DS__
    if(ac3ds_menu_panel() == Ac3dsMenu::Standard)
    {
        renderhandheldmenu(m);
        return;
    }
    extern void *servmenu, *searchmenu;
    if(curmenu == servmenu || curmenu == searchmenu)
    {
        renderhandheldservers(m);
        return;
    }
#endif
    m.render();
}

void mitem::render(int x, int y, int w)
{
    if(isselection()) renderbg(x, y, w, menuselbgcolor);
    else if(bgcolor) renderbg(x, y, w, bgcolor);
}

void mitem::renderbg(int x, int y, int w, color *c)
{
    if(isselection()) blendbox(x-FONTH/4, y-FONTH/6, x+w+FONTH/4, y+FONTH+FONTH/6, false, -1, c);
    else blendbox(x, y, x+w, y+FONTH, false, -1, c);
}

int mitem::execaction(const char *arg1)
{
    int result = 0;
    if(getaction())
    {
        if(curmenu) setcontext("menu", curmenu->name);
        push("arg1", arg1);
        result = execute(getaction());
        pop("arg1");
        resetcontext();
    }
    return result;
}

bool mitem::isselection()
{
    return parent->allowinput
#ifndef __3DS__
           && !parent->hotkeys
#endif
           && parent->items.inrange(parent->menusel) && parent->items[parent->menusel]==this;
}

color mitem::gray(0.2f, 0.2f, 0.2f);
color mitem::white(1.0f, 1.0f, 1.0f);
color mitem::whitepulse(1.0f, 1.0f, 1.0f);

bool mitem::menugreyedout = false;

// text item

struct mitemmanual : mitem
{
    const char *text, *action, *hoveraction, *desc;

    mitemmanual(gmenu *parent, char *text, char *action, char *hoveraction, color *bgcolor, const char *desc) : mitem(parent, bgcolor, mitem::TYPE_MANUAL), text(text), action(action), hoveraction(hoveraction), desc(desc) {}

    virtual int width() { return text_width(text) + (strchr(text, '\n') ? menurighttabwidth : 0); }

    virtual void render(int x, int y, int w)
    {
        int c = greyedout ? 128 : 255;
        mitem::render(x, y, w);
        const char *nl = strchr(text, '\n');
        if(nl)
        {
            string l;
            copystring(l, text, min(MAXSTRLEN, (int)strcspn(text, "\n") + 1));
            draw_text(l, x, y, c, c, c);
            draw_text(nl + 1, x + w - max(menurighttabwidth, text_width(nl + 1)), y, c, c, c);
        }
        else draw_text(text, x, y, c, c, c);
    }

    virtual void focus(bool on)
    {
        if(on && hoveraction) execute(hoveraction);
    }

    virtual int select()
    {
        int result = 0;
        if(action && action[0])
        {
            gmenu *oldmenu = curmenu;
            result = execaction(text);
            if(result >= 0 && oldmenu == curmenu)
            {
                menuset(NULL, false);
                menustack.shrink(0);
            }
        }
        return result;
    }
    virtual const char *getdesc() { return desc; }
    virtual const char *gettext() { return text; }
    virtual const char *getaction() { return action; }
};

struct mitemtext : mitemmanual
{
    mitemtext(gmenu *parent, char *text, char *action, char *hoveraction, color *bgcolor, const char *desc = NULL) : mitemmanual(parent, text, action, hoveraction, bgcolor, desc) {}
    virtual ~mitemtext()
    {
        DELETEA(text);
        DELETEA(action);
        DELETEA(hoveraction);
        DELETEA(desc);
    }
};

VARP(hidebigmenuimages, 0, 0, 1);

struct mitemimagemanual : mitemmanual
{
    Texture *image;
    font *altfont;

    mitemimagemanual(gmenu *parent, const char *filename, const char *altfontname, char *text, char *action, char *hoveraction, color *bgcolor, const char *desc = NULL) : mitemmanual(parent, text, action, hoveraction, bgcolor, desc)
    {
        image = filename ? textureload(filename, 3) : NULL;
        altfont = altfontname ? getfont(altfontname) : NULL;
    }
    virtual ~mitemimagemanual() {}
    virtual int width()
    {
        if(image && *text != '\t') return (FONTH*image->xs)/image->ys + FONTH/2 + mitemmanual::width();
        return mitemmanual::width();
    }
    virtual void render(int x, int y, int w)
    {
        mitem::render(x, y, w);
        if(image || altfont)
        {
            int xs = 0, c = greyedout ? 128 : 255;
            if(image)
            {
                xs = (FONTH*image->xs)/image->ys;
                framedquadtexture(image->id, x, y, xs, FONTH, 0, 255, true);
            }
            draw_text(text, !image || *text == '\t' ? x : x+xs + FONTH/2, y, c, c, c);
            if(altfont && strchr(text, '\a'))
            {
                char *r = newstring(text), *re, *l = r;
                while((re = strchr(l, '\a')) && re[1])
                {
                    *re = '\0';
                    x += text_width(l);
                    l = re + 2;
                    pushfont(altfont->name);
                    draw_textf("%s%c", x, y, greyedout ? "\f4" : "", re[1]);
                    popfont();
                }
                delete[] r;
            }
            if(image && isselection() && !hidebigmenuimages && image->ys > FONTH)
            {
                w += FONTH;
                int xs = (2 * VIRTW - w) / 5, ys = (xs * image->ys) / image->xs;
                x = (6 * VIRTW + w - 2 * xs) / 4; y = VIRTH - ys / 2;
                framedquadtexture(image->id, x, y, xs, ys, FONTH);
            }
        }
        else mitemmanual::render(x, y, w);
    }
};

struct mitemimage : mitemimagemanual
{
    mitemimage(gmenu *parent, const char *filename, char *text, char *action, char *hoveraction, color *bgcolor, const char *desc = NULL, const char *altfont = NULL) : mitemimagemanual(parent, filename, altfont, text, action, hoveraction, bgcolor, desc) {}
    virtual ~mitemimage()
    {
        DELETEA(text);
        DELETEA(action);
        DELETEA(hoveraction);
        DELETEA(desc);
    }
};

VARP(browsefiledesc, 0, 1, 1);

struct mitemmapload : mitemmanual
{
    const char *filename, *mapmessage;
    Texture *image;
    bool ownpreview; // image is this map's own preview, not the shared placeholder

    mitemmapload(gmenu *parent, const char *filename, char *text, char *action, char *hoveraction, const char *desc) : mitemmanual(parent, text, action, hoveraction, NULL, desc), filename(filename), mapmessage(NULL), image(NULL), ownpreview(false) {}

    virtual ~mitemmapload()
    {
        delstring(filename);
        DELETEA(mapmessage);
        DELETEA(text);
        DELETEA(action);
        DELETEA(hoveraction);
        DELETEA(desc);
    }

    virtual void key(int code, bool isdown)
    {
        if(code == SDLK_LEFT && parent->xoffs > -50) parent->xoffs -= 2;
        else if(code == SDLK_RIGHT && parent->xoffs < 50) parent->xoffs += 2;
    }

    void loadpreview()
    {
        if(!image && !mapmessage)
        {
            silent_texture_load = true;
            const char *cgzpath = "packages" PATHDIVS "maps";
            if(browsefiledesc)
            {
                mapmessage = getfiledesc(cgzpath, behindpath(filename), "cgz");
                if(!mapmessage) mapmessage = getfiledesc((cgzpath = "packages" PATHDIVS "maps" PATHDIVS "official"), behindpath(filename), "cgz");
            }
            defformatstring(p2p)("%s/preview/%s.jpg", cgzpath, filename);
            if(!hidebigmenuimages) image = textureload(p2p, 3);
            ownpreview = image && image != notexture;
            if(!image || image == notexture) image = textureload("packages/misc/nopreview.jpg", 3);
            silent_texture_load = false;
        }
#ifdef __3DS__
        // Every preview is 256 KiB of GPU memory and the texture table never
        // lets go of one, so browsing the map lists used to pin up to 11 MiB.
        // Keep only the preview on show; an evicted one reloads when its row
        // is selected again.
        static Texture *resident = NULL;
        if(ownpreview && image)
        {
            if(resident != image)
            {
                if(resident && resident->id)
                {
                    glDeleteTextures(1, &resident->id);
                    resident->id = 0;
                    resident->evicted = true;
                }
                resident = image;
            }
            if(!image->id)
            {
                silent_texture_load = true;
                if(!reloadtexture(*image))
                {
                    resident = NULL;
                    ownpreview = false;
                    image = textureload("packages/misc/nopreview.jpg", 3);
                }
                silent_texture_load = false;
            }
        }
#endif
    }

    virtual void render(int x, int y, int w)
    {
        int c = greyedout ? 128 : 255;
        mitem::render(x, y, w);
        draw_text(text, x, y, c, c, c);
        if(isselection())
        {
            loadpreview();
            w += FONTH;
            int xs = (2 * VIRTW - w - x) / 2, xp = x + w + xs / 2, ym = VIRTH - FONTH/2;
            if(image && !hidebigmenuimages && image->ys > FONTH)
            {
                int ys = (xs * image->ys) / image->xs, yp = VIRTH - ys / 2;
                ym = yp + ys + 2 * FONTH;
                framedquadtexture(image->id, xp, yp, xs, ys, FONTH);
                draw_text(text, xp + xs/2 - text_width(text)/2, yp + ys);
            }
            if(mapmessage && *mapmessage) draw_text(mapmessage, xp - FONTH, ym, 0xFF, 0xFF, 0xFF, 0xFF, -1, 2 * VIRTW - (xp - FONTH) - FONTH/2);
        }
    }
};

// text input item

struct mitemtextinput : mitemtext
{
    textinputbuffer input;
    char *defaultvalueexp;
    bool modified, hideinput;

    mitemtextinput(gmenu *parent, char *text, char *value, char *action, char *hoveraction, color *bgcolor, int maxchars, int maskinput) : mitemtext(parent, text, action, hoveraction, bgcolor), defaultvalueexp(value), modified(false), hideinput(false)
    {
        mitemtype = TYPE_TEXTINPUT;
        copystring(input.buf, value);
        input.max = maxchars > 0 ? min(maxchars, MAXSTRLEN - 1) : 15;
        hideinput = (maskinput != 0);
#ifdef __3DS__
        if(!strncmp(parent->name, "auth", 4) &&
           (strstr(text, "password") || strstr(text, "Password"))) hideinput = true;
#endif
    }

    virtual int width()
    {
        return text_width(text) + menurighttabwidth;
    }

    virtual void render(int x, int y, int w)
    {
        int c = greyedout ? 128 : 255;
        bool sel = isselection();
        if(sel)
        {
            renderbg(x+w-menurighttabwidth, y-FONTH/6, menurighttabwidth, menuselbgcolor);
            renderbg(x, y-FONTH/6, w-menurighttabwidth-FONTH/2, menuseldescbgcolor);
        }
        draw_text(text, x, y, c, c, c);
        int cibl = (int)strlen(input.buf); // current input-buffer length
        int iboff = input.pos > 14 ? (input.pos < cibl ? input.pos - 14 : cibl - 14) : (input.pos == -1 ? (cibl > 14 ? cibl - 14 : 0) : 0); // input-buffer offset
        string showinput, tempinputbuf; int sc = 14;
        copystring(tempinputbuf, input.buf);
        if(hideinput && !(SDL_GetModState() & MOD_KEYS_CTRL))
        { // "mask" user input with asterisks, use for menuitemtextinputs that take passwords
            for(char *c = tempinputbuf; *c; c++)
                *c = '*';
        }
        while(iboff > 0)
        {
            copystring(showinput, tempinputbuf + iboff - 1, sc + 2);
            if(text_width(showinput) > menurighttabwidth) break;
            iboff--; sc++;
        }
        while(iboff + sc < cibl)
        {
            copystring(showinput, tempinputbuf + iboff, sc + 2);
            if(text_width(showinput) > menurighttabwidth) break;
            sc++;
        }
        copystring(showinput, tempinputbuf + iboff, sc + 1);
        draw_text(showinput, x+w-menurighttabwidth, y, c, c, c, 255, sel && !greyedout ? (input.pos>=0 ? (input.pos > sc ? sc : input.pos) : cibl) : -1);
    }

    virtual void focus(bool on)
    {
        if(on && hoveraction) execute(hoveraction);

        textinput(on, TI_MENU);
        if(action && !on && modified && parent->items.find(this) != parent->items.length() - 1)
        {
            modified = false;
            execaction(input.buf);
        }
    }

    virtual void key(int code, bool isdown)
    {
        if(input.key(code)) modified = true;
        if(action && code == SDLK_RETURN && modified && parent->items.find(this) != parent->items.length() - 1)
        {
            modified = false;
            execaction(input.buf);
        }
    }

    void say(const char *text)
    {
        if(input.say(text)) modified = true;
    }

    virtual void init()
    {
        setdefaultvalue();
        modified = false;
    }

    virtual int select()
    {
#ifdef __3DS__
        string edited;
        if(!ac3ds_edit_text(hideinput ? "Password" : text, input.buf, edited, sizeof(edited),
                            input.maxlen(), hideinput))
            return -1;
        string previous;
        copystring(previous, input.buf);
        input.buf[0] = '\0';
        input.pos = -1;
        input.say(edited);
        modified |= strcmp(previous, input.buf) != 0;
#endif
        int result = 0;
        if(parent->menusel == parent->items.length() - 1)
        {
            const char *tmp = text;
            text = input.buf;
            result = mitemmanual::select();
            text = tmp;
            textinput(result, TI_MENU);
        }
#ifdef __3DS__
        else if(action && modified)
        {
            modified = false;
            execaction(input.buf);
        }
#endif
        return result;
    }

    void setdefaultvalue()
    {
        const char *p = defaultvalueexp;
        char *r = executeret(p);
        copystring(input.buf, r ? r : "");
        if(r) delete[] r;
    }
};

// slider item
VARP(wrapslider, 0, 0, 1);

struct mitemslider : mitem
{
    int min_, max_, step, value, maxvaluewidth;
#ifdef __3DS__
    int handheld_scale = 1;
#endif
    char *text, *valueexp, *action;
    string curval;
    vector<char *> opts;
    bool wrap, isradio;

    mitemslider(gmenu *parent, char *text, int _min, int _max, char *value, char *display, char *action, color *bgcolor, bool wrap, bool isradio) : mitem(parent, bgcolor, mitem::TYPE_SLIDER),
                               min_(_min), max_(_max), value(_min), maxvaluewidth(0), text(text), valueexp(value), action(action), wrap(wrap), isradio(isradio)
    {
        char *enddigits;
        step = (int) strtol(display, &enddigits, 10);
        if(!*display || *enddigits)
        {
            step = 1;
            explodelist(display, opts);
            if(max_ - min_ + 1 != opts.length())
            {
                if(max_ != -1) clientlogf("menuitemslider: display string length (%d) doesn't match max-min (%d) \"%s\" [%s]", opts.length(), max_ - min_ + 1, text, display);
                max_ = min_ + opts.length() - 1;
            }
        }
        getmaxvaluewidth();
    }

    virtual ~mitemslider()
    {
        opts.deletearrays();
        DELETEA(text);
        DELETEA(valueexp);
        DELETEA(action);
    }

    virtual int width() { return text_width(text) + (isradio ? 0 : menurighttabwidth) + maxvaluewidth + 2*FONTH; }

    virtual void render(int x, int y, int w)
    {
        bool sel = isselection();
        int range = max_-min_;
        int cval = value-min_;

        int tw = text_width(text), ow = isradio ? text_width(curval) : menurighttabwidth, pos = !isradio || ow < menurighttabwidth ? menurighttabwidth : ow;
        if(sel)
        {
            renderbg(x + w - pos, y, ow, menuselbgcolor);
            renderbg(x, y, w - pos - FONTH/2, menuseldescbgcolor);
            if(pos - ow > FONTH/2) renderbg(x + w - pos + ow + FONTH/2, y, pos - ow - FONTH/2, menuseldescbgcolor);
        }
        int c = greyedout ? 128 : 255;
        draw_text(text, x, y, c, c, c);
        draw_text(curval, x + (isradio ? w - pos : tw), y, c, c, c);

        if(!isradio)
        {
            blendbox(x+w-menurighttabwidth, y+FONTH/3, x+w, y+FONTH*2/3, false, -1, &gray);
            int offset = (int)(cval/((float)range)*menurighttabwidth);
            blendbox(x+w-menurighttabwidth+offset-FONTH/6, y, x+w-menurighttabwidth+offset+FONTH/6, y+FONTH, false, -1, greyedout ? &gray : (sel ? &whitepulse : &white));
        }
    }

    virtual void key(int code, bool isdown)
    {
        if(code == SDLK_LEFT) slide(false);
        else if(code == SDLK_RIGHT) slide(true);
    }

    virtual void init()
    {
        const char *p = valueexp;
        char *v = executeret(p);
        if(v)
        {
            value = clamp(int(ATOI(v)), min_, max_);
            delete[] v;
        }
        displaycurvalue();
    }

    void slide(bool right)
    {
        value += right ? step : -step;
        if (wrapslider || wrap)
        {
            if (value > max_) value = min_;
            if (value < min_) value = max_;
        }
        else value = min(max_, max(min_, value));
        displaycurvalue();
        if(action)
        {
            string v;
            itoa(v, value);
            execaction(v);
        }
    }

    void displaycurvalue()
    {
        if(isradio)
        {
            *curval = '\0';
            loopv(opts) concatformatstring(curval, "%s\1%c %s", i ? "  " : "", (i + min_ == value) ? '\11' : '\10', opts[i]);
        }
        else if(opts.length()) // extract display name from list
        {
            int idx = value - min_;
            copystring(curval, opts.inrange(idx) ? opts[idx] : "");
        }
        else itoa(curval, value); // display number only
    }

    void getmaxvaluewidth()
    {
        int oldvalue = value;
        maxvaluewidth = 0;
        for(int v = min_; v <= max_; v++)
        {
            value = v;
            displaycurvalue();
            maxvaluewidth = max(text_width(curval), maxvaluewidth);
        }
        value = oldvalue;
        displaycurvalue();
    }

    virtual const char *gettext() { return text; }
    virtual const char *getaction() { return action; }
};

// key input item

struct mitemkeyinput : mitem
{
    char *text, *bindcmd;
    const char *keyname;
    vector<int> newkeys;
    int isup, bindtype;
    string keynames;
    static const char *unknown, *empty;
    bool capture;

    mitemkeyinput(gmenu *parent, char *text, char *bindcmd, color *bgcolor, int bindtype) : mitem(parent, bgcolor, mitem::TYPE_KEYINPUT), text(text), bindcmd(bindcmd),
                                                                              keyname(NULL), isup(0), bindtype(bindtype), capture(false) {};

    ~mitemkeyinput()
    {
        DELETEA(text);
        DELETEA(bindcmd);
    }

    virtual int width() { return text_width(text)+text_width(keyname ? keyname : " "); }

    virtual void render(int x, int y, int w)
    {
        int tw = text_width(keyname ? keyname : " "), c = greyedout ? 128 : 255;
        static color capturec(0.4f, 0, 0);
        if(isselection())
        {
            blendbox(x+w-tw-FONTH, y-FONTH/6, x+w+FONTH, y+FONTH+FONTH/6, false, -1, capture ? &capturec : NULL);
            blendbox(x, y-FONTH/6, x+w-tw-FONTH, y+FONTH+FONTH/6, false, -1, menuseldescbgcolor);
        }
        draw_text(text, x, y, c, c, c);
        draw_text(keyname, x+w-tw, y, c, c, c);
    }

    virtual void init()
    {
        keynames[0] = 0;
        for(keym **km = findbinda(bindcmd, bindtype); *km; km++)
        {
#ifdef __3DS__
            // Lower-screen touch keys are fixed match-screen buttons, not rebindable buttons.
            if(!ac3ds_is_physical_button((*km)->code)) continue;
#endif
            concatformatstring(keynames, " or %s", (*km)->name);
        }
        keyname = *keynames ? keynames + 4 : unknown;
        capture = false;
        menuheader(parent, NULL, NULL);
    }

    virtual int select()
    {
        capture = true;
        isup = 0;
        keyname = empty;
        newkeys.setsize(0);
        keynames[0] = '\0';
        return 0;
    }

    virtual void key(int code, bool isdown)
    {
        keym *km;
        if(!capture || !((km = findbindc(code)))) return;
        if(code == SDLK_ESCAPE)
        {
            capture = false;
            parent->init();
            return;
        }
        bool known = newkeys.find(code) >= 0;
        if(isdown)
        {
            if(!known)
            {
                if(km->actions[bindtype][0] && strcmp(km->actions[bindtype], bindcmd))
                {
#ifdef __3DS__
                    static defformatstring(keybound)("\f3Warning: \"%s\" is already in use. Press START to cancel.", km->name);
#else
                    static defformatstring(keybound)("\f3Warning: \"%s\" key is already in use. You can press ESC to cancel.", km->name);
#endif
                    menuheader(parent, NULL, keybound);
                }
                newkeys.add(code);
                concatformatstring(keynames, "%s or ", km->name);
                keyname = keynames;
            }
        }
        else
        {
            if(known && ++isup == newkeys.length())
            {
                for(keym **km = findbinda(bindcmd, bindtype); *km; km++)  // clear existing binds to this cmd
                {
#ifdef __3DS__
                    // Keep the lower-screen touch buttons bound to their actions.
                    if(!ac3ds_is_physical_button((*km)->code)) continue;
#endif
                    bindkey(*km, "", bindtype);
                }
                bool bindfailed = false;
                loopv(newkeys) bindfailed = !bindc(newkeys[i], bindcmd, bindtype) || bindfailed;
                if(bindfailed) conoutf("\f3could not bind key");
                else parent->init(); // show new keys
            }
        }
    }

    virtual const char* gettext() { return text; }
};

const char *mitemkeyinput::unknown = "?";
const char *mitemkeyinput::empty = " ";

// checkbox menu item

struct mitemcheckbox : mitem
{
    char *text, *valueexp, *action;
    int pos;
    bool checked;

    mitemcheckbox(gmenu *parent, char *text, char *value, char *action, int pos, color *bgcolor) : mitem(parent, bgcolor, mitem::TYPE_CHECKBOX), text(text), valueexp(value), action(action), pos(pos), checked(false) {}

    ~mitemcheckbox()
    {
        DELETEA(text);
        DELETEA(valueexp);
        DELETEA(action);
    }

    virtual int width() { return text_width(text) + (pos ? menurighttabwidth : FONTH + FONTH/3); }

    virtual int select()
    {
        checked = !checked;
        return execaction(checked ? "1" : "0");
    }

    virtual void init()
    {
        const char *p = valueexp;
        char *r = executeret(p);
        checked = (r && atoi(r) > 0);
        if(r) delete[] r;
    }

    virtual void render(int x, int y, int w)
    {
        bool sel = isselection();
        const static int boxsize = FONTH;
        int offs = ((menurighttabwidth - FONTH) * ((msctrl % 41) ? pos : (50 + 50 * sinf(totalmillis / 300.0f + y)))) / 100;
        int c = greyedout ? 128 : 255;

        if(sel)
        {
            renderbg(x, y, w-boxsize-offs-FONTH/2, menuseldescbgcolor);
            renderbg(x+w-boxsize-offs, y, boxsize, menuselbgcolor);
            if(offs > FONTH/2) renderbg(x+w-offs+FONTH/2, y, offs-FONTH/2, menuseldescbgcolor);
        }
        draw_text(text, x, y, c, c, c);
        x -= offs;
        blendbox(x+w-boxsize, y, x+w, y+boxsize, false, -1, &gray);
        color *col = greyedout ? &gray : (sel ? &whitepulse : &white);
        if(checked)
        {
            int x1 = x+w-boxsize-FONTH/6, x2 = x+w+FONTH/6, y1 = y-FONTH/6, y2 = y+boxsize+FONTH/6;
            line(x1, y1, x2, y2, col);
            line(x2, y1, x1, y2, col);
        }
    }

    virtual const char* gettext() { return text; }

    virtual const char *getaction() { return action; }
};


// console iface

void *addmenu(const char *name, const char *title, bool allowinput, void (__cdecl *refreshfunc)(void *, bool), bool (__cdecl *keyfunc)(void *, int, bool), bool hotkeys, bool forwardkeys)
{
    gmenu *m = menus.access(name);
    if(!m)
    {
        name = newstring(name);
        m = &menus[name];
        m->name = name;
        m->initaction = NULL;
        m->header = m->footer = NULL;
        m->menusel = 0;
    }
    m->title = title;
    DELSTRING(m->mdl);
    m->allowinput = allowinput;
    m->inited = false;
    m->hotkeys = hotkeys;
    m->refreshfunc = refreshfunc;
    m->keyfunc = keyfunc;
    DELSTRING(m->usefont);
    m->allowblink = false;
    m->forwardkeys = forwardkeys;
#ifdef __3DS__
    if(curmenu == m) ++handheld_menu_revision;
    m->handheld_weapon_picker = false;
#endif
    lastmenu = m;
    mitem::menugreyedout = false;
    return m;
}

void newmenu(char *name, int *hotkeys, int *forwardkeys)
{
    addmenu(name, NULL, true, NULL, NULL, *hotkeys > 0, *forwardkeys > 0);
}
COMMAND(newmenu, "sii");

void menureset(void *menu)
{
    gmenu &m = *(gmenu *)menu;
#ifdef __3DS__
    ac3ds_invalidate_menu_touch(menu);
    m.handheld_weapon_picker = false;
#endif
    m.items.deletecontents();
}

void delmenu(const char *name)
{
    if (!name) return;
    gmenu *m = menus.access(name);
    if (!m) return;
    else menureset(m);
}
COMMAND(delmenu, "s");

void menuitemmanual(void *menu, char *text, char *action, color *bgcolor, const char *desc)
{
    gmenu &m = *(gmenu *)menu;
    m.items.add(new mitemmanual(&m, text, action, NULL, bgcolor, desc));
}

void menuitemcopy(void *menu, const char *text, const char *action, const char *desc)
{
    gmenu &m = *static_cast<gmenu *>(menu);
    m.items.add(new mitemtext(&m, newstring(text), action ? newstring(action) : NULL, NULL, NULL, desc ? newstring(desc) : NULL));
}

void menuimagemanual(void *menu, const char *filename, const char *altfontname, char *text, char *action, color *bgcolor, const char *desc)
{
    gmenu &m = *(gmenu *)menu;
    m.items.add(new mitemimagemanual(&m, filename, altfontname, text, action, NULL, bgcolor, desc));
}

void menutitlemanual(void *menu, const char *title)
{
    gmenu &m = *(gmenu *)menu;
    m.title = title;
}

void menuheader(void *menu, char *header, char *footer, bool heap)
{
    gmenu &m = *(gmenu *)menu;
    if(m.headfootheap) { DELSTRING(m.header); DELSTRING(m.footer); }
    m.header = (header && *header) || heap ? header : NULL;
    m.footer = (footer && *footer) || heap ? footer : NULL;
    m.headfootheap = heap; // false: "manual"
}

COMMANDF(menuheader, "ss", (char *header, char *footer) { if(lastmenu) menuheader(lastmenu, *header ? newstring(header) : NULL, *footer ? newstring(footer) : NULL, true); });

void menufont(char *menu, const char *usefont)
{
    gmenu *m = *menu ? menus.access(menu) : lastmenu;
    if(m)
    {
        DELSTRING(m->usefont);
        m->usefont = usefont && *usefont ? newstring(usefont) : NULL;
    }
}
COMMAND(menufont, "ss");

void setmenublink(int *truth)
{
    if(!lastmenu) return;
    gmenu *m = lastmenu;
    m->allowblink = *truth != 0;
}
COMMANDN(menucanblink, setmenublink, "i");

void menutitle(char *title)
{
    if(!lastmenu) return;
    gmenu *m = lastmenu;
    DELSTRING(m->title);
    m->title = newstring(title);
}
COMMAND(menutitle, "s");

void menuinit(char *initaction)
{
    if(!lastmenu) return;
    DELSTRING(lastmenu->initaction);
    lastmenu->initaction = newstring(initaction);
}
COMMAND(menuinit, "s");

void menuinitselection(int *line)
{
    if(!lastmenu) return;
    if(lastmenu->items.inrange(*line)) lastmenu->menusel = *line;
    else lastmenu->menuselinit = *line;
}
COMMAND(menuinitselection, "i");

void menuselectionpersistent()
{
    if(!curmenu) return;
    curmenu->persistentselection = true;
    const char *val = getalias(persistentmenuselectionalias(curmenu->name));
    if(val) menuselect(curmenu, ATOI(val));
}
COMMAND(menuselectionpersistent, "");

void menusynctabstops(int *onoff)
{
    if(curmenu) curmenu->synctabstops = *onoff != 0;
}
COMMAND(menusynctabstops, "i");

void menurenderoffset(int *xoff, int *yoff)
{
    if(!lastmenu) return;
    lastmenu->xoffs = *xoff;
    lastmenu->yoffs = *yoff;
}
COMMAND(menurenderoffset, "ii");

void menuselection(char *menu, int *line)
{
    if(!menu || !menus.access(menu)) return;
    gmenu &m = menus[menu];
    menuselect(&m, *line);
}
COMMAND(menuselection, "si");

void menuitemgreyedout(int *onoff)
{
    mitem::menugreyedout = *onoff != 0;
}
COMMAND(menuitemgreyedout, "i");

void menuitem(char *text, char *action, char *hoveraction, char *desc)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemtext(lastmenu, newstring(text), newstring(action[0] ? action : text), hoveraction[0] ? newstring(hoveraction) : NULL, NULL, *desc ? newstring(desc) : NULL));
}
COMMAND(menuitem, "ssss");

void menuitemimage(char *name, char *text, char *action, char *hoveraction)
{
    if(!lastmenu) return;
    if(fileexists(name, "r") || findfile(name, "r") != name)
        lastmenu->items.add(new mitemimage(lastmenu, name, newstring(text), action[0] ? newstring(action) : NULL, hoveraction[0] ? newstring(hoveraction) : NULL, NULL));
    else
        lastmenu->items.add(new mitemtext(lastmenu, newstring(text), newstring(action[0] ? action : text), hoveraction[0] ? newstring(hoveraction) : NULL, NULL));
}
COMMAND(menuitemimage, "ssss");

void menuitemaltfont(char *altfont, char *text, char *action, char *hoveraction, char *desc)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemimage(lastmenu, NULL, newstring(text), action[0] ? newstring(action) : NULL, hoveraction[0] ? newstring(hoveraction) : NULL, NULL, desc[0] ? newstring(desc) : NULL, altfont));
}
COMMAND(menuitemaltfont, "sssss");

void menuitemmapload(char *name, char *action, char *hoveraction, char *desc)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemmapload(lastmenu, newstring(name), newstring(name), *action ? newstring(action) : NULL, *hoveraction ? newstring(hoveraction) : NULL, *desc ? newstring(desc) : NULL));
}
COMMAND(menuitemmapload, "ssss");

void menuitemtextinput(char *text, char *value, char *action, char *hoveraction, int *maxchars, int *maskinput)
{
    if(!lastmenu || !text || !value) return;
    lastmenu->items.add(new mitemtextinput(lastmenu, newstring(text), newstring(value), action[0] ? newstring(action) : NULL, hoveraction[0] ? newstring(hoveraction) : NULL, NULL, *maxchars, *maskinput));
}
COMMAND(menuitemtextinput, "ssssii");

void menuitemslider(char *text, int *min_, int *max_, char *value, char *display, char *action, int *wrap
#ifdef __3DS__
                    , int *scale
#endif
                    )
{
    if(!lastmenu) return;
    auto *slider = new mitemslider(lastmenu, newstring(text), *min_, *max_, newstring(value), display,
                                  action[0] ? newstring(action) : NULL, NULL, *wrap != 0, false);
#ifdef __3DS__
    slider->handheld_scale = max(*scale, 1);
#endif
    lastmenu->items.add(slider);
}
#ifdef __3DS__
COMMAND(menuitemslider, "siisssii");
#else
COMMAND(menuitemslider, "siisssi");
#endif

void menuitemradio(char *text, int *min_, int *max_, char *value, char *display, char *action)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemslider(lastmenu, newstring(text), *min_, *max_, newstring(value), display, action[0] ? newstring(action) : NULL, NULL, true, true));
}
COMMAND(menuitemradio, "siisss");

void menuitemcheckbox(char *text, char *value, char *action, int *pos)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemcheckbox(lastmenu, newstring(text), newstring(value), action[0] ? newstring(action) : NULL, clamp(*pos, 0, 100), NULL));
}
COMMAND(menuitemcheckbox, "sssi");

void menuitemkeyinput(char *text, char *bindcmd)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemkeyinput(lastmenu, newstring(text), newstring(bindcmd), NULL, keym::ACTION_DEFAULT));
}
COMMAND(menuitemkeyinput, "ss");

void menuitemeditkeyinput(char *text, char *bindcmd)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemkeyinput(lastmenu, newstring(text), newstring(bindcmd), NULL, keym::ACTION_EDITING));
}
COMMAND(menuitemeditkeyinput, "ss");

void menuitemspectkeyinput(char *text, char *bindcmd)
{
    if(!lastmenu) return;
    lastmenu->items.add(new mitemkeyinput(lastmenu, newstring(text), newstring(bindcmd), NULL, keym::ACTION_SPECTATOR));
}
COMMAND(menuitemspectkeyinput, "ss");

void menumdl(char *menu, char *mdl, char *anim, int *rotspeed, int *scale)
{
    gmenu *m = *menu ? menus.access(menu) : lastmenu;
    if(!m) return;
    DELETEA(m->mdl);
    if(!*mdl) return;
    m->mdl = newstring(mdl);
    m->anim = findanim(anim)|ANIM_LOOP;
    m->rotspeed = clamp(*rotspeed, 0, 100);
    m->scale = clamp(*scale, 0, 100);
}
COMMAND(menumdl, "sssii");

void menudirlist(char *dir, char *ext, char *action, int *image, char *searchfile)
{
    if(!lastmenu || !*action) return;
    if(lastmenu->dirlist) delete lastmenu->dirlist;
    mdirlist *d = lastmenu->dirlist = new mdirlist;
    d->dir = newstring(dir);
    d->ext = ext[0] ? newstring(ext): NULL;
    d->action = action[0] ? newstring(action) : NULL;
    d->image = *image!=0;
    d->searchfile = searchfile[0] ? newstring(searchfile) : NULL;
    d->subdiraction = NULL;
    d->subdirdots = false;
}
COMMAND(menudirlist, "sssis");

void menudirlistsub(char *action, int *dots)
{
    if(!lastmenu || !lastmenu->dirlist || !*action) return;
    mdirlist *d = lastmenu->dirlist;
    d->subdirdots = *dots != 0;
    DELETEA(d->subdiraction);
    d->subdiraction = *action ? newstring(action) : NULL;
}
COMMAND(menudirlistsub, "si");

void chmenutexture(char *menu, char *texname, char *title)
{
    gmenu *m = *menu ? menus.access(menu) : NULL;
    if(!m) return;
    DELETEA(m->previewtexture);
    if(*texname) m->previewtexture = newstring(texname);
    DELETEA(m->previewtexturetitle);
    if(*title) m->previewtexturetitle = newstring(title);
}
COMMAND(chmenutexture, "sss");

bool parsecolor(color *col, const char *r, const char *g, const char *b, const char *a)
{
    if(!r[0] || !col) return false;    // four possible parameter schemes:
    if(!g[0]) g = b = r;               // grey
    if(!b[0]) { a = g; g = b = r; }    // grey alpha
    col->r = ((float) atoi(r)) / 100;  // red green blue
    col->g = ((float) atoi(g)) / 100;  // red green blue alpha
    col->b = ((float) atoi(b)) / 100;
    col->alpha = a[0] ? ((float) atoi(a)) / 100 : 1.0;
    return true;
}

void menuselectionbgcolor(char *r, char *g, char *b, char *a)
{
    if(!menuselbgcolor) menuselbgcolor = new color;
    if(!r[0]) { DELETEP(menuselbgcolor); return; }
    parsecolor(menuselbgcolor, r, g, b, a);
}
COMMAND(menuselectionbgcolor, "ssss");

void menuselectiondescbgcolor(char *r, char *g, char *b, char *a)
{
    if(!menuseldescbgcolor) menuseldescbgcolor = new color;
    if(!r[0]) { DELETEP(menuseldescbgcolor); return; }
    parsecolor(menuseldescbgcolor, r, g, b, a);
}
COMMAND(menuselectiondescbgcolor, "ssss");

static bool iskeypressed(int key)
{
    int numkeys = 0;
    Uint8 const* state = SDL_GetKeyboardState(&numkeys);
    return key < numkeys && state[key] != 0;
}

void menusay(const char *text)
{
    if(curmenu && curmenu->allowinput && curmenu->items.inrange(curmenu->menusel)) curmenu->items[curmenu->menusel]->say(text);
}

#ifdef __3DS__
Ac3dsMenu ac3ds_menu_panel()
{
    if(!curmenu || !curmenu->allowinput) return Ac3dsMenu::None;
    extern void *servmenu, *searchmenu, *serverinfomenu;
    if(curmenu == servmenu || curmenu == searchmenu) return Ac3dsMenu::ServerBrowser;
    return curmenu == serverinfomenu ? Ac3dsMenu::ServerInfo : Ac3dsMenu::Standard;
}

const void *ac3ds_menu_identity()
{
    return curmenu && curmenu->allowinput ? curmenu : NULL;
}

void ac3ds_invalidate_menu_touch(const void *menu)
{
    if(menu && curmenu == menu) ++handheld_menu_revision;
}

// Strip Cube text formatting before drawing with the lower screen's pixel font.
static void handheldtext(char *out, size_t capacity, const char *text)
{
    size_t length = 0;
    if(text) for(; *text && length + 1 < capacity; ++text)
    {
        unsigned char c = *text;
        if(c == '\f' || c == '\a' || c == '\1' || c == '\v')
        {
            if(text[1]) ++text;
            continue;
        }
        if(c <= ' ')
        {
            if(length && out[length-1] != ' ') out[length++] = ' ';
        }
        else out[length++] = c < 127 ? c : '?';
    }
    if(length && out[length-1] == ' ') --length;
    out[length] = '\0';
}

static bool handheldmenurow(mitem &item)
{
    const char *label = item.gettext();
    return label && *label;
}

static int handheldmenupage(const gmenu &menu, int direction)
{
    int total = 0, selection = 0;
    loopv(menu.items) if(handheldmenurow(*menu.items[i]))
    {
        if(i <= menu.menusel) selection = total;
        ++total;
    }
    if(!total) return menu.menusel;
    constexpr int size = Ac3dsMenuView::row_count;
    const int page = selection / size;
    const int target = clamp(page + direction, 0, (total - 1) / size);
    if(target == page) return menu.menusel;
    int row = target * size;
    loopv(menu.items) if(handheldmenurow(*menu.items[i]) && row-- == 0) return i;
    return menu.menusel;
}

struct HandheldPrimary
{
    int gun;
    const char *label;
};

static constexpr HandheldPrimary handheldprimaries[] =
{
    {GUN_ASSAULT, "ASSAULT"},
    {GUN_SUBGUN, "SMG"},
    {GUN_SNIPER, "SNIPER"},
    {GUN_SHOTGUN, "SHOTGUN"},
    {GUN_CARBINE, "CARBINE"}
};
static constexpr int handheldprimarycount = sizeof(handheldprimaries) / sizeof(handheldprimaries[0]);
static_assert(handheldprimarycount + 1 == Ac3dsMenuView::row_count, "The last touch cell is Back");

static bool handheldweaponpicker(const gmenu &menu)
{
    return menu.handheld_weapon_picker && menu.items.length() == handheldprimarycount;
}

static int handheldprimaryselection()
{
    if(player1) loopi(handheldprimarycount)
        if(handheldprimaries[i].gun == player1->nextprimary) return i;
    return 0;
}

static const char *handheldmenutitle(const gmenu &menu)
{
    return !strcmp(menu.name, "main") ? "AssaultCube 3DS" : menu.title ? menu.title : menu.name;
}

static bool handheldmenuentry(Ac3dsMenuRow &entry, mitem &item, int i, int primaryselection)
{
    entry.index = i;
    entry.disabled = item.greyedout;
    handheldtext(entry.label, sizeof(entry.label), item.gettext());
    if(primaryselection >= 0)
        copystring(entry.value, i == primaryselection ? "Selected" : guns[handheldprimaries[i].gun].title, sizeof(entry.value));
    switch(item.mitemtype)
    {
        case mitem::TYPE_SLIDER:
        {
            mitemslider &slider = static_cast<mitemslider &>(item);
            entry.kind = Ac3dsMenuItem::Slider;
            const int choice = slider.value - slider.min_;
            if(slider.opts.inrange(choice)) handheldtext(entry.value, sizeof(entry.value), slider.opts[choice]);
            else if(slider.handheld_scale > 1)
                snprintf(entry.value, sizeof(entry.value), "%.1f", double(slider.value) / slider.handheld_scale);
            else snprintf(entry.value, sizeof(entry.value), "%d", slider.value);
            break;
        }
        case mitem::TYPE_CHECKBOX:
            entry.kind = Ac3dsMenuItem::Toggle;
            copystring(entry.value, static_cast<mitemcheckbox &>(item).checked ? "On" : "Off", sizeof(entry.value));
            break;
        case mitem::TYPE_TEXTINPUT:
        {
            mitemtextinput &field = static_cast<mitemtextinput &>(item);
            entry.kind = Ac3dsMenuItem::Text;
            handheldtext(entry.value, sizeof(entry.value), field.hideinput && *field.input.buf ? "********" : field.input.buf);
            break;
        }
        case mitem::TYPE_KEYINPUT:
        {
            mitemkeyinput &binding = static_cast<mitemkeyinput &>(item);
            entry.kind = Ac3dsMenuItem::Binding;
            handheldtext(entry.value, sizeof(entry.value), binding.keyname);
            return binding.capture;
        }
        default:
        {
            const char *action = item.getaction();
            entry.kind = action && *action && strcmp(action, "-1") ? Ac3dsMenuItem::Action : Ac3dsMenuItem::ReadOnly;
            break;
        }
    }
    return false;
}

void ac3ds_menu_view(Ac3dsMenuView &view)
{
    if(ac3ds_menu_panel() != Ac3dsMenu::Standard) return;
    gmenu &menu = *curmenu;
    view.revision = handheld_menu_revision;
    view.selection = menu.menusel;
    view.back = !menustack.empty();
    view.weapon_picker = handheldweaponpicker(menu);
    view.mode_loadout = view.weapon_picker && (m_noprimary || m_osok);
    handheldtext(view.title, sizeof(view.title), handheldmenutitle(menu));

    constexpr int size = Ac3dsMenuView::row_count;
    int count = 0, selected = 0;
    loopv(menu.items) if(handheldmenurow(*menu.items[i]))
    {
        if(i <= menu.menusel) selected = count;
        ++count;
    }
    view.page = selected / size;
    view.pages = (count + size - 1) / size;
    const int primaryselection = view.weapon_picker ? handheldprimaryselection() : -1;
    int skip = view.page * size, row = 0;
    loopv(menu.items)
    {
        mitem &item = *menu.items[i];
        if(!handheldmenurow(item) || skip-- > 0) continue;
        Ac3dsMenuRow &entry = view.rows[row++];
        const bool capturing = handheldmenuentry(entry, item, i, primaryselection);
        view.binding |= capturing && i == menu.menusel;
        if(row == size) break;
    }
}

void ac3ds_menu_touch(unsigned revision, int index, Ac3dsMenuTouch action)
{
    if(getcurcommand(NULL) || ac3ds_menu_panel() != Ac3dsMenu::Standard || revision != handheld_menu_revision) return;
    gmenu *menu = curmenu;
    if(menu->items.inrange(menu->menusel))
    {
        mitem *selected = menu->items[menu->menusel];
        if(selected->mitemtype == mitem::TYPE_KEYINPUT && static_cast<mitemkeyinput *>(selected)->capture)
        {
            if(action == Ac3dsMenuTouch::Back) menukey(SDLK_ESCAPE);
            return;
        }
    }
    switch(action)
    {
        case Ac3dsMenuTouch::Back: menukey(SDLK_ESCAPE); return;
        case Ac3dsMenuTouch::Previous: menukey(SDLK_PAGEUP); return;
        case Ac3dsMenuTouch::Next: menukey(SDLK_PAGEDOWN); return;
        default: break;
    }
    if(!menu->items.inrange(index) || !handheldmenurow(*menu->items[index])) return;
    mitem *item = menu->items[index];
    menuselect(menu, index);
    // Focus callbacks can open another menu or rebuild its entries.
    if(revision != handheld_menu_revision || !menu->items.inrange(index) || menu->items[index] != item || item->greyedout) return;
    switch(action)
    {
        case Ac3dsMenuTouch::Decrease: menukey(SDLK_LEFT); break;
        case Ac3dsMenuTouch::Increase: menukey(SDLK_RIGHT); break;
        case Ac3dsMenuTouch::Activate:
            if(item->mitemtype != mitem::TYPE_SLIDER &&
               (item->mitemtype != mitem::TYPE_MANUAL || (item->getaction() && strcmp(item->getaction(), "-1"))))
                menukey(SDLK_RETURN, false);
            break;
        default: break;
    }
}

void ac3ds_install_handheld_menus()
{
    delmenu("Weapons");
    gmenu *weapons = static_cast<gmenu *>(addmenu("Weapons", NULL, true));
    execute("menutitle [Primary weapon]; menuinit []; menuheader [] [Choose your preferred primary.]");
    for(const HandheldPrimary &choice : handheldprimaries)
    {
        const guninfo &gun = guns[choice.gun];
        defformatstring(action)("nextprimary %s; closemenu Weapons", gun.modelname);
        defformatstring(preview)("menumdl Weapons weapons/%s/menu mapmodel 75 12", gun.modelname);
        weapons->items.add(new mitemtext(weapons, newstring(choice.label),
            newstring(action), newstring(preview), NULL, newstring(gun.title)));
    }
    weapons->handheld_weapon_picker = true;

    execute(R"CFG(
        delmenu welcome
        newmenu welcome
        menutitle "Welcome"
        menuinit []
        menuinitselection 0
        menuheader "" "Set your nickname and screen brightness before playing."
        menuitemtextinput "Nickname" [ if (strcmp $curname "unarmed") [ result "" ] [ result $curname ] ] [ name $arg1 ]
        menuitemslider "Brightness" 70 130 "$gamma" 1 [ gamma $arg1 ]
        menuitem "Continue" [ closecurmenu; showmenu [auth setup] ]

        newmenu "Match menu"
        menuitem "Resume match" [ paused 0; closecurmenu ]
        menuitem "Chat" [ closecurmenu; saycommand ]
        menuitem "Team chat" [ closecurmenu; key_teamchat ]
        menuitem "Voice commands" [ key_showmenuvoicecom ]
        menuitem "Vote yes" [ closecurmenu; key_voteyes ]
        menuitem "Vote no" [ closecurmenu; key_voteno ]
        menuitem "Toggle full map" [ closecurmenu; showscores 0; showmap (! $showmap) ]
        menuitem "Drop flag" [ closecurmenu; dropflag ]
        menuitem "Primary weapon" [ showmenu Weapons ]
        menuitem "Handheld controls" [ showmenu [Handheld controls] ]
        menuitem "Settings" [ showmenu Settings ]
        menuitem "Screenshot" [ closecurmenu; sleep 100 [ screenshot ] ]
        menuitem "Command line" [ closecurmenu; key_commandline ]
        menuitem "Edit map" [ closecurmenu; edittoggle ]
        menuitem "Main menu" [ showmenu main ]

        // Online play never pauses; this menu only runs ordinary client commands.
        newmenu "Match menu online"
        menutitle "Match menu"
        menuitem "Resume" [ closecurmenu ]
        menuitem "Chat" [ closecurmenu; saycommand ]
        menuitem "Team chat" [ closecurmenu; key_teamchat ]
        menuitem "Voice commands" [ key_showmenuvoicecom ]
        menuitem "Vote yes" [ closecurmenu; key_voteyes ]
        menuitem "Vote no" [ closecurmenu; key_voteno ]
        menuitem "Toggle full map" [ closecurmenu; showscores 0; showmap (! $showmap) ]
        menuitem "Drop flag" [ closecurmenu; dropflag ]
        menuitem "Primary weapon" [ showmenu Weapons ]
        menuitem "Scores" [ closecurmenu; showscores 1 ]
        menuitem "Server / team menu" [ ingamemenu ]
        menuitem "Handheld controls" [ showmenu [Handheld controls] ]
        menuitem "Settings" [ showmenu Settings ]
        menuitem "Screenshot" [ closecurmenu; sleep 100 [ screenshot ] ]
        menuitem "Command line" [ closecurmenu; key_commandline ]
        menuitem "Disconnect" [ closecurmenu; disconnect ]

        delmenu "\fsVideo"
        newmenu "\fsVideo"
        menutitle "Video"
        menuitemslider "Brightness" 70 130 "$gamma" 1 [ gamma $arg1 ]
        menuitemslider "Field of view" 75 120 "$fov" 2 [ fov $arg1 ]
        menuitemslider "Graphics detail" 1 6 "$gfxqualityalias" [Horrible Low Medium Good High Maximum] [ new_gfx_quality $arg1 ]
        menuitem "Advanced video settings" [ showmenu [Advanced video settings] ]
        menuitem "Apply graphics changes" [ resetgl ]

        newmenu "Handheld controls"
        menuitemslider "C-Stick speed" 1 50 "round (*f $csticksensitivity 10)" 1 [ csticksensitivity (divf $arg1 10) ] 0 10
        menuitemslider "Vertical aim ratio" 1 20 "round (*f $cstickysensitivity 10)" 1 [ cstickysensitivity (divf $arg1 10) ] 0 10
        menuitemslider "C-Stick deadzone" 0 40 "$cstickdeadzone" 1 [ cstickdeadzone $arg1 ]
        menuitemslider "Touch speed" 1 200 "round (*f $touchsensitivity 10)" 1 [ touchsensitivity (divf $arg1 10) ] 0 10
        menuitemcheckbox "Drag to aim" "$touchaim" [ touchaim $arg1 ]
        menuitem "Button bindings" [ showmenu "Handheld bindings" ]

        newmenu "Handheld bindings"
        menuheader "" "Choose an action, then press and release its new button. Press START to cancel."
        menuitemkeyinput "Fire" attack
        menuitemkeyinput "Scope / special action" altaction
        menuitemkeyinput "Grenade" quicknadethrow
        menuitemkeyinput "Crouch" crouch
        menuitemkeyinput "Jump" jump
        menuitemkeyinput "Knife attack" key_knifeattack
        menuitemkeyinput "Reload" reload
        menuitemkeyinput "Last weapon" key_prevweapon
        menuitemkeyinput "Scores" key_showscores
        menuitemkeyinput "Full map (hold)" key_showmap
        menuitem "All binding menus" [ showmenu Keyboard ]
    )CFG");
    gmenu *main = menus.access("main");
    if(main)
        main->items.insert(min(3, main->items.length()),
                           new mitemtext(main, newstring("Handheld controls"),
                                         newstring("showmenu [Handheld controls]"), NULL, NULL,
                                         newstring("Adjust C-Stick and touch aim, or remap the game buttons.")));
}

static void ac3ds_pause_menu()
{
    if(watchingdemo || !*getclientmap())
    {
        execute("key_escape");
        return;
    }
    if(multiplayer(NULL))
    {
        // Online matches keep running; never touch `paused` here.
        showmenu("Match menu online");
        return;
    }
    extern int paused;
    if(!paused)
    {
        setvar("paused", 1);
        handheld_menu_paused = true;
    }
    showmenu("Match menu");
}
COMMAND(ac3ds_pause_menu, "");
#endif

// move menu selection forward or backward and skip empty items that are not selectable
int movemenuselection(int currentmenusel, int direction)
{
    direction = clamp(direction, -1, 1);
    int newmenusel = currentmenusel;
    bool selectable = false;
    for(int i = 0; i < curmenu->items.length(); i++)
    {
        newmenusel += direction;
        newmenusel = normalizemenuselection(newmenusel, curmenu->items.length());
        selectable = curmenu->items.inrange(newmenusel) && curmenu->items[newmenusel]->gettext() && curmenu->items[newmenusel]->gettext()[0] != '\0';
        if(selectable) break;
    } 

    if(selectable) return newmenusel;
    else return currentmenusel;
}

#ifdef __3DS__
struct ScoreboardLayout
{
    float scale;
    int height, pagesize;
};

static int displayedScoreboardPageSize = 0;

static ScoreboardLayout scoreboardlayout(int itemcount, int headerrows, int fontheight)
{
    extern int hudtextscale, hidespecthud;
    int respawnsecs;
    const bool prompt = !hidespecthud && respawnstatus(respawnsecs) != RS_NONE;
    const float fullheight = 2.0f * VIRTH * (prompt ? (AC3DS_RESPAWN_PROMPT_TOP + 10) / 240.0f : 1.0f);
    const int rowheight = fontheight * 5 / 4;
    const int fixedheight = (headerrows + 2) * rowheight + 6 * fontheight;
    const int limit = MAXMENU - headerrows;
    const float ceiling = hudtextscale / 100.0f;
    const float scale = clamp(fullheight / (min(itemcount, limit) * rowheight + fixedheight), min(1.35f, ceiling), ceiling);
    const int height = int(fullheight / scale + 0.5f);
    const int fitrows = max(1, (height - fixedheight) / rowheight);
    return {scale, height, min(limit, fitrows)};
}

// L/ZL and R/ZR arrive as Page Up and Page Down. Over the scoreboard, the one
// menu that takes no input, they still turn its page, but the key is not
// consumed: the shoulder buttons keep their match bindings, so fire can respawn
// the player while the scoreboard that opens on death is up.
static bool scoreboardpage(gmenu &menu, int headerrows, int direction)
{
    const int count = menu.items.length();
    if(count)
    {
        // Navigate the rows drawn in the last frame. The font and footer
        // measured during rendering can differ from the input-time layout.
        int size = displayedScoreboardPageSize;
        if(!size)
        {
            const font *menufont = menu.usefont ? getfont(menu.usefont) : NULL;
            size = scoreboardlayout(count, headerrows, menufont ? menufont->defaulth : FONTH).pagesize;
        }
        const int pages = (count + size - 1) / size;
        const int page = (menu.menusel / size + direction + pages) % pages;
        menuselect(&menu, page * size);
        extern int debugevents;
        if(debugevents >= 2) clientlogf("scoreboard: page %d/%d (%d rows per page)", page + 1, pages, size);
    }
    return false;
}
#endif

bool menukey(int code, bool isdown, SDL_Keymod mod)
{
    if(!curmenu) return false;
    int n = curmenu->items.length(), menusel = curmenu->menusel;

    if(curmenu->items.inrange(menusel) && !curmenu->items[menusel]->greyedout)
    {
        mitem *m = curmenu->items[menusel];
        if(m->mitemtype == mitem::TYPE_KEYINPUT && ((mitemkeyinput *)m)->capture)
        {
#ifdef __3DS__
            if(ac3ds_binding_cancel_key(code)) code = SDLK_ESCAPE;
#endif
            m->key(code, isdown);
            return true;
        }
    }

#ifdef __3DS__
    if(!curmenu->allowinput && ac3ds_binding_cancel_key(code))
    {
        if(isdown) closemenu(NULL);
        return true;
    }
    if(ac3ds_keyboard_key(code) && curmenu->allowinput &&
       curmenu->items.inrange(menusel))
    {
        mitem &item = *curmenu->items[menusel];
        if(!item.greyedout && item.mitemtype == mitem::TYPE_TEXTINPUT)
        {
            if(!isdown) item.select();
            return true;
        }
    }
    code = ac3ds_menu_key(code);
    if(isdown && handheldweaponpicker(*curmenu))
    {
        constexpr int columns = Ac3dsMenuView::weapon_columns;
        int target = menusel;
        switch(code)
        {
            case SDLK_LEFT: target -= menusel % columns; break;
            case SDLK_RIGHT: target += columns - 1 - menusel % columns; break;
            case SDLK_UP: target -= columns; break;
            case SDLK_DOWN: target += columns; break;
        }
        if(target != menusel && curmenu->items.inrange(target)) menuselect(curmenu, target);
        if(code == SDLK_LEFT || code == SDLK_RIGHT || code == SDLK_UP || code == SDLK_DOWN) return true;
    }
#endif
    if(isdown)
    {
        const int headerrows = curmenu->headeritems(), pagesize = MAXMENU - headerrows;

        switch(code)
        {
            case SDLK_PAGEUP:
#ifdef __3DS__
                if(curmenu == scoremenu) return scoreboardpage(*curmenu, headerrows, -1);
                if(ac3ds_menu_panel() == Ac3dsMenu::Standard)
                {
                    menusel = handheldmenupage(*curmenu, -1);
                    break;
                }
#endif
                menusel -= pagesize;
                break;
            case SDLK_PAGEDOWN:
#ifdef __3DS__
                if(curmenu == scoremenu) return scoreboardpage(*curmenu, headerrows, 1);
                if(ac3ds_menu_panel() == Ac3dsMenu::Standard)
                {
                    menusel = handheldmenupage(*curmenu, 1);
                    break;
                }
#endif
                if(menusel+pagesize>=n && menusel/pagesize!=(n-1)/pagesize) menusel = n-1;
                else menusel += pagesize;
                break;
            case SDLK_ESCAPE:
            case SDL_AC_BUTTON_RIGHT:
                if(!curmenu->allowinput) return false;
                menuset(menustack.empty() ? NULL : menustack.pop(), false);
                return true;
                break;
            case SDLK_UP:
            case SDL_AC_BUTTON_WHEELUP:
                if(iskeypressed(SDL_SCANCODE_LCTRL)) return menukey(SDLK_LEFT);
                if(iskeypressed(SDL_SCANCODE_LALT)) return menukey(SDLK_RIGHTBRACKET);
                if(!curmenu->allowinput) return false;
                menusel = movemenuselection(menusel, -1);
                break;
            case SDLK_DOWN:
            case SDL_AC_BUTTON_WHEELDOWN:
                if(iskeypressed(SDL_SCANCODE_LCTRL)) return menukey(SDLK_RIGHT);
                if(iskeypressed(SDL_SCANCODE_LALT)) return menukey(SDLK_LEFTBRACKET);
                if(!curmenu->allowinput) return false;
                menusel = movemenuselection(menusel, 1);
                break;
            case SDLK_TAB:
                if(!curmenu->allowinput) return false;
                if(mod & KMOD_LSHIFT) menusel = movemenuselection(menusel, -1);
                else menusel = movemenuselection(menusel, 1);
                break;

            case SDLK_1:
            case SDLK_2:
            case SDLK_3:
            case SDLK_4:
            case SDLK_5:
            case SDLK_6:
            case SDLK_7:
            case SDLK_8:
            case SDLK_9:
                if(curmenu->allowinput && curmenu->hotkeys)
                {
                    int idx = code-SDLK_1;
                    if(curmenu->items.inrange(idx) && !curmenu->items[idx]->greyedout)
                    {
                        menuselect(curmenu, idx);
                        mitem &m = *curmenu->items[idx];
                        m.select();
                    }
                    return true;
                }
            default:
            {
                if(!curmenu->allowinput) return false;
                if(curmenu->keyfunc && (*curmenu->keyfunc)(curmenu, code, isdown)) return true;
                if(!curmenu->items.inrange(menusel)) return false;
                mitem &m = *curmenu->items[menusel];
                if(!m.greyedout) m.key(code, isdown);
                if(code == SDLK_HOME && m.mitemtype != mitem::TYPE_TEXTINPUT) menuselect(curmenu, (menusel = 0));
                return !curmenu->forwardkeys;
            }
        }

#ifndef __3DS__
        if(!curmenu->hotkeys)
#endif
            menuselect(curmenu, menusel);
        return true;
    }
    else
    {
        switch(code)   // action on keyup to avoid repeats
        {
            case SDLK_PRINTSCREEN:
                curmenu->conprintmenu();
                return true;

            case SDLK_F12:
                if(curmenu->allowinput)
                {
                    extern void screenshot(const char *filename);
                    screenshot(NULL);
                }
                break;
        }
        if(!curmenu->allowinput || !curmenu->items.inrange(menusel)) return false;
        mitem &m = *curmenu->items[menusel];
        if(code==SDLK_RETURN || code==SDLK_KP_ENTER || code==SDLK_SPACE || code==SDL_AC_BUTTON_LEFT || code==SDL_AC_BUTTON_MIDDLE)
        {
            if(!m.greyedout && m.select() != -1) audiomgr.playsound(S_MENUENTER, SP_HIGHEST);
            return true;
        }
        return false;
    }
}

void rendermenumdl()
{
    if(!curmenu) return;
    gmenu &m = *curmenu;
    if(!m.mdl) return;

    glPushMatrix();
    glLoadIdentity();
    glRotatef(90+180, 0, -1, 0);
    glRotatef(90, -1, 0, 0);
    glScalef(1, -1, 1);

    bool isplayermodel = !strncmp(m.mdl, "playermodels", strlen("playermodels"));
    bool isweapon = !strncmp(m.mdl, "weapons", strlen("weapons"));

    vec pos;
    if(!isweapon) pos = vec(0.5f, 0.3f, -0.1f); 
    else pos = vec(0.50f, 0, 0.425f);

    float yaw = 1.0f, pitch = isplayermodel || isweapon ? 0.0f : camera1->pitch;
    if(m.rotspeed) yaw += lastmillis/5.0f/100.0f*m.rotspeed;
    if(fabs(pitch) < 10.0f) pitch = 0.0f;
    else pitch += 10.0f * (pitch < 0 ? 1 : -1);

    int tex = 0;
    if(isplayermodel)
    {
        defformatstring(skin)("packages/models/%s.jpg", m.mdl);
        tex = -(int)textureload(skin)->id;
    }
    modelattach a[2];
    if(isplayermodel)
    {
        a[0].name = "weapons/subgun/world";
        a[0].tag = "tag_weapon";
    }
    float scale = (m.scale ? m.scale * 0.04f: 1.0f) * 0.25f;
    rendermodel(isplayermodel ? "playermodels" : m.mdl, m.anim|ANIM_DYNALLOC, tex, -1, pos, 0, yaw, pitch, 0, 0, NULL, a, scale);

    glPopMatrix();
    dimeditinfopanel = 0;
}

void gmenu::refresh()
{
    if(refreshfunc)
    {
        (*refreshfunc)(this, !inited);
        inited = true;
    }
    if(menusel>=items.length()) menusel = max(items.length()-1, 0);
}

void gmenu::open()
{
    inited = false;
    if(!allowinput) menusel = 0;
#ifdef __3DS__
    if(this == scoremenu) displayedScoreboardPageSize = 0;
    if(handheldweaponpicker(*this)) menusel = handheldprimaryselection();
#endif
    if(!forwardkeys) player1->stopmoving();
    setcontext("menu", name);
    if(initaction)
    {
        gmenu *oldlastmenu = lastmenu;
        lastmenu = this;
        execute(initaction);
        lastmenu = oldlastmenu;
    }
    if(items.inrange(menuselinit)) { menusel = menuselinit; menuselinit = -1; }
    if(items.inrange(menusel) && !items[menusel]->greyedout) items[menusel]->focus(true);
    init();
    resetcontext();
}

void gmenu::close()
{
    if(items.inrange(menusel)) items[menusel]->focus(false);
}

void gmenu::conprintmenu()
{
    if(title) conoutf( "[::  %s  ::]", title);//if(items.length()) conoutf( " %03d items", items.length());
    loopv(items) { conoutf("%03d: %s%s%s", 1+i, items[i]->gettext(), items[i]->getaction()?"|\t|":"", items[i]->getaction()?items[i]->getaction():""); }
}

const char *menufilesortorders[] = { "normal", "reverse", "ignorecase", "ignorecase_reverse", "" };
int (*menufilesortcmp[])(const char **, const char **) = { stringsort, stringsortrev, stringsortignorecase, stringsortignorecaserev };

void gmenu::init()
{
    if(!menuseldescbgcolor) menuseldescbgcolor = new color(0.2f, 0.2f, 0.2f, 0.2f);
    if(dirlist && dirlist->dir && dirlist->ext)
    {
        menureset(this);

        if(dirlist->subdiraction)
        {
            vector<char *> subdirs;
            if(dirlist->subdirdots) subdirs.add(newstring(".."));
            listsubdirs(dirlist->dir, subdirs, stringsort);
            loopv(subdirs)
            {
                defformatstring(cdir)("\fs\f1[%s]\fr", subdirs[i]);
                defformatstring(act)("%s %s", dirlist->subdiraction, escapestring(subdirs[i]));
                items.add(new mitemtext(this, newstring(cdir), newstring(act), NULL, NULL, NULL));
            }
        }

        defformatstring(sortorderalias)("menufilesort_%s", dirlist->ext);
        int sortorderindex = 0;
        const char *customsortorder = getalias(sortorderalias);
        if(customsortorder) sortorderindex = getlistindex(customsortorder, menufilesortorders, true, 0);

        vector<char *> files;
        listfiles(dirlist->dir, dirlist->ext, files, menufilesortcmp[sortorderindex]);

        string searchfileuc;
        if(dirlist->searchfile)
        {
            const char *searchfilealias = getalias(dirlist->searchfile);
            copystring(searchfileuc, searchfilealias ? searchfilealias : dirlist->searchfile);
            strtoupper(searchfileuc);
        }

        loopv(files)
        {
            char *f = files[i], *d = (strcmp(dirlist->ext, "cgz") || dirlist->searchfile) && browsefiledesc ? getfiledesc(dirlist->dir, f, dirlist->ext) : NULL;
            bool filefound = false;
            if(dirlist->searchfile)
            {
                string fuc, duc;
                copystring(fuc, f);
                strtoupper(fuc);
                copystring(duc, d ? d : "");
                strtoupper(duc);
                if(strstr(fuc, searchfileuc) || strstr(duc, searchfileuc)) filefound = true;
            }

            if(dirlist->image)
            {
                string fullname = "";
                if(dirlist->dir[0]) formatstring(fullname)("%s/%s", dirlist->dir, f);
                else copystring(fullname, f);
                if(dirlist->ext)
                {
                    concatstring(fullname, ".");
                    concatstring(fullname, dirlist->ext);
                }
                items.add(new mitemimage  (this, newstring(fullname), f, newstring(dirlist->action), NULL, NULL, d));
            }
            else if(!strcmp(dirlist->ext, "cgz"))
            {
                if(!dirlist->searchfile || filefound)
                {
                    items.add(new mitemmapload(this, newstring(f), newstring(f), newstring(dirlist->action), NULL, NULL));
                }
                DELSTRING(d);
            }
            else if(!strcmp(dirlist->ext, "dmo"))
            {
                if(!dirlist->searchfile || filefound) items.add(new mitemtext(this, f, newstring(dirlist->action), NULL, NULL, d));
            }
            else items.add(new mitemtext(this, f, newstring(dirlist->action), NULL, NULL, d));
        }
    }
    loopv(items) items[i]->init();
}

int gmenu::headeritems()  // returns number of header and footer lines (and also recalculates hasdesc)
{
    hasdesc = false;
    loopv(items) if(items[i]->getdesc()) { hasdesc = true; break; }
    return (header ? 2 : 0) + (footer || hasdesc ? (footlen ? (footlen + 1) : 2) : 0);
}

FVAR(menutexturesize, 0.1f, 1.0f, 5.0f);
FVAR(menupicturesize, 0.1f, 1.6f, 5.0f);

void rendermenutexturepreview(char *previewtexture, int w, const char *title)
{
    static Texture *pt = NULL;
    static uint last_pt = 0;
    bool ispicture = title != NULL;
    uint cur_pt = hthash(previewtexture);
    if(cur_pt != last_pt)
    {
        silent_texture_load = ispicture;
        defformatstring(texpath)("packages/textures/%s", previewtexture);
        pt = textureload(texpath, ispicture ? 3 : 0);
        last_pt = cur_pt;
        silent_texture_load = false;
    }
    if(pt && pt != notexture && pt->xs && pt->ys)
    {
        int xs = (VIRTW * (ispicture ? menupicturesize : menutexturesize)) / 4, ys = (xs * pt->ys) / pt->xs, ysmax = (3 * VIRTH) / 2;
        if(ys > ysmax) ys = ysmax, xs = (ys * pt->xs) / pt->ys;
        int x = (6 * VIRTW + w - 2 * xs) / 4, y = VIRTH - ys / 2 - 2 * FONTH;
        extern int fullbrightlevel;
        framedquadtexture(pt->id, x, y, xs, ys, FONTH / 2, fullbrightlevel);
        defformatstring(res)("%dx%d", pt->xs, pt->ys);
        draw_text(title ? title : res, x, y + ys + 2 * FONTH);
    }
}

#ifdef __3DS__
static void handheldmenurect(int x, int y, int w, int h, int r, int g, int b, int a)
{
    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4ub(r, g, b, a);
    glBegin(GL_TRIANGLE_STRIP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x, y + h);
    glVertex2f(x + w, y + h);
    glEnd();
    glEnable(GL_TEXTURE_2D);
    xtraverts += 4;
}

static void handheldmenuimage(GLuint texture, int x, int y, int w, int h, float top = 0, float bottom = 1)
{
    glDisable(GL_BLEND);
    glColor3ub(255, 255, 255);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0, top);    glVertex2f(x, y);
    glTexCoord2f(1, top);    glVertex2f(x + w, y);
    glTexCoord2f(1, bottom); glVertex2f(x + w, y + h);
    glTexCoord2f(0, bottom); glVertex2f(x, y + h);
    glEnd();
    glEnable(GL_BLEND);
    xtraverts += 4;
}

static Texture *handheldpreview(gmenu &menu, mitem *item, const char *&description)
{
    if(!hidebigmenuimages && item)
    {
        if(auto *map = dynamic_cast<mitemmapload *>(item))
        {
            map->loadpreview();
            if(map->mapmessage && *map->mapmessage) description = map->mapmessage;
            return map->image;
        }
        if(auto *image = dynamic_cast<mitemimagemanual *>(item)) return image->image;
    }
    if(menu.previewtexture && *menu.previewtexture)
    {
        defformatstring(path)("packages/textures/%s", menu.previewtexture);
        return textureload(path, menu.previewtexturetitle ? 3 : 0);
    }
    return NULL;
}

static void renderhandheldmenu(gmenu &menu)
{
    const int selected = menu.menusel;
    mitem *item = menu.items.inrange(selected) ? menu.items[selected] : NULL;
    Ac3dsMenuRow row;
    const bool binding = item && handheldmenuentry(row, *item, selected,
                         handheldweaponpicker(menu) ? handheldprimaryselection() : -1);
    const char *detail = item ? item->getdesc() : NULL;
    if(!detail || !*detail) detail = menu.footer;
    Texture *preview = handheldpreview(menu, item, detail);
    const bool brand = !strcmp(menu.name, "main") || !strcmp(menu.name, "welcome");

    const bool pixel = ac3ds_push_pixel_font();
    const int width = pixel ? AC3DS_WIDTH : VIRTW * 3 / 4;
    const int height = pixel ? AC3DS_HEIGHT : VIRTH * 3 / 4;
    const int pad = pixel ? 15 : 82;
    const int headingScale = pixel ? 2 : 1;
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    ignoreblinkingbit = !menu.allowblink;

    if(brand)
    {
        handheldmenurect(0, 0, width, height, 0, 0, 0, 255);
        extern Texture *startscreen;
        if(startscreen && startscreen != notexture)
        {
            const int size = width - 2 * pad;
            handheldmenuimage(startscreen->id, pad, pad / 2, size, size * 48 / 100, 0.04f, 0.52f);
        }
    }
    else if(preview && preview != notexture && preview->xs && preview->ys)
    {
        const int h = min(height / 2, (width - 2 * pad) * preview->ys / preview->xs);
        const int w = h * preview->xs / preview->ys;
        const int x = (width - w) / 2, y = pad / 2, frame = pad / 6;
        handheldmenurect(x - frame, y - frame, w + 2 * frame, h + 2 * frame, 12, 21, 18, 255);
        handheldmenuimage(preview->id, x, y, w, h);
    }

    const char *heading = row.label[0] ? row.label : handheldmenutitle(menu);
    if(!strcmp(menu.name, "Match menu")) heading = "Match paused";
    else if(!strcmp(menu.name, "Match menu online")) heading = "Match menu";
    if(menu.previewtexturetitle && *menu.previewtexturetitle) heading = menu.previewtexturetitle;
    if(binding) detail = "Press and release a button. START cancels.";
    else if(!detail && item && !row.disabled)
    {
        if(row.kind == Ac3dsMenuItem::Slider) detail = "Use Left / Right or tap - / + to adjust.";
        else if(row.kind == Ac3dsMenuItem::Text) detail = "Press A or tap the row to open the keyboard.";
        else if(row.kind == Ac3dsMenuItem::Binding) detail = "Press A or tap the row to change the binding.";
    }
    if(menu.items.empty() && !detail) detail = "No entries";

    const int textwidth = width - 4 * pad;
    int textw, headingh, detailh = 0, headerh = 0;
    text_bounds(heading, textw, headingh, textwidth);
    if(detail) text_bounds(detail, textw, detailh, textwidth);
    if(menu.header) text_bounds(menu.header, textw, headerh, textwidth);
    headingh *= headingScale;
    const int cardh = min(height - 2 * pad, 3 * pad / 2 + headingh + detailh + headerh +
                          (row.value[0] ? FONTH : 0));
    const int y = height - pad - cardh;
    handheldmenurect(pad, y, width - 2 * pad, cardh, 12, 21, 18, 245);
    handheldmenurect(pad, y, pixel ? 2 : 14, cardh, 169, 207, 118, 255);
    int line = y + 3 * pad / 4;
    const int x = 2 * pad, bottom = height - pad / 2;
    if(menu.header)
    {
        draw_text(menu.header, x, line, 171, 195, 174, 255, -1, textwidth);
        line += headerh;
    }
    if(line + FONTH * headingScale < bottom)
    {
        glPushMatrix();
        glTranslatef(x, line, 0);
        glScalef(1, headingScale, 1);
        draw_text(heading, 0, 0, 235, 246, 231, 255, -1, textwidth);
        glPopMatrix();
    }
    line += headingh;
    if(row.value[0] && line + FONTH < bottom)
    {
        draw_text(row.value, x, line, 186, 216, 145, 255, -1, textwidth);
        line += FONTH;
    }
    if(detail && line + FONTH < bottom)
    {
        string clipped;
        copystring(clipped, detail);
        clipped[text_visible(clipped, 0, (bottom - line) / FONTH * FONTH, textwidth)] = '\0';
        draw_text(clipped, x, line, 185, 207, 188, 255, -1, textwidth);
    }
    ignoreblinkingbit = false;
    glPopMatrix();
    if(pixel) popfont();
}
#endif

#ifdef __3DS__
#include "ac3ds_servers.h"
// Top-screen server list sized for the 3DS: about 17 px text (half the
// virtual HUD size), one row per server with a coloured ping marker, players
// and name; the selected server's map, mode and time below. The lower screen
// carries the details and actions.
static void renderhandheldservers(gmenu &menu)
{
    const int width = VIRTW / 2, height = VIRTH / 2, pad = FONTH / 2;
    const int headerh = FONTH * 3 / 2, footerh = FONTH * 2, rowh = FONTH * 6 / 5;
    const int top = headerh + FONTH / 6, visible = max(1, (height - footerh - top) / rowh);
    static vector<Ac3dsServerRow> rows;
    while(rows.length() < visible) rows.add();
    const Ac3dsServerList list = ac3ds_server_window(&menu, rows.getbuf(), visible);
    const int count = list.count, first = list.first, selected = list.selected;
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    handheldmenurect(0, 0, width, height, 6, 11, 9, 255);

    // Header: title and count, sort order on the right.
    handheldmenurect(0, 0, width, headerh, 12, 21, 18, 255);
    handheldmenurect(0, headerh - FONTH / 10, width, FONTH / 10, 169, 207, 118, 255);
    extern void *searchmenu;
    defformatstring(title)("%s %d", &menu == searchmenu ? "Found" : "Servers", count);
    draw_text(title, pad, (headerh - FONTH) / 2, 235, 246, 231, 255);
    defformatstring(sorted)("by %s", ac3ds_server_sort_name());
    draw_text(sorted, width - pad - text_width(sorted), (headerh - FONTH) / 2, 171, 195, 174, 255);

    // Rows.
    const int marker = FONTH / 2, playersx = pad + marker + FONTH / 3;
    const int namex = playersx + text_width("88/88") + FONTH / 3;
    if(!count)
        draw_text(ac3ds_server_total() ? "Waiting for servers..." : "No servers. Tap SCAN.", pad, top + rowh, 185, 207, 188, 255);
    for(int i = first; i < count && i < first + visible; ++i)
    {
        const Ac3dsServerRow &r = rows[i - first];
        const int y = top + (i - first) * rowh;
        if(i == selected)
        {
            handheldmenurect(0, y - FONTH / 10, width, rowh, 32, 58, 34, 255);
            handheldmenurect(0, y - FONTH / 10, FONTH / 6, rowh, 169, 207, 118, 255);
        }
        const bool usable = r.responded && r.compatible && !r.banned;
        const int dim = usable ? 255 : 110;
        // Ping as a coloured square: green fast, yellow middling, red slow.
        if(r.responded)
        {
            const int pr = r.ping < 100 ? 90 : r.ping < 200 ? 235 : 240, pg = r.ping < 100 ? 210 : r.ping < 200 ? 200 : 90, pb = r.ping < 100 ? 90 : 60;
            handheldmenurect(pad, y + (FONTH - marker) / 2, marker, marker, pr, pg, pb, dim);
            defformatstring(players)("%d/%d", r.players, r.maxclients);
            draw_text(players, playersx, y, r.full ? 240 : 235, r.full ? 120 : 246, r.full ? 110 : 231, dim);
        }
        else draw_text("..", pad, y, 140, 150, 140, 255);
        string name;
        if(!r.compatible && r.responded) formatstring(name)("(other version) %s", r.desc);
        else copystring(name, r.desc);
        if(r.password) concatstring(name, " *");
        const int namew = width - pad - namex;
        name[text_visible(name, namew, 0, -1)] = '\0';
        draw_text(name, namex, y, r.connected ? 169 : 235, r.connected ? 230 : 246, r.connected ? 118 : 231, dim);
    }
    // Scroll position: a thin bar on the right edge.
    if(count > visible)
    {
        const int trackh = visible * rowh, barh = max(FONTH / 2, trackh * visible / count);
        const int bary = top + (trackh - barh) * first / max(1, count - visible);
        handheldmenurect(width - FONTH / 8, top, FONTH / 8, trackh, 30, 45, 35, 255);
        handheldmenurect(width - FONTH / 8, bary, FONTH / 8, barh, 169, 207, 118, 255);
    }

    // Footer: the selected server's map, mode and time left.
    handheldmenurect(0, height - footerh, width, footerh, 12, 21, 18, 255);
    string line;
    if(selected >= 0)
    {
        const Ac3dsServerRow &r = rows[selected - first];
        if(r.map[0]) formatstring(line)("%s  %s  %dmin", r.map, r.mode, r.minremain);
        else copystring(line, r.responded ? "Empty server" : "Waiting for an answer");
    }
    else copystring(line, "Choose with the D-Pad");
    line[text_visible(line, width - 2 * pad, 0, -1)] = '\0';
    draw_text(line, pad, height - footerh + (footerh - FONTH) / 2, 235, 246, 231, 255);
    glPopMatrix();
}

static const char *nextscorecell(const char *row, string &cell, char *flag = NULL)
{
    const size_t length = strcspn(row, "\t");
    size_t copied = min(length, sizeof(cell) - 1);
    memcpy(cell, row, copied);
    cell[copied] = '\0';
    if(flag)
    {
        const char *icon = strchr(cell, '\a');
        *flag = icon ? icon[1] : '\0';
        if(icon) copied = icon - cell;
    }
    while(copied && cell[copied - 1] == ' ') --copied;
    cell[copied] = '\0';
    row += length;
    return *row == '\t' ? row + 1 : row;
}

static void drawscoretext(const char *text, int x, int y, const bvec &color, int heightScale)
{
    if(heightScale == 1) draw_text(text, x, y, color.x, color.y, color.z);
    else
    {
        glPushMatrix();
        glTranslatef(x, y, 0);
        glScalef(1, heightScale, 1);
        draw_text(text, 0, 0, color.x, color.y, color.z);
        glPopMatrix();
    }
}

static void drawscorelabel(const char *text, int x, int y, int width, const bvec &color)
{
    if(!text || !*text || width <= 0) return;
    string label;
    copystring(label, text);
    if(text_width(label) > width)
    {
        const int ellipsis = text_width("...");
        const bool canEllipsize = width > ellipsis;
        label[text_visible(label, width - (canEllipsize ? ellipsis : 0), 0, -1)] = '\0';
        if(canEllipsize) concatstring(label, "...");
    }
    draw_text(label, x, y, color.x, color.y, color.z);
}

constexpr int scoreCellPad = 4, scoreFlagWidth = 12;

struct HandheldScoreColumn { const char *heading; int width, x, line; bool name; };

struct HandheldScoreColumns
{
    unsigned revision = 0;
    int count = 0, lines = 1;
    HandheldScoreColumn columns[7];
};

static const HandheldScoreColumns &handheldscorecolumns(const gmenu &menu, int maxWidth)
{
    static HandheldScoreColumns layout;
    if(layout.revision == ac3ds_score_display.layoutRevision) return layout;
    layout.revision = ac3ds_score_display.layoutRevision;
    layout.count = 0;
    layout.lines = 1;
    const bool flagMode = m_flags_;

    const char *headings = menu.title ? menu.title : "";
    while(*headings && layout.count < int(sizeof(layout.columns) / sizeof(layout.columns[0])))
    {
        string heading;
        headings = nextscorecell(headings, heading);
        HandheldScoreColumn column{"", 0, 0, 0, false};
        if(!strcmp(heading, "flags")) column.heading = "Flag";
        else if(!strcmp(heading, "frags")) column.heading = "K";
        else if(!strcmp(heading, "deaths")) column.heading = "D";
        else if(!strcmp(heading, "ratio"))
        {
            column.heading = "K/D";
            column.width = text_width("10.00");
        }
        else if(!strcmp(heading, "pj/ping")) column.heading = "PJ/Ping";
        else if(!strcmp(heading, "cn"))
        {
            column.heading = "CN";
            column.width = text_width("RVSF");
        }
        else if(!strcmp(heading, "name"))
        {
            column.heading = "Player";
            column.width = MAXNAMELEN * curfont->defaultw + (flagMode ? scoreFlagWidth : 0);
            column.name = true;
        }
        else continue;
        column.width = max(column.width, max(2 * curfont->defaultw, text_width(column.heading))) + scoreCellPad;
        layout.columns[layout.count++] = column;
    }

    // Fit the complete cells on every page. Widths are recalculated only when
    // score data or column preferences change, so paging itself is inexpensive.
    loopv(menu.items)
    {
        const char *row = menu.items[i]->gettext();
        if(!row || !*row) continue;
        loopj(layout.count)
        {
            string cell;
            HandheldScoreColumn &column = layout.columns[j];
            char flag;
            row = nextscorecell(row, cell, column.name ? &flag : NULL);
            // Reserve the icon after the full name and status, including when
            // nobody is carrying a flag. Possession must not change the pages.
            const int iconSpace = flagMode && column.name ? scoreFlagWidth : 0;
            column.width = max(column.width, text_width(cell) + scoreCellPad + iconSpace);
        }
    }

    int used = 0;
    loopi(layout.count)
    {
        HandheldScoreColumn &column = layout.columns[i];
        if(used && used + column.width > maxWidth)
        {
            ++layout.lines;
            used = 0;
        }
        column.x = used;
        column.line = layout.lines - 1;
        used += column.width;
    }
    return layout;
}

static void renderhandheldscoreboard(gmenu &menu)
{
    constexpr int left = 8, top = 6, width = AC3DS_WIDTH - 2 * left, pad = 8;
    constexpr int innerWidth = width - 2 * pad;
    const HandheldScoreColumns &layout = handheldscorecolumns(menu, innerWidth);
    const bool outcome = *ac3ds_score_display.outcome;
    const int columnsY = top + 38 + (outcome ? 11 : 0);
    extern int hidespecthud, hudtextscale;
    int seconds;
    const bool respawn = !hidespecthud && respawnstatus(seconds) != RS_NONE;
    const int limit = respawn ? AC3DS_RESPAWN_PROMPT_TOP - 4 : AC3DS_HEIGHT - top;
    const int footerHeight = menu.footer && *menu.footer ? 16 : 4;
    // A large row needs room for both its headings and one whole record.
    const int largeRowHeight = layout.lines * (FONTH * 2 + 2) + 1;
    const bool largeFits = columnsY + 2 * largeRowHeight + 2 + footerHeight <= limit;
    const int glyphScale = hudtextscale >= 240 && largeFits ? 2 : 1;
    const int lineHeight = FONTH * glyphScale + 2;
    const int rowHeight = layout.lines * lineHeight + 1, rowsY = columnsY + rowHeight + 2;
    const int count = menu.items.length();
    displayedScoreboardPageSize = max(1, (limit - rowsY - footerHeight) / rowHeight);
    const int pageSize = displayedScoreboardPageSize;
    const int pages = max(1, (count + pageSize - 1) / pageSize);
    menu.menusel = clamp(menu.menusel, 0, max(0, count - 1));
    const int page = menu.menusel / pageSize, offset = page * pageSize;
    const int visible = min(count - offset, pageSize);
    const int bottom = rowsY + min(count, pageSize) * rowHeight + footerHeight;

    static const bvec textColor(242, 246, 236), muted(177, 194, 177), accent(181, 224, 123);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, AC3DS_WIDTH, AC3DS_HEIGHT, 0, -1, 1);
    glEnable(GL_BLEND);
    handheldmenurect(left, top, width, bottom - top, 9, 17, 13, 247);
    handheldmenurect(left, top, width, columnsY - top, 19, 36, 26, 255);
    handheldmenurect(left, top, width, 1, 181, 224, 123, 255);

    glPushMatrix();
    glTranslatef(left + pad, top + 7, 0);
    glScalef(2, 2, 1);
    draw_text("Scoreboard", 0, 0, textColor.x, textColor.y, textColor.z);
    glPopMatrix();

    drawscorelabel(ac3ds_score_display.summary, left + pad, top + 27, innerWidth, muted);
    if(outcome) drawscorelabel(ac3ds_score_display.outcome, left + pad, top + 38, innerWidth, textColor);
    if(pages > 1)
    {
        defformatstring(indicator)("%d/%d", page + 1, pages);
        draw_text(indicator, left + width - pad - text_width(indicator), top + 11, accent.x, accent.y, accent.z);
    }

    handheldmenurect(left, columnsY, width, rowsY - columnsY, 27, 47, 33, 255);
    loopi(layout.count)
    {
        const HandheldScoreColumn &column = layout.columns[i];
        const int x = left + pad + column.x + (column.name ? scoreCellPad / 2 : (column.width - text_width(column.heading)) / 2);
        drawscoretext(column.heading, x, columnsY + 2 + column.line * lineHeight, accent, glyphScale);
    }

    loopi(visible)
    {
        mitem &item = *menu.items[offset + i];
        const char *row = item.gettext();
        if(!row || !*row) continue;
        const int y = rowsY + i * rowHeight;
        if(const color *background = item.bgcolor)
        {
            if(background->r > background->b) handheldmenurect(left, y, width, rowHeight, 61, 30, 31, 255);
            else if(background->b > background->r) handheldmenurect(left, y, width, rowHeight, 29, 43, 63, 255);
            else
            {
                handheldmenurect(left, y, width, rowHeight, 38, 65, 41, 255);
                handheldmenurect(left, y, 2, rowHeight, 181, 224, 123, 255);
            }
        }
        else if(i % 2) handheldmenurect(left, y, width, rowHeight, 17, 28, 21, 255);

        const bvec &rowColor = row[0] == '\f' && row[1] == '4' ? muted : textColor;
        loopj(layout.count)
        {
            const HandheldScoreColumn &column = layout.columns[j];
            string cell;
            char flag;
            row = nextscorecell(row, cell, &flag);
            const int x = left + pad + column.x;
            const int lineY = y + column.line * lineHeight;
            if(*cell)
                drawscoretext(cell, x + (column.name ? scoreCellPad / 2 : (column.width - text_width(cell)) / 2),
                              lineY + 1, rowColor, glyphScale);
            if(flag)
            {
                const bvec flagColor = flag == 'D' ? bvec(255, 126, 117) : flag == 'H' ? bvec(118, 186, 255) : bvec(245, 216, 94);
                const int flagX = x + scoreCellPad / 2 + text_width(cell) + 2;
                const int flagY = lineY + (lineHeight + 1 - 7) / 2;
                handheldmenurect(flagX, flagY, 1, 7, flagColor.x, flagColor.y, flagColor.z, 255);
                handheldmenurect(flagX + 1, flagY, 6, 4, flagColor.x, flagColor.y, flagColor.z, 255);
            }
        }
    }

    if(menu.footer && *menu.footer)
    {
        const int y = bottom - footerHeight;
        handheldmenurect(left, y, width, footerHeight, 19, 36, 26, 255);
        drawscorelabel(menu.footer, left + pad, y + 4, innerWidth, muted);
    }
    glPopMatrix();
}
#endif

void gmenu::render()
{
#ifdef __3DS__
    if(this == scoremenu && ac3ds_push_pixel_font())
    {
        renderhandheldscoreboard(*this);
        popfont();
        return;
    }
#endif
    if(usefont) pushfont(usefont);
    const int hitems = headeritems();
#ifdef __3DS__
    const bool scoreboard = this == scoremenu;
    const ScoreboardLayout layout = scoreboard ? scoreboardlayout(items.length(), hitems, FONTH) :
                                                 ScoreboardLayout{1.0f, 2 * VIRTH, MAXMENU - hitems};
    if(scoreboard) displayedScoreboardPageSize = layout.pagesize;
    const int VW2 = int(2 * VIRTW / layout.scale + 0.5f), VH2 = layout.height;
    const int pagesize = layout.pagesize;
#else
    const int VW2 = 2 * VIRTW, VH2 = 2 * VIRTH;
    const int pagesize = MAXMENU - hitems;
#endif
    if(!allowblink) ignoreblinkingbit = true;
    bool synctabs = title != NULL || synctabstops;
    const char *t = title;
    if(!t)
    {
        static string buf;
        if(hotkeys) formatstring(buf)("%s hotkeys", name);
        else formatstring(buf)("[ %s menu ]", name);
        t = buf;
    }
    int w = 0, footermaxw = VW2 - 6 * FONTH, footwidth, footheight, maxfootheight = 0;
    if(hasdesc) loopv(items) if(items[i]->getdesc())
    {
        text_bounds(items[i]->getdesc(), footwidth, footheight, footermaxw);
        w = max(w, footwidth);
        maxfootheight = max(maxfootheight, footheight);
    }
    if(synctabs) text_startcolumns();
    loopv(items) w = max(w, items[i]->width());
    int offset = menusel - (menusel%pagesize),
        mdisp = min(items.length(), pagesize),
        cdisp = min(items.length()-offset, pagesize),
        pages = (items.length() - 1) / pagesize;
#ifdef __3DS__
    glPushMatrix();
    glScalef(layout.scale, layout.scale, 1);
    struct PopScale { ~PopScale() { glPopMatrix(); } } popscale;
#endif
    mitem::whitepulse.alpha = (sinf(lastmillis/200.0f)+1.0f)/2.0f;
    defformatstring(menupages)("%d/%d", (offset / pagesize) + 1, pages + 1);
    int menupageswidth = pages ? text_width(menupages) : 0; // adds to topmost line, either menu title or header
    w = max(w, text_width(t) + (header ? 0 : menupageswidth));
    if(header) w = max(w, text_width(header) + menupageswidth);

    int step = (FONTH*5)/4;
    int h = (mdisp+hitems+2)*step;
    int y = (VH2-h)/2;
    int x = hotkeys ? (VW2-w)/6 : (VW2-w)/2;
    x = clamp(x + (VW2 / 2 * xoffs) / 100, 3 * FONTH, VW2 - w - 3 * FONTH);
    y = clamp(y + (VH2 / 2 * yoffs) / 100, 3 * FONTH, VH2 - h - 3 * FONTH);

    if(!hotkeys) renderbg(x - FONTH*3/2, y - FONTH, x + w + FONTH*3/2, y + h + FONTH, true);
    if(pages)
    {
        if(offset > 0) drawarrow(1, x + w + FONTH*3/2 - FONTH*5/6, y - FONTH*5/6, FONTH*2/3);
        if(offset + pagesize < items.length()) drawarrow(0, x + w + FONTH*3/2 - FONTH*5/6, y + h + FONTH/6, FONTH*2/3);
        draw_text(menupages, x + w + FONTH*3/2 - FONTH*5/6 - menupageswidth, y);
    }

    if(header)
    {
        draw_text(header, x, y);
        y += step*2;
    }
    draw_text(t, x, y);
    y += step*2;
    loopj(cdisp)
    {
        items[offset+j]->render(x, y, w);
        y += step;
    }
    if(synctabs) text_endcolumns();
    if(footer || hasdesc)
    {
        y += ((mdisp-cdisp)+1)*step;
        const char *usefoot = hasdesc && items.inrange(menusel) ? items[menusel]->getdesc() : footer;
        if(!hasdesc) text_bounds(footer, footwidth, maxfootheight, w);
        if(maxfootheight) footlen = min(MAXMENU / 4, (maxfootheight + step / 2) / step);
        if(usefoot) draw_text(usefoot, x, y, 0xFF, 0xFF, 0xFF, 0xFF, -1, w);
    }
    if(previewtexture && *previewtexture) rendermenutexturepreview(previewtexture, w, previewtexturetitle);
    if(usefont) popfont(); // setfont("default");
    ignoreblinkingbit = false;
}

void gmenu::renderbg(int x1, int y1, int x2, int y2, bool border)
{
    static Texture *tex = NULL;
    if(!tex) tex = textureload("packages/misc/menu.jpg");
    static color transparent(1, 1, 1, 0.75f);
    blendbox(x1, y1, x2, y2, border, tex->id, allowinput ? NULL : &transparent);
}

// apply changes menu

void *applymenu = NULL;
static vector<const char *> needsapply;
VARP(applydialog, 0, 1, 1);

void addchange(const char *desc, int type)
{
    if(!applydialog) return;
    if(type!=CHANGE_GFX)
    {
        conoutf("..restart AssaultCube for this setting to take effect");
        return;
    }
    bool changed = false;
    loopv(needsapply) if(!strcmp(needsapply[i], desc)) { changed = true; break; }
    if(!changed) needsapply.add(desc);
    showmenu("apply", false);
}

void clearchanges(int type)
{
    if(type!=CHANGE_GFX) return;
    needsapply.shrink(0);
    closemenu("apply");
}

void refreshapplymenu(void *menu, bool init)
{
    gmenu *m = (gmenu *) menu;
    if(!m || (!init && needsapply.length() == m->items.length()-3)) return;
    menureset(m);
    loopv(needsapply) m->items.add(new mitemtext(m, newstring(needsapply[i]), NULL, NULL, NULL));
    m->items.add(new mitemtext(m, newstring(""), NULL, NULL, NULL));
    m->items.add(new mitemtext(m, newstring("Yes"), newstring("resetgl"), NULL, NULL));
    m->items.add(new mitemtext(m, newstring("No"), newstring("echo [..restart AssaultCube to apply the new settings]"), NULL, NULL));
    if(init) m->menusel = m->items.length()-2; // select OK
}

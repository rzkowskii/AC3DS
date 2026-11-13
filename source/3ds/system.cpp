#include <3ds.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cube.h"
#include "audio_3ds.h"
#include "system.h"

extern void pushevent(const SDL_Event &event);

extern "C" {
    u32 __stacksize__ = 1024 * 1024;
    // New-model homebrew has room to reserve a larger GPU-visible heap.
    // libctru still sizes the CPU heap from the remaining process budget.
    u32 __ctru_linear_heap_size = 48 * 1024 * 1024;
}

VARP(cstickdeadzone, 0, 10, 40);
FVARP(csticksensitivity, 0.1f, 1.0f, 5.0f);
FVARP(cstickysensitivity, 0.1f, 1.0f, 2.0f);
FVARP(touchsensitivity, 0.1f, 5.0f, 20.0f);
VARP(touchaim, 0, 1, 1);

namespace
{
    const char *const game_dir = "sdmc:/3ds/assaultcube";
    constexpr size_t socket_buffer_size = 1 << 20;
    constexpr int max_config_size = 1 << 20;
    bool gfx_ready, romfs_ready, network_ready, apt_hooked, lcd_ready;
    bool clock_reset, controls_dirty = true, active = true;
    aptHookCookie apt_cookie;
    u32 *socket_buffer;
    u32 original_brightness;

    class ConfigBuffer final : public stream
    {
        vector<uchar> bytes;
        int position = 0;
        bool failed = false;

    public:
        void close() override {}
        bool end() override { return position >= bytes.length(); }
        long tell() override { return position; }
        long size() override { return failed ? -1 : bytes.length(); }
        bool seek(long offset, int whence) override
        {
            if(offset != 0 || whence != SEEK_SET) return false;
            position = 0;
            return true;
        }
        int read(void *out, int length) override
        {
            length = min(length, bytes.length() - position);
            if(length <= 0) return 0;
            memcpy(out, bytes.getbuf() + position, length);
            position += length;
            return length;
        }
        int write(const void *data, int length) override
        {
            if(failed || length < 0 || length > max_config_size - bytes.length())
            {
                failed = true;
                return 0;
            }
            bytes.put(static_cast<const uchar *>(data), length);
            return length;
        }
        int printf(const char *format, ...) override
        {
            if(failed) return -1;
            va_list args, copy;
            va_start(args, format);
            va_copy(copy, args);
            int length = vsnprintf(NULL, 0, format, copy);
            va_end(copy);
            if(length < 0 || length > max_config_size - bytes.length())
            {
                va_end(args);
                failed = true;
                return -1;
            }
            char *out = reinterpret_cast<char *>(bytes.pad(length + 1));
            int written = vsnprintf(out, length + 1, format, args);
            va_end(args);
            bytes.drop();
            if(written != length) failed = true;
            return failed ? -1 : length;
        }
    };

    enum Key
    {
        KEYCODE_A = 0x10000,
        KEYCODE_B, KEYCODE_X, KEYCODE_Y, KEYCODE_L, KEYCODE_R,
        KEYCODE_ZL, KEYCODE_ZR, KEYCODE_START, KEYCODE_SELECT,
        KEYCODE_UP, KEYCODE_DOWN, KEYCODE_LEFT, KEYCODE_RIGHT,
        KEYCODE_CPAD_UP, KEYCODE_CPAD_DOWN, KEYCODE_CPAD_LEFT, KEYCODE_CPAD_RIGHT,
        KEYCODE_CHAT, KEYCODE_TEAM, KEYCODE_CONSOLE, KEYCODE_EDIT,
        KEYCODE_VOICE, KEYCODE_MAP, KEYCODE_VOTEYES, KEYCODE_VOTENO,
        KEYCODE_FLAG, KEYCODE_SCREENSHOT, KEYCODE_WEAPONS, KEYCODE_NEXT,
        KEYCODE_PREV, KEYCODE_UNDO, KEYCODE_REDO, KEYCODE_DELETE, KEYCODE_EDITMENU
    };

    struct Control
    {
        Key code;
        const char *name, *normal, *editing, *spectator;
    };

    const Control controls[] = {
        {KEYCODE_A, "3DS_A", "jump", "", ""},
        {KEYCODE_B, "3DS_B", "key_knifeattack", "crouch", ""},
        {KEYCODE_X, "3DS_X", "reload", "copy", ""},
        {KEYCODE_Y, "3DS_Y", "key_prevweapon", "paste", ""},
        {KEYCODE_L, "3DS_L", "attack", "", ""},
        {KEYCODE_R, "3DS_R", "altaction", "editkey_show_menu", ""},
        {KEYCODE_ZL, "3DS_ZL", "quicknadethrow", "editmeta", ""},
        {KEYCODE_ZR, "3DS_ZR", "crouch", "editkey_editmeta2", ""},
        {KEYCODE_START, "3DS_START", "ac3ds_pause_menu", "", ""},
        {KEYCODE_SELECT, "3DS_SELECT", "key_showscores", "editkey_show_menu", ""},
        {KEYCODE_UP, "3DS_UP", "primary", "editkey_move_floor_up", "speckey_demo_gamespeedup"},
        {KEYCODE_DOWN, "3DS_DOWN", "secondary", "editkey_move_floor_down", "speckey_demo_gamespeeddown"},
        {KEYCODE_LEFT, "3DS_LEFT", "key_universaldelta_minus", "", ""},
        {KEYCODE_RIGHT, "3DS_RIGHT", "key_universaldelta_plus", "", ""},
        {KEYCODE_CPAD_UP, "3DS_CPAD_UP", "forward", "", ""},
        {KEYCODE_CPAD_DOWN, "3DS_CPAD_DOWN", "backward", "", ""},
        {KEYCODE_CPAD_LEFT, "3DS_CPAD_LEFT", "left", "", ""},
        {KEYCODE_CPAD_RIGHT, "3DS_CPAD_RIGHT", "right", "", ""},
        {KEYCODE_CHAT, "3DS_TOUCH_CHAT", "saycommand", "", ""},
        {KEYCODE_TEAM, "3DS_TOUCH_TEAM", "key_teamchat", "", ""},
        {KEYCODE_CONSOLE, "3DS_TOUCH_CONSOLE", "key_commandline", "", ""},
        {KEYCODE_EDIT, "3DS_TOUCH_EDIT", "edittoggle", "", ""},
        {KEYCODE_VOICE, "3DS_TOUCH_VOICE", "key_showmenuvoicecom", "", ""},
        {KEYCODE_MAP, "3DS_TOUCH_MAP", "key_showmap", "", ""},
        {KEYCODE_VOTEYES, "3DS_TOUCH_YES", "key_voteyes", "", ""},
        {KEYCODE_VOTENO, "3DS_TOUCH_NO", "key_voteno", "", ""},
        {KEYCODE_FLAG, "3DS_TOUCH_FLAG", "dropflag", "", ""},
        {KEYCODE_SCREENSHOT, "3DS_TOUCH_SCREENSHOT", "screenshot", "", ""},
        {KEYCODE_WEAPONS, "3DS_TOUCH_WEAPONS", "key_showmenuweapons", "", ""},
        {KEYCODE_NEXT, "3DS_TOUCH_NEXT", "key_universaldelta_plus", "", ""},
        {KEYCODE_PREV, "3DS_TOUCH_PREV", "key_universaldelta_minus", "", ""},
        {KEYCODE_UNDO, "3DS_TOUCH_UNDO", "", "undo", ""},
        {KEYCODE_REDO, "3DS_TOUCH_REDO", "", "redo", ""},
        {KEYCODE_DELETE, "3DS_TOUCH_DELETE", "", "editkey_delete_entity", ""},
        {KEYCODE_EDITMENU, "3DS_TOUCH_EDITMENU", "", "editkey_show_menu", ""}
    };
    static_assert(sizeof(controls) / sizeof(controls[0]) == KEYCODE_EDITMENU - KEYCODE_A + 1,
                  "The default control table follows the native key code order.");

    const struct { u32 mask; Key code; } buttons[] = {
        {KEY_A, KEYCODE_A}, {KEY_B, KEYCODE_B}, {KEY_X, KEYCODE_X}, {KEY_Y, KEYCODE_Y},
        {KEY_L, KEYCODE_L}, {KEY_R, KEYCODE_R}, {KEY_ZL, KEYCODE_ZL}, {KEY_ZR, KEYCODE_ZR},
        {KEY_START, KEYCODE_START}, {KEY_SELECT, KEYCODE_SELECT},
        {KEY_DUP, KEYCODE_UP}, {KEY_DDOWN, KEYCODE_DOWN},
        {KEY_DLEFT, KEYCODE_LEFT}, {KEY_DRIGHT, KEYCODE_RIGHT},
        {KEY_CPAD_UP, KEYCODE_CPAD_UP}, {KEY_CPAD_DOWN, KEYCODE_CPAD_DOWN},
        {KEY_CPAD_LEFT, KEYCODE_CPAD_LEFT}, {KEY_CPAD_RIGHT, KEYCODE_CPAD_RIGHT}
    };

    const struct { int code; const char *label; } touch_controls[][12] = {
        {
            {KEYCODE_CHAT, "CHAT"}, {KEYCODE_TEAM, "TEAM"}, {KEYCODE_CONSOLE, "COMMAND"},
            {KEYCODE_WEAPONS, "WEAPONS"}, {KEYCODE_VOICE, "VOICE"}, {KEYCODE_MAP, "MAP"},
            {KEYCODE_FLAG, "DROP FLAG"}, {KEYCODE_SCREENSHOT, "CAPTURE"},
            {KEYCODE_VOTEYES, "VOTE YES"}, {KEYCODE_VOTENO, "VOTE NO"},
            {KEYCODE_EDIT, "EDIT MAP"}, {SDLK_PAUSE, "PAUSE"}
        },
        {
            {KEYCODE_CHAT, "CHAT"}, {KEYCODE_TEAM, "TEAM"}, {KEYCODE_CONSOLE, "COMMAND"},
            {KEYCODE_EDITMENU, "EDIT TOOLS"}, {KEYCODE_UNDO, "UNDO"}, {KEYCODE_REDO, "REDO"},
            {KEYCODE_NEXT, "NEXT"}, {KEYCODE_PREV, "PREVIOUS"},
            {KEYCODE_DELETE, "DELETE"}, {KEYCODE_SCREENSHOT, "CAPTURE"},
            {KEYCODE_EDIT, "EXIT EDIT"}, {SDLK_PAUSE, "PAUSE"}
        },
        {},
        {
            {KEYCODE_X, "KEYBOARD"}, {SDLK_UP, "HISTORY -"}, {SDLK_DOWN, "HISTORY +"},
            {SDLK_LEFT, "CURSOR <"}, {SDLK_RIGHT, "CURSOR >"}, {SDLK_TAB, "COMPLETE"},
            {SDLK_F1, "HELP"}, {SDLK_F2, "HELP UP"}, {SDLK_F3, "HELP DOWN"},
            {SDLK_BACKSPACE, "DELETE"}, {SDLK_RETURN, "SEND"}, {SDLK_ESCAPE, "CANCEL"}
        },
        {
            {SDLK_F1, "HELP"}, {SDLK_F2, "NAMES"}, {SDLK_F3, "SEARCH"},
            {SDLK_F4, "FAVORITES"}, {SDLK_F5, "REFRESH"}, {SDLK_F6, "SORT DIR"},
            {SDLK_F7, "GOOD ONLY"}, {SDLK_F8, "TIME LEFT"}, {SDLK_F9, "DETAILS"},
            {SDLK_LEFTBRACKET, "PREV VIEW"}, {SDLK_RIGHTBRACKET, "NEXT VIEW"},
            {SDLK_ESCAPE, "BACK"}
        },
        {
            {SDLK_F1, "HELP"}, {SDLK_F2, "UPLINK"}, {SDLK_F5, "REFRESH"},
            {SDLK_m, "ROTATION"}, {SDLK_c, "CONNECTED"}, {SDLK_PAGEUP, "PAGE UP"},
            {SDLK_PAGEDOWN, "PAGE DOWN"}, {SDLK_ESCAPE, "BACK"},
            {0, ""}, {0, ""}, {0, ""}, {0, ""}
        },
        {
            {SDLK_1, "1"}, {SDLK_2, "2"}, {SDLK_3, "3"}, {SDLK_4, "4"},
            {SDLK_5, "5"}, {SDLK_6, "6"}, {SDLK_7, "7"}, {SDLK_8, "8"},
            {SDLK_9, "9"}, {SDLK_0, "0"}, {0, ""}, {0, ""}
        }
    };

    enum Panel { GAME_PANEL, EDIT_PANEL, MENU_PANEL, CONSOLE_PANEL,
                 SERVER_PANEL, SERVER_INFO_PANEL, FAVORITES_PANEL };
    constexpr int touch_width = 320, touch_height = 240;
    constexpr int panel_top = 131, cell_height = 32, cell_width = 78;
    constexpr int menu_top = 33, menu_row_height = 27, menu_footer = 204;
    constexpr int menu_minus = 224, menu_plus = 264;
    int drawn_panel = -1, drawn_bind_type = -1, touch_key = 0;
    Ac3dsMenuView drawn_menu;
    string drawn_status;
    int drawn_touch_aim = -1;
    Uint32 touch_key_context;
    u32 held_buttons, inhibited_buttons;
    bool touch_held, touch_aim, dedicated_display;
    touchPosition last_touch;
    float aim_x, aim_y;
    Uint32 last_poll;
    Uint32 repeat_at[sizeof(buttons) / sizeof(buttons[0])];
    Uint32 button_context = 1;
    const void *input_menu;
    bool input_console, input_editing;

    int current_panel()
    {
        if(getcurcommand(NULL)) return CONSOLE_PANEL;
        switch(ac3ds_menu_panel())
        {
            case Ac3dsMenu::Standard: return MENU_PANEL;
            case Ac3dsMenu::ServerBrowser: return (held_buttons & KEY_SELECT) ? FAVORITES_PANEL : SERVER_PANEL;
            case Ac3dsMenu::ServerInfo: return SERVER_INFO_PANEL;
            default: break;
        }
        return editmode ? EDIT_PANEL : GAME_PANEL;
    }

    bool uses_default_binding(int code, int type)
    {
        if(code < KEYCODE_A || code > KEYCODE_EDITMENU) return true;
        const Control &control = controls[code - KEYCODE_A];
        keym *binding = findbindc(code);
        if(!binding) return true;
        const char *expected = type == keym::ACTION_EDITING ? control.editing :
                               type == keym::ACTION_SPECTATOR ? control.spectator : control.normal;
        const char *actual = binding->actions[type];
        if(!expected[0]) expected = control.normal;
        if(!actual[0]) actual = binding->actions[keym::ACTION_DEFAULT];
        return !strcmp(expected, actual);
    }

    void push_key(int code, bool down, bool repeat = false, Uint32 context = 0)
    {
        SDL_Event event = {};
        event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        event.key.repeat = repeat;
        event.key.keysym.sym = code;
        event.key.windowID = context;
        pushevent(event);
    }

    void reset_physical_input()
    {
        // Physical edges queued for the old menu cannot act in the new one.
        if(!++button_context) ++button_context;
        inhibited_buttons |= held_buttons & ~KEY_TOUCH;
        held_buttons &= KEY_TOUCH;
        aim_x = aim_y = 0.0f;
        last_poll = 0;
        memset(repeat_at, 0, sizeof(repeat_at));
        for(const auto &control : controls)
        {
            keym *binding = findbindc(control.code);
            if(binding && binding->pressed) keypress(control.code, 0, false);
        }
    }

    void release_input()
    {
        reset_physical_input();
        keym *binding = touch_key ? findbindc(touch_key) : NULL;
        if(binding && binding->pressed) keypress(touch_key, 0, false);
        inhibited_buttons |= held_buttons & KEY_TOUCH;
        held_buttons = 0;
        touch_key = 0;
        touch_held = touch_aim = false;
    }

    void apt_event(APT_HookType hook, void *)
    {
        switch(hook)
        {
            case APTHOOK_ONSUSPEND:
            case APTHOOK_ONSLEEP:
            case APTHOOK_ONEXIT:
                ac3ds_audio_lifecycle_event(true);
                break;
            case APTHOOK_ONRESTORE:
            case APTHOOK_ONWAKEUP:
                ac3ds_audio_lifecycle_event(false);
                clock_reset = true;
                controls_dirty = true;
                break;
            default:
                break;
        }
    }

    bool select_assets(const char *path)
    {
        struct stat info;
        return chdir(path) == 0 && stat("config/font.cfg", &info) == 0 && S_ISREG(info.st_mode) &&
               stat("packages/misc/notexture.jpg", &info) == 0 && S_ISREG(info.st_mode);
    }

    void add_cstick_aim(const circlePosition &stick, float speed, float &dx, float &dy)
    {
        const float x = stick.dx, y = stick.dy;
        const float magnitude = sqrtf(x*x + y*y);
        if(magnitude <= cstickdeadzone) return;
        const float normalized = min(1.0f, (magnitude - cstickdeadzone) / (105 - cstickdeadzone));
        const float response = normalized * (0.32f + 0.68f * normalized) * speed / magnitude;
        dx += x * response;
        dy -= y * response * cstickysensitivity;
    }

    void fill_rect(u16 *screen, int x0, int y0, int x1, int y1, u16 color)
    {
        for(int x = x0; x < x1; ++x)
        {
            u16 *column = screen + x * touch_height;
            for(int y = y0; y < y1; ++y) column[touch_height - 1 - y] = color;
        }
    }

    void draw_label(u16 *screen, int x, int y, const char *label, u16 color,
                    int right = touch_width)
    {
        const ConsoleFont &font = consoleGetDefault()->font;
        int columns = max(0, (right - x) / 8);
        const bool clipped = int(strlen(label)) > columns;
        for(int i = 0; label[i] && i < columns; ++i, x += 8)
        {
            unsigned char c = clipped && i >= max(0, columns - 3) ? '.' : label[i];
            int index = c - font.asciiOffset;
            if(index < 0 || index >= font.numChars) continue;
            const u8 *glyph = font.gfx + 8 * index;
            for(int row = 0; row < 8; ++row)
                for(int col = 0; col < 8; ++col)
                    if(glyph[row] & (0x80 >> col))
                        screen[(x + col) * touch_height + touch_height - 1 - y - row] = color;
        }
    }

    const u16 background = RGB565(2, 5, 4), surface = RGB565(4, 10, 7);
    const u16 border = RGB565(7, 17, 11), text = RGB565(29, 60, 28);
    const u16 muted = RGB565(18, 39, 19), accent = RGB565(21, 52, 14);

    void draw_button(u16 *screen, int x, int y, int w, int h, const char *label, bool enabled = true)
    {
        fill_rect(screen, x, y, x + w, y + h, enabled ? border : surface);
        fill_rect(screen, x + 1, y + 1, x + w - 1, y + h - 1, surface);
        int length = min(int(strlen(label)), (w - 2) / 8);
        draw_label(screen, x + (w - 8 * length) / 2, y + (h - 8) / 2, label,
                   enabled ? text : muted, x + w - 1);
    }

    bool same_menu_view(const Ac3dsMenuView &a, const Ac3dsMenuView &b)
    {
        if(a.revision != b.revision || a.selection != b.selection || a.page != b.page ||
           a.pages != b.pages || a.back != b.back || a.binding != b.binding || strcmp(a.title, b.title)) return false;
        for(int i = 0; i < Ac3dsMenuView::row_count; ++i)
        {
            const Ac3dsMenuRow &x = a.rows[i], &y = b.rows[i];
            if(x.index != y.index || x.kind != y.kind || x.disabled != y.disabled ||
               strcmp(x.label, y.label) || strcmp(x.value, y.value)) return false;
        }
        return true;
    }

    void draw_handheld_menu(u16 *screen, const Ac3dsMenuView &view)
    {
        fill_rect(screen, 0, 0, touch_width, 29, surface);
        fill_rect(screen, 0, 28, touch_width, 29, border);
        draw_label(screen, 10, 10, view.title, text, view.pages > 1 ? 251 : 310);
        if(view.pages > 1)
        {
            defformatstring(page)("%d/%d", view.page + 1, view.pages);
            draw_label(screen, 310 - 8 * int(strlen(page)), 10, page, accent);
        }
        for(int i = 0; i < Ac3dsMenuView::row_count; ++i)
        {
            const Ac3dsMenuRow &row = view.rows[i];
            if(row.index < 0) break;
            const int y = menu_top + i * menu_row_height;
            const bool selected = row.index == view.selection;
            const u16 color = row.disabled ? muted : text;
            fill_rect(screen, 8, y, 312, y + 24, selected ? RGB565(8, 19, 11) : surface);
            fill_rect(screen, 8, y, 11, y + 24, selected ? accent : border);
            const int value_right = row.kind == Ac3dsMenuItem::Slider ? menu_minus - 4 : 302;
            const int value_length = min(int(strlen(row.value)), 12);
            const int value_left = value_right - 8 * value_length;
            draw_label(screen, 18, y + 8, row.label, color,
                       value_length ? value_left - 7 : value_right - 16);
            if(value_length) draw_label(screen, value_left, y + 8, row.value, row.disabled ? muted : accent, value_right);
            if(row.kind == Ac3dsMenuItem::Slider)
            {
                draw_button(screen, menu_minus + 1, y, menu_plus - menu_minus - 3, 24, "-", !row.disabled);
                draw_button(screen, menu_plus + 1, y, 312 - menu_plus - 1, 24, "+", !row.disabled);
            }
            else if(row.kind == Ac3dsMenuItem::Action && !row.disabled)
                draw_label(screen, 294, y + 8, ">", selected ? accent : muted);
        }
        if(view.binding)
            draw_label(screen, 12, 218, "Press a button", accent);
        else
        {
            draw_button(screen, 8, menu_footer, 98, 33, "L  Prev", view.page > 0);
            draw_button(screen, 111, menu_footer, 98, 33, "R  Next", view.page + 1 < view.pages);
        }
        draw_button(screen, 214, menu_footer, 98, 33, view.binding ? "Cancel" : view.back ? "B  Back" : "B  Close");
    }

    void queue_menu_touch(const touchPosition &touch)
    {
        if(!drawn_menu.revision || touch.px < 8 || touch.px >= 312) return;
        int index = -1;
        Ac3dsMenuTouch action = Ac3dsMenuTouch::Activate;
        if(touch.py >= menu_footer)
        {
            if(touch.px >= 214) action = Ac3dsMenuTouch::Back;
            else if(!drawn_menu.binding && touch.px < 106 && drawn_menu.page > 0) action = Ac3dsMenuTouch::Previous;
            else if(!drawn_menu.binding && touch.px >= 111 && touch.px < 209 &&
                    drawn_menu.page + 1 < drawn_menu.pages) action = Ac3dsMenuTouch::Next;
            else return;
        }
        else
        {
            const int y = int(touch.py) - menu_top;
            if(y < 0 || y / menu_row_height >= Ac3dsMenuView::row_count || y % menu_row_height >= 24) return;
            const Ac3dsMenuRow &row = drawn_menu.rows[y / menu_row_height];
            if(row.index < 0) return;
            index = row.index;
            if(row.kind == Ac3dsMenuItem::Slider && touch.px >= menu_minus)
                action = touch.px >= menu_plus ? Ac3dsMenuTouch::Increase : Ac3dsMenuTouch::Decrease;
        }
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = AC3DS_MENU_TOUCH_EVENT;
        event.user.windowID = drawn_menu.revision;
        event.user.data1 = reinterpret_cast<void *>(intptr_t(index));
        event.user.data2 = reinterpret_cast<void *>(intptr_t(action));
        pushevent(event);
    }
}

const char *ac3ds_system_init()
{
    SDL_SetMainReady();
    bool is_new = false;
    if(R_FAILED(APT_CheckNew3DS(&is_new)) || !is_new)
        return "AssaultCube requires a New Nintendo 3DS or New Nintendo 2DS XL.";
    osSetSpeedupEnable(true);
    mkdir("sdmc:/3ds", 0777);
    mkdir(game_dir, 0777);
    romfs_ready = R_SUCCEEDED(romfsInit());
    if(!(romfs_ready && select_assets("romfs:/")) && !select_assets(game_dir))
        return "AssaultCube assets were not found. Install the Lockdown Edition config and packages directories under /3ds/assaultcube.";

    gfxInit(GSP_BGR8_OES, GSP_RGB565_OES, false);
    gfx_ready = true;
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    if(R_SUCCEEDED(gspLcdInit()))
    {
        lcd_ready = R_SUCCEEDED(GSPLCD_GetBrightness(GSPLCD_SCREEN_TOP, &original_brightness));
        if(!lcd_ready) gspLcdExit();
    }
    aptHook(&apt_cookie, apt_event, NULL);
    apt_hooked = true;
    return NULL;
}

void ac3ds_system_shutdown()
{
    if(apt_hooked)
    {
        aptUnhook(&apt_cookie);
        apt_hooked = false;
    }
    if(network_ready)
    {
        socExit();
        network_ready = false;
    }
    free(socket_buffer);
    socket_buffer = NULL;
    if(romfs_ready)
    {
        chdir("sdmc:/");
        romfsExit();
        romfs_ready = false;
    }
    if(lcd_ready)
    {
        GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, original_brightness);
        gspLcdExit();
        lcd_ready = false;
    }
    if(gfx_ready)
    {
        gfxExit();
        gfx_ready = false;
    }
    osSetSpeedupEnable(false);
}

void ac3ds_show_error(const char *message)
{
    if(aptShouldClose()) return;
    if(!gfx_ready)
    {
        gfxInitDefault();
        gfx_ready = true;
    }
    errorConf dialog;
    errorInit(&dialog, ERROR_TEXT_WORD_WRAP, CFG_LANGUAGE_EN);
    errorText(&dialog, message);
    errorDisp(&dialog);
}

bool ac3ds_main_loop()
{
    while(aptMainLoop())
    {
        active = aptIsActive();
        if(active)
        {
            if(clock_reset) release_input();
            return true;
        }
        release_input();
        clock_reset = true;
        gspWaitForVBlank();
    }
    active = false;
    ac3ds_audio_lifecycle_event(true);
    ac3ds_audio_service();
    return false;
}

bool ac3ds_take_clock_reset()
{
    bool reset = clock_reset;
    clock_reset = false;
    return reset;
}

bool ac3ds_start_network()
{
    if(network_ready) return true;
    socket_buffer = static_cast<u32 *>(memalign(0x1000, socket_buffer_size));
    if(!socket_buffer) return false;
    network_ready = R_SUCCEEDED(socInit(socket_buffer, socket_buffer_size));
    if(!network_ready)
    {
        free(socket_buffer);
        socket_buffer = NULL;
    }
    return network_ready;
}

void ac3ds_install_keymap()
{
    extern void keymap(int *code, char *name);
    for(const auto &control : controls)
    {
        int code = control.code;
        string name;
        copystring(name, control.name);
        keymap(&code, name);
    }
}

void ac3ds_default_binds()
{
    extern int _defaultbinds;
    int previous = _defaultbinds;
    _defaultbinds = 1;
    for(const auto &control : controls)
    {
        bindc(control.code, control.normal, keym::ACTION_DEFAULT);
        bindc(control.code, control.editing, keym::ACTION_EDITING);
        bindc(control.code, control.spectator, keym::ACTION_SPECTATOR);
    }
    _defaultbinds = previous;
}

int ac3ds_menu_key(int code)
{
    switch(code)
    {
        case KEYCODE_A: return SDLK_RETURN;
        case KEYCODE_B:
        case KEYCODE_START: return SDLK_ESCAPE;
        case KEYCODE_X: return SDLK_F5;
        case KEYCODE_Y: return SDLK_F12;
        case KEYCODE_L:
        case KEYCODE_ZL: return SDLK_PAGEUP;
        case KEYCODE_R:
        case KEYCODE_ZR: return SDLK_PAGEDOWN;
        case KEYCODE_UP:
        case KEYCODE_CPAD_UP: return SDLK_UP;
        case KEYCODE_DOWN:
        case KEYCODE_CPAD_DOWN: return SDLK_DOWN;
        case KEYCODE_LEFT:
        case KEYCODE_CPAD_LEFT: return SDLK_LEFT;
        case KEYCODE_RIGHT:
        case KEYCODE_CPAD_RIGHT: return SDLK_RIGHT;
        default: return code;
    }
}

int ac3ds_console_key(int code)
{
    switch(code)
    {
        case KEYCODE_Y: return SDLK_TAB;
        case KEYCODE_L: return SDLK_BACKSPACE;
        case KEYCODE_R: return SDLK_DELETE;
        default: return ac3ds_menu_key(code);
    }
}

bool ac3ds_keyboard_key(int code)
{
    return code == KEYCODE_X;
}

bool ac3ds_binding_cancel_key(int code)
{
    return code == KEYCODE_START;
}

bool ac3ds_edit_text(const char *title, const char *initial, char *out, size_t capacity,
                     int maxchars, bool password)
{
    if(capacity < 2 || maxchars < 1 || aptShouldClose() || !aptIsActive()) return false;
    ac3ds_gl_end_frame();
    extern void ac3ds_discard_pending_input();
    ac3ds_discard_pending_input();
    release_input();
    ac3ds_audio_lifecycle_event(true);
    ac3ds_audio_service();

    SwkbdState keyboard;
    swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, min(maxchars, int(capacity - 1)));
    swkbdSetFeatures(&keyboard, SWKBD_DARKEN_TOP_SCREEN | SWKBD_DEFAULT_QWERTY | SWKBD_ALLOW_HOME);
    swkbdSetHintText(&keyboard, title);
    swkbdSetInitialText(&keyboard, initial);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "OK", true);
    if(password) swkbdSetPasswordMode(&keyboard, SWKBD_PASSWORD_HIDE);
    SwkbdButton button = swkbdInputText(&keyboard, out, capacity);

    hidScanInput();
    inhibited_buttons |= hidKeysHeld();
    clock_reset = controls_dirty = true;
    if(!aptShouldClose() && aptIsActive())
    {
        ac3ds_audio_lifecycle_event(false);
        ac3ds_audio_service();
    }
    return button == SWKBD_BUTTON_CONFIRM;
}

bool ac3ds_is_physical_button(int code)
{
    return code >= KEYCODE_A && code <= KEYCODE_CPAD_RIGHT;
}

bool ac3ds_gameplay_input_active()
{
    return !getcurcommand(NULL) && !ac3ds_menu_identity();
}

void ac3ds_sync_input_context()
{
    const void *menu = ac3ds_menu_identity();
    bool console = getcurcommand(NULL) != NULL;
    if(menu == input_menu && console == input_console && editmode == input_editing) return;
    input_menu = menu;
    input_console = console;
    input_editing = editmode;
    touch_aim = false;
    reset_physical_input();
}

bool ac3ds_accept_button_event(const SDL_KeyboardEvent &event)
{
    if(event.keysym.sym < KEYCODE_A || event.keysym.sym > KEYCODE_EDITMENU || !event.windowID) return true;
    ac3ds_sync_input_context();
    return event.windowID == button_context;
}

void ac3ds_poll_input()
{
    if(!active) return;
    hidScanInput();
    if(!dedicated_display) ac3ds_sync_input_context();
    u32 held = hidKeysHeld();
    inhibited_buttons &= held;
    held &= ~inhibited_buttons;
    if(dedicated_display)
    {
        if((held & KEY_B) && !(held_buttons & KEY_B)) push_key(KEYCODE_B, true);
        if((held & KEY_START) && !(held_buttons & KEY_START)) push_key(KEYCODE_START, true);
        held_buttons = held;
        return;
    }
    int panel = current_panel();
    Uint32 now = SDL_GetTicks();
    bool can_repeat = panel >= MENU_PANEL;
    for(size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
    {
        const auto &button = buttons[i];
        bool down = (held & button.mask) != 0;
        if(down != ((held_buttons & button.mask) != 0))
        {
            push_key(button.code, down, false, button_context);
            repeat_at[i] = down ? now + 340 : 0;
        }
        else if(down && can_repeat && (Sint32)(now - repeat_at[i]) >= 0)
        {
            int code = ac3ds_menu_key(button.code);
            if(code == SDLK_UP || code == SDLK_DOWN || code == SDLK_LEFT || code == SDLK_RIGHT)
                push_key(button.code, true, true, button_context);
            repeat_at[i] = now + 90;
        }
    }
    held_buttons = held;
    panel = current_panel();

    float dx = 0.0f, dy = 0.0f;
    bool can_aim = panel == GAME_PANEL || panel == EDIT_PANEL;
    if(held & KEY_TOUCH)
    {
        touchPosition touch;
        hidTouchRead(&touch);
        if(!touch_held)
        {
            touch_aim = touchaim && touch.py >= 33 && touch.py < panel_top && can_aim && drawn_panel == panel;
            if(drawn_panel == panel && panel == MENU_PANEL) queue_menu_touch(touch);
            else if(drawn_panel == panel && touch.py >= panel_top)
            {
                int row = (touch.py - panel_top) / cell_height;
                int x = int(touch.px) - 4;
                int col = x / cell_width;
                if(row < 3 && x >= 0 && col < 4 && x % cell_width < cell_width - 3 &&
                   (touch.py - panel_top) % cell_height < cell_height - 3)
                {
                    touch_key = touch_controls[panel][row * 4 + col].code;
                    touch_key_context = button_context;
                    if(touch_key) push_key(touch_key, true, false, touch_key_context);
                }
            }
        }
        else if(touch_aim && touchaim && can_aim)
        {
            dx = (int(touch.px) - int(last_touch.px)) * touchsensitivity;
            dy = (int(touch.py) - int(last_touch.py)) * touchsensitivity;
        }
        last_touch = touch;
        touch_held = true;
    }
    else if(touch_held)
    {
        if(touch_key && touch_key_context == button_context) push_key(touch_key, false, false, touch_key_context);
        touch_key = 0;
        touch_held = touch_aim = false;
    }

    if(can_aim)
    {
        circlePosition stick;
        hidCstickRead(&stick);
        float elapsed = last_poll ? min(now - last_poll, Uint32(40)) / 1000.0f : 0.0f;
        float speed = 1800.0f * elapsed * csticksensitivity;
        add_cstick_aim(stick, speed, dx, dy);
        aim_x += dx;
        aim_y += dy;
        int ix = int(aim_x), iy = int(aim_y);
        aim_x -= ix;
        aim_y -= iy;
        if(ix || iy)
        {
            SDL_Event event = {};
            event.type = SDL_MOUSEMOTION;
            event.motion.xrel = ix;
            event.motion.yrel = iy;
            pushevent(event);
        }
    }
    else aim_x = aim_y = 0.0f;
    last_poll = now;
}

void ac3ds_draw_controls()
{
    const int panel = current_panel();
    const int bind_type = editmode ? keym::ACTION_EDITING :
                          player1 && player1->isspectating() ? keym::ACTION_SPECTATOR : keym::ACTION_DEFAULT;
    Ac3dsMenuView menu;
    if(panel == MENU_PANEL) ac3ds_menu_view(menu);
    string status = "";
    if(panel == GAME_PANEL && player1)
    {
        extern int paused;
        if(watchingdemo) copystring(status, "Demo playback");
        else if(paused) copystring(status, "Match paused");
        else if(player1->isspectating()) copystring(status, "Spectating");
        else if(player1->state == CS_ALIVE && player1->weaponsel)
        {
            const weapon &gun = *player1->weaponsel;
            const bool reloading = gun.reloading && lastmillis < gun.reloading + gun.info.reloadtime;
            if(reloadable_gun(gun.type))
                formatstring(status)("%s  %s%d/%d", gun.info.title, reloading ? "Reloading  " : "", gun.mag, gun.ammo);
            else if(gun.type == GUN_GRENADE) formatstring(status)("%s  %d", gun.info.title, gun.mag);
            else copystring(status, gun.info.title);
        }
        else copystring(status, "Match");
    }

    if(!controls_dirty && drawn_panel == panel && drawn_bind_type == bind_type &&
       drawn_touch_aim == touchaim && !strcmp(status, drawn_status) &&
       (panel != MENU_PANEL || same_menu_view(menu, drawn_menu))) return;
    u16 *screen = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    if(!screen) return;
    fill_rect(screen, 0, 0, touch_width, touch_height, background);

    if(panel == MENU_PANEL) draw_handheld_menu(screen, menu);
    else
    {
        const char *title = panel == GAME_PANEL ? "Match controls" :
                            panel == EDIT_PANEL ? "Map editor" :
                            panel == CONSOLE_PANEL ? "Command line" :
                            panel == FAVORITES_PANEL ? "Favorite groups" :
                            panel == SERVER_INFO_PANEL ? "Server details" : "Server browser";
        fill_rect(screen, 0, 0, touch_width, 29, surface);
        fill_rect(screen, 0, 28, touch_width, 29, border);
        draw_label(screen, 10, 10, title, text);

        if(panel == GAME_PANEL || panel == EDIT_PANEL)
        {
            if(*status) draw_label(screen, 10, 42, status, text, 310);
            else draw_label(screen, 10, 42, "Circle Pad  Move   C-Stick  Aim", text);
            if(panel == GAME_PANEL)
            {
                if(uses_default_binding(KEYCODE_L, bind_type) && uses_default_binding(KEYCODE_ZR, bind_type))
                    draw_label(screen, 10, 63, "L  Fire   ZR  Crouch", muted);
                if(uses_default_binding(KEYCODE_START, bind_type) && uses_default_binding(KEYCODE_SELECT, bind_type))
                    draw_label(screen, 10, 83, "START  Menu   SELECT  Scores", muted);
            }
            else if(uses_default_binding(KEYCODE_L, bind_type) && uses_default_binding(KEYCODE_R, bind_type))
                draw_label(screen, 10, 63, "L  Select area   R  Edit tools", muted);

            if(touchaim)
            {
                fill_rect(screen, 4, 102, 316, 128, surface);
                fill_rect(screen, 4, 102, 6, 128, border);
                draw_label(screen, 92, 111, "Drag here to aim", accent);
            }
        }
        else if(panel == CONSOLE_PANEL)
        {
            draw_label(screen, 10, 47, "X  Open keyboard", text);
            draw_label(screen, 10, 69, "A  Submit    B  Cancel", muted);
        }
        else
        {
            draw_label(screen, 10, 47, "D-Pad  Navigate    A  Select", text);
            draw_label(screen, 10, 69, "B  Back    X  Refresh", muted);
            if(panel != SERVER_INFO_PANEL)
                draw_label(screen, 10, 91, "Hold SELECT for favorite groups", muted);
        }

        for(int row = 0; row < 3; ++row)
            for(int col = 0; col < 4; ++col)
            {
                const auto &control = touch_controls[panel][row * 4 + col];
                if(!control.code) continue;
                const char *label = panel <= EDIT_PANEL && !uses_default_binding(control.code, bind_type) ?
                                    "CUSTOM" : control.label;
                draw_button(screen, 4 + col * cell_width, panel_top + row * cell_height,
                            cell_width - 3, cell_height - 3, label);
            }
    }
    controls_dirty = R_FAILED(GSPGPU_FlushDataCache(screen, touch_width * touch_height * sizeof(*screen)));
    drawn_panel = panel;
    drawn_bind_type = bind_type;
    drawn_menu = menu;
    drawn_touch_aim = touchaim;
    copystring(drawn_status, status);
}

void ac3ds_controls_changed()
{
    controls_dirty = true;
}

void ac3ds_draw_server_controls()
{
    dedicated_display = true;
    u16 *screen = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    if(!screen) return;
    fill_rect(screen, 0, 0, touch_width, touch_height, RGB565(2, 5, 3));
    fill_rect(screen, 0, 0, touch_width, 26, RGB565(4, 13, 7));
    draw_label(screen, 12, 9, "ASSAULTCUBE", RGB565(25, 60, 19));
    draw_label(screen, 12, 51, "Dedicated server", RGB565(29, 58, 29));
    draw_label(screen, 12, 79, "Press B or START to stop.", RGB565(20, 42, 21));
    GSPGPU_FlushDataCache(screen, touch_width * touch_height * sizeof(*screen));
}

int ac3ds_set_brightness(int percent)
{
    if(!lcd_ready) return 0;
    u32 value = min<u32>(172, max<u32>(1, original_brightness * percent / 100u));
    if(percent == 100) value = original_brightness;
    return R_SUCCEEDED(GSPLCD_SetBrightness(GSPLCD_SCREEN_TOP, value)) ? 0 : -1;
}

stream *ac3ds_config_buffer()
{
    return new ConfigBuffer();
}

bool ac3ds_install_config(const char *basename, stream *config)
{
    long size = config->size();
    if(size <= 0 || size > max_config_size) return false;
    filerotate(basename, "cfg", CONFIGROTATEMAX, NULL, true);
    defformatstring(path)("%s.cfg", basename);
    return installfile3ds(path, config, max_config_size);
}

#include <3ds.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>

#include "cube.h"
#include "audio_3ds.h"
#include "system.h"
#include "crash_3ds.h"
#include "ac3ds_servers.h"

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
extern bool inmainloop;
extern void writeinitcfg();
extern void writecfg();
VARP(touchaim, 0, 1, 1);
// Double-tap the look area of the match screen to jump (taps under 250 ms,
// second tap within doubletapms of the first and near it).
VARP(doubletapjump, 0, 1, 1);
VARP(doubletapms, 150, 300, 600);

// Network keepalive while the main thread is blocked in a system applet (the
// software keyboard) or suspended in the HOME menu. Runs upstream's own
// c2skeepalive() so the server does not time the 3DS out; the main thread does
// not touch ENet while the flag is set, and takes the lock before it resumes.
namespace
{
    LightLock netkeep_lock;
    LightEvent netkeep_wake;
    Thread netkeep_thread = NULL;
    std::atomic<bool> netkeep_active{false};
    u64 netkeep_started = 0; // written before netkeep_active is set, read after it is seen
    // ENet queues every packet that arrives while nobody dispatches events, with
    // no cap. Past this long in the HOME menu or the software keyboard the
    // keepalive stops and the server times the client out as it would a
    // closed lid, instead of the game replaying minutes of backlog (or
    // running out of heap) on return.
    constexpr u64 netkeep_budget_ms = 3 * 60 * 1000;

    void netkeep_main(void *)
    {
        for(;;)
        {
            LightEvent_WaitTimeout(&netkeep_wake, 50000000LL); // 50 ms
            if(!netkeep_active.load(std::memory_order_acquire)) continue;
            if(osGetTime() - netkeep_started > netkeep_budget_ms)
            {
                netkeep_active.store(false, std::memory_order_release);
                continue;
            }
            LightLock_Lock(&netkeep_lock);
            if(netkeep_active.load(std::memory_order_acquire)) c2skeepalive();
            LightLock_Unlock(&netkeep_lock);
        }
    }

    void netkeep_begin()
    {
        if(!netkeep_thread)
        {
            LightLock_Init(&netkeep_lock);
            LightEvent_Init(&netkeep_wake, RESET_ONESHOT);
            s32 priority = 0x30;
            svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
            const s32 lower = priority + 1 > 0x3f ? 0x3f : priority + 1;
            netkeep_thread = threadCreate(netkeep_main, NULL, 16 * 1024, lower, 2, true);
            if(!netkeep_thread) netkeep_thread = threadCreate(netkeep_main, NULL, 16 * 1024, lower, 0, true);
            if(!netkeep_thread) return;
        }
        netkeep_started = osGetTime();
        netkeep_active.store(true, std::memory_order_release);
        LightEvent_Signal(&netkeep_wake);
    }

    void netkeep_end()
    {
        if(!netkeep_thread) return;
        netkeep_active.store(false, std::memory_order_release);
        // Wait out a keepalive in progress before the main thread uses ENet.
        LightLock_Lock(&netkeep_lock);
        LightLock_Unlock(&netkeep_lock);
    }
}

namespace
{
    const char *const game_dir = "sdmc:/3ds/assaultcube";
    constexpr size_t socket_buffer_size = 1 << 20;
    constexpr int max_config_size = 1 << 20;
    bool gfx_ready, romfs_ready, network_ready, apt_hooked, lcd_ready;
    bool clock_reset, controls_dirty = true, active = true;
    bool resumed_from_suspend = false; // HOME menu or sleep, not the keyboard applet
    bool in_keyboard_applet = false;   // the software keyboard also fires the suspend/restore hooks
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
        KEYCODE_PREV, KEYCODE_UNDO, KEYCODE_REDO, KEYCODE_DELETE, KEYCODE_EDITMENU,
        KEYCODE_RELOAD, KEYCODE_GRENADE, KEYCODE_SCOPE, KEYCODE_NEXTWEAPON,
        KEYCODE_MATCHMENU, KEYCODE_SCORES, KEYCODE_JUMP,
        KEYCODE_LAST = KEYCODE_JUMP
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
        {KEYCODE_EDITMENU, "3DS_TOUCH_EDITMENU", "", "editkey_show_menu", ""},
        {KEYCODE_RELOAD, "3DS_TOUCH_RELOAD", "reload", "", ""},
        {KEYCODE_GRENADE, "3DS_TOUCH_GRENADE", "quicknadethrow", "", ""},
        {KEYCODE_SCOPE, "3DS_TOUCH_SCOPE", "altaction", "", ""},
        {KEYCODE_NEXTWEAPON, "3DS_TOUCH_WEAPON", "shiftweapon 1", "", ""},
        {KEYCODE_MATCHMENU, "3DS_TOUCH_MENU", "ac3ds_pause_menu", "", ""},
        {KEYCODE_SCORES, "3DS_TOUCH_SCORES", "key_showscores", "", ""},
        {KEYCODE_JUMP, "3DS_TOUCH_JUMP", "jump", "", ""} // double-tap on the match screen
    };
    static_assert(sizeof(controls) / sizeof(controls[0]) == KEYCODE_LAST - KEYCODE_A + 1,
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
            {SDLK_F4, "FAVORITES"}, {SDLK_F5, "SCAN"}, {SDLK_F6, "SORT DIR"},
            {SDLK_F7, "GOOD ONLY"}, {SDLK_F8, "TIME LEFT"}, {SDLK_F9, "DETAILS"},
            {SDLK_LEFTBRACKET, "PREV VIEW"}, {SDLK_RIGHTBRACKET, "NEXT VIEW"},
            {SDLK_ESCAPE, "BACK"}
        },
        {
            {SDLK_F1, "HELP"}, {SDLK_F2, "UPLINK"}, {SDLK_F5, "SCAN"},
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
        if(code < KEYCODE_A || code > KEYCODE_LAST) return true;
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

    // Double tap on the look area of the match screen. A tap is a touch that
    // lifts within 250 ms and stays within 12 px of where it started; the
    // second touch must land within doubletapms of the first and near it. The
    // jump fires on the second touch-down, and that touch can still drag to aim.
    struct DoubleTap
    {
        Uint32 down_at = 0, tap_at = 0;
        int start_x = 0, start_y = 0, tap_x = 0, tap_y = 0;
        bool tracking = false, moved = false, have_tap = false;

        bool down(Uint32 now, int x, int y, Uint32 window)
        {
            const bool second = have_tap && now - tap_at <= window &&
                                abs(x - tap_x) <= 40 && abs(y - tap_y) <= 40;
            have_tap = false;
            tracking = !second; // a third tap starts a new pair
            moved = false;
            down_at = now;
            start_x = x;
            start_y = y;
            return second;
        }
        void move(int x, int y)
        {
            if(abs(x - start_x) > 12 || abs(y - start_y) > 12) moved = true;
        }
        void up(Uint32 now)
        {
            have_tap = tracking && !moved && now - down_at <= 250;
            if(have_tap)
            {
                tap_at = now;
                tap_x = start_x;
                tap_y = start_y;
            }
            tracking = false;
        }
        void reset() { tracking = have_tap = false; }
    } double_tap;

    // QA: a scripted stylus sequence fed through the real input path.
    struct QaTouch
    {
        int x = 0, y = 0, presses = 0, hold = 0, gap = 0, jumps = 0;
        Uint32 start = 0;
        float base_z = 0, peak_z = 0;
        bool active = false;
    } qa_touch;

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
                netkeep_begin(); // HOME menu: stay connected
                [[fallthrough]];
            case APTHOOK_ONSLEEP:
            {
                // aptMainLoop runs this hook from the top of a frame on the main
                // thread; save settings now in case the console is powered off
                // from the HOME menu or while asleep.
                if(inmainloop && !in_keyboard_applet) { writeinitcfg(); writecfg(); }
                ac3ds_crash_pause(true); // the main loop legitimately stops
                ac3ds_audio_lifecycle_event(true);
                break;
            }
            case APTHOOK_ONEXIT:
                ac3ds_crash_pause(true); // the main loop legitimately stops
                ac3ds_audio_lifecycle_event(true);
                break;
            case APTHOOK_ONRESTORE:
            case APTHOOK_ONWAKEUP:
                if(!in_keyboard_applet) resumed_from_suspend = true;
                netkeep_end();
                ac3ds_crash_pause(false);
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

    // Pixel-doubled glyphs for headings: the 8x8 console font is too small
    // to read at arm's length on the New 2DS XL's 4.18" lower panel.
    void draw_label_large(u16 *screen, int x, int y, const char *label, u16 color, int scale = 2)
    {
        const ConsoleFont &font = consoleGetDefault()->font;
        for(int i = 0; label[i] && x + 8 * scale <= touch_width; ++i, x += 8 * scale)
        {
            int index = (unsigned char)label[i] - font.asciiOffset;
            if(index < 0 || index >= font.numChars) continue;
            const u8 *glyph = font.gfx + 8 * index;
            for(int row = 0; row < 8; ++row)
                for(int col = 0; col < 8; ++col)
                    if(glyph[row] & (0x80 >> col))
                        fill_rect(screen, x + col * scale, y + row * scale, x + (col + 1) * scale, y + (row + 1) * scale, color);
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

// Measured ratio of CPU speed with the New 3DS boost requested vs not; about
// 3.0 means 804 MHz is really in effect for this .3dsx, about 1.0 means not.
float ac3ds_cpu_clock_ratio = 0.0f;

static __attribute__((noinline)) u64 time_spin(u32 iterations)
{
    const u64 start = svcGetSystemTick();
    asm volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(iterations) :: "cc");
    return svcGetSystemTick() - start;
}

static float measure_cpu_boost()
{
    constexpr u32 iterations = 400000;
    osSetSpeedupEnable(false);
    svcSleepThread(2000000LL);
    u64 slow = time_spin(iterations);
    slow = min(slow, time_spin(iterations));
    osSetSpeedupEnable(true);
    svcSleepThread(2000000LL);
    u64 fast = time_spin(iterations);
    fast = min(fast, time_spin(iterations));
    return fast ? float(double(slow) / double(fast)) : 0.0f;
}

const char *ac3ds_system_init()
{
    SDL_SetMainReady();
    bool is_new = false;
    if(R_FAILED(APT_CheckNew3DS(&is_new)) || !is_new)
        return "AssaultCube requires a New Nintendo 3DS or New Nintendo 2DS XL.";
    ac3ds_cpu_clock_ratio = measure_cpu_boost(); // leaves the boost on
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
    netkeep_end(); // nothing may use the sockets past this point
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
    // HOME -> Close wakes the app with APTCMD_WAKEUP_CANCEL, for which libctru
    // runs no restore hook: stop the keepalive thread here, before the caller
    // tears ENet down.
    netkeep_end();
    active = false;
    ac3ds_audio_lifecycle_event(true);
    ac3ds_audio_service();
    return false;
}

void ac3ds_set_hosting(bool hosting)
{
    aptSetSleepAllowed(!hosting);
    aptSetHomeAllowed(!hosting);
}

bool ac3ds_take_resume()
{
    const bool resumed = resumed_from_suspend;
    resumed_from_suspend = false;
    return resumed;
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
    ac3ds_crash_pause(true); // the keyboard applet blocks the main loop
    netkeep_begin();         // ...so keep the server connection serviced meanwhile
    in_keyboard_applet = true;
    SwkbdButton button = swkbdInputText(&keyboard, out, capacity);
    in_keyboard_applet = false;
    netkeep_end();
    ac3ds_crash_pause(false);

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

extern votedisplayinfo *curvote;
bool ac3ds_radar_view(playerent *&p, playerent *&d, bool &spect3rd, int &p_baseteam, float &yaw);
bool ac3ds_radar_shows_player(playerent *p, playerent *d, bool spect3rd, int p_baseteam, playerent *pl);
bool ac3ds_radar_shows_thief(playerent *p, int p_baseteam, int flag);

// Lower-screen loading state (see ac3ds_loading_screen).
namespace
{
    char loading_title[64] = "", loading_stage[64] = "";
    int loading_percent = -1;
    u64 loading_drawn_tick = 0;
}

// ---------------------------------------------------------------------------
// Match screen: the lower screen during play. The map fills the whole screen
// and doubles as the stylus look area. Only the two actions players reach for
// mid-fight sit on it (RELOAD top centre, GRENADE bottom centre), plus a small
// MAP/RADAR toggle; everything else lives in a drawer that slides out from a
// small tab in the top-left corner, so it is always reachable but never in
// the way.
namespace
{
    constexpr int radar_top = 0, radar_bottom = touch_height;
    struct HudButton { int x0, y0, x1, y1, code; const char *label; bool large; };
    enum { HUD_MAPTOGGLE = 1 }; // local actions, not key codes
    const HudButton hud_buttons[] = {
        {100, 4, 220, 40, KEYCODE_RELOAD, "RELOAD", true},
        {92, 200, 228, 236, KEYCODE_GRENADE, "GRENADE", true},
        {262, 206, 316, 236, HUD_MAPTOGGLE, "MAP", false},
    };
    constexpr int hud_button_count = sizeof(hud_buttons) / sizeof(hud_buttons[0]);

    // The drawer tab and its items.
    const HudButton drawer_tab = {0, 4, 30, 40, 0, "", false};
    constexpr int drawer_width = 124;
    const HudButton drawer_items[] = {
        {6, 48, 118, 88, KEYCODE_MATCHMENU, "MENU", false},
        {6, 94, 118, 134, KEYCODE_SCORES, "SCORES", false},
        {6, 140, 118, 180, KEYCODE_NEXTWEAPON, "WEAPON", false},
        {6, 186, 118, 226, KEYCODE_CHAT, "CHAT", false},
    };
    constexpr int drawer_item_count = sizeof(drawer_items) / sizeof(drawer_items[0]);
    bool drawer_open = false;
    int drawer_offset = -drawer_width; // slides from -drawer_width (hidden) to 0
    bool map_overview = false;          // whole map, north up, instead of the close radar

    // Vote buttons below RELOAD while a vote waits for this player.
    const HudButton vote_buttons[] = {
        {4, 48, 158, 80, KEYCODE_VOTEYES, "VOTE YES", false},
        {162, 48, 316, 80, KEYCODE_VOTENO, "VOTE NO", false},
    };
    u64 vote_shown_tick = 0;
    const void *vote_shown_for = NULL;

    bool vote_pending()
    {
        return curvote && curvote->millis >= totalmillis && curvote->result == VOTE_NEUTRAL && !curvote->localplayervoted;
    }

    // Touches are ignored briefly after the vote buttons appear, so a tap meant
    // for the map cannot cast a vote by accident.
    bool vote_armed()
    {
        if(!vote_pending()) { vote_shown_for = NULL; return false; }
        if(vote_shown_for != curvote) { vote_shown_for = curvote; vote_shown_tick = svcGetSystemTick(); }
        return svcGetSystemTick() - vote_shown_tick >= SYSCLOCK_ARM11 / 2;
    }

    // Minimap copy from the GPU (RGB565).
    u16 *minimap_pixels = NULL;
    int minimap_w = 0, minimap_h = 0, minimap_wait = 0;
    bool minimap_request = false, minimap_valid = false;
    u16 *minimap_staging = NULL;
    int staging_w = 0, staging_h = 0;
    float minimap_x0 = 0, minimap_y0 = 0, minimap_span = 1;
    u64 radar_drawn_tick = 0;
    int pressed_hud_code = 0;

    const u16 radar_void = RGB565(1, 3, 2), hud_rvsf = RGB565(6, 26, 31), hud_cla = RGB565(31, 12, 8);
    const u16 hud_self = RGB565(31, 61, 10), hud_outline = RGB565(0, 0, 0);

    inline bool hit(const HudButton &b, const touchPosition &t)
    {
        return t.px >= b.x0 && t.px < b.x1 && t.py >= b.y0 && t.py < b.y1;
    }

    inline void plot(u16 *screen, int x, int y, u16 color)
    {
        if(x >= 0 && x < touch_width && y >= radar_top && y < radar_bottom)
            screen[x * touch_height + touch_height - 1 - y] = color;
    }

    // 50% blend toward a tint: translucent buttons keep the map visible.
    void shade_rect(u16 *screen, int x0, int y0, int x1, int y1, u16 tint)
    {
        x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, touch_width); y1 = min(y1, touch_height);
        const u16 half = (tint & 0xF7DE) >> 1;
        for(int x = x0; x < x1; ++x)
        {
            u16 *column = screen + x * touch_height;
            for(int y = y0; y < y1; ++y)
            {
                u16 &c = column[touch_height - 1 - y];
                c = u16(((c & 0xF7DE) >> 1) + half);
            }
        }
    }

    void frame_rect(u16 *screen, int x0, int y0, int x1, int y1, u16 color)
    {
        fill_rect(screen, max(x0, 0), y0, x1, y0 + 2, color);
        fill_rect(screen, max(x0, 0), y1 - 2, x1, y1, color);
        if(x0 >= 0) fill_rect(screen, x0, y0, x0 + 2, y1, color);
        fill_rect(screen, x1 - 2, y0, x1, y1, color);
    }

    void draw_dot(u16 *screen, int x, int y, int r, u16 color)
    {
        for(int j = -r - 1; j <= r + 1; ++j) for(int i = -r - 1; i <= r + 1; ++i)
        {
            const int d = i * i + j * j;
            if(d <= r * r) plot(screen, x + i, y + j, color);
            else if(d <= (r + 1) * (r + 1)) plot(screen, x + i, y + j, hud_outline);
        }
    }

    // Player arrow pointing along screen angle `a` (0 = up), outlined.
    void draw_arrow(u16 *screen, int cx, int cy, float a)
    {
        const float c = cosf(a), s = sinf(a);
        for(int pass = 0; pass < 2; ++pass)
        {
            const float grow = pass ? 0.0f : 1.6f;
            const u16 color = pass ? hud_self : hud_outline;
            for(int row = 0; row < 13; ++row)
            {
                const float half = row * 0.5f + grow;
                for(float i = -half; i <= half; i += 0.5f)
                {
                    const float lx = i, ly = row - 7.0f - grow * 0.5f; // tip at the top
                    plot(screen, cx + int(lx * c - ly * s), cy + int(lx * s + ly * c), color);
                }
            }
        }
    }

    bool hud_button_enabled(int code)
    {
        if(code == HUD_MAPTOGGLE || code == KEYCODE_MATCHMENU || code == KEYCODE_SCORES || code == KEYCODE_CHAT) return true;
        if(!player1 || player1->state != CS_ALIVE) return false;
        const weapon *gun = player1->weaponsel;
        switch(code)
        {
            case KEYCODE_RELOAD: return gun && reloadable_gun(gun->type) && gun->ammo > 0 && gun->mag < gun->info.magsize;
            case KEYCODE_GRENADE: return player1->weapons[GUN_GRENADE] && player1->weapons[GUN_GRENADE]->mag > 0;
            default: return true;
        }
    }

    bool reloading_now()
    {
        if(!player1 || player1->state != CS_ALIVE || !player1->weaponsel) return false;
        const weapon &gun = *player1->weaponsel;
        return gun.reloading && lastmillis < gun.reloading + gun.info.reloadtime;
    }

    void draw_hud_button(u16 *screen, const HudButton &b, bool enabled, bool active, int dx = 0)
    {
        const int x0 = b.x0 + dx, x1 = b.x1 + dx;
        if(x1 <= 0) return;
        shade_rect(screen, x0 + 2, b.y0 + 2, x1 - 2, b.y1 - 2, active ? accent : RGB565(2, 6, 4));
        frame_rect(screen, x0, b.y0, x1, b.y1, active ? accent : enabled ? border : surface);
        const u16 color = enabled ? text : muted;
        const char *label = b.code == HUD_MAPTOGGLE && map_overview ? "RADAR" : b.label;
        const int len = int(strlen(label));
        if(b.large)
            draw_label_large(screen, (x0 + x1 - 16 * len) / 2, (b.y0 + b.y1 - 16) / 2, label, color);
        else if(x0 >= 0)
            draw_label(screen, (x0 + x1 - 8 * len) / 2, (b.y0 + b.y1 - 8) / 2, label, color, x1);
    }

    void draw_drawer(u16 *screen)
    {
        if(drawer_offset > -drawer_width)
        {
            shade_rect(screen, 0, 0, drawer_width + drawer_offset, touch_height, RGB565(1, 4, 2));
            shade_rect(screen, 0, 0, drawer_width + drawer_offset, touch_height, RGB565(1, 4, 2));
            fill_rect(screen, max(0, drawer_width + drawer_offset - 2), 0, max(0, drawer_width + drawer_offset), touch_height, border);
            for(const HudButton &b : drawer_items)
                draw_hud_button(screen, b, hud_button_enabled(b.code), pressed_hud_code == b.code, drawer_offset);
        }
        // Tab: three bars; rides inside the drawer's top-right corner when open.
        const int tx = max(0, drawer_width + drawer_offset - 34);
        HudButton tab = drawer_tab;
        tab.x0 += tx; tab.x1 += tx;
        shade_rect(screen, tab.x0, tab.y0, tab.x1, tab.y1, RGB565(2, 6, 4));
        frame_rect(screen, tab.x0, tab.y0, tab.x1, tab.y1, drawer_open ? accent : border);
        for(int k = 0; k < 3; ++k) fill_rect(screen, tab.x0 + 8, tab.y0 + 10 + k * 7, tab.x1 - 8, tab.y0 + 13 + k * 7, text);
    }

    bool tab_hit(const touchPosition &t)
    {
        HudButton tab = drawer_tab;
        const int tx = max(0, drawer_width + drawer_offset - 34);
        tab.x0 += tx; tab.x1 += tx;
        return hit(tab, t);
    }

    void radar_probe()
    {
        static bool done = false;
        static int qa = -1;
        if(qa < 0) { struct stat st; qa = stat("sdmc:/3ds/assaultcube/qa-loadshot", &st) == 0; }
        if(done || !qa || !minimap_valid) return;
        int alive = 0;
        loopv(players) if(players[i] && players[i]->state == CS_ALIVE) ++alive;
        if(alive < 4) return;
        done = true;
        // Orientation probe: count players on lit pixels per transform.
        int hits[8] = {}, total = 0;
        auto lit = [&](int x, int y) {
            if(x < 0 || y < 0 || x >= minimap_w || y >= minimap_h) return false;
            const u16 c = minimap_pixels[y * minimap_w + x];
            return ((c >> 11) & 31) + ((c >> 5) & 63) / 2 + (c & 31) > 6; };
        auto probe = [&](const vec &o) {
            const float k = minimap_w / minimap_span;
            const int u = int((o.x - minimap_x0) * k), v = int((o.y - minimap_y0) * k), m = minimap_w - 1;
            const int cand[8][2] = {{u, v}, {m - u, v}, {u, m - v}, {m - u, m - v}, {v, u}, {m - v, u}, {v, m - u}, {m - v, m - u}};
            for(int t = 0; t < 8; ++t) if(lit(cand[t][0], cand[t][1])) ++hits[t];
            ++total; };
        loopv(players) if(players[i] && players[i]->state == CS_ALIVE) probe(players[i]->o);
        clientlogf("radar probe: %d players, hits %d %d %d %d %d %d %d %d", total, hits[0], hits[1], hits[2], hits[3], hits[4], hits[5], hits[6], hits[7]);
    }

    void draw_radar(u16 *screen)
    {
        radar_probe();
        const int cx = touch_width / 2, cy = (radar_top + radar_bottom) / 2;
        // Subject, centre and rotation follow the PC radar (renderhud.cpp).
        playerent *p = NULL, *view = NULL;
        bool spect3rd = false;
        int baseteam = 0;
        float yawdeg = 0;
        const bool radar = ac3ds_radar_view(p, view, spect3rd, baseteam, yawdeg);
        if(!radar) view = player1;
        // Close radar: centred on the player, forward up. Overview: the whole
        // map, north up, like the PC's full map.
        const bool overview = map_overview && minimap_valid;
        const float yaw = overview ? 0.0f : yawdeg * RAD, c = cosf(yaw), s = sinf(yaw);
        const float zoom = overview ? (radar_bottom - radar_top - 8) / minimap_span : 2.2f; // pixels per world unit
        vec centre = view ? view->o : vec(0, 0, 0);
        if(overview) centre = vec(minimap_x0 + minimap_span / 2, minimap_y0 + minimap_span / 2, 0);
        if(minimap_valid && view)
        {
            const float k = minimap_w / minimap_span;
            // Minimap pixel coordinates in 16.16 fixed point, stepped per pixel.
            // The transfer leaves rows bottom-up (world +y is the last row).
            const float bx = (centre.x - minimap_x0) * k, by = (centre.y - minimap_y0) * k;
            const int stepx_y = int(-s / zoom * k * 65536), stepy_y = int(c / zoom * k * 65536);
            const int maxx = minimap_w << 16, maxy = minimap_h << 16;
            for(int x = 0; x < touch_width; ++x)
            {
                const float dx = x - cx, dy = radar_top - cy;
                int mx = int((bx + (dx * c - dy * s) / zoom * k) * 65536);
                int my = int((by + (dx * s + dy * c) / zoom * k) * 65536);
                u16 *column = screen + x * touch_height + touch_height - 1 - radar_top;
                for(int y = radar_top; y < radar_bottom; ++y, --column, mx += stepx_y, my += stepy_y)
                    *column = (mx >= 0 && my >= 0 && mx < maxx && my < maxy) ?
                              minimap_pixels[(minimap_h - 1 - (my >> 16)) * minimap_w + (mx >> 16)] : radar_void;
            }
        }
        else fill_rect(screen, 0, radar_top, touch_width, radar_bottom, radar_void);
        if(!radar || !view) return; // the PC draws no radar in overview spectating

        auto toscreen = [&](const vec &o, int &sx, int &sy)
        {
            const float rx = o.x - centre.x, ry = o.y - centre.y;
            sx = cx + int(zoom * (rx * c + ry * s));
            sy = cy + int(zoom * (-rx * s + ry * c));
        };
        loopv(players)
        {
            playerent *pl = players[i];
            if(!ac3ds_radar_shows_player(p, view, spect3rd, baseteam, pl)) continue;
            int sx, sy;
            toscreen(pl->o, sx, sy);
            const u16 color = spect3rd && pl == view ? hud_self : team_base(pl->team) ? hud_rvsf : hud_cla;
            draw_dot(screen, sx, sy, 3, pl->state == CS_ALIVE ? color : RGB565(12, 24, 12));
        }
        if(m_flags_) loopi(2)
        {
            const flaginfo &f = flaginfos[i];
            const u16 color = m_ktf ? RGB565(31, 55, 0) : f.team ? hud_rvsf : hud_cla;
            const entity *e = f.flagent;
            const bool hasflagent = e && e->x != -1 && e->y != -1; // no base for HTF dummies
            if(hasflagent)
            {
                int sx, sy;
                toscreen(vec(e->x, e->y, 0), sx, sy);
                for(int j = -4; j <= 4; ++j) for(int k2 = -4; k2 <= 4; ++k2)
                    if(abs(j) == 4 || abs(k2) == 4) plot(screen, sx + k2, sy + j, color);
            }
            if((f.state == CTFF_INBASE && hasflagent) || f.state == CTFF_DROPPED)
            {
                int sx, sy;
                toscreen(f.pos, sx, sy);
                draw_dot(screen, sx, sy, 2, color);
            }
            if(ac3ds_radar_shows_thief(p, baseteam, i))
            {
                // Flag carrier marker: a pulsing ring, as the PC radar's
                // blinking flag icon near the thief.
                int sx, sy;
                toscreen(f.actor->o, sx, sy);
                const int r = 5 + (lastmillis / 150) % 2;
                for(int a = 0; a < 32; ++a)
                {
                    const float t = a * (2 * PI / 32);
                    plot(screen, sx + int(r * cosf(t)), sy + int(r * sinf(t)), color);
                }
            }
        }
        if(!(p->isspectating() && spect3rd))
        {
            int sx = cx, sy = cy;
            if(overview) toscreen(view->o, sx, sy);
            draw_arrow(screen, sx, sy, overview ? yawdeg * RAD : 0.0f);
        }
    }

    // The whole match screen: map, then the overlays.
    void draw_match_screen(u16 *screen)
    {
        draw_radar(screen);
        for(const HudButton &b : hud_buttons)
            draw_hud_button(screen, b, hud_button_enabled(b.code),
                            pressed_hud_code == b.code || (b.code == KEYCODE_RELOAD && reloading_now()));
        if(vote_pending())
        {
            const bool armed = vote_armed();
            for(const HudButton &b : vote_buttons) draw_hud_button(screen, b, armed, false);
        }
        draw_drawer(screen);
    }

    // Advances the drawer slide; true while it is still moving.
    bool animate_drawer()
    {
        const int target = drawer_open ? 0 : -drawer_width;
        if(drawer_offset == target) return false;
        const int step = 31; // about 4 frames end to end
        drawer_offset = drawer_offset < target ? min(target, drawer_offset + step) : max(target, drawer_offset - step);
        return true;
    }

    // Handles a new touch on the match screen. Returns the key code to press
    // (0 if none) and sets aim when the touch should steer the view.
    int match_touch(const touchPosition &t, bool &aim)
    {
        aim = false;
        if(drawer_open)
        {
            if(tab_hit(t)) { drawer_open = false; return 0; }
            if(t.px < drawer_width)
            {
                for(const HudButton &b : drawer_items)
                    if(hit(b, t) && hud_button_enabled(b.code)) return b.code;
                return 0;
            }
            drawer_open = false; // a tap outside closes it
            return 0;
        }
        if(tab_hit(t)) { drawer_open = true; return 0; }
        if(vote_armed()) for(const HudButton &b : vote_buttons) if(hit(b, t)) return b.code;
        for(const HudButton &b : hud_buttons)
        {
            if(!hit(b, t)) continue;
            if(b.code == HUD_MAPTOGGLE) { map_overview = !map_overview; return 0; }
            return hud_button_enabled(b.code) ? b.code : 0;
        }
        aim = true;
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Server browser, lower screen: the selected server as a card, a big JOIN,
// large scroll arrows (repeat while held) and the few actions players use.
namespace
{
    struct BrowserButton { int x0, y0, x1, y1, code; const char *label; bool large; };
    enum { BROWSER_UP_ARROW = 1, BROWSER_DOWN_ARROW = 2 };
    const BrowserButton browser_buttons[] = {
        {4, 106, 316, 148, SDLK_RETURN, "JOIN", true},
        {4, 152, 106, 192, SDLK_UP, "", false},
        {110, 152, 212, 192, SDLK_DOWN, "", false},
        {216, 152, 316, 192, SDLK_F5, "SCAN", true},
        {4, 196, 106, 236, SDLK_F7, "FILTER", true},
        {110, 196, 212, 236, SDLK_RIGHT, "SORT", true},
        {216, 196, 316, 236, SDLK_ESCAPE, "BACK", true},
    };
    Uint32 browser_repeat_at = 0;

    void draw_arrow_glyph(u16 *screen, int cx, int cy, bool up, u16 color)
    {
        for(int row = 0; row < 12; ++row)
        {
            const int half = row;
            const int y = up ? cy - 6 + row : cy + 5 - row;
            fill_rect(screen, cx - half, y, cx + half + 1, y + 1, color);
        }
    }

    void draw_server_panel(u16 *screen)
    {
        static Ac3dsServerRow rows[256];
        int selected = -1;
        const int count = ac3ds_server_rows(const_cast<void *>(ac3ds_menu_identity()), rows, 256, selected);
        fill_rect(screen, 0, 0, touch_width, 28, surface);
        fill_rect(screen, 0, 27, touch_width, 28, border);
        draw_label(screen, 10, 10, "Server browser", text);
        char total[24];
        snprintf(total, sizeof(total), "%d servers", count);
        draw_label(screen, touch_width - 10 - 8 * int(strlen(total)), 10, total, muted);
        // Card: the selected server in the large font.
        fill_rect(screen, 4, 32, 316, 102, surface);
        fill_rect(screen, 4, 32, 7, 102, accent);
        if(selected >= 0)
        {
            const Ac3dsServerRow &r = rows[selected];
            char line[128];
            const int len = int(strlen(r.desc));
            if(len > 18) snprintf(line, sizeof(line), "%.16s..", r.desc);
            else snprintf(line, sizeof(line), "%s", r.desc);
            draw_label_large(screen, 12, 36, line, text);
            if(r.map[0]) snprintf(line, sizeof(line), "%.12s %s", r.map, r.mode);
            else snprintf(line, sizeof(line), "%s", r.responded ? "Empty" : "Waiting...");
            draw_label_large(screen, 12, 58, line, accent);
            if(r.responded)
            {
                snprintf(line, sizeof(line), "%d/%d  %dms%s", r.players, r.maxclients, r.ping, r.password ? " *" : "");
                draw_label_large(screen, 12, 80, line, r.full ? RGB565(31, 30, 12) : muted);
            }
        }
        else draw_label_large(screen, 12, 58, count ? "Pick a server" : "Tap SCAN", muted);
        for(const BrowserButton &b : browser_buttons)
        {
            const bool active = touch_key == b.code;
            fill_rect(screen, b.x0, b.y0, b.x1, b.y1, active ? accent : border);
            fill_rect(screen, b.x0 + 2, b.y0 + 2, b.x1 - 2, b.y1 - 2, active ? RGB565(8, 22, 10) : surface);
            if(b.code == SDLK_UP || b.code == SDLK_DOWN)
                draw_arrow_glyph(screen, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, b.code == SDLK_UP, text);
            else if(b.large)
                draw_label_large(screen, (b.x0 + b.x1 - 16 * int(strlen(b.label))) / 2, (b.y0 + b.y1 - 16) / 2, b.label,
                                 b.code == SDLK_RETURN && selected < 0 ? muted : text); // only JOIN needs a server
            else
                draw_label(screen, (b.x0 + b.x1 - 8 * int(strlen(b.label))) / 2, (b.y0 + b.y1 - 8) / 2, b.label, text, b.x1);
        }
    }

    // Change key for redraws: the selection and what the card shows.
    void server_panel_status(char *out, size_t size)
    {
        static Ac3dsServerRow rows[256];
        int selected = -1;
        const int count = ac3ds_server_rows(const_cast<void *>(ac3ds_menu_identity()), rows, 256, selected);
        if(selected >= 0)
        {
            const Ac3dsServerRow &r = rows[selected];
            snprintf(out, size, "S%d|%d|%s|%s|%d|%d|%d|%d", count, selected, r.desc, r.map, r.players, r.ping / 10, r.responded, touch_key);
        }
        else snprintf(out, size, "S%d|-|%d", count, touch_key);
    }

    int server_touch(const touchPosition &t)
    {
        for(const BrowserButton &b : browser_buttons)
            if(t.px >= b.x0 && t.px < b.x1 && t.py >= b.y0 && t.py < b.y1) return b.code;
        return 0;
    }
}

// QA: a tap at (x, y) on the match screen or server browser, through the
// same handlers as a real touch.
void ac3ds_qa_tap(int x, int y)
{
    touchPosition t;
    t.px = u16(max(0, min(x, touch_width - 1)));
    t.py = u16(max(0, min(y, touch_height - 1)));
    bool aim = false;
    const int code = current_panel() == SERVER_PANEL ? server_touch(t) : match_touch(t, aim);
    if(code)
    {
        push_key(code, true, false, button_context);
        push_key(code, false, false, button_context);
        if(drawer_open && code != KEYCODE_SCORES) drawer_open = false;
    }
    controls_dirty = true;
}

// QA: `presses` stylus taps at (x, y), each held hold_ms with gap_ms between,
// through the real touch path. Logs the double-tap jumps and how far the player rose.
void ac3ds_qa_touch(int x, int y, int presses, int hold_ms, int gap_ms)
{
    qa_touch = {};
    qa_touch.x = max(0, min(x, touch_width - 1));
    qa_touch.y = max(0, min(y, touch_height - 1));
    qa_touch.presses = max(1, presses);
    qa_touch.hold = max(1, hold_ms);
    qa_touch.gap = max(1, gap_ms);
    qa_touch.start = SDL_GetTicks();
    qa_touch.base_z = qa_touch.peak_z = player1 ? player1->o.z : 0;
    qa_touch.active = true;
}

bool ac3ds_minimap_info(unsigned &tex, float &x0, float &y0, float &span);

void ac3ds_minimap_updated()
{
    minimap_request = true;
    minimap_valid = false;
}

namespace
{
    void service_minimap()
    {
        if(minimap_request)
        {
            minimap_request = false;
            unsigned tex;
            if(ac3ds_minimap_info(tex, minimap_x0, minimap_y0, minimap_span) &&
               ac3ds_gl_queue_readback_565(tex, &minimap_staging, &staging_w, &staging_h))
                minimap_wait = 3; // read after the queued copy has run
        }
        else if(minimap_wait && !--minimap_wait)
        {
            const size_t bytes = size_t(staging_w) * staging_h * sizeof(u16);
            if(R_SUCCEEDED(GSPGPU_InvalidateDataCache(minimap_staging, bytes)))
            {
                u16 *copy = static_cast<u16 *>(realloc(minimap_pixels, bytes));
                if(copy)
                {
                    minimap_pixels = copy;
                    memcpy(minimap_pixels, minimap_staging, bytes);
                    minimap_w = staging_w;
                    minimap_h = staging_h;
                    minimap_valid = true;
                    struct stat st;
                    if(stat("sdmc:/3ds/assaultcube/qa-loadshot", &st) == 0)
                    {
                        // QA: raw RGB565 minimap dump
                        if(FILE *f = fopen("sdmc:/3ds/assaultcube/profile/minimap.raw", "wb"))
                        {
                            fwrite(minimap_pixels, 1, bytes, f);
                            fclose(f);
                        }
                        clientlogf("radar: minimap %dx%d covers %.1f,%.1f span %.1f", minimap_w, minimap_h, minimap_x0, minimap_y0, minimap_span);
                    }
                }
            }
        }
    }
}

bool ac3ds_accept_button_event(const SDL_KeyboardEvent &event)
{
    if(event.keysym.sym < KEYCODE_A || event.keysym.sym > KEYCODE_LAST || !event.windowID) return true;
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
    bool qa_pressed = false;
    if(qa_touch.active)
    {
        const int elapsed = int(SDL_GetTicks() - qa_touch.start);
        const int period = qa_touch.hold + qa_touch.gap;
        qa_pressed = elapsed / period < qa_touch.presses && elapsed % period < qa_touch.hold;
        if(player1) qa_touch.peak_z = max(qa_touch.peak_z, player1->o.z);
        if(elapsed >= qa_touch.presses * period + 800)
        {
            clientlogf("qa: touch sequence done: %d taps, %d double-tap jumps, rose %.2f (state %d, on floor %d)",
                       qa_touch.presses, qa_touch.jumps, qa_touch.peak_z - qa_touch.base_z,
                       player1 ? player1->state : -1, player1 ? int(player1->onfloor) : -1);
            qa_touch.active = false;
        }
    }
    if(qa_pressed) held |= KEY_TOUCH;
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
        if(qa_pressed)
        {
            touch.px = u16(qa_touch.x);
            touch.py = u16(qa_touch.y);
        }
        if(!touch_held)
        {
            touch_aim = touchaim && touch.py >= 33 && touch.py < panel_top && can_aim && drawn_panel == panel;
            if(panel == GAME_PANEL && drawn_panel == panel)
            {
                bool aim = false;
                touch_key = match_touch(touch, aim);
                touch_aim = aim && touchaim;
                controls_dirty = true;
                if(touch_key)
                {
                    touch_key_context = button_context;
                    pressed_hud_code = touch_key;
                    push_key(touch_key, true, false, touch_key_context);
                }
                else if(aim && doubletapjump && double_tap.down(now, touch.px, touch.py, Uint32(doubletapms)))
                {
                    // Held until the stylus lifts, like the A button.
                    touch_key = KEYCODE_JUMP;
                    touch_key_context = button_context;
                    push_key(touch_key, true, false, touch_key_context);
                    if(qa_touch.active) ++qa_touch.jumps;
                }
                if(!aim || !doubletapjump) double_tap.reset();
            }
            else if(drawn_panel == panel && panel == MENU_PANEL) queue_menu_touch(touch);
            else if(drawn_panel == panel && panel == SERVER_PANEL)
            {
                touch_key = server_touch(touch);
                if(touch_key)
                {
                    touch_key_context = button_context;
                    push_key(touch_key, true, false, touch_key_context);
                    browser_repeat_at = now + 340;
                    controls_dirty = true;
                }
            }
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
        else if(panel == SERVER_PANEL && (touch_key == SDLK_UP || touch_key == SDLK_DOWN) &&
                (Sint32)(now - browser_repeat_at) >= 0)
        {
            // Scroll arrows repeat while held.
            push_key(touch_key, true, true, touch_key_context);
            browser_repeat_at = now + 90;
        }
        else if(touch_aim && touchaim && can_aim)
        {
            dx = (int(touch.px) - int(last_touch.px)) * touchsensitivity;
            dy = (int(touch.py) - int(last_touch.py)) * touchsensitivity;
        }
        if(touch_held) double_tap.move(touch.px, touch.py);
        last_touch = touch;
        touch_held = true;
    }
    else if(touch_held)
    {
        double_tap.up(now);
        if(touch_key && touch_key_context == button_context) push_key(touch_key, false, false, touch_key_context);
        // One-shot drawer actions close the drawer once used (SCORES is held).
        if(drawer_open && touch_key && touch_key != KEYCODE_SCORES) drawer_open = false;
        if(pressed_hud_code) { pressed_hud_code = 0; controls_dirty = true; }
        touch_key = 0;
        touch_held = touch_aim = false;
    }
    else if(pressed_hud_code)
    {
        // Released while the panel changed (or input was reset): clear the highlight.
        pressed_hud_code = 0;
        controls_dirty = true;
    }
    if(panel != GAME_PANEL)
    {
        drawer_open = false;
        double_tap.reset();
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

void ac3ds_service_controls()
{
    service_minimap();
}

void ac3ds_draw_controls()
{
    if(loading_percent >= 0) return; // the loading screen owns the lower screen
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

    if(panel == SERVER_PANEL) server_panel_status(status, sizeof(status));
    // Button enable states are part of the match screen's look.
    if(panel == GAME_PANEL)
    {
        concatstring(status, "|");
        for(const HudButton &b : hud_buttons)
            concatstring(status, hud_button_enabled(b.code) ? (pressed_hud_code == b.code ? "P" : "1") : "0");
        concatstring(status, reloading_now() ? "R" : "");
        concatstring(status, vote_pending() ? (vote_armed() ? "V" : "v") : "");
        concatstring(status, map_overview ? "M" : "");
        defformatstring(drawer)("D%d%d", drawer_open ? 1 : 0, pressed_hud_code);
        concatstring(status, drawer);
    }
    const bool unchanged = !controls_dirty && drawn_panel == panel && drawn_bind_type == bind_type &&
       drawn_touch_aim == touchaim && !strcmp(status, drawn_status) &&
       (panel != MENU_PANEL || same_menu_view(menu, drawn_menu));
    const u64 now_tick = svcGetSystemTick();
    if(unchanged)
    {
        // The map moves: refresh it ten times a second, every frame while the
        // drawer slides.
        if(panel != GAME_PANEL) return;
        const bool sliding = animate_drawer();
        if(!sliding && now_tick - radar_drawn_tick < SYSCLOCK_ARM11 / 10) return;
        u16 *screen = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
        if(!screen) return;
        draw_match_screen(screen);
        radar_drawn_tick = now_tick;
        GSPGPU_FlushDataCache(screen, touch_width * touch_height * sizeof(*screen));
        return;
    }
    u16 *screen = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    if(!screen) return;
    fill_rect(screen, 0, 0, touch_width, touch_height, background);

    if(panel == MENU_PANEL) draw_handheld_menu(screen, menu);
    else if(panel == GAME_PANEL)
    {
        animate_drawer();
        draw_match_screen(screen);
        radar_drawn_tick = now_tick;
    }
    else if(panel == SERVER_PANEL) draw_server_panel(screen);
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


void ac3ds_loading_screen(const char *title, const char *stage, float progress)
{
    // Software-drawn on the lower screen, so it keeps moving while the GPU
    // and the main thread are busy loading.
    if(dedicated_display) return;
    if(!title && !loading_title[0]) return; // progress outside a load (e.g. a model loaded mid-match)
    const bool newtitle = title && strcmp(title, loading_title);
    if(newtitle && strcmp(title, "Starting up") && strcmp(title, "Applying settings"))
        minimap_valid = false; // a new map: the old minimap no longer matches
    int percent = max(0, min(100, int(progress * 100 + 0.5f)));
    if(!newtitle && percent < loading_percent) percent = loading_percent; // never moves backwards
    const bool changed = newtitle || (stage && strcmp(stage, loading_stage));
    const u64 now = svcGetSystemTick();
    if(!changed && percent == loading_percent) return;
    if(!changed && now - loading_drawn_tick < SYSCLOCK_ARM11 / 30) return; // at most 30 redraws a second
    if(title) copystring(loading_title, title, sizeof(loading_title));
    if(stage) copystring(loading_stage, stage, sizeof(loading_stage));
    loading_percent = percent;
    loading_drawn_tick = now;
    u16 *screen = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    if(!screen) return;
    fill_rect(screen, 0, 0, touch_width, touch_height, background);
    fill_rect(screen, 0, 0, touch_width, 34, surface);
    fill_rect(screen, 0, 34, touch_width, 36, accent);
    draw_label_large(screen, 12, 9, "ASSAULTCUBE", text);
    draw_label(screen, 12 + 11 * 16 + 6, 17, "3DS", accent);
    draw_label_large(screen, 16, 70, loading_title, text);
    draw_label(screen, 16, 104, loading_stage, muted, touch_width - 16);
    // Progress bar
    const int x0 = 16, x1 = touch_width - 16, y0 = 124, y1 = 140;
    fill_rect(screen, x0, y0, x1, y1, border);
    fill_rect(screen, x0 + 2, y0 + 2, x1 - 2, y1 - 2, background);
    fill_rect(screen, x0 + 2, y0 + 2, x0 + 2 + (x1 - x0 - 4) * percent / 100, y1 - 2, accent);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    draw_label(screen, x1 - 8 * int(strlen(pct)), 148, pct, muted);
    draw_label(screen, 16, 212, "The first load of a map takes longest.", muted);
    GSPGPU_FlushDataCache(screen, touch_width * touch_height * sizeof(*screen));
    // QA: capture one loading frame when sdmc:/3ds/assaultcube/qa-loadshot exists.
    static int qaload = -1;
    if(qaload < 0) { struct stat st; qaload = stat("sdmc:/3ds/assaultcube/qa-loadshot", &st) == 0; }
    static bool captured = false;
    if(qaload && !captured && percent >= 40 && strcmp(loading_title, "Starting up"))
    {
        captured = true;
        ac3ds_save_bottom_screen("sdmc:/3ds/assaultcube/profile/loading.bmp");
    }
}

bool ac3ds_save_bottom_screen(const char *path)
{
    // The lower framebuffer is RGB565, stored as 320 columns of 240 pixels
    // bottom-to-top. Writes a 24-bit BMP for QA captures.
    const u16 *screen = reinterpret_cast<const u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    FILE *f = screen ? fopen(path, "wb") : NULL;
    if(!f) return false;
    const int w = touch_width, h = touch_height, row = w * 3, size = 54 + row * h;
    u8 header[54] = { 'B', 'M' };
    auto put32 = [&](int at, u32 v) { for(int i = 0; i < 4; ++i) header[at + i] = u8(v >> (8 * i)); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    header[26] = 1; header[28] = 24; put32(34, row * h);
    fwrite(header, 1, sizeof(header), f);
    static u8 line[touch_width * 3];
    for(int y = h - 1; y >= 0; --y) // BMP rows run bottom-up
    {
        for(int x = 0; x < w; ++x)
        {
            const u16 c = screen[x * touch_height + touch_height - 1 - y];
            const u8 r = u8(((c >> 11) & 31) * 255 / 31), g = u8(((c >> 5) & 63) * 255 / 63), b = u8((c & 31) * 255 / 31);
            line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
    return true;
}

void ac3ds_loading_done()
{
    loading_title[0] = loading_stage[0] = '\0';
    loading_percent = -1;
    controls_dirty = true;
}

void ac3ds_show_startup_report(const char *const *lines, int count)
{
    // Software-drawn, so it stays readable when the GPU path has stopped.
    u16 *screen = reinterpret_cast<u16 *>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL));
    if(!screen) return;
    fill_rect(screen, 0, 0, touch_width, touch_height, background);
    fill_rect(screen, 0, 0, touch_width, 29, surface);
    fill_rect(screen, 0, 28, touch_width, 29, border);
    draw_label(screen, 10, 10, "AssaultCube startup check", text);
    for(int i = 0; i < count && i < 16; ++i)
        draw_label(screen, 6, 38 + i * 12, lines[i], i ? muted : accent);
    GSPGPU_FlushDataCache(screen, touch_width * touch_height * sizeof(*screen));
    controls_dirty = true;
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
    defformatstring(path)("%s.cfg", basename);

    // Settings are saved on every HOME press, lid close and exit. Rotating the
    // five backups and rewriting the file costs about twenty SD directory
    // operations per file, so skip all of it when the text is unchanged. The
    // first save of a session compares against the file on the card.
    struct Written { const char *name; bool known; long size; uLong crc; };
    static Written written[] = { { "config/init", false, 0, 0 }, { "config/saved", false, 0, 0 } };
    Written *slot = NULL;
    for(Written &w : written) if(!strcmp(w.name, basename)) slot = &w;
    uchar chunk[1024];
    uLong crc = crc32(0, Z_NULL, 0);
    if(slot && config->seek(0, SEEK_SET))
    {
        for(int n; (n = config->read(chunk, sizeof(chunk))) > 0;) crc = crc32(crc, chunk, n);
        if(!slot->known)
        {
            if(FILE *current = fopen(findfile(path, "wb"), "rb"))
            {
                uLong disk = crc32(0, Z_NULL, 0);
                long length = 0;
                for(size_t n; (n = fread(chunk, 1, sizeof(chunk), current)) > 0; length += long(n))
                    disk = crc32(disk, chunk, uInt(n));
                const bool complete = !ferror(current);
                fclose(current);
                if(complete) { slot->known = true; slot->size = length; slot->crc = disk; }
            }
        }
        if(slot->known && slot->size == size && slot->crc == crc) return true;
    }

    filerotate(basename, "cfg", CONFIGROTATEMAX, NULL, true);
    const bool installed = installfile3ds(path, config, max_config_size);
    if(slot) { slot->known = installed; slot->size = size; slot->crc = crc; }
    return installed;
}

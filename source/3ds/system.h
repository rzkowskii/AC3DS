#pragma once

#include <stddef.h>

enum
{
    AC3DS_WIDTH = 400,
    AC3DS_HEIGHT = 240
};

enum class Ac3dsMenu { None, Standard, ServerBrowser, ServerInfo };

enum class Ac3dsMenuItem { Empty, ReadOnly, Action, Slider, Toggle, Text, Binding };
enum class Ac3dsMenuTouch { Activate, Decrease, Increase, Previous, Next, Back };

struct Ac3dsMenuRow
{
    int index = -1;
    Ac3dsMenuItem kind = Ac3dsMenuItem::Empty;
    bool disabled = false;
    char label[96] = {}, value[64] = {};
};

struct Ac3dsMenuView
{
    static constexpr int row_count = 6;
    unsigned revision = 0;
    int page = 0, pages = 0, selection = -1;
    bool binding = false, back = false;
    char title[64] = {};
    Ac3dsMenuRow rows[row_count];
};

struct SDL_KeyboardEvent;
constexpr int AC3DS_MENU_TOUCH_EVENT = 0x4143;

const char *ac3ds_system_init();
void ac3ds_system_shutdown();
void ac3ds_show_error(const char *message);
bool ac3ds_main_loop();
bool ac3ds_take_clock_reset();
bool ac3ds_start_network();
void ac3ds_read_launch_arguments(int &argc, char **&argv);
void ac3ds_poll_input();
void ac3ds_draw_controls();
void ac3ds_draw_server_controls();
void ac3ds_controls_changed();
Ac3dsMenu ac3ds_menu_panel();
const void *ac3ds_menu_identity();
void ac3ds_menu_view(Ac3dsMenuView &view);
void ac3ds_menu_touch(unsigned revision, int index, Ac3dsMenuTouch action);
void ac3ds_install_handheld_menus();
void ac3ds_install_keymap();
void ac3ds_default_binds();
bool ac3ds_is_physical_button(int code);
bool ac3ds_accept_button_event(const SDL_KeyboardEvent &event);
bool ac3ds_gameplay_input_active();
void ac3ds_sync_input_context();
int ac3ds_menu_key(int code);
int ac3ds_console_key(int code);
bool ac3ds_keyboard_key(int code);
bool ac3ds_binding_cancel_key(int code);
bool ac3ds_edit_text(const char *title, const char *initial, char *out, size_t capacity,
                    int maxchars, bool password = false);
int ac3ds_set_brightness(int percent);

struct stream;
stream *ac3ds_config_buffer();
bool ac3ds_install_config(const char *basename, stream *config);

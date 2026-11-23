#pragma once

// One server row of the handheld server browser (see serverbrowser.cpp).
struct Ac3dsServerRow
{
    int item;                    // the menu line it starts on
    unsigned id;                // stable across sorting, assigned when the server is added
    char desc[96];               // description without color codes
    char map[40], mode[16], address[64];
    int players, maxclients, ping, minremain;
    bool responded, compatible, full, password, banned, connected;
};

struct Ac3dsServerList
{
    int count = 0, first = 0, selected = -1;
};

// Both screens use the same list. Format only the requested window of rows,
// centred on the preferred server if listed, or the current selection.
Ac3dsServerList ac3ds_server_window(const void *menu, Ac3dsServerRow *rows, int capacity, unsigned preferred = 0);
void ac3ds_join_server_from_menu(unsigned id);
int ac3ds_server_total();
const char *ac3ds_server_sort_name();

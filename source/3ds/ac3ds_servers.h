#pragma once

// One server row of the handheld server browser (see serverbrowser.cpp).
struct Ac3dsServerRow
{
    int item;                    // the menu line it starts on
    char desc[96];               // description without color codes
    char map[40], mode[16], address[64];
    int players, maxclients, ping, minremain;
    bool responded, compatible, full, password, banned, connected;
};

// Fills rows (up to max) with what the browser menu lists, in order, and sets
// selected to the row holding the menu selection (or -1).
int ac3ds_server_rows(void *menu, Ac3dsServerRow *rows, int max, int &selected);
int ac3ds_server_total();
const char *ac3ds_server_sort_name();

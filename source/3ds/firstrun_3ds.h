#pragma once

// First-run download of the game data. An engine-only build that finds no
// game data offers to fetch the official AssaultCube 1.3.0.2 package, checks
// it and unpacks the folders the game uses into one file on the SD card, the
// RomFS image of romfs_image.h. See docs/first-run-download.md.

enum Ac3dsFirstRun { AC3DS_FIRSTRUN_INSTALLED, AC3DS_FIRSTRUN_QUIT };

// Runs the offer and, if accepted, the download. Needs the display; nothing
// else of the game has to be running, and the image must not be mounted.
// damaged: why an image already on the card was refused, NULL when there was
// none. AC3DS_FIRSTRUN_QUIT: the player declined, or it failed and the player
// has been told why.
Ac3dsFirstRun ac3ds_first_run(const char *game_dir, const char *damaged);
// Removes the file a download that did not finish leaves behind.
void ac3ds_first_run_tidy(const char *game_dir);

// Provided by system.cpp: a page of text on the lower screen, drawn with the
// console font. 39 columns a line.
struct Ac3dsTextPage
{
    static constexpr int max_lines = 14;
    const char *title = "";
    const char *lines[max_lines] = {};
    int count = 0;
    unsigned bright = 0;      // bit i: line i in the brighter colour
    int percent = -1;         // >= 0: a progress bar below the twelfth line
    const char *footer = NULL;
};
void ac3ds_text_page(const Ac3dsTextPage &page);
void ac3ds_blank_top_screen();

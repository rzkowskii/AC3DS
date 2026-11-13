# New Nintendo 3DS and New Nintendo 2DS XL controls

The top screen shows the match, menu details and map previews. The touch screen shows the active menu or match controls. The server browser keeps its server table on top and its filter, refresh and detail buttons below.

| Control | In a match | In the map editor |
|---|---|---|
| Circle Pad | Move and strafe | Move and strafe |
| C-Stick or drag the upper touch area | Aim | Look and position the selection cursor |
| A / B | Jump / knife attack | Fly up / down |
| L | Fire | Hold and drag to select |
| R | Scope with the sniper rifle, special action with other weapons | Editing tools |
| ZL / ZR | Quick grenade / crouch | Edit modifier / secondary edit modifier |
| X / Y | Reload / previous weapon | Copy / paste |
| D-Pad Up / Down | Primary / secondary weapon | Raise / lower floor |
| D-Pad Left / Right | Previous / next weapon | Decrease / increase the active editing value |
| START | Open the match menu | Open the match menu |
| SELECT | Hold for scores | Editing tools |

During a demo, D-Pad Up and Down adjust playback speed. In spectator mode, weapon cycling follows the original spectating controls.

During play, the lower screen shows the selected weapon and ammunition and provides touch shortcuts for chat, weapons, the map, voting, screenshots and other match actions. Drag in the marked area to aim. The editor replaces match shortcuts with editing tools, undo, redo, previous and next values, deleting an entity, and exiting edit mode. Offline matches pause while the START menu is open. Choose **Resume match** or press B or START to close it. Online matches use the server's normal pause rules.

Tap a menu row to select it, or use the D-Pad or Circle Pad and press A. Adjust sliders with Left/Right or the on-screen −/+ buttons. Use L/R, ZL/ZR or the touch footer to change pages. B or START goes back. X opens the keyboard on a text field or refreshes the current server view. Y saves a screenshot. In the server browser, hold SELECT for the ten favorite group shortcuts and use D-Pad Left/Right to change the sort column.

Public chat, team chat, and the command prompt open the Nintendo software keyboard with the current text. Cancel returns to the command line so you can inspect history or edit the draft. X reopens the keyboard, A submits, and B or START cancels. The touch panel also provides history, cursor, completion, help, delete, submit, and cancel controls. For a menu text field, A or X opens the keyboard.

Open **Handheld controls** from the main menu or offline match menu to tune aim and remap common actions. **Button bindings → All binding menus** opens the full set. Select an action and press and release the new button. START cancels capture. You can also remap in `config/autoexec.cfg` inside the SD profile. The key names use the `3DS_` prefix, for example `bind 3DS_ZR reload`, `editbind 3DS_CPAD_UP forward`, or `specbind 3DS_UP speckey_demo_gamespeedup`. The full set is:

```text
3DS_A 3DS_B 3DS_X 3DS_Y 3DS_L 3DS_R 3DS_ZL 3DS_ZR
3DS_START 3DS_SELECT 3DS_UP 3DS_DOWN 3DS_LEFT 3DS_RIGHT
3DS_CPAD_UP 3DS_CPAD_DOWN 3DS_CPAD_LEFT 3DS_CPAD_RIGHT
3DS_TOUCH_CHAT 3DS_TOUCH_TEAM 3DS_TOUCH_CONSOLE 3DS_TOUCH_EDIT
3DS_TOUCH_VOICE 3DS_TOUCH_MAP 3DS_TOUCH_YES 3DS_TOUCH_NO
3DS_TOUCH_FLAG 3DS_TOUCH_SCREENSHOT 3DS_TOUCH_WEAPONS
3DS_TOUCH_NEXT 3DS_TOUCH_PREV 3DS_TOUCH_UNDO 3DS_TOUCH_REDO
3DS_TOUCH_DELETE 3DS_TOUCH_EDITMENU
```

`csticksensitivity` adjusts C-Stick turn speed. `cstickysensitivity` adjusts its vertical speed relative to horizontal speed, and `cstickdeadzone` sets how much movement the stick needs before it turns the camera. `touchaim` enables touchscreen aiming, and `touchsensitivity` adjusts how far the view turns as you drag. The original sensitivity, scope sensitivity, and invert settings also apply. Settings and screenshots are stored under `sdmc:/3ds/assaultcube/profile` by default.

The Circle Pad maps to AssaultCube's four movement commands, with diagonal movement when two are held. Explicit bindings in an existing profile take priority over the new defaults.

# Lumiose Online for ZA Recomp â€” v0.1.38 (Windows)

## Title screen crash fix

A game lookup can return no entry just after the title screen. The launcher now handles that case and prepares updated game code on the first start after upgrading.

## Install or update

- New installation: extract the complete **ZA Recomp** folder into a writable
  location and open **Lumiose Online.exe**. Supply your own Pokemon Legends:
  Z-A game files, update 2.0.2 and DLC. A 2.0.2 `.nup` works too and needs no
  keys; add the DLC 2 `.nsp` with your `prod.keys` / `title.keys` if you want the
  DLC. Normal bundled mod setup is included.
- Existing installation: close the game and use **Update now**, or copy the
  complete archive over the existing installation, merging folders. Keep your
  saves, settings, locally generated game code and installed mod data.
- Keep the preparation window open until any required compatibility preparation
  finishes. Saved customized bindings remain; Restore default controls in
  Settings > Controls adopts the documented layout.

## Upgrade repair

This update forces game-code preparation before the mod is refreshed, fixing
upgrades where the new runtime could start the rebuilt vanilla game without
the matching prepared mod. Keep the preparation window open until both steps
finish. Existing saves, settings and installed mod versions are retained.

## Bundled mod and future updates

This full Windows archive includes the normal mod,
**1.5.8 build 8**, and launcher support for future server-delivered mod updates.
The bundled mod SHA-256 is:
`ccf73f151ec203f39cb0f2e49828d046aa699d83273b3c7edb4e9a5f2a3b5786`.

Server-delivered raw mod builds are inspected locally to derive their supported
ABI, camera query locations and switch-table targets. Build IDs identify cached
versions and do not require registration. Server-supplied compatibility metadata
is optional. The original update transport, public server connection and normal
PC controls remain unchanged. The launcher verifies preparation before activating
a version and keeps startup output from the current attempt, including the
runtime log. Earlier attempts cannot supply stale success markers.

## Native PC controls

WASD movement; mouse camera; E interact; Shift sprint; Space dodge; Ctrl crouch.
Tab toggles a valid lock-on and releases when its target is lost. Hold RMB to
aim a Poke Ball, LMB to throw, and release RMB to put the ball away. Wheel cycles
balls while aiming, or party members otherwise. Middle-click sends/switches or
recalls the selected Pokemon. Moves use 1/2/3/4; Q is Mega Evolution; R is Plus.

Esc opens the main menu or goes back. I opens Satchel, B Boxes, P Pokedex,
O Mable's Research, and M the map. Map: wheel zoom, WASD pan, E interact.
Satchel tabs: A/D. Menu up/down: W/S. Hover and click to select in menus.
Drag Pokemon between Box/party slots, and eligible items onto party members.
Shop and clothing clicks remain tied to the displayed item after scrolling or
changing tabs. Click outside a context menu to close it.

Prompts follow keyboard/mouse, Xbox or generic-controller input and configured
bindings. Mouse menus remain available with a controller connected. See
**docs/PC-CONTROLS.md** for the complete guide.

## Support

Please report bugs in the **LumioseOnline Discord**, including the affected
screen, the action and the log file. This release contains the accepted controls
changes; the separate graphics work is not included. Game ROMs, extracted game
assets, generated game libraries, saves, settings and caches are not distributed.

With a controller connected the cursor stays visible: click mod icons and menus directly. Keyboard-only players can hold Left Alt for overworld UI.

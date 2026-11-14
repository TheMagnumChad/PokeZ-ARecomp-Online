# v0.1.33 — Controller mouse UI hotfix

- Keeps the mouse cursor visible and clickable while a controller is connected, including after controller buttons, sticks, or keyboard input.
- Restores clicking LumioseOnline overworld icons and mod menus with a controller connected. Alt is no longer required in controller mode.
- Preserves keyboard-only mouse camera controls and Left Alt cursor release. Mouse camera motion remains disabled while a controller is connected.
- Includes the current launcher, every accepted 0.1.32 controls change, setup tools and dependencies. The separate graphics work is not included.
- Includes the existing source fix connecting the SDK keyboard-font selector. The first launch refreshes game code and control glyphs once; saves and customized bindings remain.

The full ZA-Recomp-windows.zip supports both new installations and existing players. Existing players can use Update now. Extract the complete folder for a new installation and open Lumiose Online.exe with your own game files. Linux is unchanged; no new Linux binary is provided in this release.

Focused CPU checks reproduce the controller/cursor failure in the released input code and verify the fix through SDK touch samples, click release, controller activity, Alt and camera isolation. Previously accepted controls were not broadly retested. No game or GPU test was used.

Please report bugs in the LumioseOnline Discord.

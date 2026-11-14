# Keyboard and mouse controls for Pokémon Legends: Z-A

The default layout is designed around movement and aiming with the mouse. The ball-aim camera stability option is in Settings > Controls. The game’s Poké Ball auto-targeting remains enabled. A connected controller disables mouse camera control during gameplay; mouse controls remain available in supported menus. Hold Left Alt to release the cursor and use the mod's touch interface. With Pokémon Legends: Z-A 2.0.2, the cursor also releases automatically on the main menu, Boxes, Satchel, Pokédex, map and Mable's Research screens, and over recognized context menus. Scene detection reads the game itself and does not depend on the Lumiose build.

Hover over main-menu options, party members, Satchel entries, Pokédex grid entries, Mable's Requests, available moves, Outfits entries, and Box slots to highlight them, and click to select them. Click tabs in the Satchel, Mable's screen, Change Moves, and Outfits to change to them. Clicking a Pokémon in Boxes opens its context menu; click outside the menu to close it. To move one instead, hold the left mouse button on it, drag it to a slot and release; its slot image follows the pointer. Scroll to change Boxes, including during a drag. Release outside a slot or right-click to cancel a drag. In the Satchel, drag an item onto a party Pokémon to give it to them when the item supports giving. Right-click also goes back in the supported menus. In lists such as the Satchel, Pokédex grid and Mable's Requests, the wheel scrolls the contents without changing the selected entry. Mable's reward tab retains Up/Down style scrolling. Reading a Pokédex entry blocks mouse input to the grid behind it. Click an active system message box when its continue indicator appears to advance it.

| Action | Default |
| --- | --- |
| Move | W, A, S, D |
| Navigate supported game menus | W / S or Up / Down arrows |
| Camera | Mouse |
| Interact / confirm | E |
| Sprint | Left Shift |
| Dodge roll | Space |
| Crouch / stand | Left Ctrl |
| Focus / lock on | Press Tab to toggle; requires a closed focus indicator and releases when the target is lost |
| Aim Poké Ball | Hold right mouse button |
| Throw Poké Ball | Left mouse button (while aiming or on its own) |
| Cycle Poké Ball types | Scroll wheel while aiming |
| Cancel aiming | Release right mouse button |
| Game menu | Esc from the overworld |
| Satchel | I from the overworld |
| Change Satchel tab | A / D while in the Satchel |
| Boxes | B from the overworld |
| Pokédex | P from the overworld |
| Mable's Research | O from the overworld |
| Map | M |
| Pan map | W / A / S / D |
| Zoom map | Mouse wheel up / down |
| Interact / confirm on map | E |
| Toggle Plus moves while focused | R |
| Cancel / back | Esc |
| Pokémon selection | Scroll wheel when not aiming, or Left / Right arrows |
| Send or switch to selected Pokémon | Middle mouse button or Up arrow |
| Recall Pokémon | Down arrow |
| Moves while focused | 1, 2, 3, 4 (A, B, X, Y respectively) |
| Mega Evolution | Q |
| Reset camera | T |
| Photo mode | Backspace |
| Launcher overlay | F1 |

The overworld shortcuts use the game’s own verified menu-state information on version 2.0.2, independently of the Lumiose version. On unverified game versions, I retains its original game-menu function and Esc remains cancel/back. In a game menu, I continues to act as the game's X button. The dedicated move keys work only while focused. Interact, dodge, menu, and cancel keep their normal functions while focus is toggled on; focus is briefly released for the game to receive the normal action, then resumes. Sprint is suppressed while focused or aiming so it cannot trigger a move or cancel a throw. Keyboard bindings and mouse button assignments can be changed in Settings > Controls > Configure inputs. Existing customized keyboard layouts retain their old defaults for any keys that were not explicitly saved; choosing Restore default controls adopts the new layout.

Automatic prompts follow the latest keyboard or controller activity. On supported game builds, keyboard prompts use separate field, menu, Satchel, focused-move, and ball-aim labels. The overworld menu shows Esc, aiming shows LMB to throw and Esc to cancel, and Satchel tabs show the configured A/D movement keys. Generated game-font glyphs and prompt textures change during play; other builds use the selection from game startup. Xbox identification depends on the controller identity reported by SDL; some third-party devices present themselves as Xbox controllers.

Middle-click recalls the selected Pokémon if it is already out; otherwise it sends the same command as the Up arrow, including when switching party members. Field party prompts show this mouse binding, and box page prompts show wheel up/down. The out-slot is identified from the loaded mod without a fixed mod version; if it cannot be identified safely, middle-click sends Up. A quick scroll followed by middle-click waits for the new selection before choosing the command.

While using keyboard and mouse, the four battle moves appear in a horizontal row at the bottom center, ordered 1, 2, 3, 4. Their move names, cooldown indicators and selection effects remain attached to the corresponding move. The existing controller layout returns when controller input becomes active, and keyboard prompts follow your configured move keys. Camera control during the return from ball aiming uses an elapsed-time window, so it stays available at higher frame rates.

Keyboard prompts include a complete key glyph bank, with distinct left/right modifiers, numpad keys, function/media keys, mouse buttons and wheel directions. Inline button prompts and the move row read current bindings during play. Texture prompts refresh after the binding helper finishes. Existing custom keyboard layouts retain their saved keys; untouched layouts adopt the new defaults.

At the game title screen, press any keyboard key to continue. This confirmation is limited to the verified title state; the confirming key does not become a held gameplay input.

With a controller connected, the mouse cursor stays visible and usable during gameplay and in all menus. Click the mod icons in the overworld directly; Alt is not required. The controller continues to control the camera. Without a controller, hold Left Alt to release the mouse for overworld UI.

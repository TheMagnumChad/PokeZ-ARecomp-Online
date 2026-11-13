# Lumiose Online v0.1.30 - Windows full launcher repair

This full package restores the runtime DLLs and setup tools omitted from v0.1.29. It does not change graphics dependencies or add new performance features.

Existing installations: close the game and use Update now, or extract the entire archive over your existing installation. Keep your game files, lumiose folder, saves, settings and compatible caches. They are not included or deleted by this update.

Fresh installations: extract the entire ZA Recomp folder to a writable location and open Lumiose Online.exe. Keep the preparation console open. First setup asks for TWO locally supplied mod files: the supported legacy 1.3.9 compatibility-reference subsdk9 and your current online-mod subsdk9. Obtain these through your mod provider. Arbitrary NSO files are not supported. The launcher checks compatibility before copying or compiling them. Neither file is bundled here.

Then select your own Pokemon Legends: Z-A game files, including update 2.0.2 and DLC, and follow setup. Game code, models, textures and fonts are processed locally from those files. The first compilation can take several minutes. No game data, game assets, generated game libraries, saves or mod binaries are distributed in this archive.

Requires Windows x64, a Vulkan-capable graphics driver and the Microsoft Visual C++ x64 runtime where required by the graphics components. Third-party notices and model-renderer corresponding source are included. Linux users should retain the existing Linux experimental UI package; this repair targets Windows. No AMD or Steam Deck hardware validation is claimed.

The model viewer remains experimental. This repair does not claim exhaustive model correctness.

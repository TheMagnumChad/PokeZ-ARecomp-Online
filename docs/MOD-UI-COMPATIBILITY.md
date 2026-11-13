# Mod UI compatibility integration

This source change prepares fonts, 160-pixel icons, item pictures, model geometry,
animations and materials from the player's supplied local game. Extracted files,
rendered frames and caches must never be committed or included in distributions.

The optional model worker uses a separately built, self-contained renderer. Its
corresponding source, pinned dependencies and notices must accompany a reviewed
release. Do not use the old NxMiddleware/FlatSharp renderer package. A renderer
build is not an end-to-end launcher pass. Review the actual published folder and
embedded resources before distribution; never package a private game install.

The helper must be rebuilt from `tools/za/prompt_textures.py` for 160-pixel icons.
A successful freezer exit alone is insufficient: test `zaprompts --help`, local
extraction, dimensions, decoder imports, and representative normal/shiny pictures.
Build dependencies are recorded in `tools/lumiose/ui-helper-build-requirements.txt`.

Model cache format 4 includes shared material textures. The worker rebuilds old
incomplete caches but reuses compatible caches. All material paths must stay
within bounded, explicitly permitted game resources and the local output cache.

Validation still required before release: normal-launcher startup, normal/shiny
animated models, material fidelity, persisted Experimental/Icon preference, switching
species and tabs, focus preservation, search/mail entry, item pictures, missing
helper behavior, and worker cleanup after game exit. Use a copied save and the
shared exclusive test slot. Preserve the installed game and user saves.

This branch does not integrate the separately failed controls candidate. UI
input compatibility changes require their own approved scope and testing.

## v0.1.28

Windows renderer v6 passed 18 representative front/back cases (54 changing frames), selection recovery and service restart checks, then personal testing. Market cards default to Icons; saved preferences remain intact and 3D mode is labelled Experimental. Linux uses icons without the Windows 3D worker. This is not exhaustive species or hardware validation. Player game files, extracted assets, translated game libraries, caches and saves are not release content.

# Private renderer v6 corresponding source

Exact modified renderer source; no game assets, ROMs, animations, caches, frames, saves or credentials are included. Runtime assets must be extracted locally from the user's own game.

## Provenance
GFTool base cac7a210fcbef5ecf1ee108c10d1a1bdddc4cd4c, GPL-2.0; preserve LICENSE, notices and corresponding source. MIT BntxLibrary712162c165ed2bd783fe5287a7723f856be19342 with documented byte-array hashing correction. FlatSharp/NxMiddleware removed, replaced by independent bounded read-only parser and texture adapter/deswizzle. Unsupported formats fail explicitly.

## Build
Windows .NET8 SDK:
    dotnet restore UiModelProbe/UiModelProbe.csproj -r win-x64
    dotnet publish UiModelProbe/UiModelProbe.csproj -c Release -r win-x64 --self-contained true --no-restore -o publish -p:UseSharedCompilation=false --disable-build-servers
    dotnet run --project ParserTests/ParserTests.csproj
    dotnet run --project TextureAdapterTests/TextureAdapterTests.csproj

## Changes
Independent UV0/UV1 and material UV selection; sampler metadata; RGBA16F and cubemap support; transparent alpha; straight-sRGB float capture; eye layering; correct TRMSH aggregate-statistics versus joint-index handling; authored cull state and displacement; exact fixed TRACM visibility; bounded animated camera framing; immediate animation frame zero; lazy texture cache ownership; generic authored-parent segment-scale compensation with consistent rest/bind semantics and signed rest scale; material-gated eye surface depth reconstruction using authored positive ParallaxHeight.

Eye surface and displacement are independently implemented compatibility behavior, not proof of original shader equivalence. No species-specific mesh shifts or visibility rules were introduced in these fixes. Existing engineering diagnostic switches are default-off. Existing original probe Bulbasaur preview-bounds specialization remains inherited and is not generalized game logic.

## Validation and limits
V6 passed actual Armature rest identity/world-bind tests. Earlier synthetic parser, adapter, pixel, joint-map, raster, visibility, framing-budget and actual texture-reference ownership tests passed. Nine-model front/back matrix produced54nonempty animation frames, all18cases completed and changedframes. Gastly face/cloud attachment, Binacle both crowns, Spewpa cloak, Venipede primary eyes/green underside and affected eye surfaces were visually inspected. A prior same-source framing-budget candidate passed9selection service replay with distinct animatedframes perselection; finalv6 service/personal acceptance remains required.

Fixed visibility only: unsupported dynamic tracks fail explicitly. Missing companion retains default visibility. Bounded framing samples a segment, not every possible pose. No original-renderer pixel parity, exhaustive species/form coverage, AMD/Linux/SteamDeck hardware validation, public release approval or legal conclusion is claimed. This private candidate must pass final service checks and user testing before any push/release.

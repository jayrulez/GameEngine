# The data root's layout

> Status: DONE (2026-10-28)

`Data/` held everything under one `Assets` folder: what the engine reads at run time, the editor's
own files, the code samples' models and sounds, test fixtures and the files a new project starts
with. The editor's distribution copied all of it (34 MB, about 5 MB of which the editor reads). The
folders now say who reads them, so a distribution copies whole folders.

## The layout (the user's ruling, 2026-10-28)

| Folder | Holds | Ships with |
|---|---|---|
| `Data/Engine` | what the runtime reads: `Shaders` (the sources, and the cooked pack a distribution starts from) and `Fonts` (Roboto, the GUI's built-in default) | the editor, every exported game |
| `Data/Editor` | the editor's pages, sheets and icons, its interface fonts (`Fonts`: Inter, JetBrains Mono) and the code editor's languages (`Code`) | the editor |
| `Data/Branding` | the engine's logo, splash and window icons | the editor (a game embeds the logo) |
| `Data/Templates` | what a new project starts with: its sky (and its font, read from `Data/Engine/Fonts`, not copied) | the editor |
| `Data/Samples` | the code samples' content, one shared library by kind (`Models`, `Environment`, `Audio`, `Images`, `Shaders`, `UI`, `Fonts`) | nothing |
| `Data/Tests` | test fixtures, and the files only tests read | nothing |
| `Data/SampleProjects` | the game projects, unchanged for now (whether the editor ships them is open) | nothing yet |
| `Data/Output` | generated (the native samples' cooked content, `Output/<Sample>`), ignored | nothing |

Rulings: the native samples keep one shared library rather than a folder each, since they share
models (the Fox, the Quaternius character), and their output stays in `Data/Output`. Tests read an
asset a sample also uses from `Data/Samples` rather than keeping a copy. `BlueSky.hdr` (from
traktor) is dropped; the Sandbox's HDR sky is the template's. The licence of each asset moves with
it, and a missing one is added where its source is known (Quaternius CC0, the Fox's for its
nested variant); `images/logo.png` is the project's own; `cube_sky`'s source is the user's to
look up.

## The layers

Each layer is one commit: the files moved with `git mv` and every path that names them changed,
the build green, the editor started and its log read (a font, logo or sky that is not found only
logs).

1. `Data/Branding`.
2. The code languages into `Data/Editor/Code`.
3. The editor's fonts into `Data/Editor/Fonts`.
4. The shaders into `Data/Engine/Shaders` (the pack's path changes for distributions, exported
   games and the web player; the export templates are rebuilt with it).
5. Roboto into `Data/Engine/Fonts`.
6. The new project's sky into `Data/Templates`.
7. The files only tests read into `Data/Tests`.
8. The rest into `Data/Samples`, a folder at a time; `BlueSky.hdr` and the unused duplicates
   (`audio/music/platformer`, the Kenney and Quaternius FBX folders, `Character_Gun.gltf`) dropped.
9. `Data/Assets` gone; the distributions copy `Engine`, `Editor` (less its mock), `Branding` and
   `Templates`; the documents (`Systems/data-root.md`, the READMEs) in step.

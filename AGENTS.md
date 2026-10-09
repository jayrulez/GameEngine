# Working on the engine: notes for agents

How to build, test and change this engine without breaking it. The README describes what the
engine is; this is how to work in it. The binding rules in full are in
[Documentation/Process/CONVENTIONS.md](Documentation/Process/CONVENTIONS.md).

## Build

Everything goes through CMake presets; outputs land in `Bin/<Config>/<Platform>-<Compiler>/`.

```
cmake --preset clang                                   # once per build tree
cmake --build build/clang -j4 --target Tools.Editor    # one target
cmake --build build/clang -j4                          # everything
```

- Develop against Debug, and keep BOTH compilers green: `build/clang` and `build/gcc`.
- Build at `-j4`, ONE build at a time, and never beside another heavy build (a second ninja,
  the Steam Deck container, another engine's build). Two ninjas on one tree corrupt its dyndep
  state; two heavy builds at once run the machine out of memory.
- Point the compiler's temp at the disk for big builds: `export TMPDIR=$PWD/build/tmp` (an
  absolute path; `/tmp` is a small tmpfs).
- A tool built before a change does not have it: rebuild the editor, the player or `Tools.Mcp`
  after changing what they use. The script compiler lives in them too, so a stale editor
  refuses a game script that uses new script API. An export template is a built player as
  well: rebuild it (`scripts/build-steamdeck.sh` for the Deck) after engine changes, or an
  exported game runs yesterday's engine.

## Test

- Every Foundation and Engine module has a sibling `.Tests` target; `Code/Integration/` holds
  the flows that cross collections. Every behaviour you add, change or fix lands with a test.
- Before calling work done, run the whole battery, not only the touched targets, on the
  lanes: clang, gcc and clang-shared (`ctest --preset <lane> -j4`), the wasm build
  (`cmake --build build/wasm -j4`, no ctest), and ASAN (a `-fsanitize=address,undefined` Debug
  tree; `ASAN_OPTIONS=detect_leaks=1 ctest -j4`, then search its `LastTest.log` for
  `runtime error|AddressSanitizer|LeakSanitizer`). Reports inside `ThirdParty/angelscript` are
  known noise; anything in `Code/` is ours.
- Tests write scratch data under the build tree they run in or `.test-scratch/`; both are ignored.

## Change

- **Verify, don't guess.** Read the source, or run it, before relying on or stating a
  constant, a count or an API's behaviour.
- **Match the code around you**: C++23 modules (heavy headers and `REFLECT_*` bodies in
  implementation units), Allman braces, PascalCase functions, full names, `m_` members,
  comments that say why. No em or en dashes anywhere. Thread the owner's allocator; a new
  `DefaultAllocator()` outside tests fails Core.Tests' tripwire. Ask a type or a capability
  instead of hard-coding a list of type names.
- **Keep the documents honest in the same commit.** A change to the MCP tools updates
  `Documentation/Shipping/McpGuide.md`; the engine tool count (`kEngineToolCount`) is a
  deliberate tripwire, bumped when a tool is added.
- **Commits**: one per piece of work (a layer with its tests), the subject `Area: what it
  does` with a body saying why, and no `Co-Authored-By` or session trailer lines. Stage
  explicit paths. Push only when asked.
- **Third-party content**: check its licence before adding it. Nothing goes in that may not
  be published; keep the licence text beside it, and credit what requires it.
- **Visual checks belong to the user.** Hand over a build and list what to look at; do not
  claim something looks right.

## Agent tooling

- `Tools.Mcp` is the headless MCP host; the editor serves the same tools over HTTP for its open
  project (`--mcp` or `--mcp-port <port>`), plus page and play-in-editor tools.
  `.claude/skills/engine-mcp` is the recipe to build and wire it; once connected, read
  `docs://McpGuide.md` (`Documentation/Shipping/McpGuide.md`), the operating manual.
- When an MCP tool is missing or wrong for a task, that is a finding: fix the tool in the
  engine, rather than routing around it by editing project files by hand.
- Play in editor runs the game through the editor's Game tab; after a playtest there, play the
  exported build too, since the player is a different host.

## Sample projects

`Data/SampleProjects/` holds game projects: **Sky Hopper** (`PlatformerGame`), built entirely
through the MCP tools, **PaperKid**, rebuilt the same way (its authoring scripts in `Tools/`),
**Snowline**, a snowboard time trial generated the same way (`Tools/`: terrain, courses, sounds),
**Lamplight**, a stealth heist generated the same way (`Tools/`: Blender models, the level
generator, sounds), and **NativeSample**, a game with native code.
Their `Cooked/`, `.cache/`, `Editor/` and `Dist/` are generated and ignored. Integration.Mcp
checks that PaperKid, Sky Hopper and Snowline read at the current data versions and cook. Sky Hopper's
`CREDITS.md` and `Licenses/` must stay in step with its assets, and so must PaperKid's.

## Steam Deck

`scripts/build-steamdeck.sh` builds a player in an Ubuntu 22.04 container (glibc 2.35, below
SteamOS) and installs it as the `gameengine-steamdeck-release-<version>` export template. It
needs podman or docker, mounts the live tree (do not edit it while the build runs), and
nothing else building beside it.

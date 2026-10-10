# 0.1 backlog - open items for the 0.1 release

> The work queued for the engine's 0.1 release (the editor's polish, the release builds, the
> website, the flagship game, custom shaders, the docs, the fifth sample). Moved here from
> `weekly_backlog.md` on 2026-10-10; that file keeps everything else open. Pull an item out of
> here into the current weekly when it is started; delete it here when done.

## Queued 2026-10-09 and after (user)

- **A scene grid that fits our scenes, perhaps infinite** (user 2026-10-09): the scene view's
  ground grid is debug-draw lines, a fixed 20 m square in 20 cells (`ScenePageImpl.cpp`,
  `dd.DrawGrid(..., 20.0f, 20, ...)`), too small for most of our scenes now (Snowline's
  courses, Lamplight's levels). Look into the options, likely done in a shader: a ground plane
  drawn as one quad (or full-screen pass) whose fragment shader draws the lines from world
  position, anti-aliased by screen-space derivatives, fading with distance, with the spacing
  stepping by powers of ten as the camera climbs and a coarser line every N, depth-tested
  against the scene so geometry hides it. Compare with resizing the debug-draw grid to the
  scene's bounds or the camera. Keep the per-scene grid toggle (`SceneViewSettings`).
- **A measurement overlay while zooming** (user 2026-10-09): when the mouse wheel zooms the scene
  view, show the scale for a moment (what a grid cell or a screen length measures in metres at
  the focus, and the camera's distance to it), fading out shortly after the zoom stops. Pairs
  with the grid above: the grid's spacing steps as you zoom, and the overlay says what a cell is.
- **An orientation gizmo in the scene view's top right** (user 2026-10-09): a small overlay
  showing which way the camera faces, with the world axes and the faces / directions labelled
  (X, Y, Z and their negatives, or Top, Front, Right), turning with the view. A view cube or
  axis tripod as other editors have. Clicking a face or an axis pole snaps the view to look
  along it (user 2026-10-09: "clicking on the faces/poles should work").
- **An orthographic editor camera** (user 2026-10-09): the scene view's camera can switch
  between perspective and orthographic with a toggle (in the view's toolbar, and a key). Pairs
  with the orientation gizmo: snapping to a face is where an orthographic top, front or side
  view is most used. The engine's cameras already have an orthographic projection
  (`MakeCameraProjection`); the editor camera's zoom becomes the ortho size there.
- **A scene page keeps its split sizes** (user 2026-10-09): the hierarchy / viewport /
  inspector splits of a scene page go back to their defaults each time a scene opens, so they
  are dragged again every time. Save them per page (per scene) and restore them on open,
  as more fields on the per-scene record the grid toggle already lives in (`SceneViewPref`,
  `Editor.Scene/SceneViewSettings.cppm`, kept in the project's editor settings).
- **The Welcome page needs a purpose** (user 2026-10-09, to think about): it has been a stub
  from the start. Its real job is to hold the top split: with every editor page closed, the
  top section stays for a newly opened page to dock into (without it the Console and Assets
  panels grow to the top and a new scene docks as a tab beside them). Give it a use while it
  does that: perhaps the project's page, with the project's information and quick ways to its
  settings and the other project-level things. Keep the docking job whatever it becomes.
- **An editor logo, and a splash screen while the editor loads** (user 2026-10-09): the editor
  has no logo (for its window and taskbar icon, the project manager and the Welcome page), and
  shows nothing while it starts and opens a project. Design a logo (our own, with its licence
  beside it; GameEngine, never Raptor, in public text), then a splash window with it and the
  loading progress, shown from launch until the main window is ready.
- **Polish the project launcher** (user 2026-10-09): the page the editor opens on to pick a
  project. A list / grid toggle, the grid showing each project's thumbnail (a picture of the
  project to recognise it by); and some of the per-project options moved into a menu (a
  hamburger or similar) so the page reads cleanly.
- **The asset browser's tree: the chevron gap shrinks with each level** (user 2026-10-09): in the
  Assets panel's folder hierarchy the space before each level's chevron gets smaller the deeper
  the level, where the scene page's entity hierarchy keeps it even. Compare how the two build
  their trees (`Editor.App/AssetsView.cppm` and `Editor.Scene/HierarchyView*`) and make the
  assets tree indent as the entity one does.
- **The asset browser's List / Grid buttons as icon buttons** (user 2026-10-09): they are text
  buttons now; make them icon buttons (a list icon and a grid icon, with the words as their
  tooltips).
- **A more polished Console header** (user 2026-10-09): revamp the look of the Console panel's
  header row, its log-level filters and its search field, so it reads as finished UI.
- **Editor preferences by category, in tabs** (user 2026-10-09): the preferences show every
  setting in one long view; split them into categories, a tab each.
- **Project settings by category, in tabs** (user 2026-10-09): the same clean-up for the
  project settings, their categories a tab each, matching the preferences.
- **Clean up and polish the export UI** (user 2026-10-09): the editor's export flow (choosing
  a preset, its settings, the templates it resolves to, running the export and its result),
  its look and how it is used, to the same finish as the rest.
- **Clean up and polish export template management** (user 2026-10-09): how the editor lists,
  installs, inspects and removes export templates (the prebuilt players a preset exports
  against, `~/.local/share/gameengine/templates`), and how a preset shows the template it
  resolves to.
- **Project-wide defaults for scene settings, per domain** (user 2026-10-09, to think about): a
  scene's render settings come from the scene itself (its own values, or a profile asset it
  names, as Lamplight's levels share theirs), else from hard-coded defaults; other domains are
  probably the same. Add a project layer between: each domain's settings with project-wide
  defaults, set in the project settings, so the lookup goes scene, then project, then the
  hard-coded default. Survey which domains have scene settings, and how a project default
  relates to the profiles (a project default could simply name a profile).
- **The scene hierarchy's scrollbar takes its own space** (user 2026-10-09): it overlays the
  rows now, and grabbing it easily drags an entity instead. Reserve its width beside the rows;
  and first check whether a bug lets a press on the scrollbar reach the row under it (it
  should never start a row drag), which would be the real fix either way.
- **Polish the input map page** (user 2026-10-09: "needs a lot of polish"): the editor page for
  an input map asset (its actions and their key, pad and touch bindings). Its layout, how
  bindings are shown, added and changed, and its look, to the finish of the other pages.
- **A preview scene for the environment and post-process profile pages** (user 2026-10-09): let
  the page for an Environment or Post Process profile pick one of the project's scenes to preview
  the profile on, so you see exactly how it looks there (Lamplight's shared profiles were tuned by
  playing the levels). Remember the choice per profile.
- **Custom shaders through the editor, before the 0.1 release** (user 2026-10-09, ready and
  showcased for 0.1): custom shaders already work at the engine level, and there are shader
  and shader include assets, but nothing outside the unit tests has used them (the shader asset
  is `Pipeline/Shaders.Pipeline/ShaderAsset.cppm`; includes resolve in `Shaders.System`; confirm
  where the include asset lives when this starts). Expose the pipeline in the editor (author a
  shader asset, a material that uses it, see it cook, compile and draw, with its errors shown),
  and showcase it: a sample project or a demo game's effect built on a custom shader.
- **Documentation cleaned up and made proper, for the 0.1 release** (user 2026-10-09): the
  documentation as a user of the engine meets it, current and organised (getting started, the
  editor, each system, scripting, exporting, the MCP guide), with the plans, specs and history
  that grew in `Documentation/` kept apart from it or archived.
- **The 0.1 release** (user 2026-10-09): the release itself, with
  - **a website** for the engine (beyond the demos site, GameEngineDemos);
  - **a showcase game**: the engine's flagship, released on Steam. Separate from the sample
    games in this repository and not part of it (its own project and repository); the samples
    stay the engine's examples.
  The items marked for 0.1 above (custom shaders through the editor, the documentation, the
  fifth sample below) belong to it.
- **A fifth sample game, on networking and crowds, before 0.1** (user 2026-10-09): no sample
  project uses the network stack or crowds yet, so neither has been showcased or stressed in a
  game. The fifth sample in `Data/SampleProjects/` built around both: players over the network
  (the engine's reliable UDP and replication) and a crowd (instanced skinning, navigation
  crowds), built and playtested through the MCP tools as the others were. (Lamplight's crowd
  was set aside "for later" when it was planned.)
- **Scene facades as scene properties, as Sedulous has them? Decide before 0.1** (user
  2026-10-09): Sedulous binds a scene facade as a property of the scene (`scene.Physics.RayCast`,
  its SceneFacade role); ours are reached through the facade type (`ScenePhysics::of(scene)`,
  `SceneRender::of`, `SceneAudio::of`, ...), kept so by the ruling of 2026-10-01 until the role
  was revisited. Look at it before 0.1, while the script surface can still change freely: which
  reads more naturally in AngelScript and Luau, what moving means for every facade, the sample
  games' scripts and the docs. The earlier write-up is `Plans/beef-script-facades-note.md`.
- **Sort the backlog and plans into 0.1 and 0.2** (user 2026-10-09): go through `weekly_backlog.md`,
  the plans and everything else open (`Documentation/Plans`, `Ideas`, `Specs` with open parts,
  `terrain-backlog.md`), keep what 0.1 needs, and move everything that will not be looked at for
  0.1 to 0.2, so what is left in front of 0.1 is only the release's work.
- **The 0.1 release builds and samples** (user 2026-10-09): build and package what users
  download for 0.1: the export templates (each platform), the editor and the command-line tools
  (Tools.Mcp, Tools.Export, Tools.Cook and the rest a user needs; `scripts/build-editor-dist.*`
  and `scripts/build-export-templates.*` are the starting point), and the sample projects
  packaged so users have something to open and play with. Perhaps the project launcher lists
  remote samples too, to download and open from there (pairs with the launcher polish above).
- **Windows build scripts and presets, made consistent** (user 2026-10-10): the Windows scripts
  build with one compiler and package from another's output. `build-editor-dist.ps1` configures
  `build\msvc-release` with no compiler pinned (whatever the environment hands CMake; on the
  user's machine, clang) but copies from a fixed `Bin\Release\Win64-MSVC`.
  `build-export-templates.ps1 -Compiler Clang` passes clang-cl, yet uses the same
  `build/msvc-release` tree, and an existing tree keeps the compiler it was first configured
  with. `CMakePresets.json` has only Debug MSVC presets (`msvc`, `msvc-shared`): no MSVC
  release or shipping preset, and no clang-cl ones. Add the missing presets (release and
  shipping for MSVC and clang-cl, matching the Linux set), have both scripts configure through
  a preset chosen by `-Compiler`, and derive the `Bin\...\Win64-<Compiler>` folder from that
  same choice, so the build and the copy cannot disagree. Check the `.sh` scripts the same way.
- **The editor dist cannot export: it ships no shader sources** (user 2026-10-10):
  `build-editor-dist.sh` and `.ps1` stage only the cooked `Data/Shaders/shaders.dpak` and skip
  the `Data/Shaders` sources on purpose ("unneeded in pack mode"). But an export cooks the
  player's pack from those sources (`StageShaderPack` in `Editor.Project/ExportImpl.cpp` reads
  `<dataRoot>/Shaders` and fails with "cannot cook shaders: no 'Shaders' under the data root"),
  so the distributed editor cannot export a game. Stage the shader sources (the `.hlsl` and
  `.hlsli` the cook reads) beside the pack in both scripts, fix their comments, and check that
  the editor still runs from the pack. Then export a project from a staged dist as the test.
  Custom shaders in the editor (above) will need the sources there too.
- **Luau variants of the sample games, and whether we keep two languages** (user 2026-10-09):
  perhaps write a Luau version of each sample game's scripts (all four are AngelScript), as an
  exercise of the Luau side end to end either way. First decide whether we really want to
  support two scripting languages (the cost of every script facade, test and doc twice, against
  what each brings). The heist plan already had a Luau-only game coming after Lamplight.

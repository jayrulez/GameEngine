# Present transform - a landscape game on a portrait screen

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> Status: APPROVED (user, 2026-10-11: the decisions below). Built, P1 to P5; the user's phone test passed (held upright and turned, auto-rotate on and off, and the lock).
> Track: 0.1 backlog, the web demos on a phone ("Orientation on mobile").
> Mirror: Sedulous ports each phase after it lands.

## The problem

A phone held upright gives the page, and so the canvas, a portrait shape. Every sample game is
landscape, so held upright it renders squeezed into a tall narrow view, its touch buttons in odd
places. Turned sideways with auto-rotate on, everything already works, with no engine involvement:
the browser lays the page out again in landscape, the page's resize handler sizes the canvas's
backing store to the new shape, the web shell sees the size change and the swapchain is
reconfigured, and touch positions arrive in the turned page's frame. The system compositor turns the
pixels onto the glass. The engine only ever sees a wider window.

What does not work is the other case: the page stays portrait (the phone held upright, or its
auto-rotate off) while the game wants landscape. Then the engine must present its image turned a
quarter and turn input back, the job a native Android app gives the swapchain's pre-transform
(Vulkan `VkSurfaceTransformFlagBitsKHR`). A browser has no such surface transform, so the engine
does the equivalent itself. Designed as one abstraction, the same piece later maps onto Vulkan's
pre-transform for a native Android player.

## Goals

- A game declares its orientation; on a window of the other shape, it renders at its own shape and
  is presented turned a quarter, filling the screen sideways.
- Every pointer and touch position, every touch region, the UI's layout and its hit testing work in
  the game's frame, as if the window were the game's shape. Nothing in a game changes.
- Where the browser can lock the orientation, the page asks it to, and then nothing turns.
- The editor's Game tab can preview the game on a portrait phone, exercising the turn without one.
- No cost when nothing turns (desktop, a matching window): the path stays as it is today.

## Not in scope

- A half turn (180): no host asks for it; the type allows it, nothing chooses it.
- Native Android (a Vulkan pre-transform): later, on the same type (see "Native, later").
- Per-scene orientation: it is a game's, like its render resolution.

## What exists (facts, 2026-10-11)

- **The present path.** `ApplicationHost::Tick` acquires the backbuffer (`RenderWindow::BeginFrame`,
  `FrameContext{width, height, backbufferView}`); `DefaultApplication::RenderFrame` renders the
  scene straight into it. With a render resolution set (`ProjectSettings::renderWidth/renderHeight`,
  `renderFit`) it builds a `ContentFit` over the backbuffer and passes `SceneSize::FromFit(fit)`:
  the pipeline then draws the scene at that size into a transient target and a **present copy** (the
  tonemap pass in copy mode, `PipelineImpl.cpp` ~2462) samples it into the fitted rectangle of the
  real target. Screen overlays (`RenderSubsystem::RenderOverlays`: the UI's screen tier, ImGui) draw
  after it, at the backbuffer's full size.
- **`ContentFit`** (`Core/Math/ContentFit.cppm`): a region (target space), a content size and a
  `FitMode` (Stretch, Letterbox, Crop, IntegerScale), giving `DstRect`, `SrcRect`, `ToContent`,
  `FromContent`, `Scale`. Axis-aligned. Its users: the present copy (`SceneSize::FromFit`), the
  player's input (`FittedInputSource`: mouse, deltas, touches), the editor's Game tab input
  (`InputSurface`, `InputRouter`) and the UI's screen tier (`UISubsystem::ScreenFit`: layout at the
  resolution, drawn into `DstRect`, pointer points through `ToContent`).
- **The swapchain** (`RHI SwapChainDesc`) has no transform; Vulkan passes `preTransform =
  currentTransform` through; nothing rotates anywhere.

So every place that maps between the window and the game's frame already goes through `ContentFit`.
Teaching it a rotation teaches all of them.

## Design

### 1. `ContentFit` gains a rotation

```cpp
enum class ContentRotation : u8 { None, Quarter, Half, ThreeQuarter }; // content turned clockwise
struct ContentFit { Rectangle region; Float2 contentSize; FitMode mode; ContentRotation rotation; };
```

The content is fitted into the region as if the region were turned back: with a quarter turn, a 1080
x 1920 portrait region fits 1920 x 1080 content exactly. `DstRect` stays the axis-aligned rectangle
the content covers on the target; `ToContent`/`FromContent` turn points through it; `Scale` returns
the per-axis factor in the content's frame (a quarter turn swaps the axes); a new `ToContentDelta`
turns a delta (mouse motion). `None` is today's behaviour, bit for bit.

### 2. A game declares its orientation

`ProjectSettings::orientation : ScreenOrientation { Any, Landscape, Portrait }` (not `Orientation`,
the UI's layout direction), appended (a manifest saved before it reads `Any`), reflected into
Project Settings' Display tab, and overridable per export preset like the render size (a desktop
preset keeps `Any`; a phone-facing Web preset says `Landscape`). `Any` is today's behaviour: the
game takes the window's shape.

The host chooses the rotation each frame: `Landscape` on a window taller than wide, or `Portrait` on
one wider than tall, gets a quarter turn; otherwise none. The direction is the platform's primary
landscape (the phone turned counter-clockwise, Android's `landscape-primary`): the game's top faces
the screen's left edge. (Open question 1.)

### 3. The present turns the image

When the rotation is not `None`, the host always renders through the scaled path, even without a
render resolution: the content size is the render resolution when set, else the window's size with
its axes swapped (so the game renders at the landscape shape of the same pixels). `SceneSize`
carries the rotation (`FromFit`), and the present copy turns its sampling: in copy mode the tonemap
shader swizzles its local UV by the rotation before `UvScale`/`UvOffset` (a push-constant field; the
pass already has the slot for a scaled view's present copy). The scene-tier UI (HUDs on canvases)
draws inside the compose, so it turns with the image for free.

### 4. Input turns back

The host uses `FittedInputSource` whenever the fit is not plain (a render resolution, or a
rotation), not only for a render resolution. Through the rotated `ContentFit`: the mouse's position
and the fingers' `pointerX/Y` map into the content's pixels, the fingers' normalized `x/y` over the
content (so touch regions are in the game's frame), and mouse deltas turn with `ToContentDelta` (a
look speed keeps its direction). The editor's `InputSurface` and `InputRouter` follow from the same
`ContentFit` methods.

### 5. The screen-tier UI draws turned

The screen tier (pushed screens, the overlays, the touch guide) lays out at the content size, as it
does with a render resolution, and `DrawTier` draws its root with the rotation: a VG transform
(translate to the destination's corner, rotate a quarter) before `DrawRootInPass`. Its pointer
mapping (`ScreenPointerPoint`, `ScreenLayoutPoint`) already goes through `ToContent`. The touch
guide is a screen-tier view in the content's frame, so its buttons land in the game's bottom-right
corner whatever the phone does. ImGui (debug only) stays unturned.

### 6. The web page asks for the lock where it can

The player's first tap (a browser grants fullscreen and the lock only from one; the loading card
has no Play button, the game starting as soon as it loads) requests fullscreen and, for a game
that declares an orientation, `screen.orientation.lock("landscape")`, once. Where the lock is granted (Android
Chrome), the page turns and the window is landscape: no rotation. Where it is refused (Safari on
iPhone, a desktop browser), section 3 turns the image. The page reads the orientation from
player.xml, as it reads the name. (Open question 3: whether to ask at all.)

### 7. The editor previews a portrait phone

The Game tab gains a "Phone, portrait" preview: the tab's viewport takes a phone's portrait shape
(9 by 19.5), and the host's choice of rotation runs on it as it runs in the player, so a landscape
game is presented turned in the tab and the mouse drives it through the turned `InputSurface`. It
exercises the whole path (present, input, the screen-tier UI, the touch guide) without a phone, and
an agent can see it through `pie_screenshot`.

### 8. Native, later

A native Android player reads the surface's `currentTransform`: a quarter turn there becomes
`ContentRotation::Quarter` for the present copy and input, the swapchain is created with
`preTransform = currentTransform` and its extent swapped, and the compositor does no work. Same
type, same paths; only the source of the rotation differs (the surface, rather than the orientation
setting against the window's shape). Noted here so the type is right; not built now.

## Phases

- **P1 `ContentFit` rotation (built).** The enum, the four methods with the rotation,
  `ToContentDelta`. Tests (Core.Tests): a quarter turn fits a portrait region exactly; `ToContent` /
  `FromContent` round trip at the corners and centre for each rotation and each `FitMode`; `Scale`
  stays per content axis; `None` is byte-identical to today.
- **P2 The setting and the present (built).** `ProjectSettings::orientation` (+ preset override, the dialog,
  player.xml); `SceneSize` rotation; the tonemap copy swizzle; the host's choice. Tests:
  Engine.Project (round trip, an old manifest reads `Any`); Render.Tests (a scaled view with a
  quarter turn sets the copy's rotation and swaps the destination); a pixel probe (RHI.TestSupport
  harness: a known gradient presented with a quarter turn lands turned, on Vulkan and WebGPU).
- **P3 Input (built).** `FittedInputSource` whenever the fit is not plain; deltas turned. Tests
  (Engine.Input.Tests): a portrait window with a landscape game: a mouse and a finger at the
  window's corners map to the content's corners, a finger's normalized place is over the content, a
  drag's delta turns.
- **P4 The screen-tier UI and the editor preview (built).** `DrawTier` rotation; the Game tab's "Phone,
  portrait" preview. Tests (Engine.UI.Tests): with a quarter turn, a tap at the window point where a
  button is drawn presses it; the touch guide's buttons are in the content's bottom-right.
  Editor.Scene.Tests: the preview's shape and its rotation choice. The user's check in the editor.
- **P5 The page and the samples (built).** The fullscreen and lock request on Play; the four samples' Web
  presets say `Landscape`. The user's phone test, held upright and turned, auto-rotate on and off.

## Decisions (user, 2026-10-11)

1. **The quarter turn is fixed counter-clockwise** when the page is portrait: the game's top on the
   screen's left, Android's primary landscape. No sensor.
2. **The editor previews a portrait phone** (section 7).
3. **The page asks for fullscreen and the orientation lock on Play** only for a game that declares
   an orientation, and only on a touch device.
4. **`Any` is the default**: every project keeps today's behaviour; the samples opt in through their
   Web presets, and a new project's Web preset defaults to `Landscape`.

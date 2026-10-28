# Input

> Status: CURRENT
> Verified: 2026-08-12 @ 9e80f286
> Track: [[input-subsystem]]

Named-action input as data: gameplay reads actions ("Jump", "Move", "Fire"), never raw keys.
Bindings are a cooked asset, rebindable + persistable at runtime, organized into priority-ordered
action sets, with declared value kinds (Button / Axis1D / Axis2D), interactions (Hold/Tap/DoubleTap),
UI-vs-game arbitration, play-in-editor forwarding, and a script facade. Shipped P1-P2 + the touch and
play-in-editor slices of P3; per-player pairing is the remaining P3 item.

## Modules

- **`foundation.shell`** (`Code/Foundation/Shell`) - the RAW device layer (already existed): the
  tagged `InputEvent` stream, `IKeyboard`/`IMouse`/`IGamepad`/`ITouch` facades, and
  `InputSurface`/`InputRouter` for transformed + gated viewport input.
- **`foundation.input`** (`Code/Foundation/Input`) - the ACTION layer: the `InputMap` data model +
  `ActionRuntime` (evaluation, sets, edges, aggregation, processors, interactions) + the consumption
  mask + rebind + user-overlay persistence.
- **`foundation.input.resource`** (`Code/Foundation/Input.Resource`) - the cooked `InputMapResource`.
- **`input.pipeline`** (`Code/Pipeline/Input.Pipeline`) - `InputMapAsset` + its builder.
- **`engine.input`** (`Code/Engine/Engine.Input`) - `InputSubsystem` (runtime tick + the
  `IInputSourceProvider` seam + the per-context `Input` script facade).
- **`editor.input`** (`Code/Editor/Editor.Input`) - `InputMapPage`.

## Data model (the asset)

`InputMap` (one asset = one game's bindings) -> `ActionSet[]` (name + priority) -> `Action[]` (name +
declared `kind`: Button / Axis1D / Axis2D) -> `Binding[]` (key / mouse-button / mouse-axis /
gamepad-button / gamepad-axis / gamepad-stick / composite-2D / touch). Per-action processors
(smoothing sensitivity/gravity/snap, response-curve exponent, a time-scale flag with mouse-delta
NEVER time-scaled) and interactions (`Hold` / `Tap` / `DoubleTap`). Kinds are DECLARED, not inferred
from Sedulous's collapsed `{X,Y}`; binding-to-kind mismatches validate at save + cook.

## Runtime evaluation (`ActionRuntime`)

Fed per frame from the shell event stream + polled device facades:
- **Per-device aggregation** (Godot): pressed OR-folds, strength MAX-folds across devices/bindings;
  `raw` bypasses processors.
- **Edges** (`WasPressed`/`WasReleased`) against a frame counter.
- **Queries** on a name-hash-resolved `ActionRef`: `IsDown` / `WasPressed` / `WasReleased` / `Value` /
  `Value2D`, plus synthesized `Axis(neg,pos)` / `Vector2(...)` with circular dead zone.
- **Set stack**: all enabled sets evaluate; queries resolve by priority. Pushing an exclusive set
  (a pause menu -> "Menu") makes other sets read released, with release-edges firing once and
  held-then-suppressed actions latched until physically released.

## UI-vs-game arbitration

`ActionRuntime::SetConsumptionMask` is driven each frame by `Engine.GUI`'s `GuiSubsystem`: the GUI
reports which input CLASSES (pointer / keyboard / text) it took, and matching binding sources are
masked for that frame (wired by construction - both live in the runtime tick, the GUI first). An exclusive
set handles modal states; `WantsTextInput` additionally suppresses ALL key bindings while an EditText
has focus.

## Play-in-editor

`ActionRuntime` takes its devices through the `IInputSourceProvider` seam: the game host passes the
raw shell, play-in-editor passes the viewport's gated/transformed `InputSurface` (hover = mouse,
focus = keyboard, click-to-focus) - so keyboard + gamepad forward from the editor viewport by
construction.

## Resources + rebinding

- **`InputMapResource`** (cooked, write-through: validate + serialize) - `resource::Ref`; the runtime
  binds the project's `defaultInputMapId` at startup.
- **User rebind overlay** - NOT part of the asset. `InputBindingOverrides` (a `foundation.settings`
  section in the USER file) stores per-action replacement bindings; the runtime applies
  overlay-over-asset at load and after each rebind. Reset = clear the overlay (the asset stays
  pristine).

## Editor + scripting

- **`InputMapPage`** - tree of sets -> actions -> bindings (add/remove/reorder, undoable), with a
  per-row **Listen** capture (must-have / must-not-have capability filters so an axis rebind ignores
  keys) and inline validation (duplicate bindings within a set, kind mismatches, empty actions).
- **`Input` facade** (per-context service resolution: each script reaches its own run's
  `ActionRuntime`; AngelScript `Input::`, Luau `Input.`): `isDown` / `wasPressed` / `wasReleased` /
  `value` / `valueX` / `valueY` / `value2D` by action name, `pushSet` / `popSet` / `enableSet`.
  **Rumble**: `rumble(low, high, seconds)` runs gamepad 0's motors (`low` the heavy, low-frequency
  one, `high` the light, high-frequency one, each 0 to 1), `rumble(gamepad, low, high, seconds)` a
  pad by index, `stopRumble()` every pad. A request waits for the runtime's next update and reaches
  the pad through that frame's devices, so a runtime holds no device and each run (an editor Game
  tab, the player) rumbles only its own; a later request for the same pad replaces it. A run's end
  stops its pads (`GameInstance::StopScript`). Rumble is not gated on the Game tab's focus, as in
  Sedulous: asking an unfocused pad to stop has to work.

## Touch + portability

Touch bindings shipped (TouchButton regions + a floating TouchStick), demonstrated on the Steam Deck
in the InputActions sample. SDL scancodes are physical (layout-independent matching; localized labels
via keycode translation). WASM delivers the same events; the runtime tolerates device counts changing
any frame (hotplug + Gamepad-API gesture activation).

## Deferred

Per-player device pairing (`PlayerInput`), action-triggered haptics, per-scene/split-screen input:
`Documentation/Backlog/input-followups.md`.

---

Design rationale (the reference survey - Sedulous / ezEngine / Godot / Flax - and the open questions)
is in `Documentation/Archive/input-design-history.md`.

# Luau scripting backend + Wren retirement

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

**Status:** SPEC, ready to build (2026-08-08). Decision made: Luau (not Lua
5.4) - the agent/MCP synergy (real typechecker -> `script_validate` returns
type errors against the actual bound surface), best-in-class interpreter
performance on the no-JIT targets (wasm, console-class), native vector type,
sandboxed stdlib, MSVC-native. Wren is retired at the END (P7), not the
start - the two run in parallel until Luau passes everything Wren passes.

The backend-neutral architecture (registry + conformance battery +
capability flags + backend-agnostic facades) is what makes this a bounded
swap: Luau must satisfy the same contract AngelScript certified against.

## Vendoring + build (P1 gate)

- `ThirdParty/Luau`, pinned release tag, SOURCE vendored (the SDL
  precedent; no FetchContent). Only the pieces we need: VM, Compiler,
  Analysis (for P5) - Luau's repo layout separates them.
- Build constraints to verify AT BRING-UP (fail loud, fix before P2):
  - The engine builds `-fno-exceptions -fno-rtti`; Luau is C++ and its
    error unwinding must be configured for LONGJMP mode, not C++
    exceptions. Confirm the vendored version supports it and pin the
    config; a Luau error crossing our frames as a C++ exception is a
    hard-abort under our flags.
  - GCC module hygiene: Luau headers appear ONLY in implementation units
    (the third-party-headers rule).
  - Compilers: clang + gcc + MSVC (the new preset) + Emscripten (gate the
    wasm climb like AngelScript's - expect a small patch class, e.g. the
    Jolt `<type_traits>` kind; isolate any Luau patch as its own commit).
- Bytecode compatibility is NOT stable across Luau versions: the cook
  fingerprint must include the vendored Luau version so a vendor bump
  recooks every script pack automatically. Do this in P3, day one.

## The authoring idiom (design piece - Lua has no classes)

The behavior contract stays shape-identical to Wren's, spelled in Lua's
metatable OOP. A behavior chunk RETURNS its class table:

    local Mover = {}
    Mover.__index = Mover

    function Mover.new(entity)
        local self = setmetatable({}, Mover)
        self.entity = entity
        self.speed = 2.0            -- editor-property (harvested)
        return self
    end

    function Mover:onStart() end
    function Mover:onUpdate(dt)
        local p = self.entity.position()
        self.entity.setPosition(p.x + self.speed * dt, p.y, p.z)
    end

    return Mover

- The backend calls `new(entity)` (Level: `new(scene)`; Game:
  `new()`), then invokes methods with self - the same
  construct-with-owner contract every tier already has.
- Editor properties harvest from the constructed instance's fields (the
  Wren harvest precedent, table-walk instead of field-list).
- Handlers are dispatch-by-presence (`onStart`/`onUpdate`/`on<Event>`/
  `onContactBegin`...), same as both existing backends.
- Reserved names carry over: `Level`, `Game`, `SceneLoader`, `run`.
- Numbers: Luau numbers are doubles ONLY (no 5.3-style integer subtype) -
  identical to Wren, so the facade-numerics rule transfers verbatim
  (methods narrow via reflected parameter types, range-checked; globals
  surface f64).

## Phases

**P1 - VM bring-up + battery.** `Foundation/Script.Luau` (module
`draconic.script.luau`) implementing the IScriptManager/context contract:
compile+load a chunk, globals, GC-after-load, `IScriptDelegate` (a Lua
function ref held via the registry, Invoke marshals args, the Detach
protocol on VM teardown - mirror the Wren delegate exactly, including
"error if the owning context is gone"), capability flags (coroutines: YES,
native). CERTIFICATION = the conformance battery green, the same bar
AngelScript met. ASAN from the first test.

**P2 - the emitter (bind the reflected surface).** Foreign types via
userdata + metatables: reflected classes get a metatable with `__index`
dispatching properties/methods through the existing reflection dispatch
(`ToInstance`, borrow mode, RESOLVE mode - all backend-neutral already).
Component `.of` factories, enums, containers-as-ops, constructor-less
returned handles - everything Units 1/2a/2b delivered for Wren+AS.
- SURFACE POLICY: the reachability closure remains the policy (per the
  smoketest-fixes #8 ruling) even though Lua's closure-based binding has
  no dispatch-pool constraint - the bounded, script-relevant surface is
  the product decision; Luau just makes it cheap.
- NATIVE VECTORS (the Luau-specific win): map `Float3` to Luau's built-in
  vector type (3x f32 - an exact match). Property gets/sets and method
  params marshal Float3 <-> vector directly; scripts get `v.x`,
  arithmetic, and no userdata allocation per vector. This REPLACES the
  boxed-Float3-handle model the other backends use - measure the
  difference in the battery's perf case and document it.

**P3 - cook + starters.** Luau Compiler at cook: `ScriptClassAsset`
(language = luau) cooks source -> bytecode into the pack; the PLAYER loads
bytecode only (no compiler shipped - sandbox win); dev-mode hot reload
compiles at load like today. New-Asset creator + starter script (the
idiom above, with the worked facade examples the other starters have -
starters must not reproduce classic errors, per the AS lesson). Cook
fingerprint includes the Luau version (see Vendoring).

**P4 - behaviors + tiers end-to-end. LARGELY DONE (Opus, 2026-08-10).**
The run host was ALREADY backend-neutral (EnsureClassLoaded resolves the
backend from the ScriptClass language), so most wiring was free - the work
was proving each dispatch surface on Luau + fixing the gaps:
- a91cc0f1: behavior lifecycle (onStart/onUpdate) + PROPERTY APPLY. Gap:
  ApplyProperties Invokes `<name>=` (Wren setter convention); Luau properties
  are plain instance fields, so LuauScriptObject::Invoke special-cases a
  trailing `=` to write the field directly (mirrors AngelScript's settable
  member - no run-host change).
- 4930b534: Level tier (SceneScriptSystem) + the scene EVENT BUS end to end
  (self.entity.scene.events:emit(name, payload) -> on<Event>).
- e09829f8: entity.send, updateInterval, coroutines, physics onContactBegin.
  Coroutine TIMING FIX: Luau defers the first resume to AdvanceCoroutines, so
  it now also counts that frame's dt against the wait - identical wait timing
  to Wren/AS (was one frame late).
- bc930857: the Roll Call sample game runs on a Luau Level (the acceptance).
- ALSO fixed a PRE-EXISTING AngelScript bug the emit path surfaced (e0c09ab0):
  a `?&in` primitive payload (emit/send) was read by-value (garbage) instead of
  through GetArgAddress, so numeric payloads never reached on<Event>/on<Message>.
- rest of the scene battery mirrored on Luau: enable/disable edges, onDestroy
  (entity + scene-stop), faulting-behavior-disabled, hot reload (product swap +
  override re-apply), entity-typed property (guid -> live handle), Log/Time/
  Random facades, Scene.spawn (Asset/Guid property both ways). ALL three tiers
  now proven: the Game ORCHESTRATOR tier runs on Luau too (game.luau resolves
  the backend; launch/update/exit) - Engine.GameInstance.Tests.
Engine.Script.Tests + Engine.GameInstance.Tests green on clang + gcc + ASAN.
The behavior/tier dispatch surface is fully covered on Luau; what remains for
the TRACK is P5 (typed declarations) onward, not more P4.

Original scope: Wire the backend into the behavior dispatch, Level and Game
tiers, coroutines, `entity.send`, physics contact events, the event-bus bridge
(`on<Event>` harvest), pre-scene null-safety battery, updateInterval.
Acceptance: the full script battery + scene battery green on Luau; Roll Call
runs on a Luau Level.

**P5 - typed declarations + analysis (the agent payoff).** A declaration
EMITTER alongside the binding emitter: reflection -> `.d.luau`
declarations for the whole bound surface (classes, methods with typed
params from ParamInfo + the A6 names, enums, vectors). Wire
`luau-analyze` into: (a) the MCP `script_validate` tool (agents get TYPE
errors against the real API), (b) an optional cook WARNING pass (typecheck
on cook, off by default until proven). The declarations regenerate when
reflection changes (cook-time artifact, not committed).

**P6 - debugger (deferred-allowed).** Luau's single-step/breakpoint
callbacks mapped onto the AS suspension model (IsDebugPaused, ScriptPage
arrow + locals). This is the least-documented Luau area - timebox a spike
first; if it fights back, ship P1-P5 without it and keep the AS debugger
as the debugging backend until a second pass. Luau does not gate Wren
retirement on the debugger (Wren never had one either).

**P7 - Wren retirement (one sweep, AFTER parity).** Gate: battery + scene
battery + demo scripts + Roll Call all green on Luau, and the user's
visual pass on a Luau-scripted scene. Then one sweep commit: delete
`Foundation/Script.Wren` + `ThirdParty/wren` + the Wren starters/creator/
tests, `DRACONIC_ENABLE_WREN` plumbing, the Wren prelude/emitter, and
every `.wren` demo script (rewritten to `.luau` in P4). Docs sweep:
adding-facades.md gotchas gain a Luau section, lose the Wren one; the
capability registry drops the Wren row. AngelScript keeps the second seat
(typed contrast + the debugger until P6).

## Gotchas ledger (start here when something is weird)

- Luau strips `io`/`os`/`loadstring` by design - correct for us; the
  backend must also NOT expose `require` file access (module resolution
  goes through our content system only, like Wren's prelude).
- One VM per run host, main-thread only (the existing model).
- Delegate lifetime: registry refs must be released on VM close in the
  right order (the Wren Detach lesson - the test exists, port it).
- Bytecode is version-locked (cook fingerprint, above).
- `-fno-exceptions` + longjmp mode: verify a script RUNTIME ERROR inside
  a native callback unwinds cleanly through OUR frames (no destructors
  skipped in the C++ layers Luau jumps over - keep native callback frames
  trivially destructible or route through a protected-call boundary at
  the backend edge; write the test that errors from inside a facade call).

## Acceptance (track-level)

Both compilers + MSVC + wasm green; conformance battery + script/scene
batteries green on Luau; ASAN + TSAN on the backend tests; Roll Call and
the demo scripts run on Luau (user visual pass); `script_validate`
returns a real type error for a wrong-typed facade call (the P5 proof);
Wren fully deleted with no dangling references (grep gate: wren,
DRACONIC_ENABLE_WREN, .wren).

---

## PRECURSOR SHIPPED (Fable, 2026-08-09) - Opus handoff

P1 (vendor + VM bring-up + battery) is DONE and exceeds the phase bar:

- **Vendored**: FULL upstream tree (user decision - no pruning) at
  `ThirdParty/luau`, release tag 0.733. House-pattern target `ThirdParty::Luau`
  in ThirdParty/CMakeLists.txt compiles the interpreter set only (VM + Compiler
  + Ast + Bytecode + Common + Config) with **LUA_USE_LONGJMP=1 PUBLIC** (the
  -fno-exceptions requirement, verified live). Analysis/ and CodeGen/ are in
  the tree, not compiled - Analysis gets its own target at P5.
- **Backend**: `Code/Foundation/Script.Luau` (module `foundation.script.luau`,
  alias `Foundation::Script.Luau`), factory `CreateLuauScriptManager()`.
  One isolated lua_State per context; Luau's sandboxed stdlib. Implemented:
  Load (compile errors vs runtime faults reported as their kinds), globals,
  Call, CreateInstance (the `Name.new(...)` metatable-OOP idiom), ScriptObject
  Invoke (colon-call self), delegates (registry-ref'd functions, the
  Detach-on-close protocol), the coroutine scheduler (startCoroutine /
  waitSeconds / waitUntil on lua threads; first resume DEFERRED to
  AdvanceCoroutines so a mid-call begin never re-enters the VM),
  CancelCoroutinesFor by instance-table identity, CollectGarbage, and the
  reflected-type emitter (per-context metatables: methods table + property
  get/set through Variant-in-userdata + ToInstance; class tables with new +
  statics; DescribeBoundApi with A6 param names in signatures).
- **CERTIFIED**: the full conformance battery (Script.Luau.Tests, the Luau
  dialect) - 76/76 on clang, gcc, AND ASAN. That includes the introspection
  diff (every bindable registry type bound; Float3::Dot reported), all three
  coroutine scenarios, and delegate GC survival.

### What Opus picks up next (the spec phases, adjusted)

- **P2 - DONE (Opus, 2026-08-10)**: enum marshalling + named tables (525bbbee);
  NATIVE VECTOR mapping Float3 <-> Luau vector (0b877731, measured ~2.6x + enables
  arithmetic; surfaced + fixed a pre-existing Luau numeric-param coercion gap);
  reachability-closure alignment (4f4604d0 - factored HasBindableSurface +
  CollectEmittableTypes into foundation.script.facades as the ONE shared policy,
  Wren + Luau both use it; Luau script_api 26 -> 47 types, now including the
  constructor-less factory-return handles the old simple filter missed; also fixed
  a pre-existing Wren delegate-handle leak ASAN caught); Load line-number parse
  (9175ee85, was always -1). Precursor limits: luaL_sandbox deferred to P4 (revisit
  with the behavior-module design); tables-as-arguments has no reflected-param
  target (documented edge, not a gap).
- **P3 cook - DONE (Opus, 2026-08-10)**: the whole cook + bytecode track.
  - P3a (4cab8ae3): the Bytecode CAPABILITY - Luau is the FIRST backend to
    implement the committed seam. CompileToBlob compiles via luau_compile
    (validating in a throwaway state, since luau_compile embeds syntax errors
    that only surface at load); LoadBlob runs luau_load only (no compiler in
    the player). Added a symmetric IScriptManager::CreateBlob() factory - the
    runtime cannot construct an opaque IScriptBlob by type name, so the
    producing backend makes the empty shell to Serialize(read) a stored blob
    into. LuauScriptBlob serializes its opaque bytes.
  - P3b (e4c6a839): the cook service - new Pipeline::Script.Luau
    (script.luau.pipeline), a LuauScriptCook the neutral builder resolves by
    language, registered in Pipeline.Registration (the editor's New-Asset
    creator loop then lights up "Luau Behavior/Level/Game" automatically - no
    per-language wiring). Compile-check = Load raw source (Luau needs no
    behavior-module framing). Class name found the Luau way (no `class`
    keyword: scan `function <Name>.new`). Property harvest = CONSTRUCT the
    class with a nil owner and walk its scalar fields (number->float,
    boolean->bool, string, Float3->vec3; handles/tables skipped, sorted); a
    probe/ctor fault is FORGIVEN (valid at runtime), not a cook failure. Three
    tier starters use the real engine idiom (global class table, COLON for
    reflected instance methods, DOT for static facades, Float3 native vector).
    Shared harvest-record parse (ParseFloatList + ParseHarvestRecord) promoted
    from the Wren cook into script.pipeline (shared cook convention).
  - P3c (1486e960): bytecode INTO the pack - the cook compiles source to a
    blob and stores the serialized blob in ScriptClassSource.bytecode (source
    stays for dev hot reload); the player reconstructs via CreateBlob +
    Serialize(read) + LoadBlob. Version-locked: LuauBytecodeVersion() =
    LBC_VERSION_TARGET (Luau/Bytecode.h); IScriptLanguageCook::CookVersion()
    feeds ScriptLanguageCookRegistry::CombinedCookVersion() into
    ScriptClassAssetBuilder::Version() (base 1->2 for the new field), so a
    vendor bump recooks every pack.
  - P3e (ddf77bb3): AngelScript ALSO gets bytecode (user request) via
    SaveByteCode/LoadByteCode + the cook emitting bytecode + CookVersion =
    ANGELSCRIPT_VERSION. AS builds through CScriptBuilder (metadata strip), so
    AngelScriptBlobFromModule wraps the ALREADY-built module into the neutral
    blob - same stored format as Luau, and the player loads metadata-stripped
    bytecode instead of re-parsing metadata at runtime.
  All green on clang + gcc + ASAN (0 leaks). Runtime CONSUMPTION of the stored
  bytecode (LoadBehaviorModule preferring bytecode over source) is P4 - the
  module assembly is source-concatenation today; bytecode is produced + carried
  + proven loadable, not yet the runtime's load path.
- **P4 behaviors + tiers** per the spec (LoadBehaviorModule with per-class
  sections, the run-host integration, Roll Call).
- **P5 typed declarations** (Analysis target + .d.luau emitter +
  script_validate). P5a DONE (Opus, 2026-08-10, 4e9192e6): the .d.luau
  EMITTER - EmitLuauDeclarations(types) -> Luau declarations from reflection
  (declare class + value tables, overloaded `new` via intersection types,
  Float3->native vector, enums as number tables), off the shared reachability
  closure. First-pass limits: method arity families collapse to first overload,
  unreflected param names -> arg0/argN.
  **P5b DEFERRED (user, 2026-08-10):** the luau-analyze integration is parked.
  Key reason to record for whoever picks it up: Luau's Analysis library (the
  typechecker, 77 srcs) USES C++ EXCEPTIONS, but the engine builds
  -fno-exceptions - so Analysis cannot be linked in-process (a thrown TypeError
  crossing our frames = hard abort). The path is an EXTERNAL `luau-analyze`
  binary (built with exceptions from the vendored CLI + Analysis + CLI.lib +
  Require, invoked as a subprocess like DXC/naga/tint), which script_validate /
  the cook shells out to with the .d.luau + script. The emitter (P5a) already
  produces the declarations that tool would consume.
- **Known precursor limits to revisit**: Load reports line = -1 (parse the
  "chunk:line:" prefix Luau puts in messages); luaL_sandbox is NOT applied
  (scripts can write globals - required by the battery contract; revisit
  with the behavior-module design); tables as ARGUMENTS to native calls
  marshal as empty (only functions/scalars/strings/userdata cross today).

### OPEN DESIGN QUESTION (Opus -> Fable, 2026-08-09): overloaded methods on non-resolving backends

Surfaced while Opus was doing the enum-marshalling P2 item. Two findings, one
question.

ENUM CORRECTION (context, not the question): my first pass assumed enums are
never emitted as named types. WRONG - AngelScript emits NATIVE enums
(DeclareEnum -> RegisterEnum/RegisterEnumValue), so AS scripts write
`EnumType::Value`. WREN does NOT (bare numbers, enums excluded from binding).
So the contract is already inconsistent. For Luau the plan is: keep the
number<->i64 marshalling (in progress, both directions correct) AND emit named
enum TABLES (`EnumType = { Value = 0 }`) - matching AS ergonomics, agent-friendly,
and P5-typed-decl-friendly. No decision needed; recorded so the inconsistency is
known. (If Wren should later gain enum-constant tables for parity, that is a
separate small item.)

THE QUESTION - OVERLOADED METHODS. The reflection system registers overloaded
methods (same name, different signatures - TypeBuilder Method() overloads).
Backends diverge:
- Wren: resolves overloads AT RUNTIME by arg-type (ResolveOverload /
  SlotMatchesParam) - certified.
- AngelScript: native overloading.
- Luau: CANNOT - its per-context methods table is keyed by NAME, so overloaded
  methods COLLIDE (one wins arbitrarily). A latent bug today, independent of the
  enum work; my enum param-type threading (method->params) is only unambiguous
  for non-overloaded methods.

User's proposal: an `overloadedName` on a method; backends WITHOUT native/runtime
overload resolution (Luau) expose each overload under that distinct script name.
Two fixes exist:
- A. Mirror Wren's RUNTIME arg-type resolution in the Luau thunk. Contained to
  Luau, no reflection change. BUT overload resolution is invisible + ambiguous in
  a TYPED surface (a number matches f32/i32/enum) - it cannot be expressed in
  P5's .d.luau declarations or the agent-facing script_api.
- B. The `overloadedName` route (user's proposal). Cross-cutting: MethodInfo has
  NO metadata slot today, so it needs one; overloaded sites get annotated; each
  backend decides to honor or ignore. Each overload gets a CLEAR unique name in
  script_api + .d.luau - which is exactly what a typed backend + an agent need.

MY LEAN: B - it aligns with where Luau is going (P5 typed decls + agent surface),
where A's invisible resolution is a dead end. But it is your reflection seam, so
two sub-decisions are yours (I put them to the user; they routed both to you):

  Q1 - MECHANISM: (a) general method attributes - add Attribute[] to MethodInfo
  (PropertyInfo/TypeInfo already have them) + TypeBuilder.MethodAttribute();
  overloadedName is one attribute, reusable for later method metadata (displayName/
  deprecated/...). More machinery. OR (b) a dedicated `const char* overloadedName`
  field on MethodInfo + a TypeBuilder setter - minimal, targeted; a general method-
  attribute system can subsume it later.

  Q2 - SCOPE: (a) only non-resolving backends honor it (Luau spells overloads by
  overloadedName; Wren keeps runtime resolution, AS keeps native) - least churn,
  but the script SPELLING of an overload differs per backend. OR (b) ALL backends
  spell an overload by its overloadedName - ONE script surface across backends (an
  agent writes the same call everywhere; script_api is identical per backend),
  Wren/AS stop relying on resolution for annotated overloads.

My leanings if useful: Q1 (b) dedicated field (targeted, least risk; generalize
when a 2nd method-attr appears), Q2 (b) all backends (the cross-backend-consistent
script surface is the agent payoff). But this is your call. Opus holds the Luau
overload work until you rule; the enum marshalling+tables proceeds meanwhile
(non-overloaded methods only - correct today, and overloads are already broken in
Luau regardless).

### Fable RULING (2026-08-09): B, dedicated field, ONE surface - and make the
### contract enforceable

B is right, and your framing found the real principle: overload RESOLUTION is a
runtime guess, and a guess over Lua/Wren doubles cannot even be correct (f32 vs
i32 vs enum overloads are literally indistinguishable at the call site - Wren's
"certified" resolution is certified only for the cases that dodge this). A
distinct name is not a workaround for weak backends; it is the honest spelling
of what an overload IS on a dynamically-typed surface. A is a dead end twice
over: inexpressible in script_api/.d.luau, and unsound for numeric overloads.

Q1 - (b), the dedicated `overloadedName` field on MethodInfo + TypeBuilder
setter. Reasons beyond minimalism: overloadedName is not method METADATA, it is
the method's script-surface IDENTITY - a field reads honestly where an
attribute would bury identity in a metadata bag. When a real second
method-attribute customer appears (per-method doc strings for script_api is
the likely one), the general Attribute[] mirror of PropertyInfo can subsume
this field mechanically. Do not build it on spec.

Q2 - (b), ALL backends spell an annotated overload by its overloadedName. One
script surface is the whole point of the backend-neutral architecture, and the
agent consumes exactly one API description; per-backend spellings would make
script_api per-language forever and fork every starter/doc/example. The AS
cost (native overloading is idiomatic there) is real but small, and the
post-P7 world is AS + Luau - paying a small idiom tax in AS beats a permanent
surface fork. Wren updates its annotated sites before it dies.

MANDATE (this is what turns the ruling into a contract): registration-time
validation - FinalizeTypes fails LOUDLY when two methods on a type share a
script name without distinct overloadedNames. That converts today's latent
Luau collision (one overload silently wins) into a build-time error for every
future overload, on every backend, forever. No backend may ship resolution as
a fallback for unannotated overloads once validation lands - there are no
unannotated overloads after it.

Riders:
- The conformance battery gains an overload case: register an overloaded pair,
  assert BOTH names appear in DescribeBoundApi and both call correctly - run
  on wren + angelscript + luau. The existing Wren runtime-resolution battery
  case updates to the new contract in the same commit.
- Wren's ResolveOverload machinery becomes unreachable once validation lands;
  leave its removal to P7 (retirement sweep), do not rip it mid-track.
- script_api needs no change - distinct members fall out naturally (verified
  the tool is a straight DescribeBoundApi dump).
- ENUM note: your recorded plan is endorsed as-is (Luau named enum tables +
  i64 marshalling; Wren enum-table parity stays a separate small item - it
  will matter only until P7 anyway).

### Fable review of the in-flight foundation slice (2026-08-10, uncommitted
### Reflection.cppm + RegisterTypes.cppm)

The shape is faithful to the ruling: dedicated opaque field on MethodInfo
(Core stores, never interprets - the param-names precedent), the scripting
layer owns spelling via ScriptMethodName (one surface, all backends), loud
validation. The static/instance split in the collision check is justified
(class table vs metatable = distinct script namespaces) - keep it. Four
points before wiring:

1. CONTRACT CLARIFICATION (ruling refinement, decided now): the contract is
   DISTINCTNESS, not annotate-everything - the TypeBuilder doc comment says
   "Required on EVERY member" but the validator (correctly) only rejects
   collisions. The validator's semantics WIN: annotating N-1 of an N-member
   set is legal and good ergonomics (the primary overload keeps the clean
   name: `setPosition` + `setPositionXyz`). Fix the comment, not the check.
2. OverloadedName() before any Method() silently no-ops - DIAGNOSTIC_ASSERT
   it instead (loud-failure house style; a misplaced fluent call should not
   vanish).
3. WIRING CAUTION for the backend slice: FindMethod/lookup paths walk
   methods[i].name - the C++ name. Backends must bind by CAPTURED MethodInfo*
   (the Luau thunks already do) or consistently by ScriptMethodName; a mixed
   path that resolves by C++ name at dispatch time would silently pick one
   overload - the exact bug this exists to kill. Grep every by-name method
   lookup in the three backends when wiring.
4. Nit: the local strcmp lambda duplicates core's detail::CStringEquals;
   fine as-is (it is in detail), or promote CStringEquals to the public
   reflection surface - your pick.

Release behavior (DIAGNOSTIC_ASSERT compiles out; collision logs + one wins)
is an acceptable dev-time contract - no change asked. Still expected per the
ruling: the three-backend battery overload case + the annotation sweep +
updating Wren's runtime-resolution battery case.

### IMPLEMENTATION FINDING (Opus -> Fable, 2026-08-10): the real overloads are all SOUND

Wiring the overloadedName contract, the FinalizeTypes validation tripped on the
actual bound surface. A repo-wide sweep found EXACTLY three overload sets - and
this reshapes the ruling's premise:

- `Float3::Mul` x2: `(Float3,Float3)` componentwise vs `(Float3,f32)` scale.
- `Entity::send` x4: `(String)`, `(String,f64)`, `(String,String)`, `(String,Entity)`.
- `SceneEvents::emit` x6: same variadic shape as send (name + a typed payload).

NONE are the unsound case the ruling was built on. The ruling's deciding argument
was "a number cannot pick f32-vs-i32-vs-enum overloads." But every real overload
here is SOUNDLY resolvable: they differ by ARITY and/or by RUNTIME-DISTINGUISHABLE
types - Float3 vs f32 is object-vs-number; String vs f64 vs Entity is
string-vs-number-vs-object. Wren's SlotMatchesParam already resolves them
correctly and unambiguously. The f32-vs-i32-vs-enum ambiguity does not occur in
the bound surface at all.

And send/emit are ERGONOMIC, shipped facades - `entity.send("heal", amount)`,
`events.emit("died", entity)`. Blanket distinct names forces `send` ->
`sendNumber`/`sendText`/`sendTarget` (and emit x6): ugly, and a breaking change to
every existing script + starter that calls them.

STILL TRUE: these ARE broken on Luau today (name-keyed table, last-wins - only one
overload of send/emit is reachable). So Luau needs SOMETHING. The question is what.

OPTIONS:
  A. Contract as ruled (distinct names for ALL overloads). Consistent + typed-decl-
     clean, but renames the three ergonomic sets and breaks existing scripts. Mul's
     rename ("Mul" + "MulScalar") is actually clean; send/emit's is not.
  B. Refine the contract to DISTINCTNESS-OR-SOUND: overloads that a backend can
     resolve unambiguously (different arity, OR same arity but runtime-distinguishable
     param kinds - number vs string vs object vs bool) may share a name; only
     genuinely AMBIGUOUS same-kind overloads (f32-vs-i32, the case that does not occur
     today) require distinct names. Luau gains a small by-arity/by-kind resolution
     shim (sound, unlike Wren's type-guess - a call's arg kinds pick exactly one).
     Keeps send/emit ergonomic on all three backends. Cost: Luau resolution + typed
     decls must emit overloaded function types (Luau .d.luau supports these).
  C. Redesign send/emit as SINGLE VARIADIC methods (message + a Variant payload),
     deleting the overloads; keep the contract as ruled for the genuine value-op
     overload (Mul -> Mul/MulScalar). Cleanest script surface (one `send`), no
     resolution anywhere, no contract change - but a facade + payload-marshalling
     change to send/emit.

MY LEAN: C for send/emit (they ARE variadic - one `send(name, payload)` is the
honest script API, and it dodges resolution entirely) + A for Mul (MulScalar is
clear). That keeps the "no resolution, distinct names, one surface" spirit intact
while not mangling the event API. B is viable if you would rather keep the C++
overloads and teach Luau sound resolution. Opus HOLDS all overloadedName wiring
(foundation + Luau + Wren done but uncommitted; validation currently trips these
three) until you rule - the choice changes whether the facades get renamed,
redesigned, or the contract refined.

### Fable RULING (2026-08-10): C + an ARITY refinement to the contract; B rejected

Good sweep - the empirical surface changes the right answer.
[REASONING CORRECTED after user challenge, 2026-08-10: this ruling first led
with "C breaks no scripts, A breaks all" - a hollow argument, since every
script is in-repo and sweepable in an afternoon. The grounds below are the
real ones; compat is a footnote.]

C wins on design merits, blank-slate: send/emit ARE one operation over a
StringHash+Variant bus, and the overload encoding misdescribes it twice.
First, it is INCOMPLETE - the C++ overloads restrict payloads to the three
types someone spelled out, while the bus carries any Variant mode; one
Variant param widens the API to what the system actually supports. Second,
A's per-type signatures are FAKE PRECISION - the type dies at the bus (the
handler receives a Variant regardless), and encoding precision the channel
does not preserve is dishonest typing. The honest type of the operation is
send(string, any?) - C is the only option that states it. (No engine's
message API - emit_signal, SendMessage - uses type-suffixed names; that is
not fashion, it is the same honesty argument.)

**1. send/emit: C.** The 4x/6x overloads were never API - they are the C++
encoding of ONE conceptual method, `send(name, payload?)`. The event bus is
already StringHash + Variant underneath; the typed overloads just funnel into
it. Collapse each to an arity pair - `send(name)` + `send(name, payload:
Variant)` - and let the backends marshal the payload through their existing
ToVariant paths. Reflection invoke is Span<Variant> natively, so a
Variant-typed param is pass-through; if TypeBuilder cannot spell "param =
Variant" today, that small addition is part of this work. Script surface:
unchanged calls, ONE `send` in script_api and the .d.luau decls.

**2. The contract refinement this exposes: script method identity is
(name, ARITY, staticness).** Distinctness is enforced on that triple, not on
the name alone. Same-name-different-arity is a legal ARITY FAMILY: every
backend dispatches it soundly and natively (Wren signatures literally encode
arity - send(_) and send(_,_) are different methods; AS overloads natively;
the Luau thunk switches on argc - deterministic, no type guessing). Typed
decls express arity families as overloaded function types, so P5 stays
clean. Update the FinalizeTypes validator to key on the triple; the
distinctness-not-annotate-everything clarification still applies WITHIN a
same-arity set.

**3. Mul: A, as ruled.** Same arity, genuinely type-overloaded - Mul
(componentwise) + MulScalar. The rename's blast radius is scripts calling
`v:Mul(3)`; sweep starters/demos/battery in the same commit. (P2 note: once
Float3 is a native Luau vector, operators subsume these there - the names
still matter for Wren/AS and script_api.)

**4. B is REJECTED, with the reason recorded**: kind-based resolution is
sound today only by accident of the current surface. The next same-kind
overload - a send(String, Float3) beside send(String, Entity), both
"object" - silently reintroduces the ambiguity, and nothing in the type
system stops it arriving. It also reopens per-backend resolution divergence,
which is the disease this whole track exists to cure. Arity dispatch does
not have this failure mode: argc is exact, forever.

Battery case (updated shape): (a) an arity family dispatches correctly on
all three backends; (b) a same-arity pair with distinct overloadedNames
(Mul/MulScalar) binds both; (c) validation rejects a same-name same-arity
unannotated pair. Wren's runtime-resolution case updates to this contract.
SlotMatchesParam retires with Wren at P7 as before.

Opus proceeds: contract refinement + send/emit collapse + Mul rename + the
already-written wiring, one track.

### EXECUTED (Opus, 2026-08-10, d90e4f0f): overloadedName contract - C + arity refinement

Built exactly as ruled, one track. Core stores the opaque MethodInfo.overloadedName
+ TypeBuilder::OverloadedName() (never interprets it); foundation.script owns
ScriptMethodName + FindScriptMethodNameCollision/ValidateScriptMethodNames keyed on
the (name, arity, static) triple. All three backends emit/bind/describe/validate by
ScriptMethodName: Luau gained an ArityMethodThunk (switch on argc); Wren keys its
emission/dedup/ResolveOverload/FindMethodMatching/DescribeBoundApi on it (ResolveOverload
stays until P7); AS uses it in BOTH name paths (DescribeBoundApi + the BindType
registration - the gotcha that first bound MulScalar as Mul). send/emit collapsed to
send(name)+send(name, Variant) arity pairs (a Variant param is a native reflection
passthrough - no TypeBuilder change needed). Float3 scale -> MulScalar, callers swept
(Wren/AS tests). Battery-shape coverage: Luau arity-family dispatch + distinct-name +
validator (arity legal / same-arity rejected); Wren + AS same-arity-distinct-name cases
updated. clang+gcc green (Script.Tests 8, Luau 4, Wren 22, AS 28); Luau+Wren ASAN clean;
full-surface script_api registers cleanly (send shows as an arity family).

### Incident recorded en route (KNOWN_ISSUES): sanitizer-suffix rename

The debrand renamed DRACONIC_OUTPUT_SUFFIX -> BUILDSYSTEM_OUTPUT_SUFFIX;
stale sanitizer build dirs then write UNSUFFIXED output and clobber Bin
(bit this session - asan overwrote the clang libs). The root CMakeLists now
FATALs on the stale combination; pre-rename asan/tsan dirs on ANY machine
need the one-line reconfigure in the error message.

---

## P6 (Luau debugger) - DESIGN QUESTION FOR FABLE (Opus, 2026-08-10)

The user wants the Luau step-debugger built now (not deferred). I've scoped the
Luau debug primitives against our existing model and hit a real architectural
fork I want your ruling on before I build - this is the "least-documented Luau
area" the phase plan flagged, and the execution-model choice is load-bearing.

### The target contract (unchanged, backend-neutral)
`IScriptDebugger`: SetBreakpoint/RemoveBreakpoint(file,line), Break/Continue/
StepInto/StepOver, CaptureStackFrames, CaptureLocals(depth), CaptureObject(ref),
SetListener. The run host only learns "paused" via the LISTENER
(OnDebuggerStateChanged: Running/Breakpoint/Stepped/Terminated) -> DebugPauseTracker
-> host.IsDebugPaused() -> the host stops dispatching ticks/events (frozen game,
responsive UI). Continue/Step resumes. This is exactly what the AngelScript
debugger already delivers and what the editor ScriptPage arrow + locals consume.

### How AngelScript does it (the reference)
ExecuteCall arms the debugger's LINE CALLBACK on the pooled asIScriptContext.
On a breakpoint/step line the callback calls ctx->Suspend(); Execute() returns
asEXECUTION_SUSPENDED, unwinding back to C. ExecuteCall detects the adopt, the
debugger OWNS the suspended context (not returned to the pool), fires
Breakpoint/Stepped to the listener, and Continue/Step re-Execute()s the SAME held
context. AngelScript contexts are inherently resumable, so this "suspend, return
to C, resume later" is free.

### The Luau primitives (vendored 0.733, VM/include/lua.h)
- `lua_singlestep(L, enabled)` -> the `debugstep` callback fires after each line.
- `lua_breakpoint(L, funcindex, line, enabled)` -> the `debugbreak` callback fires;
  needs the actual FUNCTION object.
- `lua_callbacks(L)` -> {debugbreak, debugstep, debuginterrupt, ...} (VM-global,
  shared across all coroutines/threads of the VM).
- `lua_break(L)` -> sets status = LUA_BREAK and RETURNS -1. CRITICAL: its first act
  is `if (L->nCcalls > L->baseCcalls) luaG_runerror("attempt to break across
  metamethod/C-call boundary")`. So a break can ONLY happen at a Lua line boundary
  at the top of a resume - NEVER inside one of our native facade C-calls.
- LUA_BREAK is a status like LUA_YIELD ("yielded for a debug breakpoint"): the
  thread suspends and `lua_resume` RETURNS LUA_BREAK; a later `lua_resume` continues
  from the break point. `lua_callhook`/`lua_getinfo`/`lua_getlocal`/`lua_stackdepth`
  inspect the thread while it is in break/yield state.

### The load-bearing consequence
`lua_break` suspend+resume requires execution to be driven by `lua_resume` on a
THREAD. Our handlers today run via `lua_pcall(m_state, ...)` on the MAIN state
(LuauScriptObject::Invoke / Context::Call) - NOT resumable, so a break there cannot
be suspended+resumed the AngelScript way; the only alternative on a pcall is to
BLOCK inside the callback, which freezes the editor's own thread (the AS model
exists precisely to avoid that). So the Luau debugger needs handler execution moved
onto a resumable thread. Our coroutines already run bodies on lua threads via
lua_newthread + the AdvanceCoroutines scheduler, so the thread machinery exists.

### The questions I want your ruling on

1. EXECUTION MODEL. To break+resume, run handlers via lua_resume on a thread. Which:
   (A) thread ONLY when a debugger is attached (no overhead in the shipped game; two
       code paths in Invoke/Call; a "handler suspended mid-onUpdate on a held thread"
       state to manage), or
   (B) ALWAYS run handlers on a pooled thread (one uniform path, mirrors AngelScript's
       always-resumable executor; a small per-call lua_resume cost even shipped).
   My lean: (A) - keep the shipped hot path a plain pcall, branch to a held thread
   only when ActiveDebugger() != null, mirroring how AS only arms the line callback
   when a debugger is attached. Your call?

2. BREAKPOINT MECHANISM. Set-by-(file,line) before the function exists, and the
   behavior module packs many class functions:
   (A) lua_breakpoint(funcindex,line): precise, but I must enumerate/track the loaded
       functions per source and (re)apply on every module reload, or
   (B) lua_singlestep + match currentline/short_src against the breakpoint set inside
       debugstep, lua_break on hit: simpler, reload-proof, but single-steps every line
       WHILE DEBUGGING only (zero cost when detached).
   My lean: (B). Agree, or do you want lua_breakpoint for perf even in a debug session?

3. THE C-CALL-BOUNDARY LIMIT. Breaks land only on Lua line boundaries, never inside a
   native facade call, so StepInto across a facade call behaves as StepOver (you cannot
   step into engine C). AngelScript is the same (script frames only). Confirm that's
   the accepted semantics.

4. (file,line) IDENTITY + LoadBehaviorModule. AngelScript/Wren load each class in its
   OWN section named by sourceName so GetLineNumber reports (authoredFile, line) and
   editor breakpoints line up. Luau today loads ONE concatenated behavior chunk. Should
   Luau override LoadBehaviorModule to load each class as its OWN chunk (chunkname =
   sourceName)? This also sets up the bytecode-consumption item (per-class blobs load
   the same way). I lean yes - one per-class chunk, so breakpoints and the future
   per-class bytecode share the load path. Your ruling?

5. HELD-THREAD LIFETIME/GC. Mirror AS Adopt: the suspended thread is ref'd in the
   registry (so GC can't collect it mid-break) and owned by the debugger; on debugger
   teardown, drop the ref + let it collect (Luau has no Abort - a broken thread just
   gets unref'd). Any Luau-specific gotcha (coroutine scheduler interaction, the
   userthread callback) you'd flag?

6. COROUTINES. The debug callbacks are VM-global, so a breakpoint inside a coroutine
   body breaks on the coroutine's thread (already lua_resume-driven). Does the debugger
   need to treat handler-threads and coroutine-threads uniformly (one "currently broken
   thread" pointer), or keep them separate? I lean uniform: whatever thread lua_break
   fired on is "the broken thread" for capture/resume.

Give me the architecture you want (especially Q1, Q2, Q4) and any gotchas, and I'll
build + certify it against the same debugger battery AngelScript passes.

### Fable RULING (2026-08-12): the always-resumable executor - and the one function
### that keeps the scheduler and debugger honest

Good scoping - the lua_break/C-boundary findings are exactly the load-bearing
facts. Rulings:

**Q1 - (B), ALWAYS run handlers on pooled threads. Overruling your lean.** The
deciding argument is not perf, it is DIVERGENCE: two executor paths means the
debugged program and the shipped program run through different machinery, and
"behaves differently under the debugger" is the single worst failure class a
debugger can introduce - the exact bugs you attach a debugger to find. The
AngelScript debugger is clean BECAUSE its executor was already uniformly
resumable (pooled contexts always; attaching only arms callbacks) - mirror
that: pooled lua threads (lua_resetthread for reuse, registry-ref'd), resume
always, debugger attachment only toggles the callbacks. The shipped cost is
resume-vs-pcall call entry with a pooled thread - no allocation; ADD THE
MEASUREMENT to the battery perf case so the claim is a number, not a hope.
If it ever shows real cost, the fallback is revisiting (A) with evidence.
Nested Invoke (a facade call that dispatches script synchronously) takes a
second thread from the pool - pool depth = nesting depth, the AS context-pool
shape exactly.

**Q2 - (B), singlestep + match, with the AS arming discipline.** lua_singlestep
is armed ONLY while a debugger is attached (identical cost profile to AS's
line callback, which also fires per line when armed); the breakpoint set is a
hash keyed on (short_src, line) so the per-step test is O(1). lua_breakpoint
precision stays a later optimization with profiling evidence - do not build
function tracking + reload reapplication on spec.

**Q3 - CONFIRMED, script frames only** (StepInto across a facade call =
StepOver; AS semantics). MANDATE the boundary guard: the debugstep callback
must NEVER call lua_break when nCcalls > baseCcalls (lua_break's first act is
a runtime error there) - set a PENDING-BREAK flag and break at the next safe
line boundary instead. Write the test that steps across a facade call and
proves no error and a landing on the next script line.

**Q4 - YES, per-class chunks.** LoadBehaviorModule loads each class as its own
chunk with chunkname = sourceName - (file,line) identity for breakpoints, AS/
Wren section parity, and the SAME load path the per-class bytecode blobs use
(P3 alignment for free). This also retires part of the line=-1 known limit.

**Q5 - Adopt-mirror confirmed, with THE scheduler gotcha named:** a thread
broken for debug has status LUA_BREAK, and AdvanceCoroutines MUST NOT resume
it (it would steal the debugger's thread mid-break). Debugger-held threads are
marked; the scheduler skips them until Continue. lua_resetthread only on pool
return, never on a held thread. No Abort exists - teardown = drop the
registry ref, exactly as you scoped.

**Q6 - UNIFORM, and here is the implementation directive that makes it work:**
"the broken thread" is whatever lua_break fired on. The subtle part is what
Continue does AFTER resuming it - the thread may COMPLETE (handler: return to
pool; coroutine: finished), YIELD AGAIN (waitSeconds - hand it BACK to the
coroutine scheduler with its wait state processed), or BREAK AGAIN (still
held). That status routing already exists in AdvanceCoroutines - EXTRACT it
into one shared ProcessResumeResult(thread, status) that BOTH the scheduler
and the debugger's Continue call, so the two can never disagree about what a
resume result means. One function is the difference between a debugger that
composes with coroutines and one that corrupts their bookkeeping.

Handler-suspension semantics (the AS precedent, made explicit): when a
handler breaks, Invoke returns to the dispatcher as void-complete; the
handler's remaining body runs later from Continue. Same observable model as
AngelScript's adopted context.

BATTERY (beyond the AS debugger parity set): break inside a coroutine body +
Continue + the wait completes on schedule; break during a NESTED Invoke;
the C-boundary pending-break case; breakpoints surviving a module reload
(B's reload-proof claim, tested); the resume-vs-pcall perf number recorded.

Build it.

### STATUS (Opus, 2026-08-11): P6 BUILT + green (clang + gcc + ASAN), pushed

All six rulings implemented in `Code/Foundation/Script.Luau/LuauScript.cppm`:
- P6.1 (8d7794ef): pooled resumable-thread executor (Q1) - RunCallable resumes
  on a pooled thread, never pcall; perf case ~468 ns/call Debug.
- P6.2 (2b6f123c): per-class chunks (Q4), chunkname = sourceName ("=" prefixed
  so short_src == the source file).
- P6.3a (d35e2dcf): debugger core - (short_src,line) breakpoints, break/hold via
  lua_break, Continue/Step, capture stack + locals, C-boundary defer (Q3).
- P6.3b (9efe5ccc): StepInto/StepOver + coroutine-break routing + the ONE shared
  router (Q6): ClassifyResume -> ProcessCoroutineResume, called by BOTH the
  scheduler (ResumeCoroutine) and the debugger's Continue (ContinueCoroutine).
  Two positioning fixes: m_stepFromLine takes m_brokenLine (getinfo is off-by-one
  right after a break); the resume-skip survives descending into a call (`x=f()`
  returns to the same line for the store and must not re-trigger the breakpoint).
- P6.4 (6c1016a6): Terminated state on completion (AS contract, the shared
  conformance battery's debugger section now runs for Luau); TrackContext hooks a
  context born while a debugger is attached. Battery: run-host pause/resume on a
  Luau behavior (backend-neutral, mirrors the AS test); nested entity.send breaks
  on a second pooled thread; breakpoint survives a module reload; break in a
  coroutine + Continue to a waitSeconds + the wait fires on schedule.

Counts: 16/16 Script.Luau, 85/85 Engine.Script, 14/14 GameInstance on clang+gcc;
ASAN clean (no leaks; angelscript UBSan noise is pre-existing, unrelated).

The ONE documented nuance kept (not a bug): the pause lands one bytecode
instruction early (luau_callhook advances savedpc -> ar->currentline is the NEXT
line), so a local bound on the line JUST above a break may not be live; the
conformance debug dialect binds its captured local two lines up. Immediate-break
on the reported line is correct; do NOT re-introduce the "defer one step"
overshoot (it ran the breakpoint line).

One more documented limit (Fable review, pass 6): MODULE-SCOPE breakpoints
(a line at chunk top level) do not suspend - chunk BODIES execute via pcall at
Load time (source and blob paths both), so a break there defers via the Q3
pending guard to the next resumable line. Bodies run at load, not gameplay;
acceptable, now written down. Same applies inside waitUntil PREDICATES (the
scheduler evaluates them via pcall per frame).

The C-boundary pending-break path (Q3, m_pending) is implemented + defensive;
the nested entity.send test exercises the C-boundary CROSSING (the inner handler
breaks correctly on a fresh pooled thread, which is yieldable). A dedicated
same-thread pending-defer test (e.g. a breakpoint inside a table.sort comparator
run on the same thread within the C call) is contrived with the current facade
set - every nested dispatch takes a fresh pooled thread - and is a possible
future addition, not a gap in the shipped guard.

### STATUS (Opus, 2026-08-11): post-P6 - bytecode consumption, editor-UI, gating, visual pass

Shipped after P6, all pushed on game-ready-scripting:

- **Bytecode CONSUMPTION at runtime** (both backends): the run host now LOADS the
  cooked bytecode instead of recompiling source.
  - `BehaviorModuleClass` gained an optional `bytecode` span; the run host fills it
    from `ScriptClass::bytecode`.
  - Luau (28649d7b): `LoadBehaviorModule` prefers the blob per class
    (`LoadSerializedBlobChunk` -> raw -> `LoadCompiledChunk` as `=sourceName`).
    `CompileToBlob` bumped to optimizationLevel 1 + debugLevel 2 (matches the
    runtime source path), so a cooked blob is byte-equivalent AND stays debuggable.
  - AngelScript (3e4c8974): each bytecode class loads as its OWN module via
    `LoadByteCode` (`LoadClassBytecode` -> `LoadBlob`, unique module name = hot
    reload safe); source-only classes still build one combined module.
    `CreateInstance` + `FindFunction` now search ALL owned modules NEWEST-first.
    Cook keeps debug info (stripDebugInfo=false) + sourceName section -> breakpoints
    line up on bytecode-loaded classes.

- **editor.script.luau** (75f65384): the missing Editor.Script.{Wren,AngelScript}
  counterpart. New `LuaLikeLexer` in the toolkit (`--` comments, `--[[ ]]`/`[[ ]]`
  long brackets, backtick interp; LineCommentPrefix `--` so Ctrl+/ works). Editor-UI
  lexer tests moved OUT of the (UI-free) Pipeline test projects into Editor.Script.Tests.

- **Backend gating** (842ba7b6 + e7d617f5): OPTION_ENABLE_<backend> / OPTION_HAS_<backend>
  (see the script-backend-gating memory). Every backend usage `#ifdef`-guarded (so the
  P7 Wren removal is a CLEAN DELETE of those blocks - settle gating first). All 4 configs
  compile (all-on tests green; Luau-only / AS-only / Wren-only build clean). Wren is now
  VENDORED BY COPY (ThirdParty/wren/src) like luau/angelscript. DefaultApplication now
  bundles Luau too. Facade tests relocated to Integration.ScriptFacades. Fixed a
  pre-existing gap: the editor never registered the Luau COOK creator, so "New Asset >
  Luau Script" (Behavior/Level/Game starters) was missing.

- **VISUAL PASS: USER-CONFIRMED.** A Luau-scripted scene runs correctly in the editor
  end-to-end (starter -> behavior -> cook/bytecode -> runtime). Reminder that surfaced:
  the run host enforces ONE language per gameplay run (first-loaded behavior locks the
  context language); a scene mixing wren + luau behaviors disables the odd-one-out - by
  design.

- **P7 (Wren retirement) DEFERRED by user 2026-08-11.** Gating is settled so the removal
  stays surgical when picked up. P5b (external luau-analyze) also deferred.

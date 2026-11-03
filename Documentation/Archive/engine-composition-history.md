# Engine composition - one declaration per domain, every root a facet of it

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: BUILT 2026-09-27, all four phases (P1 032abd16, P2 cd508901, P3 84ac006f, P4 below).
> Approved the same day (user: "write the spec then execute it"). Sized L.
> Origin: the scene format reference (scene-format-reference.md P1) needed each factory's
> product and cooked types to name an asset, and found the stdio MCP host composes no
> factories at all. The discussion that followed (2026-09-27): "factories are registered, or
> should be registered, with their domains, which already go through a composition root. A
> domain reachable should imply its factory is. We don't go collecting factories as a group
> separate from the entry point setting up its composition root." Every citation below was
> checked against master at 7f700040. Read CONVENTIONS.md first.

## The problem as it stands

The engine has a real declarative composition for ONE facet of a domain and hand-kept lists
for every other facet:

- **Scene content is declared once.** Each domain exposes `Add<Domain>SceneManagers` and
  `Register<Domain>ComponentReflection`; Engine.SceneSurface pairs them into a `SceneModule`
  per domain and lists the thirteen modules once (`SceneSurfaceImpl.cpp:41-80`). Every scene
  consumer, runtime or headless, composes from that list.
- **Resource factories are a flat list in the runtime.** `DefaultApplication::
  RegisterStandardFactories` (`DefaultApplicationImpl.cpp:583-660`) constructs about two dozen
  factories across every domain, owns them as eighteen members
  (`DefaultApplication.cppm:274-311`), and gates one (`TextureFactory`) on the host's device.
  No subsystem and no scene module names a factory. `ShaderFactory` (needs a `ShaderSystem`)
  and `ImageFactory` are registered by no host at all; only their tests construct them.
- **Resource type registration is a second flat list**, in `DefaultApplication::OnStartup`
  (`DefaultApplicationImpl.cpp:545-554`, ten `Register*Resource` calls plus the scene document
  types) and again in `Pipeline.Registration` (`RegistrationImpl.cpp:102-134`).
- **The editor composes nothing and has a dead seam.** `EditorApplication::AddResourceFactory`
  (`ApplicationImpl.cpp:115`) has no caller; the editor's manager gets the runtime's list
  because the embedded `DefaultApplication` registers into it.
- **Four samples keep their own copies** (AnimatedCrowd `main.cpp:224-234`, Sandbox `:505`,
  AnimStressTest, ParticleFX), each with the device gate re-written by hand.
- **The script facades are a third root.** Engine.ScriptSurface lists thirteen registrars
  (`ScriptSurfaceImpl.cpp:44-56`) for the same domains Engine.SceneSurface lists, under its own
  count tripwire (`kSubsystemFacadeNameCount`).
- **The stdio MCP host has no factories**, so the scene format reference it serves names a
  reference's resource type but not the asset type an agent authors.

So "a domain is reachable" implies its scene managers and its reflection, and nothing else.

## Principle

A domain is declared once, in its own library, with every facet it contributes. A composition
root never collects anything; it lists the domains and answers for a facet. A consumer asks the
root for the facets it needs and only those. Where a facet needs something the domain cannot
know (a GPU device), the domain declares the need and the consumer supplies it.

## Decisions

### D1. A resource library declares a resource module: its types and its factory descriptions

`foundation.resource` gains three small types beside `IResourceFactory`:

- `ResourceFactoryDesc`: what a factory IS before one exists. `product()` and `cooked()` return
  the two `TypeInfo`s (the same pair `IResourceFactory::ProductType/CookedType` answer live),
  `requires()` returns the `TypeInfo` of a service the factory needs beyond an allocator (null
  for most; `rhi::Device` for the texture factory, `ShaderSystem` for the shader factory), and
  `create(IAllocator&, const IResourceServices&)` makes the factory, returning null when its
  required service is absent. Function pointers, no state: a description is a constant.
- `ResourceModule`: `id`, `registerTypes` (the library's existing `Register*Resource` function
  or null), and its `factories` span.
- Each of the nineteen `Foundation/*.Resource` libraries exposes `const ResourceModule&
  <Name>ResourceModule()`. That is where a factory belongs: with the resource it produces, not
  with an engine subsystem and not with an executable.

The description is data the scene format reference can read without constructing anything,
which is why the stdio host's join no longer needs a ResourceManager (D6).

### D2. Services are asked for by type

`IResourceServices` has one method, `Service(TypeId)`, and a typed `Get<T>()` over it; a
factory's `create` asks for what its `requires()` declares. Identity is `TypeOf<T>().id`, which
is process-single and stable across shared libraries (shared-libraries.md). The runtime's
implementation answers `rhi::Device` from the host's graphics device and `ShaderSystem` from
the render subsystem; a headless host passes `NoResourceServices`. No enum of capabilities, no
list to extend when a third service appears: the factory that needs it names it.

### D3. A factory set owns what a composition created

`ResourceFactorySet` owns the factories (`Array<UniquePtr<IResourceFactory>>`) and offers
`Create(Span<const ResourceModule*>, IAllocator&, const IResourceServices&)`,
`Register(ResourceManager&)`, `ForEach`, and `Skipped()`: the descriptions it could not create,
each with the service it required. `Create` is idempotent by product type, so a second call
with richer services fills the gaps and creates nothing twice. This replaces the runtime's
eighteen members, the editor's dead array and the samples' copies with one owner each.

### D4. An engine domain declares one module with every facet

A new small library, `Engine.Domain` (module `engine.domain`, links Foundation::Scene and
Foundation::Resource), holds `engine::DomainModule`: `id`, `dependsOn`, the scene facet (the
manager installer and the reflection registrar `SceneModule` carries today), the script
facade registrar, and the resource modules the domain brings. Each `Engine.<Domain>` library
declares `const DomainModule& <Domain>Domain()` in an implementation unit (one instance per
process, the shared-library rule), naming:

| domain | resource modules |
| --- | --- |
| render | geometry, model, materials, texture, image, shaders |
| animation | animation, property animation |
| particles | particles |
| physics | physics |
| terrain | terrain (Terrain, SplatWeights), heightfield |
| vegetation | vegetation |
| navigation | navigation |
| audio | audio |
| script | script |
| ui | ui, fonts |
| input | input |
| net, spline | none |

A domain with no scene content (input) is a module without a scene facet; a library that is
only a facade (Engine.UI.Script's `ui` screen facade, Engine.GameInstance's `run` facade) is a
module with only that facet. The `prefabs` module (Foundation's `PrefabSpawnSystem`, the scene
document types) has no engine library and is declared by the root (D5). Where a domain's
library does not yet link a resource library it now declares (render: geometry, model,
materials.resource, image, shaders.resource; animation: animation.resource; audio:
audio.resource; terrain: heightfield.resource; input: input.resource), the link is added: the
dependency was always real, the runtime carried it on the domain's behalf.

### D5. One root, `Engine.Composition`, with facets; the two surfaces fold into it

Engine.SceneSurface becomes `Engine.Composition` (module `engine.composition`) and absorbs
Engine.ScriptSurface. It lists the domain modules once and exposes `FullComposition()`:

- `Scene()`: the `SceneComposition` built from every module's scene facet (unchanged type, so
  `SceneSubsystem::SetComposition` and the plugin contributions keep working);
- `Modules()`, `ResourceModules()` (deduplicated by id, in domain order),
  `FactoryDescriptions()` (flattened);
- `RegisterResourceTypes()`, `RegisterReflection()`, `RegisterScriptFacades()`;
- `CreateFactories(ResourceFactorySet&, IAllocator&, const IResourceServices&)`.

The free functions every consumer calls today keep their names and move here:
`FullSceneComposition()`, `AddAllSceneManagers`, `RegisterAllSceneComponentReflection`,
`RegisterAllScriptFacades`, with `kSubsystemFacadeNameCount`. Consumers change an import and a
link line, nothing else. The two test suites merge into `Engine.Composition.Tests`.

### D6. Consumers compose from the root and only from it

- **DefaultApplication** owns one `ResourceFactorySet` and a `RuntimeResourceServices`;
  `RegisterStandardFactories` becomes `Create` from the composition's resource modules and
  `Register` into whichever manager the app uses; `OnStartup`'s type list becomes
  `RegisterResourceTypes()`. The eighteen members go. Behaviour change, deliberate: the image
  factory joins the standard set (it was never registered), and the shader factory joins it
  wherever a `ShaderSystem` is offered.
- **The editor** loses `AddResourceFactory` and `m_resourceFactories`; the embedded runtime's
  set is the editor's set, as it already was in practice.
- **The samples** that kept lists compose from the root's resource modules with a
  `ResourceFactorySet` of their own and the same services shape.
- **The scene format reference** joins through `FullComposition().FactoryDescriptions()` and
  the builders; `ProjectSession::resources` goes, and both hosts produce identical schemas
  with every asset type resolved.
- **Tools.Export, Tools.Cook, Tools.Mcp, Editor.Mcp, Editor.Scene**: import and link renames.
- **Pipeline.Registration** keeps its own type list: it registers ASSET types the engine never
  sees and stands above the engine. Folding its resource half into `RegisterResourceTypes()`
  is a follow-up for when the pipeline links the composition.

### D7. Subsystem construction stays where it is

`DefaultApplication` still adds its subsystems imperatively (`DefaultApplicationImpl.cpp:
187-236`): their constructors take different arguments and several exist only with a device.
Declaring subsystem construction is a facet the module CAN grow, but it is a separate spec with
its own questions (lifetimes, the device gate as a capability, the editor's embedded runtime).
Out of scope here, stated so nobody reads this spec as having done it.

### D8. Tripwires move to the root and become joins

- Composition: the module count (fourteen: thirteen scene modules plus input, plus the two
  facade-only modules), the scene composition still covering every manager the surface test
  checks today, the factory description count (27), every description's product and cooked
  types non-null and the cooked type a registered serializable after `RegisterResourceTypes()`.
- Factory set: `Create` with `NoResourceServices` makes 25 and skips exactly the two that
  declare a service, naming `rhi::Device` and `ShaderSystem`; with a services object that
  answers both, 27 and nothing skipped; a second `Create` adds nothing.
- Facades: `kSubsystemFacadeNameCount` unchanged, asserted in the merged suite.
- Runtime: attaching a manager registers the composition's headless set (25) with the font and
  terrain pins; with a device the texture factory too.
- Pipeline join (Engine.DefaultApp.Tests, pipeline present): every description's cooked type is
  some builder's product - the P0b tripwire, now over descriptions.
- Reference: the stdio-shaped host's schema resolves `mesh.mesh` to `StaticMeshAsset`; the
  "resource name alone" case is removed because it cannot arise.

## Phases

- **P1: the resource layer** (Foundation; M). `ResourceFactoryDesc`, `ResourceModule`,
  `IResourceServices` + `NoResourceServices`, `ResourceFactorySet` in `foundation.resource`
  with tests over a fake module (two descriptions, one requiring a service: skip, fill, no
  duplicate, register). Every `*.Resource` library declares its module.
- **P2: the domain modules and the root** (Engine; L). `Engine.Domain`; a `DomainModule` per
  engine library (D4); `Engine.Composition` replacing the two surfaces with its facets and
  merged tests; every consumer's import and link renamed. Green at the end of the commit with
  the runtime still on its old list.
- **P3: the consumers** (Engine, Editor, Samples; M). DefaultApplication on the factory set,
  the services, and `RegisterResourceTypes()`; the editor's dead seam removed; the four samples;
  the tripwires of D8 in their new places.
- **P4: the reference join** (Editor.Mcp; S). Descriptions instead of a ResourceManager;
  `ProjectSession::resources` removed; the tests flipped.

Each phase is one commit with its tests; the five lanes run at the end of the group.

## Acceptance

- No executable or library names a factory class except the resource library that owns it.
- One list of domains in the tree; the scene managers, the reflection, the resource types, the
  factories and the script facades are all answered from it.
- The stdio MCP host and the editor serve byte-identical scene schemas with every asset type
  resolved.
- Green on the four lanes plus ASAN over the touched suites.

## Found along the way

- `ShaderFactory` and `ImageFactory` exist, are tested, and are registered by no host. D6
  registers both through the composition (shader only where a `ShaderSystem` is offered).
- `RegisterUIComponentReflection` is called by hand in `DefaultApplication::OnStartup`
  (`:558`) although the composition's `RegisterReflection` already covers it.
- `Pipeline.Registration` duplicates the resource type list (D6, follow-up).

## As built (2026-09-27)

- Built as specified. Two shapes were added in the building: `ResourceServiceTable`
  (`Add<T>(&instance)`, the one `IResourceServices` every host needs) and the three constexpr
  description helpers (`FactoryWithAllocator`, `FactoryByDefault`, `FactoryWithService`) that
  cover the factories' three constructor shapes.
- A resource module is an `inline constexpr` table in its library's interface: a constant,
  duplicated per image without harm, and outside the shared-library tripwire (no Meyers
  accessor, no state). Domain modules are defined in implementation units as the spec said.
- `DomainModule` keeps `dependsOn` and hands it to the scene composition only; every other facet
  is order-free and stays in declaration order. No domain declares a dependency today.
- `RegisterAllScriptFacades` now registers the domain facades in domain order (render first)
  instead of the old hand list's order; the facade count is unchanged and the registries key by
  name. Geometry, animation, materials and shaders gained the type registrars they never had;
  the model registrar calls theirs. `ShaderFactory` takes an allocator like every other factory.
- The runtime creates the shader factory when the render subsystem's `ShaderSystem` exists at
  attach time and skips it otherwise; the set reports the skip. Nothing binds a `Ref<Shader>`
  through the manager yet.
- Sandbox asked its own texture-factory member whether textures could load; it now asks the
  manager (`HasFactory`), which is what it meant.

## For the Beef port (Sedulous)

The shape is the contract, not the code: a resource module per resource library with factory
descriptions that name their product, cooked type and required service; a domain module per
engine domain with scene, reflection, facade and resource facets; one root with facets; the
runtime, the editor and the tools composing from it; the tripwires of D8. Port after the
Editor.Project split and the scene format reference.

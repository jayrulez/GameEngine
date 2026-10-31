// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene tests: the scene editor's MCP tools over a real EditorContext holding pages that
// publish ISceneEditorPage (each backed by a headless scene and edit context) and one page that
// does not. They cover page addressing (the active page, an explicit page, a page that is not a
// scene), the selection round-trip with names and the primary, the refusals for unknown entities
// and pages, and the simulate control reflecting the page's state.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.input;
import foundation.shell;
import foundation.json;
import foundation.mcp;
import foundation.scene;
import foundation.resource;
import foundation.materials;
import engine.render;
import engine.script;
import foundation.script;
import foundation.script.resource;
import editor.core;
import editor.scene;
import editor.viewporttools;
import editor.camera;
import engine.navigation;     // NavMeshZoneComponent (navigation_bake)
import foundation.content;    // Instance (the zone asset)
import navigation.pipeline;   // NavigationZoneAsset
import engine.physics;        // RigidBodyComponent (the static ground navigation_bake collects)
import engine.animation;      // SkeletalAnimationComponent (a list of entity references)
import foundation.physics;    // MotionKind

using namespace foundation::core;
using namespace foundation::mcp;
using namespace editor;
namespace scene = foundation::scene;
namespace json = foundation::json;
using json::JsonValue;

namespace
{
    // A page the rest of the editor treats as a scene page: a headless scene and its edit
    // context, with the simulate state as flags. It has no UI and no viewport.
    class HeadlessScenePage final : public EditorPage, public ISceneEditorPage
    {
    public:
        HeadlessScenePage(StringView title, const Guid& asset)
            : EditorPage(DefaultAllocator()), m_title(title),
              m_scene(DefaultAllocator(), u8"headless"), m_edit(m_scene, Commands())
        {
            SetInstanceId(asset);
            Provide<ISceneEditorPage>(*this);
        }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override { return Status{}; }
        [[nodiscard]] SceneEditContext& EditContext() noexcept override { return m_edit; }
        void StartSimulation() override { m_simulating = true; }
        void StopSimulation() override { m_simulating = false; }
        void PauseSimulation(bool) override {}
        [[nodiscard]] bool IsSimulating() const noexcept override { return m_simulating; }
        [[nodiscard]] bool IsPaused() const noexcept override { return false; }
        [[nodiscard]] ViewportToolManager* ViewportTools() noexcept override { return nullptr; }
        [[nodiscard]] GizmoController* Gizmos() noexcept override { return nullptr; }
        [[nodiscard]] bool CameraOwnsInput() const noexcept override { return false; }
        [[nodiscard]] bool MarkersShown() const noexcept override { return true; }
        // The page pretends to have a viewport when `hasViewport` is set: the camera is real, and
        // the capture advances when the test reports the frame rendered (CompleteCapture or
        // FailCapture).
        [[nodiscard]] EditorCamera* ViewportCamera() noexcept override { return hasViewport ? &camera : nullptr; }
        // Records what was framed; the real page measures and moves the camera.
        bool FrameEntities(Span<const Guid> entities, bool ease) override
        {
            framed.Clear();
            for (const Guid& id : entities)
            {
                framed.PushBack(id);
            }
            framedEased = ease;
            return hasViewport && !entities.IsEmpty();
        }
        Array<Guid> framed{DefaultAllocator()};
        bool framedEased = true;
        [[nodiscard]] Status RequestProjectThumbnail() override { return Status{ErrorCode::NotSupported}; }
        [[nodiscard]] Status RequestViewportCapture(StringView path) override
        {
            if (!hasViewport)
            {
                return Status{ErrorCode::NotSupported};
            }
            capture = ViewportCapture{};
            capture.state = ViewportCaptureState::Pending;
            capture.path = String(path);
            ++captureRequests;
            return Status{};
        }
        [[nodiscard]] const ViewportCapture& LastViewportCapture() const noexcept override { return capture; }
        void CompleteCapture(u32 width, u32 height)
        {
            capture.state = ViewportCaptureState::Written;
            capture.width = width;
            capture.height = height;
        }
        void FailCapture() { capture.state = ViewportCaptureState::Failed; }
        // A playtest in the pretended viewport; the test moves its clock with AdvanceRun.
        bool BeginScriptedInput(UniquePtr<foundation::input::ScriptedInputSource> source) override
        {
            if (!hasViewport)
                return false;
            scripted = Move(source);
            scriptedTime = 0.0;
            runEnded = false;
            return true;
        }
        void EndScriptedInput() override { runEnded = true; }
        [[nodiscard]] f64 ScriptedInputTime() const noexcept override { return scripted ? scriptedTime : -1.0; }
        [[nodiscard]] Float2 ViewportPixels() const noexcept override { return Float2{800.0f, 600.0f}; }
        void AdvanceRun(f64 to)
        {
            scriptedTime = to;
            scripted->Advance(to);
        }
        UniquePtr<foundation::input::ScriptedInputSource> scripted;
        f64 scriptedTime = 0.0;
        bool runEnded = false;
        bool hasViewport = false;
        EditorCamera camera;
        ViewportCapture capture;
        u32 captureRequests = 0;
        void SetMarkersShown(bool) override {}
        [[nodiscard]] bool AnimationPanelShown() const noexcept override { return false; }
        void SetAnimationPanelShown(bool) override {}
        void EditAnimationClip(const Guid&, const Guid&) override {}
        void CreatePrefabFromEntity(const Guid&) override {}
        void PickAndSpawnPrefab(const Guid&) override {}
        void ApplyInstanceToPrefab(const Guid&) override {}
        void RevertInstance(const Guid&) override {}

    private:
        String m_title;
        scene::Scene m_scene;
        SceneEditContext m_edit; // over the page's own command stack, like the real page's
        bool m_simulating = false;
    };

    class PlainPage final : public EditorPage
    {
    public:
        explicit PlainPage(const Guid& asset) : EditorPage(DefaultAllocator())
        {
            SetInstanceId(asset);
        }
        [[nodiscard]] StringView Title() const override { return u8"a material"; }
        [[nodiscard]] Status Save() override { return Status{}; }
    };

    struct Answer
    {
        bool ok = false;
        JsonValue payload;
        String error;
    };
    Answer Call(McpServer& server, StringView tool, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"{}\",\"arguments\":{}}}}}",
                                   tool, argumentsJson);
        LineOutcome outcome = server.HandleLine(line.AsView());
        REQUIRE(outcome.state == LineState::Answered);
        JsonValue result = json::Parse(outcome.response.AsView()).value.Get(u8"result");
        Answer answer;
        answer.ok = !result.Get(u8"isError").AsBool();
        const String text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (answer.ok)
        {
            answer.payload = json::Parse(text.AsView()).value;
        }
        else
        {
            answer.error = text;
        }
        return answer;
    }

    String GuidText(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        return String(StringView(text, 36));
    }

    /// One pump of a tool that may ask to be re-entered: the raw line outcome.
    LineOutcome Pump(McpServer& server, StringView tool, StringView argumentsJson)
    {
        const String line = Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   u8"\"params\":{{\"name\":\"{}\",\"arguments\":{}}}}}",
                                   tool, argumentsJson);
        return server.HandleLine(line.AsView());
    }
    Answer AnswerOf(const LineOutcome& outcome)
    {
        REQUIRE(outcome.state == LineState::Answered);
        JsonValue result = json::Parse(outcome.response.AsView()).value.Get(u8"result");
        Answer answer;
        answer.ok = !result.Get(u8"isError").AsBool();
        const String text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (answer.ok)
        {
            answer.payload = json::Parse(text.AsView()).value;
        }
        else
        {
            answer.error = text;
        }
        return answer;
    }
}

TEST_CASE("scene-mcp-tools: page addressing, the selection round-trip, its refusals, and the "
          "simulate control")
{
    Random rng(7);
    const Guid sceneA = Guid::Generate(rng);
    const Guid sceneB = Guid::Generate(rng);
    const Guid material = Guid::Generate(rng);

    EditorContext context{DefaultAllocator()};
    auto* pageA = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro", sceneA), DefaultAllocator())));
    auto* pageB = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Menu", sceneB), DefaultAllocator())));
    EditorPage* plain = context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<PlainPage>(material), DefaultAllocator()));
    REQUIRE(context.OpenPages().Size() == 3u);
    const Guid lamp = pageA->EditContext().CreateEntity(u8"Lamp");
    const Guid table = pageA->EditContext().CreateEntity(u8"Table");
    // The other page's entity is created directly on its scene with its own guid, because a
    // page's CreateEntity also selects, and two fresh scenes may mint the same first guid.
    const Guid otherSceneEntity = Guid::Generate(rng);
    (void)pageB->EditContext().Scene().CreateEntity(otherSceneEntity, u8"Elsewhere");
    pageA->EditContext().EntitySelection().Set(Span<const Guid>{});

    McpServer server;
    RegisterSceneLiveTools(server, context);
    CHECK(server.ToolCount() == kSceneLiveToolCount);
    CHECK(kSceneLiveToolCount == 20u); // a tripwire: update it when a live tool is added or removed
    const String aGuid = GuidText(sceneA);
    const String lampGuid = GuidText(lamp);
    const String tableGuid = GuidText(table);

    // Default addressing uses the active page: first a scene page, then a page that is not one.
    context.SetActivePage(pageA);
    Answer got = Call(server, u8"selection_get", u8"{}");
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"page").Get(u8"title").AsString() == StringView(u8"Bistro"));
    CHECK(got.payload.Get(u8"entities").Count() == 0);
    CHECK(got.payload.Get(u8"primary").IsNull());
    context.SetActivePage(plain);
    got = Call(server, u8"selection_get", u8"{}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"page 'a material' is not a scene or prefab page"));
    // Explicit addressing reaches a scene page whatever is active; unknown pages refuse.
    got = Call(server, u8"selection_get", Format(u8"{{\"page\":\"{}\"}}", aGuid.AsView()).AsView());
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"page").Get(u8"title").AsString() == StringView(u8"Bistro"));
    got = Call(server, u8"selection_get", u8"{\"page\":\"00000000-0000-0000-0000-000000000000\"}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"no open page for guid"));

    // The selection round-trip keeps the order, makes the first entity the primary and resolves
    // names.
    Answer set = Call(server, u8"selection_set",
                      Format(u8"{{\"page\":\"{}\",\"entities\":[\"{}\",\"{}\"]}}", aGuid.AsView(),
                             tableGuid.AsView(), lampGuid.AsView())
                          .AsView());
    REQUIRE(set.ok);
    REQUIRE(set.payload.Get(u8"entities").Count() == 2);
    CHECK(set.payload.Get(u8"entities").At(0).Get(u8"name").AsString() == StringView(u8"Table"));
    CHECK(set.payload.Get(u8"entities").At(1).Get(u8"name").AsString() == StringView(u8"Lamp"));
    CHECK(set.payload.Get(u8"primary").AsString() == tableGuid.AsView());
    REQUIRE(pageA->EditContext().EntitySelection().Items().Size() == 2u);
    CHECK(*pageA->EditContext().EntitySelection().Primary() == table);
    CHECK(pageB->EditContext().EntitySelection().IsEmpty()); // the other page is untouched
    // An entity of another page's scene is refused for this page, and nothing changes.
    Answer wrong = Call(server, u8"selection_set",
                        Format(u8"{{\"page\":\"{}\",\"entities\":[\"{}\"]}}", aGuid.AsView(),
                               GuidText(otherSceneEntity).AsView())
                            .AsView());
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.error.AsView().StartsWith(u8"no entity with guid"));
    CHECK(pageA->EditContext().EntitySelection().Items().Size() == 2u);
    // An empty list clears.
    set = Call(server, u8"selection_set",
               Format(u8"{{\"page\":\"{}\",\"entities\":[]}}", aGuid.AsView()).AsView());
    REQUIRE(set.ok);
    CHECK(pageA->EditContext().EntitySelection().IsEmpty());

    // Simulate reflects the page's state and addresses the same way.
    Answer sim = Call(server, u8"simulate_start", Format(u8"{{\"page\":\"{}\"}}", aGuid.AsView()).AsView());
    REQUIRE(sim.ok);
    CHECK(sim.payload.Get(u8"simulating").AsBool());
    CHECK(pageA->IsSimulating());
    CHECK_FALSE(pageB->IsSimulating());
    sim = Call(server, u8"simulate_stop", Format(u8"{{\"page\":\"{}\"}}", aGuid.AsView()).AsView());
    REQUIRE(sim.ok);
    CHECK_FALSE(sim.payload.Get(u8"simulating").AsBool());
    CHECK_FALSE(pageA->IsSimulating());
    context.SetActivePage(plain);
    sim = Call(server, u8"simulate_start", u8"{}");
    CHECK_FALSE(sim.ok);

    context.ClosePage(plain);
    context.ClosePage(pageB);
    context.ClosePage(pageA);
}

TEST_CASE("scene-mcp-tools: entity_inspect reads an entity and its components through reflection - "
          "identity, hierarchy, transform, enums by name, references as guids, lists expanded, "
          "the primary selection as the default, and the refusals")
{
    engine::render::RegisterRenderComponentReflection();
    Random rng(21);
    const Guid sceneId = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro", sceneId), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();
    scene::Scene& scene = edit.Scene();
    auto* lights = scene.AddSystem<engine::render::LightComponentManager>();
    auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();

    const Guid lampId = edit.CreateEntity(u8"Lamp");
    const Guid bulbId = edit.CreateEntity(u8"Bulb", lampId);
    const scene::EntityHandle lamp = edit.Resolve(lampId);
    const scene::EntityHandle bulb = edit.Resolve(bulbId);
    engine::render::LightComponent& light = lights->Add(bulb);
    light.type = engine::render::LightType::Spot;
    light.intensity = 2.5f;
    light.color = Color{1.0f, 0.5f, 0.25f, 1.0f};
    engine::render::MeshComponent& mesh = meshes->Add(lamp);
    const Guid meshAsset = Guid::Generate(rng);
    const Guid materialAsset = Guid::Generate(rng);
    mesh.mesh.SetId(meshAsset);
    mesh.materials.PushBack(foundation::resource::Ref<foundation::materials::Material>{});
    mesh.materials[0].SetId(materialAsset);
    mesh.visible = false;
    Transform placed;
    placed.position = Float3{1.0f, 2.0f, 3.0f};
    scene.SetLocalTransform(lamp, placed);

    McpServer server;
    RegisterSceneLiveTools(server, context);
    const String pageGuid = GuidText(sceneId);

    // By guid: the lamp with its child, its transform, and the mesh's references and list.
    Answer got = Call(server, u8"entity_inspect",
                      Format(u8"{{\"page\":\"{}\",\"entity\":\"{}\"}}", pageGuid.AsView(),
                             GuidText(lampId).AsView())
                          .AsView());
    REQUIRE(got.ok);
    const JsonValue entity = got.payload.Get(u8"entity");
    CHECK(entity.Get(u8"name").AsString() == StringView(u8"Lamp"));
    CHECK(entity.Get(u8"active").AsBool());
    CHECK(entity.Get(u8"parent").IsNull());
    REQUIRE(entity.Get(u8"children").Count() == 1);
    CHECK(entity.Get(u8"children").At(0).AsString() == GuidText(bulbId).AsView());
    CHECK(entity.Get(u8"transform").Get(u8"position").At(2).AsNumber() == doctest::Approx(3.0));
    REQUIRE(entity.Get(u8"components").Count() == 1);
    const JsonValue meshJson = entity.Get(u8"components").At(0);
    CHECK(meshJson.Get(u8"type").AsString() == StringView(u8"mesh"));
    CHECK(meshJson.Get(u8"typeName").AsString() == StringView(u8"MeshComponent"));
    const JsonValue meshProps = meshJson.Get(u8"properties");
    CHECK(meshProps.Get(u8"mesh").AsString() == GuidText(meshAsset).AsView());
    CHECK_FALSE(meshProps.Get(u8"visible").AsBool());
    REQUIRE(meshProps.Get(u8"materials").Count() == 1);
    CHECK(meshProps.Get(u8"materials").At(0).AsString() == GuidText(materialAsset).AsView());

    // The primary selection is the default: the bulb, a child, with its light's enum by name.
    edit.EntitySelection().Set(bulbId);
    got = Call(server, u8"entity_inspect", Format(u8"{{\"page\":\"{}\"}}", pageGuid.AsView()).AsView());
    REQUIRE(got.ok);
    const JsonValue bulbJson = got.payload.Get(u8"entity");
    CHECK(bulbJson.Get(u8"parent").AsString() == GuidText(lampId).AsView());
    REQUIRE(bulbJson.Get(u8"components").Count() == 1);
    const JsonValue lightProps = bulbJson.Get(u8"components").At(0).Get(u8"properties");
    CHECK(lightProps.Get(u8"type").AsString() == StringView(u8"Spot"));
    CHECK(lightProps.Get(u8"intensity").AsNumber() == doctest::Approx(2.5));
    REQUIRE(lightProps.Get(u8"color").Count() == 4);
    CHECK(lightProps.Get(u8"color").At(1).AsNumber() == doctest::Approx(0.5));

    // Refusals: no selection and no entity; an unknown entity.
    edit.EntitySelection().Clear();
    got = Call(server, u8"entity_inspect", Format(u8"{{\"page\":\"{}\"}}", pageGuid.AsView()).AsView());
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"page 'Bistro' has no selection"));
    got = Call(server, u8"entity_inspect",
               Format(u8"{{\"page\":\"{}\",\"entity\":\"00000000-0000-0000-0000-000000000001\"}}",
                      pageGuid.AsView())
                   .AsView());
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"no entity '00000000-0000-0000-0000-000000000001' in page 'Bistro'"));

    context.ClosePage(page);
}

// The reads that entity_inspect over a running game and pie_run share: an entity found by guid,
// slash path or name, and a field path into its transform or a component's property, then into
// the value.
TEST_CASE("scene-mcp-tools: an entity found by guid, path or name, and its fields read by path")
{
    engine::render::RegisterRenderComponentReflection();
    scene::Scene scene(DefaultAllocator(), u8"fields");
    auto* lights = scene.AddSystem<engine::render::LightComponentManager>();
    const scene::EntityHandle lamp = scene.CreateEntity(u8"Lamp");
    const scene::EntityHandle bulb = scene.CreateEntity(u8"Bulb");
    scene.SetParent(bulb, lamp, false);
    Transform placed;
    placed.position = Float3{1.0f, 2.0f, 3.0f};
    scene.SetLocalTransform(lamp, placed);
    engine::render::LightComponent& light = lights->Add(bulb);
    light.type = engine::render::LightType::Spot;
    light.color = Color{1.0f, 0.5f, 0.25f, 1.0f};
    scene.UpdateTransforms(); // the world position comes from the last update, as in a running game

    utf8char text[37];
    scene.GetEntityId(bulb).ToChars(text);
    CHECK(FindEntity(scene, StringView(text, 36)) == bulb);
    CHECK(FindEntity(scene, u8"Lamp/Bulb") == bulb);
    CHECK(FindEntity(scene, u8"Bulb") == bulb);
    CHECK_FALSE(FindEntity(scene, u8"Ghost").IsAssigned());

    auto read = [&](scene::EntityHandle e, StringView path) { return EntityFieldJson(scene, e, u8"it", path); };
    CHECK(read(lamp, u8"position.y").Value().AsNumber() == doctest::Approx(2.0));
    CHECK(read(bulb, u8"worldPosition.z").Value().AsNumber() == doctest::Approx(3.0));
    CHECK(read(lamp, u8"scale").Value().Count() == 3);
    CHECK(read(lamp, u8"rotation.w").Value().AsNumber() == doctest::Approx(1.0));
    CHECK(read(bulb, u8"active").Value().AsBool());
    // A component by its serialization id or its type name, then into the value.
    CHECK(read(bulb, u8"light.type").Value().AsString() == StringView(u8"Spot"));
    CHECK(read(bulb, u8"LightComponent.color.1").Value().AsNumber() == doctest::Approx(0.5));
    CHECK(read(bulb, u8"light.color.y").Value().AsNumber() == doctest::Approx(0.5));

    auto refused = read(lamp, u8"light.intensity");
    REQUIRE_FALSE(refused.HasValue());
    CHECK(refused.Error().AsView().StartsWith(u8"entity 'it' has no field 'light.intensity'"));
    refused = read(lamp, u8"position.q");
    REQUIRE_FALSE(refused.HasValue());
    CHECK(refused.Error().AsView() == StringView(u8"'position.q' of entity 'it': nothing at 'q'"));
}

// Every scene tool takes an entity by guid, name or slash path. A Script component lists its
// behaviours (a hidden list the reflected properties leave out), and each override is named by
// its class's name for it, or by hash when the class is not available.
TEST_CASE("scene-mcp-tools: an entity by name or path, and a Script component's behaviours")
{
    Random rng(33);
    EditorContext context{DefaultAllocator()};
    const Guid arenaId = Guid::Generate(rng);
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Arena", arenaId), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();
    const String arena = GuidText(arenaId);
    auto* scripts = edit.Scene().AddSystem<engine::script::ScriptComponentManager>();
    const Guid player = edit.CreateEntity(u8"Player");
    const Guid weapon = edit.CreateEntity(u8"Weapon", player);
    engine::script::ScriptComponent& component = scripts->Add(edit.Resolve(weapon));
    engine::script::ScriptBehavior behavior;
    const Guid script = Guid::Generate(rng);
    behavior.script.SetId(script);
    foundation::script::ScriptPropertyValue speed;
    speed.kind = foundation::script::ScriptPropertyType::Float;
    speed.number = 4.0;
    behavior.SetOverride(foundation::script::ScriptPropertyNameHash(u8"speed"), speed);
    component.behaviors.PushBack(Move(behavior));

    McpServer server;
    RegisterSceneLiveTools(server, context);
    Answer got = Call(server, u8"entity_inspect",
                      Format(u8"{{\"page\":\"{}\",\"entity\":\"Player/Weapon\"}}", arena.AsView()).AsView());
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"entity").Get(u8"name").AsString() == StringView(u8"Weapon"));
    const JsonValue scriptJson = got.payload.Get(u8"entity").Get(u8"components").At(0);
    const JsonValue behaviors = scriptJson.Get(u8"behaviors");
    REQUIRE(behaviors.Count() == 1);
    CHECK(behaviors.At(0).Get(u8"script").AsString() == GuidText(script).AsView());
    CHECK(behaviors.At(0).Get(u8"class").IsNull()); // not cooked here
    CHECK(behaviors.At(0).Get(u8"enabled").AsBool());
    const JsonValue properties = behaviors.At(0).Get(u8"properties");
    REQUIRE(properties.Count() == 1);
    CHECK(properties.Items()[0].AsNumber() == doctest::Approx(4.0)); // named by hash (#n), not cooked

    got = Call(server, u8"entity_inspect",
               Format(u8"{{\"page\":\"{}\",\"entity\":\"Player\"}}", arena.AsView()).AsView());
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"entity").Get(u8"name").AsString() == StringView(u8"Player"));
    got = Call(server, u8"entity_inspect",
               Format(u8"{{\"page\":\"{}\",\"entity\":\"Ghost\"}}", arena.AsView()).AsView());
    CHECK(got.error.AsView().StartsWith(u8"no entity 'Ghost' in page 'Arena'"));
    context.ClosePage(page);
}

namespace
{
    // A running behaviour's instance: two fields, `speed` and the private `m_hits`.
    class FakeInstance final : public foundation::script::ScriptObject
    {
    public:
        [[nodiscard]] Result<Variant> Invoke(StringView, Span<Variant>) override
        {
            return Err(ErrorCode::NotFound);
        }
        [[nodiscard]] Result<Variant> GetProperty(StringView name) override
        {
            if (name == u8"speed")
            {
                return Variant::From<f64>(3.5);
            }
            if (name == u8"m_hits")
            {
                return Variant::From<f64>(2.0);
            }
            return Err(ErrorCode::NotFound);
        }
    };
}

// A running behaviour's state reads by `<BehaviorClass>.<field>`, private fields included, and
// entity_inspect lists each running behaviour's values under `live`.
TEST_CASE("scene-mcp-tools: a running behaviour's fields read by class name, and listed as live")
{
    scene::Scene level(DefaultAllocator(), u8"run");
    auto* scripts = level.AddSystem<engine::script::ScriptComponentManager>();
    const scene::EntityHandle hero = level.CreateEntity(u8"Hero");
    RefPtr<foundation::script::ScriptClass> mover = MakeRef<foundation::script::ScriptClass>(DefaultAllocator());
    mover->className = String(u8"Mover");
    foundation::script::ScriptPropertyDesc speed;
    speed.name = String(u8"speed");
    speed.hash = foundation::script::ScriptPropertyNameHash(u8"speed");
    speed.type = foundation::script::ScriptPropertyType::Float;
    mover->properties.PushBack(Move(speed));
    engine::script::ScriptBehavior behavior;
    behavior.script.SetDirect(mover);
    behavior.boundClass = mover.Get();
    behavior.instance = MakeRef<FakeInstance>(DefaultAllocator());
    scripts->Add(hero).behaviors.PushBack(Move(behavior));

    CHECK(EntityFieldJson(level, hero, u8"Hero", u8"Mover.speed").Value().AsNumber() == doctest::Approx(3.5));
    CHECK(EntityFieldJson(level, hero, u8"Hero", u8"Mover.m_hits").Value().AsNumber() == doctest::Approx(2.0));
    auto other = EntityFieldJson(level, hero, u8"Hero", u8"Jumper.speed");
    REQUIRE_FALSE(other.HasValue());
    CHECK(other.Error().AsView().ContainsIgnoreCase(u8"<BehaviorClass>.<field>"));

    const JsonValue entity = EntityJson(level, hero);
    const JsonValue live = entity.Get(u8"components").At(0).Get(u8"behaviors").At(0).Get(u8"live");
    CHECK(live.Get(u8"speed").AsNumber() == doctest::Approx(3.5));
}

TEST_CASE("scene-mcp-tools: component_set writes one property through the undo path - leaves, an "
          "enum by name, a reference by guid - one locked step per call that Undo takes back, "
          "and the refusals leave nothing behind")
{
    engine::render::RegisterRenderComponentReflection();
    Random rng(33);
    const Guid sceneId = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro", sceneId), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();
    scene::Scene& scene = edit.Scene();
    auto* lights = scene.AddSystem<engine::render::LightComponentManager>();
    auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();
    const Guid lampId = edit.CreateEntity(u8"Lamp");
    const scene::EntityHandle lamp = edit.Resolve(lampId);
    engine::render::LightComponent& light = lights->Add(lamp);
    light.intensity = 1.0f;
    engine::render::MeshComponent& mesh = meshes->Add(lamp);
    (void)mesh;
    page->ClearDirty();
    edit.Commands().Clear();

    McpServer server;
    RegisterSceneLiveTools(server, context);
    const String pageGuid = GuidText(sceneId);
    const String lampGuid = GuidText(lampId);
    const auto set = [&](StringView component, StringView property, StringView valueJson)
    {
        return Call(server, u8"component_set",
                    Format(u8"{{\"page\":\"{}\",\"entity\":\"{}\",\"component\":\"{}\",\"property\":\"{}\","
                           u8"\"value\":{}}}",
                           pageGuid.AsView(), lampGuid.AsView(), component, property, valueJson)
                        .AsView());
    };

    // A float, by the component's serialization id. The page is dirty afterwards, and the value
    // reads back as entity_inspect shows it.
    Answer got = set(u8"light", u8"intensity", u8"2.5");
    REQUIRE(got.ok);
    CHECK(lights->Get(lamp)->intensity == doctest::Approx(2.5f));
    CHECK(got.payload.Get(u8"value").AsNumber() == doctest::Approx(2.5));
    CHECK(got.payload.Get(u8"component").AsString() == StringView(u8"light"));
    CHECK(page->IsDirty());
    // An enum by name, with the component named by its type; a color as four numbers; a bool.
    REQUIRE(set(u8"LightComponent", u8"type", u8"\"Spot\"").ok);
    CHECK(lights->Get(lamp)->type == engine::render::LightType::Spot);
    REQUIRE(set(u8"light", u8"color", u8"[0.1,0.2,0.3,1]").ok);
    CHECK(lights->Get(lamp)->color.g == doctest::Approx(0.2f));
    REQUIRE(set(u8"mesh", u8"visible", u8"false").ok);
    CHECK_FALSE(meshes->Get(lamp)->visible);
    // A reference by guid. A headless page has no resource manager, so only the id is written.
    const Guid meshAsset = Guid::Generate(rng);
    got = set(u8"mesh", u8"mesh", Format(u8"\"{}\"", GuidText(meshAsset).AsView()).AsView());
    REQUIRE(got.ok);
    CHECK(meshes->Get(lamp)->mesh.id == meshAsset);
    CHECK(got.payload.Get(u8"value").AsString() == GuidText(meshAsset).AsView());

    // Five writes are five undo steps: each Undo reverts exactly one, the reference first.
    REQUIRE(edit.Commands().CanUndo());
    edit.Commands().Undo();
    CHECK(meshes->Get(lamp)->mesh.id.IsNil());
    CHECK_FALSE(meshes->Get(lamp)->visible); // the previous step still stands
    edit.Commands().Undo();
    CHECK(meshes->Get(lamp)->visible);
    edit.Commands().Undo();
    CHECK(lights->Get(lamp)->color.g == doctest::Approx(1.0f));
    edit.Commands().Undo();
    CHECK(lights->Get(lamp)->type == engine::render::LightType::Directional);
    edit.Commands().Undo();
    CHECK(lights->Get(lamp)->intensity == doctest::Approx(1.0f));
    CHECK_FALSE(edit.Commands().CanUndo());
    // Two writes of the same property are still two steps: the user's scrubs merge, but an
    // agent's calls do not.
    REQUIRE(set(u8"light", u8"intensity", u8"3").ok);
    REQUIRE(set(u8"light", u8"intensity", u8"4").ok);
    edit.Commands().Undo();
    CHECK(lights->Get(lamp)->intensity == doctest::Approx(3.0f));
    edit.Commands().Undo();
    CHECK(lights->Get(lamp)->intensity == doctest::Approx(1.0f));

    // Refusals, each leaving the value and the stack as they were.
    const i64 stackBefore = edit.Commands().UndoIndex();
    got = set(u8"light", u8"intensity", u8"\"bright\"");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'intensity' of 'light' takes a number"));
    got = set(u8"light", u8"type", u8"\"Laser\"");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'type' takes one of: Directional, Point, Spot"));
    got = set(u8"light", u8"brightness", u8"1");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"component 'light' has no property 'brightness'"));
    got = set(u8"physics.RigidBody", u8"mass", u8"1");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"entity 'Lamp' has no reflected component"));
    got = set(u8"mesh", u8"materials", u8"\"one\"");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'materials' is a list - `value` is an array"));
    got = set(u8"mesh", u8"materials", u8"[\"not-a-guid\"]");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"element 0 of 'materials' is not an asset guid"));
    got = set(u8"mesh", u8"mesh", u8"\"not-a-guid\"");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'mesh' is a reference"));
    CHECK(edit.Commands().UndoIndex() == stackBefore);
    CHECK(lights->Get(lamp)->intensity == doctest::Approx(1.0f));
    // Simulating locks the edits.
    page->StartSimulation();
    got = set(u8"light", u8"intensity", u8"9");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"page 'Bistro' is simulating"));
    page->StopSimulation();
    CHECK(lights->Get(lamp)->intensity == doctest::Approx(1.0f));

    context.ClosePage(page);
}


namespace
{
    // A component with the shapes the render components do not offer: a string, a vector, and
    // a stored field the type publishes read-only.
    enum class PlaqueMood : i32
    {
        Calm = 0,
        Loud = 1,
        Wild = 5 // non-contiguous: a number names the enumerator's value, not its index
    };
    struct PlaqueComponent
    {
        String text{u8"untitled"};
        Float3 offset{0, 0, 0};
        Quaternion rotation{0, 0, 0, 1};
        scene::EntityRef target;
        PlaqueMood mood = PlaqueMood::Calm;
        i32 serial = 7;
    };
    class PlaqueManager final : public scene::ComponentManager<PlaqueComponent>
    {
    };
}

REFLECT_ENUM(PlaqueMood, "rtti::editor::scene::test")
{
    builder.Value("Calm", PlaqueMood::Calm);
    builder.Value("Loud", PlaqueMood::Loud);
    builder.Value("Wild", PlaqueMood::Wild);
}

REFLECT_VALUE(PlaqueComponent, "rtti::editor::scene::test")
{
    builder.Property<&PlaqueComponent::text>("text")
        .Property<&PlaqueComponent::offset>("offset")
        .Property<&PlaqueComponent::rotation>("rotation")
        .Property<&PlaqueComponent::target>("target")
        .Property<&PlaqueComponent::mood>("mood")
        .Property<&PlaqueComponent::serial>("serial", PropertyFlags::ReadOnly);
}

// component_set writes whole lists: an imported model's animator feeds its skinned mesh through
// a list of entity references, and a vegetation layer's materials are a list.
TEST_CASE("scene-mcp-tools: component_set writes a whole list - references by guid, entity references "
          "- in one undo step, and entity_inspect reads a list of entity references")
{
    engine::render::RegisterRenderComponentReflection();
    engine::animation::RegisterAnimationComponentReflection();
    Random rng(41);
    const Guid sceneId = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Rider", sceneId), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();
    scene::Scene& scene = edit.Scene();
    auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();
    auto* animators = scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    const Guid riderId = edit.CreateEntity(u8"Rider");
    const Guid bodyId = edit.CreateEntity(u8"Body");
    const scene::EntityHandle rider = edit.Resolve(riderId);
    (void)meshes->Add(rider);
    (void)animators->Add(rider);
    edit.Commands().Clear();

    McpServer server;
    RegisterSceneLiveTools(server, context);
    const String pageGuid = GuidText(sceneId);
    const String riderGuid = GuidText(riderId);
    const auto set = [&](StringView component, StringView property, StringView valueJson)
    {
        return Call(server, u8"component_set",
                    Format(u8"{{\"page\":\"{}\",\"entity\":\"{}\",\"component\":\"{}\",\"property\":\"{}\","
                           u8"\"value\":{}}}",
                           pageGuid.AsView(), riderGuid.AsView(), component, property, valueJson)
                        .AsView());
    };

    // A list of references: two material guids and an empty slot, read back as written.
    const Guid bark = Guid::Generate(rng);
    const Guid needles = Guid::Generate(rng);
    Answer got = set(u8"mesh", u8"materials",
                     Format(u8"[\"{}\", null, \"{}\"]", GuidText(bark).AsView(), GuidText(needles).AsView())
                         .AsView());
    REQUIRE(got.ok);
    REQUIRE(meshes->Get(rider)->materials.Size() == 3u);
    CHECK(meshes->Get(rider)->materials[0].id == bark);
    CHECK(meshes->Get(rider)->materials[1].id.IsNil());
    CHECK(meshes->Get(rider)->materials[2].id == needles);
    REQUIRE(got.payload.Get(u8"value").Count() == 3);
    CHECK(got.payload.Get(u8"value").At(2).AsString() == GuidText(needles).AsView());
    // A shorter list shrinks it.
    REQUIRE(set(u8"mesh", u8"materials", Format(u8"[\"{}\"]", GuidText(needles).AsView()).AsView()).ok);
    REQUIRE(meshes->Get(rider)->materials.Size() == 1u);
    CHECK(meshes->Get(rider)->materials[0].id == needles);

    // A list of entity references, which entity_inspect reads back.
    got = set(u8"skeletal_animation", u8"meshEntities",
              Format(u8"[\"{}\"]", GuidText(bodyId).AsView()).AsView());
    REQUIRE(got.ok);
    REQUIRE(animators->Get(rider)->meshEntities.Size() == 1u);
    CHECK(animators->Get(rider)->meshEntities[0].id == bodyId);
    CHECK(got.payload.Get(u8"value").At(0).AsString() == GuidText(bodyId).AsView());
    // The same with EntityRef reflected, as the editor's inspector reflects it for its slot.
    scene::RegisterEntityRefReflection();
    got = set(u8"skeletal_animation", u8"meshEntities", Format(u8"[\"{}\"]", GuidText(bodyId).AsView()).AsView());
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"value").At(0).AsString() == GuidText(bodyId).AsView());
    edit.Commands().Undo(); // undo this write's step so the counts below are unchanged

    // A list of structures (foot IK's legs): each element is an object of the fields it sets,
    // and the rest keep their defaults.
    auto* feet = scene.AddSystem<engine::animation::FootIkComponentManager>();
    (void)feet->Add(rider);
    const i64 stackBeforeLegs = edit.Commands().UndoIndex();
    got = set(u8"foot_ik", u8"legs",
              u8"[{\"startBone\":\"UpperLeg.L\",\"midBone\":\"LowerLeg.L\",\"endBone\":\"Foot.L\"},"
              u8"{\"startBone\":\"UpperLeg.R\",\"midBone\":\"LowerLeg.R\",\"endBone\":\"Foot.R\","
              u8"\"hingeAxis\":[1,0,0]}]");
    REQUIRE(got.ok);
    REQUIRE(feet->Get(rider)->legs.Size() == 2u);
    CHECK(feet->Get(rider)->legs[0].midBone == u8"LowerLeg.L");
    CHECK(feet->Get(rider)->legs[0].hingeAxis.x == 0.0f); // not named, so it keeps its default
    CHECK(feet->Get(rider)->legs[1].endBone == u8"Foot.R");
    CHECK(feet->Get(rider)->legs[1].hingeAxis.x == 1.0f);
    CHECK(got.payload.Get(u8"value").Count() == 2);
    // A field the structure lacks, or a shape a field cannot take, is refused and writes nothing.
    got = set(u8"foot_ik", u8"legs", u8"[{\"thigh\":\"UpperLeg.L\"}]");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"element 0 of 'legs' has no field 'thigh' (its fields: startBone"));
    got = set(u8"foot_ik", u8"legs", u8"[{\"hingeAxis\":\"up\"}]");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"element 0 of 'legs': 'hingeAxis' takes [x, y, z]"));
    got = set(u8"foot_ik", u8"legs", u8"[\"UpperLeg.L\"]");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"element 0 of 'legs' is not an object"));
    CHECK(feet->Get(rider)->legs.Size() == 2u);
    // The write was one step: one Undo takes it back to where the stack stood before it.
    edit.Commands().Undo();
    CHECK(feet->Get(rider)->legs.IsEmpty());
    CHECK(edit.Commands().UndoIndex() == stackBeforeLegs);

    // One undo step per call: the entity list, then the shrink, then the first write.
    edit.Commands().Undo();
    CHECK(animators->Get(rider)->meshEntities.IsEmpty());
    edit.Commands().Undo();
    CHECK(meshes->Get(rider)->materials.Size() == 3u);
    edit.Commands().Undo();
    CHECK(meshes->Get(rider)->materials.IsEmpty());
    CHECK_FALSE(edit.Commands().CanUndo());

    context.ClosePage(page);
}

TEST_CASE("scene-mcp-tools: component_set writes a string, a vector, a quaternion, an entity reference "
          "(set and cleared), an enum by number, clears a reference with null, and refuses a "
          "read-only property before anything changes")
{
    RttiRegisterEnum_PlaqueMood();
    RttiRegisterValue_PlaqueComponent();
    engine::render::RegisterRenderComponentReflection();
    Random rng(34);
    const Guid sceneId = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro", sceneId), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();
    scene::Scene& scene = edit.Scene();
    auto* plaques = scene.AddSystem<PlaqueManager>();
    auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();
    const Guid signId = edit.CreateEntity(u8"Sign");
    const Guid postId = edit.CreateEntity(u8"Post");
    const scene::EntityHandle sign = edit.Resolve(signId);
    plaques->Add(sign);
    const Guid meshAsset = Guid::Generate(rng);
    meshes->Add(sign).mesh.id = meshAsset;
    page->ClearDirty();
    edit.Commands().Clear();

    McpServer server;
    RegisterSceneLiveTools(server, context);
    const String pageGuid = GuidText(sceneId);
    const String signGuid = GuidText(signId);
    const auto set = [&](StringView component, StringView property, StringView valueJson)
    {
        return Call(server, u8"component_set",
                    Format(u8"{{\"page\":\"{}\",\"entity\":\"{}\",\"component\":\"{}\",\"property\":\"{}\","
                           u8"\"value\":{}}}",
                           pageGuid.AsView(), signGuid.AsView(), component, property, valueJson)
                        .AsView());
    };

    // A string, by the component's type name (a plain manager has no serialization id).
    Answer got = set(u8"PlaqueComponent", u8"text", u8"\"Open late\"");
    REQUIRE(got.ok);
    CHECK(plaques->Get(sign)->text == u8"Open late");
    CHECK(got.payload.Get(u8"value").AsString() == StringView(u8"Open late"));
    // A vector as three numbers.
    got = set(u8"PlaqueComponent", u8"offset", u8"[1,2,3]");
    REQUIRE(got.ok);
    CHECK(plaques->Get(sign)->offset.z == doctest::Approx(3.0f));
    CHECK(got.payload.Get(u8"value").Count() == 3);
    // null clears a reference.
    got = set(u8"mesh", u8"mesh", u8"null");
    REQUIRE(got.ok);
    CHECK(meshes->Get(sign)->mesh.id.IsNil());
    CHECK(got.payload.Get(u8"value").IsNull());
    CHECK(edit.Commands().CanUndo());
    CHECK(page->IsDirty());
    // A quaternion as four numbers.
    got = set(u8"PlaqueComponent", u8"rotation", u8"[0,0.7071068,0,0.7071068]");
    REQUIRE(got.ok);
    CHECK(plaques->Get(sign)->rotation.y == doctest::Approx(0.7071068f));
    // An entity reference by the entity's guid, then cleared with null.
    got = set(u8"PlaqueComponent", u8"target", Format(u8"\"{}\"", GuidText(postId).AsView()).AsView());
    REQUIRE(got.ok);
    CHECK(plaques->Get(sign)->target.id == postId);
    CHECK(got.payload.Get(u8"value").AsString() == GuidText(postId).AsView());
    got = set(u8"PlaqueComponent", u8"target", u8"null");
    REQUIRE(got.ok);
    CHECK(plaques->Get(sign)->target.IsNil());
    // An enum by number: the number is the enumerator's value (Wild = 5), read back by name.
    got = set(u8"PlaqueComponent", u8"mood", u8"5");
    REQUIRE(got.ok);
    CHECK(plaques->Get(sign)->mood == PlaqueMood::Wild);
    CHECK(got.payload.Get(u8"value").AsString() == StringView(u8"Wild"));

    // A read-only field is refused by name before any undo group opens; the read-only flag
    // decides it.
    const i64 stackBefore = edit.Commands().UndoIndex();
    got = set(u8"PlaqueComponent", u8"serial", u8"9");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'serial' of 'PlaqueComponent' is read-only"));
    CHECK(plaques->Get(sign)->serial == 7);
    // A wrong shape for these field types is refused with a message naming what they take.
    got = set(u8"PlaqueComponent", u8"text", u8"5");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'text' of 'PlaqueComponent' takes a string"));
    got = set(u8"PlaqueComponent", u8"offset", u8"[1,2]");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'offset' of 'PlaqueComponent' takes "));
    got = set(u8"PlaqueComponent", u8"target", u8"\"not-a-guid\"");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'target' is an entity reference"));
    got = set(u8"PlaqueComponent", u8"mood", u8"2"); // no enumerator has the value 2
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"property 'mood' takes one of: Calm, Loud, Wild"));
    CHECK(edit.Commands().UndoIndex() == stackBefore);
    CHECK(plaques->Get(sign)->mood == PlaqueMood::Wild);

    // Seven undos take the seven writes back, newest first.
    edit.Commands().Undo();
    CHECK(plaques->Get(sign)->mood == PlaqueMood::Calm);
    edit.Commands().Undo();
    CHECK(plaques->Get(sign)->target.id == postId);
    edit.Commands().Undo();
    CHECK(plaques->Get(sign)->target.IsNil());
    edit.Commands().Undo();
    CHECK(plaques->Get(sign)->rotation.y == doctest::Approx(0.0f));
    edit.Commands().Undo();
    CHECK(meshes->Get(sign)->mesh.id == meshAsset);
    edit.Commands().Undo();
    CHECK(plaques->Get(sign)->offset.z == doctest::Approx(0.0f));
    edit.Commands().Undo();
    CHECK(plaques->Get(sign)->text == u8"untitled");

    context.ClosePage(page);
}

TEST_CASE("scene-mcp-tools: the viewport camera reads and moves in degrees (position, yaw, pitch, "
          "lookAt wins), and viewport_screenshot waits for the page's capture frame by frame")
{
    Random rng(35);
    const Guid sceneId = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro", sceneId), DefaultAllocator())));
    auto* headless = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Menu", Guid::Generate(rng)), DefaultAllocator())));
    page->hasViewport = true;
    McpServer server;
    RegisterSceneLiveTools(server, context);
    const String pageGuid = GuidText(sceneId);
    const String pageArg = Format(u8"{{\"page\":\"{}\"}}", pageGuid.AsView());

    // With no viewport, every viewport tool refuses by name.
    context.SetActivePage(headless);
    Answer got = Call(server, u8"viewport_camera_get", u8"{}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"page 'Menu' has no viewport"));
    got = Call(server, u8"viewport_camera_set", u8"{\"yawDegrees\":90}");
    CHECK_FALSE(got.ok);
    got = AnswerOf(Pump(server, u8"viewport_screenshot", u8"{}"));
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"page 'Menu' has no viewport"));

    // The pose reads in degrees from the camera's radians.
    page->camera.position = Float3{1.0f, 2.0f, 3.0f};
    page->camera.yaw = 0.0f;
    page->camera.pitch = 0.0f;
    got = Call(server, u8"viewport_camera_get", pageArg.AsView());
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"position").At(2).AsNumber() == doctest::Approx(3.0));
    CHECK(got.payload.Get(u8"yawDegrees").AsNumber() == doctest::Approx(0.0));
    CHECK(got.payload.Get(u8"forward").At(2).AsNumber() == doctest::Approx(-1.0)); // yaw 0 looks down -Z

    // Set: position, then yaw and pitch in degrees; the pitch clamps short of the pole.
    got = Call(server, u8"viewport_camera_set",
               Format(u8"{{\"page\":\"{}\",\"position\":[10,5,0],\"yawDegrees\":90,\"pitchDegrees\":-30}}",
                      pageGuid.AsView())
                   .AsView());
    REQUIRE(got.ok);
    CHECK(page->camera.position.x == doctest::Approx(10.0f));
    CHECK(page->camera.yaw == doctest::Approx(kHalfPi));
    CHECK(page->camera.pitch == doctest::Approx(-30.0f * kPi / 180.0f));
    CHECK(got.payload.Get(u8"yawDegrees").AsNumber() == doctest::Approx(90.0));
    got = Call(server, u8"viewport_camera_set",
               Format(u8"{{\"page\":\"{}\",\"pitchDegrees\":-120}}", pageGuid.AsView()).AsView());
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"pitchDegrees").AsNumber() == doctest::Approx(-89.0));
    // lookAt aims from the position and wins over yaw and pitch given beside it.
    got = Call(server, u8"viewport_camera_set",
               Format(u8"{{\"page\":\"{}\",\"position\":[0,0,10],\"yawDegrees\":45,\"lookAt\":[0,0,0]}}",
                      pageGuid.AsView())
                   .AsView());
    REQUIRE(got.ok);
    CHECK(page->camera.yaw == doctest::Approx(0.0f));
    CHECK(page->camera.pitch == doctest::Approx(0.0f));
    CHECK(page->camera.focusDistance == doctest::Approx(10.0f));
    CHECK(got.payload.Get(u8"focusDistance").AsNumber() == doctest::Approx(10.0));
    // Wrong shapes change nothing.
    got = Call(server, u8"viewport_camera_set",
               Format(u8"{{\"page\":\"{}\",\"position\":[1,2],\"yawDegrees\":10}}", pageGuid.AsView()).AsView());
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"`position` takes [x, y, z]"));
    CHECK(page->camera.yaw == doctest::Approx(0.0f));
    // Orthographic: switched on and zoomed about the focus to a height (the focus is 10 m ahead,
    // at the origin).
    got = Call(server, u8"viewport_camera_set",
               Format(u8"{{\"page\":\"{}\",\"orthographic\":true,\"orthoHeight\":6}}", pageGuid.AsView())
                   .AsView());
    REQUIRE(got.ok);
    CHECK(page->camera.orthographic);
    CHECK(page->camera.OrthoHeight() == doctest::Approx(6.0f));
    CHECK(got.payload.Get(u8"orthographic").AsBool());
    CHECK(got.payload.Get(u8"orthoHeight").AsNumber() == doctest::Approx(6.0));
    const Float3 focus = page->camera.position + page->camera.Forward() * page->camera.focusDistance;
    CHECK(focus.z == doctest::Approx(0.0f).epsilon(1e-4));
    got = Call(server, u8"viewport_camera_set",
               Format(u8"{{\"page\":\"{}\",\"orthoHeight\":0}}", pageGuid.AsView()).AsView());
    CHECK_FALSE(got.ok);
    got = Call(server, u8"viewport_camera_set",
               Format(u8"{{\"page\":\"{}\",\"orthographic\":false}}", pageGuid.AsView()).AsView());
    REQUIRE(got.ok);
    CHECK_FALSE(page->camera.orthographic);

    // The screenshot: the first pump brings the page to the front (active and revealed, since a
    // background tab's viewport never renders) and asks for the capture. The call is then
    // re-entered each pump until the page reports the frame written.
    context.SetActivePage(headless);
    EditorPage* revealed = nullptr;
    context.OnRevealPage = [&revealed](EditorPage* shown) { revealed = shown; };
    LineOutcome outcome = Pump(server, u8"viewport_screenshot",
                               Format(u8"{{\"page\":\"{}\",\"path\":\"/tmp/bistro.png\"}}", pageGuid.AsView()).AsView());
    CHECK(outcome.state == LineState::NotFinished);
    CHECK(context.ActivePage() == page);
    CHECK(revealed == page);
    CHECK(page->captureRequests == 1u);
    CHECK(page->capture.state == ViewportCaptureState::Pending);
    CHECK(page->capture.path == u8"/tmp/bistro.png");
    outcome = Pump(server, u8"viewport_screenshot",
                   Format(u8"{{\"page\":\"{}\",\"path\":\"/tmp/bistro.png\"}}", pageGuid.AsView()).AsView());
    CHECK(outcome.state == LineState::NotFinished); // not yet rendered
    CHECK(page->captureRequests == 1u);              // the same request, not a new one
    page->CompleteCapture(1280, 720);
    got = AnswerOf(Pump(server, u8"viewport_screenshot",
                        Format(u8"{{\"page\":\"{}\",\"path\":\"/tmp/bistro.png\"}}", pageGuid.AsView()).AsView()));
    REQUIRE(got.ok);
    CHECK(got.payload.Get(u8"path").AsString() == StringView(u8"/tmp/bistro.png"));
    CHECK(got.payload.Get(u8"width").AsNumber() == doctest::Approx(1280));
    CHECK(got.payload.Get(u8"height").AsNumber() == doctest::Approx(720));
    // A failed capture is an error naming the log; a new call starts a new request.
    outcome = Pump(server, u8"viewport_screenshot",
                   Format(u8"{{\"page\":\"{}\",\"path\":\"/tmp/again.png\"}}", pageGuid.AsView()).AsView());
    CHECK(outcome.state == LineState::NotFinished);
    CHECK(page->captureRequests == 2u);
    page->FailCapture();
    got = AnswerOf(Pump(server, u8"viewport_screenshot",
                        Format(u8"{{\"page\":\"{}\",\"path\":\"/tmp/again.png\"}}", pageGuid.AsView()).AsView()));
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"the capture of page 'Bistro' failed"));

    context.ClosePage(page);
    context.ClosePage(headless);
}

TEST_CASE("scene-mcp-tools: viewport_frame frames named entities or the selection, at once")
{
    Random rng(41);
    const Guid sceneId = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro", sceneId), DefaultAllocator())));
    McpServer server;
    RegisterSceneLiveTools(server, context);
    CHECK(server.ToolCount() == kSceneLiveToolCount);
    context.SetActivePage(page);

    scene::Scene& scene = page->EditContext().Scene();
    const Guid table = scene.GetEntityId(scene.CreateEntity(u8"Table"));
    const Guid lamp = scene.GetEntityId(scene.CreateEntity(u8"Lamp"));

    // With no viewport the tool refuses by name.
    Answer got = Call(server, u8"viewport_frame", u8"{\"entities\":[\"Table\"]}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"page 'Bistro' has no viewport"));
    page->hasViewport = true;

    // Named entities are framed at once, with no glide, so a screenshot taken next shows them.
    // The answer carries the camera.
    got = Call(server, u8"viewport_frame", u8"{\"entities\":[\"Table\",\"Lamp\"]}");
    REQUIRE(got.ok);
    REQUIRE(page->framed.Size() == 2);
    CHECK(page->framed[0] == table);
    CHECK(page->framed[1] == lamp);
    CHECK_FALSE(page->framedEased);
    CHECK(got.payload.Get(u8"focusDistance").IsNumber());

    // With no names the selection is framed; with nothing selected the tool refuses and says
    // what to do.
    got = Call(server, u8"viewport_frame", u8"{}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"nothing to frame"));
    page->EditContext().EntitySelection().Set(lamp);
    got = Call(server, u8"viewport_frame", u8"{}");
    REQUIRE(got.ok);
    REQUIRE(page->framed.Size() == 1);
    CHECK(page->framed[0] == lamp);

    // A name the scene does not have.
    got = Call(server, u8"viewport_frame", u8"{\"entities\":[\"Sofa\"]}");
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"no entity 'Sofa'"));
    context.ClosePage(page);
}

TEST_CASE("scene-mcp-tools: navigation_bake bakes a page's zone into its asset, and says why not")
{
    pipeline::RegisterNavigationZoneAsset();
    const StringView dir = u8"scratch_navigation_bake_project";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    EditorContext context{DefaultAllocator()};
    McpServer server;
    RegisterSceneLiveTools(server, context);
    Random rng(77);
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Block", Guid::Generate(rng)), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();
    scene::Scene& sceneRef = edit.Scene();
    engine::navigation::AddNavigationSceneManagers(sceneRef);
    if (!sceneRef.HasSystem<engine::physics::RigidBodyComponentManager>())
    {
        sceneRef.AddSystem<engine::physics::RigidBodyComponentManager>();
    }

    // A 20 x 20 static ground slab with its top at y 0 (the bake reads static bodies), and a zone
    // over it.
    const Guid groundId = edit.CreateEntity(u8"Ground");
    sceneRef.SetLocalPosition(edit.Resolve(groundId), Float3{0, -0.5f, 0});
    engine::physics::RigidBodyComponent& ground =
        sceneRef.GetSystem<engine::physics::RigidBodyComponentManager>()->Add(edit.Resolve(groundId));
    ground.motion = foundation::physics::MotionKind::Static;
    ground.halfExtents = Float3{10, 0.5f, 10};
    const Guid zoneId = edit.CreateEntity(u8"Zone");
    engine::navigation::NavMeshZoneComponent& zone =
        sceneRef.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(
            edit.Resolve(zoneId));
    zone.extents = Float3{15, 10, 15};

    // With no project the bake is refused.
    Answer bake = Call(server, u8"navigation_bake", u8"{}");
    CHECK_FALSE(bake.ok);
    context.SetProject(project.Get());

    // A zone with no asset is refused, with a message saying what to do.
    bake = Call(server, u8"navigation_bake", u8"{}");
    REQUIRE_FALSE(bake.ok);
    CHECK(bake.error.AsView().ContainsIgnoreCase(u8"Navigation Zone asset"));

    // An entity that is not a zone is refused.
    bake = Call(server, u8"navigation_bake", u8"{\"entity\":\"Ground\"}");
    CHECK_FALSE(bake.ok);

    // With the asset the zone bakes; the scene's only zone is found without naming it.
    foundation::content::Instance* asset = project->SourceDb().RootGroup()->CreateInstance(
        u8"BlockZone", pipeline::NavigationZoneAsset::StaticType());
    REQUIRE(asset != nullptr);
    zone.zone.SetId(asset->Id());
    bake = Call(server, u8"navigation_bake", u8"{}");
    REQUIRE(bake.ok);
    CHECK(bake.payload.Get(u8"baked").AsBool());
    CHECK(bake.payload.Get(u8"triangles").AsNumber() == doctest::Approx(12.0)); // the slab's box
    CHECK(bake.payload.Get(u8"asset").AsString() == Format(u8"{}", asset->Id()));
    pipeline::NavigationZoneAsset readBack;
    {
        RefPtr<ISerializable> object = asset->ReadObject();
        auto* stored = Cast<pipeline::NavigationZoneAsset>(object.Get());
        REQUIRE(stored != nullptr);
        REQUIRE(pipeline::EnsureNavMeshLoaded(*asset, *stored).IsOk());
        CHECK_FALSE(stored->navMeshBlob.IsEmpty());
    }

    // Not while the page simulates.
    page->StartSimulation();
    bake = Call(server, u8"navigation_bake", u8"{\"entity\":\"Zone\"}");
    CHECK_FALSE(bake.ok);
    page->StopSimulation();

    context.SetProject(nullptr);
    context.ClosePage(page);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("scene-mcp-tools: viewport_run plays a timeline into a page's viewport, its screenshots taken as their times come")
{
    Random rng(11);
    const Guid sceneGuid = Guid::Generate(rng);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"meadow", sceneGuid), DefaultAllocator())));
    page->hasViewport = true;
    context.SetActivePage(page);
    McpServer server;
    RegisterSceneLiveTools(server, context);
    utf8char text[37];
    sceneGuid.ToChars(text);
    const String args = Format(u8"{{\"page\":\"{}\",\"duration\":1,\"input\":[{{\"at\":0.1,\"mouseMove\":[100,50]}},"
                               u8"{{\"at\":0.2,\"mouseButton\":\"Left\"}}],\"screenshots\":[0.5],\"screenshotDir\":\"/tmp\"}}",
                               StringView(text, 36));

    // Once begun, the page holds the playtest's devices, and the call waits on its clock.
    CHECK(Pump(server, u8"viewport_run", args.AsView()).state == LineState::NotFinished);
    REQUIRE(page->scripted.Get() != nullptr);
    page->AdvanceRun(0.3);
    CHECK(Pump(server, u8"viewport_run", args.AsView()).state == LineState::NotFinished);
    CHECK(page->scripted->Mouse()->X() == doctest::Approx(100.0f));
    CHECK(page->scripted->Mouse()->IsButtonDown(foundation::shell::MouseButton::Left));

    // When the screenshot's time comes, the capture is requested, waited on and kept.
    page->AdvanceRun(0.6);
    CHECK(Pump(server, u8"viewport_run", args.AsView()).state == LineState::NotFinished);
    CHECK(page->captureRequests == 1u);
    CHECK(page->capture.path.AsView().StartsWith(u8"/tmp/"));
    CHECK(Pump(server, u8"viewport_run", args.AsView()).state == LineState::NotFinished); // not rendered yet
    page->CompleteCapture(800, 600);
    CHECK(Pump(server, u8"viewport_run", args.AsView()).state == LineState::NotFinished); // the run goes on

    // When the run's time is up, the held devices are released and the call answers.
    page->AdvanceRun(1.0);
    Answer got = AnswerOf(Pump(server, u8"viewport_run", args.AsView()));
    REQUIRE(got.ok);
    CHECK(page->runEnded);
    CHECK(got.payload.Get(u8"inputs").AsNumber() == doctest::Approx(2.0));
    CHECK(got.payload.Get(u8"width").AsNumber() == doctest::Approx(800.0));
    REQUIRE(got.payload.Get(u8"screenshots").Count() == 1);
    CHECK(got.payload.Get(u8"screenshots").At(0).Get(u8"at").AsNumber() == doctest::Approx(0.6));

    // Refusals: a wrong entry, a duration out of range, and a page with no viewport.
    got = AnswerOf(Pump(server, u8"viewport_run", Format(u8"{{\"page\":\"{}\",\"duration\":1,\"input\":[{{\"at\":0.1,\"key\":\"Nope\"}}]}}", StringView(text, 36)).AsView()));
    CHECK_FALSE(got.ok);
    CHECK(got.error.AsView().StartsWith(u8"input[0]: no key 'Nope'"));
    got = AnswerOf(Pump(server, u8"viewport_run", Format(u8"{{\"page\":\"{}\",\"duration\":0}}", StringView(text, 36)).AsView()));
    CHECK_FALSE(got.ok);
    page->hasViewport = false;
    got = AnswerOf(Pump(server, u8"viewport_run", Format(u8"{{\"page\":\"{}\",\"duration\":1}}", StringView(text, 36)).AsView()));
    CHECK_FALSE(got.ok);
}

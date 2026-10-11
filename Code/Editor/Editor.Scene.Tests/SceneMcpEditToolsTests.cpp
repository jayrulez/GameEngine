// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene tests - the live editing tools (Sedulous fb32ee69) over a real EditorContext
// holding a headless scene page: building a level entity by entity, each call one undo step;
// components added and removed; a prefab placed and linked; a behaviour listed; the refusals.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.scene;
import foundation.scene.resource;
import engine.render;
import engine.script;
import editor.core;
import editor.scene;
import editor.camera;

using namespace foundation::core;
using namespace foundation::mcp;
using namespace editor;
namespace scene = foundation::scene;
namespace json = foundation::json;
using json::JsonValue;

namespace
{
    // A scene page to the editor: a real edit context over a scene with the managers the test
    // edits, and a prefab resolver answering one prefab.
    class EditablePage final : public EditorPage, public ISceneEditorPage
    {
    public:
        explicit EditablePage(const Guid& asset)
            : EditorPage(DefaultAllocator()), m_scene(DefaultAllocator(), u8"level"),
              m_edit(m_scene, Commands())
        {
            SetInstanceId(asset);
            Provide<ISceneEditorPage>(*this);
            m_scene.AddSystem<engine::render::LightComponentManager>();
            m_scene.AddSystem<engine::script::ScriptComponentManager>();
            // The prefab: one "Crate" entity.
            scene::Scene author(DefaultAllocator(), u8"author");
            const scene::EntityHandle crate = author.CreateEntity(u8"Crate");
            MemoryStream payload;
            REQUIRE(scene::CapturePrefab(author, crate, payload).IsOk());
            for (byte b : payload.Bytes())
            {
                m_prefab.PushBack(b);
            }
            EditablePage* self = this;
            m_edit.SetPrefabResolver(scene::PrefabPayloadResolver{
                [self](const Guid& id) -> UniquePtr<IStream>
                {
                    if (id != self->prefabId)
                    {
                        return UniquePtr<IStream>{};
                    }
                    auto stream = MakeUnique<MemoryStream>(DefaultAllocator());
                    (void)stream->Write(self->m_prefab.Data(), self->m_prefab.Size());
                    (void)stream->Seek(0, SeekOrigin::Begin);
                    return UniquePtr<IStream>(Move(stream));
                }});
        }
        [[nodiscard]] StringView Title() const override { return u8"Level"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        [[nodiscard]] SceneEditContext& EditContext() noexcept override { return m_edit; }
        void StartSimulation() override { simulating = true; }
        void StopSimulation() override { simulating = false; }
        void PauseSimulation(bool) override {}
        [[nodiscard]] bool IsSimulating() const noexcept override { return simulating; }
        [[nodiscard]] bool IsPaused() const noexcept override { return false; }
        [[nodiscard]] GizmoController* Gizmos() noexcept override { return nullptr; }
        [[nodiscard]] bool CameraOwnsInput() const noexcept override { return false; }
        [[nodiscard]] bool MarkersShown() const noexcept override { return true; }
        [[nodiscard]] EditorCamera* ViewportCamera() noexcept override { return nullptr; }
        bool FrameEntities(Span<const Guid>, bool) override { return false; }
        [[nodiscard]] Status RequestProjectThumbnail() override { return Status{ErrorCode::NotSupported}; }
        [[nodiscard]] Status RequestViewportCapture(StringView) override
        {
            return Status{ErrorCode::NotSupported};
        }
        [[nodiscard]] const ViewportCapture& LastViewportCapture() const noexcept override
        {
            return m_capture;
        }
        void SetMarkersShown(bool) override {}
        [[nodiscard]] bool AnimationPanelShown() const noexcept override { return false; }
        void SetAnimationPanelShown(bool) override {}
        void CreatePrefabFromEntity(const Guid&) override {}
        void PickAndSpawnPrefab(const Guid&) override {}
        void ApplyInstanceToPrefab(const Guid&) override {}
        void RevertInstance(const Guid&) override {}

        bool simulating = false;
        Guid prefabId{0x7A, 0x7B};

    private:
        scene::Scene m_scene;
        SceneEditContext m_edit;
        Array<byte> m_prefab;
        ViewportCapture m_capture;
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

    bool HasComponent(const JsonValue& entity, StringView wire)
    {
        const JsonValue components = entity.Get(u8"components");
        for (i64 i = 0; i < components.Count(); ++i)
        {
            if (components.At(i).Get(u8"type").AsString() == wire)
            {
                return true;
            }
        }
        return false;
    }

    String GuidText(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        return String(StringView(text, 36));
    }
}

TEST_CASE("scene-edit-tools: an agent builds a level one undo step at a time")
{
    engine::render::RegisterRenderComponentReflection();
    Random rng(41);
    EditorContext context{DefaultAllocator()};
    auto* page = static_cast<EditablePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<EditablePage>(Guid::Generate(rng)), DefaultAllocator())));
    context.SetActivePage(page);
    McpServer server;
    RegisterSceneLiveTools(server, context);
    EditorCommandStack& commands = page->Commands();
    scene::Scene& level = page->EditContext().Scene();

    // An entity, placed: one undo step for the create and its transform together.
    Answer made = Call(server, u8"entity_create",
                       u8"{\"name\":\"Platform\",\"position\":[1,2,3],\"yawDegrees\":90,\"scale\":[2,1,2]}");
    REQUIRE(made.ok);
    const String platformId = made.payload.Get(u8"entity").Get(u8"guid").AsString();
    const JsonValue transform = made.payload.Get(u8"entity").Get(u8"transform");
    CHECK(transform.Get(u8"position").At(1).AsNumber() == doctest::Approx(2.0));
    CHECK(transform.Get(u8"scale").At(0).AsNumber() == doctest::Approx(2.0));
    CHECK(transform.Get(u8"rotation").At(1).AsNumber() == doctest::Approx(0.70710678)); // a quarter turn
    commands.Undo();
    CHECK_FALSE(level.FindEntityByName(u8"Platform").IsAssigned()); // create and placement, one step
    commands.Redo();
    const scene::EntityHandle redone = level.FindEntityByName(u8"Platform");
    REQUIRE(redone.IsAssigned());
    CHECK(level.GetLocalTransform(redone).position.y == doctest::Approx(2.0f));

    // Components by wire name, added once; removed by name.
    Answer added = Call(server, u8"component_add", u8"{\"entity\":\"Platform\",\"component\":\"light\"}");
    REQUIRE(added.ok);
    CHECK(HasComponent(added.payload.Get(u8"entity"), u8"light"));
    Answer again = Call(server, u8"component_add", u8"{\"entity\":\"Platform\",\"component\":\"light\"}");
    CHECK(again.error.AsView().StartsWith(u8"'Platform' already has a light"));
    Answer unknown = Call(server, u8"component_add", u8"{\"entity\":\"Platform\",\"component\":\"physics.Warp\"}");
    CHECK(unknown.error.AsView().StartsWith(u8"no component 'physics.Warp'"));
    Answer removed = Call(server, u8"component_remove", u8"{\"entity\":\"Platform\",\"component\":\"light\"}");
    REQUIRE(removed.ok);
    CHECK_FALSE(HasComponent(removed.payload.Get(u8"entity"), u8"light"));

    // A child made under the platform, then renamed, switched off and moved to the root in one
    // step; a parent loop is refused.
    Answer child = Call(server, u8"entity_create", u8"{\"name\":\"Lamp\",\"parent\":\"Platform\"}");
    REQUIRE(child.ok);
    CHECK(child.payload.Get(u8"entity").Get(u8"parent").AsString() == platformId.AsView());
    Answer loop = Call(server, u8"entity_update", u8"{\"entity\":\"Platform\",\"parent\":\"Lamp\"}");
    CHECK(loop.error.AsView().StartsWith(u8"an entity cannot move under itself"));
    Answer updated = Call(server, u8"entity_update",
                          u8"{\"entity\":\"Platform/Lamp\",\"name\":\"Torch\",\"active\":false,"
                          u8"\"parent\":\"\",\"position\":[0,5,0]}");
    REQUIRE(updated.ok);
    const JsonValue torch = updated.payload.Get(u8"entity");
    CHECK(torch.Get(u8"name").AsString() == StringView(u8"Torch"));
    CHECK_FALSE(torch.Get(u8"active").AsBool());
    CHECK(torch.Get(u8"parent").IsNull());
    CHECK(torch.Get(u8"transform").Get(u8"position").At(1).AsNumber() == doctest::Approx(5.0));
    commands.Undo();
    const scene::EntityHandle lamp = level.FindEntityByName(u8"Lamp");
    REQUIRE(lamp.IsAssigned()); // one undo takes the whole update back
    CHECK(level.IsActive(lamp));
    CHECK(level.GetParent(lamp) == level.FindEntityByName(u8"Platform"));
    commands.Redo();

    // A prefab placed and linked to its prefab, at its position; an unknown one refused.
    Answer spawned = Call(server, u8"prefab_spawn",
                          Format(u8"{{\"prefab\":\"{}\",\"position\":[4,0,-2]}}",
                                 GuidText(page->prefabId).AsView())
                              .AsView());
    REQUIRE(spawned.ok);
    const JsonValue root = spawned.payload.Get(u8"entity");
    CHECK(root.Get(u8"name").AsString() == StringView(u8"Crate"));
    CHECK(root.Get(u8"transform").Get(u8"position").At(0).AsNumber() == doctest::Approx(4.0));
    Guid rootId;
    REQUIRE(Guid::TryParse(root.Get(u8"guid").AsString().AsView(), rootId));
    CHECK(level.FindPrefabInstanceByRoot(rootId) != nullptr); // linked to its prefab
    Answer missing = Call(server, u8"prefab_spawn",
                          Format(u8"{{\"prefab\":\"{}\"}}", GuidText(Guid::Generate(rng)).AsView()).AsView());
    CHECK(missing.error.AsView().StartsWith(u8"no prefab"));

    // A script on an entity: entity_inspect lists the behaviour; one not cooked is refused.
    const Guid script = Guid::Generate(rng);
    Guid platform;
    REQUIRE(Guid::TryParse(platformId.AsView(), platform));
    REQUIRE(page->EditContext().MutateComponent<engine::script::ScriptComponent>(platform, [](engine::script::ScriptComponent&) {}) ==
            false); // no Script component yet
    page->EditContext().AddComponent(platform, &TypeOf<engine::script::ScriptComponent>());
    REQUIRE(page->EditContext().MutateComponent<engine::script::ScriptComponent>(
        platform,
        [&](engine::script::ScriptComponent& component)
        {
            engine::script::ScriptBehavior behavior;
            behavior.script.SetId(script);
            component.behaviors.PushBack(Move(behavior));
        }));
    Answer inspected = Call(server, u8"entity_inspect", u8"{\"entity\":\"Platform\"}");
    REQUIRE(inspected.ok);
    JsonValue behaviors;
    const JsonValue components = inspected.payload.Get(u8"entity").Get(u8"components");
    for (i64 i = 0; i < components.Count(); ++i)
    {
        if (components.At(i).Has(u8"behaviors"))
        {
            behaviors = components.At(i).Get(u8"behaviors");
        }
    }
    REQUIRE(behaviors.Count() == 1);
    CHECK(behaviors.At(0).Get(u8"script").AsString() == GuidText(script).AsView());
    Answer refused = Call(server, u8"behavior_add",
                          Format(u8"{{\"entity\":\"Platform\",\"script\":\"{}\"}}",
                                 GuidText(script).AsView())
                              .AsView());
    CHECK(refused.error.AsView().ContainsIgnoreCase(u8"is not a cooked script class"));
    Answer noBehaviour = Call(server, u8"behavior_set", u8"{\"entity\":\"Platform\",\"index\":3}");
    CHECK(noBehaviour.error.AsView().StartsWith(u8"'Platform' has no behaviour 3"));

    // A deletion takes the subtree; while simulating, every edit is refused.
    Answer deleted = Call(server, u8"entity_delete", u8"{\"entity\":\"Platform\"}");
    REQUIRE(deleted.ok);
    CHECK_FALSE(level.FindEntityByName(u8"Platform").IsAssigned());
    page->simulating = true;
    Answer locked = Call(server, u8"entity_create", u8"{\"name\":\"Late\"}");
    CHECK(locked.error.AsView().ContainsIgnoreCase(u8"is simulating"));
    page->simulating = false;
    Answer gone = Call(server, u8"entity_delete", u8"{\"entity\":\"Nobody\"}");
    CHECK_FALSE(gone.ok);
    context.ClosePage(page);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: asset_uses, the reverse dependency query an agent reads before any
// destructive change. It exercises three kinds of edge against a real project: an asset to asset
// edge (a MaterialAsset naming a texture, found by the builder's ScanDependencies), a scene to
// asset edge (a MeshComponent Ref, collected by loading the scene through a full manager with a
// ResourceManager that has no factories), and a project settings edge (defaultSceneId). It also
// checks the refusals.
#include <doctest/doctest.h>
#include <filesystem>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs; // FindDataRoot, to find the engine library the shader cook reads
import foundation.json;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource;
import foundation.mcp;
import pipeline.core;
import pipeline.registration;
import materials.pipeline;
import texture.pipeline;
import audio.pipeline; // SoundCueAsset, for the empty-cue health warning
import engine.render;
import engine.composition;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;
namespace scene = foundation::scene;

namespace
{
    JsonValue UsesCallResponse(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = s.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        return json::Parse(line.response.AsView()).value;
    }

    JsonValue UsesCallOk(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = UsesCallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(
            resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    String UsesCallErr(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = UsesCallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == true);
        return String(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    JsonValue UsesObj() { return JsonValue::MakeObject(); }
    JsonValue UsesWith(JsonValue o, StringView k, StringView v)
    {
        o.Set(String(k), JsonValue::MakeString(String(v)));
        return o;
    }

    String GuidText(const Guid& id)
    {
        utf8char chars[37];
        id.ToChars(chars);
        return String(StringView(chars, 36));
    }

    // The usedBy entry for `guid`, or a null value.
    JsonValue FindUser(const JsonValue& usedBy, StringView guid)
    {
        for (i64 i = 0; i < usedBy.Count(); ++i)
        {
            if (usedBy.At(i).Get(u8"guid").AsString() == guid)
            {
                return usedBy.At(i);
            }
        }
        return JsonValue();
    }

    bool HasEdge(const JsonValue& user, StringView kind)
    {
        const JsonValue& edges = user.Get(u8"edges");
        for (i64 i = 0; i < edges.Count(); ++i)
        {
            if (edges.At(i).AsString() == kind)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("integration.mcp: asset_uses - reverse dependencies across all edge kinds")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_uses_project", ec);

    McpServer server;
    editor::mcp::ProjectSession session;
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterPipelineTypes();
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    engine::RegisterAllSceneComponentReflection();
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::RegisterAssetUsesTool(server, session, builders);

    (void)UsesCallOk(server, u8"project_create",
                     UsesWith(UsesWith(UsesObj(), u8"directory", u8"mcp_uses_project"), u8"name",
                              u8"Uses"));
    (void)UsesCallOk(server, u8"project_open",
                     UsesWith(UsesObj(), u8"directory", u8"mcp_uses_project"));
    auto* root = session.project->SourceDb().RootGroup();

    // The queried asset: a texture.
    auto* tex = root->CreateInstance(u8"stone", pipeline::TextureAsset::StaticType());
    REQUIRE(tex != nullptr);
    {
        pipeline::TextureAsset asset;
        REQUIRE(tex->WriteObject(asset).IsOk());
    }
    const String texGuid = GuidText(tex->Id());

    // The first user, an asset to asset edge: a material whose builder declares the texture as a
    // runtime reference (ScanDependencies adds it to deps.references).
    auto* mat = root->CreateInstance(u8"wall", pipeline::MaterialAsset::StaticType());
    REQUIRE(mat != nullptr);
    {
        pipeline::MaterialAsset asset;
        asset.source.textureSlots.PushBack(String(u8"albedo"));
        asset.source.textureIds.PushBack(tex->Id());
        REQUIRE(mat->WriteObject(asset).IsOk());
    }
    const String matGuid = GuidText(mat->Id());

    // The second user, a scene to asset edge: a scene whose MeshComponent Ref carries the
    // texture's guid. The collector reports bound ids, so the referenced type does not matter to
    // the edge.
    String sceneGuid;
    {
        scene::Scene authored(DefaultAllocator(), u8"level");
        engine::AddAllSceneManagers(authored);
        scene::EntityHandle e = authored.CreateEntity(u8"rock");
        auto* meshes = authored.GetSystem<engine::render::MeshComponentManager>();
        REQUIRE(meshes != nullptr);
        meshes->Add(e).mesh.SetId(tex->Id());
        auto* inst = root->CreateInstance(u8"level", scene::SceneDocument::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
        sceneGuid = GuidText(inst->Id());
    }

    // The query: both users, each with its edge kind.
    JsonValue uses =
        UsesCallOk(server, u8"asset_uses", UsesWith(UsesObj(), u8"guid", texGuid.AsView()));
    CHECK(uses.Get(u8"useCount").AsNumber() == doctest::Approx(2.0));
    JsonValue matUse = FindUser(uses.Get(u8"usedBy"), matGuid.AsView());
    REQUIRE(matUse.IsObject());
    CHECK(HasEdge(matUse, u8"references"));
    JsonValue sceneUse = FindUser(uses.Get(u8"usedBy"), sceneGuid.AsView());
    REQUIRE(sceneUse.IsObject());
    CHECK(HasEdge(sceneUse, u8"scene-resource"));
    CHECK(uses.Get(u8"projectSettingsUses").Count() == 0);

    // Project-settings edge: point defaultSceneId at the scene, then query the scene.
    Guid sceneId;
    REQUIRE(Guid::TryParse(sceneGuid.AsView(), sceneId));
    session.project->Settings().defaultSceneId = sceneId;
    JsonValue sceneUses =
        UsesCallOk(server, u8"asset_uses", UsesWith(UsesObj(), u8"guid", sceneGuid.AsView()));
    CHECK(sceneUses.Get(u8"useCount").AsNumber() == doctest::Approx(0.0));
    REQUIRE(sceneUses.Get(u8"projectSettingsUses").Count() == 1);
    CHECK(sceneUses.Get(u8"projectSettingsUses").At(0).AsString() ==
          StringView(u8"defaultSceneId"));

    // An unused asset answers empty (the "safe to touch" signal).
    JsonValue matUses =
        UsesCallOk(server, u8"asset_uses", UsesWith(UsesObj(), u8"guid", matGuid.AsView()));
    CHECK(matUses.Get(u8"useCount").AsNumber() == doctest::Approx(0.0));

    // Refusals carry reasons: a malformed guid and a valid-but-unknown one.
    CHECK(UsesCallErr(server, u8"asset_uses", UsesWith(UsesObj(), u8"guid", u8"not-a-guid"))
              .Size() > 0u);
    CHECK(UsesCallErr(server, u8"asset_uses",
                      UsesWith(UsesObj(), u8"guid",
                               u8"00000000-0000-0000-0000-0000000000ff"))
              .Size() > 0u);
}

TEST_CASE("integration.mcp: project_health - the soundness sweep finds what broke")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_health_project", ec);

    McpServer server;
    editor::mcp::ProjectSession session;
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterPipelineTypes();
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    engine::RegisterAllSceneComponentReflection();
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::RegisterProjectHealthTool(server, session, builders);

    (void)UsesCallOk(server, u8"project_create",
                     UsesWith(UsesWith(UsesObj(), u8"directory", u8"mcp_health_project"), u8"name",
                              u8"Health"));
    (void)UsesCallOk(server, u8"project_open",
                     UsesWith(UsesObj(), u8"directory", u8"mcp_health_project"));
    auto* root = session.project->SourceDb().RootGroup();

    // An empty project is sound and has nothing to cook.
    JsonValue clean = UsesCallOk(server, u8"project_health", UsesObj());
    CHECK(clean.Get(u8"sound").AsBool() == true);
    CHECK(clean.Get(u8"dirty").AsNumber() == doctest::Approx(0.0));
    CHECK(clean.Get(u8"danglingRefs").Count() == 0);

    // Intact references stay sound even while uncooked (dirty is workflow state, not breakage).
    auto* tex = root->CreateInstance(u8"stone", pipeline::TextureAsset::StaticType());
    {
        pipeline::TextureAsset asset;
        REQUIRE(tex->WriteObject(asset).IsOk());
    }
    auto* mat = root->CreateInstance(u8"wall", pipeline::MaterialAsset::StaticType());
    {
        pipeline::MaterialAsset asset;
        asset.source.textureSlots.PushBack(String(u8"albedo"));
        asset.source.textureIds.PushBack(tex->Id());
        REQUIRE(mat->WriteObject(asset).IsOk());
    }
    JsonValue intact = UsesCallOk(server, u8"project_health", UsesObj());
    CHECK(intact.Get(u8"sound").AsBool() == true);
    CHECK(intact.Get(u8"dirty").AsNumber() >= 2.0);
    CHECK(intact.Get(u8"danglingRefs").Count() == 0);

    // An empty sound cue is a warning, reported in emptyCues, and not breakage: it is a valid,
    // buildable draft, so `sound` stays true. A cue with a clip assigned is not flagged.
    auto* emptyCue = root->CreateInstance(u8"silence", pipeline::SoundCueAsset::StaticType());
    {
        pipeline::SoundCueAsset asset; // every slot nil
        REQUIRE(emptyCue->WriteObject(asset).IsOk());
    }
    auto* filledCue = root->CreateInstance(u8"footstep", pipeline::SoundCueAsset::StaticType());
    {
        pipeline::SoundCueAsset asset;
        asset.clipIds[0] = tex->Id(); // any non-nil guid means a clip is assigned
        REQUIRE(filledCue->WriteObject(asset).IsOk());
    }
    JsonValue withCues = UsesCallOk(server, u8"project_health", UsesObj());
    CHECK(withCues.Get(u8"sound").AsBool() == true); // a draft cue is a warning, not a break
    REQUIRE(withCues.Get(u8"emptyCues").Count() == 1);
    CHECK(withCues.Get(u8"emptyCues").At(0).Get(u8"name").AsString() == StringView(u8"silence"));

    // Break three things: a material referencing a missing texture, a scene component Ref to a
    // missing guid, and a settings field pointing nowhere.
    const Guid missing{0xdead, 0xbeef};
    auto* badMat = root->CreateInstance(u8"cracked", pipeline::MaterialAsset::StaticType());
    {
        pipeline::MaterialAsset asset;
        asset.source.textureSlots.PushBack(String(u8"albedo"));
        asset.source.textureIds.PushBack(missing);
        REQUIRE(badMat->WriteObject(asset).IsOk());
    }
    foundation::content::Instance* brokenScene = nullptr;
    {
        scene::Scene authored(DefaultAllocator(), u8"broken");
        engine::AddAllSceneManagers(authored);
        scene::EntityHandle e = authored.CreateEntity(u8"ghost");
        authored.GetSystem<engine::render::MeshComponentManager>()->Add(e).mesh.SetId(missing);
        brokenScene = root->CreateInstance(u8"broken", scene::SceneDocument::StaticType());
        REQUIRE(brokenScene != nullptr);
        REQUIRE(scene::SaveScene(authored, *brokenScene).IsOk());
    }
    session.project->Settings().defaultSceneId = missing;

    JsonValue broken = UsesCallOk(server, u8"project_health", UsesObj());
    CHECK(broken.Get(u8"sound").AsBool() == false);
    REQUIRE(broken.Get(u8"danglingRefs").Count() == 2);
    bool sawReferences = false;
    bool sawSceneResource = false;
    for (i64 i = 0; i < broken.Get(u8"danglingRefs").Count(); ++i)
    {
        const JsonValue& d = broken.Get(u8"danglingRefs").At(i);
        CHECK(d.Get(u8"to").AsString() == GuidText(missing).AsView());
        if (d.Get(u8"edge").AsString() == StringView(u8"references"))
        {
            sawReferences = true;
        }
        if (d.Get(u8"edge").AsString() == StringView(u8"scene-resource"))
        {
            sawSceneResource = true;
        }
    }
    CHECK(sawReferences);
    CHECK(sawSceneResource);
    REQUIRE(broken.Get(u8"projectSettingsDangling").Count() == 1);
    CHECK(broken.Get(u8"projectSettingsDangling").At(0).AsString() ==
          StringView(u8"defaultSceneId"));

    // Healing every break flips the verdict back.
    session.project->Settings().defaultSceneId = Guid();
    {
        pipeline::MaterialAsset asset;
        asset.source.textureSlots.PushBack(String(u8"albedo"));
        asset.source.textureIds.PushBack(tex->Id());
        REQUIRE(badMat->WriteObject(asset).IsOk());
    }
    {
        scene::Scene authored(DefaultAllocator(), u8"broken");
        engine::AddAllSceneManagers(authored);
        scene::EntityHandle e = authored.CreateEntity(u8"ghost");
        authored.GetSystem<engine::render::MeshComponentManager>()->Add(e).mesh.SetId(tex->Id());
        REQUIRE(scene::SaveScene(authored, *brokenScene).IsOk());
    }
    JsonValue healed = UsesCallOk(server, u8"project_health", UsesObj());
    CHECK(healed.Get(u8"sound").AsBool() == true);
    CHECK(healed.Get(u8"danglingRefs").Count() == 0);
}

// asset_delete, the Assets browser's Delete for an agent. It is refused while anything uses the
// asset, found by the same scan asset_uses runs, and the refusal names the users; forcing it
// deletes anyway.
TEST_CASE("integration.mcp: asset_delete - refused while used, unless forced")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_delete_project", ec);

    McpServer server;
    editor::mcp::ProjectSession session;
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterPipelineTypes();
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    engine::RegisterAllSceneComponentReflection();
    editor::mcp::ProjectOwner owner;
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterAssetDeleteTool(server, session, builders, operations);

    (void)UsesCallOk(server, u8"project_create",
                     UsesWith(UsesWith(UsesObj(), u8"directory", u8"mcp_delete_project"), u8"name", u8"Delete"));
    (void)UsesCallOk(server, u8"project_open", UsesWith(UsesObj(), u8"directory", u8"mcp_delete_project"));
    auto* root = session.project->SourceDb().RootGroup();

    // A texture a material uses, and a scene the project settings name.
    auto* tex = root->CreateInstance(u8"stone", pipeline::TextureAsset::StaticType());
    REQUIRE(tex != nullptr);
    {
        pipeline::TextureAsset asset;
        REQUIRE(tex->WriteObject(asset).IsOk());
    }
    const Guid texId = tex->Id();
    auto* mat = root->CreateInstance(u8"wall", pipeline::MaterialAsset::StaticType());
    REQUIRE(mat != nullptr);
    {
        pipeline::MaterialAsset asset;
        asset.source.textureSlots.PushBack(String(u8"albedo"));
        asset.source.textureIds.PushBack(texId);
        REQUIRE(mat->WriteObject(asset).IsOk());
    }
    const Guid matId = mat->Id();
    Guid sceneId;
    {
        scene::Scene authored(DefaultAllocator(), u8"level");
        engine::AddAllSceneManagers(authored);
        auto* inst = root->CreateInstance(u8"level", scene::SceneDocument::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
        sceneId = inst->Id();
    }
    session.project->Settings().defaultSceneId = sceneId;

    // Used: refused, naming the users; nothing deleted.
    const String texRefusal =
        UsesCallErr(server, u8"asset_delete", UsesWith(UsesObj(), u8"guid", GuidText(texId).AsView()));
    CHECK(texRefusal.AsView().StartsWith(u8"refused - 'stone' is still used by 'wall'"));
    CHECK(session.project->SourceDb().GetInstance(texId) != nullptr);
    const String sceneRefusal =
        UsesCallErr(server, u8"asset_delete", UsesWith(UsesObj(), u8"guid", GuidText(sceneId).AsView()));
    CHECK(sceneRefusal.AsView().ContainsIgnoreCase(u8"by the project settings: defaultSceneId"));

    // Unused: deleted.
    JsonValue deleted = UsesCallOk(server, u8"asset_delete", UsesWith(UsesObj(), u8"guid", GuidText(matId).AsView()));
    CHECK(deleted.Get(u8"deleted").AsBool());
    CHECK(deleted.Get(u8"name").AsString() == StringView(u8"wall"));
    CHECK(session.project->SourceDb().GetInstance(matId) == nullptr);

    // Forced: deleted though the settings still name it.
    JsonValue forced = UsesWith(UsesObj(), u8"guid", GuidText(sceneId).AsView());
    forced.Set(u8"force", JsonValue::MakeBool(true));
    CHECK(UsesCallOk(server, u8"asset_delete", Move(forced)).Get(u8"deleted").AsBool());
    CHECK(session.project->SourceDb().GetInstance(sceneId) == nullptr);

    // Gone, or never there: refused with the reason.
    CHECK(UsesCallErr(server, u8"asset_delete", UsesWith(UsesObj(), u8"guid", GuidText(matId).AsView()))
              .AsView()
              .StartsWith(u8"no asset with guid"));

    owner.project.Reset();
    session.project = nullptr;
    std::filesystem::remove_all("mcp_delete_project", ec);
}

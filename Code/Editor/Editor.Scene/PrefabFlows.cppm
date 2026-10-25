// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :prefab_flows partition.
//
// The scene page's prefab flows, which write assets and ask first where they cannot be undone: a
// prefab made from an entity and what is under it (under "Prefabs/", named after the entity, the
// original replaced by an instance of it, one undo group); an instance's state written back to its
// prefab and every instance in the open scenes rebuilt from it (not undoable, so asked first); an
// instance's overrides discarded (asked first too: entities parented under it that are not the
// prefab's go); an instance spawned through the asset picker. And the prefab half of a save: what
// refuses it (a prefab has one root and never holds itself), and the other open scenes' instances
// rebuilt once it is saved. Out of the page's class as it was (Documentation/Specs/editor-gui.md,
// 5.3, layer 2); the page's dialogs, its deferral and its picker come in as services.

module;
#include "Core/Prelude.h"

export module editor.scene:prefab_flows;

import foundation.core;
import foundation.content;
import foundation.resource; // AsyncBindScope
import foundation.scene;
import foundation.scene.resource;
import engine.scene;
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    class PrefabFlows
    {
    public:
        struct Services
        {
            EditorContext* context = nullptr;
            SceneEditContext* edit = nullptr;
            engine::scene::SceneSubsystem* scenes = nullptr; // the open scenes (null: this one alone)
            /// The asset the page edits (it changes on Save As).
            Function<Guid()> pageAsset;
            /// Asks `text` under `title`; `yes` runs when the user takes `verb`.
            Function<void(StringView title, StringView text, StringView verb, Function<void()> yes)> confirm;
            /// Runs work after the event that asked for it (a dialog's answer).
            Function<void(Function<void()>)> later;
            /// Opens the asset picker over `types`; `chosen` gets what is picked.
            Function<void(Span<const String> types, Function<void(const Guid&)> chosen)> pickAsset;
        };

        explicit PrefabFlows(Services services) : m_services(Move(services)) {}

        /// A prefab asset from `entityId` and everything under it, the entity replaced by an instance.
        void CreateFromEntity(const Guid& entityId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            if (project == nullptr)
                return;
            const foundation::scene::EntityHandle live = Edit().Resolve(entityId);
            if (!live.IsAssigned())
                return;
            MemoryStream payload;
            if (!foundation::scene::CapturePrefab(scene, live, payload).IsOk())
            {
                Context().Notify(NoticeKind::Error, u8"Prefab capture failed.");
                return;
            }
            foundation::content::Group* root = project->SourceDb().RootGroup();
            foundation::content::Group* prefabs = root->GetGroup(u8"Prefabs");
            if (prefabs == nullptr)
                prefabs = root->CreateGroup(u8"Prefabs");
            String base(scene.GetEntityName(live));
            if (base.IsEmpty())
                base = String(u8"Prefab");
            const String name = prefabs->UniqueInstanceName(base.AsView());
            foundation::content::Instance* asset = prefabs->CreateInstance(name.AsView(), foundation::scene::PrefabDocument::StaticType());
            if (asset == nullptr)
                return;
            foundation::scene::PrefabDocument doc;
            doc.name = name;
            if (!asset->WriteObject(doc).IsOk() || !asset->WriteData(u8"scene", payload.Bytes(), foundation::content::StreamEncoding::Text).IsOk())
            {
                Context().Notify(NoticeKind::Error, u8"Prefab asset write failed.");
                return;
            }
            Array<byte> bytes;
            const Span<const byte> view = payload.Bytes();
            bytes.Reserve(view.Size());
            for (byte b : view)
                bytes.PushBack(b);
            const Guid instanceRoot = Edit().ReplaceWithPrefabInstance(entityId, asset->Id(), Move(bytes));
            if (!instanceRoot.IsNil())
                Context().Notify(NoticeKind::Success, Format(u8"Created prefab '{}'.", name).AsView());
        }

        /// Asks, then writes the instance at `rootId` back to its prefab.
        void Apply(const Guid& rootId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            foundation::scene::Scene::PrefabInstanceState* state = scene.FindPrefabInstanceByRoot(rootId);
            if (project == nullptr || state == nullptr || !m_services.confirm)
                return;
            foundation::content::Instance* asset = project->SourceDb().GetInstance(state->prefabId);
            if (asset == nullptr)
            {
                Context().Notify(NoticeKind::Warning, u8"The instance's prefab asset no longer exists.");
                return;
            }
            const String text = Format(u8"Apply '{}' to prefab '{}'? The prefab asset is rewritten and every instance in open scenes "
                                       u8"updates to match. This cannot be undone.",
                                       scene.GetEntityName(scene.FindEntity(rootId)), asset->Name());
            PrefabFlows* self = this;
            // Deferred: rebuilding instances destroys and respawns entities (and their rows),
            // never in the middle of the dialog's event.
            m_services.confirm(u8"Apply to Prefab", text.AsView(), u8"Apply", [self, rootId]() { self->Later([self, rootId]() { self->ApplyNow(rootId); }); });
        }
        /// The instance's current state becomes the template (source-id keyed, so other instances'
        /// overrides stay valid), then every instance everywhere rebuilds from it, this one too.
        void ApplyNow(const Guid& rootId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            foundation::scene::Scene::PrefabInstanceState* state = scene.FindPrefabInstanceByRoot(rootId);
            if (project == nullptr || state == nullptr)
                return;
            foundation::content::Instance* asset = project->SourceDb().GetInstance(state->prefabId);
            if (asset == nullptr)
            {
                Context().Notify(NoticeKind::Warning, u8"The instance's prefab asset no longer exists.");
                return;
            }
            MemoryStream payload;
            if (!foundation::scene::CaptureInstanceAsTemplate(scene, *state, payload, Edit().PrefabResolver()).IsOk() ||
                !asset->WriteData(u8"scene", payload.Bytes(), foundation::content::StreamEncoding::Text).IsOk())
            {
                Context().Notify(NoticeKind::Error, u8"Apply to Prefab failed.");
                return;
            }
            Array<byte> bytes;
            for (byte b : payload.Bytes())
                bytes.PushBack(b);
            const Guid prefabId = state->prefabId;
            RebuildInstances(prefabId, Span<const byte>{bytes.Data(), bytes.Size()}, nullptr);
            // An open page on the prefab itself shows the template (plain entities, not an
            // instance): the rebuild cannot reach it, so it is told to refresh.
            (void)Context().NotifyAssetExternallyModified(prefabId);
            Context().Notify(NoticeKind::Success, Format(u8"Applied to prefab '{}' (not undoable - the asset changed).", asset->Name()).AsView());
        }

        /// Asks, then discards the instance's overrides.
        void Revert(const Guid& rootId)
        {
            foundation::scene::Scene& scene = Scene();
            if (Project() == nullptr || scene.FindPrefabInstanceByRoot(rootId) == nullptr || !m_services.confirm)
                return;
            const String text = Format(u8"Revert '{}' to its prefab? All overrides on this instance are discarded, and any non-prefab "
                                       u8"entities parented under it are destroyed. This cannot be undone.",
                                       scene.GetEntityName(scene.FindEntity(rootId)));
            PrefabFlows* self = this;
            m_services.confirm(u8"Revert Instance", text.AsView(), u8"Revert", [self, rootId]() { self->Later([self, rootId]() { self->RevertNow(rootId); }); });
        }
        /// The instance respawned from the current template, its placement kept. Not undoable.
        void RevertNow(const Guid& rootId)
        {
            EditorProject* project = Project();
            foundation::scene::Scene& scene = Scene();
            foundation::scene::Scene::PrefabInstanceState* state = scene.FindPrefabInstanceByRoot(rootId);
            if (project == nullptr || state == nullptr)
                return;
            foundation::content::Instance* asset = project->SourceDb().GetInstance(state->prefabId);
            UniquePtr<IStream> payload = asset != nullptr ? asset->ReadData(u8"scene") : UniquePtr<IStream>{};
            if (payload.Get() == nullptr)
            {
                Context().Notify(NoticeKind::Warning, u8"The instance's prefab asset no longer exists.");
                return;
            }
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(payload->Size()));
            (void)payload->Read(bytes.Data(), bytes.Size());
            if (foundation::scene::RevertPrefabInstance(scene, rootId, Span<const byte>{bytes.Data(), bytes.Size()}, Edit().PrefabResolver()))
            {
                if (Context().Resources() != nullptr)
                {
                    foundation::resource::AsyncBindScope asyncScope(*Context().Resources());
                    foundation::scene::ResolveSceneResources(scene, *Context().Resources());
                }
                Context().Notify(NoticeKind::Info, u8"Instance reverted to its prefab (not undoable).");
            }
        }

        /// An instance of the prefab the picker gives, under `parent` (nil: the scene's root).
        void PickAndSpawn(const Guid& parent)
        {
            if (Project() == nullptr || !m_services.pickAsset)
                return;
            const String types[] = {String(u8"PrefabDocument")};
            PrefabFlows* self = this;
            m_services.pickAsset(Span<const String>(types, 1u), [self, parent](const Guid& picked) { self->Spawn(picked, parent); });
        }
        /// An instance of prefab `prefabId` under `parent`; false, said, when it cannot be.
        bool Spawn(const Guid& prefabId, const Guid& parent)
        {
            EditorProject* project = Project();
            if (prefabId.IsNil() || project == nullptr)
                return false;
            if (m_services.pageAsset && prefabId == m_services.pageAsset())
            {
                Context().Notify(NoticeKind::Warning, u8"A prefab cannot contain an instance of itself.");
                return false;
            }
            foundation::content::Instance* prefab = project->SourceDb().GetInstance(prefabId);
            UniquePtr<IStream> payload = prefab != nullptr ? prefab->ReadData(u8"scene") : UniquePtr<IStream>{};
            if (payload.Get() == nullptr)
            {
                Context().Notify(NoticeKind::Warning, u8"Prefab has no content yet (save it once first).");
                return false;
            }
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(payload->Size()));
            (void)payload->Read(bytes.Data(), bytes.Size());
            return !Edit().SpawnPrefabInstance(prefabId, Move(bytes), parent).IsNil();
        }

        /// Why the scene, as the prefab `prefabId`, may not be saved; empty when it may.
        [[nodiscard]] static String SaveRefusal(foundation::scene::Scene& scene, const Guid& prefabId)
        {
            usize roots = 0;
            for (foundation::scene::EntityHandle r = scene.GetFirstRoot(); r.IsAssigned(); r = scene.GetNextSibling(r))
                ++roots;
            if (roots > 1)
                return String(u8"A prefab needs exactly one root entity - parent everything under a single root, then save.");
            bool self = false;
            scene.ForEachPrefabInstance([&](foundation::scene::Scene::PrefabInstanceState& state) { self = self || state.prefabId == prefabId; });
            if (self)
                return String(u8"A prefab cannot contain an instance of itself - remove it, then save.");
            return String{};
        }
        /// The prefab `asset` was saved: its instances in every other open scene rebuild, their
        /// overrides kept (captured, respawned, applied again).
        void RebuildOtherInstances(foundation::content::Instance& asset)
        {
            UniquePtr<IStream> payload = asset.ReadData(u8"scene");
            if (payload.Get() == nullptr)
                return;
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(payload->Size()));
            (void)payload->Read(bytes.Data(), bytes.Size());
            RebuildInstances(asset.Id(), Span<const byte>{bytes.Data(), bytes.Size()}, &Scene());
        }

    private:
        [[nodiscard]] EditorContext& Context() const { return *m_services.context; }
        [[nodiscard]] SceneEditContext& Edit() const { return *m_services.edit; }
        [[nodiscard]] foundation::scene::Scene& Scene() const { return m_services.edit->Scene(); }
        [[nodiscard]] EditorProject* Project() const { return m_services.context->Project(); }
        void Later(Function<void()> work)
        {
            if (m_services.later)
                m_services.later(Move(work));
            else
                work();
        }
        // Every open scene's instances of `prefabId` from `payload`, but `skip`'s.
        void RebuildInstances(const Guid& prefabId, Span<const byte> payload, foundation::scene::Scene* skip)
        {
            EditorContext& context = Context();
            const auto rebuild = [&](foundation::scene::Scene& scene)
            {
                if (&scene == skip)
                    return;
                const u32 rebuilt = foundation::scene::RebuildPrefabInstances(scene, prefabId, payload, Edit().PrefabResolver());
                if (rebuilt > 0 && context.Resources() != nullptr)
                {
                    foundation::resource::AsyncBindScope asyncScope(*context.Resources());
                    foundation::scene::ResolveSceneResources(scene, *context.Resources());
                }
            };
            if (m_services.scenes != nullptr)
                m_services.scenes->ForEachScene(rebuild);
            else
                rebuild(Scene());
        }

        Services m_services;
    };
}

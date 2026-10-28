// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Physics - the `editor.physics` module (tooling).
//
// CollisionShapeEditorPage: the bespoke authoring page for a CollisionShapeAsset. On the GUI from
// the preview pages' markup (Data/Editor/PreviewPage.xml; Documentation/Specs/editor-gui.md, 5.4,
// layer 3): the cooked collider's outline as a wireframe in the preview, framed when it first
// shows and when a cook changes it, beside the asset's rows: its source mesh (an asset slot taking
// what a mesh cooks to, static or skinned), the cook mode and the hull's tolerance. Each edit is
// one undo step, where the page had changed the asset with no undo at all; a shape with no mesh
// says so in a notice, and shows no collider (a collider cooked before its mesh was cleared is not
// one it can make now). Save writes the asset back and requests a re-cook, so a shape=Cooked body
// picks up the new collider; the old page's Cook now is the bar's Cook, as every asset page has it.
// Never linked by the runtime.

module;
#include "Core/Prelude.h"

export module editor.physics;

export import :collision_thumbnail;

import foundation.core;
import foundation.content;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import physics.pipeline;             // CollisionShapeAsset + CollisionCookKind
import foundation.physics.resource;  // CollisionShape product (cooked outline)
import foundation.graphics;          // FrameContext
import foundation.gui;
import editor.core;
import editor.app;
import editor.preview;               // PreviewViewport (+ EditorCamera)

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace resource = foundation::resource;
    namespace physics = foundation::physics;

    class CollisionShapeEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/PreviewPage.xml";

        CollisionShapeEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);

        // 3D preview: draw the cooked outline wireframe, drive the orbit camera, hot-swap on re-cook.
        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        /// What a cook kind is called (headless; a test reads it).
        [[nodiscard]] static StringView CookLabel(pipeline::CollisionCookKind kind);
        /// Whether the preview shows the cooked collider: there is one, and the asset still names
        /// a mesh to make it from (cleared, the last cook's collider is one it can no longer make).
        [[nodiscard]] static bool ShowsCollider(const Guid& sourceMesh, const physics::CollisionShape* shape)
        {
            return !sourceMesh.IsNil() && shape != nullptr && shape->outline.Size() >= 3;
        }

        // === For tests ===
        [[nodiscard]] app::PreviewPane* Pane() const noexcept { return m_pane.Get(); }

    protected:
        void OnObjectChanged() override { ShowStatus(); }

    private:
        // The notice and the facts: what it cooks from and how; the preview or why there is none.
        void ShowStatus();
        void DrawOutline(); // immediate-mode wireframe of the outline triangles (per frame)
        void FrameOutline();
        [[nodiscard]] String MeshName(const Guid& id) const;

        // Preview: the shared substrate + the bound cooked product (the Proxy auto-follows re-cooks,
        // so DrawOutline always reads the current outline). m_framedCount reframes the camera when
        // the outline first appears and whenever a re-cook changes its vertex count.
        UniquePtr<app::PreviewPane> m_pane;
        resource::Proxy<physics::CollisionShape> m_shape;
        usize m_framedCount = 0;
    };

    class CollisionShapeEditorPageFactory final : public IEditorPageFactory
    {
    public:
        explicit CollisionShapeEditorPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override { return &pipeline::CollisionShapeAsset::StaticType(); }
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override
        {
            return UniquePtr<EditorPage>(
                editor::EditorRootAllocator().New<CollisionShapeEditorPage>(context, m_services->Make(), instance),
                editor::EditorRootAllocator());
        }

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// The page (asking for its services when it is made) and the collision thumbnails.
    inline void RegisterCollisionShapeEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<CollisionShapeEditorPageFactory>(services), editor::EditorRootAllocator()));
        if (context.Thumbnails() != nullptr)
        {
            RegisterCollisionThumbnailGenerator(*context.Thumbnails());
        }
    }
}

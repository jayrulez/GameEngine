// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Physics - the `editor.physics` module (tooling).
//
// CollisionShapeEditorPage is the authoring page for a CollisionShapeAsset, built on the GUI from
// the preview pages' markup (Data/Editor/PreviewPage.xml). The preview shows the cooked collider's
// outline as a wireframe, framed when it first shows and whenever a cook changes it. Beside it are
// the asset's rows: its source mesh (an asset slot that takes what a mesh cooks to, static or
// skinned), the cook mode and the hull's tolerance. Each edit is one undo step. A shape with no
// mesh says so in a notice and shows no collider, because a collider cooked before the mesh was
// cleared is not one the asset can make now. Save writes the asset back and requests a re-cook, so
// a body with shape=Cooked picks up the new collider; cooking itself is the bar's Cook, as on every
// asset page. The runtime never links this module.

module;
#include "Core/Prelude.h"

export module editor.physics;

export import :collision_thumbnail;

import foundation.core;
import foundation.content;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import physics.pipeline;             // CollisionShapeAsset and CollisionCookKind
import foundation.physics.resource;  // CollisionShape product (cooked outline)
import foundation.graphics;          // FrameContext
import foundation.gui;
import editor.core;
import editor.app;
import editor.preview;               // PreviewViewport and EditorCamera

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

        // 3D preview: draws the cooked outline as a wireframe, drives the orbit camera, and picks up
        // a re-cooked shape.
        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        /// What a cook kind is called (headless; a test reads it).
        [[nodiscard]] static StringView CookLabel(pipeline::CollisionCookKind kind);
        /// Whether the preview shows the cooked collider: there is one, and the asset still names
        /// a mesh to make it from (once the mesh is cleared, the last cooked collider is not one it
        /// can make).
        [[nodiscard]] static bool ShowsCollider(const Guid& sourceMesh, const physics::CollisionShape* shape)
        {
            return !sourceMesh.IsNil() && shape != nullptr && shape->outline.Size() >= 3;
        }

        // === For tests ===
        [[nodiscard]] app::PreviewPane* Pane() const noexcept { return m_pane.Get(); }

    protected:
        void OnObjectChanged() override { ShowStatus(); }

    private:
        // Shows the notice and the facts: what it cooks from and how, and the preview or why there is
        // none.
        void ShowStatus();
        void DrawOutline(); // immediate-mode wireframe of the outline triangles (per frame)
        void FrameOutline();
        [[nodiscard]] String MeshName(const Guid& id) const;

        // Preview: the shared preview pane and the bound cooked product. The Proxy follows re-cooks,
        // so DrawOutline always reads the current outline. m_framedCount reframes the camera when
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

    /// Registers the page (which asks for its services when it is made) and the collision thumbnails.
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

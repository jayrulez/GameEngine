// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Physics - the collision shape page, its body. See CollisionShapePage.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.physics;

import foundation.core;
import foundation.content;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import foundation.render;
import physics.pipeline;
import foundation.physics.resource;
import foundation.graphics;
import foundation.gui;
import editor.core;
import editor.app;
import editor.preview; // PreviewViewport

using namespace foundation::core;
namespace content = foundation::content;

namespace editor
{
    namespace runtime = foundation::runtime;
    namespace resource = foundation::resource;
    namespace physics = foundation::physics;
    namespace render = foundation::render;

    StringView CollisionShapeEditorPage::CookLabel(pipeline::CollisionCookKind kind)
    {
        return kind == pipeline::CollisionCookKind::ConvexHull ? StringView(u8"Convex hull (dynamic)")
                                                              : StringView(u8"Triangle mesh (static)");
    }

    String CollisionShapeEditorPage::MeshName(const Guid& id) const
    {
        if (id.IsNil() || Context().Project() == nullptr)
            return String(u8"Not set");
        content::Instance* inst = Context().Project()->SourceDb().GetInstance(id);
        return inst != nullptr ? String(inst->Path().AsView()) : String(u8"(missing mesh)");
    }

    CollisionShapeEditorPage::CollisionShapeEditorPage(EditorContext& context, app::AssetPageServices services, content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::CollisionShapeAsset::StaticType(), kPath)
    {
        // The outline is drawn as unlit debug lines each frame, so the preview's scene needs no
        // entities or light.
        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"collision.preview");
            CollisionShapeEditorPage* self = this;
            m_pane->OnFrame = [self]() { self->FrameOutline(); };
        }
        // Bind the cooked product (auto-follows re-cooks); DrawOutline reads it each frame.
        if (Context().Resources() != nullptr)
            m_shape = Context().Resources()->Bind<physics::CollisionShape>(InstanceId());
        ShowStatus();
    }

    void CollisionShapeEditorPage::ShowStatus()
    {
        const auto* asset = ObjectAs<pipeline::CollisionShapeAsset>();
        if (asset == nullptr)
            return;
        SetNotice(u8"mesh", asset->sourceMesh.IsNil() ? StringView(u8"No source mesh: pick one, then save to cook its collider.") : StringView{});
        SetFact(u8"Cooks From", MeshName(asset->sourceMesh).AsView());
        SetFact(u8"Cooks As", CookLabel(asset->cook));
        const physics::CollisionShape* shape = m_shape ? m_shape.Get() : nullptr;
        if (!ShowsCollider(asset->sourceMesh, shape))
        {
            SetFact(u8"Collider", asset->sourceMesh.IsNil() ? StringView(u8"None: no source mesh") : StringView(u8"Not cooked yet"));
            if (m_pane)
                m_pane->ShowEmpty(u8"No collider yet", asset->sourceMesh.IsNil() ? StringView(u8"Pick a source mesh, then save: its collider cooks and shows here.")
                                                                                 : StringView(u8"Save to cook the collider; it shows here once cooked."));
            return;
        }
        SetFact(u8"Collider", Format(u8"{} triangles", shape->outline.Size() / 3).AsView());
        if (m_pane)
            m_pane->Show();
    }

    void CollisionShapeEditorPage::FrameOutline()
    {
        const physics::CollisionShape* shape = m_shape ? m_shape.Get() : nullptr;
        if (shape == nullptr || shape->outline.Size() < 3 || !m_pane || m_pane->Viewport() == nullptr)
            return;
        const Array<Float3>& tris = shape->outline;
        Float3 lo = tris[0];
        Float3 hi = tris[0];
        for (const Float3& v : tris)
        {
            lo = Float3{Min(lo.x, v.x), Min(lo.y, v.y), Min(lo.z, v.z)};
            hi = Float3{Max(hi.x, v.x), Max(hi.y, v.y), Max(hi.z, v.z)};
        }
        m_pane->Viewport()->Frame((lo + hi) * 0.5f, Max(0.25f, Length((hi - lo) * 0.5f)));
    }

    void CollisionShapeEditorPage::DrawOutline()
    {
        PreviewViewport* preview = m_pane ? m_pane->Viewport() : nullptr;
        if (preview == nullptr || !preview->IsValid())
            return;
        const physics::CollisionShape* shape = m_shape ? m_shape.Get() : nullptr;
        if (shape == nullptr || shape->outline.Size() < 3)
            return;
        const Array<Float3>& tris = shape->outline; // triangle list: 3 vertices each
        // Frame the camera when the outline first appears or a re-cook changes its vertex count,
        // and say what it is now.
        if (tris.Size() != m_framedCount)
        {
            FrameOutline();
            m_framedCount = tris.Size();
            ShowStatus();
        }
        // Immediate-mode wireframe: three edges per triangle (DebugScene clears each frame).
        render::debug::DebugDraw& draw = preview->SceneDebugDraw();
        const Color color{0.30f, 0.95f, 0.55f, 1.0f};
        for (usize i = 0; i + 2 < tris.Size(); i += 3)
        {
            draw.DrawLine(tris[i], tris[i + 1], color);
            draw.DrawLine(tris[i + 1], tris[i + 2], color);
            draw.DrawLine(tris[i + 2], tris[i], color);
        }
    }

    void CollisionShapeEditorPage::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        if (m_pane)
            m_pane->Update(dt);
        DrawOutline();
    }

    void CollisionShapeEditorPage::OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void CollisionShapeEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }
}

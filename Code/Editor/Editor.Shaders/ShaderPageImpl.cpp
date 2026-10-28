// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Shaders - ShaderEditorPage (declared in :shader_page).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.shaders;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.geometry;
import foundation.shaders;
import foundation.shaders.system;
import foundation.shaders.resource;
import foundation.materials;
import foundation.materials.resource;
import foundation.gui;
import foundation.gui.code;
import engine.render;
import shaders.pipeline;
import materials.pipeline;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace shaders = foundation::shaders;
    namespace materials = foundation::materials;
    namespace gui = foundation::gui;

    ShaderEditorPage::ShaderEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance,
                                       StringView shaderLibrary)
        : app::CodeAssetPage(context, Move(services), instance, pipeline::ShaderAsset::StaticType(), kPath), m_shaderLibrary(shaderLibrary)
    {
        // A name of the page's own: the preview never replaces the cooked shader a scene draws.
        auto* render = Services().host != nullptr ? Services().host->Ctx().GetSubsystem<engine::render::RenderSubsystem>() : nullptr;
        m_previewShader.Attach(render != nullptr ? render->Shaders() : nullptr, Format(u8"editor.preview.{}", instance.Id()));
        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"shader.preview");
            ShaderEditorPage* self = this;
            m_pane->OnFrame = [self]() { self->FramePreview(); };
            if (PreviewViewport* preview = m_pane->Viewport())
            {
                // The scene's clock runs only while it simulates, and Time() reads it: a surface that
                // scrolls or a Vertex() that moves would stand still otherwise.
                preview->SetSimulationEnabled(true);
                preview->Camera().position = Float3{0.0f, 0.9f, 2.6f};
                preview->Camera().LookAt(Float3{0.0f, 0.0f, 0.0f});
            }
        }
        m_staleNote = Find<gui::Node>(u8"shader-stale");
        BuildPreviewScene();
        OpenCode(u8"hlsl"); // checked at once: the page opens with the shader's state and its preview
    }

    ShaderEditorPage::~ShaderEditorPage()
    {
        if (m_compiler != nullptr)
            m_compiler->Destroy();
    }

    UniquePtr<EditorPage> ShaderEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<ShaderEditorPage>(context, m_services->Make(), instance, m_shaderLibrary.AsView());
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void ShaderEditorPage::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        app::CodeAssetPage::OnUpdate(host, dt);
        if (m_pane)
            m_pane->Update(dt);
    }

    void ShaderEditorPage::OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void ShaderEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
        m_previewShader.Release(); // before the renderer goes: the page is destroyed after it
    }

    void ShaderEditorPage::FramePreview()
    {
        if (m_pane && m_pane->Viewport() != nullptr)
            m_pane->Viewport()->Frame(Float3{0.0f, 0.0f, 0.0f}, 1.0f);
    }

    void ShaderEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        if (scenePtr == nullptr)
        {
            return;
        }
        m_sphere = scenePtr->CreateEntity(u8"PreviewSphere");
        m_previewMesh = foundation::geometry::Primitives::Sphere(Allocator(), 1.0f, 48, 24);
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            engine::render::MeshComponent& mc = meshes->Add(m_sphere);
            mc.mesh = m_previewMesh.Get(); // direct override (runtime-built, not an asset)
        }
        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                     Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, t);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.castsShadows = false; // a lone sphere has nothing to shadow
        }
    }

    Array<app::CodeProblem> ShaderEditorPage::CheckCode(StringView text)
    {
        if (m_compiler == nullptr && (!shaders::createCompiler(shaders::CompilerDesc{}, m_compiler).IsOk() || m_compiler == nullptr))
            m_compiler = nullptr;
        const auto* asset = ObjectAs<pipeline::ShaderAsset>();
        pipeline::ShaderCheckOptions options;
        if (asset != nullptr)
        {
            options.vertexEntryPoint = asset->vertexEntryPoint.AsView();
            options.fragmentEntryPoint = asset->fragmentEntryPoint.AsView();
        }
        options.shaderLibrary = m_shaderLibrary.AsView();
        options.depth = pipeline::ShaderCheckDepth::BaseVariants;
        options.compiler = m_compiler;
        const pipeline::ShaderCheck check = pipeline::CheckShaderSource(text, SourceFileName(), options);

        // This file's errors on their lines; an include's named by its file; the whole file's at its top.
        Array<app::CodeProblem> problems;
        for (const pipeline::ShaderDiagnostic& d : check.errors)
        {
            app::CodeProblem p;
            p.severity = gui::CodeSeverity::Error;
            p.message = d.message;
            p.file = d.file.AsView() == SourceFileName() ? String{} : d.file;
            p.line = d.line > 0 ? d.line - 1 : 0;
            p.atColumn = d.line > 0 && d.column > 0;
            p.column = d.column > 0 ? d.column - 1 : 0;
            problems.PushBack(Move(p));
        }
        if (check.IsOk())
        {
            m_kind = String(check.surface == pipeline::SurfaceModel::Lit     ? StringView(u8"Lit surface shader")
                            : check.surface == pipeline::SurfaceModel::Unlit ? StringView(u8"Unlit surface shader")
                                                                             : StringView(u8"Low-level shader"));
            m_properties = check.block.properties.Size();
            m_compiled = true;
            UpdatePreview(check);
        }
        else
            m_kind.Clear();
        // The preview keeps the last code that compiled, and says so while the code is not it.
        m_stale = !check.IsOk() && m_compiled;
        if (m_staleNote != nullptr)
            m_staleNote->SetDisplayed(m_stale);
        if (m_pane)
        {
            if (m_compiled)
                m_pane->Show();
            else
                m_pane->ShowEmpty(u8"Nothing to preview yet", u8"It shows once the code compiles.");
        }
        return problems;
    }

    void ShaderEditorPage::UpdatePreview(const pipeline::ShaderCheck& check)
    {
        scene::Scene* scenePtr = m_pane && m_pane->Viewport() != nullptr ? m_pane->Viewport()->Scene() : nullptr;
        if (!m_previewShader.IsHeld() || scenePtr == nullptr || !m_sphere.IsAssigned())
        {
            return;
        }
        // The main program as the cook would ship it (a surface shader's forward program),
        // under the page's own name, with its entries and lattices.
        const pipeline::ShaderCheckProgram& main = check.programs[0];
        const StringView name = m_previewShader.Name();
        m_previewShader.Register(main.text.AsView(), main.vertexEntryPoint.AsView(), main.vertexMask,
                                 main.fragmentEntryPoint.AsView(), main.fragmentMask);

        // A material laid out by the block as a cooked material on this shader is, at its
        // defaults.
        shaders::ShaderSource shader;
        shader.name = String(name);
        shader.SetProperties(check.block);
        materials::MaterialSource authored;
        authored.name = String(u8"preview");
        materials::MaterialSource laidOut;
        pipeline::LayOutMaterialByShader(authored, shader, laidOut);
        m_previewMaterial = materials::MaterialFromSource(Allocator(), laidOut, name);
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            if (engine::render::MeshComponent* mc = meshes->Get(m_sphere))
            {
                mc->SetMaterial(RefPtr<materials::Material>(m_previewMaterial.Get()));
            }
        }
    }

    void PreviewShaderSlot::Register(StringView hlsl, StringView vertexEntry,
                                     shaders::ShaderFlags vertexMask, StringView fragmentEntry,
                                     shaders::ShaderFlags fragmentMask)
    {
        if (m_shaders == nullptr)
        {
            return;
        }
        const StringView name = m_name.AsView();
        m_shaders->RegisterSource(name, shaders::ShaderStage::Vertex, hlsl, vertexEntry, vertexMask);
        m_shaders->RegisterSource(name, shaders::ShaderStage::Fragment, hlsl, fragmentEntry,
                                  fragmentMask);
        m_shaders->InvalidateShader(name);
    }

    void PreviewShaderSlot::Release()
    {
        if (m_shaders != nullptr && !m_name.IsEmpty())
        {
            m_shaders->RemoveSource(m_name.AsView());
            m_shaders->InvalidateShader(m_name.AsView());
        }
        m_shaders = nullptr;
    }
}

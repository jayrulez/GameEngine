// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Shaders - the `:shader_page` partition: a custom shader's page.
//
// The shader's .hlsl on the GUI from its markup (Data/Editor/ShaderPage.gml;
// Documentation/Specs/editor-gui.md, 5.4, layer 6, part 4), on the code pages' base (the source
// file, the shared HLSL colours, one history, the problems list): checked a moment after each edit
// (CheckShaderSource on each stage's base variant), its file's errors on their lines and an
// include's in the list; beside it a sphere drawn with the code being edited, its material laid out
// by the properties block exactly as a cooked material is, over the asset's grid (its name and entry
// points, each edit one undo step with the code's). While the code does not compile the preview
// keeps the last version that did, and says so. Save writes the file and the asset and recooks,
// which compiles every variant; the preview draws under a name of its own, so a scene keeps the
// last cooked shader until then.

module;
#include "Core/Prelude.h"

export module editor.shaders:shader_page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.geometry;
import foundation.shaders;
import foundation.shaders.system;
import foundation.materials;
import foundation.gui;
import engine.render;
import shaders.pipeline;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;

    // A page's preview shader in the renderer's shader system, under the page's own name:
    // registered each time the text checks, removed once when the page closes. Released, it
    // forgets the system: at editor exit a page outlives the renderer (OnClose runs, the runtime
    // context shuts the renderer down, then the pages are destroyed), so whatever releases again
    // must not reach it.
    class PreviewShaderSlot
    {
    public:
        PreviewShaderSlot() = default;
        ~PreviewShaderSlot() { Release(); }
        PreviewShaderSlot(const PreviewShaderSlot&) = delete;
        PreviewShaderSlot& operator=(const PreviewShaderSlot&) = delete;

        // Whether it still holds the system (not yet released, and there was one).
        [[nodiscard]] bool IsHeld() const noexcept { return m_shaders != nullptr; }
        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }

        // The system to register in (null: none, the slot does nothing) and the name to use.
        void Attach(foundation::shaders::ShaderSystem* shaders, String name)
        {
            Release();
            m_shaders = shaders;
            m_name = Move(name);
        }
        // One program's text for both stages, with their entry points and lattices; a draw on
        // the name compiles it anew.
        void Register(StringView hlsl, StringView vertexEntry,
                      foundation::shaders::ShaderFlags vertexMask, StringView fragmentEntry,
                      foundation::shaders::ShaderFlags fragmentMask);
        // Removes the sources and the compiled variants, then lets go of the system.
        void Release();

    private:
        foundation::shaders::ShaderSystem* m_shaders = nullptr; // the renderer's; borrowed
        String m_name;
    };

    class ShaderEditorPage final : public app::CodeAssetPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ShaderPage.gml";

        /// `shaderLibrary`: the engine's Shaders/ folder, the include path a check compiles with.
        ShaderEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance, StringView shaderLibrary);
        ~ShaderEditorPage() override;

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        // === For tests ===
        /// What the last check said the shader is ("Lit surface shader", "Low-level shader"), empty while it does not compile.
        [[nodiscard]] StringView Kind() const noexcept { return m_kind.AsView(); }
        /// The properties block's count, as the last check that compiled read it.
        [[nodiscard]] usize PropertyCount() const noexcept { return m_properties; }
        /// Whether the preview shows an older version than the code (the code does not compile).
        [[nodiscard]] bool PreviewStale() const noexcept { return m_stale; }

    protected:
        [[nodiscard]] Array<app::CodeProblem> CheckCode(StringView text) override;
        void OnObjectChanged() override { CheckNow(); } // an entry point changes what compiles

    private:
        void BuildPreviewScene();
        void UpdatePreview(const pipeline::ShaderCheck& check);
        void FramePreview();

        PreviewShaderSlot m_previewShader; // what the sphere draws with
        String m_shaderLibrary;
        foundation::shaders::Compiler* m_compiler = nullptr; // reused by every check
        UniquePtr<app::PreviewPane> m_pane;
        foundation::scene::EntityHandle m_sphere{};
        RefPtr<foundation::geometry::StaticMesh> m_previewMesh;
        RefPtr<foundation::materials::Material> m_previewMaterial;
        foundation::gui::Node* m_staleNote = nullptr;
        String m_kind;
        usize m_properties = 0;
        bool m_compiled = false; // the preview shows a version that compiled
        bool m_stale = false;
    };

    class ShaderEditorPageFactory final : public IEditorPageFactory
    {
    public:
        ShaderEditorPageFactory(const app::AssetPageServicesSource& services, StringView shaderLibrary) : m_services(&services), m_shaderLibrary(shaderLibrary) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override { return &pipeline::ShaderAsset::StaticType(); }
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
        String m_shaderLibrary;
    };

    /// The shader page's registration (from the editor's assembly). `shaderLibrary`: the engine's
    /// Shaders/ folder under the data root.
    inline void RegisterShaderEditor(EditorContext& context, const app::AssetPageServicesSource& services, StringView shaderLibrary)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<ShaderEditorPageFactory>(services, shaderLibrary), editor::EditorRootAllocator()));
    }
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.ModelViewer - a standalone viewer for model files (Documentation/Specs/model-viewer.md):
// open a glTF, GLB, FBX or OBJ file without a project and look at it in a lit 3D view with the
// editor's camera. The app is the engine's default application (its scene, render and animation
// subsystems) with a GUI of its own on the window (the editor's look: its sheet, its density, the
// viewer's rules after them), the view a preview viewport in it. A model opens through the
// `modelviewer` library: cooked into the viewer's cache, bound, put into the preview scene.
//
//   Tools.ModelViewer [model files...] [--screenshot <png> [--screenshot-exit] ...]
//
// P0: the window, the view, one model from the command line, framed, on a grid scaled to it.

#include <cmath>
#include <cstdio>
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

import foundation.core;
import foundation.shell;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.rhi;
import foundation.runtime;
import foundation.runtime.client;
import foundation.runtime.desktop;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.harfbuzz;
import foundation.fonts.bidi;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.toolkit;
import foundation.gui.vfs;
import foundation.scene;
import foundation.render;
import foundation.model.resource;
import foundation.geometry;
import engine.defaultapp;
import engine.render;
import engine.model;
import editor.preview.viewport;
import modelviewer;

using namespace foundation::core;
namespace shell = foundation::shell;
namespace graphics = foundation::graphics;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace vfs = foundation::vfs;
namespace fonts = foundation::fonts;
namespace gui = foundation::gui;
namespace scene = foundation::scene;
namespace render = foundation::render;

namespace
{
    constexpr const char8_t* kMarkupPath = u8"ModelViewer/Viewer.xml";
    constexpr const char8_t* kSheets[] = {u8"Editor/editor.css", u8"Editor/density-default.css", u8"ModelViewer/viewer.css"};

    class ModelViewerApp final : public engine::runtime::DefaultApplication
    {
    public:
        void AddFile(StringView path) { m_files.PushBack(String(path)); }

        void OnStartup(runtime::IApplicationHost& host) override
        {
            DefaultApplication::OnStartup(host);
            m_host = &host;
            m_window = host.MainRenderWindow();
            if (m_window == nullptr || host.Graphics() == nullptr || host.Graphics()->Raw() == nullptr || host.Shell() == nullptr)
            {
                std::fprintf(stderr, "Tools.ModelViewer: no window or device\n");
                host.RequestExit(1);
                return;
            }
            m_device = host.Graphics()->Raw();
            if (!OpenGui(host))
            {
                host.RequestExit(1);
                return;
            }
            m_viewport = MakeUnique<editor::PreviewViewport>(m_allocator, m_allocator, host, m_guiHost.Get(), u8"model.viewer");
            m_viewport->SetSimulationEnabled(true); // the animation plays
            if (gui::Node* stage = FindById(u8"viewer-stage"))
                stage->AddChild(m_viewport->View().Get());
            SetUpScene();
            m_cacheRoot = PathJoin(GetUserDataDirectory().AsView(), u8"model-viewer/cache");
            if (!m_files.IsEmpty())
                OpenFile(m_files[0].AsView());
            ShowEmpty();
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 deltaTime) override
        {
            DefaultApplication::OnUpdate(host, deltaTime); // a recorded screenshot written, --screenshot-exit
            if (!m_guiHost)
                return;
            if (m_open.Resources() != nullptr)
                m_open.Resources()->Pump();
            m_guiHost->Update(deltaTime);
            if (m_viewport->Update(deltaTime))
                FrameModel();
            DrawGrid();
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (!m_guiHost || !frame.valid)
                return;
            if (frame.window == m_window && frame.encoder != nullptr)
            {
                if (auto* render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>(); render != nullptr && render->IsReady())
                {
                    render->BeginRendering(*frame.encoder, frame.frameIndex);
                    m_viewport->RenderFrame(frame);
                    render->EndRendering();
                }
            }
            m_guiHost->RenderWindow(frame);
            if (frame.window == m_window)
                FinishFrame(host, frame); // --screenshot: this frame as presented, the GUI included
        }

        void OnShutdown(runtime::IApplicationHost& host) override
        {
            if (m_viewport)
                m_viewport->Shutdown();
            m_viewport.Reset();
            m_open.Close();
            // The GUI's tree (the viewport's render targets in it) goes before its host and the device.
            m_frame = {};
            m_root = {};
            m_guiHost.Reset();
            m_provider.Reset();
            m_fonts.Reset();
            m_shaders.Reset();
            DefaultApplication::OnShutdown(host);
        }

    private:
        // The window's GUI: the GUI's shaders, the faces, the host, the editor's sheet with the
        // viewer's after it, the frame from the viewer's markup.
        bool OpenGui(runtime::IApplicationHost& host)
        {
            m_dataRoot = String(DataRoot());
            if (m_dataRoot.IsEmpty())
            {
                std::fprintf(stderr, "Tools.ModelViewer: no data folder\n");
                return false;
            }
            m_data = MakeUnique<vfs::NativeFileSystem>(m_allocator, m_dataRoot.AsView(), m_allocator);
            m_shaders = MakeUnique<gui::VgShaderLibrary>(m_allocator, m_allocator);
            if (!m_shaders->Load(*m_device, *m_data))
            {
                std::fprintf(stderr, "Tools.ModelViewer: the GUI's shaders did not load\n");
                return false;
            }
            gui::RegisterGuiElements();
            gui::RegisterToolkitElements();
            fonts::HarfBuzzShaping::Install();
            fonts::BidiAlgorithm::Install();
            m_fonts = MakeUnique<fonts::TrueTypeFontService>(m_allocator, m_allocator);
            m_provider = MakeUnique<gui::vfs::VfsResourceProvider>(m_allocator, m_data.Get(), m_allocator);
            m_guiHost = MakeUnique<gui::GuiHost>(m_allocator, m_allocator, *host.Graphics(), *host.Shell(), m_fonts.Get(), m_shaders->Set());
            gui::StyleManager& styles = m_guiHost->Styles();
            styles.SetResourceProvider(m_provider.Get());
            m_guiHost->Context().SetResourceProvider(m_provider.Get());
            ModelViewerApp* self = this;
            styles.SetFontFaceLoader([self](StringView face, StringView source) { return self->LoadFontFace(face, source); });
            const String theme = Format(u8"{}\n{}", gui::DefaultThemeCSS(), gui::ToolkitSheetCSS());
            styles.SetUserAgentSheet(gui::CSSParser::Parse(theme.AsView(), StringView(u8"theme")));
            m_guiHost->Context().SetColorScheme(true); // dark, as the editor is by default
            String sheet;
            for (const char8_t* path : kSheets)
            {
                String part;
                if (m_provider->LoadText(StringView(path), part))
                {
                    sheet += part.AsView();
                    sheet += u8"\n";
                }
                else
                    LOG_WARNING(u8"ModelViewer", u8"the sheet {} is not there", StringView(path));
            }
            styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView(), StringView(u8"ModelViewer/viewer.css")));

            m_root = MakeRef<gui::SceneRoot>(m_allocator, m_guiHost->ContextRef());
            String markup;
            if (!m_provider->LoadText(StringView(kMarkupPath), markup))
            {
                std::fprintf(stderr, "Tools.ModelViewer: its window's markup is not there\n");
                return false;
            }
            gui::MarkupLoader loader(gui::ElementSet::All(), m_allocator, nullptr, m_fonts.Get());
            loader.SetDataModel(m_guiHost->Context().GetDataRef());
            m_frame = loader.LoadFromString(markup.AsView());
            if (!m_frame)
            {
                std::fprintf(stderr, "Tools.ModelViewer: its window's markup did not load\n");
                return false;
            }
            m_root->AddChild(m_frame.Get());
            m_guiHost->AttachWindow(*m_window, m_root);
            return true;
        }

        // A face the sheet's @font-face names, from its file under the data root (the editor's own way).
        bool LoadFontFace(StringView face, StringView source)
        {
            fonts::FontLoadOptions options = fonts::FontLoadOptions::ExtendedLatin();
            options.pixelHeight = 16.0f;
            options.lastCodepoint = 0x17F;
            if (m_fonts->LoadFont(face, PathJoin(m_dataRoot.AsView(), source).AsView(), options) == fonts::FontLoadResult::Success)
                return true;
            LOG_ERROR(u8"ModelViewer", u8"the @font-face {} ({}) did not load", face, source);
            return false;
        }

        // The preview scene's light: the procedural sky (its image lighting) and a sun with shadows.
        void SetUpScene()
        {
            scene::Scene* level = m_viewport->Scene();
            if (level == nullptr)
                return;
            if (auto* environment = level->GetSystem<engine::render::EnvironmentSystem>())
            {
                engine::render::EnvironmentSettings& e = environment->Environment();
                e.skyMode = render::SkyMode::Procedural;
                e.skyIntensity = 1.0f;
                e.ambientIntensity = 0.3f;
            }
            if (auto* lights = level->GetSystem<engine::render::LightComponentManager>())
            {
                const scene::EntityHandle sun = level->CreateEntity(u8"sun");
                Transform t;
                t.rotation = FromYawPitchRoll(0.6f, -0.9f, 0.0f); // from the side, down at 50 degrees
                level->SetLocalTransform(sun, t);
                engine::render::LightComponent& light = lights->Add(sun);
                light.type = engine::render::LightType::Directional;
                light.intensity = 2.5f;
                light.castsShadows = true;
            }
        }

        // A model file opened into the view: cooked or taken from the cache, put into the scene,
        // framed. What failed is said where the empty view is.
        void OpenFile(StringView path)
        {
            if (m_instance.root.IsAssigned() && m_viewport->Scene() != nullptr)
                m_viewport->Scene()->DestroyEntity(m_instance.root);
            m_instance = {};
            const modelviewer::OpenModel::Outcome outcome = m_open.Open(path, m_cacheRoot.AsView(), m_device);
            if (!m_open.IsOpen())
            {
                m_failure = Format(u8"{} did not open ({}).", path, OutcomeText(outcome));
                LOG_ERROR(u8"ModelViewer", u8"{}", m_failure.AsView());
                return;
            }
            m_failure = {};
            LOG_INFO(u8"ModelViewer", u8"opened {} ({})", path, OutcomeText(outcome));
            m_instance = engine::model::InstantiateModel(*m_viewport->Scene(), *m_open.Model(), u8"model");
            MeasureBounds();
            if (auto* file = FindAs<gui::Label>(u8"viewer-file"))
                file->SetText(path);
            FrameModel(/*snap*/ true);
        }

        [[nodiscard]] static StringView OutcomeText(modelviewer::OpenModel::Outcome outcome)
        {
            switch (outcome)
            {
            case modelviewer::OpenModel::Outcome::Opened: return u8"cooked";
            case modelviewer::OpenModel::Outcome::FromCache: return u8"from the cache";
            case modelviewer::OpenModel::Outcome::NotFound: return u8"the file is not there";
            case modelviewer::OpenModel::Outcome::LoadFailed: return u8"the file could not be read";
            case modelviewer::OpenModel::Outcome::CookFailed: return u8"it did not cook";
            case modelviewer::OpenModel::Outcome::BindFailed: return u8"it did not load";
            }
            return u8"";
        }

        // The empty view while nothing is open: how to open one, or why the last did not.
        void ShowEmpty()
        {
            gui::Node* empty = FindById(u8"viewer-empty");
            if (empty == nullptr)
                return;
            const bool open = m_open.IsOpen();
            empty->SetDisplayed(!open);
            m_viewport->View()->SetDisplayed(open);
            if (auto* text = FindAs<gui::Label>(u8"viewer-empty-text"); text != nullptr && !m_failure.IsEmpty())
                text->SetText(m_failure.AsView());
        }

        // The model's bounds as placed: each mesh's box through its entity's world (a file's node
        // scale, 0.01 or 100, is in it), measured when the model opens.
        void MeasureBounds()
        {
            m_center = Float3{};
            m_radius = 1.0f;
            scene::Scene* level = m_viewport->Scene();
            auto* meshes = level != nullptr ? level->GetSystem<engine::render::MeshComponentManager>() : nullptr;
            if (meshes == nullptr || !m_instance.root.IsAssigned())
                return;
            AABB box = AABB::Empty();
            meshes->ForEach([&](engine::render::MeshComponent& component, scene::EntityHandle entity)
                            {
                                const foundation::geometry::StaticMesh* mesh = component.mesh.Get();
                                if (mesh == nullptr || !mesh->bounds.IsValid())
                                    return;
                                box = Merge(box, TransformAABB(mesh->bounds, level->ComposeWorldMatrix(entity)));
                            });
            if (!box.IsValid())
                return;
            m_center = (box.min + box.max) * 0.5f;
            m_radius = Max(Length(box.max - box.min) * 0.5f, 0.01f);
        }

        void Bounds(Float3& center, f32& radius) const
        {
            center = m_center;
            radius = m_radius;
        }

        void FrameModel(bool snap = false)
        {
            Float3 center;
            f32 radius;
            Bounds(center, radius);
            // The clip range follows the model: a 2 cm prop and a 200 m building both in reach. The
            // near plane at a twentieth of its size keeps the scene's reverse-Z depth well above the
            // grid's fixed bias toward the viewer, which a nearer plane lets the grid win through.
            editor::EditorCamera& camera = m_viewport->Camera();
            camera.nearZ = Max(radius * 0.05f, 0.0001f);
            camera.farZ = Max(radius * 100.0f, 50.0f);
            m_viewport->Frame(center, radius);
            if (snap)
                m_viewport->Camera().Advance(1000.0f); // straight there, no glide on opening
        }

        // The ground grid, its cells sized to the model (a tenth of its size, to a power of ten)
        // and fading at twenty times it, so a 2 cm prop and a 200 m building both sit on one.
        void DrawGrid()
        {
            if (!m_viewport || !m_viewport->IsValid())
                return;
            Float3 center;
            f32 radius;
            Bounds(center, radius);
            render::debug::GridPlaneDesc grid;
            grid.spacing = std::pow(10.0f, std::floor(std::log10(Max(radius * 0.2f, 0.001f))));
            grid.fadeDistance = radius * 20.0f;
            grid.extent = grid.fadeDistance;
            grid.cameraPosition = m_viewport->Camera().position;
            grid.origin = Float3{grid.cameraPosition.x, 0.0f, grid.cameraPosition.z};
            m_viewport->SceneDebugDraw().DrawGridPlane(grid);
        }

        [[nodiscard]] gui::Node* FindById(StringView id) const { return m_frame ? FindIn(*m_frame, id) : nullptr; }
        template <typename T>
        [[nodiscard]] T* FindAs(StringView id) const
        {
            return Cast<T>(FindById(id));
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        IAllocator& m_allocator = DefaultAllocator();
        runtime::IApplicationHost* m_host = nullptr;
        graphics::RenderWindow* m_window = nullptr;
        rhi::Device* m_device = nullptr;
        String m_dataRoot;
        String m_cacheRoot;
        Array<String> m_files;
        UniquePtr<vfs::NativeFileSystem> m_data;
        UniquePtr<gui::VgShaderLibrary> m_shaders;
        UniquePtr<fonts::TrueTypeFontService> m_fonts;
        UniquePtr<gui::vfs::VfsResourceProvider> m_provider;
        UniquePtr<gui::GuiHost> m_guiHost;
        RefPtr<gui::SceneRoot> m_root;
        RefPtr<gui::Node> m_frame;
        UniquePtr<editor::PreviewViewport> m_viewport;
        modelviewer::OpenModel m_open{m_allocator};
        engine::model::ModelInstance m_instance;
        Float3 m_center{};
        f32 m_radius = 1.0f;
        String m_failure;
    };
}

int main(int argc, char** argv)
{
    ModelViewerApp app;
    for (int i = 1; i < argc; ++i)
    {
        const StringView arg(reinterpret_cast<const utf8char*>(argv[i]));
        if (arg.Size() > 0 && arg[0] == u8'-')
        {
            if (arg.StartsWith(u8"--screenshot") && i + 1 < argc && arg != StringView(u8"--screenshot-exit"))
                ++i; // the option's value (read by ScreenshotOptionsFromArguments)
            continue;
        }
        app.AddFile(arg);
    }
    app.SetScreenshotOptions(rhi::ScreenshotOptionsFromArguments(argc, argv));

    shell::WindowSettings settings;
    settings.title = u8"Model Viewer";
    settings.width = 1280;
    settings.height = 800;
    UniquePtr<shell::IShell> shellPtr = shell::CreateShell(DefaultAllocator(), settings);
    if (shellPtr.Get() == nullptr || shellPtr->MainWindow() == nullptr)
        return 1;
    Result<UniquePtr<graphics::GraphicsDevice>> device = graphics::CreateGraphicsDevice(graphics::DeviceDescFromArguments(argc, argv));
    if (!device.HasValue())
        return 1;
    return runtime::RunApplication(app, *shellPtr, device.Value().Get());
}

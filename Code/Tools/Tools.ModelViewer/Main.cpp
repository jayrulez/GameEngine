// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.ModelViewer - a standalone viewer for model files: open a glTF, GLB, FBX or OBJ file
// without a project and look at it in a lit 3D view with the editor's camera. The app is the
// engine's default application (its scene, render and animation subsystems) with its own GUI on
// the window, styled like the editor (the editor's sheet and density, then the viewer's rules),
// and the 3D view is a preview viewport inside it. A model opens through the `modelviewer`
// library: cooked into the viewer's cache, bound, and put into the preview scene. Each open model
// has a tab (modelviewer::ModelTabs). Its instance stays in the scene but is active only while its
// tab is in front, and the tab restores the camera it was left with.
//
//   Tools.ModelViewer [model files...] [--rest] [--skeleton] [--mcp | --mcp-port <port>]
//                     [--screenshot <png> [--screenshot-exit] ...]
//   Tools.ModelViewer --headless
//
// --rest shows the model in its rest pose with no clip playing; --skeleton starts with the bones
// drawn. --mcp serves the viewer's commands to an agent over HTTP (port 7406, or the --mcp-port
// value), with the bearer token in mcp-token in the user data folder; --headless serves them over
// stdio with no window. The agent's manual is ModelViewerGuide.md.
//
// The window has a Browse tab (a folder tree, filters, and a thumbnail per model, drawn by
// modelviewer::ThumbnailRenderer), a Library tab (collections, tags, search), and a tab per open
// model with its side panel, animation bar (modelviewer::ClipPlayback) and skeleton overlay. Every
// control runs one of the window's commands (WindowCommandSpecs), the same commands an agent runs
// over MCP, and the views refresh from the library's change notifications whoever made the change.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

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
import foundation.gui.viewport;
import foundation.rhi.capture;
import foundation.image;
import foundation.image.io;
import foundation.scene;
import foundation.render;
import foundation.model.resource;
import foundation.geometry;
import foundation.animation;
import engine.defaultapp;
import engine.render;
import engine.model;
import editor.preview.viewport;
import modelviewer;
import modelviewer.thumbnails;
import modelviewer.preferences;
import foundation.json;
import foundation.mcp;
import foundation.mcp.http;

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

extern "C" const char* BuildStamp(); // defined in Runtime.Client: this build's identity

namespace modelviewer_window
{
    namespace gui = foundation::gui;

    /// One model's cell in a grid, drawn like the editor's asset tiles: the thumbnail (a glyph until
    /// one is drawn), a corner mark when the model is in the library and a badge when its file is
    /// missing, then its name, and below that its member name in the collection shown.
    class ModelCell final : public gui::UIWidget
    {
        RTTI_OBJECT(ModelCell, gui::UIWidget)
    public:
        struct Shown
        {
            String name;
            String sub;
            bool inLibrary = false;
            bool missing = false;
            RefPtr<gui::Drawable> picture;
        };

        ModelCell()
        {
            AddClass(u8"model-cell");
            m_thumb = Part<gui::Panel>(*this, u8"model-thumb");
            m_glyph = Part<gui::Image>(*m_thumb, u8"model-glyph");
            m_glyph->SetSource(String(u8"Editor/Icons/cube.svg"));
            m_picture = Part<gui::Image>(*m_thumb, u8"model-picture");
            m_mark = Part<gui::Panel>(*m_thumb, u8"model-mark");
            m_mark->SetTooltip(u8"In the library");
            m_mark->SetHitTestVisible(true); // so its tooltip shows
            m_badge = Part<gui::Label>(*m_thumb, u8"badge");
            m_badge->AddClass(u8"danger");
            m_badge->SetText(u8"Missing");
            m_name = Part<gui::Label>(*this, u8"model-name");
            m_sub = Part<gui::Label>(*this, u8"model-sub");
        }

        void Show(const Shown& shown)
        {
            m_picture->SetDrawable(shown.picture);
            m_picture->SetDisplayed(shown.picture.Get() != nullptr);
            m_glyph->SetDisplayed(shown.picture.Get() == nullptr);
            m_mark->SetDisplayed(shown.inLibrary);
            m_badge->SetDisplayed(shown.missing);
            m_name->SetText(shown.name.AsView());
            m_sub->SetText(shown.sub.AsView());
            m_sub->SetDisplayed(!shown.sub.IsEmpty());
            SetTooltip(shown.name.AsView());
        }

    private:
        template <typename T>
        RefPtr<T> Part(gui::Node& into, StringView cls)
        {
            RefPtr<T> part = MakeRef<T>(MemoryAllocator());
            part->AddClass(cls);
            part->SetHitTestVisible(false);
            into.AddChild(part.Get());
            return part;
        }

        RefPtr<gui::Panel> m_thumb;
        RefPtr<gui::Image> m_glyph;
        RefPtr<gui::Image> m_picture;
        RefPtr<gui::Panel> m_mark;
        RefPtr<gui::Label> m_badge;
        RefPtr<gui::Label> m_name;
        RefPtr<gui::Label> m_sub;
    };
    RTTI_DEFINE_OBJECT(ModelCell, "rtti::modelviewer::window")

    /// A grid row: one model file, with the path's hash as its id so a selection survives a refill.
    struct GridRow
    {
        String path;
        String sub;     // its member name in the collection shown
        bool inLibrary = false;
        bool missing = false;
        Guid entry;     // its library entry, if any
    };

    class GridModel final : public gui::IModel
    {
    public:
        Array<GridRow> rows;

        [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : rows.Size(); }
        [[nodiscard]] usize ColumnCount() const override { return 1; }
        [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
        {
            return role == gui::ModelRole::Display && Valid(index.Row) ? gui::Variant(rows[static_cast<usize>(index.Row)].path.AsView()) : gui::Variant{};
        }
        [[nodiscard]] gui::ItemId IdOf(const gui::ModelIndex& index) const override
        {
            return Valid(index.Row) ? IdOfPath(rows[static_cast<usize>(index.Row)].path.AsView()) : gui::kNoItem;
        }
        [[nodiscard]] gui::ModelIndex IndexOfId(gui::ItemId id) const override
        {
            for (usize i = 0; i < rows.Size(); ++i)
                if (IdOfPath(rows[i].path.AsView()) == id)
                    return gui::ModelIndex{static_cast<i32>(i), 0};
            return {};
        }
        [[nodiscard]] static gui::ItemId IdOfPath(StringView path) { return Hash<StringView>{}(path) & ~(1ull << 63); }
        [[nodiscard]] bool Valid(i32 row) const noexcept { return row >= 0 && static_cast<usize>(row) < rows.Size(); }
        [[nodiscard]] const GridRow* RowOfId(gui::ItemId id) const
        {
            const gui::ModelIndex index = IndexOfId(id);
            return Valid(index.Row) ? &rows[static_cast<usize>(index.Row)] : nullptr;
        }
        void Reset() { NotifyReset(); }
        void Changed()
        {
            if (!rows.IsEmpty())
                NotifyRowsChanged(gui::ModelIndex{}, 0, static_cast<i32>(rows.Size()));
        }
    };
}

namespace
{
    constexpr u16 kDefaultMcpPort = 7406; // the editor uses 7405, so both can run at once

    [[nodiscard]] String LibraryFolder() { return PathJoin(GetUserDataDirectory().AsView(), u8"model-viewer"); }

    // The bearer token an agent presents: mcp-token in the user data folder, shared with the
    // editor, generated from system entropy when it does not exist yet.
    [[nodiscard]] String McpToken()
    {
        const String path = PathJoin(GetUserDataDirectory().AsView(), u8"mcp-token");
        if (Result<Array<byte>> bytes = ReadFile(path.AsView()); bytes.HasValue())
        {
            String token(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            while (!token.IsEmpty() && (token[token.Size() - 1] == u8'\n' || token[token.Size() - 1] == u8'\r' || token[token.Size() - 1] == u8' '))
                token = String(token.AsView().SubStr(0, token.Size() - 1));
            if (!token.IsEmpty())
                return token;
        }
        Guid secret;
        if (!Guid::TryGenerateFromSystemEntropy(secret))
            return {};
        utf8char text[37];
        secret.ToChars(text);
        const String token(StringView(text, 36));
        (void)CreateDirectories(GetUserDataDirectory().AsView());
        (void)WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(token.Data()), token.Size()));
        return token;
    }

    // The viewer's folder in the user data folder: it holds the settings and, by default, the cache.
    [[nodiscard]] String ViewerDataFolder() { return PathJoin(GetUserDataDirectory().AsView(), u8"model-viewer"); }
    [[nodiscard]] String SettingsPath() { return PathJoin(ViewerDataFolder().AsView(), u8"settings.txt"); }
    // Where the cooked models and thumbnails go, per the settings.
    [[nodiscard]] String CacheBase(const modelviewer::ViewerSettings& settings)
    {
        return modelviewer::CacheBaseOf(settings, ViewerDataFolder().AsView());
    }

    // Opens the library in the user data folder and registers its commands, the settings commands
    // and viewer.info; cache.clear is refused while `busy` returns a reason.
    void OpenLibrary(modelviewer::LibrarySession& library, StringView mode, modelviewer::SettingsStore& settings, Function<String()> busy)
    {
        String error;
        if (!library.Open(LibraryFolder().AsView(), error))
            LOG_ERROR(u8"ModelViewer", u8"the library did not open: {}", error.AsView());
        else if (library.ReadOnly())
            LOG_WARNING(u8"ModelViewer", u8"the library is open read-only: another viewer (process {}) writes it", library.File().LockHolder());
        modelviewer::RegisterLibraryCommands(library);
        const String base = CacheBase(settings.value);
        const String cacheFolders[] = {PathJoin(base.AsView(), u8"cache"), PathJoin(base.AsView(), u8"thumbnails")};
        modelviewer::RegisterSettingsCommands(library.Commands(), settings, Span<const String>(cacheFolders, 2), foundation::core::Move(busy));
        modelviewer::SettingsStore* kept = &settings;
        modelviewer::RegisterExportCommands(library, [kept]() { return kept->value.looseTextures; });
        const String modeName(mode);
        modelviewer::RegisterInfoCommand(library, LibraryFolder().AsView(),
                                         [modeName]()
                                         {
                                             foundation::json::JsonValue v = foundation::json::JsonValue::MakeObject();
                                             v.Set(String(u8"process"), foundation::json::JsonValue::MakeNumber(static_cast<f64>(ProcessId())));
                                             v.Set(String(u8"version"), foundation::json::JsonValue::MakeString(String(reinterpret_cast<const char8_t*>(BuildStamp()))));
                                             v.Set(String(u8"mode"), foundation::json::JsonValue::MakeString(modeName));
                                             return v;
                                         });
    }

    // Serves the commands as the MCP server's tools, along with the guide.
    void ServeCommands(foundation::mcp::McpServer& server, modelviewer::LibrarySession& library)
    {
        server.SetServerInfo(String(u8"model-viewer"), String(reinterpret_cast<const char8_t*>(BuildStamp())));
        modelviewer::RegisterCommandTools(server, library.Commands());
        const String executable = ExecutablePath();
        const String starts[] = {String(PathParent(executable.AsView())), GetCurrentDirectory()};
        const String guide = modelviewer::LocateGuide(Span<const String>(starts, 2));
        if (guide.IsEmpty())
            LOG_WARNING(u8"ModelViewer", u8"the agent's guide ({}) was not found", modelviewer::kGuideFileName);
        modelviewer::RegisterGuideResource(server, guide.AsView());
    }

    // Sends engine logs to stderr, since under --headless stdout carries the protocol.
    class StderrSink final : public ILogSink
    {
    public:
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            const String line = Format(u8"[{}] {}: {}\n", LogLevelName(level), category, message);
            std::fwrite(line.Data(), 1, line.Size(), stderr);
            std::fflush(stderr);
        }
    };

    // --headless: serves the library's commands over stdio, with no window, until the input ends.
    int RunHeadless()
    {
        static StderrSink sink;
        GlobalLogger().AddSink(&sink);
        modelviewer::LibrarySession library(DefaultAllocator());
        modelviewer::SettingsStore settings;
        settings.Open(SettingsPath().AsView());
        modelviewer::LibrarySession* held = &library;
        OpenLibrary(library, u8"headless", settings,
                    [held]()
                    {
                        // A windowed viewer that holds the library is using the cache too.
                        return held->ReadOnly()
                                   ? Format(u8"another viewer (process {}) is using the cache: clear it there", held->File().LockHolder())
                                   : String{};
                    });
        modelviewer::RegisterWindowlessCommands(library.Commands());
        foundation::mcp::McpServer server;
        ServeCommands(server, library);
        foundation::mcp::StdioTransport transport;
        foundation::mcp::Serve(server, transport);
        return 0;
    }

    constexpr const char8_t* kMarkupPath = u8"ModelViewer/Viewer.xml";
    constexpr StringView kDensities[] = {u8"compact", u8"default", u8"comfortable"}; // the editor's densities: Editor/density-<name>.css

    constexpr i32 kFixedTabs = 2; // Browse and Library, before the model tabs
    constexpr StringView kModelsDrag = u8"modelviewer.models"; // a drag of grid tiles carries their paths, one per line
    // The grid cell width for each tile size (small, medium, large): the thumbnail's side (from
    // viewer.css, .tiles-small and .tiles-large) plus room for padding and two lines of name.
    constexpr f32 kTileWidths[] = {120.0f, 152.0f, 216.0f};
    constexpr f32 kTileHeights[] = {152.0f, 184.0f, 248.0f};
    constexpr StringView kGridLists[] = {u8"viewer-grid", u8"viewer-lib-grid"};
    constexpr StringView kTileSliders[] = {u8"viewer-tile-size", u8"viewer-lib-tile-size"};

    enum class Page : u8
    {
        Model,
        Browse,
        Library,
    };

    /// The Library tab's tree model. Collection and tag rows rename in place: the view's commit
    /// hook runs the rename command and the tree is rebuilt from the library afterwards, so the
    /// model itself stores nothing.
    class LibraryTreeModel final : public gui::TreeModel
    {
    public:
        Function<bool(i32 node)> renamable;
        /// Whether a row is a collection that accepts dropped models (`kModelsDrag` paths). When it
        /// is unset, no row accepts them.
        Function<bool(i32 node)> takesModels;
        Function<void(i32 node, StringView paths)> dropModels;

        [[nodiscard]] bool IsEditable(const gui::ModelIndex& index) const override
        {
            return renamable && index.IsValid() && renamable(static_cast<i32>(index.InternalId));
        }
        bool SetData(const gui::ModelIndex&, const gui::Variant&) override { return true; }
        // Grid tiles dropped onto a collection's row join it.
        [[nodiscard]] gui::DropEffect CanDrop(const gui::ModelDrop& drop, String& reason) const override
        {
            if (drop.Payload == nullptr || drop.Payload->Type.AsView() != kModelsDrag)
                return gui::TreeModel::CanDrop(drop, reason);
            if (drop.Place == gui::DropPlace::On && drop.Index.IsValid() && takesModels && takesModels(static_cast<i32>(drop.Index.InternalId)))
                return gui::DropEffect::Copy;
            reason = String(u8"Drop the models on a collection");
            return gui::DropEffect::None;
        }
        bool Drop(const gui::ModelDrop& drop) override
        {
            if (drop.Payload == nullptr || drop.Payload->Type.AsView() != kModelsDrag)
                return false; // the tree's own rows cannot be dragged: the library decides where they are
            String ignored;
            if (CanDrop(drop, ignored) == gui::DropEffect::None || !dropModels)
                return false;
            dropModels(static_cast<i32>(drop.Index.InternalId), drop.Payload->Value.Get<String>().AsView());
            return true;
        }
    };

    /// What a row of the Library tab's tree stands for.
    struct LibraryNode
    {
        enum class Kind : u8
        {
            All,
            Unsorted,
            Collection,
        };
        explicit LibraryNode(Kind k = Kind::All) : kind(k) {}
        Kind kind;
        Guid collection;
    };

    class ModelViewerApp final : public engine::runtime::DefaultApplication
    {
    public:
        void AddFile(StringView path) { m_files.PushBack(String(path)); }
        /// --rest: the model in its rest pose, no clip playing.
        void SetRest(bool rest) { m_rest = rest; }
        /// --skeleton: the bones drawn from the start.
        void SetSkeleton(bool on) { m_showSkeleton = on; }
        /// --mcp / --mcp-port: serve the commands to an agent over HTTP.
        void SetMcp(u16 port) { m_mcpPort = port; }

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
            m_settings.Open(SettingsPath().AsView());
            if (!OpenGui(host))
            {
                host.RequestExit(1);
                return;
            }
            if (auto* text = FindAs<gui::Label>(u8"viewer-empty-text"))
                m_emptyText = String(text->GetText());
            m_viewport = MakeUnique<editor::PreviewViewport>(m_allocator, m_allocator, host, m_guiHost.Get(), u8"model.viewer");
            m_viewport->SetSimulationEnabled(true); // so the animation plays
            if (gui::Node* view = FindById(u8"viewer-view"))
                view->AddChild(m_viewport->View().Get());
            SetUpScene();
            // The cache folder and thumbnail size are read from the settings at startup only.
            const String cacheBase = CacheBase(m_settings.value);
            m_cacheRoot = PathJoin(cacheBase.AsView(), u8"cache");
            m_thumbnailSize = m_settings.value.thumbnailSize;
            m_thumbnailRoot = PathJoin(PathJoin(cacheBase.AsView(), u8"thumbnails").AsView(), Format(u8"{}", m_thumbnailSize).AsView());
            m_thumbnails = MakeUnique<modelviewer::ThumbnailRenderer>(m_allocator, m_allocator, host, m_cacheRoot.AsView(), m_thumbnailRoot.AsView(),
                                                                      m_thumbnailSize);
            ModelViewerApp* self = this;
            m_thumbnails->SetOnDone([self](StringView model, StringView png) { self->ThumbnailDone(model, png); });
            OpenLibrary(m_library, u8"window", m_settings, [self]() { return self->CacheBusy(); });
            m_settings.onChanged = [self]() { self->ApplySettings(); };
            RegisterWindowCommands();
            m_library.Commands().AddListener([self](StringView name)
                                             {
                                                 self->m_libraryStale = true;
                                                 if (name == StringView(u8"cache.clear"))
                                                     self->CacheCleared();
                                             });
            StartMcp();
            // The toasts stack over the window's corner, as the editor's do.
            m_toasts = MakeRef<gui::ToastHost>(m_allocator);
            m_frame->AddChild(m_toasts.Get());
            WireControls(host);
            if (gui::Node* popover = FindById(u8"viewer-light-pop"))
            {
                m_lightPopover = RefPtr<gui::Node>(popover);
                if (gui::Node* home = popover->GetParent())
                    home->RemoveChild(popover); // shown only as the Lighting button's popover
            }
            WireBrowser(host);
            if (auto* bar = FindAs<gui::TabBar>(u8"viewer-tabs"))
            {
                gui::Tab* browse = bar->AddTab(u8"Browse");
                browse->SetClosable(false);
                browse->SetTooltip(u8"The models under a folder, a picture each");
                gui::Tab* library = bar->AddTab(u8"Library");
                library->SetClosable(false);
                library->SetTooltip(u8"The models kept, in their collections, with their tags and names");
            }
            ApplySettings();
            // The view's lighting starts from the settings.
            JsonValue light = JsonValue::MakeObject();
            light.Set(String(u8"exposure"), Number(m_settings.value.exposure));
            light.Set(String(u8"ambient"), Number(m_settings.value.ambient));
            light.Set(String(u8"sky"), Number(m_settings.value.sky));
            Run(u8"view.light", light);
            WireLibrary(host);
            WireGrids();
            DefineViewerCommands();
            for (StringView id : kTileSliders)
                if (auto* slider = FindAs<gui::Slider>(id))
                    slider->SetOnValueChanging([self](f64 value)
                                               {
                                                   const i64 step = static_cast<i64>(value + 0.5);
                                                   if (!self->m_syncingTiles && static_cast<u32>(step) != self->m_settings.value.tileSize)
                                                       self->Run(u8"settings.set", Arg(u8"tileSize", Number(static_cast<f64>(step))));
                                               });
            ApplyTileSize(); // the grids exist now
            RefreshLibrary();
            for (const String& file : m_files)
                OpenFile(file.AsView());
            if (m_tabs.Count() == 0)
                ShowBrowser();
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 deltaTime) override
        {
            DefaultApplication::OnUpdate(host, deltaTime); // writes a recorded screenshot and handles --screenshot-exit
            if (!m_guiHost)
                return;
            for (i32 i = 0; i < m_tabs.Count(); ++i)
                if (m_tabs.At(i).open->Resources() != nullptr)
                    m_tabs.At(i).open->Resources()->Pump();
            m_guiHost->Update(deltaTime);
            if (m_thumbnails)
            {
                m_thumbnails->Update();
                ShowProgress();
            }
            TakeReads();
            if (m_mcpHost)
                (void)m_mcpHost->Pump();
            if (!m_browsing && m_viewport->Update(deltaTime))
                FrameModel();
            Turn(deltaTime);
            if (m_libraryStale)
                RefreshLibrary();
            ShowPlayback();
            DrawGrid();
            DrawSkeleton();
            FinishCapture();
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (!m_guiHost || !frame.valid)
                return;
            if (frame.window == m_window && frame.encoder != nullptr)
            {
                if (auto* render = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>(); render != nullptr && render->IsReady())
                {
                    render->BeginRendering(*frame.encoder, frame.frameIndex);
                    m_viewport->RenderFrame(frame);
                    RecordCapture(*frame.encoder);
                    if (m_thumbnails)
                        m_thumbnails->Render(*frame.encoder);
                    render->EndRendering();
                }
            }
            m_guiHost->RenderWindow(frame);
            if (frame.window == m_window)
                FinishFrame(host, frame); // --screenshot: captures this frame as presented, GUI included
        }

        void OnShutdown(runtime::IApplicationHost& host) override
        {
            if (m_device != nullptr)
                m_capture.Release(*m_device);
            if (m_thumbnails)
                m_thumbnails->Shutdown();
            m_thumbnails.Reset();
            m_mcpHost.Reset();
            const StringView grids[] = {u8"viewer-grid", u8"viewer-lib-grid"};
            for (const StringView id : grids)
                if (auto* list = FindAs<gui::ListView>(id))
                    list->SetModel(nullptr);
            m_thumbImages.Clear();
            if (m_viewport)
                m_viewport->Shutdown();
            m_viewport.Reset();
            m_tabs.Clear();
            // Release the GUI tree, which holds the viewport's render targets, before its host and the device.
            m_frame = {};
            m_root = {};
            m_guiHost.Reset();
            m_provider.Reset();
            m_fonts.Reset();
            m_shaders.Reset();
            DefaultApplication::OnShutdown(host);
        }

    private:
        // Applies the style sheets in order: the editor's, the density's (from the settings, else
        // default), then the viewer's.
        void ApplySheets()
        {
            m_density = String(u8"default");
            for (StringView density : kDensities)
                if (m_settings.value.density.AsView() == density)
                    m_density = String(density);
            const String paths[] = {String(u8"Editor/editor.css"), Format(u8"Editor/density-{}.css", m_density.AsView()), String(u8"ModelViewer/viewer.css")};
            String sheet;
            for (const String& path : paths)
            {
                String part;
                if (m_provider->LoadText(path.AsView(), part))
                {
                    sheet += part.AsView();
                    sheet += u8"\n";
                }
                else
                    LOG_WARNING(u8"ModelViewer", u8"the sheet {} is not there", path.AsView());
            }
            m_guiHost->Styles().SetStyleSheet(gui::CSSParser::Parse(sheet.AsView(), StringView(u8"ModelViewer/viewer.css")));
        }

        // Sets up the window's GUI: the GUI shaders, the fonts, the host, the style sheets, and the
        // frame loaded from the viewer's markup.
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
            ApplySheets();

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

        // Loads a font face named by the sheet's @font-face from its file under the data root, as
        // the editor does.
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

        // Lights the preview scene with the procedural sky (and its image lighting) and a sun with shadows.
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
                t.rotation = FromYawPitchRoll(0.6f, -0.9f, 0.0f); // from the side, pointing down at about 50 degrees
                level->SetLocalTransform(sun, t);
                engine::render::LightComponent& light = lights->Add(sun);
                light.type = engine::render::LightType::Directional;
                light.intensity = 2.5f;
                light.castsShadows = true;
            }
        }

        // === The window's commands (modelviewer::WindowCommandSpecs) ===
        //
        // Every control runs one of these through Run, and an agent runs the same ones through MCP.
        // Each command updates the controls it affects, so the window shows the result either way.

        using JsonValue = foundation::json::JsonValue;
        using CommandResult = modelviewer::CommandResult;

        /// Runs a command for a control. A refusal is logged and shown as a warning toast; an agent
        /// running the same command gets the refusal as its result.
        void Run(StringView name, const JsonValue& args = JsonValue::MakeObject())
        {
            Array<String> before;
            m_library.HistoryLabels(before);
            const CommandResult result = m_library.Commands().Execute(name, args);
            if (!result.ok)
            {
                LOG_WARNING(u8"ModelViewer", u8"{}: {}", name, result.error.AsView());
                // Show the refusal in the window, where the user is looking.
                Toast(gui::ToastSeverity::Warning, Capitalized(result.error.AsView()).AsView(), name);
                return;
            }
            // Announce a library change by its undo label, with an Undo button; an undo announces
            // what it undid.
            if (name == StringView(u8"history.undo"))
            {
                Toast(gui::ToastSeverity::Info, Format(u8"Undone: {}", result.value.Get(String(u8"undone")).AsString().AsView()).AsView(), u8"library");
                return;
            }
            Array<String> after;
            m_library.HistoryLabels(after);
            if (!after.IsEmpty() && (after.Size() != before.Size() || before.IsEmpty() || after[0] != before[0]) &&
                name != StringView(u8"entry.note.set")) // a note saves as it is typed, so no notice for each change
                Toast(gui::ToastSeverity::Success, Capitalized(after[0].AsView()).AsView(), u8"library", /*undo*/ true);
        }

        // Shows a notice in the window's corner, like the editor's toasts. Repeats with the same
        // `key` fold into one; `undo` adds a button that undoes the library's last change.
        void Toast(gui::ToastSeverity severity, StringView message, StringView key = {}, bool undo = false)
        {
            if (!m_toasts)
                return;
            gui::ToastRequest request;
            request.Message = String(message);
            request.Severity = severity;
            request.Key = String(key);
            if (!undo)
            {
                (void)m_toasts->Post(foundation::core::Move(request));
                return;
            }
            request.Actions.PushBack(gui::ToastAction{String(u8"undo"), String(u8"Undo"), String{}});
            ModelViewerApp* self = this;
            (void)m_toasts->Post(foundation::core::Move(request), [self](const gui::ToastAction&) { self->Run(u8"history.undo"); });
        }
        [[nodiscard]] static String Capitalized(StringView text)
        {
            String out(text);
            if (!out.IsEmpty() && out[0] >= u8'a' && out[0] <= u8'z')
                out[0] = static_cast<utf8char>(out[0] - (u8'a' - u8'A'));
            return out;
        }
        [[nodiscard]] static JsonValue Arg(StringView key, JsonValue value)
        {
            JsonValue a = JsonValue::MakeObject();
            a.Set(String(key), foundation::core::Move(value));
            return a;
        }
        [[nodiscard]] static JsonValue Text(StringView text) { return JsonValue::MakeString(String(text)); }
        [[nodiscard]] static JsonValue Number(f64 n) { return JsonValue::MakeNumber(n); }
        [[nodiscard]] static JsonValue Flag(bool b) { return JsonValue::MakeBool(b); }

        [[nodiscard]] JsonValue TabsJson()
        {
            JsonValue v = JsonValue::MakeObject();
            JsonValue tabs = JsonValue::MakeArray();
            for (i32 i = 0; i < m_tabs.Count(); ++i)
            {
                JsonValue t = JsonValue::MakeObject();
                t.Set(String(u8"tab"), Number(i));
                t.Set(String(u8"path"), Text(m_tabs.At(i).path.AsView()));
                t.Set(String(u8"front"), Flag(!m_browsing && i == m_tabs.Active()));
                t.Set(String(u8"ready"), Flag(m_tabs.At(i).ready));
                tabs.Add(foundation::core::Move(t));
            }
            v.Set(String(u8"tabs"), foundation::core::Move(tabs));
            v.Set(String(u8"browsing"), Flag(m_page == Page::Browse));
            v.Set(String(u8"library"), Flag(m_page == Page::Library));
            return v;
        }
        [[nodiscard]] JsonValue LightJson()
        {
            JsonValue v = JsonValue::MakeObject();
            if (auto* post = Post())
                v.Set(String(u8"exposure"), Number(post->Post().exposureEV));
            if (auto* environment = Environment())
            {
                v.Set(String(u8"ambient"), Number(environment->Environment().ambientIntensity));
                v.Set(String(u8"sky"), Number(environment->Environment().skyIntensity));
            }
            v.Set(String(u8"turntable"), Flag(m_turntable));
            v.Set(String(u8"skeleton"), Flag(m_showSkeleton));
            return v;
        }
        [[nodiscard]] JsonValue PlaybackJson(const modelviewer::ClipPlayback& p)
        {
            JsonValue v = JsonValue::MakeObject();
            v.Set(String(u8"clip"), Text(p.ClipName(p.Selected()).AsView()));
            v.Set(String(u8"seconds"), Number(p.Time()));
            v.Set(String(u8"length"), Number(p.Duration()));
            v.Set(String(u8"playing"), Flag(p.Playing()));
            v.Set(String(u8"loop"), Flag(p.Looping()));
            v.Set(String(u8"speed"), Number(p.Speed()));
            return v;
        }
        /// The playback of the front model, or null with `why` set.
        [[nodiscard]] modelviewer::ClipPlayback* FrontPlayback(String& why)
        {
            modelviewer::ModelTab* tab = m_browsing ? nullptr : m_tabs.ActiveTab();
            if (tab == nullptr)
            {
                why = String(u8"no model is in front");
                return nullptr;
            }
            if (!tab->ready)
            {
                why = Format(u8"{} is still cooking", tab->title.AsView());
                return nullptr;
            }
            if (tab->playback.Get() == nullptr || !tab->playback->HasClips())
            {
                why = Format(u8"{} has no clips", tab->title.AsView());
                return nullptr;
            }
            return tab->playback.Get();
        }
        /// The tab an argument names, else the front tab; -1 when there is none.
        [[nodiscard]] i32 TabArg(const JsonValue& a) const
        {
            if (a.Has(String(u8"tab")) && !a.Get(String(u8"tab")).IsNull())
                return static_cast<i32>(a.Get(String(u8"tab")).AsInt(-1));
            return m_tabs.Active();
        }

        void RegisterWindowCommands()
        {
            ModelViewerApp* self = this;
            usize registered = 0;
            auto reg = [&](StringView name, Function<CommandResult(const JsonValue&)> run)
            {
                for (const modelviewer::WindowCommandSpec& spec : modelviewer::WindowCommandSpecs())
                    if (spec.name == name)
                    {
                        (void)m_library.Commands().Register(modelviewer::MakeWindowCommand(spec, foundation::core::Move(run)));
                        ++registered;
                        return;
                    }
                LOG_ERROR(u8"ModelViewer", u8"no window command named {} in the table", name);
            };

            reg(u8"model.open", [self](const JsonValue& a)
                {
                    const String path = a.Get(String(u8"path")).AsString();
                    self->OpenFile(path.AsView());
                    const i32 tab = self->m_tabs.Find(path.AsView());
                    if (tab < 0)
                        return CommandResult::Fail(self->m_failure.AsView());
                    JsonValue v = JsonValue::MakeObject();
                    v.Set(String(u8"tab"), Number(tab));
                    v.Set(String(u8"ready"), Flag(self->m_tabs.At(tab).ready));
                    return CommandResult::Ok(foundation::core::Move(v));
                });
            reg(u8"model.close", [self](const JsonValue& a)
                {
                    const i32 tab = self->TabArg(a);
                    if (!self->m_tabs.Valid(tab))
                        return CommandResult::Fail(u8"no such tab");
                    self->CloseTab(tab);
                    return CommandResult::Ok(self->TabsJson());
                });
            reg(u8"tabs.list", [self](const JsonValue&) { return CommandResult::Ok(self->TabsJson()); });
            reg(u8"tabs.select", [self](const JsonValue& a)
                {
                    const i32 tab = static_cast<i32>(a.Get(String(u8"tab")).AsInt(-3));
                    if (tab == -1)
                        self->ShowBrowser();
                    else if (tab == -2)
                        self->ShowLibrary();
                    else if (self->m_tabs.Valid(tab))
                        self->ShowTab(tab);
                    else
                        return CommandResult::Fail(u8"no such tab");
                    return CommandResult::Ok(self->TabsJson());
                });
            reg(u8"view.screenshot", [self](const JsonValue& a)
                {
                    const modelviewer::ModelTab* tab = self->m_browsing ? nullptr : self->m_tabs.ActiveTab();
                    if (tab == nullptr)
                        return CommandResult::Fail(u8"no model is in front to take a picture of");
                    if (!tab->ready)
                        return CommandResult::Fail(Format(u8"{} is still cooking: ask tabs_list until it is ready", tab->title.AsView()).AsView());
                    String path = a.Get(String(u8"path")).AsString();
                    if (path.IsEmpty())
                    {
                        const String folder = modelviewer::ScreenshotFolderOf(self->m_settings.value);
                        (void)CreateDirectories(folder.AsView());
                        path = PathJoin(folder.AsView(), self->ScreenshotNameFor(*tab).AsView());
                    }
                    self->m_capture.Request(path.AsView());
                    JsonValue v = JsonValue::MakeObject();
                    v.Set(String(u8"file"), Text(path.AsView()));
                    return CommandResult::Ok(foundation::core::Move(v));
                });
            reg(u8"view.camera", [self](const JsonValue& a)
                {
                    modelviewer::ModelTab* tab = self->m_browsing ? nullptr : self->m_tabs.ActiveTab();
                    if (tab == nullptr || !tab->ready)
                        return CommandResult::Fail(tab == nullptr ? u8"no model is in front" : u8"the model in front is still cooking");
                    editor::EditorCamera& camera = self->m_viewport->Camera();
                    if (a.Has(String(u8"yaw")) || a.Has(String(u8"pitch")))
                    {
                        // At the requested angle, as close as the model still fits the view.
                        camera.yaw = static_cast<f32>(a.Get(String(u8"yaw")).AsNumber(0.0)) * kPi / 180.0f;
                        camera.pitch = static_cast<f32>(a.Get(String(u8"pitch")).AsNumber(-15.0)) * kPi / 180.0f;
                        const f32 distance = tab->radius / Sin(camera.fovY * 0.5f) * 1.1f;
                        camera.position = tab->center - camera.Forward() * distance;
                        camera.focusDistance = distance;
                        self->ClipToModel(tab->radius);
                    }
                    else
                        self->FrameModel(/*snap*/ true);
                    return CommandResult::Ok();
                });
            reg(u8"view.light", [self](const JsonValue& a)
                {
                    auto number = [&](StringView key, f64 lo, f64 hi, auto apply)
                    {
                        if (a.Has(String(key)) && a.Get(String(key)).IsNumber())
                            apply(Clamp(a.Get(String(key)).AsNumber(), lo, hi));
                    };
                    self->m_syncingView = true;
                    number(u8"exposure", -4.0, 4.0, [self](f64 v)
                           {
                               if (auto* post = self->Post())
                                   post->Post().exposureEV = static_cast<f32>(v);
                               if (auto* slider = self->FindAs<gui::Slider>(u8"viewer-exposure"))
                                   slider->SetValue(v);
                           });
                    number(u8"ambient", 0.0, 2.0, [self](f64 v)
                           {
                               if (auto* environment = self->Environment())
                                   environment->Environment().ambientIntensity = static_cast<f32>(v);
                               if (auto* slider = self->FindAs<gui::Slider>(u8"viewer-ambient"))
                                   slider->SetValue(v);
                           });
                    number(u8"sky", 0.0, 3.0, [self](f64 v)
                           {
                               if (auto* environment = self->Environment())
                                   environment->Environment().skyIntensity = static_cast<f32>(v);
                               if (auto* slider = self->FindAs<gui::Slider>(u8"viewer-sky"))
                                   slider->SetValue(v);
                           });
                    if (a.Has(String(u8"turntable")))
                    {
                        self->SetTurntable(a.Get(String(u8"turntable")).AsBool());
                        if (auto* toggle = self->FindAs<gui::Button>(u8"viewer-turntable-toggle"))
                            toggle->SetChecked(self->m_turntable);
                    }
                    if (a.Has(String(u8"skeleton")))
                    {
                        self->m_showSkeleton = a.Get(String(u8"skeleton")).AsBool();
                        if (auto* toggle = self->FindAs<gui::Button>(u8"viewer-skeleton-toggle"))
                            toggle->SetChecked(self->m_showSkeleton);
                    }
                    self->m_syncingView = false;
                    return CommandResult::Ok(self->LightJson());
                });
            reg(u8"clip.list", [self](const JsonValue&)
                {
                    String why;
                    modelviewer::ClipPlayback* p = self->FrontPlayback(why);
                    if (p == nullptr)
                        return CommandResult::Fail(why.AsView());
                    JsonValue clips = JsonValue::MakeArray();
                    for (i32 i = 0; i < p->ClipCount(); ++i)
                        clips.Add(Text(p->ClipName(i).AsView()));
                    JsonValue v = self->PlaybackJson(*p);
                    v.Set(String(u8"clips"), foundation::core::Move(clips));
                    return CommandResult::Ok(foundation::core::Move(v));
                });
            reg(u8"clip.select", [self](const JsonValue& a)
                {
                    String why;
                    modelviewer::ClipPlayback* p = self->FrontPlayback(why);
                    if (p == nullptr)
                        return CommandResult::Fail(why.AsView());
                    const String name = a.Get(String(u8"clip")).AsString();
                    for (i32 i = 0; i < p->ClipCount(); ++i)
                        if (p->ClipName(i) == name)
                        {
                            p->Select(i);
                            self->ShowClips(*self->m_tabs.ActiveTab());
                            return CommandResult::Ok();
                        }
                    return CommandResult::Fail(Format(u8"no clip named {} (clip.list says which there are)", name.AsView()).AsView());
                });
            reg(u8"clip.play", [self](const JsonValue& a)
                {
                    String why;
                    modelviewer::ClipPlayback* p = self->FrontPlayback(why);
                    if (p == nullptr)
                        return CommandResult::Fail(why.AsView());
                    if (a.Has(String(u8"loop")))
                        p->SetLooping(a.Get(String(u8"loop")).AsBool());
                    if (a.Has(String(u8"speed")) && a.Get(String(u8"speed")).IsNumber())
                        p->SetSpeed(static_cast<f32>(Clamp(a.Get(String(u8"speed")).AsNumber(), 0.0, 8.0)));
                    if (a.Has(String(u8"playing")))
                        p->SetPlaying(a.Get(String(u8"playing")).AsBool());
                    self->ShowClips(*self->m_tabs.ActiveTab());
                    return CommandResult::Ok(self->PlaybackJson(*p));
                });
            reg(u8"clip.scrub", [self](const JsonValue& a)
                {
                    String why;
                    modelviewer::ClipPlayback* p = self->FrontPlayback(why);
                    if (p == nullptr)
                        return CommandResult::Fail(why.AsView());
                    p->SetPlaying(false);
                    p->Scrub(static_cast<f32>(a.Get(String(u8"seconds")).AsNumber(0.0)));
                    return CommandResult::Ok(self->PlaybackJson(*p));
                });
            reg(u8"browse.folder", [self](const JsonValue& a)
                {
                    const String path = a.Get(String(u8"path")).AsString();
                    if (path.IsEmpty() || !DirectoryExists(path.AsView()))
                        return CommandResult::Fail(Format(u8"{} is not a folder", path.AsView()).AsView());
                    self->ShowBrowser();
                    self->SetBrowseRoot(path.AsView());
                    self->SaveSettings();
                    JsonValue v = JsonValue::MakeObject();
                    v.Set(String(u8"models"), Number(static_cast<f64>(self->m_folderModels.Size())));
                    return CommandResult::Ok(foundation::core::Move(v));
                });
            reg(u8"thumbnail.get", [self](const JsonValue& a)
                {
                    const String path = a.Get(String(u8"path")).AsString();
                    const String png = modelviewer::ThumbnailPathFor(self->m_thumbnailRoot.AsView(), path.AsView());
                    if (png.IsEmpty())
                        return CommandResult::Fail(Format(u8"{} is not there", path.AsView()).AsView());
                    JsonValue v = JsonValue::MakeObject();
                    if (FileExists(png.AsView()))
                        v.Set(String(u8"file"), Text(png.AsView()));
                    else
                    {
                        self->m_thumbnails->Prepend(path.AsView());
                        v.Set(String(u8"queued"), Flag(true));
                    }
                    return CommandResult::Ok(foundation::core::Move(v));
                });
            if (registered != modelviewer::WindowCommandSpecs().Size())
                LOG_ERROR(u8"ModelViewer", u8"{} of the {} window commands have no handler", modelviewer::WindowCommandSpecs().Size() - registered,
                          modelviewer::WindowCommandSpecs().Size());
        }

        // Wires the toolbar buttons, the tabs, the panel's controls, and files dropped on the window.
        void WireControls(runtime::IApplicationHost& host)
        {
            ModelViewerApp* self = this;
            if (auto* tabs = FindAs<gui::TabBar>(u8"viewer-tabs"))
            {
                // The bar's first two tabs are Browse and Library; a model's bar index is its index in
                // m_tabs plus kFixedTabs.
                tabs->SetOnSelected([self](i32 index)
                                    {
                                        // tabs.select takes -1 for Browse, -2 for Library, and models from 0.
                                        if (!self->m_syncingTabs)
                                            self->Run(u8"tabs.select", Arg(u8"tab", Number(index == 0 ? -1 : index == 1 ? -2 : index - kFixedTabs)));
                                    });
                tabs->SetCloseHandler([self](i32 index)
                                      {
                                          if (index >= kFixedTabs)
                                              self->Run(u8"model.close", Arg(u8"tab", Number(index - kFixedTabs)));
                                      });
                tabs->SetOnTabMoved([self, tabs](i32 from, i32 to)
                                    {
                                        if (self->m_syncingTabs)
                                            return;
                                        if (from < kFixedTabs || to < kFixedTabs)
                                        {
                                            // Browse and Library stay first: a drag involving them is undone.
                                            self->m_syncingTabs = true;
                                            tabs->MoveTab(to, from);
                                            self->m_syncingTabs = false;
                                            return;
                                        }
                                        (void)self->m_tabs.Move(from - kFixedTabs, to - kFixedTabs);
                                    });
            }
            if (auto* open = FindAs<gui::Button>(u8"viewer-open"))
                open->SetOnClick([self, &host]() { self->ShowOpenDialog(host); });
            if (auto* shot = FindAs<gui::Button>(u8"viewer-screenshot"))
                shot->SetOnClick([self, &host]()
                                 {
                                     if (self->m_settings.value.askScreenshot)
                                         self->ShowScreenshotDialog(host);
                                     else
                                         self->Run(u8"view.screenshot");
                                 });
            if (auto* recent = FindAs<gui::Button>(u8"viewer-recent"))
                recent->SetOnClick([self, recent]() { self->ShowRecentMenu(*recent); });
            if (auto* preferences = FindAs<gui::Button>(u8"viewer-preferences"))
                preferences->SetOnClick([self]() { self->ShowPreferences(); });
            auto light = [self](StringView key, JsonValue value)
            {
                if (!self->m_syncingView)
                    self->Run(u8"view.light", Arg(key, foundation::core::Move(value)));
            };
            if (auto* exposure = FindAs<gui::Slider>(u8"viewer-exposure"))
                exposure->SetOnValueChanging([light](f64 value) { light(u8"exposure", Number(value)); });
            if (auto* ambient = FindAs<gui::Slider>(u8"viewer-ambient"))
                ambient->SetOnValueChanging([light](f64 value) { light(u8"ambient", Number(value)); });
            if (auto* sky = FindAs<gui::Slider>(u8"viewer-sky"))
                sky->SetOnValueChanging([light](f64 value) { light(u8"sky", Number(value)); });
            if (auto* turntable = FindAs<gui::Button>(u8"viewer-turntable-toggle"))
                turntable->SetOnClick([light, turntable]() { light(u8"turntable", Flag(turntable->IsChecked())); });
            if (auto* skeleton = FindAs<gui::Button>(u8"viewer-skeleton-toggle"))
            {
                skeleton->SetChecked(m_showSkeleton);
                skeleton->SetOnClick([light, skeleton]() { light(u8"skeleton", Flag(skeleton->IsChecked())); });
            }
            if (auto* lighting = FindAs<gui::Button>(u8"viewer-light"))
                lighting->SetOnClick([self, lighting]() { self->ShowLightPopover(*lighting); });
            WireAnimationBar();
            m_guiHost->SetDropObserver([self](const shell::DroppedFile& file)
                                       {
                                           if (modelviewer::IsModelFile(file.path.AsView()))
                                               self->Run(u8"model.open", Arg(u8"path", Text(file.path.AsView())));
                                       });
        }

        [[nodiscard]] engine::render::EnvironmentSystem* Environment() const
        {
            scene::Scene* level = m_viewport ? m_viewport->Scene() : nullptr;
            return level != nullptr ? level->GetSystem<engine::render::EnvironmentSystem>() : nullptr;
        }
        [[nodiscard]] engine::render::PostProcessSystem* Post() const
        {
            scene::Scene* level = m_viewport ? m_viewport->Scene() : nullptr;
            return level != nullptr ? level->GetSystem<engine::render::PostProcessSystem>() : nullptr;
        }

        void ShowOpenDialog(runtime::IApplicationHost& host)
        {
            shell::IDialogService* dialogs = host.Shell() != nullptr ? host.Shell()->Dialogs() : nullptr;
            if (dialogs == nullptr)
                return;
            const shell::FileFilter filters[] = {{u8"Models", modelviewer::kModelExtensions}, {u8"All files", u8"*"}};
            ModelViewerApp* self = this;
            dialogs->ShowOpenFile([self](Span<const String> paths)
                                  {
                                      for (const String& path : paths)
                                          self->Run(u8"model.open", Arg(u8"path", Text(path.AsView())));
                                  },
                                  Span<const shell::FileFilter>(filters, 2), {}, true);
        }

        // Saves the 3D view (without the window's panels) to a PNG chosen in the save dialog.
        void ShowScreenshotDialog(runtime::IApplicationHost& host)
        {
            shell::IDialogService* dialogs = host.Shell() != nullptr ? host.Shell()->Dialogs() : nullptr;
            const modelviewer::ModelTab* tab = m_tabs.ActiveTab();
            if (dialogs == nullptr || tab == nullptr)
                return;
            const shell::FileFilter filters[] = {{u8"PNG images", u8"png"}};
            const String folder = modelviewer::ScreenshotFolderOf(m_settings.value);
            (void)CreateDirectories(folder.AsView());
            const String suggested = PathJoin(folder.AsView(), ScreenshotNameFor(*tab).AsView());
            ModelViewerApp* self = this;
            dialogs->ShowSaveFile([self](Span<const String> paths)
                                  {
                                      if (!paths.IsEmpty())
                                          self->Run(u8"view.screenshot", Arg(u8"path", Text(paths[0].AsView())));
                                  },
                                  Span<const shell::FileFilter>(filters, 1), suggested.AsView());
        }

        // When a capture is armed, copies the view's colour target after the frame has drawn it.
        void RecordCapture(rhi::CommandEncoder& encoder)
        {
            if (!m_capture.Armed() || m_device == nullptr)
                return;
            auto* view = Cast<gui::Viewport>(m_viewport->View().Get());
            if (view == nullptr || view->ColorTexture() == nullptr || view->RenderWidth() == 0)
                return;
            (void)m_capture.Record(*m_device, encoder, view->ColorTexture(), view->ColorFormat(), view->RenderWidth(),
                                   view->RenderHeight(), rhi::ResourceState::ShaderRead);
        }

        // Writes the copy recorded last frame, once the GPU has run it.
        void FinishCapture()
        {
            if (!m_capture.Recorded() || m_device == nullptr)
                return;
            m_device->WaitIdle();
            foundation::image::Image written;
            const Status saved = m_capture.Complete(*m_device, m_allocator, written);
            if (saved.IsOk())
                LOG_INFO(u8"ModelViewer", u8"saved the view, {}x{}", written.Width(), written.Height());
        }

        // Opens a model file in a new tab, or brings its tab to the front when it is already open.
        // The model is cooked or taken from the cache, put into the scene and framed. A failure is
        // shown in the empty view and logged.
        void OpenFile(StringView path)
        {
            SaveCamera();
            const modelviewer::ModelTabs::Opened opened = m_tabs.Begin(path, m_cacheRoot.AsView(), m_device, m_settings.value.looseTextures);
            if (opened.index < 0)
            {
                m_failure = Format(u8"{} did not open ({}).", path, OutcomeText(opened.outcome));
                LOG_ERROR(u8"ModelViewer", u8"{}", m_failure.AsView());
                ShowEmpty();
                return;
            }
            m_failure = {};
            NoteRecent(false, m_tabs.At(opened.index).path.AsView());
            if (opened.added)
            {
                modelviewer::ModelTab& tab = m_tabs.At(opened.index);
                if (auto* bar = FindAs<gui::TabBar>(u8"viewer-tabs"))
                {
                    gui::Tab* added = bar->AddTab(tab.title.AsView());
                    added->SetClosable(true);
                    added->SetTooltip(tab.path.AsView());
                }
                if (opened.needsFile)
                    m_tabLoads.Request(tab.path.AsView(), m_settings.value.looseTextures); // the tab shows "cooking" until the read arrives
                else
                    PlaceModel(tab, opened.outcome);
            }
            ShowTab(opened.index);
        }

        // The viewer's own commands in the GUI's command model, each with a label, keys and an
        // action, for the shortcuts. Most press the toolbar's button or run the window command
        // that already exists for it, so a key and a click take the same path.
        struct ViewerCommand
        {
            StringView id;
            StringView label;
            StringView keys; // in the keymap's spelling ("ctrl-o"); empty for none
        };
        static constexpr ViewerCommand kViewerCommands[] = {
            {u8"viewer.open", u8"Open...", u8"ctrl-o"},
            {u8"viewer.close-tab", u8"Close Tab", u8"ctrl-w"},
            {u8"viewer.screenshot", u8"Screenshot", {}},
            {u8"viewer.preferences", u8"Preferences...", {}},
            {u8"viewer.quit", u8"Quit", u8"ctrl-q"},
            {u8"viewer.next-tab", u8"Next Tab", u8"ctrl-tab"},
            {u8"viewer.previous-tab", u8"Previous Tab", u8"ctrl-shift-tab"},
            {u8"viewer.find", u8"Find", u8"ctrl-f"},
            {u8"viewer.play", u8"Play or Pause", u8"space"},
            {u8"viewer.step-back", u8"Step Back a Frame", u8"left"},
            {u8"viewer.step-forward", u8"Step Forward a Frame", u8"right"},
            {u8"viewer.frame", u8"Frame the Model", u8"f"},
        };

        // Presses one of the window's buttons, as a click would.
        void Press(StringView id)
        {
            if (auto* button = FindAs<gui::Button>(id); button != nullptr && button->IsTreeEnabled())
                (void)button->HandleActivate();
        }

        void DefineViewerCommands()
        {
            gui::GUIContext& context = m_guiHost->Context();
            ModelViewerApp* self = this;
            for (const ViewerCommand& c : kViewerCommands)
            {
                context.DefineCommand(c.id).SetLabel(c.label);
                if (!c.keys.IsEmpty())
                    (void)context.GetKeymap().Bind(c.keys, c.id);
            }
            auto on = [self](StringView id, Function<void()> run)
            {
                (void)self->m_root->OnCommand(id, [action = foundation::core::Move(run)](gui::CommandEvent&) { action(); });
            };
            on(u8"viewer.open", [self]() { self->Press(u8"viewer-open"); });
            on(u8"viewer.close-tab", [self]()
               {
                   if (!self->m_browsing && self->m_tabs.ActiveTab() != nullptr)
                       self->Run(u8"model.close");
               });
            on(u8"viewer.screenshot", [self]() { self->Press(u8"viewer-screenshot"); });
            on(u8"viewer.preferences", [self]() { self->ShowPreferences(); });
            on(u8"viewer.quit", [self]()
               {
                   if (self->m_host != nullptr)
                       self->m_host->RequestExit(0);
               });
            on(u8"viewer.next-tab", [self]() { self->StepTab(1); });
            on(u8"viewer.previous-tab", [self]() { self->StepTab(-1); });
            on(u8"viewer.find", [self]()
               {
                   // Focus the front page's search field (a model's page has none).
                   if (self->m_page == Page::Model)
                       return;
                   if (auto* field = self->FindAs<gui::TextField>(self->m_page == Page::Library ? StringView(u8"viewer-lib-search")
                                                                                                   : StringView(u8"viewer-filter-name")))
                       field->RequestFocus();
               });
            on(u8"viewer.play", [self]() { self->Press(u8"viewer-play"); });
            on(u8"viewer.step-back", [self]() { self->Press(u8"viewer-step-back"); });
            on(u8"viewer.step-forward", [self]() { self->Press(u8"viewer-step"); });
            on(u8"viewer.frame", [self]()
               {
                   if (self->m_page == Page::Model)
                       self->Run(u8"view.camera", Arg(u8"frame", Flag(true)));
               });
            if (auto* clear = FindAs<gui::Button>(u8"viewer-tags-clear"))
                clear->SetOnClick([self]()
                                  {
                                      self->m_activeTags.Clear();
                                      self->ShowTagChips();
                                      self->ShowLibraryGrid();
                                  });
            // The tab bar's previous and next tab. The model tab's own bar: frame, and toggle the side panel.
            if (auto* frame = FindAs<gui::Button>(u8"viewer-frame"))
                frame->SetOnClick([self]() { self->Run(u8"view.camera", Arg(u8"frame", Flag(true))); });
            if (auto* previous = FindAs<gui::Button>(u8"viewer-tab-previous"))
                previous->SetOnClick([self]() { self->StepTab(-1); });
            if (auto* next = FindAs<gui::Button>(u8"viewer-tab-next"))
                next->SetOnClick([self]() { self->StepTab(1); });
            if (auto* toggle = FindAs<gui::Button>(u8"viewer-panel-toggle"))
                toggle->SetOnClick([self, toggle]() { self->ShowSidePanel(toggle->IsChecked()); });
            // The toolbar's grid toggle changes the same preference Preferences sets.
            if (auto* grid = FindAs<gui::Button>(u8"viewer-grid-toggle"))
                grid->SetOnClick([self, grid]()
                                 {
                                     if (!self->m_syncingPanel)
                                         self->Run(u8"settings.set", Arg(u8"grid", Flag(grid->IsChecked())));
                                 });
        }

        // Shows or hides a model tab's side panel, which is a pane of the splitter. Hiding takes the
        // pane out and gives its space to the view; showing puts it back.
        void ShowSidePanel(bool shown)
        {
            auto* body = FindAs<gui::Splitter>(u8"viewer-model-body");
            if (body == nullptr || shown == m_panelShown)
                return;
            m_panelShown = shown;
            if (!shown)
            {
                m_sidePanel = RefPtr<gui::Node>(FindById(u8"viewer-panel"));
                if (m_sidePanel)
                    body->RemoveChild(m_sidePanel.Get());
                return;
            }
            if (m_sidePanel)
                body->AddChild(m_sidePanel.Get());
            m_sidePanel = {};
            if (m_tabs.ActiveTab() != nullptr && !m_browsing)
                ShowTab(m_tabs.Active()); // refresh what changed while it was hidden
        }

        // Shows the lighting sliders as a popover under the toolbar's Lighting button. Like a menu,
        // it closes on a click elsewhere or Escape.
        void ShowLightPopover(gui::Node& anchor)
        {
            gui::EventDispatcher* dispatcher = m_root ? m_root->GetEventDispatcher() : nullptr;
            if (!m_lightPopover || dispatcher == nullptr || dispatcher->IsPopupOpen(*m_lightPopover))
                return;
            gui::PopupOptions options;
            options.Kind = gui::PopupKind::Auto;
            options.Anchor = &anchor;
            options.Placement.Side = gui::PopupSide::Below;
            options.Placement.Align = gui::PopupAlign::Start;
            options.Placement.Gap = 4.0f;
            options.FitContent = true;
            dispatcher->OpenPopup(*m_root, *m_lightPopover, Move(options));
        }


        // Saves a collection's CSV to a file chosen in the save dialog.
        void ExportCsvDialog(StringView collection)
        {
            shell::IDialogService* dialogs = m_host != nullptr && m_host->Shell() != nullptr ? m_host->Shell()->Dialogs() : nullptr;
            Guid id;
            const modelviewer::Collection* c = Guid::TryParse(collection, id) ? m_library.Get().FindCollection(id) : nullptr;
            if (dialogs == nullptr || c == nullptr)
                return;
            const shell::FileFilter filters[] = {{u8"CSV files", u8"csv"}};
            ModelViewerApp* self = this;
            const String target(collection);
            dialogs->ShowSaveFile([self, target](Span<const String> paths)
                                  {
                                      if (paths.IsEmpty())
                                          return;
                                      JsonValue a = Arg(u8"id", Text(target.AsView()));
                                      a.Set(String(u8"path"), Text(paths[0].AsView()));
                                      const CommandResult result = self->m_library.Commands().Execute(u8"export.csv", a);
                                      if (result.ok)
                                          self->Toast(gui::ToastSeverity::Success,
                                                      Format(u8"Exported {} rows to {}", result.value.Get(String(u8"rows")).AsInt(), modelviewer::FileTitle(paths[0].AsView()).AsView()).AsView());
                                      else
                                          self->Toast(gui::ToastSeverity::Error, Format(u8"Not exported: {}", result.error.AsView()).AsView());
                                  },
                                  Span<const shell::FileFilter>(filters, 1), Format(u8"{}.csv", modelviewer::export_detail::SafeName(c->name.AsView()).AsView()).AsView());
        }

        // Copies a collection's files into a folder chosen in the folder dialog and reports the result.
        void CopyFilesDialog(StringView collection)
        {
            shell::IDialogService* dialogs = m_host != nullptr && m_host->Shell() != nullptr ? m_host->Shell()->Dialogs() : nullptr;
            if (dialogs == nullptr)
                return;
            ModelViewerApp* self = this;
            const String target(collection);
            dialogs->ShowOpenFolder([self, target](Span<const String> paths)
                                    {
                                        if (paths.IsEmpty())
                                            return;
                                        JsonValue a = Arg(u8"id", Text(target.AsView()));
                                        a.Set(String(u8"folder"), Text(paths[0].AsView()));
                                        const CommandResult result = self->m_library.Commands().Execute(u8"export.copy_files", a);
                                        if (!result.ok)
                                        {
                                            self->Toast(gui::ToastSeverity::Error, Format(u8"Not copied: {}", result.error.AsView()).AsView());
                                            return;
                                        }
                                        String text = Format(u8"{} files copied into {}.", result.value.Get(String(u8"copied")).Count(), paths[0].AsView());
                                        if (const i64 there = result.value.Get(String(u8"alreadyThere")).AsInt(); there > 0)
                                            text += Format(u8" {} were there already.", there).AsView();
                                        const JsonValue missing = result.value.Get(String(u8"missing"));
                                        if (missing.Count() > 0)
                                        {
                                            text += Format(u8"\n\nMissing ({}):", missing.Count()).AsView();
                                            for (i64 i = 0; i < missing.Count() && i < 10; ++i)
                                                text += Format(u8"\n{}", missing.At(i).Get(String(u8"file")).AsString().AsView()).AsView();
                                        }
                                        const JsonValue clashes = result.value.Get(String(u8"clashes"));
                                        if (clashes.Count() > 0)
                                        {
                                            text += Format(u8"\n\nLeft as they were, a different file there already ({}):", clashes.Count()).AsView();
                                            for (i64 i = 0; i < clashes.Count() && i < 10; ++i)
                                                text += Format(u8"\n{}", clashes.At(i).AsString().AsView()).AsView();
                                        }
                                        // Report any problems, with the full list in the log. A warning toast stays
                                        // until closed; a success toast dismisses itself.
                                        const bool trouble = missing.Count() > 0 || clashes.Count() > 0;
                                        if (!trouble)
                                        {
                                            self->Toast(gui::ToastSeverity::Success, text.AsView());
                                            return;
                                        }
                                        LOG_WARNING(u8"ModelViewer", u8"{}", text.AsView());
                                        String summary = Format(u8"{} files copied.", result.value.Get(String(u8"copied")).Count());
                                        if (missing.Count() > 0)
                                            summary += Format(u8" {} missing.", missing.Count()).AsView();
                                        if (clashes.Count() > 0)
                                            summary += Format(u8" {} left as they were, a different file there already.", clashes.Count()).AsView();
                                        summary += u8" The log lists them.";
                                        self->Toast(gui::ToastSeverity::Warning, summary.AsView());
                                    });
        }

        // Steps to the next or previous tab (Browse, Library, then the models), wrapping around.
        void StepTab(i32 by)
        {
            const i32 count = kFixedTabs + m_tabs.Count();
            const i32 current = m_page == Page::Browse ? 0 : m_page == Page::Library ? 1 : kFixedTabs + m_tabs.Active();
            const i32 next = ((current + by) % count + count) % count;
            Run(u8"tabs.select", Arg(u8"tab", Number(next == 0 ? -1 : next == 1 ? -2 : next - kFixedTabs)));
        }

        // The grid's count line, with thumbnail progress while they are drawn
        // ("12 of 12 models, drawing 3 of 12").
        void ShowProgress()
        {
            String line = m_gridCount;
            if (!line.IsEmpty() && m_thumbnails->Remaining() > 0)
                line += Format(u8", drawing {} of {}", modelviewer::GroupedCount(m_thumbnails->Done() + 1).AsView(),
                               modelviewer::GroupedCount(m_thumbnails->Queued()).AsView())
                            .AsView();
            if (line == m_shownProgress)
                return;
            m_shownProgress = line;
            if (auto* count = FindAs<gui::Label>(u8"viewer-grid-count"))
                count->SetText(line.AsView());
        }

        // Puts a tab's ready model into the scene and sets up its instance, playback and bounds.
        void PlaceModel(modelviewer::ModelTab& tab, modelviewer::OpenModel::Outcome outcome)
        {
            LOG_INFO(u8"ModelViewer", u8"opened {} ({})", tab.path.AsView(), OutcomeText(outcome));
            engine::model::InstantiateOptions options;
            options.animate = !m_rest;
            tab.instance = engine::model::InstantiateModel(*m_viewport->Scene(), *tab.open->Model(), tab.title.AsView(), options);
            tab.playback = MakeUnique<modelviewer::ClipPlayback>(m_allocator, *m_viewport->Scene(), tab.instance.root, *tab.open->Model());
            tab.playback->SetLooping(m_settings.value.loop);
            KeepStats(tab.path.AsView(), tab.open->Stats());
            MeasureBounds(tab);
        }

        // Takes the files the worker has read. Each waiting tab is finished (cooked, bound, placed,
        // and shown when in front), or closed with a message saying why it did not open.
        void TakeReads()
        {
            modelviewer::FileRead read;
            while (m_tabLoads.Take(read))
            {
                const i32 index = m_tabs.Find(read.path.AsView());
                if (index < 0 || m_tabs.At(index).ready)
                    continue; // the tab closed during the read, or the file was read twice
                const modelviewer::OpenModel::Outcome outcome = m_tabs.Finish(index, read);
                if (outcome != modelviewer::OpenModel::Outcome::Opened)
                {
                    m_failure = Format(u8"{} did not open ({}).", read.path.AsView(), OutcomeText(outcome));
                    LOG_ERROR(u8"ModelViewer", u8"{}", m_failure.AsView());
                    CloseTab(index);
                    ShowEmpty();
                    continue;
                }
                PlaceModel(m_tabs.At(index), outcome);
                if (index == m_tabs.Active() && !m_browsing)
                    ShowTab(index);
            }
        }

        // Brings a tab to the front: its model becomes the only active one in the scene, its camera
        // is restored (framed the first time), and its stats fill the panel.
        void ShowTab(i32 index)
        {
            if (!m_tabs.Valid(index))
            {
                ShowEmpty();
                return;
            }
            if (index != m_tabs.Active() && !m_browsing)
                SaveCamera();
            SetPage(Page::Model);
            m_tabs.SetActive(index);
            scene::Scene* level = m_viewport->Scene();
            for (i32 i = 0; i < m_tabs.Count(); ++i)
            {
                const scene::EntityHandle root = m_tabs.At(i).instance.root;
                if (level != nullptr && level->IsValid(root))
                    level->SetActive(root, i == index);
            }
            modelviewer::ModelTab& tab = m_tabs.At(index);
            if (tab.camera.saved)
            {
                editor::EditorCamera& camera = m_viewport->Camera();
                camera.position = tab.camera.position;
                camera.yaw = tab.camera.yaw;
                camera.pitch = tab.camera.pitch;
                camera.focusDistance = tab.camera.focusDistance;
                ClipToModel(tab.radius);
            }
            else
                FrameModel(/*snap*/ true);
            if (auto* bar = FindAs<gui::TabBar>(u8"viewer-tabs"); bar != nullptr && bar->GetSelectedIndex() != index + kFixedTabs)
            {
                m_syncingTabs = true;
                bar->SetSelectedIndex(index + kFixedTabs);
                m_syncingTabs = false;
            }
            ShowStats(tab);
            ShowLibraryPanel();
            ShowClips(tab);
            ShowEmpty();
        }

        // Saves the front tab's camera for when the tab comes back.
        void SaveCamera()
        {
            modelviewer::ModelTab* tab = m_tabs.ActiveTab();
            if (tab == nullptr || !tab->ready || !m_viewport || m_browsing)
                return; // a tab still cooking has no view of its own yet
            const editor::EditorCamera& camera = m_viewport->Camera();
            tab->camera.saved = true;
            tab->camera.position = camera.position;
            tab->camera.yaw = camera.yaw;
            tab->camera.pitch = camera.pitch;
            tab->camera.focusDistance = camera.focusDistance;
        }

        void CloseTab(i32 index)
        {
            if (!m_tabs.Valid(index))
                return;
            scene::Scene* level = m_viewport->Scene();
            if (level != nullptr && level->IsValid(m_tabs.At(index).instance.root))
                level->DestroyEntity(m_tabs.At(index).instance.root);
            const bool wasActive = index == m_tabs.Active();
            // The frames in flight still draw the model's textures (their views are in the frames'
            // descriptor sets), so wait for the GPU before the model releases them.
            if (m_device != nullptr)
                m_device->WaitIdle();
            const i32 next = m_tabs.Close(index);
            if (auto* bar = FindAs<gui::TabBar>(u8"viewer-tabs"))
            {
                m_syncingTabs = true;
                bar->RemoveTab(index + kFixedTabs);
                m_syncingTabs = false;
            }
            if (wasActive)
                m_tabs.SetActive(-1); // nothing to save: the camera belonged to the closed tab
            if (m_browsing)
                return; // a tab closed from the browser keeps the browser in front
            if (next >= 0)
                ShowTab(next);
            else
                ShowBrowser();
        }

        // Fills the panel with the front tab's stats: the file, then a row per count.
        void ShowStats(const modelviewer::ModelTab& tab)
        {
            if (auto* file = FindAs<gui::Label>(u8"viewer-file"))
                file->SetText(tab.path.AsView());
            // Show the size as placed in the view (the file's axes turned upright, its node scale
            // applied), not the file's raw box.
            modelviewer::ModelStats stats = tab.open->Stats();
            stats.size = tab.size;
            ShowStatRows(u8"viewer-stats", stats);
            ShowContents(u8"viewer", stats);
        }

        // Lists a model's materials (how each shades, the textures in its slots) and its textures
        // (size in pixels, and the image's file or "in the file") as rows under `prefix`-materials
        // and `prefix`-textures.
        void ShowContents(StringView prefix, const modelviewer::ModelStats& stats)
        {
            auto fill = [this](gui::Node* rows, auto&& each, usize count, StringView none)
            {
                if (rows == nullptr)
                    return;
                while (rows->ChildCount() > 0)
                    rows->RemoveChild(rows->GetChildAt(rows->ChildCount() - 1));
                if (count == 0)
                {
                    auto note = MakeRef<gui::Label>(m_allocator);
                    note->AddClass(u8"viewer-empty-note");
                    note->SetText(none);
                    rows->AddChild(note.Get());
                    return;
                }
                for (usize i = 0; i < count; ++i)
                {
                    String label;
                    String value;
                    String tip;
                    each(i, label, value, tip);
                    auto row = MakeRef<gui::Panel>(m_allocator);
                    row->AddClass(u8"prop-row");
                    row->SetTooltip(tip.AsView());
                    auto name = MakeRef<gui::Label>(m_allocator);
                    name->AddClass(u8"prop-name");
                    name->SetText(label.AsView());
                    auto text = MakeRef<gui::Label>(m_allocator);
                    text->AddClass(u8"viewer-stat");
                    text->AddClass(u8"viewer-content");
                    text->SetText(value.AsView());
                    row->AddChild(name.Get());
                    row->AddChild(text.Get());
                    rows->AddChild(row.Get());
                }
            };
            const auto textureName = [&stats](u32 index)
            {
                if (index >= stats.textureList.Size())
                    return String(u8"?");
                const modelviewer::ModelTextureInfo& t = stats.textureList[index];
                return !t.name.IsEmpty() ? t.name : !t.file.IsEmpty() ? t.file : Format(u8"texture {}", index + 1);
            };
            fill(FindById(Format(u8"{}-materials", prefix).AsView()),
                 [&](usize i, String& label, String& value, String& tip)
                 {
                     const modelviewer::ModelMaterialInfo& m = stats.materialList[i];
                     label = m.name.IsEmpty() ? Format(u8"material {}", i + 1) : m.name;
                     value = m.shading;
                     tip = Format(u8"{}, shaded {}", label.AsView(), m.shading.AsView());
                     if (m.slots.IsEmpty())
                         value += u8", no textures";
                     for (const modelviewer::ModelMaterialInfo::Slot& slot : m.slots)
                     {
                         value += Format(u8", {}", slot.slot.AsView()).AsView();
                         tip += Format(u8"\n{}: {}", slot.slot.AsView(), textureName(slot.texture).AsView()).AsView();
                     }
                 },
                 stats.materialList.Size(), stats.materials > 0 ? StringView(u8"Listed once it cooks again.") : StringView(u8"None."));
            fill(FindById(Format(u8"{}-textures", prefix).AsView()),
                 [&](usize i, String& label, String& value, String& tip)
                 {
                     const modelviewer::ModelTextureInfo& t = stats.textureList[i];
                     label = textureName(static_cast<u32>(i));
                     value = t.width > 0 ? Format(u8"{} x {}", t.width, t.height) : String(u8"size unknown");
                     value += t.file.IsEmpty() ? StringView(u8", in the file") : Format(u8", {}", t.file.AsView()).AsView();
                     tip = t.file.IsEmpty() ? Format(u8"{}: its image is inside the model file", label.AsView())
                                            : Format(u8"{}: {}", label.AsView(), t.file.AsView());
                 },
                 stats.textureList.Size(), stats.textures > 0 ? StringView(u8"Listed once it cooks again.") : StringView(u8"None."));
        }

        // Fills the panel `id` names (a model tab's panel or a details pane) with a model's stats.
        void ShowStatRows(StringView id, const modelviewer::ModelStats& stats)
        {
            gui::Node* rows = FindById(id);
            if (rows == nullptr)
                return;
            while (rows->ChildCount() > 0)
                rows->RemoveChild(rows->GetChildAt(rows->ChildCount() - 1));
            for (const modelviewer::StatRow& stat : modelviewer::StatRows(stats))
            {
                auto row = MakeRef<gui::Panel>(m_allocator);
                row->AddClass(u8"prop-row");
                auto name = MakeRef<gui::Label>(m_allocator);
                name->AddClass(u8"prop-name");
                name->SetText(stat.label.AsView());
                auto value = MakeRef<gui::Label>(m_allocator);
                value->AddClass(u8"viewer-stat");
                value->SetText(stat.value.AsView());
                row->AddChild(name.Get());
                row->AddChild(value.Get());
                rows->AddChild(row.Get());
            }
        }

        void SetTurntable(bool on)
        {
            m_turntable = on;
            if (on)
                return;
            // Turned off: the model faces the way it was imported.
            m_turnAngle = 0.0f;
            if (modelviewer::ModelTab* tab = m_tabs.ActiveTab())
                PlaceTurned(*tab);
        }

        // The turntable slowly turns the front model about the middle of its footprint.
        void Turn(f32 deltaTime)
        {
            modelviewer::ModelTab* tab = m_tabs.ActiveTab();
            if (!m_turntable || tab == nullptr)
                return;
            m_turnAngle += deltaTime * m_settings.value.turntableSpeed * kPi / 180.0f;
            PlaceTurned(*tab);
        }

        void PlaceTurned(modelviewer::ModelTab& tab)
        {
            scene::Scene* level = m_viewport->Scene();
            if (level == nullptr || !level->IsValid(tab.instance.root))
                return;
            const Float3 pivot{tab.center.x, 0.0f, tab.center.z};
            Transform t = level->GetLocalTransform(tab.instance.root);
            t.rotation = Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, m_turnAngle);
            t.position = pivot - RotateVector(t.rotation, pivot);
            level->SetLocalTransform(tab.instance.root, t);
        }

        // The animation bar runs the clip commands on the front tab's playback.
        void WireAnimationBar()
        {
            ModelViewerApp* self = this;
            auto playback = [self]() -> modelviewer::ClipPlayback*
            {
                String ignored;
                return self->FrontPlayback(ignored);
            };
            if (auto* clip = FindAs<gui::ComboBox>(u8"viewer-clip"))
                clip->SetOnSelectionChanged([self, playback](i32 index)
                                            {
                                                if (modelviewer::ClipPlayback* p = playback(); p != nullptr && !self->m_syncingBar)
                                                    self->Run(u8"clip.select", Arg(u8"clip", Text(p->ClipName(index).AsView())));
                                            });
            if (auto* play = FindAs<gui::Button>(u8"viewer-play"))
                play->SetOnClick([self, playback]()
                                 {
                                     if (modelviewer::ClipPlayback* p = playback())
                                         self->Run(u8"clip.play", Arg(u8"playing", Flag(!p->Playing())));
                                 });
            if (auto* stop = FindAs<gui::Button>(u8"viewer-stop"))
                stop->SetOnClick([self]() { self->Run(u8"clip.scrub", Arg(u8"seconds", Number(0.0))); }); // rewind to the start, paused
            auto step = [self, playback](i32 frames)
            {
                if (modelviewer::ClipPlayback* p = playback())
                    self->Run(u8"clip.scrub", Arg(u8"seconds", Number(p->Time() + static_cast<f32>(frames) / modelviewer::ClipPlayback::kStepFps)));
            };
            if (auto* back = FindAs<gui::Button>(u8"viewer-step-back"))
                back->SetOnClick([step]() { step(-1); });
            if (auto* forward = FindAs<gui::Button>(u8"viewer-step"))
                forward->SetOnClick([step]() { step(1); });
            if (auto* scrub = FindAs<gui::Slider>(u8"viewer-scrub"))
                scrub->SetOnValueChanging([self, playback](f64 ratio)
                                          {
                                              if (modelviewer::ClipPlayback* p = playback(); p != nullptr && !self->m_syncingBar)
                                                  self->Run(u8"clip.scrub", Arg(u8"seconds", Number(ratio * p->Duration())));
                                          });
            if (auto* loop = FindAs<gui::CheckBox>(u8"viewer-loop"))
                loop->SetOnToggled([self](bool on)
                                   {
                                       if (!self->m_syncingBar)
                                           self->Run(u8"clip.play", Arg(u8"loop", Flag(on)));
                                   });
            if (auto* speed = FindAs<gui::ComboBox>(u8"viewer-speed"))
                speed->SetOnSelectionChanged([self](i32 index)
                                             {
                                                 static constexpr f32 kSpeeds[] = {0.25f, 0.5f, 1.0f, 2.0f};
                                                 if (!self->m_syncingBar && index >= 0 && index < 4)
                                                     self->Run(u8"clip.play", Arg(u8"speed", Number(kSpeeds[index])));
                                             });
        }

        // Fills the animation bar for the front tab: its clips, the one playing, looping and speed.
        // The bar is hidden for a model without clips.
        void ShowClips(const modelviewer::ModelTab& tab)
        {
            gui::Node* bar = FindById(u8"viewer-anim");
            const modelviewer::ClipPlayback* playback = tab.playback.Get();
            const bool clips = playback != nullptr && playback->HasClips();
            if (bar != nullptr)
                bar->SetDisplayed(clips);
            if (!clips)
                return;
            m_syncingBar = true;
            if (auto* combo = FindAs<gui::ComboBox>(u8"viewer-clip"))
            {
                String items;
                for (i32 i = 0; i < playback->ClipCount(); ++i)
                {
                    if (i > 0)
                        items += u8"|";
                    items += playback->ClipName(i).AsView();
                }
                combo->SetItemsText(items);
                combo->SetSelectedIndex(playback->Selected());
            }
            if (auto* loop = FindAs<gui::CheckBox>(u8"viewer-loop"))
                loop->SetChecked(playback->Looping());
            if (auto* speed = FindAs<gui::ComboBox>(u8"viewer-speed"))
            {
                const f32 s = playback->Speed();
                speed->SetSelectedIndex(s <= 0.3f ? 0 : s <= 0.6f ? 1 : s <= 1.5f ? 2 : 3);
            }
            m_syncingBar = false;
        }

        // Each frame, the scrub bar and time follow the clip (except while dragged), and the play
        // button shows what a click will do.
        void ShowPlayback()
        {
            modelviewer::ModelTab* tab = m_tabs.ActiveTab();
            modelviewer::ClipPlayback* playback = tab != nullptr ? tab->playback.Get() : nullptr;
            if (playback == nullptr || !playback->HasClips())
                return;
            const f32 duration = playback->Duration();
            const f32 time = playback->Time();
            m_syncingBar = true;
            if (auto* scrub = FindAs<gui::Slider>(u8"viewer-scrub"); scrub != nullptr && !scrub->IsDragging())
                scrub->SetValue(duration > 0.0f ? time / duration : 0.0);
            m_syncingBar = false;
            if (auto* label = FindAs<gui::Label>(u8"viewer-time"))
                label->SetText(Format(u8"{} / {} s", FormatFixed(time, 2), FormatFixed(duration, 2)).AsView());
            const bool playing = playback->Playing();
            if (auto* play = FindAs<gui::Button>(u8"viewer-play"); play != nullptr && playing != m_shownPlaying)
            {
                m_shownPlaying = playing;
                play->SetIconSource(String(playing ? u8"Editor/Icons/pause.svg" : u8"Editor/Icons/play.svg"));
            }
        }

        // Draws the front model's bones as a line from each bone to its parent, in the current pose
        // (the rest pose before any clip plays), through the skin's space.
        void DrawSkeleton()
        {
            modelviewer::ModelTab* tab = m_tabs.ActiveTab();
            if (!m_showSkeleton || tab == nullptr || !m_viewport || !m_viewport->IsValid() || tab->instance.skinned.IsEmpty())
                return;
            const foundation::animation::AnimationPlayer* player = tab->playback ? tab->playback->Player() : nullptr;
            foundation::animation::Skeleton* skeleton = tab->open->Model()->skeleton.Get();
            if (skeleton == nullptr)
                return;
            const usize count = static_cast<usize>(skeleton->BoneCount());
            m_bonePoses.Resize(count);
            skeleton->ComputeWorldPoses(player != nullptr ? player->GetFinalPoses() : Span<const foundation::animation::BoneTransform>{},
                                        Span<Float4x4>(m_bonePoses.Data(), count));
            const Float4x4 model = m_viewport->Scene()->ComposeWorldMatrix(tab->instance.skinned[0]);
            const Color bone{0.35f, 0.85f, 1.0f, 1.0f};
            render::debug::DebugDraw& draw = m_viewport->SceneDebugDraw();
            for (usize i = 0; i < count; ++i)
            {
                const i32 parent = skeleton->GetBone(static_cast<i32>(i))->parentIndex;
                if (parent < 0 || static_cast<usize>(parent) >= count)
                    continue;
                const Float4x4& a = m_bonePoses[static_cast<usize>(parent)];
                const Float4x4& b = m_bonePoses[i];
                draw.DrawLineOverlay(TransformPoint(Float3{a.m[3][0], a.m[3][1], a.m[3][2]}, model),
                                     TransformPoint(Float3{b.m[3][0], b.m[3][1], b.m[3][2]}, model), bone);
            }
        }

        // === The browser ===

        void WireBrowser(runtime::IApplicationHost& host)
        {
            ModelViewerApp* self = this;
            if (auto* choose = FindAs<gui::Button>(u8"viewer-choose-folder"))
                choose->SetOnClick([self, &host]()
                                   {
                                       shell::IDialogService* dialogs = host.Shell() != nullptr ? host.Shell()->Dialogs() : nullptr;
                                       if (dialogs == nullptr)
                                           return;
                                       dialogs->ShowOpenFolder([self](Span<const String> paths)
                                                               {
                                                                   if (!paths.IsEmpty())
                                                                       self->Run(u8"browse.folder", Arg(u8"path", Text(paths[0].AsView())));
                                                               },
                                                               self->m_settings.value.folder.AsView());
                                   });
            if (auto* tree = FindAs<gui::TreeView>(u8"viewer-folder-tree"))
                tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                            {
                                                if (index.IsValid())
                                                    self->ShowFolderNode(static_cast<i32>(index.InternalId));
                                            });
            if (auto* name = FindAs<gui::TextField>(u8"viewer-filter-name"))
                name->SetOnTextChanging([self](StringView text)
                                        {
                                            self->m_filter.name = String(text);
                                            self->ShowGrid();
                                        });
            if (auto* rigged = FindAs<gui::CheckBox>(u8"viewer-filter-rigged"))
                rigged->SetOnToggled([self](bool on)
                                     {
                                         self->m_filter.riggedOnly = on;
                                         self->ShowGrid();
                                     });
            if (auto* animated = FindAs<gui::CheckBox>(u8"viewer-filter-animated"))
                animated->SetOnToggled([self](bool on)
                                       {
                                           self->m_filter.animatedOnly = on;
                                           self->ShowGrid();
                                       });
            if (auto* inLibrary = FindAs<gui::CheckBox>(u8"viewer-filter-library"))
                inLibrary->SetOnToggled([self](bool on)
                                        {
                                            self->m_filter.inLibraryOnly = on;
                                            self->ShowGrid();
                                        });
        }

        // Chooses the page that fills the window: a model (the view and panel), the browser or the library.
        void SetPage(Page page)
        {
            m_page = page;
            m_browsing = page != Page::Model;
            if (gui::Node* browser = FindById(u8"viewer-browser"))
                browser->SetDisplayed(page == Page::Browse);
            if (gui::Node* library = FindById(u8"viewer-library"))
                library->SetDisplayed(page == Page::Library);
            if (gui::Node* model = FindById(u8"viewer-model-page"))
                model->SetDisplayed(page == Page::Model);
        }

        // Brings the Library tab to the front: the collections and tags, the search, the models.
        void ShowLibrary()
        {
            SaveCamera();
            SetPage(Page::Library);
            if (auto* bar = FindAs<gui::TabBar>(u8"viewer-tabs"); bar != nullptr && bar->GetSelectedIndex() != 1)
            {
                m_syncingTabs = true;
                bar->SetSelectedIndex(1);
                m_syncingTabs = false;
            }
            RefreshLibrary();
        }

        // Brings the Browse tab to the front; the models' instances stay in the scene.
        void ShowBrowser()
        {
            SaveCamera();
            SetPage(Page::Browse);
            if (auto* bar = FindAs<gui::TabBar>(u8"viewer-tabs"); bar != nullptr && bar->GetSelectedIndex() != 0)
            {
                m_syncingTabs = true;
                bar->SetSelectedIndex(0);
                m_syncingTabs = false;
            }
            if (m_folderModel.Get() == nullptr && !m_settings.value.folder.IsEmpty())
                SetBrowseRoot(m_settings.value.folder.AsView());
            if (m_settings.value.folder.IsEmpty())
                if (auto* count = FindAs<gui::Label>(u8"viewer-grid-count"))
                    count->SetText(u8"Choose a folder to browse");
        }

        // Browses a new folder: the tree restarts at it, selects it and shows its models.
        void SetBrowseRoot(StringView chosen)
        {
            const String folderCopy(chosen); // copied because `chosen` may view m_settings.value.folder, which changes below
            const StringView folder = folderCopy.AsView();
            m_settings.value.folder = folderCopy;
            NoteRecent(true, folder);
            auto* tree = FindAs<gui::TreeView>(u8"viewer-folder-tree");
            if (tree == nullptr)
                return;
            tree->SetModel(nullptr);
            m_folderModel = MakeUnique<gui::TreeModel>(m_allocator);
            m_folderNodes.Clear();
            const i32 root = AddFolderNode(gui::TreeModel::kRoot, folder);
            tree->SetModel(m_folderModel.Get());
            ListFolderNode(root);
            tree->SetIdExpanded(static_cast<gui::ItemId>(root), true);
            const gui::ItemId selected[] = {static_cast<gui::ItemId>(root)};
            tree->SetSelectedIds(Span<const gui::ItemId>(selected, 1));
            ShowFolderNode(root);
        }

        i32 AddFolderNode(i32 parent, StringView path)
        {
            const String title = modelviewer::FileTitle(path);
            const i32 id = m_folderModel->AddNode(parent, title.IsEmpty() ? path : title.AsView());
            m_folderModel->SetIcon(id, u8"Editor/Icons/folder.svg");
            if (static_cast<usize>(id) >= m_folderNodes.Size())
                m_folderNodes.Resize(static_cast<usize>(id) + 1);
            m_folderNodes[static_cast<usize>(id)] = FolderNode{String(path), parent, false};
            return id;
        }

        // Lists a node's subfolders and theirs one level ahead, so a folder with more inside shows
        // its expand arrow.
        void ListFolderNode(i32 id, u32 depth = 2)
        {
            if (depth == 0 || id < 0 || static_cast<usize>(id) >= m_folderNodes.Size())
                return;
            if (!m_folderNodes[static_cast<usize>(id)].listed)
            {
                m_folderNodes[static_cast<usize>(id)].listed = true;
                const String path = m_folderNodes[static_cast<usize>(id)].path;
                for (const String& sub : modelviewer::ListFolder(path.AsView()).folders)
                    (void)AddFolderNode(id, sub.AsView());
            }
            for (usize i = 0; i < m_folderNodes.Size(); ++i)
                if (m_folderNodes[i].parent == id)
                    ListFolderNode(static_cast<i32>(i), depth - 1);
        }

        // A folder chosen in the tree shows every model under it in the grid.
        void ShowFolderNode(i32 id)
        {
            if (id < 0 || static_cast<usize>(id) >= m_folderNodes.Size())
                return;
            ListFolderNode(id);
            m_folderModels.Clear();
            modelviewer::CollectModels(m_folderNodes[static_cast<usize>(id)].path.AsView(), m_folderModels);
            m_folderStats.Clear();
            m_folderStatsKnown.Clear();
            for (const String& model : m_folderModels)
            {
                modelviewer::ModelStats stats;
                const bool known = modelviewer::ReadCachedStats(m_cacheRoot.AsView(), model.AsView(), stats, m_settings.value.looseTextures);
                m_folderStats.PushBack(foundation::core::Move(stats));
                m_folderStatsKnown.PushBack(known);
            }
            ShowGrid();
        }

        // Fills the grid with the folder's models that pass the filter and queues their thumbnails in order.
        void ShowGrid()
        {
            m_browseGrid.rows.Clear();
            m_doneSinceFilter = 0;
            Array<String> shown;
            for (usize i = 0; i < m_folderModels.Size(); ++i)
            {
                const String& path = m_folderModels[i];
                const modelviewer::Entry* entry = m_library.Get().FindEntryByPath(path.AsView());
                if (!modelviewer::Matches(m_filter, path.AsView(), m_folderStatsKnown[i] ? &m_folderStats[i] : nullptr, entry != nullptr))
                    continue;
                modelviewer_window::GridRow row;
                row.path = path;
                row.inLibrary = entry != nullptr;
                if (entry != nullptr)
                    row.entry = entry->id;
                m_browseGrid.rows.PushBack(foundation::core::Move(row));
                shown.PushBack(path);
            }
            m_browseGrid.Reset();
            m_gridCount = Format(u8"{} of {} models", modelviewer::GroupedCount(shown.Size()).AsView(),
                                 modelviewer::GroupedCount(m_folderModels.Size()).AsView());
            if (auto* count = FindAs<gui::Label>(u8"viewer-grid-count"))
                count->SetText(m_gridCount.AsView());
            if (m_thumbnails)
                m_thumbnails->SetQueue(shown.AsSpan());
        }

        [[nodiscard]] const foundation::image::Image* ThumbImageFor(StringView model) const
        {
            for (const ThumbImage& t : m_thumbImages)
                if (t.path.AsView() == model)
                    return t.image.Get();
            return nullptr;
        }

        // A thumbnail is ready (drawn now or before): put it on its tile, and record the model's stats.
        void ThumbnailDone(StringView model, StringView png)
        {
            for (usize i = 0; i < m_folderModels.Size(); ++i)
                if (m_folderModels[i].AsView() == model && !m_folderStatsKnown[i])
                {
                    m_folderStatsKnown[i] = modelviewer::ReadCachedStats(m_cacheRoot.AsView(), model, m_folderStats[i], m_settings.value.looseTextures);
                    if (m_folderStatsKnown[i])
                        KeepStats(model, m_folderStats[i]);
                }
            if (SectionModel(u8"browse").AsView() == model)
                ShowDetails(u8"browse"); // its stats are known now
            if (png.IsEmpty() || ThumbImageFor(model) != nullptr)
                return;
            auto image = MakeUnique<foundation::image::Image>(m_allocator);
            if (!foundation::image::io::LoadImage(png, *image).IsOk())
                return;
            const foundation::image::Image* raw = image.Get();
            m_thumbImages.PushBack(ThumbImage{String(model), foundation::core::Move(image)});
            (void)raw;
            m_browseGrid.Changed(); // the cells rebind their pictures
            m_libraryGrid.Changed();
            // The rigged and animated filters learn as models are drawn, so refilter the grid every so often.
            if ((m_filter.riggedOnly || m_filter.animatedOnly) && (++m_doneSinceFilter >= 25 || (m_thumbnails && m_thumbnails->Remaining() == 0)))
                ShowGrid();
        }

        // Serves the commands over HTTP when asked (--mcp), on localhost with the shared bearer token.
        void StartMcp()
        {
            if (m_mcpPort == 0)
                return;
            ServeCommands(m_mcpServer, m_library);
            foundation::mcp::McpHttpConfig config;
            config.port = m_mcpPort;
            config.token = McpToken();
            if (config.token.IsEmpty())
            {
                LOG_ERROR(u8"ModelViewer", u8"MCP: no token could be made; not serving");
                return;
            }
            m_mcpHost = MakeUnique<foundation::mcp::McpHttpHost>(m_allocator, m_allocator, m_mcpServer);
            if (!m_mcpHost->Start(config))
            {
                LOG_ERROR(u8"ModelViewer", u8"MCP: port {} is not free; not serving", m_mcpPort);
                m_mcpHost.Reset();
                return;
            }
            LOG_INFO(u8"ModelViewer", u8"MCP: listening on 127.0.0.1:{} (the token is in <user-data>/mcp-token)", m_mcpHost->BoundPort());
        }

        // === The library in the window ===

        [[nodiscard]] bool ModifierHeld(shell::KeyModifiers which) const
        {
            shell::IInputManager* input = m_host != nullptr && m_host->Shell() != nullptr ? m_host->Shell()->Input() : nullptr;
            shell::IKeyboard* keyboard = input != nullptr ? input->Keyboard() : nullptr;
            return keyboard != nullptr && (keyboard->Modifiers() & which) != shell::KeyModifiers::None;
        }
        /// The files of a grid's selected rows, in the grid's order.
        [[nodiscard]] Array<String> SelectedPaths(StringView listId, const modelviewer_window::GridModel& grid) const
        {
            Array<String> paths;
            auto* list = FindAs<gui::ListView>(listId);
            if (list == nullptr)
                return paths;
            Array<gui::ItemId> ids;
            list->GetSelectedIds(ids);
            for (const modelviewer_window::GridRow& row : grid.rows)
                for (const gui::ItemId id : ids)
                    if (modelviewer_window::GridModel::IdOfPath(row.path.AsView()) == id)
                    {
                        paths.PushBack(row.path);
                        break;
                    }
            return paths;
        }

        // Wires both grids like the editor's asset tiles: a cell per model, the list's own selection
        // (click, Ctrl, Shift, Ctrl+A), double click to open, dragging the models onto a collection
        // in the Library tab, and a right-click menu for the row pressed.
        void WireGrids()
        {
            ModelViewerApp* self = this;
            const StringView lists[] = {u8"viewer-grid", u8"viewer-lib-grid"};
            for (usize which = 0; which < 2; ++which)
            {
                auto* list = FindAs<gui::ListView>(lists[which]);
                if (list == nullptr)
                    continue;
                modelviewer_window::GridModel* grid = which == 0 ? &m_browseGrid : &m_libraryGrid;
                gui::RowFactory rows;
                rows.Create = [self](u32) -> RefPtr<gui::Node> { return MakeRef<modelviewer_window::ModelCell>(self->m_allocator); };
                rows.Bind = [self, grid](gui::Node& cell, const gui::ModelIndex& index)
                {
                    if (!grid->Valid(index.Row))
                        return;
                    const modelviewer_window::GridRow& row = grid->rows[static_cast<usize>(index.Row)];
                    modelviewer_window::ModelCell::Shown shown;
                    shown.name = modelviewer::FileTitle(row.path.AsView());
                    shown.sub = row.sub;
                    shown.inLibrary = row.inLibrary && grid == &self->m_browseGrid; // every model in the library grid is in the library, so no mark
                    shown.missing = row.missing;
                    if (const foundation::image::Image* image = self->ThumbImageFor(row.path.AsView()))
                        shown.picture = MakeRef<gui::ImageDrawable>(self->m_allocator, image);
                    static_cast<modelviewer_window::ModelCell&>(cell).Show(shown);
                };
                list->SetLayoutMode(gui::ItemLayout::Grid);

                list->SetRowFactory(foundation::core::Move(rows));
                list->SetModel(grid);
                list->SetOnActivated([self, grid](gui::ModelIndex index)
                                     {
                                         if (!grid->Valid(index.Row))
                                             return;
                                         const modelviewer_window::GridRow& row = grid->rows[static_cast<usize>(index.Row)];
                                         if (row.missing)
                                             self->RelinkDialog(row.entry);
                                         else
                                             self->Run(u8"model.open", Arg(u8"path", Text(row.path.AsView())));
                                     });
                list->SetDragType(String(kModelsDrag));
                list->SetOnDragPayload([grid](gui::DragPayload& payload, Span<const gui::ItemId> ids)
                                       {
                                           String paths;
                                           usize count = 0;
                                           for (const gui::ItemId id : ids)
                                               if (const modelviewer_window::GridRow* row = grid->RowOfId(id))
                                               {
                                                   if (!paths.IsEmpty())
                                                       paths += u8"\n";
                                                   paths += row->path.AsView();
                                                   ++count;
                                               }
                                           if (count == 0)
                                               return;
                                           payload.Type = String(kModelsDrag);
                                           payload.Value = Variant::From<String>(paths);
                                           payload.Label = count == 1 ? modelviewer::FileTitle(paths.AsView()) : Format(u8"{} models", count);
                                       });
                const bool library = which == 1;
                (void)list->AddEventListener(gui::EventType::ContextMenu, [self, list, grid, library](const gui::Event& e)
                                             {
                                                 const Float2 at = static_cast<const gui::MouseEvent&>(e).Position;
                                                 self->ShowGridMenu(*list, *grid, library, list->ItemAt(at), at);
                                                 e.Handled = true;
                                             });
            }
        }

        // A tile's menu: open, add the selection to a collection, remove it from the collection
        // shown, copy its path, show its folder, remove it from the library. Right-clicking a tile
        // that is not selected first selects it alone, so the menu acts on it, as in the editor.
        void ShowGridMenu(gui::ListView& list, const modelviewer_window::GridModel& grid, bool library, i32 item, Float2 at)
        {
            if (item < 0 || !grid.Valid(list.GetItemIndex(item).Row))
                return;
            const modelviewer_window::GridRow& row = grid.rows[static_cast<usize>(list.GetItemIndex(item).Row)];
            const gui::ItemId id = modelviewer_window::GridModel::IdOfPath(row.path.AsView());
            if (!list.IsItemSelected(item))
            {
                const gui::ItemId only[] = {id};
                list.SetSelectedIds(Span<const gui::ItemId>(only, 1));
            }
            const Array<String> paths = SelectedPaths(library ? u8"viewer-lib-grid" : u8"viewer-grid", grid);
            ModelViewerApp* self = this;
            m_gridMenu = MakeRef<gui::Menu>(m_allocator);
            const String path = row.path;
            if (!row.missing)
                (void)m_gridMenu->AddItem(u8"Open", [self, path]() { self->Run(u8"model.open", Arg(u8"path", Text(path.AsView()))); });
            else
            {
                const Guid entry = row.entry;
                (void)m_gridMenu->AddItem(u8"Relink...", [self, entry]() { self->RelinkDialog(entry); });
            }
            if (!m_comboCollections.IsEmpty())
            {
                gui::Menu* into = m_gridMenu->AddSubMenu(paths.Size() > 1 ? Format(u8"Add {} to", paths.Size()).AsView() : StringView(u8"Add to"));
                for (const Guid& collection : m_comboCollections)
                {
                    const modelviewer::Collection* c = m_library.Get().FindCollection(collection);
                    (void)into->AddItem(c->name.AsView(), [self, collection, paths]() { self->AddToCollection(collection, paths.AsSpan()); });
                }
            }
            const LibraryNode* node = ChosenNode();
            if (library && node != nullptr && node->kind == LibraryNode::Kind::Collection)
            {
                const Guid collection = node->collection;
                (void)m_gridMenu->AddItem(u8"Take out of this collection", [self, collection, paths]() { self->TakeOut(collection, paths.AsSpan()); });
            }
            m_gridMenu->AddSeparator();
            (void)m_gridMenu->AddItem(u8"Copy path", [self, path]()
                                      {
                                          if (self->m_host != nullptr && self->m_host->Shell() != nullptr)
                                              self->m_host->Shell()->SetClipboardText(path.AsView());
                                      });
            (void)m_gridMenu->AddItem(u8"Show in folder", [self, path]()
                                      {
                                          shell::IDialogService* dialogs = self->m_host != nullptr && self->m_host->Shell() != nullptr ? self->m_host->Shell()->Dialogs() : nullptr;
                                          if (dialogs != nullptr)
                                              dialogs->OpenPath(PathParent(path.AsView()));
                                      });
            if (library)
            {
                m_gridMenu->AddSeparator();
                (void)m_gridMenu->AddItem(u8"Remove from the library", [self, paths]()
                                          {
                                              for (const String& p : paths)
                                                  if (const modelviewer::Entry* e = self->m_library.Get().FindEntryByPath(p.AsView()))
                                                      self->Run(u8"entry.remove", Arg(u8"id", Text(Format(u8"{}", e->id).AsView())));
                                          });
            }
            m_gridMenu->Open(list, at);
        }

        void TakeOut(const Guid& collection, Span<const String> paths)
        {
            JsonValue ids = JsonValue::MakeArray();
            for (const String& p : paths)
                if (const modelviewer::Entry* e = m_library.Get().FindEntryByPath(p.AsView()))
                    ids.Add(Text(Format(u8"{}", e->id).AsView()));
            if (ids.Count() == 0)
                return;
            JsonValue a = Arg(u8"collection", Text(Format(u8"{}", collection).AsView()));
            a.Set(String(u8"entries"), ids);
            Run(u8"member.remove", a);
        }

        /// The library entry for the model at `path`, adding one when needed. False when the file
        /// does not exist or the library refused.
        bool EntryFor(StringView path, String& id)
        {
            const modelviewer::CommandResult r = m_library.Commands().Execute(u8"entry.add", Arg(u8"path", Text(path)));
            if (!r.ok)
            {
                LOG_WARNING(u8"ModelViewer", u8"{}", r.error.AsView());
                return false;
            }
            id = r.value.Get(String(u8"id")).AsString();
            return true;
        }
        /// The entry ids of the models at `paths`, adding any not yet in the library.
        JsonValue EntriesFor(Span<const String> paths)
        {
            JsonValue ids = JsonValue::MakeArray();
            for (const String& path : paths)
            {
                String id;
                if (EntryFor(path.AsView(), id))
                    ids.Add(Text(id.AsView()));
            }
            return ids;
        }
        /// The collection a combo's selected item names; the items are the tree's collections in order.
        [[nodiscard]] Guid ComboCollection(StringView comboId) const
        {
            auto* combo = FindAs<gui::ComboBox>(comboId);
            const i32 at = combo != nullptr ? combo->GetSelectedIndex() : -1;
            return at >= 0 && static_cast<usize>(at) < m_comboCollections.Size() ? m_comboCollections[static_cast<usize>(at)] : Guid{};
        }
        void AddToCollection(const Guid& collection, Span<const String> paths)
        {
            if (collection.IsNil() || paths.IsEmpty())
                return;
            JsonValue a = Arg(u8"collection", Text(Format(u8"{}", collection).AsView()));
            a.Set(String(u8"entries"), EntriesFor(paths));
            Run(u8"member.add", a);
        }

        void WireLibrary(runtime::IApplicationHost&)
        {
            ModelViewerApp* self = this;
            // The browser's buttons: select all, and add the selection to a collection.
            if (auto* all = FindAs<gui::Button>(u8"viewer-select-all"))
                all->SetOnClick([self]()
                                {
                                    if (auto* list = self->FindAs<gui::ListView>(u8"viewer-grid"))
                                        list->SelectAll();
                                });
            if (auto* clear = FindAs<gui::Button>(u8"viewer-filter-clear"))
                clear->SetOnClick([self]()
                                  {
                                      if (auto* field = self->FindAs<gui::TextField>(u8"viewer-filter-name"))
                                          field->SetText(u8"");
                                      self->m_filter.name = {};
                                      self->ShowGrid();
                                  });
            if (auto* clear = FindAs<gui::Button>(u8"viewer-lib-clear"))
                clear->SetOnClick([self]()
                                  {
                                      if (auto* field = self->FindAs<gui::TextField>(u8"viewer-lib-search"))
                                          field->SetText(u8"");
                                      self->ShowLibraryGrid();
                                  });
            // The library's tree: selecting a row shows its models, and the toolbar acts on it.
            if (auto* tree = FindAs<gui::TreeView>(u8"viewer-lib-tree"))
                tree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                            {
                                                if (!index.IsValid() || self->m_syncingLibrary)
                                                    return;
                                                self->m_libraryChoice = static_cast<i32>(index.InternalId);
                                                self->ShowLibraryGrid();
                                            });
            if (auto* tree = FindAs<gui::TreeView>(u8"viewer-lib-tree"))
            {
                (void)tree->AddEventListener(gui::EventType::ContextMenu, [self, tree](const gui::Event& e)
                                             {
                                                 self->ShowLibraryMenu(*tree, static_cast<const gui::MouseEvent&>(e).Position);
                                                 e.Handled = true;
                                             });
                // A rename in place runs the command; a refusal (a name clash) keeps the row editing.
                tree->SetOnEditCommit([self](const gui::ModelIndex& index, StringView text)
                                      {
                                          const LibraryNode* node = self->NodeAt(static_cast<i32>(index.InternalId));
                                          if (node == nullptr || text.IsEmpty())
                                              return false;
                                          modelviewer::CommandResult r;
                                          JsonValue a = Arg(u8"id", Text(Format(u8"{}", node->collection).AsView()));
                                          a.Set(String(u8"name"), Text(text));
                                          r = self->m_library.Commands().Execute(u8"collection.rename", a);
                                          if (!r.ok)
                                              LOG_WARNING(u8"ModelViewer", u8"rename: {}", r.error.AsView());
                                          return r.ok;
                                      });
            }
            if (auto* create = FindAs<gui::Button>(u8"viewer-lib-new"))
                create->SetOnClick([self]()
                                   {
                                       // Inside the selected collection, else at the top level.
                                       const LibraryNode* node = self->ChosenNode();
                                       self->NewCollection(node != nullptr && node->kind == LibraryNode::Kind::Collection
                                                               ? Format(u8"{}", node->collection).AsView()
                                                               : StringView{});
                                   });
            if (auto* undo = FindAs<gui::Button>(u8"viewer-lib-undo"))
                undo->SetOnClick([self]() { self->Run(u8"history.undo"); });
            // The library grid's controls: the search, its scope, and removing the selection.
            if (auto* search = FindAs<gui::TextField>(u8"viewer-lib-search"))
                search->SetOnTextChanging([self](StringView) { self->ShowLibraryGrid(); });
            if (auto* everywhere = FindAs<gui::CheckBox>(u8"viewer-lib-everywhere"))
                everywhere->SetOnToggled([self](bool) { self->ShowLibraryGrid(); });
            if (auto* sub = FindAs<gui::CheckBox>(u8"viewer-lib-sub"))
                sub->SetOnToggled([self](bool) { self->ShowLibraryGrid(); });
            // Each Library section: the model tab's (for the front model), and the Browse and Library
            // details panes (for the model selected in their grid).
            const StringView sections[] = {u8"viewer", u8"browse", u8"library"};
            for (const StringView section : sections)
            {
                const String prefix(section);
                if (auto* add = FindAs<gui::Button>(Format(u8"{}-lib-add", prefix.AsView()).AsView()))
                    add->SetOnClick([self, prefix]()
                                    {
                                        const String path = self->SectionModel(prefix.AsView());
                                        if (!path.IsEmpty())
                                            self->AddToCollection(self->ComboCollection(Format(u8"{}-lib-add-target", prefix.AsView()).AsView()),
                                                                  Span<const String>(&path, 1));
                                    });
                if (auto* tag = FindAs<gui::TextField>(Format(u8"{}-lib-tag", prefix.AsView()).AsView()))
                    tag->SetOnSubmit([self, tag, prefix](StringView text)
                                     {
                                         const String path = self->SectionModel(prefix.AsView());
                                         if (path.IsEmpty() || text.IsEmpty())
                                             return;
                                         JsonValue a = Arg(u8"tag", Text(text));
                                         a.Set(String(u8"entries"), self->EntriesFor(Span<const String>(&path, 1)));
                                         self->Run(u8"tag.add", a);
                                         tag->SetText(u8"");
                                     });
                if (auto* note = FindAs<gui::TextEditBase>(Format(u8"{}-note", prefix.AsView()).AsView()))
                    note->SetOnTextChanged([self, prefix](StringView text)
                                           {
                                               const String path = self->SectionModel(prefix.AsView());
                                               if (path.IsEmpty() || self->m_syncingPanel)
                                                   return;
                                               const modelviewer::Entry* existing = self->m_library.Get().FindEntryByPath(path.AsView());
                                               if (existing == nullptr && text.IsEmpty())
                                                   return; // nothing to store, so the model stays out of the library
                                               String id;
                                               if (!self->EntryFor(path.AsView(), id))
                                                   return;
                                               JsonValue a = Arg(u8"id", Text(id.AsView()));
                                               a.Set(String(u8"note"), Text(text));
                                               self->Run(u8"entry.note.set", a);
                                           });
                if (auto* open = FindAs<gui::Button>(Format(u8"{}-open", prefix.AsView()).AsView()))
                    open->SetOnClick([self, prefix]()
                                     {
                                         const String path = self->SectionModel(prefix.AsView());
                                         if (!path.IsEmpty())
                                             self->Run(u8"model.open", Arg(u8"path", Text(path.AsView())));
                                     });
            }
            // A grid's selection shows in its details pane.
            if (auto* grid = FindAs<gui::ListView>(u8"viewer-grid"))
                grid->SetOnSelectionChanged([self](gui::ModelIndex) { self->ShowDetails(u8"browse"); });
            if (auto* grid = FindAs<gui::ListView>(u8"viewer-lib-grid"))
                grid->SetOnSelectionChanged([self](gui::ModelIndex) { self->ShowDetails(u8"library"); });
        }

        // Refreshes the tree, the grids' marks, the combos and the panel after the library changes
        // or the window starts.
        void RefreshLibrary()
        {
            m_libraryStale = false;
            RefreshLibraryTree();
            RefreshCollectionCombos();
            if (m_page == Page::Library)
                ShowLibraryGrid();
            else if (m_page == Page::Browse)
                ShowGrid();
            ShowLibraryPanel(); // every Library section: the model tab's and the details panes
        }

        // Rebuilds the tree: All models, Unsorted, the nested collections, then the tags with their
        // counts. The selection is kept by what it refers to, not by row.
        void RefreshLibraryTree()
        {
            auto* tree = FindAs<gui::TreeView>(u8"viewer-lib-tree");
            if (tree == nullptr)
                return;
            const LibraryNode kept = ChosenNode() != nullptr ? *ChosenNode() : LibraryNode{};
            m_syncingLibrary = true;
            tree->SetModel(nullptr);
            m_libraryModel = MakeUnique<LibraryTreeModel>(m_allocator);
            ModelViewerApp* self = this;
            m_libraryModel->renamable = [self](i32 node)
            {
                const LibraryNode* n = self->NodeAt(node);
                return n != nullptr && n->kind == LibraryNode::Kind::Collection;
            };
            m_libraryModel->takesModels = [self](i32 node)
            {
                const LibraryNode* n = self->NodeAt(node);
                return n != nullptr && n->kind == LibraryNode::Kind::Collection;
            };
            m_libraryModel->dropModels = [self](i32 node, StringView joined)
            {
                const LibraryNode* n = self->NodeAt(node);
                if (n == nullptr)
                    return;
                Array<String> paths;
                usize start = 0;
                while (start < joined.Size())
                {
                    usize end = start;
                    while (end < joined.Size() && joined[end] != u8'\n')
                        ++end;
                    if (end > start)
                        paths.PushBack(String(joined.SubStr(start, end - start)));
                    start = end + 1;
                }
                self->AddToCollection(n->collection, paths.AsSpan());
            };
            m_libraryNodes.Clear();
            const modelviewer::Library& library = m_library.Get();
            (void)AddLibraryNode(gui::TreeModel::kRoot, Format(u8"All models ({})", library.Entries().Size()).AsView(), LibraryNode{LibraryNode::Kind::All});
            (void)AddLibraryNode(gui::TreeModel::kRoot, u8"Unsorted", LibraryNode{LibraryNode::Kind::Unsorted});
            AddCollectionNodes(gui::TreeModel::kRoot, Guid{});
            ShowTagChips();
            tree->SetModel(m_libraryModel.Get());
            i32 choice = 0;
            for (usize i = 0; i < m_libraryNodes.Size(); ++i)
            {
                const LibraryNode& n = m_libraryNodes[i];
                tree->SetIdExpanded(static_cast<gui::ItemId>(i), true);
                if (n.kind == kept.kind && n.collection == kept.collection)
                    choice = static_cast<i32>(i);
            }
            if (!m_renameAfterRefresh.IsNil())
                for (usize i = 0; i < m_libraryNodes.Size(); ++i)
                    if (m_libraryNodes[i].kind == LibraryNode::Kind::Collection && m_libraryNodes[i].collection == m_renameAfterRefresh)
                        choice = static_cast<i32>(i);
            m_libraryChoice = choice;
            const gui::ItemId chosen[] = {static_cast<gui::ItemId>(choice)};
            tree->SetSelectedIds(Span<const gui::ItemId>(chosen, 1));
            m_syncingLibrary = false;
            if (!m_renameAfterRefresh.IsNil())
            {
                m_renameAfterRefresh = Guid{};
                for (i32 item = 0; item < static_cast<i32>(tree->GetItemCount()); ++item)
                    if (static_cast<i32>(tree->GetItemIndex(item).InternalId) == choice)
                        (void)tree->BeginEdit(item);
            }
        }
        // Shows the Library tab's tags as toggle chips with their counts; the grid shows the models
        // with any active tag. A chip's right-click menu renames the tag in place or deletes it from
        // every model.
        void ShowTagChips()
        {
            gui::Node* chips = FindById(u8"viewer-tag-chips");
            if (chips == nullptr)
                return;
            while (chips->ChildCount() > 0)
                chips->RemoveChild(chips->GetChildAt(chips->ChildCount() - 1));
            Array<String> tags;
            Array<u32> counts;
            m_library.Get().TagCounts(tags, counts);
            // A tag that no longer exists (renamed or deleted) is no longer active.
            for (usize i = m_activeTags.Size(); i-- > 0;)
            {
                bool there = false;
                for (const String& t : tags)
                    there = there || SameTag(t.AsView(), m_activeTags[i].AsView());
                if (!there)
                    m_activeTags.RemoveAt(i);
            }
            if (auto* clear = FindById(u8"viewer-tags-clear"))
                clear->SetDisplayed(!m_activeTags.IsEmpty());
            ModelViewerApp* self = this;
            if (tags.IsEmpty())
            {
                auto note = MakeRef<gui::Label>(m_allocator);
                note->AddClass(u8"viewer-empty-note");
                note->SetText(u8"No tags yet: tag a model in its side panel.");
                chips->AddChild(note.Get());
                return;
            }
            for (usize i = 0; i < tags.Size(); ++i)
            {
                const String tag = tags[i];
                bool on = false;
                for (const String& active : m_activeTags)
                    on = on || SameTag(active.AsView(), tag.AsView());
                auto chip = MakeRef<gui::Button>(m_allocator);
                chip->AddClass(u8"chip");
                if (on)
                    chip->AddClass(u8"checked");
                chip->SetText(Format(u8"{} ({})", tag.AsView(), counts[i]).AsView());
                chip->SetTooltip(on ? StringView(u8"On: its models show. Click to turn it off; right-click to rename or delete")
                                    : StringView(u8"Click to show its models (with any other tag that is on); right-click to rename or delete"));
                chip->SetOnClick([self, tag]() { self->ToggleTag(tag.AsView()); });
                gui::Button* raw = chip.Get();
                (void)chip->AddEventListener(gui::EventType::ContextMenu,
                                             [self, tag, raw](const gui::Event& e)
                                             {
                                                 e.Handled = true;
                                                 self->ShowTagMenu(*raw, tag.AsView());
                                             });
                chips->AddChild(chip.Get());
            }
        }

        // Tags match ignoring case, as the library's search matches them.
        [[nodiscard]] static bool SameTag(StringView a, StringView b) { return a.Size() == b.Size() && a.ContainsIgnoreCase(b); }

        void ToggleTag(StringView tag)
        {
            bool removed = false;
            for (usize i = 0; i < m_activeTags.Size(); ++i)
                if (SameTag(m_activeTags[i].AsView(), tag))
                {
                    m_activeTags.RemoveAt(i);
                    removed = true;
                    break;
                }
            if (!removed)
                m_activeTags.PushBack(String(tag));
            ShowTagChips();
            ShowLibraryGrid();
        }

        void ShowTagMenu(gui::Button& chip, StringView tag)
        {
            ModelViewerApp* self = this;
            const String name(tag);
            gui::Button* at = &chip;
            m_libraryMenu = MakeRef<gui::Menu>(m_allocator);
            (void)m_libraryMenu->AddItem(u8"Rename", [self, name, at]() { self->RenameTagInPlace(*at, name.AsView()); });
            (void)m_libraryMenu->AddItem(u8"Delete (from every model)", [self, name]() { self->Run(u8"tag.delete", Arg(u8"tag", Text(name.AsView()))); });
            m_libraryMenu->OpenBelow(chip, chip);
        }

        // Replaces the chip with a text field holding its name. Enter renames the tag on every
        // model; leaving the field puts the chip back.
        void RenameTagInPlace(gui::Button& chip, StringView tag)
        {
            gui::Node* chips = chip.GetParent();
            if (chips == nullptr)
                return;
            ModelViewerApp* self = this;
            auto field = MakeRef<gui::TextField>(m_allocator);
            field->SetText(tag);
            const String from(tag);
            field->SetOnSubmit([self, from](StringView text)
                               {
                                   if (!text.IsEmpty() && text != from.AsView())
                                   {
                                       JsonValue a = Arg(u8"from", Text(from.AsView()));
                                       a.Set(String(u8"to"), Text(text));
                                       for (String& active : self->m_activeTags)
                                           if (SameTag(active.AsView(), from.AsView()))
                                               active = String(text); // stays active under its new name
                                       self->Run(u8"tag.rename", a);
                                   }
                                   self->m_libraryStale = true;
                               });
            (void)field->AddEventListener(gui::EventType::FocusLost, [self](const gui::Event&) { self->m_libraryStale = true; });
            usize at = 0;
            while (at < chips->ChildCount() && chips->GetChildAt(at) != &chip)
                ++at;
            chips->AddChildAt(field.Get(), at);
            chip.SetDisplayed(false);
            field->RequestFocus();
        }

        [[nodiscard]] const LibraryNode* NodeAt(i32 node) const
        {
            return node >= 0 && static_cast<usize>(node) < m_libraryNodes.Size() ? &m_libraryNodes[static_cast<usize>(node)] : nullptr;
        }

        // A tree row's context menu, built for the row pressed as the hierarchy panel does. On a
        // collection it offers a new collection inside, rename and delete; elsewhere, a new collection.
        void ShowLibraryMenu(gui::TreeView& tree, Float2 at)
        {
            const i32 item = tree.ItemAt(at);
            const i32 nodeId = item >= 0 ? static_cast<i32>(tree.GetItemIndex(item).InternalId) : -1;
            const LibraryNode* node = NodeAt(nodeId);
            ModelViewerApp* self = this;
            m_libraryMenu = MakeRef<gui::Menu>(m_allocator);
            auto rename = [self, &tree, item]()
            {
                gui::TreeView* view = &tree;
                (void)self;
                (void)view->BeginEdit(item);
            };
            if (node != nullptr && node->kind == LibraryNode::Kind::Collection)
            {
                const String id = Format(u8"{}", node->collection);
                (void)m_libraryMenu->AddItem(u8"New collection inside", [self, id]() { self->NewCollection(id.AsView()); });
                (void)m_libraryMenu->AddItem(u8"Rename", rename);
                m_libraryMenu->AddSeparator();
                (void)m_libraryMenu->AddItem(u8"Export CSV...", [self, id]() { self->ExportCsvDialog(id.AsView()); });
                (void)m_libraryMenu->AddItem(u8"Copy Files to Folder...", [self, id]() { self->CopyFilesDialog(id.AsView()); });
                m_libraryMenu->AddSeparator();
                (void)m_libraryMenu->AddItem(u8"Delete (its models stay in the library)", [self, id]() { self->Run(u8"collection.delete", Arg(u8"id", Text(id.AsView()))); });
            }
            else
                (void)m_libraryMenu->AddItem(u8"New collection", [self]() { self->NewCollection({}); });
            m_libraryMenu->Open(tree, at);
        }

        // Creates a collection named "New collection" (inside `parent`, a collection id, when given)
        // and starts renaming its row in place.
        void NewCollection(StringView parent)
        {
            JsonValue a = Arg(u8"name", Text(u8"New collection"));
            if (!parent.IsEmpty())
                a.Set(String(u8"parent"), Text(parent));
            const modelviewer::CommandResult made = m_library.Commands().Execute(u8"collection.create", a);
            if (!made.ok)
            {
                LOG_WARNING(u8"ModelViewer", u8"collection.create: {}", made.error.AsView());
                return;
            }
            (void)Guid::TryParse(made.value.Get(String(u8"id")).AsString().AsView(), m_renameAfterRefresh);
            RefreshLibrary();
        }
        i32 AddLibraryNode(i32 parent, StringView text, const LibraryNode& node)
        {
            const i32 id = m_libraryModel->AddNode(parent, text);
            switch (node.kind)
            {
            case LibraryNode::Kind::All: m_libraryModel->SetIcon(id, u8"Editor/Icons/tiles.svg"); break;
            case LibraryNode::Kind::Unsorted: m_libraryModel->SetIcon(id, u8"Editor/Icons/file.svg"); break;
            case LibraryNode::Kind::Collection: m_libraryModel->SetIcon(id, u8"Editor/Icons/folder.svg"); break;
            }
            if (static_cast<usize>(id) >= m_libraryNodes.Size())
                m_libraryNodes.Resize(static_cast<usize>(id) + 1);
            m_libraryNodes[static_cast<usize>(id)] = node;
            return id;
        }
        void AddCollectionNodes(i32 parentNode, const Guid& parent)
        {
            for (const modelviewer::Collection& c : m_library.Get().Collections())
            {
                if (c.parent != parent)
                    continue;
                LibraryNode node{LibraryNode::Kind::Collection};
                node.collection = c.id;
                const i32 id = AddLibraryNode(parentNode, Format(u8"{} ({})", c.name.AsView(), c.members.Size()).AsView(), node);
                AddCollectionNodes(id, c.id);
            }
        }
        [[nodiscard]] const LibraryNode* ChosenNode() const
        {
            return m_libraryChoice >= 0 && static_cast<usize>(m_libraryChoice) < m_libraryNodes.Size() ? &m_libraryNodes[static_cast<usize>(m_libraryChoice)] : nullptr;
        }

        // Refills the collection combos (the browser's and the panel's) with the tree's collections
        // in order, nested ones indented.
        void RefreshCollectionCombos()
        {
            m_comboCollections.Clear();
            String items;
            auto walk = [&](auto& self, const Guid& parent, u32 depth) -> void
            {
                for (const modelviewer::Collection& c : m_library.Get().Collections())
                {
                    if (c.parent != parent)
                        continue;
                    if (!items.IsEmpty())
                        items += u8"|";
                    for (u32 d = 0; d < depth; ++d)
                        items += u8"  ";
                    items += c.name.AsView();
                    m_comboCollections.PushBack(c.id);
                    self(self, c.id, depth + 1);
                }
            };
            walk(walk, Guid{}, 0);
            const StringView combos[] = {u8"viewer-lib-add-target", u8"browse-lib-add-target", u8"library-lib-add-target"};
            for (const StringView id : combos)
                if (auto* combo = FindAs<gui::ComboBox>(id))
                {
                    const i32 kept = combo->GetSelectedIndex();
                    combo->SetItemsText(items);
                    combo->SetSelectedIndex(m_comboCollections.IsEmpty() ? -1 : Clamp(kept, 0, static_cast<i32>(m_comboCollections.Size()) - 1));
                }
        }

        // Fills the library grid with the models of the selected tree row that match the search.
        void ShowLibraryGrid()
        {
            const modelviewer::Library& library = m_library.Get();
            const LibraryNode* node = ChosenNode();
            auto* searchField = FindAs<gui::TextField>(u8"viewer-lib-search");
            auto* everywhereBox = FindAs<gui::CheckBox>(u8"viewer-lib-everywhere");
            auto* subBox = FindAs<gui::CheckBox>(u8"viewer-lib-sub");
            const bool everywhere = everywhereBox != nullptr && everywhereBox->IsChecked();
            const bool sub = subBox == nullptr || subBox->IsChecked();
            modelviewer::SearchQuery query = modelviewer::ParseQuery(searchField != nullptr ? searchField->GetText() : StringView{});
            Guid scope;
            Guid named; // the collection whose member names the cells show
            if (node != nullptr && !everywhere)
            {
                if (node->kind == LibraryNode::Kind::Collection)
                {
                    scope = node->collection;
                    named = node->collection;
                }
            }
            Array<Guid> found = modelviewer::Search(library, query, scope);
            if (node != nullptr && !everywhere && node->kind == LibraryNode::Kind::Collection && !sub)
            {
                const modelviewer::Collection* c = library.FindCollection(node->collection);
                for (usize i = found.Size(); i-- > 0;)
                {
                    bool own = false;
                    for (const modelviewer::Member& m : c->members)
                        own = own || m.entry == found[i];
                    if (!own)
                        found.RemoveAt(i);
                }
            }
            if (node != nullptr && !everywhere && node->kind == LibraryNode::Kind::Unsorted)
                for (usize i = found.Size(); i-- > 0;)
                {
                    Array<Guid> in;
                    library.CollectionsOf(found[i], in);
                    if (!in.IsEmpty())
                        found.RemoveAt(i);
                }
            // Keep the models with any active tag (each tag adds its models).
            found = modelviewer::WithAnyTag(library, found.AsSpan(), m_activeTags.AsSpan());
            m_libraryGrid.rows.Clear();
            Array<String> undrawn;
            for (const Guid& id : found)
            {
                const modelviewer::Entry* e = library.FindEntry(id);
                modelviewer_window::GridRow row;
                row.path = e->path;
                row.entry = id;
                row.inLibrary = true;
                row.missing = modelviewer::Library::IsMissing(*e);
                if (!named.IsNil())
                    row.sub = String(library.MemberName(named, id));
                if (!row.missing && ThumbImageFor(e->path.AsView()) == nullptr)
                    undrawn.PushBack(e->path);
                m_libraryGrid.rows.PushBack(foundation::core::Move(row));
            }
            m_libraryGrid.Reset();
            if (auto* count = FindAs<gui::Label>(u8"viewer-lib-count"))
                count->SetText(Format(u8"{} models", m_libraryGrid.rows.Size()).AsView());
            if (m_thumbnails)
                m_thumbnails->SetQueue(undrawn.AsSpan());
        }

        // Points a missing model's entry at its file's new location.
        void RelinkDialog(const Guid& entry)
        {
            shell::IDialogService* dialogs = m_host != nullptr && m_host->Shell() != nullptr ? m_host->Shell()->Dialogs() : nullptr;
            if (dialogs == nullptr)
                return;
            const shell::FileFilter filters[] = {{u8"Models", modelviewer::kModelExtensions}};
            ModelViewerApp* self = this;
            const String id = Format(u8"{}", entry);
            dialogs->ShowOpenFile([self, id](Span<const String> paths)
                                  {
                                      if (paths.IsEmpty())
                                          return;
                                      JsonValue a = Arg(u8"id", Text(id.AsView()));
                                      a.Set(String(u8"path"), Text(paths[0].AsView()));
                                      self->Run(u8"entry.relink", a);
                                  },
                                  Span<const shell::FileFilter>(filters, 1));
        }

        // Refreshes every Library section: the model tab's (for the front model) and the Browse and
        // Library details panes (for the model selected in each grid).
        void ShowLibraryPanel()
        {
            if (const modelviewer::ModelTab* tab = m_tabs.ActiveTab())
                ShowLibrarySection(u8"viewer", tab->path.AsView());
            ShowDetails(u8"browse");
            ShowDetails(u8"library");
        }

        // The model a Library section shows: the front tab's, or the one selected in a grid.
        [[nodiscard]] String SectionModel(StringView prefix) const
        {
            if (prefix == StringView(u8"viewer"))
            {
                const modelviewer::ModelTab* tab = const_cast<ModelViewerApp*>(this)->m_tabs.ActiveTab();
                return tab != nullptr ? tab->path : String{};
            }
            const bool browse = prefix == StringView(u8"browse");
            const Array<String> chosen = SelectedPaths(browse ? StringView(u8"viewer-grid") : StringView(u8"viewer-lib-grid"),
                                                       browse ? m_browseGrid : m_libraryGrid);
            return chosen.Size() == 1 ? chosen[0] : String{};
        }

        // Fills a grid's details pane (`prefix` is browse or library) for the selected model: its
        // stats as its cook counted them (once its thumbnail is drawn) and its Library section. With
        // none or several selected, it shows a hint instead.
        void ShowDetails(StringView prefix)
        {
            const bool browse = prefix == StringView(u8"browse");
            const Array<String> chosen = SelectedPaths(browse ? StringView(u8"viewer-grid") : StringView(u8"viewer-lib-grid"),
                                                       browse ? m_browseGrid : m_libraryGrid);
            gui::Node* model = FindById(Format(u8"{}-details-model", prefix).AsView());
            auto* empty = FindAs<gui::Label>(Format(u8"{}-details-empty", prefix).AsView());
            if (model == nullptr || empty == nullptr)
                return;
            model->SetDisplayed(chosen.Size() == 1);
            empty->SetDisplayed(chosen.Size() != 1);
            if (chosen.Size() != 1)
            {
                empty->SetText(chosen.IsEmpty() ? StringView(u8"Select a model to see its details.")
                                                : Format(u8"{} models selected.", chosen.Size()).AsView());
                return;
            }
            const String& path = chosen[0];
            if (auto* file = FindAs<gui::Label>(Format(u8"{}-file", prefix).AsView()))
            {
                file->SetText(modelviewer::FileTitle(path.AsView()).AsView());
                file->SetTooltip(path.AsView());
            }
            modelviewer::ModelStats stats;
            if (modelviewer::ReadCachedStats(m_cacheRoot.AsView(), path.AsView(), stats, m_settings.value.looseTextures))
            {
                ShowStatRows(Format(u8"{}-stats", prefix).AsView(), stats);
                ShowContents(prefix, stats);
            }
            else if (gui::Node* rows = FindById(Format(u8"{}-stats", prefix).AsView()))
            {
                while (rows->ChildCount() > 0)
                    rows->RemoveChild(rows->GetChildAt(rows->ChildCount() - 1));
                auto note = MakeRef<gui::Label>(m_allocator);
                note->AddClass(u8"viewer-empty-note");
                note->SetText(FileExists(path.AsView()) ? StringView(u8"Its numbers come once its picture is drawn.")
                                                        : StringView(u8"Its file is not there."));
                rows->AddChild(note.Get());
                ShowContents(prefix, modelviewer::ModelStats{}); // nothing known yet
            }
            ShowLibrarySection(prefix, path.AsView());
        }

        // Fills a Library section (`prefix` is viewer for the model tab, or browse or library for a
        // details pane) for the model at `path`: the collections it is in with its editable name in
        // each and a button to remove it, its tags (a click removes one), and its note.
        void ShowLibrarySection(StringView prefix, StringView path)
        {
            gui::Node* rows = FindById(Format(u8"{}-lib-memberships", prefix).AsView());
            gui::Node* chips = FindById(Format(u8"{}-lib-tags", prefix).AsView());
            if (path.IsEmpty() || rows == nullptr || chips == nullptr)
                return;
            while (rows->ChildCount() > 0)
                rows->RemoveChild(rows->GetChildAt(rows->ChildCount() - 1));
            while (chips->ChildCount() > 0)
                chips->RemoveChild(chips->GetChildAt(chips->ChildCount() - 1));
            const modelviewer::Library& library = m_library.Get();
            const modelviewer::Entry* e = library.FindEntryByPath(path);
            ModelViewerApp* self = this;
            if (e == nullptr)
            {
                auto note = MakeRef<gui::Label>(m_allocator);
                note->AddClass(u8"viewer-empty-note");
                note->SetText(u8"Not in the library yet: add it to a collection, or tag it.");
                rows->AddChild(note.Get());
            }
            else
            {
                Array<Guid> in;
                library.CollectionsOf(e->id, in);
                const String entryId = Format(u8"{}", e->id);
                for (const Guid& g : in)
                {
                    auto row = MakeRef<gui::Panel>(m_allocator);
                    row->AddClass(u8"viewer-membership");
                    auto label = MakeRef<gui::Label>(m_allocator);
                    label->SetText(library.FindCollection(g)->name.AsView());
                    auto name = MakeRef<gui::TextField>(m_allocator);
                    name->SetPlaceholder(u8"its name here");
                    name->SetText(library.MemberName(g, e->id));
                    name->SetTooltip(u8"The id a game knows it by, unique across the top-level collection (Enter)");
                    const String collectionId = Format(u8"{}", g);
                    name->SetOnSubmit([self, collectionId, entryId](StringView text)
                                      {
                                          JsonValue a = Arg(u8"collection", Text(collectionId.AsView()));
                                          a.Set(String(u8"entry"), Text(entryId.AsView()));
                                          a.Set(String(u8"name"), Text(text));
                                          self->Run(u8"member.rename", a);
                                          self->m_libraryStale = true; // a refused name reverts on the next refresh
                                      });
                    auto out = MakeRef<gui::Button>(m_allocator);
                    out->AddClass(u8"slot-button");
                    out->SetIconSource(String(u8"Editor/Icons/close.svg"));
                    out->SetTooltip(u8"Take it out of this collection");
                    out->SetOnClick([self, collectionId, entryId]()
                                    {
                                        JsonValue a = Arg(u8"collection", Text(collectionId.AsView()));
                                        JsonValue ids = JsonValue::MakeArray();
                                        ids.Add(Text(entryId.AsView()));
                                        a.Set(String(u8"entries"), ids);
                                        self->Run(u8"member.remove", a);
                                    });
                    row->AddChild(label.Get());
                    row->AddChild(name.Get());
                    row->AddChild(out.Get());
                    rows->AddChild(row.Get());
                }
                if (in.IsEmpty())
                {
                    auto note = MakeRef<gui::Label>(m_allocator);
                    note->AddClass(u8"viewer-empty-note");
                    note->SetText(u8"In the library, in no collection yet.");
                    rows->AddChild(note.Get());
                }
                for (const String& t : e->tags)
                {
                    auto chip = MakeRef<gui::Button>(m_allocator);
                    chip->AddClass(u8"chip");
                    chip->AddClass(u8"checked");
                    chip->AddClass(u8"viewer-tag");
                    chip->SetText(t.AsView());
                    chip->SetIconSource(String(u8"Editor/Icons/close.svg"));
                    chip->SetTooltip(u8"Take this tag off");
                    const String tag = t;
                    chip->SetOnClick([self, tag, entryId]()
                                     {
                                         JsonValue a = Arg(u8"tag", Text(tag.AsView()));
                                         JsonValue ids = JsonValue::MakeArray();
                                         ids.Add(Text(entryId.AsView()));
                                         a.Set(String(u8"entries"), ids);
                                         self->Run(u8"tag.remove", a);
                                     });
                    chips->AddChild(chip.Get());
                }
            }
            m_syncingPanel = true;
            if (auto* note = FindAs<gui::TextEditBase>(Format(u8"{}-note", prefix).AsView()))
            {
                const StringView text = e != nullptr ? e->note.AsView() : StringView{};
                if (note->GetText() != text)
                    note->SetText(text);
            }
            m_syncingPanel = false;
        }

        /// Stores the stats a cook counted on the model's entry, when it has one, for search.
        void KeepStats(StringView path, const modelviewer::ModelStats& stats)
        {
            const modelviewer::Entry* e = m_library.Get().FindEntryByPath(path);
            if (e == nullptr || m_library.ReadOnly())
                return;
            modelviewer::EntryStats kept;
            kept.known = true;
            kept.rigged = stats.skins > 0;
            kept.clips = static_cast<u32>(stats.clips.Size());
            kept.triangles = stats.triangles;
            kept.size = stats.size;
            if (e->stats.known && e->stats.rigged == kept.rigged && e->stats.clips == kept.clips && e->stats.triangles == kept.triangles)
                return;
            (void)m_library.Get().SetStats(e->id, kept);
            (void)m_library.File().Save(m_library.Get());
        }

        void SaveSettings() { (void)m_settings.Save(); }
        // Moves a model or folder to the front of the recents (the Recent button's menu).
        void NoteRecent(bool folder, StringView path) { m_settings.Recent(folder, path); }

        // Applies the settings after a change or at startup: the density's sheet, the loose texture
        // rule, the panel's toggle. The thumbnail size and cache folder take effect on the next start.
        void ApplySettings()
        {
            if (m_settings.value.density != m_density)
                ApplySheets();
            if (m_thumbnails)
                m_thumbnails->SetLooseTextures(m_settings.value.looseTextures);
            ApplyTileSize();
            m_syncingPanel = true;
            if (auto* grid = FindAs<gui::Button>(u8"viewer-grid-toggle"))
                grid->SetChecked(m_settings.value.grid);
            m_syncingPanel = false;
        }

        // Sizes the grid tiles from the settings and sets both sliders to match.
        void ApplyTileSize()
        {
            const u32 step = Min(m_settings.value.tileSize, 2u);
            for (StringView id : kGridLists)
                if (auto* list = FindAs<gui::ListView>(id))
                {
                    list->SetCellWidth(kTileWidths[step]);
                    list->SetCellHeight(kTileHeights[step]);
                    list->RemoveClass(u8"tiles-small");
                    list->RemoveClass(u8"tiles-large");
                    if (step == 0)
                        list->AddClass(u8"tiles-small");
                    else if (step == 2)
                        list->AddClass(u8"tiles-large");
                }
            m_syncingTiles = true;
            for (StringView id : kTileSliders)
                if (auto* slider = FindAs<gui::Slider>(id))
                    slider->SetValue(static_cast<f64>(step));
            m_syncingTiles = false;
        }

        // Why the cache cannot be cleared now (an open model reads its cooked files; a thumbnail
        // being drawn writes there), or empty when it can.
        [[nodiscard]] String CacheBusy() const
        {
            if (m_tabs.Count() > 0)
                return String(u8"close the model tabs first: an open model reads its cooked files");
            if (m_thumbnails && m_thumbnails->Remaining() > 0)
                return String(u8"thumbnails are being drawn: try again when they are done");
            return {};
        }
        // After the cache is cleared, recreates the thumbnail folder for what is drawn next. The
        // pictures on screen stay in memory for the grid's tiles; their files are drawn again when
        // next queued.
        void CacheCleared() { (void)CreateDirectories(m_thumbnailRoot.AsView()); }

        // A screenshot's file name for the model in `tab`, from the settings' pattern.
        [[nodiscard]] String ScreenshotNameFor(const modelviewer::ModelTab& tab) const
        {
            const StringView clip = tab.playback.Get() != nullptr && tab.playback->HasClips() ? tab.playback->ClipName(tab.playback->Selected()).AsView() : StringView{};
            return modelviewer::ScreenshotName(m_settings.value.screenshotPattern.AsView(), tab.path.AsView(), clip, GetTicks());
        }

        // The Recent button's menu: recently opened models, then recently browsed folders.
        void ShowRecentMenu(gui::Button& anchor)
        {
            ModelViewerApp* self = this;
            m_recentMenu = MakeRef<gui::Menu>(m_allocator);
            for (const String& model : m_settings.value.recentModels)
            {
                const String path = model;
                gui::MenuItem* item = m_recentMenu->AddItem(modelviewer::FileTitle(path.AsView()).AsView(),
                                                            [self, path]() { self->Run(u8"model.open", Arg(u8"path", Text(path.AsView()))); });
                item->SetTooltip(path.AsView());
            }
            if (!m_settings.value.recentModels.IsEmpty() && !m_settings.value.recentFolders.IsEmpty())
                m_recentMenu->AddSeparator();
            for (const String& folder : m_settings.value.recentFolders)
            {
                const String path = folder;
                (void)m_recentMenu->AddItem(Format(u8"Browse {}", path.AsView()).AsView(), [self, path]() { self->Run(u8"browse.folder", Arg(u8"path", Text(path.AsView()))); });
            }
            if (m_settings.value.recentModels.IsEmpty() && m_settings.value.recentFolders.IsEmpty())
                m_recentMenu->AddItem(u8"Nothing opened yet", []() {})->SetEnabled(false);
            m_recentMenu->OpenBelow(anchor, anchor);
        }

        // Opens Preferences as a modal window over the viewer; Save runs settings.set.
        void ShowPreferences()
        {
            if (!m_preferences)
            {
                String markup;
                if (!m_provider->LoadText(StringView(modelviewer::PreferencesDialog::kPath), markup))
                {
                    LOG_ERROR(u8"ModelViewer", u8"the Preferences markup ({}) is not there", StringView(modelviewer::PreferencesDialog::kPath));
                    return;
                }
                gui::MarkupLoader loader(gui::ElementSet::All(), m_allocator, nullptr, m_fonts.Get());
                loader.SetDataModel(m_guiHost->Context().GetDataRef());
                RefPtr<gui::Node> body = loader.LoadFromString(markup.AsView());
                if (!body)
                    return;
                ModelViewerApp* self = this;
                modelviewer::PreferencesDialog::Services services;
                services.save = [self](const JsonValue& changes)
                {
                    const CommandResult result = self->m_library.Commands().Execute(u8"settings.set", changes);
                    return result.ok ? String{} : result.error;
                };
                services.clearCache = [self]()
                {
                    const CommandResult result = self->m_library.Commands().Execute(u8"cache.clear");
                    return result.ok ? Format(u8"Cleared {}.", Megabytes(static_cast<u64>(result.value.Get(String(u8"freed")).AsNumber())).AsView())
                                     : Format(u8"Not cleared: {}.", result.error.AsView());
                };
                services.cacheSize = [self]()
                {
                    const String base = CacheBase(self->m_settings.value);
                    return Megabytes(modelviewer::FolderBytes(PathJoin(base.AsView(), u8"cache").AsView()) +
                                     modelviewer::FolderBytes(PathJoin(base.AsView(), u8"thumbnails").AsView()));
                };
                services.chooseFolder = [self](StringView start, Function<void(StringView)> chosen)
                {
                    shell::IDialogService* dialogs = self->m_host != nullptr && self->m_host->Shell() != nullptr ? self->m_host->Shell()->Dialogs() : nullptr;
                    if (dialogs == nullptr)
                        return;
                    dialogs->ShowOpenFolder([done = foundation::core::Move(chosen)](Span<const String> paths)
                                            {
                                                if (!paths.IsEmpty())
                                                    done(paths[0].AsView());
                                            },
                                            start);
                };
                services.defaultScreenshots = modelviewer::DefaultScreenshotFolder();
                services.defaultCache = ViewerDataFolder();
                m_preferences = MakeUnique<modelviewer::PreferencesDialog>(m_allocator, m_allocator, foundation::core::Move(services), foundation::core::Move(body));
            }
            (void)m_preferences->Open(*m_root, m_settings.value);
        }

        [[nodiscard]] static String Megabytes(u64 bytes)
        {
            return Format(u8"{} MB", FormatFixed(static_cast<f64>(bytes) / (1024.0 * 1024.0), 1).AsView());
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

        // The empty view while nothing is open: how to open a model, or why the last one failed.
        void ShowEmpty()
        {
            gui::Node* empty = FindById(u8"viewer-empty");
            if (empty == nullptr)
                return;
            const modelviewer::ModelTab* front = m_tabs.Count() > 0 ? m_tabs.ActiveTab() : nullptr;
            const bool open = front != nullptr && front->ready;
            empty->SetDisplayed(!open);
            m_viewport->View()->SetDisplayed(open);
            if (!open)
            {
                if (auto* file = FindAs<gui::Label>(u8"viewer-file"))
                    file->SetText(u8"");
                if (gui::Node* rows = FindById(u8"viewer-stats"))
                    while (rows->ChildCount() > 0)
                        rows->RemoveChild(rows->GetChildAt(rows->ChildCount() - 1));
            }
            if (auto* text = FindAs<gui::Label>(u8"viewer-empty-text"))
            {
                if (front != nullptr && !front->ready)
                    text->SetText(Format(u8"Cooking {}...", front->title.AsView()).AsView());
                else if (!m_failure.IsEmpty())
                    text->SetText(m_failure.AsView());
                else if (front == nullptr)
                    text->SetText(m_emptyText.AsView());
            }
        }

        // Measures a model's bounds as placed when it opens: each mesh's box through its entity's
        // world transform, which includes the file's node scale (often 0.01 or 100).
        void MeasureBounds(modelviewer::ModelTab& tab)
        {
            tab.center = Float3{};
            tab.radius = 1.0f;
            scene::Scene* level = m_viewport->Scene();
            auto* meshes = level != nullptr ? level->GetSystem<engine::render::MeshComponentManager>() : nullptr;
            if (meshes == nullptr || !tab.instance.root.IsAssigned())
                return;
            AABB box = AABB::Empty();
            meshes->ForEach([&](engine::render::MeshComponent& component, scene::EntityHandle entity)
                            {
                                const foundation::geometry::StaticMesh* mesh = component.mesh.Get();
                                if (mesh == nullptr || !mesh->bounds.IsValid() || !Under(*level, entity, tab.instance.root))
                                    return;
                                box = Merge(box, TransformAABB(mesh->bounds, level->ComposeWorldMatrix(entity)));
                            });
            if (!box.IsValid())
                return;
            tab.center = (box.min + box.max) * 0.5f;
            tab.radius = Max(Length(box.max - box.min) * 0.5f, 0.01f);
            tab.size = box.max - box.min;
        }

        [[nodiscard]] static bool Under(const scene::Scene& level, scene::EntityHandle entity, scene::EntityHandle root)
        {
            for (scene::EntityHandle e = entity; level.IsValid(e); e = level.GetParent(e))
                if (e == root)
                    return true;
            return false;
        }

        void Bounds(Float3& center, f32& radius)
        {
            const modelviewer::ModelTab* tab = m_tabs.ActiveTab();
            center = tab != nullptr ? tab->center : Float3{};
            radius = tab != nullptr ? tab->radius : 1.0f;
        }

        // The clip range follows the model, so a 2 cm prop and a 200 m building are both in reach.
        // The near plane sits at a twentieth of the model's size: that keeps the scene's reverse-Z
        // depth precision well above the grid's fixed bias toward the viewer, which a nearer plane
        // would let the grid show through.
        void ClipToModel(f32 radius)
        {
            editor::EditorCamera& camera = m_viewport->Camera();
            camera.nearZ = Max(radius * 0.05f, 0.0001f);
            camera.farZ = Max(radius * 100.0f, 50.0f);
        }

        void FrameModel(bool snap = false)
        {
            Float3 center;
            f32 radius;
            Bounds(center, radius);
            ClipToModel(radius);
            m_viewport->Frame(center, radius);
            if (snap)
                m_viewport->Camera().Advance(1000.0f); // jump straight there, with no glide on opening
        }

        // Draws the ground grid with cells sized to the model (a tenth of its size, rounded to a
        // power of ten) and fading out at twenty times its size, so a 2 cm prop and a 200 m
        // building both sit on a usable grid.
        void DrawGrid()
        {
            if (!m_settings.value.grid || !m_viewport || !m_viewport->IsValid())
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

        // Finds a window node by id: in the page, else in the lighting popover, which is outside the
        // page while closed and in the popup layer while open.
        [[nodiscard]] gui::Node* FindById(StringView id) const
        {
            gui::Node* found = m_frame ? FindIn(*m_frame, id) : nullptr;
            return found != nullptr || !m_lightPopover ? found : FindIn(*m_lightPopover, id);
        }
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
        bool m_rest = false;
        UniquePtr<vfs::NativeFileSystem> m_data;
        UniquePtr<gui::VgShaderLibrary> m_shaders;
        UniquePtr<fonts::TrueTypeFontService> m_fonts;
        UniquePtr<gui::vfs::VfsResourceProvider> m_provider;
        UniquePtr<gui::GuiHost> m_guiHost;
        RefPtr<gui::SceneRoot> m_root;
        RefPtr<gui::Node> m_frame;
        UniquePtr<editor::PreviewViewport> m_viewport;
        modelviewer::ModelTabs m_tabs{m_allocator};
        bool m_syncingTabs = false; // the tab bar is being changed by code, not a click
        bool m_turntable = false;
        f32 m_turnAngle = 0.0f;
        bool m_syncingBar = false;   // the animation bar is being set by code, not the user
        bool m_syncingView = false;  // the View section is being set by a command, not the user
        String m_thumbnailRoot;
        bool m_shownPlaying = true; // the play button's state (shows pause while playing, matching the markup's initial state)
        bool m_showSkeleton = false;
        Array<Float4x4> m_bonePoses;

        // The browser's state.
        struct ThumbImage
        {
            String path;
            UniquePtr<foundation::image::Image> image;
        };
        struct FolderNode
        {
            String path;
            i32 parent = -1;
            bool listed = false; // its subfolders have been listed
        };
        bool m_browsing = false; // a fixed page (Browse or Library) is in front, not a model
        Page m_page = Page::Browse;

        // The library's state in the window.
        bool m_libraryStale = true;
        bool m_syncingLibrary = false;
        UniquePtr<LibraryTreeModel> m_libraryModel;
        RefPtr<gui::Menu> m_libraryMenu;
        Guid m_renameAfterRefresh; // a newly created collection whose row starts renaming
        Array<LibraryNode> m_libraryNodes; // by the tree model's node id
        i32 m_libraryChoice = 0;
        Array<Guid> m_comboCollections;    // what each combo item names
        modelviewer_window::GridModel m_browseGrid;
        modelviewer_window::GridModel m_libraryGrid;
        RefPtr<gui::Menu> m_gridMenu;
        modelviewer::SettingsStore m_settings;
        modelviewer::FileLoads m_tabLoads{m_allocator}; // the model tabs' file reads, off the main thread
        String m_emptyText; // the empty view's text from the markup
        String m_gridCount; // the browse grid's "n of m models"
        RefPtr<gui::ToastHost> m_toasts;
        bool m_syncingTiles = false; // the tile sliders are being set by code
        Array<String> m_activeTags; // the Library tab's tags that are on
        bool m_panelShown = true;   // whether a model tab's side panel is shown (its bar's toggle)
        RefPtr<gui::Node> m_sidePanel; // holds the panel while it is out of the splitter
        RefPtr<gui::Node> m_lightPopover; // the lighting sliders, shown as the Lighting button's popover
        String m_shownProgress;
        String m_density; // the density whose sheet is applied
        u32 m_thumbnailSize = 128;
        UniquePtr<modelviewer::PreferencesDialog> m_preferences;
        RefPtr<gui::Menu> m_recentMenu;
        UniquePtr<modelviewer::ThumbnailRenderer> m_thumbnails;
        UniquePtr<gui::TreeModel> m_folderModel;
        Array<FolderNode> m_folderNodes; // by the tree model's node id
        Array<String> m_folderModels;     // every model under the chosen folder
        Array<modelviewer::ModelStats> m_folderStats;
        Array<bool> m_folderStatsKnown;
        modelviewer::BrowseFilter m_filter;
        Array<ThumbImage> m_thumbImages;
        u32 m_doneSinceFilter = 0;

        bool m_syncingPanel = false; // the panel's note is being set by code, not the user

        // The library, and the MCP server agents use.
        modelviewer::LibrarySession m_library{m_allocator};
        foundation::mcp::McpServer m_mcpServer;
        UniquePtr<foundation::mcp::McpHttpHost> m_mcpHost;
        u16 m_mcpPort = 0;
        foundation::rhi::ScreenshotCapture m_capture;
        String m_failure;
    };
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (StringView(reinterpret_cast<const utf8char*>(argv[i])) == StringView(u8"--headless"))
            return RunHeadless();
    ModelViewerApp app;
    for (int i = 1; i < argc; ++i)
    {
        const StringView arg(reinterpret_cast<const utf8char*>(argv[i]));
        if (arg == StringView(u8"--rest"))
        {
            app.SetRest(true);
            continue;
        }
        if (arg == StringView(u8"--skeleton"))
        {
            app.SetSkeleton(true);
            continue;
        }
        if (arg == StringView(u8"--mcp"))
        {
            app.SetMcp(kDefaultMcpPort);
            continue;
        }
        if (arg == StringView(u8"--mcp-port") && i + 1 < argc)
        {
            app.SetMcp(static_cast<u16>(std::atoi(argv[++i])));
            continue;
        }
        if (arg.Size() > 0 && arg[0] == u8'-')
        {
            if (arg.StartsWith(u8"--screenshot") && i + 1 < argc && arg != StringView(u8"--screenshot-exit"))
                ++i; // skip the option's value (ScreenshotOptionsFromArguments reads it)
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

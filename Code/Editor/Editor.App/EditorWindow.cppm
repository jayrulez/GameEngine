// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :editor_window partition.
//
// EditorWindow: the editor's GUI on its main window (Documentation/Specs/editor-gui.md, 5.1,
// layer 3). It owns the GUI host (the window's input, frames, renderer and picture baker), the
// fonts the editor's sheet names, the editor's own files (EditorResources) and the frame read
// from Data/Editor/Shell.xml, with the toast host over it. Its look is the editor's sheet
// (Editor/editor.css over its tokens), the chosen density's sheet after it and a `:root` block of
// the user's inputs (the accent), then the user's theme overrides (5.7, layer 1: a file in their
// data folder, so any token or rule is theirs to set), the scheme and less motion through the
// GUI's media; a change to those files on disk (the editor's development loop, or the user's
// overrides) applies at once.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:editor_window;

import foundation.core;
import foundation.shell;
import foundation.graphics;
import foundation.rhi; // PresentMode
import foundation.runtime;
import foundation.runtime.client; // IApplicationHost
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.harfbuzz; // the shaper the editor's fonts get
import foundation.fonts.bidi;     // mixed-direction lines in visual order
import foundation.vfs;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.docking; // its sheet
import foundation.gui.toolkit; // its sheet and elements
import foundation.gui.code;    // its sheet and elements
import foundation.profiler.gui; // its sheet and elements
import editor.core;
import :status_bar;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace fonts = foundation::fonts;

    /// The user's appearance inputs (Preferences): the scheme, the accent, the density, less
    /// motion and the UI scale. The defaults are the editor's (rulings 6, 7, 13).
    struct EditorLook
    {
        bool dark = true;
        String accent{u8"#2f6fdf"}; // azure
        String density{u8"compact"}; // compact, default or comfortable: Editor/density-<name>.css
        bool reduceMotion = false;
        f32 uiScale = 1.0f;
    };

    /// The densities the editor's sheets give (Editor/density-<name>.css), in their order.
    inline constexpr StringView kEditorDensities[] = {u8"compact", u8"default", u8"comfortable"};
    /// The accents Preferences offers, each a fill white text reads on (ruling 7; azure is the
    /// editor's, ruling 13). Any other #rrggbb is taken too.
    struct EditorAccent
    {
        StringView name;
        StringView fill;
    };
    inline constexpr EditorAccent kEditorAccents[] = {
        {u8"Azure", u8"#2f6fdf"},
        {u8"Violet", u8"#6d5bd8"},
        {u8"Teal", u8"#0f8a7e"},
        {u8"Magenta", u8"#b03d93"},
    };

    /// Whether `text` is a colour as #rrggbb.
    [[nodiscard]] inline bool IsHexColor(StringView text) noexcept
    {
        if (text.Size() != 7 || text[0] != u8'#')
            return false;
        for (usize i = 1; i < 7; ++i)
        {
            const char8_t c = text[i];
            if (!((c >= u8'0' && c <= u8'9') || (c >= u8'a' && c <= u8'f') || (c >= u8'A' && c <= u8'F')))
                return false;
        }
        return true;
    }

    /// The look the user's settings ask for; what they leave unset or name wrongly (a density
    /// with no sheet, an accent that is no colour) is the editor's default.
    [[nodiscard]] inline EditorLook LookFrom(const editor::EditorUiSettings* ui)
    {
        EditorLook look;
        if (ui == nullptr)
            return look;
        look.dark = ui->scheme.AsView() != StringView(u8"light");
        if (IsHexColor(ui->accent.AsView()))
            look.accent = ui->accent;
        for (StringView density : kEditorDensities)
            if (ui->density.AsView() == density)
                look.density = String(density);
        look.reduceMotion = ui->reduceMotion;
        look.uiScale = Clamp(ui->uiScale, editor::kUiScaleMin, editor::kUiScaleMax);
        return look;
    }
    /// `look` into the user's settings (the section's other fields kept).
    inline void StoreLook(editor::EditorUiSettings& ui, const EditorLook& look)
    {
        ui.scheme = String(look.dark ? StringView(u8"dark") : StringView(u8"light"));
        ui.accent = look.accent;
        ui.density = look.density;
        ui.reduceMotion = look.reduceMotion;
        ui.uiScale = look.uiScale;
    }

    /// How every editor window presents: vsync, with a third swapchain image, so a compositor late
    /// in giving one back (two images held the frame for whole refreshes, the user's spikes) costs
    /// nothing; the frame of latency it adds is no matter in an editor. A game keeps the default.
    [[nodiscard]] inline foundation::graphics::RenderWindowDesc EditorRenderWindowDesc() noexcept
    {
        foundation::graphics::RenderWindowDesc desc;
        desc.presentMode = foundation::rhi::PresentMode::Fifo;
        desc.bufferCount = 3;
        return desc;
    }

    class EditorWindow
    {
    public:
        /// The frame's markup, its sheet and the folder the editor's files are in, under the data root.
        static constexpr const char8_t* kFramePath = u8"Editor/Shell.xml";
        static constexpr const char8_t* kSheetPath = u8"Editor/editor.css";
        /// The user's theme overrides: the folder under their data folder, and the file in it.
        static constexpr const char8_t* kThemeFolder = u8"theme";
        static constexpr const char8_t* kThemeOverridesPath = u8"overrides.css";

        explicit EditorWindow(IAllocator& allocator) : m_allocator(allocator) {}
        EditorWindow(const EditorWindow&) = delete;
        EditorWindow& operator=(const EditorWindow&) = delete;
        ~EditorWindow() { Close(); }

        /// Opens the GUI on the host's main window over the data root: the shaders, the fonts, the
        /// frame and the sheet, with the user's theme overrides from `themeFolder` (made if it is
        /// not there; empty: none). False (logged) when the window, the device or the shaders are
        /// missing; the editor then runs with no interface.
        bool Open(foundation::runtime::IApplicationHost& host, StringView dataRoot, const EditorLook& look, StringView themeFolder)
        {
            m_window = host.MainRenderWindow();
            if (m_window == nullptr || host.Graphics() == nullptr || host.Graphics()->Raw() == nullptr || host.Shell() == nullptr)
            {
                LOG_ERROR(u8"Editor", u8"no window or device: the editor runs without its interface");
                return false;
            }
            m_dataRoot = String(dataRoot);
            m_data = MakeUnique<foundation::vfs::NativeFileSystem>(m_allocator, dataRoot, m_allocator);
            m_shaders = MakeUnique<gui::VgShaderLibrary>(m_allocator, m_allocator);
            if (!m_shaders->Load(*host.Graphics()->Raw(), *m_data))
            {
                LOG_ERROR(u8"Editor", u8"the GUI's shaders did not load from '{}'", dataRoot);
                return false;
            }
            gui::RegisterGuiElements();
            gui::RegisterToolkitElements();
            gui::RegisterCodeElements();
            foundation::profiler::gui::RegisterProfilerElements();
            // Shaped by HarfBuzz and reordered by the bidirectional algorithm, installed before a
            // font loads so its atlas takes what shaping reaches.
            fonts::HarfBuzzShaping::Install();
            fonts::BidiAlgorithm::Install();
            m_fonts = MakeUnique<fonts::TrueTypeFontService>(m_allocator, m_allocator);
            m_resources = MakeUnique<editor::EditorResources>(m_allocator, m_allocator, dataRoot);
            if (!themeFolder.IsEmpty())
            {
                (void)CreateDirectories(themeFolder); // a place for the file the user writes
                m_theme = MakeUnique<editor::EditorResources>(m_allocator, m_allocator, themeFolder);
            }

            m_host = MakeUnique<gui::GuiHost>(m_allocator, m_allocator, *host.Graphics(), *host.Shell(), m_fonts.Get(), m_shaders->Set());
            {
                foundation::runtime::IApplicationHost* runtimeHost = &host;
                gui::GuiHost::WindowOpener opener; // a float in an OS window of its own
                opener.Open = [runtimeHost](const foundation::shell::WindowSettings& settings)
                { return runtimeHost->OpenWindow(settings, EditorRenderWindowDesc()); };
                opener.Close = [runtimeHost](foundation::graphics::RenderWindow& window) { runtimeHost->CloseWindow(&window); };
                m_host->SetWindowOpener(Move(opener));
            }
            gui::StyleManager& styles = m_host->Styles();
            styles.SetResourceProvider(m_resources.Get());
            m_host->Context().SetResourceProvider(m_resources.Get());
            EditorWindow* self = this;
            styles.SetFontFaceLoader([self](StringView face, StringView source) { return self->LoadFontFace(face, source); });
            const String theme = Format(u8"{}\n{}\n{}\n{}\n{}", gui::DefaultThemeCSS(), gui::DockingSheetCSS(), gui::ToolkitSheetCSS(),
                                        gui::CodeSheetCSS(), foundation::profiler::gui::ProfilerSheetCSS()); // the satellites' over the widgets'
            styles.SetUserAgentSheet(gui::CSSParser::Parse(theme.AsView(), StringView(u8"theme")));

            m_root = MakeRef<gui::SceneRoot>(m_allocator, m_host->ContextRef());
            m_frame = LoadMarkup(StringView(kFramePath));
            if (!m_frame)
            {
                LOG_ERROR(u8"Editor", u8"the editor's frame ({}) did not load", StringView(kFramePath));
                return false;
            }
            m_root->AddChild(m_frame.Get());
            m_status = MakeUnique<EditorStatusBar>(m_allocator, *m_frame);
            // The toasts stack at the bottom right of the dock, over it, above the status bar.
            m_toasts = MakeRef<gui::ToastHost>(m_allocator);
            gui::Node* dock = FindById(u8"frame-dock");
            (dock != nullptr ? *dock : *m_frame).AddChild(m_toasts.Get());
            SetLook(look);
            // The editor's own files or the user's overrides changed on disk: the sheet applies again.
            m_reload = MakeUnique<gui::Effect>(m_allocator, [self]()
                                               {
                                                   self->m_resources->TrackChanges();
                                                   if (self->m_theme)
                                                       self->m_theme->TrackChanges();
                                                   if (self->m_frame)
                                                       self->ApplySheet();
                                               });
            m_host->AttachWindow(*m_window, m_root);
            return true;
        }

        /// The look: the sheets for the density, the accent as an input, the scheme and motion
        /// through the media, the UI scale through the host.
        void SetLook(const EditorLook& look)
        {
            m_look = look;
            if (!m_host)
                return;
            m_host->SetUiScale(look.uiScale);
            m_host->Context().SetColorScheme(look.dark);
            m_host->Context().SetReducedMotion(look.reduceMotion);
            ApplySheet();
        }
        [[nodiscard]] const EditorLook& Look() const noexcept { return m_look; }

        /// A file's text by its path under the data root (a code language's); false when it is not there.
        [[nodiscard]] bool LoadText(StringView path, String& out) const { return m_resources && m_resources->LoadText(path, out); }
        /// A page's or a panel's markup by its path under the data root (Editor/...): a node, or
        /// null (logged) when it does not load.
        [[nodiscard]] RefPtr<gui::Node> LoadMarkup(StringView path)
        {
            String text;
            if (!m_resources || !m_resources->LoadText(path, text))
            {
                LOG_ERROR(u8"Editor", u8"markup '{}' is not there", path);
                return {};
            }
            const gui::ElementSet elements = gui::ElementSet::All();
            gui::MarkupLoader loader(elements, m_allocator, nullptr, m_fonts.Get());
            loader.SetDataModel(m_host->Context().GetDataRef());
            return loader.LoadFromString(text.AsView());
        }

        /// A toast over the dock, its severity the notice's; the same message again counts on the
        /// one shown. An error stays until it is dismissed, and so does one with an action (Open
        /// Folder on an export), its button running `action`.
        void ShowToast(editor::NoticeKind kind, StringView message, StringView actionText = {}, Function<void()> action = {})
        {
            if (!m_toasts)
                return;
            gui::ToastRequest request = MakeToast(kind, message, actionText);
            if (!action)
            {
                (void)m_toasts->Post(Move(request));
                return;
            }
            (void)m_toasts->Post(Move(request), [action = Move(action)](const gui::ToastAction&) { action(); });
        }
        /// The toast a notice shows: its severity, the message as its title and its key; with an
        /// action's text, a button and no time out.
        [[nodiscard]] static gui::ToastRequest MakeToast(editor::NoticeKind kind, StringView message, StringView actionText = {})
        {
            gui::ToastRequest request;
            request.Severity = kind == editor::NoticeKind::Success  ? gui::ToastSeverity::Success
                               : kind == editor::NoticeKind::Warning ? gui::ToastSeverity::Warning
                               : kind == editor::NoticeKind::Error   ? gui::ToastSeverity::Error
                                                                     : gui::ToastSeverity::Info;
            request.Title = String(message);
            request.Key = String(message);
            if (!actionText.IsEmpty())
            {
                request.AddAction(String(u8"action"), String(actionText), String{});
                request.Duration = 0.0f; // until closed: its button is there to be used
            }
            return request;
        }
        /// The status bar's message.
        void SetStatus(StringView message)
        {
            if (m_status)
                m_status->SetMessage(message);
        }
        /// The status bar: its message, the job running, the agent; null with no interface.
        [[nodiscard]] EditorStatusBar* StatusBar() const noexcept { return m_status.Get(); }

        /// A screen over the whole window in place of the frame (the project manager); null puts
        /// the frame back.
        void ShowScreen(RefPtr<gui::Node> screen)
        {
            if (!m_root || !m_frame)
                return;
            if (m_screen)
                m_screen->RemoveFromParent();
            m_screen = Move(screen);
            m_frame->SetDisplayed(!m_screen);
            if (m_screen)
                m_root->AddChild(m_screen.Get());
        }
        [[nodiscard]] bool ShowsScreen() const noexcept { return m_screen.Get() != nullptr; }

        /// A frame's node by its id (the dock's host, the status bar's items).
        [[nodiscard]] gui::Node* FindById(StringView id) const { return m_frame ? FindIn(*m_frame, id) : nullptr; }

        /// One frame: the editor's files swept for changes (about once a second), then the GUI.
        void Update(f32 deltaTime)
        {
            if (!m_host)
                return;
            m_sinceSweep += deltaTime;
            if (m_sinceSweep >= kSweepSeconds)
            {
                m_sinceSweep = 0.0f;
                (void)m_resources->Poll();
                if (m_theme && m_theme->Poll())
                    LOG_INFO(u8"Editor", u8"the theme overrides applied again ({})", kThemeOverridesPath);
            }
            m_host->Update(deltaTime);
        }
        /// Draws the GUI into `frame`'s window (the main window and any float's).
        void Render(foundation::graphics::FrameContext& frame)
        {
            if (m_host)
                m_host->RenderWindow(frame);
        }

        /// Takes the GUI down (before the device goes).
        void Close()
        {
            m_reload.Reset();
            if (m_host && m_window != nullptr)
                m_host->DetachWindow(*m_window);
            m_screen = {};
            m_toasts = {};
            m_status.Reset();
            m_frame = {};
            m_root = {};
            m_host.Reset();
            m_theme.Reset();
            m_resources.Reset();
            m_fonts.Reset();
            m_shaders.Reset();
            m_data.Reset();
            m_window = nullptr;
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_host.Get() != nullptr; }
        [[nodiscard]] gui::GuiHost* Host() const noexcept { return m_host.Get(); }
        [[nodiscard]] gui::SceneRoot* Root() const noexcept { return m_root.Get(); }
        [[nodiscard]] editor::EditorResources* Resources() const noexcept { return m_resources.Get(); }
        [[nodiscard]] fonts::TrueTypeFontService* Fonts() const noexcept { return m_fonts.Get(); }

    private:
        static constexpr f32 kSweepSeconds = 1.0f;

        void ApplySheet()
        {
            m_host->Styles().SetStyleSheet(gui::CSSParser::Parse(SheetFor(*m_resources, m_look, m_theme.Get()).AsView(), StringView(kSheetPath)));
        }

    public:
        /// The sheet `look` makes from the editor's files: the editor's sheet, the density's after
        /// it (its sizes win), then a `:root` block of the inputs (the accent), then the user's
        /// overrides from `theme` (null: none), last so what they set wins.
        [[nodiscard]] static String SheetFor(gui::IResourceProvider& resources, const EditorLook& look, gui::IResourceProvider* theme)
        {
            String sheet;
            String part;
            if (resources.LoadText(StringView(kSheetPath), part))
                sheet += part.AsView();
            if (resources.LoadText(Format(u8"Editor/density-{}.css", look.density).AsView(), part))
            {
                sheet += u8"\n";
                sheet += part.AsView();
            }
            sheet += Format(u8"\n:root {{ --accent-input: {}; }}\n", look.accent).AsView();
            if (theme != nullptr && theme->LoadText(StringView(kThemeOverridesPath), part))
            {
                sheet += part.AsView();
                sheet += u8"\n";
            }
            return sheet;
        }

    private:

        // A face a sheet's @font-face names, from its file under the data root.
        bool LoadFontFace(StringView face, StringView source)
        {
            fonts::FontLoadOptions options = fonts::FontLoadOptions::ExtendedLatin();
            options.pixelHeight = 16.0f;
            options.lastCodepoint = 0x17F;
            if (m_fonts->LoadFont(face, PathJoin(m_dataRoot.AsView(), source).AsView(), options) == fonts::FontLoadResult::Success)
                return true;
            LOG_ERROR(u8"Editor", u8"the @font-face {} ({}) did not load", face, source);
            return false;
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

        IAllocator& m_allocator;
        String m_dataRoot;
        EditorLook m_look;
        foundation::graphics::RenderWindow* m_window = nullptr; // borrowed (the host's)
        UniquePtr<foundation::vfs::NativeFileSystem> m_data;   // the shaders' mount
        UniquePtr<gui::VgShaderLibrary> m_shaders;
        UniquePtr<fonts::TrueTypeFontService> m_fonts;
        UniquePtr<editor::EditorResources> m_resources;
        UniquePtr<editor::EditorResources> m_theme; // the user's overrides' folder; null: none
        UniquePtr<gui::GuiHost> m_host;
        RefPtr<gui::SceneRoot> m_root;
        RefPtr<gui::Node> m_frame;
        RefPtr<gui::ToastHost> m_toasts;
        RefPtr<gui::Node> m_screen; // over the window in place of the frame, while shown
        UniquePtr<EditorStatusBar> m_status;
        UniquePtr<gui::Effect> m_reload; // the sheet again when the editor's files or the overrides change
        f32 m_sinceSweep = 0.0f;
    };
}

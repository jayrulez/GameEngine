// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :editor_window partition.
//
// EditorWindow: the editor's GUI on its main window (Documentation/Specs/editor-gui.md, 5.1,
// layer 3). It owns the GUI host (the window's input, frames, renderer and picture baker), the
// fonts the editor's sheet names, the editor's own files (EditorResources) and the frame read
// from Data/Editor/Shell.gml, with the toast host over it. Its look is the editor's sheet
// (Editor/editor.css over its tokens), the chosen density's sheet after it and a `:root` block of
// the user's inputs (the accent), the scheme and less motion through the GUI's media; a change to
// those files on disk (the editor's development loop) applies at once.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:editor_window;

import foundation.core;
import foundation.shell;
import foundation.graphics;
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
import editor.core;

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

    class EditorWindow
    {
    public:
        /// The frame's markup, its sheet and the folder the editor's files are in, under the data root.
        static constexpr const char8_t* kFramePath = u8"Editor/Shell.gml";
        static constexpr const char8_t* kSheetPath = u8"Editor/editor.css";

        explicit EditorWindow(IAllocator& allocator) : m_allocator(allocator) {}
        EditorWindow(const EditorWindow&) = delete;
        EditorWindow& operator=(const EditorWindow&) = delete;
        ~EditorWindow() { Close(); }

        /// Opens the GUI on the host's main window over the data root: the shaders, the fonts, the
        /// frame and the sheet. False (logged) when the window, the device or the shaders are
        /// missing; the editor then runs with no interface.
        bool Open(foundation::runtime::IApplicationHost& host, StringView dataRoot, const EditorLook& look)
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
            // Shaped by HarfBuzz and reordered by the bidirectional algorithm, installed before a
            // font loads so its atlas takes what shaping reaches.
            fonts::HarfBuzzShaping::Install();
            fonts::BidiAlgorithm::Install();
            m_fonts = MakeUnique<fonts::TrueTypeFontService>(m_allocator, m_allocator);
            m_resources = MakeUnique<editor::EditorResources>(m_allocator, m_allocator, dataRoot);

            m_host = MakeUnique<gui::GuiHost>(m_allocator, m_allocator, *host.Graphics(), *host.Shell(), m_fonts.Get(), m_shaders->Set());
            {
                foundation::runtime::IApplicationHost* runtimeHost = &host;
                gui::GuiHost::WindowOpener opener; // a float in an OS window of its own
                opener.Open = [runtimeHost](const foundation::shell::WindowSettings& settings)
                { return runtimeHost->OpenWindow(settings, foundation::graphics::RenderWindowDesc{}); };
                opener.Close = [runtimeHost](foundation::graphics::RenderWindow& window) { runtimeHost->CloseWindow(&window); };
                m_host->SetWindowOpener(Move(opener));
            }
            gui::StyleManager& styles = m_host->Styles();
            styles.SetResourceProvider(m_resources.Get());
            m_host->Context().SetResourceProvider(m_resources.Get());
            EditorWindow* self = this;
            styles.SetFontFaceLoader([self](StringView face, StringView source) { return self->LoadFontFace(face, source); });
            const String theme = Format(u8"{}\n{}\n{}\n{}", gui::DefaultThemeCSS(), gui::DockingSheetCSS(), gui::ToolkitSheetCSS(),
                                        gui::CodeSheetCSS()); // the satellites' over the widgets'
            styles.SetUserAgentSheet(gui::CSSParser::Parse(theme.AsView(), StringView(u8"theme")));

            m_root = MakeRef<gui::SceneRoot>(m_allocator, m_host->ContextRef());
            m_frame = LoadMarkup(StringView(kFramePath));
            if (!m_frame)
            {
                LOG_ERROR(u8"Editor", u8"the editor's frame ({}) did not load", StringView(kFramePath));
                return false;
            }
            m_root->AddChild(m_frame.Get());
            m_toasts = MakeRef<gui::ToastHost>(m_allocator);
            m_frame->AddChild(m_toasts.Get());
            SetLook(look);
            // The editor's own files changed on disk: its sheet applies again.
            m_reload = MakeUnique<gui::Effect>(m_allocator, [self]()
                                               {
                                                   self->m_resources->TrackChanges();
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

        /// A toast over the frame, its severity the notice's.
        void ShowToast(editor::NoticeKind kind, StringView message)
        {
            if (!m_toasts)
                return;
            gui::ToastRequest request;
            request.Severity = kind == editor::NoticeKind::Success  ? gui::ToastSeverity::Success
                               : kind == editor::NoticeKind::Warning ? gui::ToastSeverity::Warning
                               : kind == editor::NoticeKind::Error   ? gui::ToastSeverity::Error
                                                                     : gui::ToastSeverity::Info;
            request.Title = String(message);
            (void)m_toasts->Post(Move(request));
        }
        /// The status bar's message (the frame's `status-message` label).
        void SetStatus(StringView message)
        {
            if (auto* label = Cast<gui::Label>(FindById(u8"status-message")))
                label->SetText(message);
        }

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
            m_toasts = {};
            m_frame = {};
            m_root = {};
            m_host.Reset();
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

        void ApplySheet() { m_host->Styles().SetStyleSheet(gui::CSSParser::Parse(SheetFor(*m_resources, m_look).AsView(), StringView(kSheetPath))); }

    public:
        /// The sheet `look` makes from the editor's files: the editor's sheet, the density's after
        /// it (its sizes win), then a `:root` block of the inputs (the accent).
        [[nodiscard]] static String SheetFor(gui::IResourceProvider& resources, const EditorLook& look)
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
        UniquePtr<gui::GuiHost> m_host;
        RefPtr<gui::SceneRoot> m_root;
        RefPtr<gui::Node> m_frame;
        RefPtr<gui::ToastHost> m_toasts;
        UniquePtr<gui::Effect> m_reload; // the sheet again when the editor's files change
        f32 m_sinceSweep = 0.0f;
    };
}

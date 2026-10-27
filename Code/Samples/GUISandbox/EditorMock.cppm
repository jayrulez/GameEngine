// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUISandbox - the editor mock (X0 of Specs/editor-gui.md): the editor's frame and scene page,
// static, in the editor's sheet (Data/Editor/editor.css over its tokens), for judging the design
// before any editor code moves. `GUISandbox --editor-mock` shows it instead of the sandbox. The
// markup is Data/Editor/Mock's; what is here is what markup cannot say: the dock, the commands
// with their keys, the inspector's grid, and the Look menu, which stands in for Preferences'
// appearance inputs (the scheme, the density, the accent, less motion) the way the editor will
// set them: a `:root` block of inputs after the editor's sheet.

module;
#include "Core/Prelude.h"
#include <cstdio> // fprintf

export module gui.sandbox.editor_mock;

import foundation.core;
import foundation.gui;
import foundation.gui.docking;
import foundation.gui.toolkit;
import gui.sandbox.mock_types;

using namespace foundation::core;
namespace core = foundation::core;
namespace gui = foundation::gui;

export namespace sandbox
{
    class EditorMock
    {
    public:
        /// What the mock asks of its host: markup and text by their path under the data folder.
        struct Host
        {
            Function<RefPtr<gui::Node>(StringView path)> Load;
            Function<String(StringView path)> Read;
        };

        EditorMock(IAllocator& allocator, gui::GUIContext& context, gui::SceneRoot& root, gui::StyleManager& styles, Host host)
            : m_allocator(allocator), m_context(context), m_root(root), m_styles(styles), m_host(Move(host)), m_shown(allocator),
              m_sources(allocator)
        {
        }

        /// The frame under the root, its panels in the dock, the editor's sheet applied. False when
        /// the frame's markup does not load.
        bool Build()
        {
            m_frame = m_host.Load(u8"Editor/Mock/frame.gml");
            if (!m_frame)
                return false;
            m_root.AddChild(m_frame.Get());
            m_toasts = MakeRef<gui::ToastHost>(m_allocator);
            m_frame->AddChild(m_toasts.Get());
            DefineCommands();
            BuildDock();
            ApplyLook();
            return true;
        }

        /// The two scene pages side by side, or tabbed in one stack (the default).
        void SetSideBySide(bool side)
        {
            if (m_dock)
                Said(m_dock->Adopt(DefaultLayout(side)));
            m_context.DefineCommand(u8"mock.side-by-side").SetChecked(side);
            m_sideBySide = side;
        }

        /// A page of the mock in front by its id (--mock-page: "input-outline", "input-split").
        void ShowPage(StringView id)
        {
            if (m_dock)
            {
                const String page(id);
                Said(m_dock->Edit([&page](gui::DockLayout& layout) { return layout.Activate(page.AsView()); }));
            }
        }

        /// The kit page in front (--mock-kit): every shared piece in its states.
        void ShowKit()
        {
            if (m_dock)
                Said(m_dock->Edit([](gui::DockLayout& layout) { return layout.Activate(u8"kit"); }));
        }

        /// The kit page scrolled down this far (--scroll, for a screenshot further down).
        void ScrollKit(f32 pixels)
        {
            if (m_kit)
                if (gui::Node* scroll = FindById(*m_kit, u8"kit-scroll"))
                    scroll->SetScrollOffset(Float2{0.0f, pixels});
        }

        /// The scheme from the start (--light).
        void SetDark(bool dark)
        {
            m_dark = dark;
            ApplyLook();
        }

    private:
        struct Accent
        {
            const char8_t* Command;
            const char8_t* Label;
            const char8_t* Fill;
        };
        // The candidates for the editor's new accent (ruling 7), each a fill white text reads on.
        static constexpr Accent kAccents[] = {
            {u8"look.azure", u8"Azure", u8"#2f6fdf"},
            {u8"look.violet", u8"Violet", u8"#6d5bd8"},
            {u8"look.teal", u8"Teal", u8"#0f8a7e"},
            {u8"look.magenta", u8"Magenta", u8"#b03d93"},
        };
        struct Density
        {
            const char8_t* Command;
            const char8_t* Label;
            const char8_t* Sheet; // its sizes in whole pixels, added after the editor's sheet
        };
        static constexpr Density kDensities[] = {
            {u8"look.compact", u8"Compact", u8"Editor/density-compact.css"},
            {u8"look.default", u8"Default", u8"Editor/density-default.css"},
            {u8"look.comfortable", u8"Comfortable", u8"Editor/density-comfortable.css"},
        };

        void Define(StringView id, StringView label, StringView keys = {}, StringView icon = {})
        {
            gui::Command& command = m_context.DefineCommand(id);
            command.SetLabel(label);
            if (icon.Size() != 0)
                command.SetIcon(icon);
            if (keys.Size() != 0)
                (void)m_context.GetKeymap().Bind(keys, id);
        }

        void DefineCommands()
        {
            // What the mock's menus and bars name. They do nothing but say so: the mock is a picture.
            Define(u8"mock.new-scene", u8"New Scene", u8"ctrl-n");
            Define(u8"mock.open", u8"Open...", u8"ctrl-o");
            Define(u8"mock.save", u8"Save", u8"ctrl-s", u8"Editor/Icons/save.svg");
            Define(u8"mock.save-all", u8"Save All", u8"ctrl-shift-s");
            Define(u8"mock.exit", u8"Exit");
            Define(u8"mock.undo", u8"Undo", u8"ctrl-z", u8"Editor/Icons/undo.svg");
            Define(u8"mock.redo", u8"Redo", u8"ctrl-shift-z", u8"Editor/Icons/redo.svg");
            Define(u8"mock.preferences", u8"Preferences...", u8"ctrl-comma", u8"Editor/Icons/settings.svg");
            Define(u8"mock.view-assets", u8"Assets");
            Define(u8"mock.view-console", u8"Console");
            Define(u8"mock.view-profiler", u8"Profiler");
            Define(u8"mock.palette", u8"Command Palette...", u8"ctrl-shift-p");
            Define(u8"mock.side-by-side", u8"Pages Side by Side");
            Define(u8"mock.play", u8"Play", u8"f5", u8"Editor/Icons/play.svg");
            Define(u8"mock.pause", u8"Pause", u8"f6", u8"Editor/Icons/pause.svg");
            Define(u8"mock.stop", u8"Stop", u8"shift-f5", u8"Editor/Icons/stop.svg");
            Define(u8"mock.frame", u8"Frame Selection", u8"f", u8"Editor/Icons/frame-selection.svg");
            Define(u8"mock.grid", u8"Grid", {}, u8"Editor/Icons/grid.svg");
            // The bar's editing controls (layer 5, part 1), each with its key.
            Define(u8"mock.tool-select", u8"Select", u8"q", u8"Editor/Icons/select.svg");
            Define(u8"mock.tool-move", u8"Move", u8"w", u8"Editor/Icons/move.svg");
            Define(u8"mock.tool-rotate", u8"Rotate", u8"e", u8"Editor/Icons/rotate.svg");
            Define(u8"mock.tool-scale", u8"Scale", u8"r", u8"Editor/Icons/scale.svg");
            Define(u8"mock.snap", u8"Snap Every Drag (Ctrl for one)", {}, u8"Editor/Icons/magnet.svg");
            Define(u8"mock.space", u8"World Space", u8"x", u8"Editor/Icons/world.svg");
            Define(u8"mock.vertex-snap", u8"Vertex Snap (V for one drag)", {}, u8"Editor/Icons/target.svg");
            Define(u8"mock.terrain-sculpt", u8"Sculpt", {}, u8"Editor/Icons/Types/TerrainComponent.svg");
            Define(u8"mock.terrain-paint", u8"Paint Layers", {}, u8"Editor/Icons/texture.svg");
            Define(u8"mock.terrain-holes", u8"Cut Holes", {}, u8"Editor/Icons/minus.svg");
            Define(u8"mock.vegetation-paint", u8"Paint Vegetation", {}, u8"Editor/Icons/Types/TerrainVegetationComponent.svg");
            Define(u8"mock.vegetation-props", u8"Paint Props", {}, u8"Editor/Icons/Types/VegetationMaskAsset.svg");
            Define(u8"mock.spline", u8"Edit Spline", {}, u8"Editor/Icons/Types/SplineComponent.svg");
            Define(u8"mock.textures", u8"Render Textures...", {}, u8"Editor/Icons/texture.svg");
            m_context.DefineCommand(u8"mock.terrain-sculpt").SetChecked(true);
            gui::Command& props = m_context.DefineCommand(u8"mock.vegetation-props");
            props.SetEnabled(false);
            props.SetDisabledReason(u8"No Terrain Vegetation component has a prop layer");
            gui::Command& spline = m_context.DefineCommand(u8"mock.spline");
            spline.SetEnabled(false);
            spline.SetDisabledReason(u8"Select an entity with a Spline component");
            Define(u8"mock.about", u8"About AssiduousEngine");
            Define(u8"mock.toast", u8"Show a Toast");
            Define(u8"mock.dialog", u8"Show a Dialog");
            Define(u8"mock.asset-picker", u8"Show the Asset Picker");
            Define(u8"mock.entity-picker", u8"Show the Entity Picker");
            m_context.DefineCommand(u8"mock.grid").SetChecked(true);
            m_context.DefineCommand(u8"mock.space").SetChecked(true);
            gui::Command& pause = m_context.DefineCommand(u8"mock.pause");
            pause.SetEnabled(false);
            pause.SetDisabledReason(u8"Nothing is playing");
            gui::Command& stop = m_context.DefineCommand(u8"mock.stop");
            stop.SetEnabled(false);
            stop.SetDisabledReason(u8"Nothing is playing");

            // The Look menu: Preferences' appearance inputs, here to compare.
            EditorMock* self = this;
            // What moves (3.3), to judge with less motion on and off: a toast of each severity in
            // turn, and a dialog.
            (void)m_root.OnCommand(u8"mock.toast", [self](gui::CommandEvent&) { self->ShowToast(); });
            (void)m_root.OnCommand(u8"mock.dialog", [self](gui::CommandEvent&) { self->ShowDialog(); });
            (void)m_root.OnCommand(u8"mock.textures", [self](gui::CommandEvent&) { self->ShowPicker(self->m_textures, u8"Editor/Mock/textures.gml"); });
            (void)m_root.OnCommand(u8"mock.asset-picker", [self](gui::CommandEvent&) { self->ShowPicker(self->m_assetPicker, u8"Editor/Mock/asset-picker.gml"); });
            (void)m_root.OnCommand(u8"mock.entity-picker", [self](gui::CommandEvent&) { self->ShowPicker(self->m_entityPicker, u8"Editor/Mock/entity-picker.gml"); });
            (void)m_root.OnCommand(u8"mock.side-by-side", [self](gui::CommandEvent&) { self->SetSideBySide(!self->m_sideBySide); });
            Define(u8"look.dark", u8"Dark");
            Define(u8"look.light", u8"Light");
            (void)m_root.OnCommand(u8"look.dark", [self](gui::CommandEvent&) { self->m_dark = true; self->ApplyLook(); });
            (void)m_root.OnCommand(u8"look.light", [self](gui::CommandEvent&) { self->m_dark = false; self->ApplyLook(); });
            for (usize i = 0; i < sizeof(kDensities) / sizeof(kDensities[0]); ++i)
            {
                Define(kDensities[i].Command, kDensities[i].Label);
                (void)m_root.OnCommand(kDensities[i].Command, [self, i](gui::CommandEvent&) { self->m_density = i; self->ApplyLook(); });
            }
            for (usize i = 0; i < sizeof(kAccents) / sizeof(kAccents[0]); ++i)
            {
                Define(kAccents[i].Command, kAccents[i].Label);
                (void)m_root.OnCommand(kAccents[i].Command, [self, i](gui::CommandEvent&) { self->m_accent = i; self->ApplyLook(); });
            }
            Define(u8"look.motion", u8"Reduce Motion");
            (void)m_root.OnCommand(u8"look.motion", [self](gui::CommandEvent&) { self->m_reduceMotion = !self->m_reduceMotion; self->ApplyLook(); });
        }

        void ShowToast()
        {
            struct Sample
            {
                gui::ToastSeverity Severity;
                const char8_t* Title;
                const char8_t* Message;
                const char8_t* Action;
            };
            static constexpr Sample kSamples[] = {
                {gui::ToastSeverity::Success, u8"Exported Sky Hopper", u8"Linux, 48 MB, in 12 s", u8"Open Folder"},
                {gui::ToastSeverity::Warning, u8"3 textures have no mipmaps", u8"They were generated for this cook.", nullptr},
                {gui::ToastSeverity::Error, u8"Player.as failed to compile", u8"'speed' is not a member of Player", u8"Show in Console"},
                {gui::ToastSeverity::Info, u8"Agent connected", u8"On 127.0.0.1:7405", nullptr},
            };
            const Sample& sample = kSamples[m_toastsShown++ % (sizeof(kSamples) / sizeof(kSamples[0]))];
            gui::ToastRequest request;
            request.Severity = sample.Severity;
            request.Title = String(sample.Title);
            request.Message = String(sample.Message);
            if (sample.Action != nullptr)
                request.AddAction(String(u8"action"), String(sample.Action), String{});
            (void)m_toasts->Post(Move(request));
        }

        // A picker as the editor's opens: modal, under the window's top (the render textures
        // picker of ruling 21; the asset picker with its groups beside it).
        void ShowPicker(RefPtr<gui::Node>& picker, StringView path)
        {
            if (!picker)
            {
                picker = m_host.Load(path);
                // The asset picker's chosen group: Props (Favourites, All, Content, Materials, Props).
                if (picker)
                    if (auto* groups = Cast<gui::TreeView>(FindById(*picker, u8"asset-picker-groups")))
                        groups->SetSelectedItem(4);
                // The entity picker's current value: Camera (Level 1, Sun, Sky, Player, Camera).
                if (picker)
                    if (auto* tree = Cast<gui::TreeView>(FindById(*picker, u8"entity-picker-tree")))
                        tree->SetSelectedItem(4);
                // Its two views, the buttons switching them (the editor keeps the last one).
                if (picker)
                {
                    gui::Node* rows = FindById(*picker, u8"asset-picker-rows");
                    gui::Node* tiles = FindById(*picker, u8"asset-picker-tiles-view");
                    auto* tilesButton = Cast<gui::Button>(FindById(*picker, u8"asset-picker-tiles"));
                    auto* listButton = Cast<gui::Button>(FindById(*picker, u8"asset-picker-list"));
                    if (rows != nullptr && tiles != nullptr && tilesButton != nullptr && listButton != nullptr)
                    {
                        const auto show = [rows, tiles, tilesButton, listButton](bool asTiles)
                        {
                            rows->SetDisplayed(!asTiles);
                            tiles->SetDisplayed(asTiles);
                            if (asTiles)
                            {
                                tilesButton->AddClass(u8"checked");
                                listButton->RemoveClass(u8"checked");
                            }
                            else
                            {
                                listButton->AddClass(u8"checked");
                                tilesButton->RemoveClass(u8"checked");
                            }
                        };
                        show(false);
                        tilesButton->SetOnClick([show]() { show(true); });
                        listButton->SetOnClick([show]() { show(false); });
                    }
                }
            }
            gui::EventDispatcher* dispatcher = m_root.GetEventDispatcher();
            if (!picker || dispatcher == nullptr || dispatcher->IsPopupOpen(*picker))
                return;
            gui::PopupOptions options;
            options.Kind = gui::PopupKind::Auto;
            options.Modal = true;
            options.TakesFocus = true;
            options.FitContent = true;
            options.HasAnchorRect = true;
            options.AnchorRect = gui::Rect{0.0f, 0.0f, m_root.GetSize().x, m_root.GetSize().y * 0.2f};
            options.Placement.Side = gui::PopupSide::Below;
            options.Placement.Align = gui::PopupAlign::Center;
            dispatcher->OpenPopup(m_root, *picker, Move(options));
        }

        void ShowDialog()
        {
            if (!m_dialog)
                m_dialog = MakeRef<gui::MessageBox>(m_allocator);
            m_dialog->Configure(u8"Delete 3 assets?", u8"These go to the project's trash. 2 scenes still use Wood.",
                                gui::MessageBox::Buttons::Ok);
            // A confirmation names its verb (catalog 2.4): Cancel, and Delete styled as destroying.
            m_dialog->ClearButtons();
            (void)m_dialog->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
            (void)m_dialog->AddButton(u8"Delete", u8"delete", gui::DialogRole::Destructive);
            m_dialog->OpenModal(m_root);
        }

        // The editor's sheet, the density's after it, then the inputs; the scheme and motion
        // through the media.
        void ApplyLook()
        {
            const String inputs = Format(u8"\n:root {{ --accent-input: {}; }}\n", kAccents[m_accent].Fill);
            String sheet = m_host.Read(u8"Editor/editor.css");
            sheet += StringView(u8"\n");
            sheet += m_host.Read(kDensities[m_density].Sheet).AsView();
            sheet += StringView(u8"\n");
            sheet += m_host.Read(u8"Editor/Mock/kit.css").AsView(); // the kit page's own layout
            sheet += StringView(u8"\n");
            sheet += m_host.Read(u8"Editor/Mock/profiler.css").AsView(); // the profiler page's, and its bars
            sheet += inputs.AsView();
            m_styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView(), StringView(u8"Editor/editor.css")));
            m_context.SetColorScheme(m_dark);
            m_context.SetReducedMotion(m_reduceMotion);
            m_context.DefineCommand(u8"look.dark").SetChecked(m_dark);
            m_context.DefineCommand(u8"look.light").SetChecked(!m_dark);
            for (usize i = 0; i < sizeof(kDensities) / sizeof(kDensities[0]); ++i)
                m_context.DefineCommand(kDensities[i].Command).SetChecked(i == m_density);
            for (usize i = 0; i < sizeof(kAccents) / sizeof(kAccents[0]); ++i)
                m_context.DefineCommand(kAccents[i].Command).SetChecked(i == m_accent);
            m_context.DefineCommand(u8"look.motion").SetChecked(m_reduceMotion);
        }

        // The dock: pages in the middle (two scene pages, each with its own hierarchy and
        // inspector, so they can stand side by side, and the kit page), the shared panels below:
        // assets and the console.
        void BuildDock()
        {
            gui::Node* host = FindById(*m_frame, u8"frame-dock");
            if (host == nullptr)
                return;
            gui::DefineDockingCommands(m_context);
            m_dock = MakeUnique<gui::DockController>(m_allocator, m_allocator);
            Register(u8"scene", u8"Level 1", u8"Editor/Mock/scene.gml");
            Register(u8"scene-2", u8"Level 2", u8"Editor/Mock/scene.gml");
            Register(u8"kit", u8"Kit", u8"Editor/Mock/kit.gml");
            // The input map page's two designs, to compare (the user, 2026-10-26).
            Register(u8"input-outline", u8"Input A: Outline", u8"Editor/Mock/input-outline.gml");
            Register(u8"input-split", u8"Input B: Tree and Grid", u8"Editor/Mock/input-split.gml");
            Register(u8"input-split-set", u8"Input B: Set Chosen", u8"Editor/Mock/input-split-set.gml");
            Register(u8"input-split-set-open", u8"Input B: Set, Actions Open", u8"Editor/Mock/input-split-set-open.gml");
            // The profiler page (X7), a tab in the page area (the user, 2026-10-27).
            Register(u8"profiler", u8"Profiler", u8"Editor/Mock/profiler.gml");
            Register(u8"assets", u8"Assets", u8"Editor/Mock/assets.gml");
            Register(u8"console", u8"Console", u8"Editor/Mock/console.gml");
            Said(m_dock->Adopt(DefaultLayout(false)));
            m_dockArea = MakeRef<gui::DockArea>(m_allocator);
            m_dockArea->EditLayoutStyle([](gui::LayoutStyle& s) { s.Grow = 1.0f; });
            host->AddChild(m_dockArea.Get());
            m_dockArea->Show(*m_dock);
        }

        // A layout step the dock refused, said (the default arrangement is the mock's own: a refusal is a bug).
        static void Said(const gui::DockOutcome& outcome)
        {
            if (!outcome.Ok())
                std::fprintf(stderr, "EditorMock: the dock refused: %s\n", reinterpret_cast<const char*>(String(outcome.Refused.AsView()).CStr()));
        }

        static gui::DockLayout DefaultLayout(bool sideBySide)
        {
            gui::DockLayout layout;
            const gui::DockNodeId central = layout.CentralStack();
            Said(layout.InsertTab(u8"scene", central));
            if (sideBySide)
                Said(layout.InsertBeside(u8"scene-2", central, gui::DockSide::End, 0.5f));
            else
                Said(layout.InsertTab(u8"scene-2", central));
            Said(layout.InsertTab(u8"kit", central));
            Said(layout.InsertTab(u8"input-outline", central));
            Said(layout.InsertTab(u8"input-split", central));
            Said(layout.InsertTab(u8"input-split-set", central));
            Said(layout.InsertTab(u8"input-split-set-open", central));
            Said(layout.InsertTab(u8"profiler", central));
            Said(layout.Activate(u8"profiler"));
            Said(layout.InsertBeside(u8"assets", central, gui::DockSide::Bottom, 0.28f));
            Said(layout.InsertTab(u8"console", layout.StackOf(u8"assets")));
            Said(layout.Activate(u8"assets"));
            return layout;
        }

        void Register(StringView id, StringView title, StringView markup)
        {
            gui::DockPanelDescription description;
            description.Id = String(id);
            description.Title = String(title);
            description.Close = gui::DockClosePolicy::Hide;
            description.Closable = true;
            description.Default.Central = id.StartsWith(StringView(u8"scene")) || id == StringView(u8"kit") || id == StringView(u8"profiler");
            EditorMock* self = this;
            const String path(markup);
            const String panel(id);
            description.Factory = [self, path, panel]()
            {
                RefPtr<gui::Node> content = self->m_host.Load(path.AsView());
                if (content && panel.AsView() == StringView(u8"kit"))
                    self->m_kit = content;
                if (content && panel.AsView().StartsWith(StringView(u8"scene")))
                    self->FillScenePage(*content, panel.AsView() == StringView(u8"scene-2"));
                if (content && panel.AsView() == StringView(u8"assets"))
                    if (auto* crumbs = Cast<gui::BreadcrumbBar>(FindById(*content, u8"assets-crumbs")))
                        crumbs->SetPath(u8"Content/Models/Props");
                return content;
            };
            (void)m_dock->Register(Move(description));
        }

        // A scene page's inspector: on the first page an entity is selected, on the second the
        // scene itself (its top row in the hierarchy), whose settings the same inspector shows.
        void FillScenePage(gui::Node& content, bool sceneSelected)
        {
            auto* grid = Cast<gui::PropertyGrid>(FindById(content, u8"inspector-grid"));
            if (grid == nullptr)
                return;
            RegisterMockTypes();
            RefPtr<Object> shown;
            if (sceneSelected)
            {
                shown = MakeRef<MockScene>(m_allocator);
                if (auto* tree = Cast<gui::TreeView>(FindById(content, u8"hierarchy-tree")))
                    tree->SetSelectedItem(0);
                if (auto* icon = Cast<gui::Image>(FindById(content, u8"inspector-icon")))
                    icon->SetSource(String(u8"Editor/Icons/layers.svg"));
                if (auto* name = Cast<gui::TextField>(FindById(content, u8"inspector-name")))
                    name->SetText(u8"Level 2");
                if (auto* kind = Cast<gui::Label>(FindById(content, u8"inspector-kind")))
                    kind->SetText(u8"Scene");
                // A scene has settings, not components: nothing to add.
                if (auto* add = Cast<gui::Button>(FindById(content, u8"add-component")))
                    add->SetDisplayed(false);
                // A render texture shown: the shading chip names it and stands out, so a debug
                // view left on is seen.
                if (auto* shading = Cast<gui::Button>(FindById(content, u8"chip-shading")))
                {
                    shading->SetText(u8"ssgi.filtered");
                    shading->AddClass(u8"on");
                }
                if (auto* projection = Cast<gui::Button>(FindById(content, u8"chip-projection")))
                {
                    projection->SetText(u8"Top");
                    projection->AddClass(u8"on");
                }
            }
            else
            {
                shown = MakeRef<MockEntity>(m_allocator);
                // A tool's panel shows on the second page only, so the first keeps a clear view.
                if (gui::Node* tool = FindById(content, u8"tool-panel"))
                    tool->SetDisplayed(false);
            }
            UniquePtr<gui::ReflectedPropertySource> source = MakeUnique<gui::ReflectedPropertySource>(m_allocator, m_allocator);
            const Instance objects[] = {Instance(shown.Get(), shown->GetType())};
            source->SetObjects(Span<const Instance>(objects, 1));
            grid->SetSource(source.Get());
            // Each section open, as the inspector shows a selection's components.
            const char8_t* const entitySections[] = {u8"transform", u8"meshRenderer", u8"rigidBody"};
            const char8_t* const sceneSections[] = {u8"environment", u8"rendering", u8"physics", u8"audio"};
            if (sceneSelected)
                for (const char8_t* section : sceneSections)
                    grid->SetOpen(section, true);
            else
                for (const char8_t* section : entitySections)
                    grid->SetOpen(section, true);
            m_shown.PushBack(Move(shown));
            m_sources.PushBack(Move(source));
        }

        static gui::Node* FindById(gui::Node& node, StringView id)
        {
            if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindById(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        IAllocator& m_allocator;
        gui::GUIContext& m_context;
        gui::SceneRoot& m_root;
        gui::StyleManager& m_styles;
        Host m_host;
        RefPtr<gui::Node> m_frame;
        UniquePtr<gui::DockController> m_dock;
        RefPtr<gui::DockArea> m_dockArea;
        RefPtr<gui::Node> m_kit; // the kit page's content, once the dock has made it
        RefPtr<gui::ToastHost> m_toasts;
        RefPtr<gui::MessageBox> m_dialog;
        RefPtr<gui::Node> m_textures;    // the render textures picker, once opened
        RefPtr<gui::Node> m_assetPicker; // the asset picker, once opened
        RefPtr<gui::Node> m_entityPicker; // the entity picker, once opened
        usize m_toastsShown = 0;
        Array<RefPtr<Object>> m_shown; // what each page's inspector shows
        Array<UniquePtr<gui::ReflectedPropertySource>> m_sources;
        bool m_dark = true;
        usize m_density = 0; // compact, the editor's default (ruling)
        usize m_accent = 0;
        bool m_reduceMotion = false;
        bool m_sideBySide = false;
    };
}

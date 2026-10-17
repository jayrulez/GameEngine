// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI Sandbox - the experimental GUI's demo (Documentation/Specs/vg-and-gui-lab.md), built from the
// lab itself. A shell (Data/Assets/ui/sandbox/shell.xml: a header, a sidebar of pages, the page on
// show) whose sidebar buttons run page.<id> commands; each page is pages/<id>.xml with an optional
// pages/<id>.css and pages/<id>.as, so a new page is a button and its files. This host only sizes
// the shell, fills the few hosts that need native models or widgets (the data views, the menu bar,
// the context menu, the floating window, the message box, an image, anchors), and
// switches the theme. The game menu (Data/Assets/ui/showcase) is a second root of the same GUI:
// it shares the data, and its Leave brings the player back here.
//
// Drawn through VG with everything it has (analytic boxes, layers and blurs, the MSAA and stencil
// pass), only on frames that need it. Input reaches the root on show through GuiInputBridge.

#include <new>
#include <cstdio> // printf, fprintf
#include <cstdlib> // atof

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system; // ShaderSystemHost
import samples.framework;
import foundation.shell;
import foundation.image;
import foundation.image.io; // the resource provider's pictures
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.distancefield.baker; // the Lilita distance-field face
import foundation.vg;
import foundation.vg.renderer;
import experimental.gui;
import experimental.gui.shell;
import foundation.script; // the pages' and the game menu's scripts
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
namespace shell = foundation::shell;
namespace image = foundation::image;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;
namespace gui = experimental::gui;
namespace script = foundation::script;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] IAllocator& AppRoot() noexcept { return DefaultAllocator(); }

    constexpr StringView kRoboto = u8"Assets/fonts/roboto/Roboto-Regular.ttf";
    constexpr StringView kLilita = u8"Assets/fonts/lilita-one/LilitaOne-Regular.ttf";
    constexpr StringView kSandboxDir = u8"Assets/ui/sandbox/";
    constexpr StringView kMenuDir = u8"Assets/ui/showcase/";
    constexpr StringView kPageCommand = u8"page.";

    constexpr f32 kHeaderHeight = 60.0f;
    constexpr f32 kSidebarWidth = 210.0f;

    // The first widget under `node` (itself included) whose id is `id`.
    [[nodiscard]] gui::Node* FindById(gui::Node& node, StringView id)
    {
        if (const gui::UIWidget* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return &node;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (gui::Node* found = FindById(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }

    // Every node under `node` (itself included) whose bound command starts with `prefix`.
    void CollectCommands(gui::Node& node, StringView prefix, Array<gui::Node*>& out)
    {
        const StringView command = node.GetCommand();
        if (command.Size() > prefix.Size() && command.SubStr(0, prefix.Size()) == prefix)
            out.PushBack(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            CollectCommands(*node.GetChildAt(i), prefix, out);
    }

    // The GUI's pictures (an Image's src, a sheet's background-image: url()), loaded from the data
    // folder once each and kept for the sandbox's life.
    class SandboxResources final : public gui::IResourceProvider
    {
    public:
        explicit SandboxResources(Function<String(StringView)> resolve) : m_resolve(Move(resolve)) {}
        const image::ImageData* LoadImage(StringView path) override
        {
            for (const Entry& e : m_images)
                if (e.Path.AsView() == path)
                    return e.Picture.Get();
            UniquePtr<image::Image> picture = MakeUnique<image::Image>(AppRoot());
            if (!image::io::LoadImage(m_resolve(path).AsView(), *picture).IsOk())
                picture = UniquePtr<image::Image>{}; // missing: remembered as such
            m_images.PushBack(Entry{String(path), Move(picture)});
            return m_images[m_images.Size() - 1].Picture.Get();
        }

    private:
        struct Entry
        {
            String Path;
            UniquePtr<image::Image> Picture;
        };
        Function<String(StringView)> m_resolve;
        Array<Entry> m_images;
    };

    struct ScriptErrors final : script::IScriptErrorHandler
    {
        void OnError(const script::ScriptError& error) override
        {
            const String message(error.message);
            const String module(error.module);
            std::fprintf(stderr, "GUISandbox script %s, line %d: %s\n", reinterpret_cast<const char*>(module.CStr()),
                         static_cast<int>(error.line), reinterpret_cast<const char*>(message.CStr()));
        }
    };
}

class GUISandbox : public samples::framework::SampleApp
{
public:
    void SetStartPage(StringView id) { m_startPage = String(id); }
    void SetOpenMenu(bool open) { m_openMenuAtStart = open; }
    void SetMenuCall(StringView function) { m_menuCall = String(function); }
    void SetKeys(StringView keys) { m_keys = String(keys); }
    void SetAlwaysRedraw(bool always) { m_alwaysRedraw = always; }
    void SetFrameStats(bool stats) { m_frameStats = stats; }
    void SetSweep(bool sweep) { m_sweep = sweep; }
    void SetScroll(f32 pixels) { m_scroll = pixels; }
    void SetLayoutOverlay(bool on) { m_layoutOverlay = on; }

    GUISandbox()
    {
        m_width = 960;
        m_height = 820;
    }
    StringView Title() const override { return u8"GUI Sandbox"; }
    bool DrawsTextComposition() const override { return true; } // its fields show an IME's composition in place
    u32 BufferCount() const override { return kFrames; }

protected:
    Status OnInit() override;
    void OnRender() override;
    void OnResize(u32 width, u32 height) override { (void)m_targets.Resize(width, height); } // the GPU is idle here
    void OnShutdown() override;

private:
    static constexpr u32 kFrames = 2;

    // === Building ===
    void LoadFonts();
    [[nodiscard]] String ReadText(StringView relative);
    [[nodiscard]] RefPtr<gui::Node> LoadMarkup(StringView text);
    RefPtr<script::IScriptContext> RunScript(StringView relative, gui::GuiScriptBinding& binding);
    void BuildSandbox();
    void LoadPages();
    void FillNativeHosts();
    void ApplySandboxSheet();
    void BuildMenu();

    // === Running ===
    void ShowPage(StringView id);
    void ToggleTheme();
    // The layout overlay (F9) on both roots: bounds, padding, margins and text baselines.
    void ApplyLayoutOverlay();
    void OpenWindow();
    void ShowMessage();
    void OpenMenu();
    void CloseMenu();
    void LayOutShell();
    void RouteInput();
    void UpdateMenu();
    void TypeKeys(gui::SceneRoot& root);
    void Sweep();
    void DrawAndPresent(gui::SceneRoot& root);
    [[nodiscard]] gui::SceneRoot& Shown() { return m_showingMenu ? *m_menuRoot : *m_root; }

    // === Options ===
    String m_startPage = String(u8"controls");
    bool m_openMenuAtStart = false;
    String m_menuCall; // a game-menu script function to call once it is up (a screenshot of a panel)
    String m_keys;     // keystrokes to type into the root on show, one every 15 frames
    bool m_layoutOverlay = false;
    f32 m_scroll = 0.0f; // the page scrolled down this far once it is up (a screenshot further down)
    bool m_alwaysRedraw = false;
    bool m_frameStats = false;
    // --sweep: every page, then the game menu, each widget hovered, clicked and right-clicked, one
    // step a frame, each printed first (a crash's last line names its widget); then exit.
    bool m_sweep = false;
    struct SweepStep
    {
        String page;     // a page to show first (empty: none)
        bool menu = false; // the game menu's root
        Float2 at{};
        String what;
    };
    Array<SweepStep> m_sweepSteps;
    usize m_sweepNext = 0;
    f32 m_nextStatsTime = 1.0f;
    u64 m_frames = 0;      // frames the loop ran
    u64 m_drawnFrames = 0; // of them, the ones that drew

    // === Render plumbing ===
    shaders::ShaderSystemHost m_shaderHost{AppRoot()}; // owns the ShaderSystem + the VG modules
    rhi::ShaderModule* m_vs = nullptr; // borrowed from m_shaderHost
    rhi::ShaderModule* m_fs = nullptr;
    rhi::ShaderModule* m_gradRadialFs = nullptr;
    rhi::ShaderModule* m_gradConicFs = nullptr;
    rhi::ShaderModule* m_distanceFieldFs = nullptr;
    rhi::ShaderModule* m_boxShadowFs = nullptr;
    rhi::ShaderModule* m_boxVs = nullptr;
    rhi::ShaderModule* m_boxFs = nullptr;
    rhi::ShaderModule* m_layerFs = nullptr;
    rhi::ShaderModule* m_blurVs = nullptr;
    rhi::ShaderModule* m_blurFs = nullptr;
    vg::renderer::VGPassTargets m_targets; // MSAA + stencil (clip paths, complex fills)
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    u64 m_fenceVal = 0;
    u32 m_frameIndex = 0;
    UniquePtr<fonts::TrueTypeFontService> m_fontService;
    UniquePtr<vg::VGContext> m_vg;
    vg::renderer::VGRenderer m_renderer{AppRoot()};
    fonts::CachedFont* m_font = nullptr; // Roboto 16, for what native code builds

    // === The GUI: one context, two roots ===
    RefPtr<gui::GUIContext> m_gui;
    RefPtr<gui::SceneRoot> m_root;     // the sandbox
    RefPtr<gui::SceneRoot> m_menuRoot; // the game menu
    UniquePtr<SandboxResources> m_resources; // pictures by path, from the data folder
    gui::StyleManager m_styles;        // the sandbox's sheet
    gui::StyleManager m_menuStyles;    // the game menu's
    UniquePtr<gui::GuiInputBridge> m_bridge;
    UniquePtr<gui::GuiInputBridge> m_menuBridge;
    UniquePtr<gui::ShellClipboard> m_clipboard;
    UniquePtr<shell::InputSurface> m_inputSurface;
    UniquePtr<shell::InputRouter> m_router;
    bool m_showingMenu = false;
    f32 m_menuLeaveAt = -1.0f; // when the game menu's Leave has faded: back to the sandbox

    // The shell and its pages.
    RefPtr<gui::Node> m_shellNode;
    gui::Node* m_header = nullptr;
    gui::Node* m_body = nullptr;
    gui::Node* m_sidebar = nullptr;
    gui::ScrollView* m_content = nullptr;
    gui::Button* m_themeToggle = nullptr;
    struct Page
    {
        String id;
        gui::Node* node = nullptr;      // in the content scroller
        gui::Node* navButton = nullptr; // in the sidebar
        String sheet;                   // its .css, which styles it alone
        RefPtr<script::IScriptContext> script;
    };
    Array<Page> m_pages;
    String m_current;
    bool m_light = false;

    // Scripts: one manager, a context per page that has a script, one for the game menu.
    RefPtr<script::IScriptManager> m_scripts;
    gui::GuiScriptBinding m_sandboxBinding;
    gui::GuiScriptBinding m_menuBinding;
    ScriptErrors m_scriptErrors;
    RefPtr<script::IScriptContext> m_menuScript;
    bool m_menuWantsUpdate = false;

    // What native hosts hold.
    UniquePtr<image::OwnedImageData> m_checker;
    UniquePtr<gui::StringListModel> m_listModel;
    UniquePtr<gui::TableModel> m_tableModel;
    UniquePtr<gui::SortingProxyModel> m_tableProxy;
    UniquePtr<gui::TreeModel> m_treeModel;
    RefPtr<gui::Menu> m_contextMenu;
    RefPtr<gui::Window> m_floating; // the Menus page's floating window
    RefPtr<gui::MessageBox> m_dialog;
};

// === Building ===

Status GUISandbox::OnInit()
{
    gui::RegisterGuiElements(); // the lab's widgets, creatable by tag from markup
    // The VG shaders through the shared ShaderSystemHost over the data mount (cooked pack or dev
    // DXC over Shaders/), the same corpus the runtime UI uses, and everything VG draws with.
    if (!m_shaderHost.Initialize(*m_device, DataFileSystem()))
        return ErrorCode::Unknown;
    const auto variant = [this](StringView name, shaders::ShaderStage stage)
    { return m_shaderHost.GetVariant(name, stage, shaders::ShaderFlags::None); };
    m_vs = variant(u8"vg", shaders::ShaderStage::Vertex);
    m_fs = variant(u8"vg", shaders::ShaderStage::Fragment);
    m_gradRadialFs = variant(u8"vg_grad_radial", shaders::ShaderStage::Fragment);
    m_gradConicFs = variant(u8"vg_grad_conic", shaders::ShaderStage::Fragment);
    m_distanceFieldFs = variant(u8"vg_df", shaders::ShaderStage::Fragment);
    m_boxShadowFs = variant(u8"vg_shadow", shaders::ShaderStage::Fragment);
    m_boxVs = variant(u8"vg_box", shaders::ShaderStage::Vertex);
    m_boxFs = variant(u8"vg_box", shaders::ShaderStage::Fragment);
    m_layerFs = variant(u8"vg_layer", shaders::ShaderStage::Fragment);
    m_blurVs = variant(u8"vg_blur", shaders::ShaderStage::Vertex);
    m_blurFs = variant(u8"vg_blur", shaders::ShaderStage::Fragment);
    if (m_vs == nullptr || m_fs == nullptr || m_gradRadialFs == nullptr || m_gradConicFs == nullptr)
        return ErrorCode::Unknown;

    m_targets.Configure(*m_device);
    const bool targetsOk = m_targets.Create(m_swapChain->Format(), m_width, m_height);
    if (!m_renderer
             .Initialize(*m_device, *m_vs, *m_fs, m_swapChain->Format(), static_cast<i32>(kFrames), m_distanceFieldFs,
                         m_gradRadialFs, m_gradConicFs, targetsOk ? m_targets.ConfigFor() : vg::renderer::VGTargetConfig{},
                         m_boxShadowFs, m_boxVs, m_boxFs, m_layerFs, m_blurVs, m_blurFs)
             .IsOk())
        return ErrorCode::Unknown;
    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) != ErrorCode::Ok)
        return ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != ErrorCode::Ok)
        return ErrorCode::Unknown;

    m_fontService = MakeUnique<fonts::TrueTypeFontService>(AppRoot(), AppRoot());
    LoadFonts();
    m_vg = MakeUnique<vg::VGContext>(AppRoot(), m_fontService.Get());
    m_vg->SetPerPixelGradients(true);
    m_vg->SetAnalyticBoxes(m_boxVs != nullptr && m_boxFs != nullptr);
    m_vg->SetLayers(m_layerFs != nullptr);
    m_vg->SetStencilFills(m_renderer.StencilFillsSupported());

#ifdef OPTION_HAS_ANGELSCRIPT
    RegisterCoreTypes();
    gui::RegisterGuiScriptSurface();
    m_scripts = script::angelscript::CreateScriptManager(AppRoot());
    script::RegisterReflectedTypes(*m_scripts);
#else
    std::fprintf(stderr, "GUISandbox: built without AngelScript; the pages' and the game menu's scripts do not run\n");
#endif

    m_gui = MakeRef<gui::GUIContext>(AppRoot());
    // The data the pages and the game menu share.
    gui::DataModel& data = m_gui->GetData();
    data.Set(StringView(u8"volume"), Variant::From<f32>(0.7f));
    data.Set(StringView(u8"music"), Variant::From<bool>(true));
    data.Set(StringView(u8"name"), Variant::From<String>(String(u8"Ada")));
    data.Set(StringView(u8"clicks"), Variant::From<i32>(0));
    data.Set(StringView(u8"cuts"), Variant::From<i32>(0));
    data.Set(StringView(u8"menu"), Variant::From<String>(String(u8"pick an item")));
    data.Set(StringView(u8"status"), Variant::From<String>(String(u8"Loading...")));
    data.Set(StringView(u8"progress"), Variant::From<f32>(0.0f));
    data.Set(StringView(u8"quit"), Variant::From<bool>(false));

    BuildSandbox();
    BuildMenu();
    if (m_openMenuAtStart)
        OpenMenu();
    return ErrorCode::Ok;
}

void GUISandbox::LoadFonts()
{
    // Roboto in coverage atlases (a size baked on first use), Lilita as one distance-field bake
    // drawn crisp at any size, whose field text effects read (12 texels: effects reach 6 past the
    // edge at the bake size).
    fonts::FontLoadOptions roboto = fonts::FontLoadOptions::ExtendedLatin();
    roboto.pixelHeight = 16.0f;
    (void)m_fontService->LoadFont(u8"Roboto", DataPath(kRoboto).AsView(), roboto);
    m_font = m_fontService->GetFont(u8"Roboto", 16.0f);
    fonts::DistanceFieldFonts::Initialize();
    fonts::FontLoadOptions lilita = fonts::FontLoadOptions::DistanceField();
    lilita.pixelHeight = 48.0f;
    lilita.distanceFieldRange = 12.0f;
    if (m_fontService->LoadFont(u8"Lilita", DataPath(kLilita).AsView(), lilita) != fonts::FontLoadResult::Success)
        std::fprintf(stderr, "GUISandbox: the Lilita face did not load\n");
}

String GUISandbox::ReadText(StringView relative)
{
    Result<Array<byte>> bytes = ReadFile(DataPath(relative).AsView(), AppRoot());
    if (!bytes.HasValue())
        return {};
    return String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
}

RefPtr<gui::Node> GUISandbox::LoadMarkup(StringView text)
{
    const gui::ElementSet elements = gui::ElementSet::All();
    gui::MarkupLoader loader(elements, AppRoot(), nullptr, m_fontService.Get());
    loader.SetDataModel(m_gui->GetDataRef());
    return loader.LoadFromString(text);
}

RefPtr<script::IScriptContext> GUISandbox::RunScript(StringView relative, gui::GuiScriptBinding& binding)
{
    if (!m_scripts)
        return {};
    const String source = ReadText(relative);
    if (source.Size() == 0)
        return {};
    RefPtr<script::IScriptContext> context = m_scripts->CreateContext();
    context->SetErrorHandler(&m_scriptErrors);
    gui::InstallGuiScriptService(*context, binding);
    if (!context->Load(source.AsView(), relative).IsOk())
        std::fprintf(stderr, "GUISandbox: %s did not load\n", reinterpret_cast<const char*>(String(relative).CStr()));
    return context;
}

void GUISandbox::BuildSandbox()
{
    m_root = MakeRef<gui::SceneRoot>(AppRoot(), m_gui);
    m_root->SetDisplayScale(m_window != nullptr ? m_window->ContentScale() : 1.0f);
    m_root->SetViewportSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_sandboxBinding.root = m_root.Get();

    String path(kSandboxDir);
    path += StringView(u8"shell.xml");
    m_shellNode = LoadMarkup(ReadText(path.AsView()).AsView());
    if (!m_shellNode)
    {
        std::fprintf(stderr, "GUISandbox: the shell did not load\n");
        return;
    }
    m_root->AddChild(m_shellNode.Get());
    m_header = FindById(*m_shellNode, u8"header");
    m_body = FindById(*m_shellNode, u8"body");
    m_sidebar = FindById(*m_shellNode, u8"sidebar");
    m_content = Cast<gui::ScrollView>(FindById(*m_shellNode, u8"content"));
    m_themeToggle = Cast<gui::Button>(FindById(*m_shellNode, u8"theme-toggle"));

    LoadPages();
    FillNativeHosts();

    // The shell's commands; the sidebar's buttons are bound to page.<id>, the header's to the theme.
    GUISandbox* self = this;
    (void)m_root->OnCommand(u8"theme.toggle", [self](gui::CommandEvent&) { self->ToggleTheme(); });
    (void)m_root->OnCommand(u8"demo.window", [self](gui::CommandEvent&) { self->OpenWindow(); });
    (void)m_root->OnCommand(u8"demo.message", [self](gui::CommandEvent&) { self->ShowMessage(); });
    (void)m_root->OnCommand(u8"showcase.open", [self](gui::CommandEvent&) { self->OpenMenu(); });
    (void)m_root->OnCommand(u8"debug.layout", [self](gui::CommandEvent&) { self->m_layoutOverlay = !self->m_layoutOverlay; self->ApplyLayoutOverlay(); });
    (void)m_gui->GetKeymap().Bind(u8"f9", u8"debug.layout");
    ApplyLayoutOverlay();

    m_styles.SetFontService(m_fontService.Get());
    m_gui->SetFontService(m_fontService.Get()); // a rich text's own faces
    GUISandbox* app = this;
    m_resources = MakeUnique<SandboxResources>(AppRoot(), [app](StringView path) { return app->DataPath(path); });
    m_styles.SetResourceProvider(m_resources.Get());
    m_gui->SetResourceProvider(m_resources.Get());
    ApplySandboxSheet();
    ShowPage(m_startPage.AsView());

    m_bridge = MakeUnique<gui::GuiInputBridge>(AppRoot(), m_root->GetEventDispatcher());
    if (m_shell != nullptr)
    {
        m_clipboard = MakeUnique<gui::ShellClipboard>(AppRoot(), m_shell);
        m_root->GetEventDispatcher()->SetClipboard(m_clipboard.Get());
    }
    m_root->GetEventDispatcher()->SetTooltipDelay(0.4f); // tooltips are the GUI's own (E5)
}

void GUISandbox::LoadPages()
{
    // The pages are what the sidebar names: each button bound to page.<id>.
    Array<gui::Node*> buttons;
    if (m_sidebar != nullptr)
        CollectCommands(*m_sidebar, kPageCommand, buttons);
    GUISandbox* self = this;
    for (gui::Node* button : buttons)
    {
        Page page;
        page.id = String(button->GetCommand().SubStr(kPageCommand.Size(), button->GetCommand().Size() - kPageCommand.Size()));
        page.navButton = button;
        String base(kSandboxDir);
        base += StringView(u8"pages/");
        base += page.id.AsView();
        String xml = base;
        xml += StringView(u8".xml");
        RefPtr<gui::Node> node = LoadMarkup(ReadText(xml.AsView()).AsView());
        if (!node)
        {
            std::fprintf(stderr, "GUISandbox: page %s did not load\n", reinterpret_cast<const char*>(xml.CStr()));
            continue;
        }
        node->SetDisplayed(false); // out of the layout until shown
        m_content->AddChild(node.Get());
        page.node = node.Get();
        String css = base;
        css += StringView(u8".css");
        page.sheet = ReadText(css.AsView());
        const String id = page.id;
        String command(kPageCommand);
        command += id.AsView();
        (void)m_root->OnCommand(command.AsView(), [self, id](gui::CommandEvent&) { self->ShowPage(id.AsView()); });
        m_pages.PushBack(Move(page));
    }
    // Their scripts, once every page is in the tree (a script may reach another page's widgets).
    for (Page& page : m_pages)
    {
        String as(kSandboxDir);
        as += StringView(u8"pages/");
        as += page.id.AsView();
        as += StringView(u8".as");
        if (FileExists(DataPath(as.AsView()).AsView()))
            page.script = RunScript(as.AsView(), m_sandboxBinding);
    }
}

void GUISandbox::FillNativeHosts()
{
    // Controls: a generated image.
    if (gui::Node* host = FindById(*m_shellNode, u8"controls-image"))
    {
        static u8 pixels[16 * 16 * 4];
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x)
            {
                const usize o = static_cast<usize>((y * 16 + x) * 4);
                const bool checker = ((x / 4) + (y / 4)) % 2 == 0;
                pixels[o + 0] = static_cast<u8>(checker ? 40 + x * 12 : 20);
                pixels[o + 1] = static_cast<u8>(checker ? 90 : 40 + y * 12);
                pixels[o + 2] = static_cast<u8>(checker ? 160 : 80);
                pixels[o + 3] = 255;
            }
        m_checker = MakeUnique<image::OwnedImageData>(AppRoot(), 16, 16, image::PixelFormat::RGBA8, Span<const u8>(pixels, sizeof(pixels)));
        auto view = MakeRef<gui::Image>(AppRoot());
        view->SetSize(host->GetSize().x > 0.0f ? host->GetSize() : Float2{160.0f, 160.0f});
        view->SetDrawable(MakeRef<gui::ImageDrawable>(AppRoot(), m_checker.Get()));
        view->SetFit(gui::ObjectFit::Contain);
        view->SetTooltip(u8"An Image from code: a generated picture, object-fit: contain");
        host->AddChild(view.Get());
    }

    // Data views over native models.
    if (gui::Node* host = FindById(*m_shellNode, u8"views-list"))
    {
        m_listModel = MakeUnique<gui::StringListModel>(AppRoot());
        Array<String> rows;
        for (i32 i = 0; i < 500; ++i)
            rows.PushBack(Format(u8"Row {}", i));
        m_listModel->SetItems(Move(rows));
        auto list = MakeRef<gui::ListView>(AppRoot());
        list->SetSize(Float2{320.0f, 180.0f});
        list->SetFont(m_font);
        list->SetRowHeight(24.0f);
        list->SetModel(m_listModel.Get());
        auto* picked = Cast<gui::Label>(FindById(*m_shellNode, u8"views-picked"));
        gui::StringListModel* model = m_listModel.Get();
        list->SetOnSelectionChanged(
            [picked, model](gui::ModelIndex index)
            {
                if (picked != nullptr && index.IsValid())
                    picked->SetText(Format(u8"picked: {}", model->ItemAt(static_cast<usize>(index.Row))).AsView());
            });
        host->AddChild(list.Get());
    }
    if (gui::Node* host = FindById(*m_shellNode, u8"views-table"))
    {
        m_tableModel = MakeUnique<gui::TableModel>(AppRoot());
        Array<String> columns;
        columns.PushBack(String(u8"Name"));
        columns.PushBack(String(u8"Score"));
        m_tableModel->SetColumns(Move(columns));
        const char8_t* names[6] = {u8"Ivy", u8"Cara", u8"Bo", u8"Ada", u8"Eve", u8"Dan"};
        const i32 scores[6] = {42, 7, 91, 15, 68, 30};
        for (usize i = 0; i < 6; ++i)
        {
            Array<gui::Variant> cells;
            cells.PushBack(gui::Variant(StringView(names[i])));
            cells.PushBack(gui::Variant(static_cast<i64>(scores[i])));
            m_tableModel->AddRow(Move(cells));
        }
        m_tableProxy = MakeUnique<gui::SortingProxyModel>(AppRoot(), m_tableModel.Get());
        auto table = MakeRef<gui::TableView>(AppRoot());
        table->SetSize(Float2{320.0f, 180.0f});
        table->SetFont(m_font);
        table->SetRowHeight(24.0f);
        table->SetModel(m_tableProxy.Get());
        gui::SortingProxyModel* proxy = m_tableProxy.Get();
        table->SetOnColumnHeaderClicked([proxy](usize column) { proxy->ToggleSort(column); });
        host->AddChild(table.Get());
    }
    if (gui::Node* host = FindById(*m_shellNode, u8"views-tree"))
    {
        m_treeModel = MakeUnique<gui::TreeModel>(AppRoot());
        const i32 code = m_treeModel->AddNode(gui::TreeModel::kRoot, u8"Code");
        const i32 experimental = m_treeModel->AddNode(code, u8"Experimental");
        m_treeModel->AddNode(experimental, u8"GUI");
        m_treeModel->AddNode(experimental, u8"GUI.Sandbox");
        const i32 foundation = m_treeModel->AddNode(code, u8"Foundation");
        m_treeModel->AddNode(foundation, u8"VG");
        m_treeModel->AddNode(foundation, u8"Script");
        const i32 data = m_treeModel->AddNode(gui::TreeModel::kRoot, u8"Data");
        m_treeModel->AddNode(data, u8"Assets");
        auto tree = MakeRef<gui::TreeView>(AppRoot());
        tree->SetSize(Float2{320.0f, 180.0f});
        tree->SetFont(m_font);
        tree->SetRowHeight(24.0f);
        tree->SetModel(m_treeModel.Get());
        tree->ExpandItem(0);
        host->AddChild(tree.Get());
    }

    // Menus: a menu bar and a context menu, each saying what was picked through the data.
    RefPtr<gui::DataModel> data = m_gui->GetDataRef();
    const auto said = [data](const char8_t* what) { return [data, what]() { data->Set(u8"menu", Variant::From<String>(String(what))); }; };
    if (gui::Node* host = FindById(*m_shellNode, u8"menus-bar"))
    {
        auto bar = MakeRef<gui::MenuBar>(AppRoot());
        bar->SetSize(Float2{590.0f, 28.0f});
        bar->SetFont(m_font);
        gui::Menu* file = bar->AddMenu(u8"File");
        file->AddItem(u8"New", said(u8"File > New"));
        file->AddItem(u8"Open...", said(u8"File > Open"));
        file->AddSeparator();
        file->AddItem(u8"Close", said(u8"File > Close"));
        gui::Menu* edit = bar->AddMenu(u8"Edit");
        edit->AddItem(u8"Undo", said(u8"Edit > Undo"))->SetShortcut(u8"Ctrl+Z");
        edit->AddItem(u8"Redo", said(u8"Edit > Redo"))->SetShortcut(u8"Ctrl+Y");
        gui::Menu* insert = edit->AddSubMenu(u8"Insert");
        insert->AddItem(u8"Image", said(u8"Edit > Insert > Image"));
        insert->AddItem(u8"Table", said(u8"Edit > Insert > Table"));
        gui::Menu* view = bar->AddMenu(u8"View");
        view->AddCheckItem(u8"Show grid", true,
                           [data](bool on) { data->Set(u8"menu", Variant::From<String>(String(on ? u8"grid on" : u8"grid off"))); });
        host->AddChild(bar.Get());
    }
    if (gui::Node* area = FindById(*m_shellNode, u8"menus-context"))
    {
        for (usize i = 0; i < area->ChildCount(); ++i)
            area->GetChildAt(i)->SetHitTestVisible(false); // the hint lets the click reach the box
        m_contextMenu = MakeRef<gui::Menu>(AppRoot());
        m_contextMenu->SetFont(m_font);
        m_contextMenu->SetWidth(200.0f);
        m_contextMenu->AddItem(u8"Cut", said(u8"context > Cut"))->SetShortcut(u8"Ctrl+X");
        m_contextMenu->AddItem(u8"Copy", said(u8"context > Copy"))->SetShortcut(u8"Ctrl+C");
        m_contextMenu->AddSeparator();
        gui::Menu* more = m_contextMenu->AddSubMenu(u8"More");
        more->SetFont(m_font);
        more->SetWidth(140.0f);
        more->AddItem(u8"Rename", said(u8"context > More > Rename"));
        gui::Menu* menu = m_contextMenu.Get();
        // A right release, a long press or Shift+F10 asks for it (E5).
        area->AddEventListener(gui::EventType::ContextMenu,
                               [menu, area](const gui::Event& e)
                               {
                                   const auto& mouse = static_cast<const gui::MouseEvent&>(e);
                                   e.Handled = true;
                                   menu->Open(*area, area->GetRootNode()->ConvertToNodeSpace(mouse.Position));
                               });
    }
}

void GUISandbox::ApplySandboxSheet()
{
    // The built-in theme for the widgets (the user agent's), the sandbox's look over it, and each
    // page's own sheet over that, styling its page alone.
    m_styles.SetUserAgentSheet(gui::CSSParser::Parse(m_light ? gui::DefaultLightThemeCSS() : gui::DefaultDarkThemeCSS()));
    String path(kSandboxDir);
    path += StringView(u8"sandbox.css");
    m_styles.SetStyleSheet(gui::CSSParser::Parse(ReadText(path.AsView()).AsView()));
    for (const Page& page : m_pages)
        if (page.node != nullptr)
            (void)m_styles.SetSubtreeSheet(*page.node, gui::CSSParser::Parse(page.sheet.AsView()));
}

void GUISandbox::ApplyLayoutOverlay()
{
    const gui::LayoutOverlay overlay = m_layoutOverlay ? gui::LayoutOverlay::All() : gui::LayoutOverlay{};
    if (m_root)
        m_root->SetLayoutOverlay(overlay);
    if (m_menuRoot)
        m_menuRoot->SetLayoutOverlay(overlay);
}

void GUISandbox::BuildMenu()
{
    m_menuRoot = MakeRef<gui::SceneRoot>(AppRoot(), m_gui);
    m_menuRoot->SetDisplayScale(m_window != nullptr ? m_window->ContentScale() : 1.0f);
    m_menuRoot->SetViewportSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_menuBinding.root = m_menuRoot.Get();
    {
        GUISandbox* self = this;
        (void)m_menuRoot->OnCommand(u8"debug.layout", [self](gui::CommandEvent&) { self->m_layoutOverlay = !self->m_layoutOverlay; self->ApplyLayoutOverlay(); });
    }
    ApplyLayoutOverlay();
    String path(kMenuDir);
    path += StringView(u8"menu.xml");
    RefPtr<gui::Node> menu = LoadMarkup(ReadText(path.AsView()).AsView());
    if (!menu)
    {
        std::fprintf(stderr, "GUISandbox: the game menu did not load\n");
        return;
    }
    m_menuRoot->AddChild(menu.Get());
    String css(kMenuDir);
    css += StringView(u8"menu.css");
    m_menuStyles.SetFontService(m_fontService.Get());
    m_menuStyles.SetStyleSheet(gui::CSSParser::Parse(ReadText(css.AsView()).AsView()));
    // Leave: the menu fades out, then the sandbox comes back.
    RefPtr<gui::DataModel> model = m_gui->GetDataRef();
    menu->Bind([this, model]()
               {
                   const Variant quit = gui::ConvertVariant(model->Get(u8"quit"), TypeOf<bool>());
                   if (!quit.IsEmpty() && quit.Get<bool>() && m_menuLeaveAt < 0.0f)
                       m_menuLeaveAt = m_totalTime + 0.7f;
               });
    String as(kMenuDir);
    as += StringView(u8"menu.as");
    m_menuScript = RunScript(as.AsView(), m_menuBinding);
    m_menuBridge = MakeUnique<gui::GuiInputBridge>(AppRoot(), m_menuRoot->GetEventDispatcher());
}

// === Running ===

void GUISandbox::ShowPage(StringView id)
{
    for (Page& page : m_pages)
    {
        const bool shown = page.id.AsView() == id;
        page.node->SetDisplayed(shown);
        if (auto* button = Cast<gui::UIWidget>(page.navButton))
        {
            if (shown)
                button->AddClass(u8"current");
            else
                button->RemoveClass(u8"current");
        }
    }
    m_current = String(id);
    if (m_content != nullptr)
    {
        m_content->SetScrollOffset(Float2{0.0f, 0.0f});
    }
}

void GUISandbox::ToggleTheme()
{
    m_light = !m_light;
    if (auto* shellWidget = Cast<gui::UIWidget>(m_shellNode.Get()))
    {
        if (m_light)
            shellWidget->AddClass(u8"light");
        else
            shellWidget->RemoveClass(u8"light");
    }
    if (m_themeToggle != nullptr)
        m_themeToggle->SetText(m_light ? StringView(u8"Dark theme") : StringView(u8"Light theme"));
    ApplySandboxSheet();
}

void GUISandbox::OpenWindow()
{
    if (!m_floating)
    {
        // A window to drag by its title and resize from its corner, holding tabs.
        m_floating = MakeRef<gui::Window>(AppRoot());
        m_floating->SetSize(Float2{340.0f, 280.0f});
        m_floating->SetPosition(Float2{520.0f, 160.0f});
        m_floating->SetTitle(u8"A floating window");
        m_floating->SetFont(m_font);
        m_root->AddChild(m_floating.Get());
        auto tabs = MakeRef<gui::TabView>(AppRoot());
        tabs->SetFont(m_font);
        tabs->EditLayoutStyle([](gui::LayoutStyle& s) { s.Grow = 1.0f; });
        m_floating->GetContent()->AddChild(tabs.Get());
        const char8_t* names[3] = {u8"First", u8"Second", u8"Third"};
        const char8_t* bodies[3] = {u8"Tabs switch the page below them.", u8"Drag the title bar to move the window.",
                                    u8"Drag the bottom-right corner to resize it."};
        for (usize i = 0; i < 3; ++i)
        {
            gui::TabPage* page = tabs->AddPage(StringView(names[i]));
            auto body = MakeRef<gui::Label>(AppRoot());
            body->SetPadding(gui::Thickness{12.0f});
            body->SetFont(m_font);
            body->SetWhiteSpace(gui::TextWhiteSpace::Normal);
            body->SetText(StringView(bodies[i]));
            body->AddClass(u8"text");
            page->AddChild(body.Get());
        }
        return;
    }
    m_floating->SetVisible(!m_floating->IsVisible());
}

void GUISandbox::ShowMessage()
{
    if (!m_dialog)
    {
        m_dialog = MakeRef<gui::MessageBox>(AppRoot());
        m_dialog->SetFont(m_font);
    }
    m_dialog->Configure(u8"A message box", u8"A modal dialog: the page behind it takes no clicks until it is answered.",
                        gui::MessageBox::Buttons::Ok);
    m_dialog->OpenModal(*m_root.Get());
}

void GUISandbox::OpenMenu()
{
    if (!m_menuRoot)
        return;
    m_showingMenu = true;
    m_menuLeaveAt = -1.0f;
    m_gui->GetData().Set(u8"quit", Variant::From<bool>(false));
    m_gui->SetActiveRoot(m_menuRoot.Get());
    if (m_menuScript && m_menuScript->HasFunction(u8"enter"))
        (void)m_menuScript->Call(u8"enter", Span<Variant>{});
    m_menuWantsUpdate = true;
}

void GUISandbox::CloseMenu()
{
    m_showingMenu = false;
    m_menuLeaveAt = -1.0f;
    m_gui->GetData().Set(u8"quit", Variant::From<bool>(false));
    m_gui->SetActiveRoot(m_root.Get());
    if (gui::Node* back = FindById(*m_shellNode, u8"nav-game-menu"))
        (void)back->FocusElement(); // where the player left from
}

void GUISandbox::LayOutShell()
{
    // The shell fills the window: the header across the top, the sidebar down the left, the page's
    // scroller the rest; the game menu's layers fill it too.
    const Float2 size = m_root->GetSize();
    if (m_shellNode)
        m_shellNode->SetSize(size);
    if (m_header != nullptr)
        m_header->SetSize(Float2{size.x, kHeaderHeight});
    const Float2 body{size.x, Max(0.0f, size.y - kHeaderHeight)};
    if (m_body != nullptr)
        m_body->SetSize(body);
    if (m_sidebar != nullptr)
        m_sidebar->SetSize(Float2{kSidebarWidth, body.y});
    if (m_content != nullptr)
        m_content->SetSize(Float2{Max(0.0f, body.x - kSidebarWidth), body.y});
    if (m_menuRoot && m_menuRoot->ChildCount() != 0)
    {
        gui::Node* menu = m_menuRoot->GetChildAt(0);
        menu->SetSize(m_menuRoot->GetSize());
        for (usize i = 0; i < menu->ChildCount(); ++i)
            menu->GetChildAt(i)->SetSize(m_menuRoot->GetSize());
    }
}

void GUISandbox::RouteInput()
{
    // A fullscreen InputSurface, gated by a router: the pointer through the surface, keys, text and
    // the gamepad from the event stream, all to the root on show.
    if (m_shell == nullptr || m_shell->Input() == nullptr || m_window == nullptr)
        return;
    shell::IInputManager& input = *m_shell->Input();
    gui::GuiInputBridge& bridge = m_showingMenu ? *m_menuBridge : *m_bridge;
    const Rectangle region{0.0f, 0.0f, static_cast<f32>(m_width), static_cast<f32>(m_height)};
    if (!m_inputSurface)
    {
        const ContentFit fit{region, Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)}, FitMode::Stretch};
        m_inputSurface = MakeUnique<shell::InputSurface>(AppRoot(), &input, m_window->Id(), fit);
        m_router = MakeUnique<shell::InputRouter>(AppRoot(), &input);
        m_router->AddSurface(m_inputSurface.Get());
        m_bridge->SetTextInputTarget(m_window);
        m_bridge->SetCursorTarget(input.Mouse()); // the pointer takes the look of what it is over
        if (m_menuBridge)
        {
            m_menuBridge->SetTextInputTarget(m_window);
            m_menuBridge->SetCursorTarget(input.Mouse());
        }
    }
    m_inputSurface->SetRegion(region);
    m_inputSurface->SetWindowSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)}); // places fingers
    m_inputSurface->SetContentSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_router->Update();
    bridge.PumpFromSurface(*m_inputSurface);
    for (const shell::InputEvent& event : input.Events())
    {
        switch (event.kind)
        {
        case shell::InputEventKind::KeyDown:
        case shell::InputEventKind::KeyUp:
        case shell::InputEventKind::TextInput:
        case shell::InputEventKind::GamepadButtonDown:
        case shell::InputEventKind::GamepadButtonUp:
        case shell::InputEventKind::GamepadAxis:
            bridge.Dispatch(event);
            break;
        default:
            break;
        }
    }
}

void GUISandbox::TypeKeys(gui::SceneRoot& root)
{
    // --keys: the named keystrokes, one every 15 frames once the GUI is up.
    if (m_keys.Size() == 0 || m_frames < 10 || (m_frames - 10) % 15 != 0)
        return;
    const Array<StringView> keys = gui::SplitTopLevel(m_keys.AsView(), u8' ');
    const usize index = static_cast<usize>((m_frames - 10) / 15);
    if (index >= keys.Size())
        return;
    if (const Optional<gui::Keystroke> stroke = gui::ParseKeystroke(keys[index]); stroke.HasValue())
    {
        if (stroke.Value().Pad)
            root.GetEventDispatcher()->InjectPadButton(static_cast<gui::PadButton>(stroke.Value().Code));
        else
            root.GetEventDispatcher()->InjectKeyDown(stroke.Value().Code, stroke.Value().Modifiers);
        m_menuWantsUpdate = true;
    }
}

namespace
{
    // The widgets under `node` a sweep visits: shown, sized, in the tree's order.
    void CollectSweep(gui::Node& node, Array<gui::Node*>& out)
    {
        if (!node.IsVisible())
            return;
        if (node.GetSize().x > 0.0f && node.GetSize().y > 0.0f && Cast<gui::UIWidget>(&node) != nullptr)
            out.PushBack(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            CollectSweep(*node.GetChildAt(i), out);
    }

    String Describe(gui::Node& node)
    {
        const gui::UIWidget* widget = Cast<gui::UIWidget>(&node);
        String text(node.GetType() != nullptr ? StringView(reinterpret_cast<const utf8char*>(node.GetType()->name)) : StringView(u8"?"));
        if (widget != nullptr && widget->GetId().Size() != 0)
        {
            text += StringView(u8" #");
            text += widget->GetId();
        }
        return text;
    }
}

void GUISandbox::Sweep()
{
    if (m_frames < 10)
        return;
    if (m_sweepSteps.IsEmpty() && m_sweepNext == 0)
    {
        // Plan it: each page shown in turn, its widgets' centres as they lie once shown.
        for (const Page& page : m_pages)
        {
            ShowPage(page.id.AsView());
            m_root->UpdateLayout();
            Array<gui::Node*> nodes;
            CollectSweep(*page.node, nodes);
            for (gui::Node* node : nodes)
            {
                const gui::Rect bounds = node->GetScreenBounds();
                if (bounds.y + bounds.height * 0.5f > static_cast<f32>(m_height))
                    continue; // below the fold
                m_sweepSteps.PushBack(SweepStep{page.id, false, Float2{bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f}, Describe(*node)});
            }
        }
        ShowPage(m_startPage.AsView());
        m_sweepSteps.PushBack(SweepStep{String(), true, Float2{}, String(u8"open the game menu")});
        std::printf("GUISandbox sweep: %llu steps\n", static_cast<unsigned long long>(m_sweepSteps.Size()));
    }
    if (m_sweepNext >= m_sweepSteps.Size())
    {
        std::printf("GUISandbox sweep: done\n");
        std::fflush(stdout);
        Exit();
        return;
    }
    const SweepStep& step = m_sweepSteps[m_sweepNext++];
    std::printf("GUISandbox sweep %llu: %s %s at %.0f, %.0f\n", static_cast<unsigned long long>(m_sweepNext),
                reinterpret_cast<const char*>(step.page.CStr()), reinterpret_cast<const char*>(step.what.CStr()), step.at.x, step.at.y);
    std::fflush(stdout);
    if (step.menu)
    {
        OpenMenu();
        return;
    }
    if (step.page.AsView() != m_current.AsView())
        ShowPage(step.page.AsView());
    // Device pixels: the root's transform maps them (its display scale).
    const Float2 at{step.at.x * m_root->GetDisplayScale(), step.at.y * m_root->GetDisplayScale()};
    gui::EventDispatcher& d = *m_root->GetEventDispatcher();
    d.InjectMouseMove(at);
    d.InjectMouseDown(at, gui::MouseButton::Left);
    d.InjectMouseUp(at, gui::MouseButton::Left);
    d.InjectMouseDown(at, gui::MouseButton::Right);
    d.InjectMouseUp(at, gui::MouseButton::Right);
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Escape)); // close what a click opened
}

void GUISandbox::UpdateMenu()
{
    // --menu-call: once the menu has been up a few frames, as a click would come.
    if (m_menuScript && m_menuCall.Size() != 0 && m_frames == 10)
    {
        if (!m_menuScript->Call(m_menuCall.AsView(), Span<Variant>{}).HasValue())
            std::fprintf(stderr, "GUISandbox: the game menu's script has no %s()\n", reinterpret_cast<const char*>(m_menuCall.CStr()));
        m_menuWantsUpdate = true;
    }
    // Its update runs while it asks, and on a frame with input; an idle menu calls no script.
    const bool input = m_shell != nullptr && m_shell->Input() != nullptr && !m_shell->Input()->Events().IsEmpty();
    if (m_menuScript && (m_menuWantsUpdate || input) && m_menuScript->HasFunction(u8"update"))
    {
        Variant args[1] = {Variant::From<f64>(static_cast<f64>(m_deltaTime))};
        Result<Variant> result = m_menuScript->Call(u8"update", Span<Variant>(args, 1));
        const bool* more = result.HasValue() ? result.Value().TryGet<bool>() : nullptr;
        m_menuWantsUpdate = more != nullptr && *more;
    }
    if (m_menuLeaveAt >= 0.0f && m_totalTime >= m_menuLeaveAt)
        CloseMenu();
}

void GUISandbox::OnRender()
{
    RouteInput();
    gui::SceneRoot& shown = Shown();
    TypeKeys(shown);
    // --scroll: once the GUI is up (held a few frames while the page settles).
    if (m_scroll > 0.0f && m_frames >= 10 && m_frames <= 20 && m_content != nullptr)
        m_content->SetScrollOffset(Float2{0.0f, m_scroll});
    if (m_sweep)
        Sweep();

    // A window moved to a monitor of another density takes its scale.
    const Float2 viewport{static_cast<f32>(m_width), static_cast<f32>(m_height)};
    gui::SceneRoot* roots[2] = {m_root.Get(), m_menuRoot.Get()};
    for (gui::SceneRoot* root : roots)
    {
        if (root == nullptr)
            continue;
        if (m_window != nullptr && m_window->ContentScale() != root->GetDisplayScale())
            root->SetDisplayScale(m_window->ContentScale());
        root->SetViewportSize(viewport);
    }
    LayOutShell();
    m_gui->Update(Duration::FromSeconds(static_cast<f64>(m_deltaTime)));
    if (m_showingMenu)
        UpdateMenu();

    // Style the root on show (the other draws nothing), then the layout styling moved.
    gui::SceneRoot& drawn = Shown(); // the menu may have closed this frame
    gui::StyleManager& styles = m_showingMenu ? m_menuStyles : m_styles;
    styles.SetMediaContext(drawn.GetMediaContext());
    styles.ApplyTree(drawn);
    drawn.UpdateLayout();

    // Nothing changed, nothing animating: skip the frame (the last one stays on screen) and wait a
    // little instead of spinning. --always-redraw draws every frame.
    ++m_frames;
    if (m_frameStats && m_totalTime >= m_nextStatsTime)
    {
        std::printf("GUISandbox: drew %llu of %llu frames\n", static_cast<unsigned long long>(m_drawnFrames),
                    static_cast<unsigned long long>(m_frames));
        std::fflush(stdout);
        m_nextStatsTime = m_totalTime + 1.0f;
    }
    if (!m_alwaysRedraw && !drawn.NeedsFrame() && !ScreenshotPending())
    {
        SleepMilliseconds(8);
        return;
    }
    ++m_drawnFrames;
    DrawAndPresent(drawn);
}

void GUISandbox::DrawAndPresent(gui::SceneRoot& root)
{
    m_vg->Clear();
    {
        gui::DrawContext dc{*m_vg};
        root.Draw(dc);
    }
    vg::VGBatch& batch = m_vg->GetBatch();

    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != ErrorCode::Ok)
        return;
    m_renderer.BeginFrame(static_cast<i32>(m_frameIndex));
    const vg::renderer::VGRenderSlice slice = m_renderer.Prepare(batch, static_cast<i32>(m_frameIndex), m_width, m_height);
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != ErrorCode::Ok || enc == nullptr)
        return;
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget);
    m_renderer.RenderLayers(*enc, static_cast<i32>(m_frameIndex), slice); // layers draw in passes of their own, first

    const rhi::ClearColor clear = rhi::ClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    rhi::RenderPassDesc rpd{};
    if (m_targets.Matches(m_width, m_height))
    {
        rpd = m_targets.BeginPass(*enc, m_swapChain->CurrentTextureView(), clear);
    }
    else
    {
        rhi::ColorAttachment ca{};
        ca.view = m_swapChain->CurrentTextureView();
        ca.loadOp = rhi::LoadOp::Clear;
        ca.storeOp = rhi::StoreOp::Store;
        ca.clearValue = clear;
        rpd.colorAttachments.Add(ca);
    }
    rhi::RenderPassEncoder* rp = enc->BeginRenderPass(rpd);
    m_renderer.Render(*rp, m_width, m_height, static_cast<i32>(m_frameIndex), slice);
    rp->End();

    CaptureBackbuffer(*enc, m_swapChain->CurrentTexture()); // F12 or --screenshot: this frame, as presented
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget, rhi::ResourceState::Present);
    rhi::CommandBuffer* cb = enc->Finish();
    ++m_fenceVal;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
    m_frameIndex = (m_frameIndex + 1) % kFrames;
}

void GUISandbox::OnShutdown()
{
    std::printf("GUISandbox: drew %llu of %llu frames\n", static_cast<unsigned long long>(m_drawnFrames),
                static_cast<unsigned long long>(m_frames));
    if (m_device)
        m_device->WaitIdle();
    // Scripts first: their handles and handlers reach the trees.
    const auto closeScript = [](RefPtr<script::IScriptContext>& context)
    {
        if (!context)
            return;
        gui::ClearGuiScriptService(*context);
        context->SetErrorHandler(nullptr);
        context.Reset();
    };
    for (Page& page : m_pages)
        closeScript(page.script);
    closeScript(m_menuScript);
    m_scripts.Reset();
    m_router.Reset();
    m_inputSurface.Reset();
    m_bridge.Reset();
    m_menuBridge.Reset();
    m_pages.Clear();
    m_contextMenu.Reset();
    m_floating.Reset();
    m_dialog.Reset();
    m_shellNode.Reset();
    m_root.Reset(); // the views go with the tree, before the models they read
    m_menuRoot.Reset();
    m_gui.Reset();
    m_tableProxy.Reset();
    m_tableModel.Reset();
    m_listModel.Reset();
    m_treeModel.Reset();
    m_checker.Reset();
    m_renderer.Dispose();
    m_targets.Destroy(); // before the framework releases the device
    m_vg.Reset();
    m_fontService.Reset();
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    m_shaderHost.Shutdown(); // the shader modules are its
}

int main(int argc, char** argv)
{
    GUISandbox app;
    // --page <id>: open on that page (controls, text, layout, data-views, menus, styling, effects,
    // motion, binding, navigation, game-menu). --showcase: open the game menu at once.
    // --menu-call <function>: call one of the game menu's script functions once it is up (onSettings,
    // onPlay, onQuit). --keys "<keys>": type keystrokes into what is shown, one every 15 frames
    // ("down down return", "pad-east"). --always-redraw: draw every frame. --frame-stats: print,
    // each second, how many frames drew. --sweep: hover, click and right-click every widget of every
    // page, then open the game menu, printing each step, and exit (a smoke test; a crash's last line
    // names its widget). --scroll <px>: scroll the page down that far once it is up.
    // --layout-overlay: start with the layout overlay on (F9 toggles it). With the
    // framework's --screenshot flags, a picture of any of it.
    for (int i = 1; i < argc; ++i)
    {
        const StringView arg(reinterpret_cast<const utf8char*>(argv[i]));
        const auto next = [&]() { return StringView(reinterpret_cast<const utf8char*>(argv[++i])); };
        if (arg == StringView(u8"--page") && i + 1 < argc)
            app.SetStartPage(next());
        else if (arg == StringView(u8"--showcase"))
            app.SetOpenMenu(true);
        else if (arg == StringView(u8"--menu-call") && i + 1 < argc)
            app.SetMenuCall(next());
        else if (arg == StringView(u8"--keys") && i + 1 < argc)
            app.SetKeys(next());
        else if (arg == StringView(u8"--always-redraw"))
            app.SetAlwaysRedraw(true);
        else if (arg == StringView(u8"--frame-stats"))
            app.SetFrameStats(true);
        else if (arg == StringView(u8"--sweep"))
            app.SetSweep(true);
        else if (arg == StringView(u8"--layout-overlay"))
            app.SetLayoutOverlay(true);
        else if (arg == StringView(u8"--scroll") && i + 1 < argc)
            app.SetScroll(static_cast<f32>(std::atof(reinterpret_cast<const char*>(argv[++i]))));
    }
    return app.Run(argc, argv);
}

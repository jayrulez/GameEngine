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

#include <algorithm> // sort (the soak's frame times)
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
import foundation.fonts.bidi;
import foundation.fonts.harfbuzz; // joined Arabic (E9e)
import foundation.i18n;           // the Language and direction page's table (E9e)
import foundation.xml;
import foundation.xml.serialization;
import foundation.fonts.distancefield.baker; // the Lilita distance-field face
import foundation.vg;
import foundation.vg.renderer;
import experimental.gui;
import experimental.gui.shell;
import experimental.gui.inspector; // Ctrl+Shift+I (E8e)
import experimental.gui.testing;   // --keys targets by selector or text (E8f), the image tests' match (E8g)
import foundation.rhi.capture;     // the image tests' readback (E8g)
import gui.sandbox.stress; // the Stress page (E8a)
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
    // This binary's composition root: the ONE ambient-allocator decision here. It counts what is
    // live (--soak reports it); built in place and never destroyed, so frees at exit still find it.
    [[nodiscard]] TrackingAllocator& AppRoot() noexcept
    {
        alignas(TrackingAllocator) static unsigned char storage[sizeof(TrackingAllocator)];
        static TrackingAllocator* const tracking = new (storage) TrackingAllocator(DefaultAllocator());
        return *tracking;
    }

    // The process's resident memory in MiB (Linux; 0 elsewhere), for what does not go through AppRoot.
    [[nodiscard]] f64 ResidentMiB()
    {
        f64 mib = 0.0;
        if (FILE* statm = std::fopen("/proc/self/statm", "r"))
        {
            unsigned long size = 0;
            unsigned long resident = 0;
            if (std::fscanf(statm, "%lu %lu", &size, &resident) == 2)
                mib = static_cast<f64>(resident) * 4096.0 / (1024.0 * 1024.0);
            std::fclose(statm);
        }
        return mib;
    }

    constexpr StringView kRoboto = u8"Assets/fonts/roboto/Roboto-Regular.ttf";
    constexpr StringView kLilita = u8"Assets/fonts/lilita-one/LilitaOne-Regular.ttf";
    constexpr StringView kDejaVu = u8"Assets/fonts/dejavu/DejaVuSansMono.ttf"; // Arabic, which Roboto has not
    constexpr StringView kStrings = u8"Assets/ui/sandbox/i18n/sandbox.strings.xml";
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

    // --narrate (E9e): a bridge that speaks to the console as a screen reader would aloud: each
    // focus change (what it reached, what it is, its value and states) and each announcement.
    class NarrateBridge final : public gui::IAccessibilityBridge
    {
    public:
        explicit NarrateBridge(IAllocator& allocator) : m_nodes(allocator) {}
        [[nodiscard]] bool IsListening() const override { return true; }
        void Update(const gui::AccessTreeUpdate& update) override
        {
            if (update.Full)
                m_nodes.Clear();
            for (const gui::AccessNode& node : update.Nodes)
                m_nodes.InsertOrAssign(node.Id, node);
            if (update.Focus == m_focus)
                return;
            m_focus = update.Focus;
            const gui::AccessNode* focused = m_nodes.Find(update.Focus);
            if (focused == nullptr || focused->Role == gui::AccessRole::Window)
                return;
            String line = focused->Name.Size() != 0 ? focused->Name : String(u8"(no name)");
            line += StringView(u8", ");
            line += gui::AccessRoleName(focused->Role);
            if (focused->Value.Size() != 0)
            {
                line += StringView(u8", ");
                line += focused->Value.AsView();
            }
            if (focused->Has(gui::AccessChecked))
                line += StringView(u8", checked");
            if (focused->Has(gui::AccessCollapsed))
                line += StringView(u8", collapsed");
            if (focused->Has(gui::AccessExpanded))
                line += StringView(u8", expanded");
            if (focused->Has(gui::AccessDisabled))
                line += StringView(u8", dimmed");
            if (focused->Description.Size() != 0)
            {
                line += StringView(u8". ");
                line += focused->Description.AsView();
            }
            std::printf("narrate: %s\n", reinterpret_cast<const char*>(line.CStr()));
            std::fflush(stdout);
        }
        void Announce(StringView text, gui::AnnouncePriority priority) override
        {
            const String said(text);
            std::printf("narrate (%s): %s\n", priority == gui::AnnouncePriority::Assertive ? "now" : "next",
                        reinterpret_cast<const char*>(said.CStr()));
            std::fflush(stdout);
        }

    private:
        HashMap<u64, gui::AccessNode> m_nodes;
        u64 m_focus = 0;
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
    /// --stress <n>: start on the Stress page, built at load n.
    void SetStress(i32 load)
    {
        m_startPage = String(u8"stress");
        m_stressLoad = load;
    }
    void SetOpenMenu(bool open) { m_openMenuAtStart = open; }
    void SetMenuCall(StringView function) { m_menuCall = String(function); }
    void SetKeys(StringView keys) { m_keys = String(keys); }
    void SetAlwaysRedraw(bool always) { m_alwaysRedraw = always; }
    void SetFrameStats(bool stats) { m_frameStats = stats; }
    void SetSweep(bool sweep) { m_sweep = sweep; }
    /// --soak <minutes> [--soak-csv <path>]: the sweep's steps over and over for that long, a CSV
    /// row every ten seconds (frame time percentiles, live allocations and bytes, resident memory).
    void SetSoak(f32 minutes) { m_soakSeconds = Max(0.0f, minutes) * 60.0f; m_sweep = m_soakSeconds > 0.0f; }
    void SetSoakCsv(StringView path) { m_soakCsv = String(path); }
    void SetScroll(f32 pixels) { m_scroll = pixels; }
    void SetLayoutOverlay(bool on) { m_layoutOverlay = on; }
    void SetInputOverlay(bool on) { m_inputOverlay = on; }
    void SetOpenInspector(bool on) { m_openInspector = on; }
    // --image-tests (E8g): each page drawn offscreen at a fixed size on a fixed clock, compared
    // with its reference; --approve-images writes the pictures as the references instead.
    void SetImageTests(bool on) { m_imageTests = on; }
    void SetApproveImages(StringView how) { m_imageTests = true; m_approve = String(how); }
    void SetImageOut(StringView dir) { m_imageOut = String(dir); }
    void SetBackendName(StringView name) { m_backendName = String(name); }
    // What main returns: the image tests' verdict (77, skipped, when no device came up).
    [[nodiscard]] int ImageExitCode(int runCode) const
    {
        if (!m_imageTests)
            return m_sweep ? SweepExitCode() : runCode;
        if (m_imagesDone == 0)
            return 77;
        return m_imageFailures == 0 ? 0 : 1;
    }
    // The sweep's verdict: 77 (skipped) when it never ran (no device), else whether anything warned.
    [[nodiscard]] int SweepExitCode() const
    {
        if (!m_sweepDone)
            return 77;
        if (m_sweepLog.problems != 0)
            std::printf("GUISandbox sweep: %u warnings or errors\n", m_sweepLog.problems);
        return m_sweepLog.problems == 0 ? 0 : 1;
    }
    void SetInspectSelector(StringView selector) { m_openInspector = true; m_inspectSelector = String(selector); }
    /// --lang <locale>: the GUI's language at start (E9e; "pseudo": English pseudolocalized).
    void SetStartLanguage(StringView locale) { m_startLanguage = String(locale); }
    /// --narrate: a stand-in screen reader on the console.
    void SetNarrate(bool on) { m_narrate = on; }

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
    [[nodiscard]] bool LoadFontFace(StringView face, StringView source);
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
    void ApplyDebugOverlay();
    void ToggleInspector();
    // The inspector's pane width in device pixels (0 while it is closed).
    [[nodiscard]] f32 InspectorPixels() const
    {
        return m_inspectorOpen ? m_inspectorWidth * (m_window != nullptr ? m_window->ContentScale() : 1.0f) : 0.0f;
    }
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
    String m_startLanguage = String(u8"en");
    bool m_narrate = false;
    bool m_openMenuAtStart = false;
    String m_menuCall; // a game-menu script function to call once it is up (a screenshot of a panel)
    String m_keys;     // keystrokes to type into the root on show, one every 15 frames
    bool m_layoutOverlay = false; // F9: the debug overlay's layout half
    bool m_inputOverlay = false;  // F8: its input half (focus path, tab order, navigation, pointer, redraws)
    f32 m_scroll = 0.0f; // the page scrolled down this far once it is up (a screenshot further down)
    bool m_alwaysRedraw = false;
    bool m_frameStats = false;
    i32 m_stressLoad = 1;
    f32 m_nextStressStatsTime = 0.0f;
    // --sweep: every page, then the game menu, each widget hovered, clicked and right-clicked, and
    // each page walked by keys and the pad, one step a frame, each printed first (a crash's last
    // line names its widget); then exit, failing if anything warned or erred while it ran (the
    // GUI.Sweep ctest entries, skipped without a device).
    bool m_sweep = false;
    bool m_sweepDone = false;
    struct SweepStep
    {
        String page;     // a page to show first (empty: none)
        bool menu = false; // the game menu's root
        bool keys = false; // the page walked by Tab, the arrows and the pad instead of a click
        Float2 at{};
        String what;
    };
    // What warned or erred while the sweep ran.
    class SweepLog final : public ILogSink
    {
    public:
        u32 problems = 0;
        void Write(LogLevel level, StringView, StringView) noexcept override
        {
            if (static_cast<u8>(level) >= static_cast<u8>(LogLevel::Warning))
                ++problems;
        }
    };
    SweepLog m_sweepLog;
    f32 m_soakSeconds = 0.0f; // > 0: soaking, not sweeping once
    String m_soakCsv = String(u8"gui-soak.csv");
    f32 m_soakStart = -1.0f;
    f32 m_soakNextRow = 0.0f;
    Array<f32> m_soakFrames; // frame times (ms) since the last row
    FILE* m_soakFile = nullptr;
    void SoakFrame();
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

    // The Stress page's builder and its views' models: before the GUI, so it outlives the views.
    UniquePtr<sandbox::StressPage> m_stress;

    // The texts the Language and direction page shows (E9e), the GUI's localization: before the
    // GUI, which lets go of it when it goes. The narrating bridge likewise.
    foundation::i18n::StringTable m_strings;
    foundation::i18n::Localization m_localization;
    UniquePtr<NarrateBridge> m_narrator;
    void LoadStrings();
    void SetLanguage(StringView locale); // "pseudo": English pseudolocalized
    void SetPageDirection(gui::Node::LayoutDirection direction);

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
    // The inspector (E8e): a pane of its own root on the right, Ctrl+Shift+I.
    UniquePtr<gui::inspector::Inspector> m_inspector;
    UniquePtr<gui::GuiInputBridge> m_inspectorBridge;
    UniquePtr<shell::InputSurface> m_inspectorSurface;
    bool m_inspectorOpen = false;
    bool m_openInspector = false; // --inspector
    // The image tests (E8g).
    static constexpr u32 kImageWidth = 960;
    static constexpr u32 kImageHeight = 720;
    static constexpr i32 kImageFrames = 40; // frames on the fixed clock before a page's picture
    bool m_imageTests = false;
    String m_approve;                       // "" compares; "shared" or "backend" writes references
    String m_imageOut = String(u8"build/gui-images");
    String m_backendName = String(u8"vulkan");
    Array<String> m_imagePages;
    usize m_imagePage = 0;
    i32 m_imageFrame = 0;
    u32 m_imagesDone = 0;
    u32 m_imageFailures = 0;
    rhi::Texture* m_imageTarget = nullptr;
    rhi::TextureView* m_imageView = nullptr;
    vg::renderer::VGPassTargets m_imageTargets;
    rhi::ScreenshotCapture m_imageCapture;
    void RunImageTests();
    bool RenderImage(gui::SceneRoot& root, StringView path, image::Image& out);
    String m_inspectSelector;     // --inspect <selector>: selected once the GUI is up
    f32 m_inspectorWidth = 460.0f;  // logical units; the boundary drags it
    bool m_resizingInspector = false; // the boundary is held
    bool m_overInspectorEdge = false; // the pointer is on the boundary (or holds it)
    // The boundary between the root on show and the pane: on it the cursor shows the resize
    // arrows, pressed it drags the pane's width (neither root sees the pointer meanwhile).
    // True while it has the pointer.
    bool ResizeInspector(shell::IInputManager& input);
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
    i32 m_insertedRows = 0; // rows the Data views page inserted at the top
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

    // Mixed-direction text reorders by the Unicode bidirectional algorithm (E9c), as Engine.UI's does.
    fonts::BidiAlgorithm::Install();
    // Shaped by HarfBuzz (E9e): Arabic joins its letters. Installed before any font loads, so each
    // one's atlas takes the joined forms its shaping reaches.
    fonts::HarfBuzzShaping::Install();
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
    data.Set(StringView(u8"files"), Variant::From<i32>(3)); // the Language page's count
    LoadStrings();
    if (m_narrate)
    {
        m_narrator = MakeUnique<NarrateBridge>(AppRoot(), AppRoot());
        m_gui->SetAccessibilityBridge(m_narrator.Get());
    }

    BuildSandbox();
    BuildMenu();
    if (m_openMenuAtStart)
        OpenMenu();
    return ErrorCode::Ok;
}

// The Language and direction page's table (E9e): the serialized StringTable in the data folder,
// read through the XML serializer, given to the GUI as its localization in the start language.
void GUISandbox::LoadStrings()
{
    foundation::i18n::RegisterStringTableType();
    const String text = ReadText(kStrings);
    foundation::xml::XmlDocument document(AppRoot());
    if (text.Size() == 0 || document.Parse(text.AsView()) != foundation::xml::XmlResult::Ok)
    {
        std::fprintf(stderr, "GUISandbox: the string table did not load\n");
        return;
    }
    foundation::xml::XmlSerializer reader(document);
    m_strings.Serialize(reader);
    if (!reader.IsPayloadOk())
    {
        std::fprintf(stderr, "GUISandbox: the string table did not read\n");
        return;
    }
    m_localization.AddTable(&m_strings);
    m_gui->SetLocalization(&m_localization);
    SetLanguage(m_startLanguage.AsView());
}

void GUISandbox::SetLanguage(StringView locale)
{
    const bool pseudo = locale == StringView(u8"pseudo");
    foundation::i18n::PseudoOptions options;
    options.Enabled = pseudo;
    // Rich-text tags and bindings stay as they are.
    options.Untouched.PushBack(foundation::i18n::UntouchedSpan{String(u8"["), String(u8"]")});
    options.Untouched.PushBack(foundation::i18n::UntouchedSpan{String(u8"{{"), String(u8"}}")});
    m_localization.SetPseudolocalization(Move(options));
    m_localization.SetLanguage(pseudo ? StringView(u8"en") : locale);
    m_gui->Announce(m_localization.Text(u8"lang.changed"));
}

void GUISandbox::SetPageDirection(gui::Node::LayoutDirection direction)
{
    for (const Page& page : m_pages)
        if (page.id.AsView() == StringView(u8"language"))
            page.node->SetLayoutDirection(direction);
}

void GUISandbox::LoadFonts()
{
    // Roboto in coverage atlases (a size baked on first use), Lilita as one distance-field bake
    // drawn crisp at any size, whose field text effects read (12 texels: effects reach 6 past the
    // edge at the bake size).
    fonts::FontLoadOptions roboto = fonts::FontLoadOptions::ExtendedLatin();
    roboto.pixelHeight = 16.0f;
    roboto.lastCodepoint = 0x17F; // Latin Extended-A too: European languages, and pseudolocalized text
    (void)m_fontService->LoadFont(u8"Roboto", DataPath(kRoboto).AsView(), roboto);
    m_font = m_fontService->GetFont(u8"Roboto", 16.0f);
    fonts::DistanceFieldFonts::Initialize();
    fonts::FontLoadOptions lilita = fonts::FontLoadOptions::DistanceField();
    lilita.pixelHeight = 48.0f;
    lilita.distanceFieldRange = 12.0f;
    if (m_fontService->LoadFont(u8"Lilita", DataPath(kLilita).AsView(), lilita) != fonts::FontLoadResult::Success)
        std::fprintf(stderr, "GUISandbox: the Lilita face did not load\n");
    // DejaVu Sans Mono for Arabic (E9e): Latin, Greek, Cyrillic, Hebrew and Arabic, and the joined
    // forms HarfBuzz reaches from them.
    fonts::FontLoadOptions dejavu = fonts::FontLoadOptions::ExtendedLatin();
    dejavu.pixelHeight = 16.0f;
    dejavu.lastCodepoint = 0x6FF;
    dejavu.atlasWidth = 2048;
    dejavu.atlasHeight = 2048;
    if (m_fontService->LoadFont(u8"DejaVu", DataPath(kDejaVu).AsView(), dejavu) != fonts::FontLoadResult::Success)
        std::fprintf(stderr, "GUISandbox: the DejaVu face did not load\n");
}

// A sheet's @font-face (E11): the file under the data folder, into the font service under the
// face's name, in coverage atlases as Roboto's are.
bool GUISandbox::LoadFontFace(StringView face, StringView source)
{
    fonts::FontLoadOptions options = fonts::FontLoadOptions::ExtendedLatin();
    options.pixelHeight = 16.0f;
    options.lastCodepoint = 0x17F;
    if (m_fontService->LoadFont(face, DataPath(source).AsView(), options) == fonts::FontLoadResult::Success)
        return true;
    std::fprintf(stderr, "GUISandbox: the @font-face %.*s (%.*s) did not load\n", static_cast<int>(face.Size()),
                 reinterpret_cast<const char*>(face.Data()), static_cast<int>(source.Size()), reinterpret_cast<const char*>(source.Data()));
    return false;
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
    for (const Page& page : m_pages)
        if (page.id.AsView() == StringView(u8"stress"))
        {
            m_stress = MakeUnique<sandbox::StressPage>(AppRoot(), AppRoot(), m_font);
            m_stress->SetLoad(m_stressLoad);
            m_stress->Attach(*page.node);
        }

    // The shell's commands; the sidebar's buttons are bound to page.<id>, the header's to the theme.
    GUISandbox* self = this;
    (void)m_root->OnCommand(u8"theme.toggle", [self](gui::CommandEvent&) { self->ToggleTheme(); });
    (void)m_root->OnCommand(u8"demo.window", [self](gui::CommandEvent&) { self->OpenWindow(); });
    (void)m_root->OnCommand(u8"demo.message", [self](gui::CommandEvent&) { self->ShowMessage(); });
    (void)m_root->OnCommand(u8"showcase.open", [self](gui::CommandEvent&) { self->OpenMenu(); });
    (void)m_root->OnCommand(u8"debug.layout", [self](gui::CommandEvent&) { self->m_layoutOverlay = !self->m_layoutOverlay; self->ApplyDebugOverlay(); });
    (void)m_root->OnCommand(u8"debug.input", [self](gui::CommandEvent&) { self->m_inputOverlay = !self->m_inputOverlay; self->ApplyDebugOverlay(); });
    (void)m_gui->GetKeymap().Bind(u8"f9", u8"debug.layout");
    (void)m_gui->GetKeymap().Bind(u8"f8", u8"debug.input");
    (void)m_root->OnCommand(u8"inspector.toggle", [self](gui::CommandEvent&) { self->ToggleInspector(); });
    // The Language and direction page's (E9e).
    (void)m_root->OnCommand(u8"lang.en", [self](gui::CommandEvent&) { self->SetLanguage(u8"en"); });
    (void)m_root->OnCommand(u8"lang.pseudo", [self](gui::CommandEvent&) { self->SetLanguage(u8"pseudo"); });
    (void)m_root->OnCommand(u8"lang.ar", [self](gui::CommandEvent&) { self->SetLanguage(u8"ar"); });
    (void)m_root->OnCommand(u8"dir.language", [self](gui::CommandEvent&) { self->SetPageDirection(gui::Node::LayoutDirection::Inherit); });
    (void)m_root->OnCommand(u8"dir.ltr", [self](gui::CommandEvent&) { self->SetPageDirection(gui::Node::LayoutDirection::LeftToRight); });
    (void)m_root->OnCommand(u8"dir.rtl", [self](gui::CommandEvent&) { self->SetPageDirection(gui::Node::LayoutDirection::RightToLeft); });
    const auto count = [self](i32 by)
    {
        gui::DataModel& data = self->m_gui->GetData();
        const Variant now = data.Get(StringView(u8"files"));
        const i32 files = now.TryGet<i32>() != nullptr ? *now.TryGet<i32>() : 0;
        data.Set(StringView(u8"files"), Variant::From<i32>(Max(0, files + by)));
    };
    (void)m_root->OnCommand(u8"files.more", [count](gui::CommandEvent&) { count(1); });
    (void)m_root->OnCommand(u8"files.fewer", [count](gui::CommandEvent&) { count(-1); });
    (void)m_gui->GetKeymap().Bind(u8"ctrl-shift-i", u8"inspector.toggle");
    m_root->SetDebugFont(m_font);
    ApplyDebugOverlay();

    GUISandbox* app = this;
    m_styles.SetFontService(m_fontService.Get());
    m_styles.SetFontFaceLoader([app](StringView face, StringView source) { return app->LoadFontFace(face, source); });
    m_gui->SetFontService(m_fontService.Get()); // a rich text's own faces
    m_resources = MakeUnique<SandboxResources>(AppRoot(), [app](StringView path) { return app->DataPath(path); });
    m_styles.SetResourceProvider(m_resources.Get());
    m_gui->SetResourceProvider(m_resources.Get());
    ApplySandboxSheet();
    ShowPage(m_startPage.AsView());

    m_bridge = MakeUnique<gui::GuiInputBridge>(AppRoot(), m_root->GetEventDispatcher());
    if (m_openInspector)
        ToggleInspector();
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
        if (page.id.AsView() == StringView(u8"stress"))
            page.sheet += sandbox::StressPage::Rules(1000).AsView(); // a large sheet's many rules
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
    // Drag and drop (E7): the cards are sources, the bins targets; the hand takes cards, the
    // discard pile refuses aces with a reason; the chip goes nowhere.
    {
        auto* said = Cast<gui::Label>(FindById(*m_shellNode, u8"dnd-said"));
        const char8_t* cards[3] = {u8"dnd-ace", u8"dnd-king", u8"dnd-chip"};
        for (const char8_t* id : cards)
            if (gui::Node* card = FindById(*m_shellNode, id))
            {
                const bool chip = StringView(id) == StringView(u8"dnd-chip");
                card->SetTabFocusable(true); // Ctrl+Shift+D or the pad's North picks it up
                gui::Label* label = Cast<gui::Label>(card);
                card->SetDragSource([label, chip](gui::DragRequest& r)
                {
                    r.Payload.Type = String(chip ? u8"chip" : u8"card");
                    r.Payload.Label = String(label != nullptr ? label->GetText() : StringView{});
                    r.Payload.Value = Variant::From<String>(r.Payload.Label);
                    return true;
                });
                card->SetOnDragEnd([said, label](const gui::DragResult& r)
                {
                    if (label == nullptr)
                        return;
                    // The card goes where it was dropped (a move), or a copy of it does.
                    if (r.Target != nullptr && r.Effect == gui::DropEffect::Move)
                        r.Target->GetChildHost()->AddChild(label);
                    else if (r.Target != nullptr && r.Effect == gui::DropEffect::Copy)
                    {
                        auto copy = MakeRef<gui::Label>(AppRoot());
                        copy->SetText(label->GetText());
                        copy->AddClass(u8"dnd-card");
                        r.Target->GetChildHost()->AddChild(copy.Get());
                    }
                    if (said == nullptr)
                        return;
                    const char8_t* how = r.Cancelled ? u8"cancelled" : r.Effect == gui::DropEffect::Copy ? u8"copied" : r.Effect == gui::DropEffect::Move ? u8"moved" : r.Effect == gui::DropEffect::Link ? u8"linked" : u8"dropped nowhere";
                    said->SetText(Format(u8"{}: {}", label->GetText(), how).AsView());
                });
            }
        const char8_t* targets[3] = {u8"dnd-hand", u8"dnd-discard", u8"dnd-files"};
        for (const char8_t* id : targets)
            if (gui::Node* target = FindById(*m_shellNode, id))
                target->SetTabFocusable(true); // a keyboard drag goes where the focus goes
        if (gui::Node* hand = FindById(*m_shellNode, u8"dnd-hand"))
            hand->SetOnDragQuery([](gui::DropQuery& q)
            {
                if (q.Payload.Type.AsView() != StringView(u8"card"))
                    return;
                q.Accept();
                q.Reason = Format(u8"{} it to the hand", q.Effect == gui::DropEffect::Copy ? u8"Copy" : u8"Move");
            });
        // Files from the OS: the files pile takes them; dropped anywhere else, the GUI's own
        // handler says so.
        if (gui::Node* files = FindById(*m_shellNode, u8"dnd-files"))
        {
            files->SetDropTypes(String(u8"files"));
            (void)files->AddEventListener(gui::EventType::Drop, [said](const gui::Event& e)
            {
                const Array<String> paths = gui::DroppedPaths(static_cast<const gui::DragEvent&>(e).Payload);
                if (said != nullptr && !paths.IsEmpty())
                    said->SetText(Format(u8"Dropped {} file(s), the first {}", paths.Size(), paths[0]).AsView());
            });
        }
        m_root->GetEventDispatcher()->SetOnFileDrop([said](const Array<String>& paths, Float2)
        {
            if (said != nullptr)
                said->SetText(Format(u8"{} file(s) dropped where nothing takes them", paths.Size()).AsView());
            return true;
        });
        const char8_t* piles[2] = {u8"dnd-long", u8"dnd-hidden"};
        for (const char8_t* id : piles)
            if (gui::Node* pile = FindById(*m_shellNode, id))
                pile->SetDropTypes(String(u8"card"));
        if (gui::Node* discard = FindById(*m_shellNode, u8"dnd-discard"))
            discard->SetOnDragQuery([](gui::DropQuery& q)
            {
                if (q.Payload.Type.AsView() != StringView(u8"card"))
                    return;
                if (q.Payload.Label.AsView().Size() >= 3 && q.Payload.Label.AsView().SubStr(0, 3) == StringView(u8"Ace"))
                    q.Refuse(u8"The discard pile takes no aces");
                else
                {
                    q.Accept(gui::DropEffect::Move);
                    q.Reason = String(u8"Discard it");
                }
            });
    }

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
        m_listModel->SetEditable(true); // F2 renames a row
        list->SetDragType(String(u8"row")); // rows drag to reorder (E7)
        auto* picked = Cast<gui::Label>(FindById(*m_shellNode, u8"views-picked"));
        gui::StringListModel* model = m_listModel.Get();
        list->SetOnSelectionChanged(
            [picked, model](gui::ModelIndex index)
            {
                if (picked != nullptr && index.IsValid())
                    picked->SetText(Format(u8"picked: {}", model->ItemAt(static_cast<usize>(index.Row))).AsView());
            });
        list->SetOnActivated(
            [picked, model](gui::ModelIndex index)
            {
                if (picked != nullptr && index.IsValid())
                    picked->SetText(Format(u8"activated: {}", model->ItemAt(static_cast<usize>(index.Row))).AsView());
            });
        host->AddChild(list.Get());
        if (auto* insert = Cast<gui::Button>(FindById(*m_shellNode, u8"views-insert")))
        {
            i32* inserted = &m_insertedRows;
            insert->SetOnClick([model, inserted]() { model->InsertItem(0, Format(u8"Inserted {}", ++*inserted).AsView()); });
        }
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
        m_tableModel->SetEditable(true); // F2 edits a name
        m_tableProxy = MakeUnique<gui::SortingProxyModel>(AppRoot(), m_tableModel.Get());
        auto table = MakeRef<gui::TableView>(AppRoot());
        table->SetSize(Float2{320.0f, 180.0f});
        table->SetFont(m_font);
        table->SetRowHeight(24.0f);
        table->SetModel(m_tableProxy.Get()); // a header click sorts through the proxy (the model's sort capability)
        table->SetColumnWidthsText(String(u8"60% auto"));
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
        tree->SetDragType(String(u8"node")); // nodes drag onto and beside others (E7)
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
    m_styles.SetUserAgentSheet(gui::CSSParser::Parse(m_light ? gui::DefaultLightThemeCSS() : gui::DefaultDarkThemeCSS(), StringView(m_light ? u8"theme (light)" : u8"theme (dark)")));
    String path(kSandboxDir);
    path += StringView(u8"sandbox.css");
    m_styles.SetStyleSheet(gui::CSSParser::Parse(ReadText(path.AsView()).AsView(), path.AsView()));
    for (const Page& page : m_pages)
        if (page.node != nullptr)
            (void)m_styles.SetSubtreeSheet(*page.node, gui::CSSParser::Parse(page.sheet.AsView(), Format(u8"pages/{}.css", page.id).AsView()));
}

void GUISandbox::ApplyDebugOverlay()
{
    gui::DebugOverlay overlay = m_layoutOverlay ? gui::DebugOverlay::Layout() : gui::DebugOverlay{};
    if (m_inputOverlay)
    {
        const gui::DebugOverlay input = gui::DebugOverlay::Input();
        overlay.FocusPath = input.FocusPath;
        overlay.TabOrder = input.TabOrder;
        overlay.Navigation = input.Navigation;
        overlay.Pointer = input.Pointer;
        overlay.RedrawFlash = input.RedrawFlash;
    }
    if (m_root)
        m_root->SetDebugOverlay(overlay);
    if (m_menuRoot)
        m_menuRoot->SetDebugOverlay(overlay);
}

bool GUISandbox::ResizeInspector(shell::IInputManager& input)
{
    if (!m_inspectorOpen || input.Mouse() == nullptr)
    {
        m_resizingInspector = m_overInspectorEdge = false;
        return false;
    }
    shell::IMouse& mouse = *input.Mouse(); // the window's, in its pixels
    const f32 scale = m_window != nullptr ? m_window->ContentScale() : 1.0f;
    const f32 edge = static_cast<f32>(m_width) - InspectorPixels();
    const bool near = Abs(mouse.X() - edge) <= 4.0f * scale && mouse.Y() >= 0.0f && mouse.Y() < static_cast<f32>(m_height);
    if (!m_resizingInspector && near && mouse.IsButtonPressed(shell::MouseButton::Left))
        m_resizingInspector = true;
    if (m_resizingInspector)
    {
        if (!mouse.IsButtonDown(shell::MouseButton::Left))
            m_resizingInspector = false;
        else
        {
            // The pane's width follows the pointer, leaving the root on show some room.
            const f32 widest = Max(260.0f, static_cast<f32>(m_width) / scale - 240.0f);
            m_inspectorWidth = Clamp((static_cast<f32>(m_width) - mouse.X()) / scale, 260.0f, widest);
        }
    }
    const bool over = m_resizingInspector || near;
    if (over != m_overInspectorEdge)
    {
        m_overInspectorEdge = over;
        Shown().Invalidate(); // the boundary's highlight
        if (!over)
        {
            // The GUI's cursors take the pointer back.
            m_bridge->SetCursorTarget(&mouse);
            if (m_menuBridge)
                m_menuBridge->SetCursorTarget(&mouse);
            m_inspectorBridge->SetCursorTarget(&mouse);
        }
    }
    if (over)
        mouse.SetCursor(shell::CursorType::ResizeEW);
    return m_resizingInspector;
}

void GUISandbox::ToggleInspector()
{
    if (!m_inspector)
    {
        m_inspector = MakeUnique<gui::inspector::Inspector>(AppRoot(), AppRoot(), m_gui, m_fontService.Get());
        m_inspectorBridge = MakeUnique<gui::GuiInputBridge>(AppRoot(), m_inspector->GetRoot().GetEventDispatcher());
        if (m_window != nullptr)
            m_inspectorBridge->SetTextInputTarget(m_window);
    }
    m_inspectorOpen = !m_inspectorOpen;
    if (m_inspectorOpen)
        m_inspector->Inspect(&Shown(), m_showingMenu ? &m_menuStyles : &m_styles);
    else
        m_inspector->SetPicking(false);
    Shown().Invalidate();
}

void GUISandbox::BuildMenu()
{
    m_menuRoot = MakeRef<gui::SceneRoot>(AppRoot(), m_gui);
    m_menuRoot->SetDisplayScale(m_window != nullptr ? m_window->ContentScale() : 1.0f);
    m_menuRoot->SetViewportSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_menuBinding.root = m_menuRoot.Get();
    {
        GUISandbox* self = this;
        (void)m_menuRoot->OnCommand(u8"debug.layout", [self](gui::CommandEvent&) { self->m_layoutOverlay = !self->m_layoutOverlay; self->ApplyDebugOverlay(); });
        (void)m_menuRoot->OnCommand(u8"debug.input", [self](gui::CommandEvent&) { self->m_inputOverlay = !self->m_inputOverlay; self->ApplyDebugOverlay(); });
        (void)m_menuRoot->OnCommand(u8"inspector.toggle", [self](gui::CommandEvent&) { self->ToggleInspector(); });
    }
    m_menuRoot->SetDebugFont(m_font);
    ApplyDebugOverlay();
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
    m_menuStyles.SetFontFaceLoader([this](StringView face, StringView source) { return LoadFontFace(face, source); });
    m_menuStyles.SetStyleSheet(gui::CSSParser::Parse(ReadText(css.AsView()).AsView(), css.AsView()));
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
    if (m_stress && id == StringView(u8"stress") && !m_stress->IsBuilt())
        m_stress->Build(m_stressLoad); // built when first shown: the other pages start fast
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
    // The inspector's pane takes the window's right side; the root on show the rest.
    const f32 pane = InspectorPixels();
    const Rectangle region{0.0f, 0.0f, static_cast<f32>(m_width) - pane, static_cast<f32>(m_height)};
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
    m_inputSurface->SetContentSize(Float2{region.width, region.height});
    if (m_inspector && !m_inspectorSurface)
    {
        const ContentFit none{Rectangle{}, Float2{1.0f, 1.0f}, FitMode::Stretch};
        m_inspectorSurface = MakeUnique<shell::InputSurface>(AppRoot(), &input, m_window->Id(), none);
        m_router->AddSurface(m_inspectorSurface.Get());
        m_inspectorBridge->SetCursorTarget(input.Mouse());
    }
    if (m_inspectorSurface)
    {
        // Its own root sits at its origin (drawn moved over): the pane maps to it from 0.
        m_inspectorSurface->SetRegion(Rectangle{region.width, 0.0f, pane, static_cast<f32>(m_height)});
        m_inspectorSurface->SetWindowSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
        m_inspectorSurface->SetContentSize(Float2{Max(pane, 1.0f), static_cast<f32>(m_height)});
    }
    m_router->Update();
    const bool resizing = ResizeInspector(input);
    const bool picking = m_inspectorOpen && m_inspector->IsPicking();
    // While the boundary has the pointer, neither root sees it.
    if (picking && !resizing)
    {
        // Picking: the pointer over the root on show goes to the inspector, not to it.
        shell::IMouse& mouse = *m_inputSurface->Mouse();
        if (m_inputSurface->MouseActive())
        {
            m_inspector->PickHover(Float2{mouse.X(), mouse.Y()});
            if (mouse.ScrollY() != 0.0f)
                m_inspector->PickWheel(mouse.ScrollY());
            if (mouse.IsButtonPressed(shell::MouseButton::Left))
                m_inspector->PickClick();
        }
    }
    else if (!resizing && m_keys.Size() == 0) // a --keys script owns the pointer: the desk's mouse would move it back
        bridge.PumpFromSurface(*m_inputSurface);
    if (m_inspectorOpen && m_inspectorSurface && !resizing)
        m_inspectorBridge->PumpFromSurface(*m_inspectorSurface);
    // Files the OS dropped on the window go through the GUI's drag path (E7).
    Array<shell::DroppedFile> drops(AppRoot());
    m_shell->DrainDroppedFiles(drops);
    for (const shell::DroppedFile& drop : drops) // said, for a desk test of a desktop's drops
        std::printf("GUISandbox: OS drop on window %u (this %u) at %.0f, %.0f: %s\n", drop.window, m_window->Id(), drop.x, drop.y,
                    reinterpret_cast<const char*>(drop.path.CStr()));
    if (!drops.IsEmpty())
        (void)bridge.DispatchFileDrops(drops, m_window->Id());
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
            // Keys go to the pane while it has the focus (it was clicked last).
            if (m_inspectorOpen && m_inspectorSurface && m_inspectorSurface->Focused())
                m_inspectorBridge->Dispatch(event);
            else
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
    // Pointer steps (logical units, as a screenshot is): click:x,y rclick:x,y move:x,y down:x,y up:x,y;
    // or at the middle of what a selector or a text finds: click:#ok, click:text=Show,
    // move:text~=Sett (no spaces: a step ends at one).
    const StringView step = keys[index];
    usize colon = 0;
    while (colon < step.Size() && step[colon] != u8':')
        ++colon;
    if (colon < step.Size())
    {
        const StringView verb = step.SubStr(0, colon);
        const StringView where = step.SubStr(colon + 1, step.Size() - colon - 1);
        usize comma = 0;
        while (comma < where.Size() && where[comma] != u8',')
            ++comma;
        const f32 scale = root.GetDisplayScale();
        Float2 at{static_cast<f32>(ParseFloat(where.SubStr(0, comma)).ValueOr(0.0)) * scale,
                  comma < where.Size() ? static_cast<f32>(ParseFloat(where.SubStr(comma + 1, where.Size() - comma - 1)).ValueOr(0.0)) * scale : 0.0f};
        const bool numeric = where.Size() != 0 && ((where[0] >= u8'0' && where[0] <= u8'9') || where[0] == u8'-');
        if (!numeric)
        {
            const Array<gui::Node*> found = gui::testing::Finder::Parse(where).All(root);
            if (found.IsEmpty())
            {
                std::printf("GUISandbox: --keys: %s found nothing\n", reinterpret_cast<const char*>(String(where).CStr()));
                return;
            }
            const gui::Rect box = found[0]->GetScreenBounds();
            at = Float2{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
        }
        gui::EventDispatcher& d = *root.GetEventDispatcher();
        if (verb == StringView(u8"move"))
            (void)d.InjectMouseMove(at);
        else if (verb == StringView(u8"down"))
            (void)d.InjectMouseDown(at, gui::MouseButton::Left);
        else if (verb == StringView(u8"up"))
            (void)d.InjectMouseUp(at, gui::MouseButton::Left);
        else if (verb == StringView(u8"click") || verb == StringView(u8"rclick"))
        {
            const gui::MouseButton button = verb == StringView(u8"click") ? gui::MouseButton::Left : gui::MouseButton::Right;
            (void)d.InjectMouseMove(at);
            (void)d.InjectMouseDown(at, button);
            (void)d.InjectMouseUp(at, button);
        }
        m_menuWantsUpdate = true;
        return;
    }
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

}

void GUISandbox::SoakFrame()
{
    if (m_soakStart < 0.0f)
    {
        m_soakStart = m_totalTime;
        m_soakNextRow = m_totalTime + 10.0f;
        m_soakFile = std::fopen(reinterpret_cast<const char*>(m_soakCsv.CStr()), "w");
        if (m_soakFile != nullptr)
            std::fprintf(m_soakFile, "seconds,frames,p50_ms,p95_ms,p99_ms,live_allocations,live_bytes,resident_mib\n");
        std::printf("GUISandbox soak: %.1f minutes, rows in %s\n", static_cast<double>(m_soakSeconds / 60.0f),
                    reinterpret_cast<const char*>(m_soakCsv.CStr()));
    }
    m_soakFrames.PushBack(m_deltaTime * 1000.0f);
    if (m_totalTime < m_soakNextRow || m_soakFrames.IsEmpty())
        return;
    m_soakNextRow += 10.0f;
    Array<f32> sorted = m_soakFrames;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&sorted](f32 share) { return sorted[Min(sorted.Size() - 1, static_cast<usize>(share * static_cast<f32>(sorted.Size())))]; };
    const TrackingAllocator& tracking = AppRoot();
    std::printf("GUISandbox soak %.0fs: p50 %.2f ms, p95 %.2f ms, p99 %.2f ms, %llu live allocations, %.1f MiB resident\n",
                static_cast<double>(m_totalTime - m_soakStart), static_cast<double>(at(0.5f)), static_cast<double>(at(0.95f)),
                static_cast<double>(at(0.99f)), static_cast<unsigned long long>(tracking.LiveAllocations()), ResidentMiB());
    if (m_soakFile != nullptr)
    {
        std::fprintf(m_soakFile, "%.1f,%llu,%.3f,%.3f,%.3f,%llu,%llu,%.2f\n", static_cast<double>(m_totalTime - m_soakStart),
                     static_cast<unsigned long long>(sorted.Size()), static_cast<double>(at(0.5f)), static_cast<double>(at(0.95f)),
                     static_cast<double>(at(0.99f)), static_cast<unsigned long long>(tracking.LiveAllocations()),
                     static_cast<unsigned long long>(tracking.LiveBytes()), ResidentMiB());
        std::fflush(m_soakFile);
    }
    m_soakFrames.Clear();
    if (m_totalTime - m_soakStart >= m_soakSeconds && m_soakFile != nullptr)
    {
        std::fclose(m_soakFile);
        m_soakFile = nullptr;
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
            if (page.id.AsView() == StringView(u8"stress"))
                continue; // thousands of widgets: a smoke test of every one would take minutes
            ShowPage(page.id.AsView());
            m_root->UpdateLayout();
            Array<gui::Node*> nodes;
            CollectSweep(*page.node, nodes);
            for (gui::Node* node : nodes)
            {
                const gui::Rect bounds = node->GetScreenBounds();
                if (bounds.y + bounds.height * 0.5f > static_cast<f32>(m_height))
                    continue; // below the fold
                m_sweepSteps.PushBack(SweepStep{page.id, false, false, Float2{bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f}, node->DebugName()});
            }
            m_sweepSteps.PushBack(SweepStep{page.id, false, true, Float2{}, String(u8"keys and pad")});
        }
        ShowPage(m_startPage.AsView());
        m_sweepSteps.PushBack(SweepStep{String(), true, false, Float2{}, String(u8"open the game menu")});
        std::printf("GUISandbox sweep: %llu steps\n", static_cast<unsigned long long>(m_sweepSteps.Size()));
        GlobalLogger().AddSink(&m_sweepLog); // from here on, a warning fails the sweep
    }
    if (m_soakSeconds > 0.0f && m_sweepNext >= m_sweepSteps.Size() && m_totalTime - m_soakStart < m_soakSeconds)
        m_sweepNext = 0; // soaking: round again
    if (m_sweepNext >= m_sweepSteps.Size())
    {
        GlobalLogger().RemoveSink(&m_sweepLog);
        m_sweepDone = true;
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
    if (step.keys)
    {
        // Through the page by Tab, the arrows and the pad: what a keyboard or pad player reaches.
        gui::EventDispatcher& keys = *m_root->GetEventDispatcher();
        const gui::KeyCode codes[] = {gui::KeyCode::Tab, gui::KeyCode::Tab, gui::KeyCode::Down, gui::KeyCode::Right,
                                      gui::KeyCode::Up, gui::KeyCode::Left, gui::KeyCode::Space};
        for (const gui::KeyCode code : codes)
        {
            keys.InjectKeyDown(static_cast<u32>(code));
            keys.InjectKeyUp(static_cast<u32>(code));
        }
        keys.InjectPadButton(gui::PadButton::South);
        keys.InjectPadButton(gui::PadButton::East);
        keys.InjectKeyDown(static_cast<u32>(gui::KeyCode::Escape));
        return;
    }
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
    if (m_imageTests)
    {
        RunImageTests();
        return;
    }
    RouteInput();
    gui::SceneRoot& shown = Shown();
    TypeKeys(shown);
    // --scroll: once the GUI is up (held a few frames while the page settles).
    if (m_scroll > 0.0f && m_frames >= 10 && m_frames <= 20 && m_content != nullptr)
        m_content->SetScrollOffset(Float2{0.0f, m_scroll});
    if (m_sweep)
    {
        Sweep();
        if (m_soakSeconds > 0.0f)
            SoakFrame();
    }

    // A window moved to a monitor of another density takes its scale.
    const Float2 viewport{static_cast<f32>(m_width) - InspectorPixels(), static_cast<f32>(m_height)};
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
    bool paneNeeds = false;
    if (m_inspectorOpen)
    {
        gui::SceneRoot& pane = m_inspector->GetRoot();
        if (m_inspector->GetTarget() != &drawn) // the game menu opened or closed
            m_inspector->Inspect(&drawn, m_showingMenu ? &m_menuStyles : &m_styles);
        if (m_window != nullptr && m_window->ContentScale() != pane.GetDisplayScale())
            pane.SetDisplayScale(m_window->ContentScale());
        pane.SetViewportSize(Float2{InspectorPixels(), static_cast<f32>(m_height)});
        if (m_inspectSelector.Size() != 0 && m_frames == 10)
            (void)m_inspector->SelectQuery(m_inspectSelector.AsView());
        m_inspector->Refresh();
        m_inspector->GetStyles().SetMediaContext(pane.GetMediaContext());
        m_inspector->GetStyles().ApplyTree(pane);
        pane.UpdateLayout();
        paneNeeds = pane.NeedsFrame();
    }

    // Nothing changed, nothing animating: skip the frame (the last one stays on screen) and wait a
    // little instead of spinning. --always-redraw draws every frame.
    ++m_frames;
    if (m_frameStats && m_totalTime >= m_nextStatsTime)
    {
        std::printf("GUISandbox: drew %llu of %llu frames\n", static_cast<unsigned long long>(m_drawnFrames),
                    static_cast<unsigned long long>(m_frames));
        // The last drawn frame's costs and what asked for it (E8a).
        std::printf("GUISandbox: last frame: %s\n", reinterpret_cast<const char*>(drawn.GetLastFrameStats().Describe().CStr()));
        std::fflush(stdout);
        m_nextStatsTime = m_totalTime + 1.0f;
    }
    // The Stress page shows the last frame's costs, each second.
    if (m_stress && m_current.AsView() == StringView(u8"stress") && m_totalTime >= m_nextStressStatsTime)
    {
        m_stress->ShowStats(drawn.GetLastFrameStats());
        m_nextStressStatsTime = m_totalTime + 1.0f;
    }
    const bool drawnNeeds = drawn.NeedsFrame();
    if (!m_alwaysRedraw && !drawnNeeds && !paneNeeds && !ScreenshotPending())
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
        root.DrawFrame(dc); // timed and counted: --frame-stats prints it
        if (m_inspectorOpen)
        {
            m_inspector->RecordFrame(root.GetLastFrameStats());
            m_inspector->DrawOverTarget(dc);
            // The pane, its root moved to the window's right side.
            gui::DrawContext paneContext{*m_vg};
            m_vg->PushState();
            m_vg->Translate(static_cast<f32>(m_width) - InspectorPixels(), 0.0f);
            m_inspector->GetRoot().DrawFrame(paneContext);
            m_vg->PopState();
            // The boundary, lit while the pointer is on it or drags it.
            if (m_overInspectorEdge)
                m_vg->FillRect(Rectangle{static_cast<f32>(m_width) - InspectorPixels() - 1.0f, 0.0f, 2.0f, static_cast<f32>(m_height)},
                               Color{0.31f, 0.61f, 1.0f, 1.0f});
        }
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

// The image tests (E8g): each page shown, run on a fixed clock at a fixed size and scale 1, drawn
// offscreen and read back through the RHI, so the backend in use is the one tested; the picture
// goes to --image-out, and is compared with its reference under Data/Tests/GUI/Images: its own
// backend's when there is one, else the one the backends share. A miss writes a diff beside the
// picture. --approve-images writes the shared reference (or, with "backend", this backend's own),
// which is for after the user has looked at the pictures.
void GUISandbox::RunImageTests()
{
    if (m_imagePages.IsEmpty() && m_imagePage == 0)
    {
        for (const Page& page : m_pages)
            if (page.id.AsView() != StringView(u8"stress")) // its numbers are the machine's
            {
                m_imagePages.PushBack(page.id);
                if (page.id.AsView() == StringView(u8"language"))
                    m_imagePages.PushBack(String(u8"language@ar")); // and right to left, in Arabic (E9e)
            }
        rhi::TextureDesc desc = rhi::TextureDesc::RenderTarget(m_swapChain->Format(), kImageWidth, kImageHeight);
        desc.usage = desc.usage | rhi::TextureUsage::CopySrc;
        if (m_device->CreateTexture(desc, m_imageTarget) != ErrorCode::Ok ||
            m_device->CreateTextureView(m_imageTarget, rhi::TextureViewDesc{}, m_imageView) != ErrorCode::Ok)
        {
            std::printf("GUISandbox: image tests: no offscreen target\n");
            Exit();
            return;
        }
        m_imageTargets.Configure(*m_device);
        (void)m_imageTargets.Create(m_swapChain->Format(), kImageWidth, kImageHeight);
        (void)CreateDirectories(m_imageOut.AsView());
    }
    if (m_imagePage >= m_imagePages.Size())
    {
        std::printf("GUISandbox: image tests on %s: %u pictures, %u failed\n", reinterpret_cast<const char*>(m_backendName.CStr()),
                    m_imagesDone, m_imageFailures);
        std::fflush(stdout);
        Exit();
        return;
    }
    // "<page>@<language>": the page in that language; any other in English. Pictured as
    // <page>.<language>.
    const String& entry = m_imagePages[m_imagePage];
    usize at = entry.Size();
    for (usize i = 0; i < entry.Size(); ++i)
        if (entry[i] == u8'@')
            at = i;
    const bool another = at < entry.Size();
    const StringView pageId = entry.AsView().SubStr(0, at);
    const StringView language = another ? entry.AsView().SubStr(at + 1, entry.Size() - at - 1) : StringView(u8"en");
    const String id = another ? Format(u8"{}.{}", pageId, language) : entry;
    if (m_imageFrame == 0)
    {
        if (m_localization.Language() != language || m_localization.GetPseudolocalization().Enabled)
            SetLanguage(language);
        ShowPage(pageId);
    }
    // A fixed size and scale and a fixed clock: the same frames on every machine.
    m_root->SetDisplayScale(1.0f);
    m_root->SetViewportSize(Float2{static_cast<f32>(kImageWidth), static_cast<f32>(kImageHeight)});
    LayOutShell();
    m_gui->Update(Duration::FromSeconds(1.0 / 60.0));
    m_styles.SetMediaContext(m_root->GetMediaContext());
    m_styles.ApplyTree(*m_root);
    m_root->UpdateLayout();
    if (++m_imageFrame < kImageFrames)
        return;
    m_imageFrame = 0;
    ++m_imagePage;

    const String name = Format(u8"{}.{}", id, m_backendName);
    const String picturePath = PathJoin(m_imageOut.AsView(), Format(u8"{}.png", name).AsView());
    image::Image picture;
    if (!RenderImage(*m_root, picturePath.AsView(), picture))
    {
        std::printf("GUISandbox: image %s: could not be drawn\n", reinterpret_cast<const char*>(name.CStr()));
        ++m_imageFailures;
        return;
    }
    ++m_imagesDone;
    const String shared = DataPath(Format(u8"Tests/GUI/Images/{}.png", id).AsView());
    const String own = DataPath(Format(u8"Tests/GUI/Images/{}.png", name).AsView());
    if (m_approve.Size() != 0)
    {
        const String& to = m_approve.AsView() == StringView(u8"backend") ? own : shared;
        (void)CreateDirectories(DataPath(u8"Tests/GUI/Images").AsView());
        const bool saved = image::io::SaveImage(picture, to.AsView(), image::io::ImageFileFormat::PNG).IsOk();
        std::printf("GUISandbox: image %s: %s %s\n", reinterpret_cast<const char*>(name.CStr()), saved ? "approved as" : "could not write",
                    reinterpret_cast<const char*>(to.CStr()));
        m_imageFailures += saved ? 0u : 1u;
        return;
    }
    const String& referencePath = FileExists(own.AsView()) ? own : shared;
    image::Image reference;
    if (!FileExists(referencePath.AsView()) || !image::io::LoadImage(referencePath.AsView(), reference).IsOk())
    {
        std::printf("GUISandbox: image %s: FAIL, no reference (look at %s, then --approve-images)\n",
                    reinterpret_cast<const char*>(name.CStr()), reinterpret_cast<const char*>(picturePath.CStr()));
        ++m_imageFailures;
        return;
    }
    reference = reference.ConvertFormat(image::PixelFormat::RGBA8);
    image::Image diff;
    const gui::testing::ImageComparison result = gui::testing::CompareImages(picture, reference, 8, 0.001, &diff);
    if (result.Matches)
    {
        std::printf("GUISandbox: image %s: ok (%llu pixels past 8, largest difference %u)\n", reinterpret_cast<const char*>(name.CStr()),
                    static_cast<unsigned long long>(result.Differing), result.LargestDelta);
        return;
    }
    ++m_imageFailures;
    const String diffPath = PathJoin(m_imageOut.AsView(), Format(u8"{}.diff.png", name).AsView());
    if (result.SameSize)
        (void)image::io::SaveImage(diff, diffPath.AsView(), image::io::ImageFileFormat::PNG);
    std::printf("GUISandbox: image %s: FAIL against %s: %s%llu of %llu pixels past 8 (largest %u); see %s\n",
                reinterpret_cast<const char*>(name.CStr()), reinterpret_cast<const char*>(referencePath.CStr()),
                result.SameSize ? "" : "not the same size; ", static_cast<unsigned long long>(result.Differing),
                static_cast<unsigned long long>(result.Pixels), result.LargestDelta, reinterpret_cast<const char*>(diffPath.CStr()));
}

// One picture of `root`, drawn into the offscreen target, read back and written to `path`.
bool GUISandbox::RenderImage(gui::SceneRoot& root, StringView path, image::Image& out)
{
    m_vg->Clear();
    {
        gui::DrawContext dc{*m_vg};
        root.DrawFrame(dc);
    }
    vg::VGBatch& batch = m_vg->GetBatch();
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    m_renderer.BeginFrame(static_cast<i32>(m_frameIndex));
    const vg::renderer::VGRenderSlice slice = m_renderer.Prepare(batch, static_cast<i32>(m_frameIndex), kImageWidth, kImageHeight);
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != ErrorCode::Ok || enc == nullptr)
        return false;
    enc->TransitionTexture(m_imageTarget, rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget);
    m_renderer.RenderLayers(*enc, static_cast<i32>(m_frameIndex), slice);
    const rhi::ClearColor clear = rhi::ClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    rhi::RenderPassDesc rpd{};
    if (m_imageTargets.Matches(kImageWidth, kImageHeight))
        rpd = m_imageTargets.BeginPass(*enc, m_imageView, clear);
    else
    {
        rhi::ColorAttachment ca{};
        ca.view = m_imageView;
        ca.loadOp = rhi::LoadOp::Clear;
        ca.storeOp = rhi::StoreOp::Store;
        ca.clearValue = clear;
        rpd.colorAttachments.Add(ca);
    }
    rhi::RenderPassEncoder* rp = enc->BeginRenderPass(rpd);
    m_renderer.Render(*rp, kImageWidth, kImageHeight, static_cast<i32>(m_frameIndex), slice);
    rp->End();
    m_imageCapture.Request(path);
    const bool recorded = m_imageCapture.Record(*m_device, *enc, m_imageTarget, m_swapChain->Format(), kImageWidth, kImageHeight);
    rhi::CommandBuffer* cb = enc->Finish();
    ++m_fenceVal;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_device->WaitIdle();
    m_pool->DestroyEncoder(enc);
    m_frameIndex = (m_frameIndex + 1) % kFrames;
    return recorded && m_imageCapture.Complete(*m_device, AppRoot(), out).IsOk();
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
    if (m_device != nullptr)
    {
        m_device->WaitIdle();
        if (m_imageView != nullptr)
            m_device->DestroyTextureView(m_imageView);
        if (m_imageTarget != nullptr)
            m_device->DestroyTexture(m_imageTarget);
        m_imageCapture.Release(*m_device); // its readback buffer
    }
    m_imageTargets.Destroy();
    m_inspectorSurface.Reset();
    m_inspectorBridge.Reset();
    m_inspector.Reset(); // its root on the GUI's context, before the context
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
    // (pointer steps too, in logical units: click:x,y rclick:x,y move:x,y down:x,y up:x,y, or at
    // what a selector or a text finds: click:#ok, click:text=Show, move:text~=Sett)
    // ("down down return", "pad-east"). --always-redraw: draw every frame. --frame-stats: print,
    // each second, how many frames drew. --sweep: hover, click and right-click every widget of every
    // page, then open the game menu, printing each step, and exit (a smoke test; a crash's last line
    // names its widget). --scroll <px>: scroll the page down that far once it is up.
    // --layout-overlay: start with the layout overlay on (F9 toggles it). --input-overlay: start with
    // the input overlay on (F8 toggles it: focus path, tab order, navigation, pointer, redraws).
    // --inspector: start with the inspector's pane open (Ctrl+Shift+I toggles it). --inspect <selector>:
    // open it with the first widget the selector matches selected. --image-tests: draw each page
    // offscreen at 960x720 on a fixed clock and compare it with its reference (Data/Tests/GUI/
    // Images; the pictures go to --image-out <dir>, build/gui-images by default; exit 1 on a miss,
    // 77 with no device); --approve-images [backend]: write them as the references instead, after
    // looking at them. --lang <locale>: start in that language (en, ar, or pseudo: English
    // pseudolocalized). --narrate: print what a screen reader would say (each focus change and
    // announcement). --stress <n>: start on
    // the Stress page, built at load n (1 to 20; measure in RelWithDebInfo). With the
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
        else if (arg == StringView(u8"--soak") && i + 1 < argc)
            app.SetSoak(static_cast<f32>(std::atof(argv[++i])));
        else if (arg == StringView(u8"--soak-csv") && i + 1 < argc)
            app.SetSoakCsv(next());
        else if (arg == StringView(u8"--layout-overlay"))
            app.SetLayoutOverlay(true);
        else if (arg == StringView(u8"--input-overlay"))
            app.SetInputOverlay(true);
        else if (arg == StringView(u8"--inspector"))
            app.SetOpenInspector(true);
        else if (arg == StringView(u8"--image-tests"))
            app.SetImageTests(true);
        else if (arg == StringView(u8"--approve-images"))
            app.SetApproveImages(i + 1 < argc && StringView(reinterpret_cast<const char8_t*>(argv[i + 1])) == StringView(u8"backend") ? next() : StringView(u8"shared"));
        else if (arg == StringView(u8"--image-out") && i + 1 < argc)
            app.SetImageOut(next());
        else if (arg == StringView(u8"--webgpu") || arg == StringView(u8"--wgpu"))
            app.SetBackendName(u8"webgpu");
        else if (arg == StringView(u8"--dx12") || arg == StringView(u8"--d3d12"))
            app.SetBackendName(u8"dx12");
        else if (arg == StringView(u8"--inspect") && i + 1 < argc)
            app.SetInspectSelector(next());
        else if (arg == StringView(u8"--lang") && i + 1 < argc)
            app.SetStartLanguage(next());
        else if (arg == StringView(u8"--narrate"))
            app.SetNarrate(true);
        else if (arg == StringView(u8"--stress") && i + 1 < argc)
            app.SetStress(std::atoi(reinterpret_cast<const char*>(argv[++i])));
        else if (arg == StringView(u8"--scroll") && i + 1 < argc)
            app.SetScroll(static_cast<f32>(std::atof(reinterpret_cast<const char*>(argv[++i]))));
    }
    return app.ImageExitCode(app.Run(argc, argv));
}

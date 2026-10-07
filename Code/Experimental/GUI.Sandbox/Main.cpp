// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI Sandbox - the first on-screen test of experimental.gui. Builds a small widget tree
// (panel + labels + buttons in a LinearLayout), styles it with CSS via a StyleManager, and
// renders it through the same VG -> VGRenderer -> RHI path as VGSandbox. Input is driven by
// an InputSurface (fullscreen, gated by an InputRouter) polled into the GUI EventDispatcher
// through GuiInputBridge, so hover/click/CSS-transitions are live.

#include <new>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system; // ShaderSystemHost
import samples.framework;
import foundation.shell;
import foundation.image;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.vg;
import foundation.vg.renderer;
import experimental.gui;
import experimental.gui.shell;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
namespace shell = foundation::shell;
namespace image = foundation::image;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;
namespace gui = experimental::gui;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}




namespace
{
    // VG shaders ship in the engine corpus (vg.vs/vg.ps); resolved via ShaderSystemHost in OnInit.
    // The demo font is the engine's built-in Roboto under the data root.
    constexpr StringView kFontAsset = u8"Assets/fonts/roboto/Roboto-Regular.ttf";

    // App-specific rules layered on top of the built-in theme: two button accent classes, a
    // font-family rule resolved through the font service, and a @keyframes pulse animation.
    const char8_t* kThemeExtras = u8R"(
        .danger { background-color: #b5453f; } /* class beats tag -> red */
        .accent { background-color: #3fa06a; } /*                  -> green */
        .highlight { color: #55d67f; }          /* survives per-frame theme re-apply */
        button  { font-family: Roboto; font-size: 18; border-radius: 6; } /* rounded + font via service */
        textfield { border-radius: 4; border: 1 solid #5a6474; } /* rounded + 1px border */
        .accent { border: 2 solid #7fe0a8; }    /* border shorthand demo (green button gets an outline) */
        @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.25; } 100% { opacity: 1; } }
        @keyframes blink { 0% { opacity: 1; } 40% { opacity: 0.08; } 60% { opacity: 0.08; } 100% { opacity: 1; } }
        @keyframes colorcycle {
            0%   { background-color: #e05555; }
            33%  { background-color: #55c07a; }
            66%  { background-color: #5580e0; }
            100% { background-color: #e05555; }
        }
        .pulse       { animation: pulse 1.4s infinite; }   /* opacity keyframes */
        .pulse-fast  { animation: pulse 0.7s infinite; }
        .pulse-slow  { animation: pulse 2.3s infinite; }
        .blink       { animation: blink 1.1s infinite; }
        .colorcycle  { animation: colorcycle 3s infinite; } /* background-color keyframes */
    )";

    inline Color Col(f32 r, f32 g, f32 b, f32 a = 1.0f) { return Color{r, g, b, a}; }

    // "Clicks: N" -> label (small non-negative values).
    void SetCounterText(gui::Label* label, i32 n)
    {
        char8_t buf[32] = u8"Clicks: ";
        usize pos = 8;
        char8_t digits[12];
        usize dc = 0;
        i32 v = n < 0 ? -n : n;
        if (v == 0)
            digits[dc++] = u8'0';
        while (v > 0)
        {
            digits[dc++] = static_cast<char8_t>(u8'0' + (v % 10));
            v /= 10;
        }
        if (n < 0)
            buf[pos++] = u8'-';
        for (usize k = 0; k < dc; ++k)
            buf[pos++] = digits[dc - 1 - k];
        buf[pos] = 0;
        label->SetText(StringView(buf));
    }

    // "N%" (0..1 -> 0..100) -> label.
    void SetPercentText(gui::Label* label, f32 value01)
    {
        const i32 pct = static_cast<i32>(value01 * 100.0f + 0.5f);
        char8_t buf[8];
        usize pos = 0;
        char8_t digits[4];
        usize dc = 0;
        i32 v = pct;
        if (v == 0)
            digits[dc++] = u8'0';
        while (v > 0)
        {
            digits[dc++] = static_cast<char8_t>(u8'0' + (v % 10));
            v /= 10;
        }
        for (usize k = 0; k < dc; ++k)
            buf[pos++] = digits[dc - 1 - k];
        buf[pos++] = u8'%';
        buf[pos] = 0;
        label->SetText(StringView(buf));
    }
}

class GUISandbox : public samples::framework::SampleApp
{
public:
    GUISandbox()
    {
        m_width = 900;
        m_height = 860;
    }
    StringView Title() const override { return u8"GUI Sandbox"; }
    u32 BufferCount() const override { return kFrames; }

protected:
    Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr u32 kFrames = 2;

    void BuildUI();
    void LoadFontSize(StringView path, f32 pixelHeight);
    [[nodiscard]] bool HasFonts() const { return FileExists(DataPath(kFontAsset).AsView()); }

    // Render plumbing (mirrors VGSandbox).
    shaders::ShaderSystemHost m_shaderHost{AppRoot()}; // owns the ShaderSystem + the VG modules
    rhi::ShaderModule* m_vs = nullptr;      // borrowed from m_shaderHost
    rhi::ShaderModule* m_fs = nullptr;
    rhi::ShaderModule* m_gradRadialFs = nullptr; // per-pixel radial gradient fragment shader
    rhi::ShaderModule* m_gradConicFs = nullptr;  // per-pixel conic gradient fragment shader
    rhi::ShaderModule* m_distanceFieldFragmentShader = nullptr;         // distance-field text fragment shader
    rhi::ShaderModule* m_boxShadowFs = nullptr; // blurred rounded rect fragment shader
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    u64 m_fenceVal = 0;
    u32 m_frameIndex = 0;

    UniquePtr<fonts::TrueTypeFontService> m_fontService;
    UniquePtr<vg::VGContext> m_vg;
    vg::renderer::VGRenderer m_renderer{AppRoot()};
    fonts::CachedFont* m_font = nullptr;
    fonts::CachedFont* m_fontLarge = nullptr;

    // GUI.
    RefPtr<gui::SceneNode> m_root;
    RefPtr<gui::LinearLayout> m_panel;
    RefPtr<gui::Label> m_counter;
    RefPtr<gui::TextField> m_textField;
    RefPtr<gui::Label> m_echo;
    RefPtr<gui::ScrollView> m_scroll;
    RefPtr<gui::ScrollView> m_pageScroll; // full-window scroller for the whole panel
    RefPtr<gui::RelativeLayout> m_relative;
    gui::StyleManager m_styles;
    UniquePtr<gui::GuiInputBridge> m_bridge;
    UniquePtr<gui::ShellClipboard> m_clipboard; // adapts the shell clipboard to gui::IClipboard
    UniquePtr<shell::InputSurface> m_inputSurface;
    UniquePtr<shell::InputRouter> m_router;
    i32 m_clicks = 0;

    // Priority-2 widget showcase (a floating Window with tabs + a right-click menu + tooltips).
    void BuildShowcaseWindow();
    void ConfirmQuit(); // modal MessageBox: confirm before exiting
    void ApplyTheme();  // (re)build the stylesheet from the current theme + extras
    bool m_darkTheme = true;
    RefPtr<gui::Window> m_widgetWindow;
    RefPtr<gui::Menu> m_contextMenu;
    RefPtr<gui::MenuBar> m_menuBar;
    RefPtr<gui::MessageBox> m_dialog;
    RefPtr<gui::ListView> m_listView;
    UniquePtr<gui::StringListModel> m_listModel;
    RefPtr<gui::TableView> m_tableView;
    UniquePtr<gui::TableModel> m_tableModel;
    UniquePtr<gui::SortingProxyModel> m_tableProxy;
    RefPtr<gui::TreeView> m_treeView;
    UniquePtr<gui::TreeModel> m_treeModel;
    RefPtr<gui::Label> m_pickEcho;
    gui::RadioGroup m_radioGroup;
    UniquePtr<gui::TooltipManager> m_tooltips;
    UniquePtr<image::OwnedImageData> m_showcaseImage;
    RefPtr<gui::ImageDrawable> m_showcaseDrawable;
};

Status GUISandbox::OnInit()
{
    // Resolve the VG shaders through the shared ShaderSystemHost over the data mount (cooked pack
    // or dev DXC over Shaders/) - the same cooked corpus (vg.vs/vg.ps) the runtime UI uses.
    if (!m_shaderHost.Initialize(*m_device, DataFileSystem()))
        return ErrorCode::Unknown;
    m_vs = m_shaderHost.GetVariant(u8"vg", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
    m_fs =
        m_shaderHost.GetVariant(u8"vg", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
    m_gradRadialFs = m_shaderHost.GetVariant(u8"vg_grad_radial", shaders::ShaderStage::Fragment,
                                             shaders::ShaderFlags::None);
    m_gradConicFs = m_shaderHost.GetVariant(u8"vg_grad_conic", shaders::ShaderStage::Fragment,
                                            shaders::ShaderFlags::None);
    m_distanceFieldFragmentShader = m_shaderHost.GetVariant(u8"vg_df", shaders::ShaderStage::Fragment,
                                     shaders::ShaderFlags::None);
    m_boxShadowFs = m_shaderHost.GetVariant(u8"vg_shadow", shaders::ShaderStage::Fragment,
                                            shaders::ShaderFlags::None);
    if (m_vs == nullptr || m_fs == nullptr || m_gradRadialFs == nullptr || m_gradConicFs == nullptr)
        return ErrorCode::Unknown;

    if (!m_renderer
             .Initialize(*m_device, *m_vs, *m_fs, m_swapChain->Format(), static_cast<i32>(kFrames),
                         m_distanceFieldFragmentShader, m_gradRadialFs, m_gradConicFs,
                         foundation::vg::renderer::VGTargetConfig{}, m_boxShadowFs)
             .IsOk())
        return ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) != ErrorCode::Ok)
        return ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != ErrorCode::Ok)
        return ErrorCode::Unknown;

    m_fontService = MakeUnique<fonts::TrueTypeFontService>(AppRoot(),
                                                           AppRoot());
    if (HasFonts())
    {
        const String fontPath = DataPath(kFontAsset);
        LoadFontSize(fontPath.AsView(), 18.0f);
        LoadFontSize(fontPath.AsView(), 30.0f);
        m_font = m_fontService->GetFont(u8"Roboto", 18.0f);
        m_fontLarge = m_fontService->GetFont(u8"Roboto", 30.0f);
    }

    m_vg = MakeUnique<vg::VGContext>(AppRoot(), m_fontService.Get());
    m_vg->SetPerPixelGradients(true); // renderer was given the radial/conic gradient shaders

    BuildUI();
    return ErrorCode::Ok;
}

void GUISandbox::LoadFontSize(StringView path, f32 pixelHeight)
{
    fonts::FontLoadOptions options = fonts::FontLoadOptions::ExtendedLatin();
    options.pixelHeight = pixelHeight;
    (void)m_fontService->LoadFont(u8"Roboto", path, options);
}

void GUISandbox::BuildUI()
{
    m_root = MakeRef<gui::SceneNode>(AppRoot());
    m_root->SetSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});

    // Full-window scroller so the (now tall) page can scroll when content runs off-screen.
    m_pageScroll = MakeRef<gui::ScrollView>(AppRoot());
    m_pageScroll->SetSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_root->AddChild(m_pageScroll.Get());

    // The panel stacks its children vertically and wraps its height to fit them (so the
    // scroller knows how tall the page is). Width stays the window width.
    m_panel = MakeRef<gui::LinearLayout>(AppRoot());
    m_panel->SetSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_panel->SetWrapContent(true);
    m_panel->SetPadding(gui::Thickness{28.0f});
    m_panel->SetSpacing(16.0f);
    m_panel->AddClass(
        StringView(u8"panel")); // themed surface (dark/light) instead of a hardcoded bg
    m_pageScroll->GetContent()->AddChild(m_panel.Get());

    // A menu bar across the top: File/Edit/View, each dropping a menu (with a submenu, a
    // separator, and a checkable item) below its button; hovering another button while one is
    // open switches to it.
    m_menuBar = MakeRef<gui::MenuBar>(AppRoot());
    m_menuBar->SetSize(Float2{600.0f, 28.0f});
    m_menuBar->SetFont(m_font);
    GUISandbox* menuSelf = this;
    {
        gui::Menu* file = m_menuBar->AddMenu(u8"File");
        file->AddItem(u8"Reset counter",
                      [menuSelf]()
                      {
                          menuSelf->m_clicks = 0;
                          SetCounterText(menuSelf->m_counter.Get(), 0);
                      });
        file->AddSeparator();
        file->AddItem(u8"Quit", [menuSelf]() { menuSelf->ConfirmQuit(); });

        gui::Menu* edit = m_menuBar->AddMenu(u8"Edit");
        edit->AddItem(u8"Add +1",
                      [menuSelf]()
                      {
                          ++menuSelf->m_clicks;
                          SetCounterText(menuSelf->m_counter.Get(), menuSelf->m_clicks);
                      });
        gui::Menu* by = edit->AddSubMenu(u8"Add by");
        by->AddItem(u8"+5",
                    [menuSelf]()
                    {
                        menuSelf->m_clicks += 5;
                        SetCounterText(menuSelf->m_counter.Get(), menuSelf->m_clicks);
                    });
        by->AddItem(u8"+10",
                    [menuSelf]()
                    {
                        menuSelf->m_clicks += 10;
                        SetCounterText(menuSelf->m_counter.Get(), menuSelf->m_clicks);
                    });

        gui::Menu* view = m_menuBar->AddMenu(u8"View");
        view->AddCheckItem(u8"Widgets window", true,
                           [menuSelf](bool on) { menuSelf->m_widgetWindow->SetVisible(on); });
    }
    m_panel->AddChild(m_menuBar.Get());

    auto title = MakeRef<gui::Label>(AppRoot());
    title->SetSize(Float2{600.0f, 46.0f});
    title->SetText(u8"GUI Sandbox");
    title->SetFont(m_fontLarge);
    title->SetTextColor(Col(0.92f, 0.94f, 0.98f));
    m_panel->AddChild(title.Get());

    m_counter = MakeRef<gui::Label>(AppRoot());
    m_counter->SetSize(Float2{500.0f, 28.0f});
    m_counter->SetText(u8"Clicks: 0");
    m_counter->SetFont(m_font);
    m_counter->SetTextColor(Col(0.75f, 0.82f, 0.9f));
    m_panel->AddChild(m_counter.Get());

    auto hint = MakeRef<gui::Label>(AppRoot());
    hint->SetSize(Float2{600.0f, 22.0f});
    hint->SetText(u8"Tip: right-click the empty background for a context menu.");
    hint->SetFont(m_font);
    hint->SetTextColor(Col(0.55f, 0.60f, 0.68f));
    hint->SetHitTestVisible(false); // don't let it swallow right-clicks meant for the panel
    m_panel->AddChild(hint.Get());

    // CSS @keyframes animation: a row of "activity light" dots pulsing/blinking at different
    // rates (all opacity-driven), plus a pulsing caption. Each animation is a CSS class.
    auto animRow = MakeRef<gui::LinearLayout>(AppRoot());
    animRow->SetOrientation(gui::Orientation::Horizontal);
    animRow->SetSize(Float2{600.0f, 24.0f});
    animRow->SetSpacing(10.0f);
    m_panel->AddChild(animRow.Get());

    struct Dot
    {
        const char8_t* cssClass;
        Color color;
    };
    const Dot dots[4] = {
        {u8"pulse-fast", Col(0.42f, 0.85f, 0.52f)}, // green, fast
        {u8"pulse", Col(0.36f, 0.66f, 0.95f)},      // blue, medium
        {u8"pulse-slow", Col(0.95f, 0.78f, 0.35f)}, // amber, slow
        {u8"blink", Col(0.90f, 0.42f, 0.42f)},      // red, blink
    };
    for (const Dot& d : dots)
    {
        auto dot = MakeRef<gui::Label>(AppRoot());
        dot->SetSize(Float2{18.0f, 18.0f});
        dot->SetBackground(MakeRef<gui::RectangleDrawable>(AppRoot(), d.color));
        dot->AddClass(StringView(d.cssClass));
        animRow->AddChild(dot.Get());
    }

    // A color-cycling box (background-color keyframes, not opacity).
    auto colorBox = MakeRef<gui::Label>(AppRoot());
    colorBox->SetSize(Float2{34.0f, 18.0f});
    colorBox->AddClass(StringView(u8"colorcycle"));
    animRow->AddChild(colorBox.Get());

    auto pulseCaption = MakeRef<gui::Label>(AppRoot());
    pulseCaption->SetSize(Float2{430.0f, 22.0f});
    pulseCaption->SetText(u8"@keyframes: opacity (dots) + background-color (box), varied rates");
    pulseCaption->SetFont(m_font);
    pulseCaption->AddClass(StringView(u8"pulse-slow"));
    animRow->AddChild(pulseCaption.Get());

    // A row of buttons.
    auto row = MakeRef<gui::LinearLayout>(AppRoot());
    row->SetOrientation(gui::Orientation::Horizontal);
    row->SetSize(Float2{static_cast<f32>(m_width) - 56.0f, 50.0f});
    row->SetSpacing(14.0f);
    m_panel->AddChild(row.Get());

    // Three buttons that are genuinely different: distinct style classes (so CSS gives them
    // distinct colors - class beats tag) and distinct behaviors.
    gui::Label* counter = m_counter.Get();
    i32* clicks = &m_clicks;

    auto makeButton = [&](const char8_t* label, const char8_t* cssClass, Function<void()> onClick)
    {
        auto btn = MakeRef<gui::Button>(AppRoot());
        btn->SetSize(Float2{170.0f, 46.0f});
        btn->SetText(StringView(label));
        btn->SetFont(m_font);
        btn->SetTextColor(Col(1.0f, 1.0f, 1.0f));
        if (cssClass[0] != 0)
            btn->AddClass(StringView(cssClass));
        btn->SetOnClick(Move(onClick));
        row->AddChild(btn.Get());
    };

    makeButton(u8"Add +1", u8"",
               [counter, clicks]()
               {
                   ++(*clicks);
                   SetCounterText(counter, *clicks);
               });
    makeButton(u8"Reset", u8"danger",
               [counter, clicks]()
               {
                   *clicks = 0;
                   SetCounterText(counter, 0);
               });
    makeButton(u8"Add +5", u8"accent",
               [counter, clicks]()
               {
                   *clicks += 5;
                   SetCounterText(counter, *clicks);
               });
    makeButton(u8"Theme", u8"",
               [this]()
               {
                   m_darkTheme = !m_darkTheme;
                   ApplyTheme();
               }); // dark <-> light

    // A checkbox + label row: toggling it highlights the counter.
    auto checkRow = MakeRef<gui::LinearLayout>(AppRoot());
    checkRow->SetOrientation(gui::Orientation::Horizontal);
    checkRow->SetSize(Float2{360.0f, 30.0f});
    checkRow->SetSpacing(10.0f);
    m_panel->AddChild(checkRow.Get());

    auto check = MakeRef<gui::CheckBox>(AppRoot());
    check->SetSize(Float2{22.0f, 22.0f});
    check->SetOnCheckedChanged(
        [counter](bool on)
        {
            // Drive the highlight via a CSS class so it survives the per-frame theme re-apply.
            if (on)
                counter->AddClass(StringView(u8"highlight"));
            else
                counter->RemoveClass(StringView(u8"highlight"));
        });
    checkRow->AddChild(check.Get());

    auto checkLabel = MakeRef<gui::Label>(AppRoot());
    checkLabel->SetSize(Float2{220.0f, 26.0f});
    checkLabel->SetText(u8"Highlight counter");
    checkLabel->SetFont(m_font);
    checkLabel->SetTextColor(Col(0.8f, 0.85f, 0.9f));
    checkRow->AddChild(checkLabel.Get());

    // A slider + live percent label.
    auto sliderRow = MakeRef<gui::LinearLayout>(AppRoot());
    sliderRow->SetOrientation(gui::Orientation::Horizontal);
    sliderRow->SetSize(Float2{380.0f, 30.0f});
    sliderRow->SetSpacing(12.0f);
    m_panel->AddChild(sliderRow.Get());

    auto slider = MakeRef<gui::Slider>(AppRoot());
    slider->SetSize(Float2{220.0f, 24.0f});
    sliderRow->AddChild(slider.Get());

    auto sliderLabel = MakeRef<gui::Label>(AppRoot());
    sliderLabel->SetSize(Float2{80.0f, 26.0f});
    sliderLabel->SetFont(m_font);
    sliderLabel->SetTextColor(Col(0.8f, 0.85f, 0.9f));
    sliderRow->AddChild(sliderLabel.Get());

    // A progress bar driven by the slider (so they move together).
    auto bar = MakeRef<gui::ProgressBar>(AppRoot());
    bar->SetSize(Float2{340.0f, 14.0f});
    m_panel->AddChild(bar.Get());

    gui::Label* pct = sliderLabel.Get();
    gui::ProgressBar* barPtr = bar.Get();
    slider->SetOnValueChanged(
        [pct, barPtr](f32 v)
        {
            SetPercentText(pct, v);
            barPtr->SetProgress(v);
        });
    slider->SetValue(0.5f); // fires the callback -> label "50%", bar half-filled

    // An editable text field (the keyboard/text path) + a live echo label.
    auto fieldRow = MakeRef<gui::LinearLayout>(AppRoot());
    fieldRow->SetOrientation(gui::Orientation::Horizontal);
    fieldRow->SetSize(Float2{560.0f, 40.0f});
    fieldRow->SetSpacing(12.0f);
    m_panel->AddChild(fieldRow.Get());

    auto fieldPrompt = MakeRef<gui::Label>(AppRoot());
    fieldPrompt->SetSize(Float2{60.0f, 34.0f});
    fieldPrompt->SetText(u8"Name:");
    fieldPrompt->SetFont(m_font);
    fieldPrompt->SetTextColor(Col(0.8f, 0.85f, 0.9f));
    fieldRow->AddChild(fieldPrompt.Get());

    m_textField = MakeRef<gui::TextField>(AppRoot());
    m_textField->SetSize(Float2{300.0f, 34.0f});
    m_textField->SetPadding(gui::Thickness{8.0f, 6.0f, 8.0f, 6.0f});
    m_textField->SetFont(m_font);
    m_textField->SetTextColor(Col(0.96f, 0.97f, 0.99f));
    m_textField->SetBackground(
        MakeRef<gui::RectangleDrawable>(AppRoot(), Col(0.20f, 0.22f, 0.27f)));
    // Placeholder shows while empty; select (shift+arrows / drag / double-click word),
    // Ctrl+A/C/X/V for select-all/copy/cut/paste; capped at 24 codepoints.
    m_textField->SetPlaceholder(u8"type here - select, Ctrl+C/V...");
    m_textField->SetMaxLength(24);
    fieldRow->AddChild(m_textField.Get());

    m_echo = MakeRef<gui::Label>(AppRoot());
    m_echo->SetSize(Float2{500.0f, 26.0f});
    m_echo->SetFont(m_font);
    m_echo->SetTextColor(Col(0.62f, 0.72f, 0.82f));
    m_echo->SetText(u8"echo: (empty)");
    m_panel->AddChild(m_echo.Get());

    gui::Label* echo = m_echo.Get();
    m_textField->SetOnTextChanged(
        [echo](StringView v)
        {
            char8_t buf[64] = u8"echo: ";
            usize pos = 6;
            for (usize i = 0; i < v.Size() && pos < 62; ++i)
                buf[pos++] = v[i];
            buf[pos] = 0;
            echo->SetText(StringView(buf));
        });

    // A word-wrapped multi-line paragraph (fixed width, SetWordWrap breaks at whitespace).
    auto wrapLabel = MakeRef<gui::Label>(AppRoot());
    wrapLabel->SetSize(Float2{560.0f, 66.0f});
    wrapLabel->SetFont(m_font);
    wrapLabel->SetTextColor(Col(0.78f, 0.82f, 0.88f));
    wrapLabel->SetTextAlignment(gui::TextHAlign::Left, gui::TextVAlign::Top);
    wrapLabel->SetWordWrap(true);
    wrapLabel->SetText(
        u8"Word wrap: this paragraph is a single Label with SetWordWrap(true), so "
        u8"the text greedily breaks at whitespace to fit the label's width and flows "
        u8"onto multiple lines - explicit newlines break too.");
    m_panel->AddChild(wrapLabel.Get());

    // A virtualized, model-backed ListView: 500 rows but only the visible handful are realized.
    auto listCaption = MakeRef<gui::Label>(AppRoot());
    listCaption->SetSize(Float2{560.0f, 22.0f});
    listCaption->SetFont(m_font);
    listCaption->SetTextColor(Col(0.78f, 0.82f, 0.88f));
    listCaption->SetText(u8"ListView (MVC, virtualized 500 rows) - click / arrow keys:");
    m_panel->AddChild(listCaption.Get());

    m_listModel = MakeUnique<gui::StringListModel>(AppRoot());
    {
        Array<String> rows;
        for (i32 i = 0; i < 500; ++i)
        {
            char8_t buf[24] = u8"Row ";
            usize pos = 4;
            char8_t digits[12];
            usize dc = 0;
            i32 v = i;
            if (v == 0)
                digits[dc++] = u8'0';
            while (v > 0)
            {
                digits[dc++] = static_cast<char8_t>(u8'0' + (v % 10));
                v /= 10;
            }
            while (dc > 0)
                buf[pos++] = digits[--dc];
            buf[pos] = 0;
            rows.PushBack(String(StringView(buf)));
        }
        m_listModel->SetItems(Move(rows));
    }

    m_listView = MakeRef<gui::ListView>(AppRoot());
    m_listView->SetSize(Float2{260.0f, 160.0f});
    m_listView->SetFont(m_font);
    m_listView->SetRowHeight(24.0f);
    m_listView->SetModel(m_listModel.Get());
    gui::Label* echoRef = m_echo.Get();
    gui::StringListModel* modelRef = m_listModel.Get();
    m_listView->SetOnSelectionChanged(
        [echoRef, modelRef](gui::ModelIndex idx)
        {
            if (!idx.IsValid())
                return;
            char8_t buf[64] = u8"picked: ";
            usize pos = 8;
            const StringView picked = modelRef->ItemAt(static_cast<usize>(idx.Row));
            for (usize i = 0; i < picked.Size() && pos < 62; ++i)
                buf[pos++] = picked[i];
            buf[pos] = 0;
            echoRef->SetText(StringView(buf));
        });
    m_panel->AddChild(m_listView.Get());

    // A sortable TableView: a TableModel behind a SortingProxyModel; clicking a column header
    // sorts by that column (numeric columns sort by value). No view changes needed - the header
    // click just calls the proxy, and the table (a client of the proxy) refreshes.
    auto tableCaption = MakeRef<gui::Label>(AppRoot());
    tableCaption->SetSize(Float2{560.0f, 22.0f});
    tableCaption->SetFont(m_font);
    tableCaption->SetTextColor(Col(0.78f, 0.82f, 0.88f));
    tableCaption->SetText(u8"TableView (MVC) - click a column header to sort:");
    m_panel->AddChild(tableCaption.Get());

    m_tableModel = MakeUnique<gui::TableModel>(AppRoot());
    {
        Array<String> cols;
        cols.PushBack(String(u8"Name"));
        cols.PushBack(String(u8"Score"));
        m_tableModel->SetColumns(Move(cols));
        const char8_t* names[6] = {u8"Ivy", u8"Cara", u8"Bo", u8"Ada", u8"Eve", u8"Dan"};
        const i32 scores[6] = {42, 7, 91, 15, 68, 30};
        for (usize i = 0; i < 6; ++i)
        {
            Array<gui::Variant> cells;
            cells.PushBack(gui::Variant(StringView(names[i])));
            cells.PushBack(gui::Variant(static_cast<i64>(scores[i])));
            m_tableModel->AddRow(Move(cells));
        }
    }
    m_tableProxy = MakeUnique<gui::SortingProxyModel>(AppRoot(), m_tableModel.Get());

    m_tableView = MakeRef<gui::TableView>(AppRoot());
    m_tableView->SetSize(Float2{300.0f, 180.0f});
    m_tableView->SetFont(m_font);
    m_tableView->SetRowHeight(24.0f);
    m_tableView->SetModel(m_tableProxy.Get());
    gui::SortingProxyModel* proxyRef = m_tableProxy.Get();
    m_tableView->SetOnColumnHeaderClicked([proxyRef](usize col) { proxyRef->ToggleSort(col); });
    m_panel->AddChild(m_tableView.Get());

    // A TreeView over a small hierarchical TreeModel - click the arrows (or use arrow keys) to
    // expand/collapse; only the visible nodes are realized.
    auto treeCaption = MakeRef<gui::Label>(AppRoot());
    treeCaption->SetSize(Float2{560.0f, 22.0f});
    treeCaption->SetFont(m_font);
    treeCaption->SetTextColor(Col(0.78f, 0.82f, 0.88f));
    treeCaption->SetText(u8"TreeView (MVC) - click arrows / arrow keys to expand:");
    m_panel->AddChild(treeCaption.Get());

    m_treeModel = MakeUnique<gui::TreeModel>(AppRoot());
    {
        const i32 src = m_treeModel->AddNode(gui::TreeModel::kRoot, u8"src");
        const i32 engine = m_treeModel->AddNode(src, u8"Engine");
        m_treeModel->AddNode(engine, u8"GUI");
        m_treeModel->AddNode(engine, u8"Core");
        m_treeModel->AddNode(engine, u8"Shell");
        const i32 samples = m_treeModel->AddNode(src, u8"Samples");
        m_treeModel->AddNode(samples, u8"GUISandbox");
        const i32 docs = m_treeModel->AddNode(gui::TreeModel::kRoot, u8"docs");
        m_treeModel->AddNode(docs, u8"design");
    }

    m_treeView = MakeRef<gui::TreeView>(AppRoot());
    m_treeView->SetSize(Float2{300.0f, 180.0f});
    m_treeView->SetFont(m_font);
    m_treeView->SetRowHeight(24.0f);
    m_treeView->SetModel(m_treeModel.Get());
    m_treeView->ExpandItem(0); // expand "src" so there's something to see
    m_panel->AddChild(m_treeView.Get());

    // A row built declaratively from XML markup (element -> widget, attributes -> text /
    // structural props / inline CSS through the same StyleApplier the stylesheet uses).
    {
        static const char8_t* kMarkup = u8R"(
            <LinearLayout orientation="horizontal" spacing="10" width="420" height="34">
                <Label text="Loaded from XML markup:" font-family="Roboto" font-size="16"
                       color="#cfd6e0" width="180" height="30" vertical-align="middle"/>
                <Button text="Alpha" class="accent" font-family="Roboto" font-size="16" width="90" height="30"/>
                <Button text="Beta"  class="danger" font-family="Roboto" font-size="16" width="90" height="30"/>
            </LinearLayout>
        )";
        gui::WidgetFactory factory = gui::DefaultWidgetFactory();
        gui::MarkupLoader loader(factory, nullptr, m_fontService.Get());
        if (RefPtr<gui::Node> markupRoot = loader.LoadFromString(StringView(kMarkup)))
            m_panel->AddChild(markupRoot.Get());
    }

    // A FlexLayout (markup) spreading three buttons across the row via justify-content.
    {
        auto flexCaption = MakeRef<gui::Label>(AppRoot());
        flexCaption->SetSize(Float2{560.0f, 22.0f});
        flexCaption->SetFont(m_font);
        flexCaption->SetTextColor(Col(0.78f, 0.82f, 0.88f));
        flexCaption->SetText(u8"FlexLayout (justify-content: space-between):");
        m_panel->AddChild(flexCaption.Get());

        static const char8_t* kFlex = u8R"(
            <FlexLayout direction="row" justify-content="space-between" align-items="center" width="420" height="34">
                <Button text="One"   font-family="Roboto" font-size="16" width="90" height="28"/>
                <Button text="Two"   font-family="Roboto" font-size="16" width="90" height="28"/>
                <Button text="Three" font-family="Roboto" font-size="16" width="90" height="28"/>
            </FlexLayout>
        )";
        gui::WidgetFactory factory = gui::DefaultWidgetFactory();
        gui::MarkupLoader loader(factory, nullptr, m_fontService.Get());
        if (RefPtr<gui::Node> flexRoot = loader.LoadFromString(StringView(kFlex)))
            m_panel->AddChild(flexRoot.Get());
    }

    // A scrollable grid: GridLayout of numbered cells inside a ScrollView. Scroll it with the
    // mouse wheel, by dragging the auto-managed scrollbar, or by clicking it (Tab-focus) and
    // using the arrow / PageUp-Down / Home-End keys.
    auto scrollLabel = MakeRef<gui::Label>(AppRoot());
    scrollLabel->SetSize(Float2{600.0f, 24.0f});
    scrollLabel->SetText(u8"Scrollable grid - wheel, drag the bar, or focus + arrow keys:");
    scrollLabel->SetFont(m_font);
    scrollLabel->SetTextColor(Col(0.8f, 0.85f, 0.9f));
    m_panel->AddChild(scrollLabel.Get());

    m_scroll = MakeRef<gui::ScrollView>(AppRoot());
    m_scroll->SetSize(Float2{360.0f, 150.0f});
    m_scroll->SetPadding(gui::Thickness{6.0f});
    m_scroll->SetBackground(
        MakeRef<gui::RectangleDrawable>(AppRoot(), Col(0.08f, 0.09f, 0.11f)));
    m_panel->AddChild(m_scroll.Get());

    auto grid = MakeRef<gui::GridLayout>(AppRoot());
    grid->SetColumns(3);
    grid->SetSpacing(8.0f, 8.0f);
    grid->SetSize(Float2{336.0f, 7.0f * 48.0f - 8.0f}); // 7 rows of 40px cells + 8px gaps
    m_scroll->GetContent()->AddChild(grid.Get());

    for (i32 i = 0; i < 21; ++i)
    {
        auto cell = MakeRef<gui::Label>(AppRoot());
        cell->SetSize(Float2{100.0f, 40.0f});
        const f32 t = static_cast<f32>(i) / 20.0f;
        cell->SetBackground(MakeRef<gui::RectangleDrawable>(
            AppRoot(), Col(0.20f + 0.30f * t, 0.35f, 0.55f - 0.25f * t)));
        cell->SetFont(m_font);
        cell->SetTextColor(Col(0.96f, 0.97f, 0.99f));
        cell->SetTextAlignment(gui::TextHAlign::Center, gui::TextVAlign::Middle);
        char8_t buf[8] = u8"#";
        usize p = 1;
        const i32 n = i + 1;
        if (n >= 10)
            buf[p++] = static_cast<char8_t>(u8'0' + (n / 10));
        buf[p++] = static_cast<char8_t>(u8'0' + (n % 10));
        buf[p] = 0;
        cell->SetText(StringView(buf));
        grid->AddChild(cell.Get());
    }
    m_scroll->SetAutoMeasureContent(true); // content sizes to the grid -> vertical bar appears

    // A RelativeLayout: five labels pinned to the corners and centre of a box.
    auto relLabel = MakeRef<gui::Label>(AppRoot());
    relLabel->SetSize(Float2{600.0f, 24.0f});
    relLabel->SetText(u8"RelativeLayout - anchored to corners + centre:");
    relLabel->SetFont(m_font);
    relLabel->SetTextColor(Col(0.8f, 0.85f, 0.9f));
    m_panel->AddChild(relLabel.Get());

    m_relative = MakeRef<gui::RelativeLayout>(AppRoot());
    m_relative->SetSize(Float2{360.0f, 90.0f});
    m_relative->AddClass(StringView(u8"panel")); // themed surface
    m_panel->AddChild(m_relative.Get());

    struct Pin
    {
        const char8_t* text;
        u32 anchor;
    };
    const Pin pins[5] = {
        {u8"TL", gui::AnchorLeft | gui::AnchorTop},
        {u8"TR", gui::AnchorRight | gui::AnchorTop},
        {u8"BL", gui::AnchorLeft | gui::AnchorBottom},
        {u8"BR", gui::AnchorRight | gui::AnchorBottom},
        {u8"middle", gui::AnchorCenter},
    };
    for (const Pin& pin : pins)
    {
        auto tag = MakeRef<gui::Label>(AppRoot());
        tag->SetSize(Float2{70.0f, 26.0f});
        tag->SetText(StringView(pin.text));
        tag->SetFont(m_font);
        tag->SetTextColor(Col(0.85f, 0.9f, 0.95f));
        tag->SetTextAlignment(gui::TextHAlign::Center, gui::TextVAlign::Middle);
        tag->SetBackground(
            MakeRef<gui::RectangleDrawable>(AppRoot(), Col(0.25f, 0.40f, 0.30f)));
        m_relative->AddChild(tag.Get());
        m_relative->SetAnchor(tag.Get(), pin.anchor);
    }

    BuildShowcaseWindow();

    // Theme: the built-in default theme drives the widget look; the font service resolves the
    // font-family rule (the GUI is agnostic to how it loads fonts). A toggle button (added in
    // BuildShowcaseWindow) flips dark/light. No image resource provider (no url() in the theme).
    m_styles.SetFontService(m_fontService.Get());
    ApplyTheme();

    m_bridge = MakeUnique<gui::GuiInputBridge>(AppRoot(), m_root->GetEventDispatcher());

    // Give the dispatcher a clipboard so TextField cut/copy/paste (Ctrl+X/C/V) reach the OS
    // clipboard - the same seam headless tests exercise with an in-memory one.
    if (m_shell != nullptr)
    {
        m_clipboard = MakeUnique<gui::ShellClipboard>(AppRoot(), m_shell);
        m_root->GetEventDispatcher()->SetClipboard(m_clipboard.Get());
    }
}

void GUISandbox::ConfirmQuit()
{
    if (!m_dialog)
    {
        m_dialog = MakeRef<gui::MessageBox>(AppRoot());
        m_dialog->SetFont(m_font);
    }
    m_dialog->Configure(u8"Quit?", u8"Close the GUI sandbox? Any unsaved changes will be lost.",
                        gui::MessageBox::Buttons::YesNo);
    GUISandbox* self = this;
    m_dialog->SetOnResult(
        [self](gui::MessageBox::Result r)
        {
            if (r == gui::MessageBox::Result::Yes)
                self->m_shell->RequestExit();
        });
    m_dialog->OpenModal(*m_root.Get());
}

void GUISandbox::ApplyTheme()
{
    // Combine the built-in theme with the app extras and hot-reload the manager's sheet.
    const StringView theme = m_darkTheme ? gui::DefaultDarkThemeCSS() : gui::DefaultLightThemeCSS();
    String combined(theme);
    combined += StringView(kThemeExtras);
    m_styles.SetStyleSheet(gui::CSSParser::Parse(combined.AsView()));
}

void GUISandbox::BuildShowcaseWindow()
{
    // A floating Window (drag its title bar, resize from the bottom-right grip) hosting a
    // TabWidget that surfaces the Priority-2 controls: ComboBox, ListBox, Radios, Image.
    m_widgetWindow = MakeRef<gui::Window>(AppRoot());
    m_widgetWindow->SetSize(Float2{340.0f, 320.0f});
    m_widgetWindow->SetPosition(Float2{470.0f, 150.0f});
    m_widgetWindow->SetTitle(u8"Widgets  (drag / resize me)");
    m_widgetWindow->SetFont(m_font);
    m_root->AddChild(m_widgetWindow.Get());

    auto tabs = MakeRef<gui::TabWidget>(AppRoot());
    tabs->SetFont(m_font);
    tabs->SetTabWidth(80.0f);
    tabs->SetTabBarHeight(28.0f);
    m_widgetWindow->GetContent()->AddChild(tabs.Get());
    // The content host sizes tabs to fill it; make it fill the window body.
    tabs->SetSize(m_widgetWindow->GetContent()->GetSize());

    // --- Tab 1: a ComboBox + a ListBox + an echo label ---
    auto pick = MakeRef<gui::LinearLayout>(AppRoot());
    pick->SetPadding(gui::Thickness{10.0f});
    pick->SetSpacing(8.0f);

    auto combo = MakeRef<gui::ComboBox>(AppRoot());
    combo->SetSize(Float2{200.0f, 26.0f});
    combo->SetFont(m_font);
    combo->SetTextColor(Col(0.92f, 0.94f, 0.98f));
    combo->SetTooltip(u8"Pick a fruit");
    const char8_t* fruits[5] = {u8"Apple", u8"Banana", u8"Cherry", u8"Date", u8"Elderberry"};
    for (const char8_t* f : fruits)
        combo->AddItem(StringView(f));
    pick->AddChild(combo.Get());

    auto list = MakeRef<gui::ListBox>(AppRoot());
    list->SetSize(Float2{260.0f, 140.0f});
    list->SetFont(m_font);
    for (i32 i = 1; i <= 14; ++i)
    {
        char8_t buf[16] = u8"Item ";
        usize p = 5;
        const i32 n = i;
        if (n >= 10)
            buf[p++] = static_cast<char8_t>(u8'0' + n / 10);
        buf[p++] = static_cast<char8_t>(u8'0' + n % 10);
        buf[p] = 0;
        list->AddItem(StringView(buf));
    }
    pick->AddChild(list.Get());

    m_pickEcho = MakeRef<gui::Label>(AppRoot());
    m_pickEcho->SetSize(Float2{260.0f, 24.0f});
    m_pickEcho->SetFont(m_font);
    m_pickEcho->SetTextColor(Col(0.62f, 0.82f, 0.68f));
    m_pickEcho->SetText(u8"selection -");
    pick->AddChild(m_pickEcho.Get());

    gui::Label* pickEcho = m_pickEcho.Get();
    gui::ComboBox* comboPtr = combo.Get();
    combo->SetOnSelectionChanged(
        [pickEcho, comboPtr](i32)
        {
            char8_t buf[48] = u8"fruit: ";
            usize pos = 7;
            const StringView t = comboPtr->GetSelectedText();
            for (usize i = 0; i < t.Size() && pos < 46; ++i)
                buf[pos++] = t[i];
            buf[pos] = 0;
            pickEcho->SetText(StringView(buf));
        });
    gui::ListBox* listPtr = list.Get();
    list->SetOnSelectionChanged(
        [pickEcho, listPtr](i32 idx)
        {
            char8_t buf[48] = u8"row: ";
            usize pos = 5;
            const StringView t = listPtr->GetItem(static_cast<usize>(idx));
            for (usize i = 0; i < t.Size() && pos < 46; ++i)
                buf[pos++] = t[i];
            buf[pos] = 0;
            pickEcho->SetText(StringView(buf));
        });

    // --- Tab 2: a radio group + a checkbox ---
    auto opts = MakeRef<gui::LinearLayout>(AppRoot());
    opts->SetPadding(gui::Thickness{10.0f});
    opts->SetSpacing(8.0f);

    const char8_t* choices[3] = {u8"Low", u8"Medium", u8"High"};
    for (const char8_t* c : choices)
    {
        auto row = MakeRef<gui::LinearLayout>(AppRoot());
        row->SetOrientation(gui::Orientation::Horizontal);
        row->SetSize(Float2{260.0f, 24.0f});
        row->SetSpacing(8.0f);

        auto radio = MakeRef<gui::RadioButton>(AppRoot());
        radio->SetSize(Float2{20.0f, 20.0f});
        m_radioGroup.Add(radio.Get());
        row->AddChild(radio.Get());

        auto lbl = MakeRef<gui::Label>(AppRoot());
        lbl->SetSize(Float2{200.0f, 22.0f});
        lbl->SetFont(m_font);
        lbl->SetTextColor(Col(0.85f, 0.88f, 0.92f));
        lbl->SetText(StringView(c));
        row->AddChild(lbl.Get());
        opts->AddChild(row.Get());
    }

    auto checkRow2 = MakeRef<gui::LinearLayout>(AppRoot());
    checkRow2->SetOrientation(gui::Orientation::Horizontal);
    checkRow2->SetSize(Float2{260.0f, 24.0f});
    checkRow2->SetSpacing(8.0f);
    auto check2 = MakeRef<gui::CheckBox>(AppRoot());
    check2->SetSize(Float2{20.0f, 20.0f});
    check2->SetTooltip(u8"A checkbox with a tooltip");
    checkRow2->AddChild(check2.Get());
    auto check2Label = MakeRef<gui::Label>(AppRoot());
    check2Label->SetSize(Float2{200.0f, 22.0f});
    check2Label->SetFont(m_font);
    check2Label->SetTextColor(Col(0.85f, 0.88f, 0.92f));
    check2Label->SetText(u8"Enable feature");
    checkRow2->AddChild(check2Label.Get());
    opts->AddChild(checkRow2.Get());

    // --- Tab 3: an Image (a generated checker) in Fit mode ---
    auto picTab = MakeRef<gui::UIWidget>(AppRoot());
    {
        // Build a 16x16 RGBA checker/gradient image the Image widget can display.
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
        m_showcaseImage =
            MakeUnique<image::OwnedImageData>(AppRoot(), 16, 16, image::PixelFormat::RGBA8,
                                              Span<const u8>(pixels, sizeof(pixels)));
        m_showcaseDrawable = MakeRef<gui::ImageDrawable>(AppRoot(), m_showcaseImage.Get());
    }
    auto imageView = MakeRef<gui::Image>(AppRoot());
    imageView->SetSize(Float2{260.0f, 200.0f});
    imageView->SetPadding(gui::Thickness{12.0f});
    imageView->SetDrawable(m_showcaseDrawable);
    imageView->SetScaleMode(gui::ImageScaleMode::Fit);
    imageView->SetTooltip(u8"An Image widget (Fit mode)");
    picTab->AddChild(imageView.Get());

    tabs->AddTab(u8"Pick", pick.Get());
    tabs->AddTab(u8"Opts", opts.Get());
    tabs->AddTab(u8"Pic", picTab.Get());

    // A right-click context menu on the main panel.
    m_contextMenu = MakeRef<gui::Menu>(AppRoot());
    m_contextMenu->SetFont(m_font);
    m_contextMenu->SetWidth(220.0f);
    GUISandbox* self = this;
    gui::MenuItem* addItem =
        m_contextMenu->AddItem(u8"Add +1 to counter",
                               [self]()
                               {
                                   ++self->m_clicks;
                                   SetCounterText(self->m_counter.Get(), self->m_clicks);
                               });
    addItem->SetShortcut(u8"Ctrl+A");
    m_contextMenu->AddItem(u8"Reset counter",
                           [self]()
                           {
                               self->m_clicks = 0;
                               SetCounterText(self->m_counter.Get(), 0);
                           });
    m_contextMenu->AddSeparator();
    // A checkable item: toggle the widgets window's visibility, reflecting its state.
    m_contextMenu->AddCheckItem(u8"Show widgets window", true,
                                [self](bool on) { self->m_widgetWindow->SetVisible(on); });
    // A submenu of counter presets.
    gui::Menu* presets = m_contextMenu->AddSubMenu(u8"Set counter to");
    presets->SetFont(m_font);
    presets->SetWidth(120.0f);
    const i32 presetValues[3] = {0, 10, 100};
    const StringView presetLabels[3] = {StringView(u8"0"), StringView(u8"10"), StringView(u8"100")};
    for (usize i = 0; i < 3; ++i)
    {
        const i32 value = presetValues[i];
        presets->AddItem(presetLabels[i],
                         [self, value]()
                         {
                             self->m_clicks = value;
                             SetCounterText(self->m_counter.Get(), value);
                         });
    }

    // Open the context menu on a right-click of the empty panel background (a node listener on
    // the panel, so right-clicking a widget or the floating window does NOT open it). The menu
    // has no persistent owner, so any click outside it dismisses; right-clicking another empty
    // spot dismisses the old one and the listener reopens it there.
    gui::Menu* menuPtr = m_contextMenu.Get();
    gui::LinearLayout* panelPtr = m_panel.Get();
    m_panel->AddEventListener(gui::EventType::MouseDown,
                              [menuPtr, panelPtr](const gui::Event& e)
                              {
                                  const gui::MouseEvent& me =
                                      static_cast<const gui::MouseEvent&>(e);
                                  if (me.Button == gui::MouseButton::Right)
                                      menuPtr->Open(*panelPtr, me.Position);
                              });

    // Tooltips: ticked each frame in OnRender.
    m_tooltips = MakeUnique<gui::TooltipManager>(AppRoot());
    m_tooltips->SetFont(m_font);
    m_tooltips->SetDelay(0.4);
}

void GUISandbox::OnRender()
{
    // Input: a fullscreen InputSurface, gated by a router, polled into the dispatcher.
    if (m_shell != nullptr && m_shell->Input() != nullptr && m_window != nullptr)
    {
        shell::IInputManager& input = *m_shell->Input();
        const Rectangle region{0.0f, 0.0f, static_cast<f32>(m_width), static_cast<f32>(m_height)};
        if (!m_inputSurface)
        {
            const ContentFit fit{region,
                                 Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)},
                                 FitMode::Stretch};
            m_inputSurface =
                MakeUnique<shell::InputSurface>(AppRoot(), &input, m_window->Id(), fit);
            m_router = MakeUnique<shell::InputRouter>(AppRoot(), &input);
            m_router->AddSurface(m_inputSurface.Get());
            m_bridge->SetTextInputTarget(m_window); // the bridge drives IME on/off from focus
        }
        m_inputSurface->SetRegion(region);
        m_inputSurface->SetContentSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
        m_router->Update();
        m_bridge->PumpFromSurface(*m_inputSurface);

        // Keyboard/text is NOT part of the mouse-only surface pump: dispatch the raw event
        // stream's key/text events through the bridge's event path (mouse events are skipped
        // here, since PumpFromSurface already handled them). Text-input enable/disable is
        // handled generically by the bridge (SetTextInputTarget below) from the focused
        // widget's WantsTextInput() - no per-widget wiring here.
        for (const shell::InputEvent& ev : input.Events())
        {
            switch (ev.kind)
            {
            case shell::InputEventKind::KeyDown:
            case shell::InputEventKind::KeyUp:
            case shell::InputEventKind::TextInput:
                m_bridge->Dispatch(ev);
                break;
            default:
                break;
            }
        }
    }

    // Blink the text field's caret while it holds focus.
    if (m_textField)
        m_textField->Update(static_cast<f64>(m_deltaTime));

    // Advance + style the tree. The panel keeps the window width but wrap-content owns its
    // height; the page scroller fills the window.
    m_root->SetSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_pageScroll->SetSize(Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)});
    m_panel->SetSize(Float2{static_cast<f32>(m_width), m_panel->GetSize().y});
    m_root->Update(Duration::FromSeconds(static_cast<f64>(m_deltaTime)));
    if (m_tooltips)
        m_tooltips->Update(*m_root->GetEventDispatcher(), *m_root.Get(),
                           static_cast<f64>(m_deltaTime));
    m_styles.ApplyTree(*m_root.Get());

    // Keep the scroller's content size in sync with the wrapped panel height (a hidden widget
    // or a size change makes the page taller/shorter).
    m_pageScroll->SetContentSize(m_panel->GetSize());

    // Draw the tree into the VG batch.
    m_vg->Clear();
    {
        gui::DrawContext dc{*m_vg};
        m_root->Draw(dc);
    }
    vg::VGBatch& batch = m_vg->GetBatch();

    // Present (same path as VGSandbox).
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != ErrorCode::Ok)
        return;

    m_renderer.BeginFrame(static_cast<i32>(m_frameIndex));
    const vg::renderer::VGRenderSlice slice =
        m_renderer.Prepare(batch, static_cast<i32>(m_frameIndex), m_width, m_height);

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != ErrorCode::Ok || enc == nullptr)
        return;

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);

    rhi::RenderPassEncoder* rp = enc->BeginRenderPass(rpd);
    m_renderer.Render(*rp, m_width, m_height, static_cast<i32>(m_frameIndex), slice);
    rp->End();

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);

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
    if (m_device)
        m_device->WaitIdle();
    m_router.Reset();
    m_inputSurface.Reset();
    m_bridge.Reset();
    m_root.Reset();
    m_counter.Reset();
    m_textField.Reset();
    m_echo.Reset();
    m_scroll.Reset();
    m_pageScroll.Reset();
    m_relative.Reset();
    m_tooltips.Reset();
    m_contextMenu.Reset();
    m_menuBar.Reset();
    m_dialog.Reset();
    m_listView.Reset(); // release before the model it references
    m_listModel.Reset();
    m_tableView.Reset(); // release before the proxy/model it references
    m_tableProxy.Reset();
    m_tableModel.Reset();
    m_treeView.Reset(); // release before its model
    m_treeModel.Reset();
    m_widgetWindow.Reset();
    m_pickEcho.Reset();
    m_showcaseDrawable.Reset();
    m_showcaseImage.Reset();
    m_panel.Reset();
    m_renderer.Dispose();
    m_vg.Reset();
    m_fontService.Reset();
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    // m_vs/m_fs are borrowed from m_shaderHost's ShaderSystem, which frees them.
    m_shaderHost.Shutdown();
}

int main(int argc, char** argv)
{
    GUISandbox app;
    return app.Run(argc, argv);
}

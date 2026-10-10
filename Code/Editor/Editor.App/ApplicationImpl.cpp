// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :application partition.
//
// EditorApplication: the editor as a runtime IApplication - the
// UISandbox wiring, assembled for real: TrueType font service + UIHost + RuntimeDockableWindowHost
// (floating panels = borderless OS windows, drag-follow Tick) + the EditorShell chrome on the main
// window, with the EditorContext + EditorProject from editor.core underneath. Opens (or
// scaffolds) the project directory on startup, restores the per-user dock layout, saves it on
// shutdown.

module;
#define _CRT_SECURE_NO_WARNINGS
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include <cstdlib>

module editor.app;

import foundation.core;
import foundation.shell;
import foundation.graphics;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.resource;
import foundation.fonts.distancefield.baker; // DistanceFieldFonts (MSDF baker registration) for the DF font path
import foundation.runtime;
import foundation.runtime.client;
import engine.defaultapp; // the embedded game application (v3) + ScreenshotCapture
import foundation.image;  // Image (the screenshot the capture hands back)
import engine.scene; // GlobalSceneContributionRecorder (game-native-code.md S1)
import foundation.scene;          // Scene (the reload scene bracket)
import foundation.scene.resource; // SceneSnapshot (the reload scene bracket)
import foundation.ui.resource;        // UITheme (the manifest's default game-UI theme)
import engine.ui;       // UISubsystem (SetDefaultTheme)
import engine.input;    // InputSubsystem (the embedded runtime's scene-input policy)
import foundation.render.api;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.application;
import foundation.content;
import foundation.vfs;
import foundation.resource;
import pipeline.core;
import editor.core;
import foundation.settings;
import editor.mcp; // EngineToolPaths + LocateShippingDocs (the host's composition)
import :assets_view;
import :mcp_host;
import :mcp_operations;
import :mcp_page_tools;
import :editor_icons;
import :settings_dialog;
import :preferences_dialog;
import :shell;
import :ui_page;

using namespace foundation::core;
using namespace pipeline;
namespace fonts = foundation::fonts;
namespace graphics = foundation::graphics;
namespace runtime = foundation::runtime;
namespace ui = foundation::ui;

// The build identity compiled into Runtime.Client (git short hash + generation time; the same string
// logged at startup). Shown as the version in the About dialog; resolved at final link.
extern "C" const char* BuildStamp();

namespace editor::app
{
    // Recursively: does the source DB hold any FontAsset? (Answers "the project HAS fonts but none is
    // the default" so the game-UI can warn actionably instead of silently rendering no text.)
    [[nodiscard]] static bool ProjectHasFontAssets(foundation::content::Group* group)
    {
        if (group == nullptr)
        {
            return false;
        }
        for (foundation::content::Instance* instance : group->Instances())
        {
            if (instance->TypeName() == StringView(u8"FontAsset"))
            {
                return true;
            }
        }
        for (foundation::content::Group* child : group->Groups())
        {
            if (ProjectHasFontAssets(child))
            {
                return true;
            }
        }
        return false;
    }

    // Editor text rendering: false = the classic per-size raster ramp; true = MSDF
    // distance-field atlases (one 48px bake per family, served at every requested size
    // through per-size scaled font views; VGContext draws glyph runs through the
    // distance-field pipeline). Flip back to false to restore the raster ramp.
    constexpr bool kUseDistanceFieldFonts = true;

    editor::EditorProject* EditorApplication::Project() const noexcept
    {
        return m_project.Get();
    }

    editor::EditorCookService& EditorApplication::CookService() noexcept
    {
        return m_cookService;
    }

    foundation::resource::ResourceManager* EditorApplication::Resources() const noexcept
    {
        return m_resources.Get();
    }

    engine::runtime::DefaultApplication* EditorApplication::EmbeddedApplication() const noexcept
    {
        return m_embeddedApp.Get();
    }

    void EditorApplication::Configure(runtime::IApplicationHost& host)
    {
        if (m_config.configureEngine)
        {
            m_config.configureEngine(host);
        }
    }

    void EditorApplication::SetSceneRenderer(foundation::render::ISceneRenderer* renderer) noexcept
    {
        m_sceneRenderer = renderer;
    }

    void EditorApplication::OnStartup(runtime::IApplicationHost& host)
    {
        m_host = &host;
        graphics::RenderWindow* mainRw = host.MainRenderWindow();
        if (mainRw == nullptr)
        {
            return;
        }

        LoadEditorSettings(); // per-user prefs FIRST: the font paths below honor them
        // Domains reach their OWN sections through the context (the contributed-settings
        // seam); the app stays ignorant of their shapes.
        m_context.SetUserEditorSettings(&m_editorSettings);

        // Fonts (CPU rasterization/baking; no device needed). Path resolution chain per
        // family: the Preferences override -> the dev-tree compile define -> the
        // exe-EMBEDDED Roboto (a relocated editor must never come up textless). Every
        // failure is LOUD - the old silent (void)LoadFont left a blank editor with no clue.
        m_fontService = MakeUnique<fonts::TrueTypeFontService>(m_editorAllocator,
                                                               m_editorAllocator);
        // MSDF bakes below hit the per-user disk cache: first launch bakes (in parallel)
        // and stores; every launch after loads the atlas instead of re-running msdfgen.
        m_fontAtlasCache = MakeUnique<editor::FontAtlasDiskCache>(
            m_editorAllocator, m_editorAllocator,
            PathJoin(GetUserDataDirectory().AsView(), u8"font-atlas-cache").AsView());
        fonts::FontAtlasBakerFactory::SetAtlasCache(m_fontAtlasCache.Get());
        String fontPath = m_config.fontPath;
        String monoFontPath = m_config.monoFontPath;
        if (const editor::EditorFontSettings* fontPrefs =
                m_editorSettings.Find<editor::EditorFontSettings>())
        {
            if (!fontPrefs->fontPath.IsEmpty())
            {
                fontPath = fontPrefs->fontPath;
            }
            if (!fontPrefs->monoFontPath.IsEmpty())
            {
                monoFontPath = fontPrefs->monoFontPath;
            }
        }

        // Load one family across `sizes`; returns true when every size loaded. Falls back
        // to the embedded face (when given) if the path fails outright.
        const auto loadFamily = [this](StringView family, StringView path,
                                       Span<const f32> sizes, fonts::FontLoadOptions options,
                                       Span<const u8> embedded) -> bool
        {
            bool anyLoaded = false;
            for (f32 size : sizes)
            {
                options.pixelHeight = size;
                if (!path.IsEmpty() &&
                    m_fontService->LoadFont(family, path, options) ==
                        fonts::FontLoadResult::Success)
                {
                    anyLoaded = true;
                    continue;
                }
                if (!embedded.IsEmpty() &&
                    m_fontService->LoadFontFromMemory(family, embedded, options) ==
                        fonts::FontLoadResult::Success)
                {
                    anyLoaded = true;
                }
            }
            if (!anyLoaded)
            {
                LOG_ERROR(u8"Editor",
                                   u8"font family '{}' failed to load (path '{}', embedded "
                                   u8"fallback {}) - its text will not render",
                                   family, path, embedded.IsEmpty() ? u8"absent" : u8"failed");
            }
            return anyLoaded;
        };
        const Span<const u8> embedded(m_config.embeddedFont, m_config.embeddedFontSize);

        if (kUseDistanceFieldFonts)
        {
            // MSDF path: ONE atlas per family, baked at 48px, sampled crisp at every size
            // the styles request (GetFont's closest-size fallback lands on it). The VG
            // renderer switches to the distance-field pipeline per glyph run automatically.
            fonts::DistanceFieldFonts::Initialize();
            fonts::FontLoadOptions options = fonts::FontLoadOptions::DistanceField();
            options.firstCodepoint = 32;
            options.lastCodepoint = 255; // the ExtendedLatin range the raster path bakes
            options.atlasWidth = 1024;
            options.atlasHeight = 1024;
            const f32 distanceFieldSize[] = {48.0f};
            (void)loadFamily(u8"Roboto", fontPath.AsView(), Span<const f32>(distanceFieldSize, 1), options,
                             embedded);
            (void)loadFamily(u8"Mono", monoFontPath.AsView(), Span<const f32>(distanceFieldSize, 1),
                             options, Span<const u8>{}); // mono has no embedded twin
        }
        else
        {
            // A full ramp so styles can pick small (property fields), regular, and
            // heading sizes without falling back to a mismatched rasterization.
            const f32 sizes[] = {10.0f, 11.0f, 12.0f, 13.0f, 14.0f,
                                 16.0f, 18.0f, 20.0f, 24.0f, 32.0f};
            (void)loadFamily(u8"Roboto", fontPath.AsView(), Span<const f32>(sizes, 10),
                             fonts::FontLoadOptions::ExtendedLatin(), embedded);
            // Fixed-pitch family for CodeEditView (script/shader/XML pages). Smaller ramp:
            // code text only needs the field-to-heading range. No embedded twin - a missing
            // mono face falls back to the default family (per the config contract).
            const f32 monoSizes[] = {10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 16.0f};
            (void)loadFamily(u8"Mono", monoFontPath.AsView(), Span<const f32>(monoSizes, 6),
                             fonts::FontLoadOptions::ExtendedLatin(), Span<const u8>{});
        }
        EditorIcons::Get().Initialize(); // shared SVG drawables (toolbar + asset types)
        // The editor's mount over the data root: the UI host reads its VG shaders through it.
        m_dataFileSystem = MakeUnique<foundation::vfs::NativeFileSystem>(
            m_editorAllocator, m_config.dataRoot.AsView(), m_editorAllocator);
        m_uiHost = MakeUnique<ui::runtime::UIHost>(m_editorAllocator, m_editorAllocator,
                                                   *host.Graphics(), *host.Shell(), *m_fontService,
                                                   *m_dataFileSystem);
        m_dockHost = MakeUnique<ui::application::RuntimeDockableWindowHost>(m_editorAllocator,
                                                                            host, *m_uiHost);

        // Theme: register the toolkit extension BEFORE creating the stylesheet (extensions
        // only apply to themes built afterward), then the editor defaults to dark.
        foundation::ui::ThemeRegistry::RegisterExtension(&m_toolkitTheme);
        // Editor theme: the warm "Graphite & Orange" palette on the rounded theme (soft corners
        // everywhere) - a crafted, less-bland alternative to the stock flat/square cool-grey dark.
        const foundation::ui::ThemePalette palette = foundation::ui::ThemePalette::GraphiteOrange();
        m_styleSheet = foundation::ui::RoundedDarkTheme::Create(m_editorAllocator, palette);
        // The window CLEAR color comes from the same palette: any surface the chrome doesn't
        // cover (the project-manager screen most of all) must read as the theme's background,
        // not the UIHost's hard-coded near-black default.
        m_uiHost->SetClearColor(palette.Background.r, palette.Background.g, palette.Background.b,
                                1.0f);
        // Editor-specific overrides on top of the stock theme: property-grid fields read
        // better noticeably smaller and tighter than the theme's 14px/6x4 control chrome
        // (a full inspector column of them is the densest text in the editor).
        m_styleSheet->ForClass(u8"property-field")
            .Set(foundation::ui::StyleProperty::FontSize, 12.0f)
            .Set(foundation::ui::StyleProperty::Padding, foundation::ui::Thickness{5, 2});

        // Icon BAKE (the Godot-verified crispness recipe): rasterize every editor SVG icon
        // once, 4x supersampled, into a shared atlas at the chrome sizes the UI actually
        // uses; BakedSVGDrawable then draws pixel-snapped quads - identical texels for
        // every instance, no per-tab subpixel shimmer. Any bake failure silently keeps the
        // live-vector fallback. The close X becomes a real themed drawable for the dock
        // chrome (tinted from the palette - the bake is near-white, multiply-tint works),
        // replacing the per-tab line-drawn X.
        {
            EditorIcons& icons = EditorIcons::Get();
            // UI-scale preference: multiplies the OS content scale for the whole editor
            // UI; also the way to exercise the DPI path without a scaled monitor.
            f32 uiScale = 1.0f;
            if (const editor::EditorUiSettings* uiPrefs =
                    m_editorSettings.Find<editor::EditorUiSettings>())
            {
                uiScale = Clamp(uiPrefs->uiScale, editor::kUiScaleMin, editor::kUiScaleMax);
            }
            m_uiHost->SetUiScale(uiScale);
            BakeEditorIcons(mainRw->Window().ContentScale() * uiScale);
            if (icons.close)
            {
                icons.close->TintColor =
                    foundation::core::Color{palette.Text.r, palette.Text.g, palette.Text.b,
                                          190.0f / 255.0f};
                const foundation::ui::DrawablePtr closeIcon(icons.close.Get());
                m_styleSheet
                    ->ForTypePseudo(&ui::toolkit::DockablePanel::StaticType(), u8"close-button")
                    .Set(foundation::ui::StyleProperty::Background, closeIcon);
                m_styleSheet
                    ->ForTypePseudo(&ui::toolkit::DockTabGroup::StaticType(), u8"close-button")
                    .Set(foundation::ui::StyleProperty::Background, closeIcon);
            }
        }
        m_uiHost->Context().SetStyleSheet(m_styleSheet);

        // Exit goes through the dirty check: the shell consults this before honoring the
        // main window's close button / OS quit; File>Exit routes through the same helper.
        host.Shell()->OnMainWindowCloseRequested = [this]() { return ConfirmExitAllowed(); };

        m_shell.Build(m_context, m_dockHost.Get(), mainRw->Window().Width(),
                      mainRw->Window().Height());
        // The ACTIVE page follows dock-tab activation, not just OpenPage/ClosePage - with
        // side-by-side tab groups, Save was hitting whichever page opened last, not the tab
        // the user selected. Non-page panels (Console, Assets) leave the active page alone.
        m_shell.Docks()->OnPanelActivated.Add(
            foundation::ui::Event<void(ui::toolkit::DockablePanel*)>::Handler{
                [this](ui::toolkit::DockablePanel* panel)
                {
                    if (panel == nullptr)
                    {
                        return;
                    }
                    for (const PagePanel& entry : m_pagePanels)
                    {
                        if (entry.panel == panel)
                        {
                            m_context.SetActivePage(entry.page);
                            return;
                        }
                    }
                }});
        m_uiHost->AttachWindow(mainRw, RefPtr<foundation::ui::RootView>(m_shell.Root()));

        // Toast overlay on the main window root (input passes through outside the cards);
        // EditorContext::Notify routes here, and also mirrors to the status bar.
        m_toastHost = MakeRef<ui::toolkit::ToastHost>(m_editorAllocator);
        m_shell.Root()->AddView(m_toastHost.Get());
        m_context.OnNotice = [this](editor::NoticeKind kind, StringView message)
        {
            ShowToast(kind, message);
            m_context.SetStatus(message);
        };
        // The console's copy says so, like every other copy.
        if (LogView* console = m_shell.Console())
        {
            console->OnCopied = [this](usize lines)
            {
                const String text = lines == 1 ? String(u8"Copied 1 log line")
                                               : Format(u8"Copied {} log lines", lines);
                m_context.Notify(editor::NoticeKind::Success, text.AsView());
            };
        }

        // ---- the EMBEDDED RUNTIME ----
        // The editor owns a second, persistent runtime Context populated by the SAME
        // DefaultApplication the player runs: gameplay subsystems live THERE, and every
        // scene (editing pages, Simulate, previews, the Game tab) is hosted there. The
        // editor's own context carries no gameplay subsystems. It starts WITHOUT a
        // resource manager: projects open and close at runtime now (the built-in project
        // manager), so the per-project manager late-attaches in OpenProjectAt and
        // detaches in CloseProject - the runtime's Resources() consumers are lazy and
        // null-tolerant between projects.
        m_embeddedHost = MakeUnique<runtime::EmbeddedApplicationHost>(m_editorAllocator, host,
                                                                      m_runtimeContext);
        m_embeddedHost->SetExitHandler(Function<void(int)>{
            [](int code)
            {
                // A Game tab routes its own instance's exit to its own run; what reaches here
                // is an instance no tab is playing, which has no run to stop.
                LOG_INFO(u8"Editor", u8"embedded app requested exit({}) with no Game tab playing it",
                         code);
            }});
        m_embeddedApp = MakeUnique<engine::runtime::DefaultApplication>(m_editorAllocator);
        m_embeddedApp->SetDataRoot(m_config.dataRoot.AsView()); // the editor's root, not a re-walk
        m_embeddedApp->Configure(*m_embeddedHost);
        // Embedded-runtime input policy: UN-BOUND input must never
        // reach scene-tier game UI here. The player's shell source owns its whole
        // window, so its un-bound input reaches every scene (the AllScenes default);
        // in the editor the same rule would let raw shell keystrokes and editor-pane
        // clicks land in game canvases of OPEN EDITING PAGES. Editing/Simulate HUDs
        // therefore render WYSIWYG but are deliberately NOT interactive; the Game
        // tab is the interactive-run surface and binds its scene on Play.
        if (m_embeddedApp->Input() != nullptr)
        {
            m_embeddedApp->Input()->SetUnboundScenePolicy(
                engine::input::UnboundInputScenePolicy::ScreenTierOnly);
        }
        // Each Game tab runs its own game inside this one application: each run gets its own
        // screen tier (menus, HUD, pause), so tabs never draw or drive each other's screens.
        if (m_embeddedApp->UI() != nullptr)
        {
            m_embeddedApp->UI()->SetRunScreens(true);
        }
        m_runtimeContext.Startup();
        m_embeddedApp->OnStartup(*m_embeddedHost);

        // Per-subsystem editor plugins register here (page factories, creators, ...), and
        // the exe injects the engine interfaces the app drives (SetSceneRenderer). They
        // receive the EMBEDDED host: every page's Ctx() resolves to the runtime context.
        // ONCE per app run - the registered factories capture the embedded host/app, which
        // stay alive across project close/open.
        RegisterActions(); // the editor-wide set first: the menu bar follows registration order
        if (m_config.registerEditors)
        {
            m_config.registerEditors(*this, *m_embeddedHost, *m_uiHost);
        }

        // The user's shortcut overrides, once every domain has declared its actions (an
        // override names an id; a domain not loaded keeps its entry for a later run).
        (void)editor::ApplyShortcutOverrides(m_editorSettings.Section<editor::EditorShortcutSettings>(),
                                             m_context.Actions());

        // Menus AFTER registration - File > New builds from the creator registry. Built once;
        // both modes share them (the manager screen simply doesn't show the shell chrome).
        BuildMenus();

        // Mode: the project manager screen (no project given on the command line), or
        // straight into the given project (a CLI-opened editor keeps the single-project
        // lifecycle - no Close Project round-trip).
        if (m_config.startInProjectManager)
        {
            EnterManagerMode();
        }
        else
        {
            OpenProjectAt(m_config.projectDirectory.AsView());
        }
    }

    void EditorApplication::CookMissingForPage(foundation::content::Instance& instance)
    {
        if (m_project.Get() == nullptr)
        {
            return;
        }
        Array<Guid> unresolved;
        if (m_resources)
        {
            m_resources->CollectUnresolved(unresolved);
        }
        // Only ids with a live SOURCE instance can cook - a stale ref to a deleted
        // asset stays unresolved forever and must not re-request a cook on every open.
        Array<Guid> roots;
        for (const Guid& id : unresolved)
        {
            if (m_project->SourceDb().GetInstance(id) != nullptr)
            {
                roots.PushBack(id);
            }
        }
        const CookBadge badge = m_cookService.BadgeFor(instance);
        if (badge == CookBadge::Missing || badge == CookBadge::Failed)
        {
            roots.PushBack(instance.Id());
        }
        if (roots.IsEmpty())
        {
            return;
        }
        m_cookService.RequestCookFor(Move(roots), false);
    }

    void EditorApplication::RefreshActivePageMark()
    {
        editor::EditorPage* active = m_context.ActivePage();
        for (const PagePanel& entry : m_pagePanels)
        {
            const bool mark = entry.page == active;
            if (entry.panel != nullptr && entry.panel->ActiveMark != mark)
            {
                entry.panel->ActiveMark = mark;
                if (entry.panel->Parent != nullptr)
                {
                    entry.panel->Parent->InvalidateVisual(); // the ring: colour only, no geometry
                }
            }
        }
    }

    void EditorApplication::OpenGamePage(bool newInstance)
    {
        if (!newInstance && m_gamePage != nullptr)
        {
            for (const PagePanel& entry : m_pagePanels)
            {
                if (entry.page == m_gamePage)
                {
                    m_context.RevealPage(m_gamePage);
                    return;
                }
            }
        }
        if (!m_context.GamePageFactory)
        {
            m_context.Notify(editor::NoticeKind::Info, u8"No game page registered in this build.");
            return;
        }
        // A unique id per tab, so a docking restore cannot collide; the page answers to it too.
        const String pieId = newInstance ? Format(u8"game-page-{}", m_gamePageCounter + 1)
                                         : String(u8"game-page");
        UniquePtr<editor::EditorPage> page = m_context.GamePageFactory(newInstance, pieId.AsView());
        if (!page)
        {
            return;
        }
        // All pages in this app are UIEditorPages (:ui_page contract), the Game page too.
        UIEditorPage* uiPage = static_cast<UIEditorPage*>(m_context.AdoptPage(Move(page)));
        if (uiPage == nullptr)
        {
            return;
        }
        if (newInstance)
        {
            ++m_gamePageCounter;
        }
        else
        {
            m_gamePage = uiPage; // only the primary tab is the focus target
        }

        ui::toolkit::DockablePanel* panel =
            m_shell.AddPagePanel(uiPage->Title(), uiPage->ContentView());
        panel->SetPersistenceId(pieId.AsView());
        panel->DestroyOnClose = true; // the page's views die with it, as for any page
        panel->OnCloseRequested.Add(
            [this, uiPage](ui::toolkit::DockablePanel*)
            {
                m_uiHost->Context().MutationQueueRef().QueueAction(
                    Function<void()>{[this, uiPage]() { ClosePage(uiPage); }});
            });
        m_pagePanels.PushBack(PagePanel{uiPage, panel});
        RefreshActivePageMark(); // the panel exists now; the page may already be the active one
    }

    UIEditorPage* EditorApplication::OpenInstancePage(foundation::content::Instance& instance)
    {
        const usize before = m_context.OpenPages().Size();
        editor::EditorPage* page = m_context.OpenPage(instance);
        if (page == nullptr)
        {
            // The toast stays short; the LOG carries the identifying details (user ask
            // 2026-08-12) - an unresolvable type here is a retired or unregistered identity,
            // and the stored spelling is the clue.
            LOG_WARNING(u8"Editor",
                        u8"no editor page for asset '{}' - stored type '{}'::'{}', guid {} "
                        u8"(unknown type name, or no page factory registered)",
                        instance.Name(), instance.TypeNamespace(), instance.TypeName(),
                        instance.Id());
            m_context.Notify(editor::NoticeKind::Warning,
                             u8"No editor registered for this asset type.");
            return nullptr;
        }
        // All factories in this app produce UIEditorPages (:ui_page contract).
        UIEditorPage* uiPage = static_cast<UIEditorPage*>(page);
        if (m_context.OpenPages().Size() == before)
        {
            // Focused an existing page - select its tab.
            for (const PagePanel& entry : m_pagePanels)
            {
                if (entry.page == uiPage)
                {
                    m_shell.Docks()->ActivatePanel(entry.panel);
                    break;
                }
            }
            return uiPage;
        }

        ui::toolkit::DockablePanel* panel =
            m_shell.AddPagePanel(uiPage->Title(), uiPage->ContentView());
        {
            // Guid-keyed persistence id: the saved dock layout re-places this page's panel
            // when the page reopens on the next launch.
            utf8char guidChars[37];
            uiPage->InstanceId().ToChars(guidChars);
            panel->SetPersistenceId(StringView(guidChars));
        }
        // The page's views die with the page: its close destroys the panel, or a later layout
        // restore would find the stale one by this id and float it holding freed content.
        panel->DestroyOnClose = true;
        // (Docking activates the new tab - toolkit behavior.)
        // The DockManager's own close handling (wired in AddPanel) destroys the panel through
        // its deferred-delete queue; we additionally tear down the PAGE - deferred through the
        // UI mutation queue, since destroying views mid-event-dispatch is unsafe.
        panel->OnCloseRequested.Add(
            [this, uiPage](ui::toolkit::DockablePanel*)
            {
                m_uiHost->Context().MutationQueueRef().QueueAction(
                    Function<void()>{[this, uiPage]() { ClosePage(uiPage); }});
            });
        // Dirty pages don't close silently: veto the gesture and prompt Save / Discard /
        // Cancel. The dialog's buttons invoke OnCloseRequested DIRECTLY (bypassing this
        // veto), which runs the normal dock + page teardown.
        panel->OnCloseInterceptor = [this, uiPage](ui::toolkit::DockablePanel* p) -> bool
        {
            if (!uiPage->IsDirty())
            {
                return true;
            }
            ShowDirtyCloseDialog(uiPage, p);
            return false;
        };
        m_pagePanels.PushBack(PagePanel{uiPage, panel});
        RefreshActivePageMark(); // the panel exists now; the page may already be the active one
        CookMissingForPage(instance); // uncooked dependencies cook without a manual step
        return uiPage;
    }

    void EditorApplication::ApplyProjectUiDefaults()
    {
        // Idempotent by construction: Bind by id returns the cached proxy when already
        // bound, and Set* with the same product is a no-op-equivalent swap. A bind that
        // MISSES (fresh checkout, products not cooked yet) simply skips - the next
        // cook-finished callback lands here again and heals it.
        if (!m_project || !m_resources || !m_embeddedApp || m_embeddedApp->UI() == nullptr)
        {
            return;
        }
        const Guid themeId = m_project->Settings().defaultUiThemeId;
        if (!themeId.IsNil())
        {
            if (auto themeProxy = m_resources->Bind<foundation::ui::UITheme>(themeId))
            {
                m_embeddedApp->UI()->SetDefaultTheme(themeProxy.Get());
            }
        }
        const Guid fontId = m_project->Settings().defaultUiFontId;
        if (!fontId.IsNil())
        {
            if (auto fontProxy = m_resources->Bind<foundation::fonts::Font>(fontId))
            {
                m_embeddedApp->UI()->SetDefaultFont(fontProxy.Get());
            }
        }
        // The other UI fonts, each a family beside the default one; always set, so a font
        // taken off the list leaves the game UI too.
        Array<const foundation::fonts::Font*> extras;
        for (const Guid& id : m_project->Settings().uiFontIds)
        {
            if (auto extra = m_resources->Bind<foundation::fonts::Font>(id))
            {
                extras.PushBack(extra.Get());
            }
        }
        m_embeddedApp->UI()->SetExtraFonts(Span<const foundation::fonts::Font* const>(extras.Data(), extras.Size()));
    }

    void EditorApplication::ShowToast(editor::NoticeKind kind, StringView message)
    {
        if (m_toastHost.Get() == nullptr)
        {
            return;
        }
        ui::toolkit::ToastRequest request;
        request.message = String(message);
        switch (kind)
        {
        case editor::NoticeKind::Success:
            request.severity = ui::toolkit::ToastSeverity::Success;
            break;
        case editor::NoticeKind::Warning:
            request.severity = ui::toolkit::ToastSeverity::Warning;
            break;
        case editor::NoticeKind::Error:
            request.severity = ui::toolkit::ToastSeverity::Error;
            break;
        case editor::NoticeKind::Info:
        default:
            request.severity = ui::toolkit::ToastSeverity::Info;
            break;
        }
        request.durationSeconds = (kind == editor::NoticeKind::Error) ? 0.0f : 5.0f; // errors stick
        (void)m_toastHost->Show(Move(request));
    }

    void EditorApplication::SavePage(editor::EditorPage& subject)
    {
        auto* page = &subject;
        if (page->Save().IsOk())
        {
            String message(u8"Saved '");
            message += page->Title();
            message += u8"'.";
            m_context.Notify(editor::NoticeKind::Success, message.AsView());
        }
        else
        {
            m_context.Notify(editor::NoticeKind::Error, u8"Save FAILED (see Console).");
        }
        FlushPendingAssetEdits();
    }

    void EditorApplication::FlushPendingAssetEdits()
    {
        // Live edits to cooked products (terrain sculpt writes the runtime Heightfield in place)
        // are registered on the context by their viewport tool; a save writes them back to their
        // SOURCE assets + recooks (see EditorContext::RegisterAssetEdit/DrainAssetEdits). This is
        // context-wide, not page-specific: any Save flushes whatever a tool has queued.
        if (!m_project || !m_context.HasPendingAssetEdits())
        {
            return;
        }
        if (m_context.DrainAssetEdits(m_project->SourceDb()).IsOk())
        {
            m_context.Notify(editor::NoticeKind::Success, u8"Persisted live asset edits.");
        }
        else
        {
            m_context.Notify(editor::NoticeKind::Error, u8"Asset-edit persist FAILED (see Console).");
        }
    }

    void EditorApplication::ClosePanelAndPage(PagePanel entry)
    {
        if (entry.panel != nullptr)
        {
            entry.panel->SetContent(nullptr); // the page holds the last reference to its views
            if (m_shell.Docks() != nullptr)
            {
                m_shell.Docks()->ClosePanel(entry.panel);
            }
        }
        ClosePage(entry.page);
    }

    void EditorApplication::ClosePage(UIEditorPage* page)
    {
        if (page == m_gamePage)
        {
            m_gamePage = nullptr;
        }
        for (usize i = 0; i < m_pagePanels.Size(); ++i)
        {
            if (m_pagePanels[i].page == page)
            {
                if (page->IsDirty())
                {
                    m_context.SetStatus(
                        u8"Closed page had unsaved changes."); // no save prompt here
                }
                page->OnClose(); // release GPU/scene resources while device + window live
                m_pagePanels.RemoveAt(i);
                m_context.ClosePage(page); // destroys the page
                return;
            }
        }
    }

    void EditorApplication::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        if (m_thumbnailStage)
        {
            m_thumbnailStage->Update(); // take/stage the next queued GPU thumbnail job
        }
        if (m_mcpHost)
        {
            m_mcpHost->Pump(); // answers a waiting agent call HERE: tools touch main-thread state
        }
        // DPI drift: the window moved to a monitor with a different content scale (or the
        // OS scale changed) - re-bake the icon set at the new device sizes so icons stay
        // 1:1-texel crisp instead of bilinear-scaled from the old bake.
        if (m_uiHost)
        {
            if (graphics::RenderWindow* mainRw = host.MainRenderWindow())
            {
                const f32 scale = mainRw->Window().ContentScale() * m_uiHost->UiScale();
                if (scale > 0.1f && Abs(scale - m_iconBakeScale) > 0.01f)
                {
                    BakeEditorIcons(scale);
                }
            }
        }

        // Drive the embedded runtime's frame lanes FIRST: per-scene fixed stepping runs
        // in BeginFrame, physics interpolation in Update - pages then read fresh state.
        // (EndFrame closes in OnRenderWindow after the scene bracket.) Mirrors the middle
        // of ApplicationHost::Tick; the embedded app's OnUpdate itself (game script) is
        // driven by the Game page's play bracket, not here.
        if (m_embeddedApp)
        {
            const f32 scaled = dt * m_runtimeContext.TimeScale();
            m_runtimeContext.BeginFrame(dt);
            // Networking's per-frame transport pump rides NetworkSubsystem::PostUpdate,
            // driven by m_runtimeContext.PostUpdate below.
            m_runtimeContext.Update(scaled);
            m_runtimeContext.PostUpdate(scaled);
            // Tick EVERY game instance's script ONCE per frame here -
            // so N game tabs don't tick every instance N times.
            m_embeddedApp->OnUpdate(*m_embeddedHost, dt);
        }

        // Window screenshots recorded last frame: written once the GPU has run the copies; a
        // window closed before it drew fails its capture rather than leaving it waiting.
        if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
        {
            m_windowCapture.Complete(*gfx->Raw(), host.Ctx().Allocator());
        }
        if (m_windowCapture.Busy())
        {
            Array<u32> open(m_editorAllocator);
            if (host.Shell() != nullptr && host.Shell()->WindowManager() != nullptr)
            {
                for (foundation::shell::IWindow* window : host.Shell()->WindowManager()->Windows())
                {
                    if (window != nullptr && window->IsOpen())
                    {
                        open.PushBack(window->Id());
                    }
                }
            }
            m_windowCapture.FailMissing(Span<const u32>(open.Data(), open.Size()));
        }
        if (m_announceScreenshot && !m_windowCapture.Busy())
        {
            m_announceScreenshot = false;
            const Array<WindowShot> shots = m_windowCapture.Shots();
            if (!shots.IsEmpty() && shots[0].state == WindowCaptureState::Written)
            {
                m_context.Notify(editor::NoticeKind::Success,
                                 Format(u8"Screenshot saved: {}", shots[0].path.AsView()).AsView());
            }
            else
            {
                m_context.Notify(editor::NoticeKind::Error,
                                 u8"The screenshot failed (the Console, category Screenshot, says why)");
            }
        }
        const bool screenshotRequested = !m_config.screenshotPath.IsEmpty();
        if (m_config.autoExitSeconds > 0.0f || m_config.autoRebuildSeconds > 0.0f ||
            screenshotRequested)
        {
            m_elapsed += dt;
            if (m_config.autoExitSeconds > 0.0f && m_elapsed >= m_config.autoExitSeconds)
            {
                host.Shell()->RequestExit();
            }
            if (m_config.autoRebuildSeconds > 0.0f && !m_autoRebuilt &&
                m_elapsed >= m_config.autoRebuildSeconds)
            {
                m_autoRebuilt = true;
                m_cookService.RequestCook(true);
            }
            if (screenshotRequested && !m_screenshotFired &&
                m_elapsed >= m_config.screenshotAfterSeconds)
            {
                m_screenshotFired = true; // one shot: the next main-window frame records it
                if (graphics::RenderWindow* main = host.MainRenderWindow(); main != nullptr)
                {
                    WindowShotRequest request;
                    request.window = main->Window().Id();
                    request.main = true;
                    request.path = m_config.screenshotPath;
                    m_windowCapture.Request(Span<const WindowShotRequest>(&request, 1));
                }
            }
        }
        // Headless-debug hook: ENV_TEST_OPEN=<guid> opens that instance's page ~2s in
        // and opens it AGAIN ~4s in (the focus-existing branch) - reproduces the asset
        // browser's double-click paths in unattended (ASAN/gdb) runs.
        if (const Optional<String> testOpen = GetEnvironmentVariable(u8"ENV_TEST_OPEN");
            testOpen.HasValue() && m_project)
        {
            m_testOpenElapsed += dt;
            const bool first = m_testOpenStage == 0 && m_testOpenElapsed >= 2.0f;
            const bool second = m_testOpenStage == 1 && m_testOpenElapsed >= 4.0f;
            if (first || second)
            {
                ++m_testOpenStage;
                Guid id;
                if (Guid::TryParse(testOpen.Value().AsView(), id))
                {
                    if (foundation::content::Instance* instance =
                            m_project->SourceDb().GetInstance(id))
                    {
                        (void)OpenInstancePage(*instance);
                    }
                }
            }
        }

        // Headless-debug hook: ENV_TEST_REIMPORT="<group>;<file>" deletes the named
        // source group ~2s in and reimports <file> ~4s in (the watcher recook follows) -
        // scripts the delete->reimport crash repro for unattended ASAN runs.
        if (const Optional<String> reimport = GetEnvironmentVariable(u8"ENV_TEST_REIMPORT");
            reimport.HasValue() && m_project)
        {
            m_testOpenElapsed += dt; // shared timer with ENV_TEST_OPEN (use one hook per run)
            const StringView spec = reimport.Value().AsView();
            usize semi = spec.Size();
            for (usize i = 0; i < spec.Size(); ++i)
            {
                if (spec.Data()[i] == u8';')
                {
                    semi = i;
                    break;
                }
            }
            if (semi < spec.Size())
            {
                if (m_testOpenStage == 0 && m_testOpenElapsed >= 2.0f)
                {
                    ++m_testOpenStage;
                    const String groupName(spec.SubStr(0, semi));
                    if (foundation::content::Group* group =
                            m_project->SourceDb().RootGroup()->GetGroup(groupName.AsView()))
                    {
                        m_cookService.RunWhenIdle(
                            Function<void()>{[this, group]()
                                             {
                                                 (void)m_project->SourceDb().DeleteGroup(*group);
                                                 m_context.SetStatus(u8"[test] deleted group");
                                                 // Mirror DeleteGroupNow: the assets tree holds raw Group*
                                                 // rows - EVERY source-DB group mutation must Rebuild before
                                                 // the next layout binds stale pointers.
                                                 if (m_assetsView)
                                                 {
                                                     m_assetsView->Rebuild();
                                                 }
                                             }});
                    }
                }
                else if (m_testOpenStage == 1 && m_testOpenElapsed >= 4.0f)
                {
                    ++m_testOpenStage;
                    const String file(spec.SubStr(semi + 1, spec.Size() - semi - 1));
                    m_context.SetStatus(u8"[test] reimporting");
                    if (m_assetsView)
                    {
                        m_assetsView->ImportFile(file.AsView());
                    }
                }
            }
        }

        DrainLog();
        SyncPageTitles();
        // Background-cook progress -> status bar (log lines reach the Console via the
        // logger); a finished cook refreshes the Assets badges through OnCookFinished.
        m_cookService.Update(
            Function<void(StringView)>{[this](StringView line) { m_context.SetStatus(line); }});

        // Background jobs: pump, drain job logs, and once an export's pre-cook has finished, submit
        // the export's pack/stage job. Show the running job's step + percent in the status bar.
        m_jobService.Update(
            Function<void(StringView)>{[this](StringView line) { m_context.SetStatus(line); }});
        if (m_pendingExport.active && m_pendingExport.waitingCook && !m_cookService.IsCooking())
        {
            m_pendingExport.waitingCook = false;
            SubmitExportJob(m_pendingExport.presetName, m_pendingExport.all);
            m_pendingExport.active = false; // the job owns it now
        }
        if (m_jobService.IsBusy())
        {
            const editor::EditorJobService::ProgressView p = m_jobService.Progress();
            if (p.active)
            {
                String s(p.title.AsView());
                if (!p.step.IsEmpty())
                {
                    s += u8": ";
                    s += p.step;
                }
                s += u8" (";
                AppendCountTo(s, static_cast<usize>(p.fraction * 100.0f + 0.5f));
                s += u8"%)";
                m_context.SetStatus(s.AsView());
            }
        }

        if (m_assetsView)
        {
            m_assetsView->Refresh();
        }
        if (m_resources)
        {
            m_resources->Pump();          // finalize async resource loads (task #123)
            m_resources->CollectGarbage(); // release hot-reloaded-away products
        }

        // OS file drops -> the import pipeline (any editor window; imports land in the
        // Assets panel's selected group).
        if (m_assetsView && host.Shell() != nullptr)
        {
            m_droppedFiles.Clear();
            host.Shell()->DrainDroppedFiles(m_droppedFiles);
            if (!m_droppedFiles.IsEmpty())
            {
                // ONE review session for the whole drop (import-workflow ruling).
                Array<String> paths;
                for (const foundation::shell::DroppedFile& drop : m_droppedFiles)
                {
                    paths.PushBack(drop.path);
                }
                m_assetsView->ImportFiles(Span<const String>{paths.Data(), paths.Size()});
            }
        }
        if (m_uiHost)
        {
            m_uiHost->Update(dt);
        }
        if (m_toastHost)
        {
            m_toastHost->Update(dt);
        }
        if (m_dockHost)
        {
            m_dockHost->Tick();
        } // drag-follow for floating OS windows

        // Page hooks AFTER the UI laid out (viewport rects are current for input gating).
        for (const PagePanel& entry : m_pagePanels)
        {
            entry.page->OnUpdate(host, dt);
        }
    }

    void EditorApplication::OnRenderWindow(runtime::IApplicationHost& host,
                                           graphics::FrameContext& frame)
    {
        // ALL pages' viewport content renders during the MAIN window's frame, inside ONE
        // scene-renderer bracket, BEFORE any UI draws (the Sedulous editor structure:
        // offscreen targets are window-agnostic, so floated panels' windows simply sample
        // the textures this pass produced). Secondary-window frames are UI-only.
        if (frame.valid && frame.window == host.MainRenderWindow())
        {
            // Through the ISceneRenderer INTERFACE (render.api) - editor.app never links the
            // renderer. Begin/EndRendering self-guard while the renderer isn't ready.
            if (m_sceneRenderer != nullptr)
            {
                m_sceneRenderer->BeginRendering(*frame.encoder, frame.frameIndex);
            }
            for (const PagePanel& entry : m_pagePanels)
            {
                entry.page->OnRenderWindow(host, frame);
            }
            if (m_thumbnailStage)
            {
                m_thumbnailStage->Render(frame); // offscreen thumbnail job (same bracket)
            }
            if (m_sceneRenderer != nullptr)
            {
                m_sceneRenderer->EndRendering();
            }
            // Post-compose overlays (the Game tab's screen-tier UI onto its viewport).
            for (const PagePanel& entry : m_pagePanels)
            {
                entry.page->OnAfterSceneRender(host, frame);
            }
            if (m_embeddedApp)
            {
                m_runtimeContext.EndFrame();
            }
        }
        if (m_uiHost)
        {
            m_uiHost->RenderWindow(frame);
        }
        // A window screenshot: this window's finished backbuffer, UI included, when one of it is
        // armed. The copy sits in this frame's command stream; OnUpdate writes it next frame.
        if (frame.valid && frame.window != nullptr && frame.encoder != nullptr &&
            host.Graphics() != nullptr && host.Graphics()->Raw() != nullptr)
        {
            m_windowCapture.Record(*host.Graphics()->Raw(), *frame.encoder, frame.window->Window().Id(),
                                   frame.backbuffer, frame.window->Swap()->Format(), frame.width,
                                   frame.height);
        }
    }

    Array<u32> EditorApplication::CapturableWindows()
    {
        Array<u32> windows(m_editorAllocator);
        if (m_host == nullptr)
        {
            return windows;
        }
        graphics::RenderWindow* main = m_host->MainRenderWindow();
        if (main == nullptr || !main->Window().IsOpen() || main->Window().IsMinimized())
        {
            return windows; // a minimised editor draws no frame
        }
        windows.PushBack(main->Window().Id());
        if (m_host->Shell() != nullptr && m_host->Shell()->WindowManager() != nullptr)
        {
            for (foundation::shell::IWindow* window : m_host->Shell()->WindowManager()->Windows())
            {
                if (window != nullptr && window->Id() != main->Window().Id() && window->IsOpen() &&
                    !window->IsMinimized())
                {
                    windows.PushBack(window->Id());
                }
            }
        }
        return windows;
    }

    void EditorApplication::TakeEditorScreenshot()
    {
        const Array<u32> windows = CapturableWindows();
        if (windows.IsEmpty())
        {
            return;
        }
        WindowShotRequest request;
        request.window = windows[0];
        request.main = true;
        request.path = NewScreenshotPath(u8"editor");
        if (request.path.IsEmpty())
        {
            m_context.Notify(editor::NoticeKind::Error, u8"Could not create the screenshots folder");
            return;
        }
        m_windowCapture.Request(Span<const WindowShotRequest>(&request, 1));
        m_announceScreenshot = true;
    }

    void EditorApplication::OnShutdown(runtime::IApplicationHost& host)
    {
        fonts::FontAtlasBakerFactory::SetAtlasCache(nullptr); // ours dies with this app
        if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
        {
            m_windowCapture.Release(*gfx->Raw()); // the readback buffers, while the device lives
        }
        StopMcpHost();
        m_cookService.Shutdown(); // joins any in-flight cook before the DBs go away
        // Release page resources while the device and windows are still alive. Pages
        // destroy their scenes in the RUNTIME context, so it must outlive them.
        for (const PagePanel& entry : m_pagePanels)
        {
            entry.page->OnClose();
        }
        SaveLayout();
        if (m_embeddedApp)
        {
            m_embeddedApp->OnShutdown(*m_embeddedHost);
            m_runtimeContext.Shutdown();
        }
        EditorIcons::Get().Shutdown(); // release drawables deterministically
        if (kUseDistanceFieldFonts)
        {
            fonts::DistanceFieldFonts::Shutdown(); // unregister the MSDF baker (mirror of OnStartup)
        }
    }

    void
    EditorApplication::CreateAndOpen(const pipeline::AssetCreator& creator,
                                     foundation::content::Group* group)
    {
        // Cook gate: the plan worker reads the DBs with their structure frozen -
        // creating instances mid-plan is a race. Queue and replay when idle.
        if (m_cookService.MutationLocked())
        {
            const pipeline::AssetCreator* entry = &creator;
            m_cookService.RunWhenIdle(
                Function<void()>{[this, entry, group]() { CreateAndOpen(*entry, group); }});
            m_context.Notify(editor::NoticeKind::Info,
                             u8"Create queued until the current cook finishes.");
            return;
        }
        if (!m_project)
        {
            m_context.Notify(editor::NoticeKind::Error, u8"Create failed: no project is open.");
            return;
        }
        const String sourcesRoot(m_project->SourcesRoot().AsView());
        foundation::content::Instance* instance =
            creator.Create(group, m_project->SourceDb().RootGroup(), sourcesRoot.AsView());
        if (instance == nullptr)
        {
            m_context.Notify(editor::NoticeKind::Error,
                             Format(u8"Create failed: the {} could not be written.", creator.label)
                                 .AsView());
            return;
        }
        AfterCreate(creator, *instance);
        (void)OpenInstancePage(*instance);
    }

    void EditorApplication::AfterCreate(const pipeline::AssetCreator& creator,
                                        foundation::content::Instance& instance)
    {
        if (creator.setsDefaultScene && m_project && m_project->Settings().defaultSceneId.IsNil() &&
            m_project->Settings().defaultScene.IsEmpty())
        {
            m_project->Settings().defaultSceneId = instance.Id();
            m_project->Settings().defaultScene = instance.Path();
            (void)m_project->SaveSettings();
        }
        // Surface the new row immediately (the File-menu path bypasses the assets view's own
        // rebuild) and cook it so builder-backed assets become pickable without a manual
        // Cook All (a no-op for builder-less scenes: nothing is dirty).
        if (m_assetsView)
        {
            m_assetsView->Rebuild();
        }
        if (m_builders.FindByTypeName(instance.TypeName()) != nullptr)
        {
            m_cookService.RequestCook(false);
        }
    }

    bool EditorApplication::ConfirmExitAllowed()
    {
        usize dirtyCount = 0;
        for (const PagePanel& entry : m_pagePanels)
        {
            if (entry.page->IsDirty())
            {
                ++dirtyCount;
            }
        }
        if (dirtyCount == 0)
        {
            return true;
        }

        String message;
        AppendCountTo(message, dirtyCount);
        message += (dirtyCount == 1) ? StringView(u8" page has unsaved changes.")
                                     : StringView(u8" pages have unsaved changes.");
        RefPtr<foundation::ui::Dialog> dialog =
            MakeRef<foundation::ui::Dialog>(m_editorAllocator, StringView(u8"Unsaved changes"));
        RefPtr<foundation::ui::Label> label =
            MakeRef<foundation::ui::Label>(m_editorAllocator, message.AsView());
        label->WordWrap.SetValue(true);
        dialog->SetContent(label.Get());

        foundation::ui::Dialog* rawDialog = dialog.Get();
        foundation::ui::Button* saveAll =
            dialog->AddButton(u8"Save All & Exit", foundation::ui::DialogResult::None);
        saveAll->OnClick.Add(
            [this, rawDialog](foundation::ui::ButtonBase*)
            {
                bool allSaved = true;
                for (const PagePanel& entry : m_pagePanels)
                {
                    if (entry.page->IsDirty() && !entry.page->Save().IsOk())
                    {
                        allSaved = false;
                    }
                }
                if (allSaved)
                {
                    m_host->RequestExit();
                }
                else
                {
                    m_context.Notify(editor::NoticeKind::Error,
                                     u8"Save FAILED (see console) - staying open.");
                }
                rawDialog->Close(allSaved ? foundation::ui::DialogResult::OK
                                          : foundation::ui::DialogResult::Cancel);
            });
        foundation::ui::Button* discard =
            dialog->AddButton(u8"Exit Without Saving", foundation::ui::DialogResult::None);
        discard->OnClick.Add(
            [this, rawDialog](foundation::ui::ButtonBase*)
            {
                m_host->RequestExit();
                rawDialog->Close(foundation::ui::DialogResult::OK);
            });
        dialog->AddButton(u8"Cancel", foundation::ui::DialogResult::Cancel);
        dialog->Show(&m_uiHost->Context());
        return false;
    }

    void EditorApplication::AppendCountTo(String& out, usize value)
    {
        utf8char digits[20];
        usize n = 0;
        do
        {
            digits[n++] = static_cast<utf8char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        while (n > 0)
        {
            out.PushBack(digits[--n]);
        }
    }

    void EditorApplication::RunExport(StringView presetName, bool all)
    {
        if (!m_project)
        {
            m_context.Notify(editor::NoticeKind::Info, u8"Open a project first.");
            return;
        }
        if (m_jobService.IsBusy() || m_pendingExport.active)
        {
            m_context.Notify(editor::NoticeKind::Info, u8"An export is already in progress.");
            return;
        }
        m_pendingExport =
            PendingExport{String(presetName), all, /*waitingCook*/ true, /*active*/ true};
        m_context.Notify(editor::NoticeKind::Info, u8"Cooking before export...");
        m_cookService.RequestCook(
            false); // safe background cook; OnUpdate fires the export job after it
    }

    void EditorApplication::LoadEditorSettings()
    {
        editor::RegisterEditorSettingsTypes();
        editor::RegisterProjectRegistryTypes();   // the manager's recent-projects section
        RegisterEditorProjectSettingsTypes();     // the per-project store's app-side sections
        const Status loaded = editor::LoadEditorSettingsFromUserData(m_editorSettings);
        if (!loaded.IsOk() && loaded.Code() != ErrorCode::NotFound)
        {
            // NotFound (first run) is fine; anything else means the store loaded
            // PARTIALLY (Settings aborts at the first uninstantiable section) - and a
            // later save would rewrite the file from that gutted store, permanently
            // losing the dropped sections (the project registry, most painfully). Say so
            // loudly instead of quietly showing an empty manager.
            LOG_ERROR(u8"Editor",
                               u8"editor.settings.xml load FAILED (partial store) - check for "
                               u8"unregistered section types; later saves may drop sections");
        }
    }

    String EditorApplication::TemplatesRoot() const
    {
        StringView overrideRoot;
        if (const editor::EditorExportSettings* s =
                m_editorSettings.Find<editor::EditorExportSettings>())
        {
            overrideRoot = s->templatesRoot.AsView();
        }
        return editor::ResolveTemplatesRoot(overrideRoot);
    }

    String EditorApplication::Absolutize(StringView path)
    {
        if (PathIsAbsolute(path))
        {
            return String(path);
        }
        return PathJoin(GetCurrentDirectory().AsView(), path);
    }

    bool EditorApplication::AnyPresetPrunes(bool all, StringView presetName) const
    {
        if (all)
        {
            for (const editor::ExportPreset& p : m_exportPresets.presets)
            {
                if (p.pruneToReachable)
                {
                    return true;
                }
            }
            return false;
        }
        const editor::ExportPreset* p = m_exportPresets.Find(presetName);
        return p != nullptr && p->pruneToReachable;
    }

    void EditorApplication::SubmitExportJob(String presetName, bool all)
    {
        editor::EditorProject* project = m_project.Get();
        pipeline::BuilderRegistry* builders = &m_builders;

        // MAIN-THREAD pre-pass: transcode scene/prefab TEXT sources to the binary wire
        // (the stager needs the SceneSubsystem). One export at a time (IsBusy-guarded),
        // so the member map stays valid for the job's lifetime.
        m_exportSceneStreams.Clear();
        if (m_context.SceneStreamStager)
        {
            editor::CollectSceneStreams(*m_project->SourceDb().RootGroup(),
                                        m_context.SceneStreamStager, m_exportSceneStreams);
        }
        const HashMap<Guid, Array<byte>>* sceneStreams = &m_exportSceneStreams;

        // Load the presets on the MAIN thread (was in the job): the pre-scan below needs to know
        // whether pruning is requested, and the job then reuses this copy instead of re-reading.
        m_exportPresets.presets.Clear();
        {
            foundation::vfs::NativeFileSystem projectFs(project->Directory(), m_editorAllocator);
            if (!editor::LoadExportPresets(projectFs, m_exportPresets).IsOk())
            {
                editor::DefaultExportPresets(m_exportPresets);
            }
        }

        // MAIN-THREAD reachability pre-scan: pruning needs
        // the scene->asset edges, which means LOADING scenes - unsafe off the main thread. So when
        // a preset in this run prunes and the scene editor supplied a scanner, expand the reachable
        // closure NOW and hand the guid set to the background job (which then only cooks + packs
        // that set - pure I/O). Without a scanner the job falls back to pack-everything (safe).
        m_exportReachableRoots.Clear();
        m_exportReachableValid = false;
        if (AnyPresetPrunes(all, presetName.AsView()) && m_context.SceneRefScanner)
        {
            EditorApplication* self = this;
            const editor::SceneReferenceScanner adapter =
                [self](foundation::content::Instance& inst, foundation::content::ContentDatabase& db,
                       editor::SceneReferences& refs)
            { self->m_context.SceneRefScanner(inst, db, refs.resources, refs.prefabs); };
            const Array<editor::ExportRoot> seeds = editor::CollectExportRoots(*project);
            m_exportReachableRoots = editor::ExpandReachableRoots(*project, seeds, adapter);
            m_exportReachableValid = true;
        }
        const Array<Guid>* reachableRoots =
            m_exportReachableValid ? &m_exportReachableRoots : nullptr;
        const editor::ExportPresetSet* presetsPtr = &m_exportPresets;

        const String toolDir = GetExecutableDirectory();
        const String templatesRoot = TemplatesRoot(); // resolve on the main thread (reads settings)
        const String dataRoot = m_config.dataRoot;     // the shader cook reads <dataRoot>/Shaders
        const String outRoot = Absolutize(PathJoin(m_project->Directory(), u8"Dist").AsView());
        const String title(all ? StringView(u8"Export All") : StringView(u8"Export"));

        m_jobService.Submit(
            title.AsView(),
            [project, builders, toolDir, templatesRoot, dataRoot, presetName, all, outRoot,
             sceneStreams, reachableRoots, presetsPtr](editor::JobContext& ctx) -> Status
            {
                foundation::vfs::NativeFileSystem toolFs(toolDir.AsView(), editor::EditorRootAllocator());
                foundation::vfs::NativeFileSystem rootFs(
                    templatesRoot.AsView(), editor::EditorRootAllocator()); // imported templates (worker)
                editor::TemplateRegistry registry;
                registry.Refresh(templatesRoot.AsView(), &rootFs, toolDir.AsView(), &toolFs);
                const editor::ExportPresetSet& presets = *presetsPtr; // loaded on the main thread
                const editor::ExportProgress onProgress = [&ctx](StringView step, f32 frac)
                {
                    ctx.SetStep(step);
                    ctx.SetFraction(frac);
                };

                if (all)
                {
                    const Span<const editor::ExportPreset> span(presets.presets.Data(),
                                                                presets.presets.Size());
                    return editor::ExportAll(*project, span, registry, *builders, outRoot.AsView(),
                                             dataRoot.AsView(),
                                             /*rebuild*/ false, onProgress, /*cook*/ false,
                                             sceneStreams,
                                             /*scanner*/ nullptr, reachableRoots);
                }
                const editor::ExportPreset* preset = presets.Find(presetName.AsView());
                if (preset == nullptr)
                {
                    return Status{ErrorCode::NotFound};
                }
                editor::ExportResult result;
                return editor::ExportOne(*project, *preset, registry, *builders, outRoot.AsView(),
                                         dataRoot.AsView(),
                                         /*rebuild*/ false, &result, onProgress, /*cook*/ false,
                                         sceneStreams,
                                         /*scanner*/ nullptr, reachableRoots);
            },
            [this, outRoot](Status s) // main thread
            {
                if (!s.IsOk())
                {
                    m_context.Notify(editor::NoticeKind::Error, u8"Export failed (see Console).");
                    return;
                }
                m_context.SetStatus(u8"Export complete.");
                // Sticky success toast with a button that reveals the Dist folder in the OS file
                // manager (the button click dismisses the toast, per ToastHost's onAction contract).
                if (m_toastHost.Get() != nullptr)
                {
                    ui::toolkit::ToastRequest request;
                    request.message = String(u8"Export complete.");
                    request.severity = ui::toolkit::ToastSeverity::Success;
                    request.durationSeconds = 0.0f; // sticky
                    request.actionLabel = String(u8"Open Folder");
                    request.onAction = [this, outRoot]()
                    {
                        LOG_DEBUG(u8"Editor", u8"Open Folder clicked -> reveal '{}'",
                                           outRoot.AsView());
                        if (m_host != nullptr && m_host->Shell() != nullptr &&
                            m_host->Shell()->Dialogs() != nullptr)
                        {
                            m_host->Shell()->Dialogs()->OpenPath(outRoot.AsView());
                        }
                        else
                        {
                            LOG_WARNING(
                                u8"Editor", u8"Open Folder: no shell dialog service available");
                        }
                    };
                    (void)m_toastHost->Show(Move(request));
                }
            });
    }

    void EditorApplication::BuildTemplateRegistryMainThread(editor::TemplateRegistry& out)
    {
        const String toolDir = GetExecutableDirectory();
        const String templatesRoot = TemplatesRoot();
        foundation::vfs::NativeFileSystem toolFs(toolDir.AsView(), m_editorAllocator);
        foundation::vfs::NativeFileSystem rootFs(templatesRoot.AsView(), m_editorAllocator);
        out.Refresh(templatesRoot.AsView(), &rootFs, toolDir.AsView(), &toolFs);
    }

    ui::FlexLayout* EditorApplication::AddFormRow(ui::FlexLayout& column, StringView label,
                                                  ui::View* field)
    {
        auto row = MakeRef<ui::FlexLayout>(m_editorAllocator);
        row->Direction = ui::Orientation::Horizontal;
        row->Spacing = 8;
        {
            auto text = MakeRef<ui::Label>(m_editorAllocator, label);
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(120));
            lp.AlignSelf = ui::Align::Center;
            row->AddView(text.Get(), lp);
        }
        if (field != nullptr)
        {
            ui::LayoutStyle lp;
            lp.FlexGrow = 1.0f;
            lp.AlignSelf = ui::Align::Center;
            row->AddView(field, lp);
        }
        ui::FlexLayout* raw = row.Get();
        ui::LayoutStyle lp;
        lp.Width = ui::SizeSpec::Match();
        column.AddView(row.Get(), lp);
        return raw;
    }

    void EditorApplication::QueueReplaceDialog(ui::Dialog* current, Function<void()> open)
    {
        m_uiHost->Context().MutationQueueRef().QueueAction(
            Function<void()>{[current, open = Move(open)]()
                             {
                                 if (current != nullptr)
                                 {
                                     current->Close();
                                 }
                                 open();
                             }});
    }

    void EditorApplication::ReopenExportPresetsPanel(ui::Dialog* current)
    {
        QueueReplaceDialog(current, Function<void()>{[this]() { OpenExportPresetsPanel(); }});
    }

    void EditorApplication::ReopenTemplatesManager(ui::Dialog* current)
    {
        QueueReplaceDialog(current, Function<void()>{[this]() { OpenTemplatesManager(); }});
    }

    void EditorApplication::SavePresetsController()
    {
        if (!m_project)
        {
            return;
        }
        foundation::vfs::NativeFileSystem projectFs(m_project->Directory(), m_editorAllocator);
        if (!m_presetsController.Save(*projectFs.AsWritable()).IsOk())
        {
            m_context.Notify(editor::NoticeKind::Error,
                             u8"Saving export presets FAILED (see console).");
        }
    }

    String EditorApplication::JoinSemicolons(const Array<String>& items)
    {
        String out;
        for (usize i = 0; i < items.Size(); ++i)
        {
            if (i > 0)
            {
                out += u8";";
            }
            out += items[i].AsView();
        }
        return out;
    }

    void EditorApplication::SplitSemicolons(StringView text, Array<String>& out)
    {
        const auto isSpace = [](utf8char c) { return c == utf8char(' ') || c == utf8char('\t'); };
        usize start = 0;
        for (usize i = 0; i <= text.Size(); ++i)
        {
            if (i != text.Size() && text[i] != utf8char(';'))
            {
                continue;
            }
            usize s = start, e = i;
            while (s < e && isSpace(text[s]))
            {
                ++s;
            }
            while (e > s && isSpace(text[e - 1]))
            {
                --e;
            }
            if (e > s)
            {
                out.PushBack(String(text.SubStr(s, e - s)));
            }
            start = i + 1;
        }
    }

    void EditorApplication::PickAdditionalFiles(RefPtr<ui::EditText> target)
    {
        if (m_host == nullptr || m_host->Shell() == nullptr ||
            m_host->Shell()->Dialogs() == nullptr)
        {
            m_context.Notify(editor::NoticeKind::Error, u8"File dialogs are unavailable.");
            return;
        }
        m_host->Shell()->Dialogs()->ShowOpenFile(
            foundation::shell::DialogResultCallback{
                [target](Span<const String> paths)
                {
                    if (paths.Size() == 0)
                    {
                        return;
                    } // cancelled
                    String text(target->Text());
                    for (usize i = 0; i < paths.Size(); ++i)
                    {
                        if (!text.IsEmpty() && text[text.Size() - 1] != utf8char(';'))
                        {
                            text += u8";";
                        }
                        text += paths[i].AsView();
                    }
                    target->SetText(text.AsView());
                }},
            {}, {}, /*allowMultiple*/ true);
    }

    void EditorApplication::ImportTemplateThenRefresh(ui::Dialog* current)
    {
        if (m_host == nullptr || m_host->Shell() == nullptr ||
            m_host->Shell()->Dialogs() == nullptr)
        {
            m_context.Notify(editor::NoticeKind::Error, u8"File dialogs are unavailable.");
            return;
        }
        m_host->Shell()->Dialogs()->ShowOpenFolder(foundation::shell::DialogResultCallback{
            [this, current](Span<const String> paths)
            {
                if (paths.Size() == 0)
                {
                    return;
                } // cancelled - leave the manager open
                const String root = TemplatesRoot();
                String id;
                if (editor::ImportTemplate(paths[0].AsView(), root.AsView(), &id).IsOk())
                {
                    String msg(u8"Imported template '");
                    msg += id;
                    msg += u8"'.";
                    m_context.Notify(editor::NoticeKind::Success, msg.AsView());
                }
                else
                {
                    m_context.Notify(editor::NoticeKind::Error,
                                     u8"Import failed - the folder has no valid template.xml.");
                }
                ReopenTemplatesManager(current);
            }});
    }

    void EditorApplication::CreateTemplateThenRefresh(ui::Dialog* current)
    {
        if (m_host == nullptr || m_host->Shell() == nullptr ||
            m_host->Shell()->Dialogs() == nullptr)
        {
            m_context.Notify(editor::NoticeKind::Error, u8"File dialogs are unavailable.");
            return;
        }
        m_host->Shell()->Dialogs()->ShowOpenFolder(foundation::shell::DialogResultCallback{
            [this, current](Span<const String> paths)
            {
                if (paths.Size() == 0)
                {
                    return;
                } // cancelled
                const String root = TemplatesRoot();
                String id, dir;
                if (editor::CreateTemplate(paths[0].AsView(), root.AsView(),
                                           editor::TemplateOutput::Install, &id, &dir)
                        .IsOk())
                {
                    String msg(u8"Created template '");
                    msg += id;
                    msg += u8"'.";
                    m_context.Notify(editor::NoticeKind::Success, msg.AsView());
                }
                else
                {
                    m_context.Notify(editor::NoticeKind::Error,
                                     u8"Create failed - pick a Bin/<Config> build dir "
                                     u8"containing Engine.Player.");
                }
                ReopenTemplatesManager(current);
            }});
    }

    void EditorApplication::OpenTemplatesManager()
    {
        if (!m_uiHost)
        {
            return;
        }

        editor::TemplateRegistry registry;
        BuildTemplateRegistryMainThread(registry);

        auto dialog =
            MakeRef<ui::Dialog>(m_editorAllocator, StringView(u8"Manage Export Templates"));
        dialog->MinWidth.SetValue(560.0f);
        dialog->MaxWidth.SetValue(780.0f);
        dialog->MinHeight.SetValue(240.0f);
        dialog->MaxHeight.SetValue(560.0f);
        ui::Dialog* raw = dialog.Get();

        auto column = MakeRef<ui::FlexLayout>(m_editorAllocator);
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = 6;

        auto header = MakeRef<ui::Label>(
            m_editorAllocator,
            StringView(u8"Installed export templates (the host build is always available):"));
        column->AddView(header.Get());

        for (usize i = 0; i < registry.Count(); ++i)
        {
            const editor::ExportTemplate* t = registry.At(i);
            if (t == nullptr)
            {
                continue;
            }
            String text(t->name.AsView());
            text += u8"  [";
            text += t->platform.AsView();
            text += u8"/";
            text += t->EffectiveConfig();
            text += u8"]";
            if (!t->engineVersion.IsEmpty())
            {
                text += u8"  v";
                text += t->engineVersion.AsView();
            }
            if (t->isHost)
            {
                text += u8"  (host)";
            }
            if (!editor::TemplateEngineMatches(*t))
            {
                text += u8"  (!) engine mismatch";
            }

            auto row = MakeRef<ui::FlexLayout>(m_editorAllocator);
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 8;
            {
                auto label = MakeRef<ui::Label>(m_editorAllocator, text.AsView());
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                lp.AlignSelf = ui::Align::Center;
                row->AddView(label.Get(), lp);
            }
            if (!t->isHost) // the host template is synthesized, never on disk => not removable
            {
                const String id(t->id.AsView());
                auto remove = MakeRef<ui::Button>(m_editorAllocator, StringView(u8"Remove"));
                remove->OnClick.Add(
                    [this, raw, id](ui::ButtonBase*)
                    {
                        const String root = TemplatesRoot();
                        if (editor::RemoveTemplate(root.AsView(), id.AsView()).IsOk())
                        {
                            String msg(u8"Removed template '");
                            msg += id;
                            msg += u8"'.";
                            m_context.Notify(editor::NoticeKind::Success, msg.AsView());
                        }
                        else
                        {
                            m_context.Notify(editor::NoticeKind::Error,
                                             u8"Remove failed (see console).");
                        }
                        ReopenTemplatesManager(raw);
                    });
                row->AddView(remove.Get());
            }
            column->AddView(row.Get());
        }

        dialog->SetContent(column.Get());

        ui::Button* import = dialog->AddButton(u8"Import...", ui::DialogResult::None);
        import->OnClick.Add([this, raw](ui::ButtonBase*) { ImportTemplateThenRefresh(raw); });
        ui::Button* create = dialog->AddButton(u8"Create...", ui::DialogResult::None);
        create->OnClick.Add([this, raw](ui::ButtonBase*) { CreateTemplateThenRefresh(raw); });
        dialog->AddButton(u8"Close", ui::DialogResult::Cancel);
        dialog->Show(&m_uiHost->Context());
    }

    void EditorApplication::OpenExportPresetsPanel()
    {
        if (!m_project)
        {
            m_context.Notify(editor::NoticeKind::Info, u8"Open a project first.");
            return;
        }
        if (!m_uiHost)
        {
            return;
        }

        {
            foundation::vfs::NativeFileSystem projectFs(m_project->Directory(), m_editorAllocator);
            m_presetsController.Load(projectFs); // reflects edits persisted by the editor form
        }

        auto dialog = MakeRef<ui::Dialog>(m_editorAllocator, StringView(u8"Export"));
        dialog->MinWidth.SetValue(600.0f);
        dialog->MaxWidth.SetValue(820.0f);
        dialog->MinHeight.SetValue(220.0f);
        dialog->MaxHeight.SetValue(560.0f);
        ui::Dialog* raw = dialog.Get();

        auto column = MakeRef<ui::FlexLayout>(m_editorAllocator);
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = 6;

        auto info = MakeRef<ui::Label>(
            m_editorAllocator, StringView(u8"Export presets (output directory: <project>/Dist):"));
        column->AddView(info.Get());

        for (usize i = 0; i < m_presetsController.Count(); ++i)
        {
            const editor::ExportPreset& p = m_presetsController.At(i);
            String text(p.name.AsView());
            text += u8"  [";
            text += p.platform.IsEmpty() ? StringView(u8"?") : p.platform.AsView();
            text += u8"/";
            text += p.config.IsEmpty() ? StringView(u8"Release") : p.config.AsView();
            text += u8"]";

            auto row = MakeRef<ui::FlexLayout>(m_editorAllocator);
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 6;
            {
                auto label = MakeRef<ui::Label>(m_editorAllocator, text.AsView());
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                lp.AlignSelf = ui::Align::Center;
                row->AddView(label.Get(), lp);
            }
            const String name(p.name.AsView());
            const usize index = i;
            {
                auto b = MakeRef<ui::Button>(m_editorAllocator, StringView(u8"Export"));
                b->OnClick.Add(
                    [this, raw, name](ui::ButtonBase*)
                    {
                        RunExport(name.AsView(), false);
                        raw->Close(ui::DialogResult::OK);
                    });
                row->AddView(b.Get());
            }
            {
                auto b = MakeRef<ui::Button>(m_editorAllocator, StringView(u8"Edit"));
                b->OnClick.Add(
                    [this, raw, index](ui::ButtonBase*)
                    {
                        editor::ExportPreset current = m_presetsController.At(index);
                        QueueReplaceDialog(
                            raw,
                            Function<void()>{
                                [this, current, index]()
                                { OpenPresetEditor(current, static_cast<isize>(index)); }});
                    });
                row->AddView(b.Get());
            }
            {
                auto b = MakeRef<ui::Button>(m_editorAllocator, StringView(u8"Duplicate"));
                b->OnClick.Add(
                    [this, raw, index](ui::ButtonBase*)
                    {
                        m_presetsController.Duplicate(index);
                        SavePresetsController();
                        ReopenExportPresetsPanel(raw);
                    });
                row->AddView(b.Get());
            }
            {
                auto b = MakeRef<ui::Button>(m_editorAllocator, StringView(u8"Delete"));
                b->OnClick.Add(
                    [this, raw, index](ui::ButtonBase*)
                    {
                        m_presetsController.Remove(index);
                        SavePresetsController();
                        ReopenExportPresetsPanel(raw);
                    });
                row->AddView(b.Get());
            }
            column->AddView(row.Get());
        }

        dialog->SetContent(column.Get());

        ui::Button* add = dialog->AddButton(u8"Add...", ui::DialogResult::None);
        add->OnClick.Add(
            [this, raw](ui::ButtonBase*)
            {
                editor::ExportPreset fresh;
                fresh.name = String(u8"New Preset");
                fresh.platform = String(GetHostPlatformName());
                QueueReplaceDialog(
                    raw, Function<void()>{[this, fresh]() { OpenPresetEditor(fresh, -1); }});
            });
        ui::Button* exportAll = dialog->AddButton(u8"Export All", ui::DialogResult::None);
        exportAll->OnClick.Add(
            [this, raw](ui::ButtonBase*)
            {
                RunExport(StringView{}, true);
                raw->Close(ui::DialogResult::OK);
            });
        ui::Button* templates = dialog->AddButton(u8"Manage Templates...", ui::DialogResult::None);
        templates->OnClick.Add(
            [this, raw](ui::ButtonBase*)
            { QueueReplaceDialog(raw, Function<void()>{[this]() { OpenTemplatesManager(); }}); });
        dialog->AddButton(u8"Close", ui::DialogResult::Cancel);
        dialog->Show(&m_uiHost->Context());
    }

    void EditorApplication::OpenPresetEditor(editor::ExportPreset initial, isize editIndex)
    {
        if (!m_uiHost)
        {
            return;
        }

        editor::TemplateRegistry registry;
        BuildTemplateRegistryMainThread(registry);

        auto dialog = MakeRef<ui::Dialog>(
            m_editorAllocator,
            StringView(editIndex < 0 ? u8"Add Export Preset" : u8"Edit Export Preset"));
        dialog->MinWidth.SetValue(600.0f);
        dialog->MaxWidth.SetValue(820.0f);
        dialog->MinHeight.SetValue(340.0f);
        dialog->MaxHeight.SetValue(640.0f);
        ui::Dialog* raw = dialog.Get();

        auto column = MakeRef<ui::FlexLayout>(m_editorAllocator);
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = 6;

        auto nameEdit = MakeRef<ui::EditText>(m_editorAllocator);
        nameEdit->SetText(initial.name.AsView());
        AddFormRow(*column, u8"Name", nameEdit.Get());

        // Template dropdown: index 0 = resolve by platform/config; each later item maps to a
        // concrete templateId (+ its platform/config), captured into the parallel arrays below.
        auto templateCombo = MakeRef<ui::ComboBox>(m_editorAllocator);
        templateCombo->AddItem(u8"(resolve by platform + config below)");
        Array<String> comboIds, comboPlatforms, comboConfigs;
        comboIds.PushBack(String{});
        comboPlatforms.PushBack(String{});
        comboConfigs.PushBack(String{});
        i32 selectedCombo = 0;
        for (usize i = 0; i < registry.Count(); ++i)
        {
            const editor::ExportTemplate* t = registry.At(i);
            if (t == nullptr)
            {
                continue;
            }
            String item(t->name.AsView());
            item += u8" [";
            item += t->platform.AsView();
            item += u8"/";
            item += t->EffectiveConfig();
            item += u8"]";
            if (t->isHost)
            {
                item += u8" (host)";
            }
            const i32 idx = templateCombo->AddItem(item.AsView());
            comboIds.PushBack(String(t->id.AsView()));
            comboPlatforms.PushBack(String(t->platform.AsView()));
            comboConfigs.PushBack(String(t->EffectiveConfig()));
            if (!initial.templateId.IsEmpty() && initial.templateId.AsView() == t->id.AsView())
            {
                selectedCombo = idx;
            }
        }
        templateCombo->SetSelectedIndex(selectedCombo);
        AddFormRow(*column, u8"Template", templateCombo.Get());

        auto platformEdit = MakeRef<ui::EditText>(m_editorAllocator);
        platformEdit->SetText(initial.platform.AsView());
        platformEdit->SetPlaceholder(GetHostPlatformName());
        AddFormRow(*column, u8"Platform", platformEdit.Get());

        auto configEdit = MakeRef<ui::EditText>(m_editorAllocator);
        configEdit->SetText(initial.config.AsView());
        configEdit->SetPlaceholder(u8"Release");
        AddFormRow(*column, u8"Config", configEdit.Get());

        auto playerEdit = MakeRef<ui::EditText>(m_editorAllocator);
        playerEdit->SetText(initial.playerName.AsView());
        playerEdit->SetPlaceholder(u8"(template default)");
        AddFormRow(*column, u8"Player name", playerEdit.Get());

        auto subdirEdit = MakeRef<ui::EditText>(m_editorAllocator);
        subdirEdit->SetText(initial.outputSubdir.AsView());
        subdirEdit->SetPlaceholder(u8"(sanitized name)");
        AddFormRow(*column, u8"Output subdir", subdirEdit.Get());

        auto filesEdit = MakeRef<ui::EditText>(m_editorAllocator);
        filesEdit->SetText(JoinSemicolons(initial.additionalFiles).AsView());
        filesEdit->SetPlaceholder(u8"icon.ico;config.xml");
        ui::FlexLayout* filesRow = AddFormRow(*column, u8"Extra files", filesEdit.Get());
        {
            RefPtr<ui::EditText> filesRef = filesEdit;
            auto browse = MakeRef<ui::Button>(m_editorAllocator, StringView(u8"Add Files..."));
            browse->OnClick.Add([this, filesRef](ui::ButtonBase*)
                                { PickAdditionalFiles(filesRef); });
            filesRow->AddView(browse.Get());
        }

        auto symbolsCheck = MakeRef<ui::CheckBox>(m_editorAllocator,
                                                  StringView(u8"Stage debug symbols into the dist"),
                                                  initial.stageSymbols);
        column->AddView(symbolsCheck.Get());
        auto pruneCheck = MakeRef<ui::CheckBox>(m_editorAllocator,
                                                StringView(u8"Prune to reachable content only"),
                                                initial.pruneToReachable);
        column->AddView(pruneCheck.Get());

        // The display, per platform: this preset's own render size or window over the project's
        // (a handheld's native panel, fullscreen on a console-like device). The choices are the
        // enums' reflected values.
        struct DisplayControls
        {
            ui::CheckBox* overridesRender = nullptr;
            ui::NumericField* renderWidth = nullptr;
            ui::NumericField* renderHeight = nullptr;
            ui::ComboBox* renderFit = nullptr;
            ui::CheckBox* overridesWindow = nullptr;
            ui::NumericField* windowWidth = nullptr;
            ui::NumericField* windowHeight = nullptr;
            ui::ComboBox* windowMode = nullptr;
            ui::CheckBox* windowResizable = nullptr;
        };
        DisplayControls display;
        {
            const auto sizeField = [this](u32 value, f64 least)
            {
                auto field = MakeRef<ui::NumericField>(m_editorAllocator);
                field->SetDecimalPlaces(0);
                field->SetStep(1.0);
                field->SetMin(least);
                field->SetMax(16384.0);
                field->SetValue(static_cast<f64>(value));
                return field;
            };
            const auto enumCombo = [this](const TypeInfo& type, i64 current)
            {
                auto combo = MakeRef<ui::ComboBox>(m_editorAllocator);
                i32 selected = 0;
                for (usize i = 0; i < EnumeratorCount(type); ++i)
                {
                    (void)combo->AddItem(StringView(reinterpret_cast<const utf8char*>(EnumeratorAt(type, i).name)));
                    selected = EnumeratorAt(type, i).value == current ? static_cast<i32>(i) : selected;
                }
                combo->SetSelectedIndex(selected);
                return combo;
            };
            const auto pair = [this](ui::View* a, ui::View* b)
            {
                auto row = MakeRef<ui::FlexLayout>(m_editorAllocator);
                row->Direction = ui::Orientation::Horizontal;
                row->Spacing = 6;
                ui::LayoutStyle fixed;
                fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(90));
                row->AddView(a, fixed);
                row->AddView(b, fixed);
                return row;
            };
            const TypeInfo& settingsType = engine::project::ProjectSettings::StaticType(); // the enums
            const TypeInfo& fitType = *FindProperty(settingsType, "renderFit")->type;
            const TypeInfo& modeType = *FindProperty(settingsType, "windowMode")->type;

            auto overridesRender = MakeRef<ui::CheckBox>(
                m_editorAllocator, StringView(u8"Draw at its own render size"), initial.overridesRender);
            column->AddView(overridesRender.Get());
            auto renderWidth = sizeField(initial.renderWidth, 0.0);
            auto renderHeight = sizeField(initial.renderHeight, 0.0);
            AddFormRow(*column, u8"Render size", pair(renderWidth.Get(), renderHeight.Get()).Get());
            auto renderFit = enumCombo(fitType, static_cast<i64>(initial.renderFit));
            AddFormRow(*column, u8"Render fit", renderFit.Get());

            auto overridesWindow = MakeRef<ui::CheckBox>(
                m_editorAllocator, StringView(u8"Open its own window"), initial.overridesWindow);
            column->AddView(overridesWindow.Get());
            auto windowWidth = sizeField(initial.windowWidth, 1.0);
            auto windowHeight = sizeField(initial.windowHeight, 1.0);
            AddFormRow(*column, u8"Window size", pair(windowWidth.Get(), windowHeight.Get()).Get());
            auto windowMode = enumCombo(modeType, static_cast<i64>(initial.windowMode));
            AddFormRow(*column, u8"Window mode", windowMode.Get());
            auto windowResizable =
                MakeRef<ui::CheckBox>(m_editorAllocator, StringView(u8"Resizable"), initial.windowResizable);
            column->AddView(windowResizable.Get());

            display = DisplayControls{overridesRender.Get(), renderWidth.Get(), renderHeight.Get(),
                                      renderFit.Get(), overridesWindow.Get(), windowWidth.Get(),
                                      windowHeight.Get(), windowMode.Get(), windowResizable.Get()};
        }

        dialog->SetContent(column.Get());

        ui::EditText* nameRaw = nameEdit.Get();
        ui::ComboBox* comboRaw = templateCombo.Get();
        ui::EditText* platformRaw = platformEdit.Get();
        ui::EditText* configRaw = configEdit.Get();
        ui::EditText* playerRaw = playerEdit.Get();
        ui::EditText* subdirRaw = subdirEdit.Get();
        ui::EditText* filesRaw = filesEdit.Get();
        ui::CheckBox* symbolsRaw = symbolsCheck.Get();
        ui::CheckBox* pruneRaw = pruneCheck.Get();

        ui::Button* save = dialog->AddButton(u8"Save", ui::DialogResult::None);
        save->OnClick.Add(
            [this, raw, editIndex, nameRaw, comboRaw, platformRaw, configRaw, playerRaw, subdirRaw,
             filesRaw, symbolsRaw, pruneRaw, comboIds, comboPlatforms, comboConfigs,
             display](ui::ButtonBase*)
            {
                editor::ExportPreset result;
                result.name = String(nameRaw->Text());
                const i32 sel = comboRaw->SelectedIndex();
                if (sel > 0 && static_cast<usize>(sel) < comboIds.Size())
                {
                    result.templateId = comboIds[static_cast<usize>(sel)];
                    result.platform = comboPlatforms[static_cast<usize>(sel)];
                    result.config = comboConfigs[static_cast<usize>(sel)];
                }
                else
                {
                    result.platform = String(platformRaw->Text());
                    result.config = String(configRaw->Text());
                }
                result.playerName = String(playerRaw->Text());
                result.outputSubdir = String(subdirRaw->Text());
                result.stageSymbols = symbolsRaw->IsChecked.Value();
                result.pruneToReachable = pruneRaw->IsChecked.Value();
                SplitSemicolons(filesRaw->Text(), result.additionalFiles);
                const TypeInfo& settingsType = engine::project::ProjectSettings::StaticType();
                const auto chosen = [&settingsType](ui::ComboBox* combo, const char* property)
                {
                    const TypeInfo& type = *FindProperty(settingsType, property)->type;
                    const i32 index = combo->SelectedIndex();
                    return (index >= 0 && static_cast<usize>(index) < EnumeratorCount(type))
                               ? EnumeratorAt(type, static_cast<usize>(index)).value
                               : 0;
                };
                result.overridesRender = display.overridesRender->IsChecked.Value();
                result.renderWidth = static_cast<u32>(display.renderWidth->Value());
                result.renderHeight = static_cast<u32>(display.renderHeight->Value());
                result.renderFit = static_cast<FitMode>(chosen(display.renderFit, "renderFit"));
                result.overridesWindow = display.overridesWindow->IsChecked.Value();
                result.windowWidth = static_cast<u32>(display.windowWidth->Value());
                result.windowHeight = static_cast<u32>(display.windowHeight->Value());
                result.windowMode =
                    static_cast<engine::project::WindowMode>(chosen(display.windowMode, "windowMode"));
                result.windowResizable = display.windowResizable->IsChecked.Value();

                if (editIndex < 0)
                {
                    m_presetsController.Add(result);
                }
                else
                {
                    m_presetsController.Update(static_cast<usize>(editIndex), result);
                }
                SavePresetsController();
                ReopenExportPresetsPanel(raw);
            });
        ui::Button* cancel = dialog->AddButton(u8"Cancel", ui::DialogResult::None);
        cancel->OnClick.Add([this, raw](ui::ButtonBase*) { ReopenExportPresetsPanel(raw); });
        dialog->Show(&m_uiHost->Context());
    }

    void EditorApplication::SavePageAs(editor::EditorPage& subject)
    {
        editor::EditorPage* page = &subject;
        if (m_project.Get() == nullptr || m_uiHost.Get() == nullptr)
        {
            return;
        }
        foundation::content::Instance* original =
            m_project->SourceDb().GetInstance(page->InstanceId());
        if (original == nullptr)
        {
            m_context.Notify(editor::NoticeKind::Warning,
                             u8"This page has no source asset to copy.");
            return;
        }
        const TypeInfo* type = GlobalTypeRegistry().FindByName(
            reinterpret_cast<const char*>(String(original->TypeNamespace()).CStr()),
            reinterpret_cast<const char*>(String(original->TypeName()).CStr()));
        if (type == nullptr)
        {
            m_context.Notify(editor::NoticeKind::Error, u8"Save As: unknown asset type.");
            return;
        }

        foundation::content::Group* group = &original->OwningGroup();
        String suggested(original->Name());
        suggested += u8" Copy";
        while (group->GetInstance(suggested.AsView()) != nullptr)
        {
            suggested += u8" Copy";
        }

        String prompt(u8"New name (created next to '");
        prompt += original->Name();
        prompt += u8"'):";
        RefPtr<foundation::ui::Dialog> dialog =
            MakeRef<foundation::ui::Dialog>(m_editorAllocator, StringView(u8"Save As"));
        auto column = MakeRef<foundation::ui::FlexLayout>(m_editorAllocator);
        column->Direction = foundation::ui::Orientation::Vertical;
        column->Spacing = 6.0f;
        RefPtr<foundation::ui::Label> label =
            MakeRef<foundation::ui::Label>(m_editorAllocator, prompt.AsView());
        label->WordWrap.SetValue(true);
        {
            foundation::ui::LayoutStyle lp;
            lp.Width = foundation::ui::SizeSpec::Match();
            column->AddView(label.Get(), lp);
        }
        auto nameEdit = MakeRef<foundation::ui::EditText>(m_editorAllocator);
        nameEdit->SetText(suggested.AsView());
        {
            foundation::ui::LayoutStyle lp;
            lp.Width = foundation::ui::SizeSpec::Match();
            column->AddView(nameEdit.Get(), lp);
        }
        // Inline validation line: empty until a rejected attempt; the dialog stays up.
        auto errorLabel = MakeRef<foundation::ui::Label>(m_editorAllocator);
        errorLabel->WordWrap.SetValue(true);
        errorLabel->TextColor.SetValue(Color{0.90f, 0.35f, 0.35f, 1.0f});
        {
            foundation::ui::LayoutStyle lp;
            lp.Width = foundation::ui::SizeSpec::Match();
            column->AddView(errorLabel.Get(), lp);
        }
        dialog->SetContent(column.Get());

        foundation::ui::Dialog* rawDialog = dialog.Get();
        foundation::ui::EditText* rawEdit = nameEdit.Get();
        foundation::ui::Label* rawError = errorLabel.Get();
        const Guid pageId = page->InstanceId();
        foundation::ui::Button* save = dialog->AddButton(u8"Save", foundation::ui::DialogResult::None);
        save->OnClick.Add(
            [this, pageId, group, type, rawDialog, rawEdit, rawError](foundation::ui::ButtonBase*)
            {
                const StringView newName = rawEdit->Text();
                if (newName.IsEmpty())
                {
                    rawError->SetText(u8"NOT saved: enter a name.");
                    return; // dialog stays up for the retry
                }
                if (group->GetInstance(newName) != nullptr)
                {
                    rawError->SetText(u8"NOT saved: that name already exists in the group.");
                    return; // dialog stays up for the retry
                }
                // Re-resolve the page: the dialog is modal-ish but pages can close under it.
                editor::EditorPage* target = nullptr;
                for (const UniquePtr<editor::EditorPage>& open : m_context.OpenPages())
                {
                    if (open->InstanceId() == pageId)
                    {
                        target = open.Get();
                        break;
                    }
                }
                if (target == nullptr)
                {
                    rawDialog->Close(foundation::ui::DialogResult::Cancel);
                    return;
                }
                foundation::content::Instance* fresh = group->CreateInstance(newName, *type);
                if (fresh == nullptr)
                {
                    m_context.Notify(editor::NoticeKind::Error,
                                     u8"NOT saved: could not create the new asset.");
                    return;
                }
                target->OnSavedAs(*fresh);
                if (target->Save().IsOk())
                {
                    String message(u8"Saved as '");
                    message += fresh->Name();
                    message += u8"'.";
                    m_context.Notify(editor::NoticeKind::Success, message.AsView());
                }
                else
                {
                    m_context.Notify(editor::NoticeKind::Error, u8"Save As FAILED (see Console).");
                }
                rawDialog->Close(foundation::ui::DialogResult::OK);
            });
        dialog->AddButton(u8"Cancel", foundation::ui::DialogResult::Cancel);
        dialog->Show(&m_uiHost->Context());
    }

    void EditorApplication::ShowDirtyCloseDialog(UIEditorPage* page,
                                                 ui::toolkit::DockablePanel* panel)
    {
        String message(u8"'");
        message += page->Title();
        message += u8"' has unsaved changes.";
        RefPtr<foundation::ui::Dialog> dialog =
            MakeRef<foundation::ui::Dialog>(m_editorAllocator, StringView(u8"Unsaved changes"));
        RefPtr<foundation::ui::Label> label =
            MakeRef<foundation::ui::Label>(m_editorAllocator, message.AsView());
        label->WordWrap.SetValue(true);
        dialog->SetContent(label.Get());

        foundation::ui::Dialog* rawDialog = dialog.Get();
        foundation::ui::Button* save = dialog->AddButton(u8"Save", foundation::ui::DialogResult::None);
        save->OnClick.Add(
            [this, page, panel, rawDialog](foundation::ui::ButtonBase*)
            {
                if (page->Save().IsOk())
                {
                    panel->OnCloseRequested.Invoke(panel);
                    rawDialog->Close(foundation::ui::DialogResult::OK);
                }
                else
                {
                    m_context.Notify(editor::NoticeKind::Error,
                                     u8"Save FAILED (see console) - page stays open.");
                    rawDialog->Close(foundation::ui::DialogResult::Cancel);
                }
            });
        foundation::ui::Button* discard =
            dialog->AddButton(u8"Discard", foundation::ui::DialogResult::None);
        discard->OnClick.Add(
            [panel, rawDialog](foundation::ui::ButtonBase*)
            {
                panel->OnCloseRequested.Invoke(panel);
                rawDialog->Close(foundation::ui::DialogResult::OK);
            });
        dialog->AddButton(u8"Cancel", foundation::ui::DialogResult::Cancel);
        dialog->Show(&m_uiHost->Context());
    }

    void EditorApplication::SyncPageTitles()
    {
        for (const PagePanel& entry : m_pagePanels)
        {
            String title;
            foundation::content::Instance* instance =
                m_project ? m_project->SourceDb().GetInstance(entry.page->InstanceId()) : nullptr;
            if (instance != nullptr)
            {
                title = String(instance->Name());
            }
            else
            {
                title = String(entry.page->Title());
            }
            if (entry.page->IsDirty())
            {
                title += u8" *";
            }
            if (entry.panel->Title() != title.AsView())
            {
                entry.panel->SetTitle(title.AsView());
            }
        }
    }

    void EditorApplication::DrainLog()
    {
        if (m_config.logBuffer == nullptr || m_shell.Console() == nullptr)
        {
            return;
        }
        m_pendingLog.Clear();
        m_logSequence = m_config.logBuffer->CollectSince(m_logSequence, m_pendingLog);
        for (const editor::EditorLogEntry& entry : m_pendingLog)
        {
            m_shell.Console()->AddEntry(entry.level, entry.category.AsView(),
                                        entry.message.AsView());
        }
    }

    void EditorApplication::OpenProjectAt(StringView directory)
    {
        if (directory.IsEmpty())
        {
            m_context.SetStatus(u8"No project directory - pass one on the command line.");
            return;
        }

        m_project = editor::EditorProject::Open(editor::EditorRootAllocator(), directory);
        if (!m_project && !m_config.startInProjectManager)
        {
            // CLI launch keeps the historical scaffold fallback (a bare directory becomes a
            // fresh project). The manager scaffolds only through its explicit New Project flow.
            const Status created =
                editor::EditorProject::Create(editor::EditorRootAllocator(), directory,
                                              m_config.projectName.AsView());
            if (created.IsOk())
            {
                m_project = editor::EditorProject::Open(editor::EditorRootAllocator(), directory);
                m_seedAfterOpen = m_project && m_config.seedOnScaffold;
            }
        }

        if (!m_project)
        {
            String message(u8"Failed to open project: ");
            message += directory;
            m_context.SetStatus(message.AsView());
            if (m_managerView)
            {
                m_managerView->SetStatus(message.AsView());
            }
            return;
        }

        m_context.SetProject(m_project.Get());
        {
            // Thumbnail cache under the project's gitignored .cache.
            String thumbsDir = Format(u8"{}/{}/thumbs", directory,
                                      engine::project::kProjectCacheDir);
            (void)foundation::core::CreateDirectory(thumbsDir.AsView());
            editor::EditorProject* project = m_project.Get();
            m_thumbnailService.Configure(
                thumbsDir.AsView(),
                Function<foundation::content::Instance*(const Guid&)>{
                    [project](const Guid& id)
                    { return project->SourceDb().GetInstance(id); }},
                &m_jobService,
                Function<foundation::core::u64(const Guid&)>{
                    [this](const Guid& id) { return m_cookService.RecipeHashFor(id); }},
                m_project->SourcesRoot().AsView());
        }

        // I4b instrumentation: what the open-time header scans actually cost. The XML
        // factory DOM-parses each whole envelope to read three fields, so bytesOpened is
        // the real parse volume - the evidence that decides the partial-header-parse work.
        {
            const auto& src = m_project->SourceDb().LastScanStats();
            const auto& cooked = m_project->CookedDb().LastScanStats();
            LOG_INFO(u8"Editor",
                     u8"project open scan: sources {} envelopes / {} KB parsed, cooked {} "
                     u8"envelopes / {} KB parsed",
                     static_cast<u64>(src.envelopes), src.bytesOpened / 1024,
                     static_cast<u64>(cooked.envelopes), cooked.bytesOpened / 1024);
        }


        // Showing the manager? Swap the window over to the editor shell first, so the
        // status bar narrates the rest of the open.
        if (m_inManagerMode)
        {
            if (graphics::RenderWindow* mainRw = m_host->MainRenderWindow())
            {
                m_uiHost->DetachWindow(mainRw);
                m_uiHost->AttachWindow(mainRw, RefPtr<foundation::ui::RootView>(m_shell.Root()));
            }
            m_inManagerMode = false;
        }

        // The per-project editor-state STORE (one structured file: dock layout, favorites,
        // open pages, per-page prefs). Absent on a fresh project - sections read as defaults.
        m_projectEditorSettings = MakeUnique<foundation::settings::Settings>(m_editorAllocator, m_editorAllocator);
        const Status projectLoaded = LoadProjectEditorSettings(
            *m_projectEditorSettings, m_project->EditorStateRoot().AsView());
        if (!projectLoaded.IsOk() && projectLoaded.Code() != ErrorCode::NotFound)
        {
            // Same failure mode as the user-level store above: a partial load means Settings
            // aborted at an uninstantiable section, and a later save rewrites the file from the
            // gutted store - silently losing dock layout / favorites / open pages.
            LOG_ERROR(u8"Editor",
                      u8"editor.project.settings.xml load FAILED (partial store) - check for "
                      u8"unregistered section types; later saves may drop sections");
        }
        m_context.SetProjectEditorSettings(m_projectEditorSettings.Get());
        m_context.OnProjectEditorSettingsSaveRequested = [this]()
        {
            if (m_project && m_projectEditorSettings)
            {
                (void)SaveProjectEditorSettings(*m_projectEditorSettings,
                                                m_project->EditorStateRoot().AsView());
            }
        };

        // Per-user pinned assets (browser + picker surface them first).
        ApplyFavorites(m_context, *m_projectEditorSettings);
        m_context.OnFavoritesChanged = [this]()
        {
            if (m_project && m_projectEditorSettings)
            {
                CaptureFavorites(m_context, *m_projectEditorSettings);
                m_context.RequestProjectEditorSettingsSave();
            }
        };

        // Per-project resources over the cooked DB, late-attached to the embedded runtime,
        // which registers the engine composition's factory set into it - the same set a preset
        // manager receives at its startup.
        // Share the global JobSystem for async resource decode (task #123); null = sync loads.
        m_resources = MakeUnique<foundation::resource::ResourceManager>(
            m_editorAllocator, m_editorAllocator, m_project->CookedDb(),
            HasGlobalJobSystem() ? &GlobalJobs() : nullptr);
        m_context.SetResources(m_resources.Get());
        m_embeddedApp->AttachResourceManager(m_resources.Get(), *m_embeddedHost);

        // Native game module (game-native-code.md N2, editor half): loaded against the
        // EMBEDDED runtime context at project open - play-in-editor, Simulate, and
        // previews all resolve the plugin's registrations there. Placed after the
        // resource attach so OnLoad sees the full runtime; unloaded in CloseProject
        // BEFORE resources detach (game teardown first). Requires a shared-engine
        // editor build (the ruled plugin model); failure is a toast + console error
        // and the project still opens (scripts work regardless).
        if (!m_project->Settings().nativeModule.IsEmpty())
        {
            m_gamePlugins =
                MakeUnique<runtime::PluginHost>(m_editorAllocator, m_runtimeContext);
            m_gamePlugins->AddRecorder(&engine::scene::GlobalSceneContributionRecorder());
            // Through a staged copy, not the declared path: on Windows LoadLibraryW holds
            // the image open against writes, so mapping Native/<Target>.dll directly would
            // make "Build Native Module" unable to relink it until the project closes.
            // Reload needs the copy anyway (dlopen refcounts by path), so one path for both.
            const String modulePath = StageNativeModuleCopy();
            bool ok = false;
            if (!modulePath.IsEmpty())
            {
                auto loaded = m_gamePlugins->Load(modulePath.AsView());
                ok = loaded.HasValue();
                if (ok)
                {
                    LOG_INFO(u8"Editor", u8"native game module '{}' loaded ('{}')",
                             m_project->Settings().nativeModule, loaded.Value()->Name());
                }
            }
            if (!ok)
            {
                m_context.Notify(editor::NoticeKind::Error,
                                 u8"Native game module failed to load (see Console).");
                LOG_ERROR(u8"Editor",
                          u8"native game module '{}' failed to load - continuing without "
                          u8"it (a static editor build cannot load native modules; use a "
                          u8"shared build, or check the path)",
                          m_project->Settings().nativeModule);
                m_gamePlugins = nullptr;
            }
        }
        // The GPU half of thumbnails: constructed HERE, after the resource manager exists -
        // the stage captures it, and a null capture would stall every queued job. The host is
        // the EMBEDDED one: scene/render subsystems live on the embedded runtime context (the
        // same host every page and PreviewViewport receives), not on the outer editor host.
        // Destroyed in CloseProject BEFORE the service resets so an in-flight job unstages.
        m_thumbnailStage = MakeUnique<editor::ThumbnailStage>(
            m_editorAllocator, *m_embeddedHost, m_thumbnailService, m_resources.Get());
        // Scripted scene loads (run.loadScene/loadSceneAsync) resolve scene + prefab content
        // out of the project's SOURCE db - the same db the Game tab's default-scene boot reads
        // (products still bind from the cooked-backed manager above). Wired here at project open
        // so it covers every run (Game tab AND Simulate); cleared at project close (below).
        m_embeddedApp->SetContentDatabase(&m_project->SourceDb());
        // Project-default UI theme + font: bound via ApplyProjectUiDefaults - ALSO
        // re-fired after every finished cook (a fresh checkout's first cook creates the
        // products the open-time bind missed) and on settings save (a changed default
        // takes effect without a project reopen).
        ApplyProjectUiDefaults();
        if (m_embeddedApp->UI() != nullptr &&
            m_project->Settings().defaultUiFontId.IsNil() &&
            ProjectHasFontAssets(m_project->SourceDb().RootGroup()))
        {
            // Migration kindness: a real project with fonts but no default set renders no game-UI
            // text (the dev-tree probe resolves nothing outside this source tree). Name the fix.
            LOG_WARNING(u8"UI",
                                 u8"game UI has no default font - set Project Settings > Default "
                                 u8"UI font (the project has fonts, but none is the default, so "
                                 u8"game-UI text will not render)");
        }

        // Settings-derived session state re-applies on save (default font/theme -
        // without this a changed default kept the OLD bind until reopen).
        m_context.OnProjectSettingsChanged = [this]() { ApplyProjectUiDefaults(); };
        // The active page's panel carries the dock's ActiveMark (the accent strip and ring): the
        // page a command goes to, visible when two pages sit side by side. A tool panel never
        // takes it - the mark follows the context's active page, not the dock's clicked panel.
        m_context.OnPagesChanged = [this]() { RefreshActivePageMark(); };
        // A page revealed through the context (page_open, an agent's viewport_screenshot)
        // brings its tab to front: a background tab's viewport never renders.
        m_context.OnRevealPage = [this](editor::EditorPage* page)
        {
            for (const PagePanel& entry : m_pagePanels)
            {
                if (entry.page == page)
                {
                    m_shell.Docks()->ActivatePanel(entry.panel);
                    return;
                }
            }
        };

        // Cook service + the real Assets panel.
        m_cookService.Initialize(*m_project, m_builders);
        // Pages request re-cooks after saving builder-backed assets (materials etc.).
        m_context.OnCookRequested = [this](bool rebuild)
        { m_cookService.RequestCook(rebuild); };
        // Cook-gated starts (PIE waits for the cook): busy = anything in flight OR a
        // remembered mid-cook request still waiting to re-issue (IsIdle, not MutationLocked).
        m_context.CookBusy = [this]() { return m_cookService.IsReady() && !m_cookService.IsIdle(); };
        m_context.SourceAssetTypesOf = [this](const TypeInfo& product)
        { return editor::mcp::SourceAssetTypesFor(m_builders, product); };
        StartMcpHost(); // the agent surface over this project, if enabled
        // Background jobs (export) read the source DB structure and pack cooked FILES
        // from their worker - DB mutations and new cooks must hold off while one runs,
        // exactly like during a cook. The cook service folds this into MutationLocked.
        m_cookService.ExternalMutationLock = [this]() { return m_jobService.IsBusy(); };
        m_context.SetJobs(&m_jobService); // pages submit light work (preview bakes) here
        // Thumbnails: the app owns the SERVICE + lifecycle only
        // (SetThumbnails happens in the CONSTRUCTOR - the composition root registers
        // generators before this UI-boot phase runs); ready thumbnails rebind the browser and
        // whatever else listens on the context (an open asset picker); inspector slots re-query
        // per refresh.
        m_thumbnailService.OnThumbnailReady = [this](const Guid& id)
        {
            if (m_assetsView)
            {
                m_assetsView->RefreshThumbnail(id);
            }
            m_context.NotifyThumbnailReady(id);
        };
        m_assetsView =
            MakeRef<AssetsView>(m_editorAllocator, m_context, m_cookService, &m_jobService);
        AssetsView* assets = m_assetsView.Get();
        m_assetsView->OnOpenInstance = [this](foundation::content::Instance& instance)
        { (void)OpenInstancePage(instance); };
        // The asset-slot Edit affordance: Guid -> instance ->
        // the same page path the browser double-click takes. Grows a type branch (e.g.
        // property-animation clips -> the in-scene panel) for panel editing surfaces.
        m_context.RevealAsset = [assets](const Guid& id) { assets->Reveal(id); };
        m_context.OpenAsset = [this](const Guid& id)
        {
            if (m_project.Get() == nullptr)
            {
                return;
            }
            if (foundation::content::Instance* instance = m_project->SourceDb().GetInstance(id))
            {
                // A claimant (e.g. the scene page's animation bar for clip assets) may take
                // the open; unclaimed falls through to the normal page path.
                if (m_context.TryInterceptOpenAsset(*instance))
                {
                    return;
                }
                (void)OpenInstancePage(*instance);
            }
        };
        m_assetsView->OnCreate =
            [this](const pipeline::AssetCreator& creator,
                   foundation::content::Group* group) { CreateAndOpen(creator, group); };
        // "Import..." in the asset-browser menu: open the native file dialog, then route each picked
        // file through ImportFile (importer chooser / options dialog / no-importer warning).
        m_assetsView->OnBrowseImport = [this]()
        {
            if (m_host == nullptr || m_host->Shell() == nullptr ||
                m_host->Shell()->Dialogs() == nullptr)
            {
                m_context.Notify(editor::NoticeKind::Error, u8"File dialogs are unavailable.");
                return;
            }
            m_host->Shell()->Dialogs()->ShowOpenFile(
                foundation::shell::DialogResultCallback{[this](Span<const String> paths)
                                                        {
                                                            if (m_assetsView.Get() == nullptr)
                                                            {
                                                                return;
                                                            }
                                                            m_assetsView->ImportFiles(paths);
                                                        }},
                {}, {}, /*allowMultiple*/ true);
        };
        // Delete-while-open policy: close-then-delete. Called from a mutation-queue
        // action (never mid-event-dispatch), so synchronous panel + page teardown is
        // safe here - the same pair of steps the tab close button triggers.
        m_assetsView->OnCloseInstancePage = [this](const Guid& id)
        {
            for (usize i = 0; i < m_pagePanels.Size(); ++i)
            {
                if (m_pagePanels[i].page->InstanceId() == id)
                {
                    ClosePanelAndPage(m_pagePanels[i]);
                    return;
                }
            }
        };
        m_cookService.OnCookFinished = [this, assets]()
        {
            m_thumbnailService.InvalidateAll(); // recipe hashes moved; disk absorbs unchanged
            assets->Rebuild();
            // Result toast: failures are sticky (Console has the log); silent when the
            // cook was a no-op (the watcher fires those constantly).
            const usize failed = m_cookService.LastCookSummary().failed;
            const usize cooked = m_cookService.LastCookSummary().cooked;
            if (failed > 0)
            {
                ShowToast(editor::NoticeKind::Error,
                          Format(u8"Cook: {} failed, {} cooked (see Console).", failed, cooked)
                              .AsView());
            }
            else if (cooked > 0)
            {
                ShowToast(editor::NoticeKind::Success,
                          Format(u8"Cook finished: {} asset(s).", cooked).AsView());
            }
            // Hot reload: rebuilt products swap in behind the proxy handles - live
            // scenes see the new resources with no reopen (dependents reload
            // transitively through the manager's recorded edges).
            if (m_resources)
            {
                for (const Guid& product : m_cookService.LastCookedProducts())
                {
                    (void)m_resources->Reload(product);
                }
            }
            // A finished cook may have CREATED products the project-open bind missed
            // (fresh checkout: the default UI font/theme cook after the open-time bind).
            // AFTER the reload: the game UI borrows the font product itself, so binding first
            // left it on the product a rebuilt font's reload then freed, and the next game UI
            // layout read freed memory.
            ApplyProjectUiDefaults();
        };
        m_shell.SetAssetsContent(m_assetsView.Get());

        // Reopen the pages from the last session (falling back to the default document),
        // THEN restore the dock layout so page panels land back in their arrangement
        // (panels carry guid PersistenceIds; the layout only reconstitutes once they exist).
        {
            Array<Guid> pages;
            Guid activePage;
            if (ApplyOpenPages(*m_projectEditorSettings, pages, activePage).IsOk())
            {
                UIEditorPage* toActivate = nullptr;
                for (const Guid& id : pages)
                {
                    if (foundation::content::Instance* instance =
                            m_project->SourceDb().GetInstance(id))
                    {
                        UIEditorPage* page = OpenInstancePage(*instance);
                        if (page != nullptr && id == activePage)
                        {
                            toActivate = page;
                        }
                    }
                }
                if (toActivate != nullptr)
                {
                    m_context.SetActivePage(toActivate);
                }
            }
            else
            {
                // No saved page set (first launch): fall back to the default scene -
                // guid first (authoritative), path mirror for guid-less manifests.
                foundation::content::Instance* instance = nullptr;
                if (!m_project->Settings().defaultSceneId.IsNil())
                {
                    instance =
                        m_project->SourceDb().GetInstance(m_project->Settings().defaultSceneId);
                }
                if (instance == nullptr && !m_project->Settings().defaultScene.IsEmpty())
                {
                    instance = m_project->SourceDb().GetInstance(
                        m_project->Settings().defaultScene.AsView());
                }
                if (instance != nullptr)
                {
                    (void)OpenInstancePage(*instance);
                }
            }
            (void)m_shell.RestoreLayout(*m_projectEditorSettings);
        }

        // Record the open in the per-user registry (most-recent-first; snapshot refreshed).
        m_projectManager.NoteOpened(directory, m_project->Name(),
                                    m_project->Settings().engineVersion.AsView());
        (void)editor::SaveEditorSettingsToUserData(m_editorSettings);

        // Starter content for a manager-created project (baseline font/sky/meshes + the
        // manifest defaults). Once, before the first cook pass picks everything up.
        if (m_seedAfterOpen)
        {
            m_seedAfterOpen = false;
            if (m_config.seedNewProject)
            {
                m_config.seedNewProject(m_context, *m_project);
                (void)m_project->SaveSettings(); // the seed set manifest defaults
                if (m_assetsView)
                {
                    m_assetsView->Rebuild();
                }
                m_cookService.RequestCook(false);
                m_context.SetStatus(u8"Project created with starter content.");
            }
        }

        String message(u8"Project: ");
        message += m_project->Name();
        message += u8"  (";
        message += m_project->Directory();
        message += u8")";
        m_context.SetStatus(message.AsView());
    }

    String EditorApplication::StageNativeModuleCopy()
    {
        // The manifest holds ONE path, but a project is checked into source control and
        // opened on BOTH platforms, where the same module is Native/libX.so and Native/X.dll.
        // Prefer the declared path; when it is absent, fall back to this platform's spelling
        // of the same target, so a Linux-authored project still loads its module on Windows
        // (and vice versa) without a per-platform manifest. shared-libraries.md W3.
        const StringView declared = m_project->Settings().nativeModule.AsView();
        String src = PathJoin(m_project->Directory(), declared);
        if (!foundation::core::FileExists(src.AsView()))
        {
            const String target = editor::detail::NativeTargetFromModulePath(declared);
            if (!target.IsEmpty())
            {
#if PLATFORM_WINDOWS
                const String alt = Format(u8"Native/{}.dll", target);
#else
                const String alt = Format(u8"Native/lib{}.so", target);
#endif
                const String altPath = PathJoin(m_project->Directory(), alt.AsView());
                if (foundation::core::FileExists(altPath.AsView()))
                {
                    LOG_INFO(u8"Editor",
                             u8"native module '{}' not present; using this platform's '{}'",
                             declared, alt);
                    src = altPath;
                }
            }
        }
        auto bytes = foundation::core::ReadFile(src.AsView(), m_editorAllocator);
        if (!bytes.HasValue())
        {
            LOG_ERROR(u8"Editor", u8"native module: cannot read '{}'", src);
            return String{};
        }
        const String hotDir = PathJoin(
            m_project->Directory(),
            Format(u8"{}/native-hot", engine::project::kProjectCacheDir).AsView());
        (void)foundation::core::CreateDirectory(hotDir.AsView());
        // From the RESOLVED path, so the staged copy keeps the extension actually loaded
        // (a .dll staged as ".so" would still load, but every log line would lie).
        StringView base = src.AsView();
        for (usize i = base.Size(); i > 0; --i)
        {
            if (base.Data()[i - 1] == '/' || base.Data()[i - 1] == '\\')
            {
                base = StringView(base.Data() + i, base.Size() - i);
                break;
            }
        }
        String dst = Format(u8"{}/reload-{}-{}", hotDir, ++m_nativeReloadCount, base);
        if (!foundation::core::WriteFile(
                 dst.AsView(),
                 Span<const byte>{bytes.Value().Data(), bytes.Value().Size()})
                 .IsOk())
        {
            LOG_ERROR(u8"Editor", u8"native module: cannot stage '{}'", dst);
            return String{};
        }
        return dst;
    }

    void EditorApplication::ReloadNativeModule()
    {
        if (!m_project || m_project->Settings().nativeModule.IsEmpty())
        {
            m_context.Notify(editor::NoticeKind::Info,
                             u8"No native module declared (Project Settings > Native module).");
            return;
        }
        // Run bracket: a live game run executes plugin code - stop every one before teardown.
        for (const UniquePtr<EditorPage>& open : m_context.OpenPages())
        {
            if (IGameRunPage* run = ServiceOf<IGameRunPage>(open.Get()))
            {
                run->StopRun();
            }
        }
        // Scene bracket (game-native-code.md N6/S1): every live scene on the embedded runtime
        // is snapshotted through the wire format BEFORE the unload withdraws the plugin's
        // contributed managers/systems (their component pools die with them, while the code
        // that built them is still mapped), and restored AFTER the rebuild re-contributes
        // them - plugin components rehydrate by name through the new module's registrations.
        // Restore runs even if the reload fails: the scene is never left stripped (records
        // of the absent plugin's types stay preserved as unresolved).
        struct SceneBracket
        {
            foundation::scene::Scene* scene;
            UniquePtr<foundation::scene::SceneSnapshot> snapshot;
        };
        Array<SceneBracket> brackets;
        if (auto* scenes = m_runtimeContext.GetSubsystem<engine::scene::SceneSubsystem>())
        {
            scenes->Registry().ForEachScene(
                [&](foundation::scene::Scene& scene)
                {
                    SceneBracket b{&scene, foundation::scene::SceneSnapshot::Capture(scene)};
                    if (b.snapshot)
                    {
                        brackets.PushBack(Move(b));
                    }
                });
        }
        auto restoreScenes = [&]()
        {
            for (SceneBracket& b : brackets)
            {
                if (!b.snapshot->Restore(*b.scene, m_resources.Get()).IsOk())
                {
                    LOG_ERROR(u8"Editor", u8"native reload: scene '{}' failed to restore",
                              b.scene->Name());
                }
            }
            brackets.Clear();
        };
        // Scope-reversed unload; the OLD library stays mapped for the process lifetime
        // (leak-on-purpose: never free pages under a pointer the teardown missed).
        if (m_gamePlugins.Get() != nullptr)
        {
            m_gamePlugins->UnloadAll(/*closeLibraries*/ false);
            m_gamePlugins = nullptr;
        }

        const String dst = StageNativeModuleCopy();
        if (dst.IsEmpty())
        {
            m_context.Notify(editor::NoticeKind::Error,
                             u8"Native module reload failed - build it first (see Console).");
            restoreScenes();
            return;
        }

        m_gamePlugins = MakeUnique<runtime::PluginHost>(m_editorAllocator, m_runtimeContext);
        m_gamePlugins->AddRecorder(&engine::scene::GlobalSceneContributionRecorder());
        auto loaded = m_gamePlugins->Load(dst.AsView());
        restoreScenes(); // after the (re)contribution, so plugin components rehydrate
        if (loaded.HasValue())
        {
            m_context.Notify(editor::NoticeKind::Success,
                             u8"Native module reloaded.");
            LOG_INFO(u8"Editor", u8"native game module reloaded ('{}', copy {})",
                     loaded.Value()->Name(), static_cast<u64>(m_nativeReloadCount));
        }
        else
        {
            m_context.Notify(editor::NoticeKind::Error,
                             u8"Native module reload failed to load (see Console).");
            LOG_ERROR(u8"Editor", u8"native module reload: load failed for '{}'", dst);
            m_gamePlugins = nullptr;
        }
    }

    void EditorApplication::StartMcpHost()
    {
        StopMcpHost();
        editor::EditorMcpSettings& settings = m_editorSettings.Section<editor::EditorMcpSettings>();
        if (!(settings.enabled || m_config.mcpEnabled) || !m_project)
        {
            return;
        }
        if (m_config.logBuffer == nullptr)
        {
            LOG_WARNING(u8"Editor", u8"MCP: no log capture was installed - the host is not started");
            return;
        }
        if (settings.token.IsEmpty())
        {
            // First enable: mint the secret once and keep it, so the token file and the
            // Preferences display stay valid across runs.
            settings.token = editor::GenerateMcpToken();
            if (const Status saved = editor::SaveEditorSettingsToUserData(m_editorSettings);
                !saved.IsOk())
            {
                LOG_WARNING(u8"Editor", u8"MCP: the minted token could not be saved to the "
                                        u8"editor settings; it changes on the next run");
            }
        }
        editor::mcp::EngineToolPaths paths;
        const String starts[] = {GetExecutableDirectory(), GetCurrentDirectory()};
        editor::mcp::LocateShippingDocs(Span<const String>(starts, 2), paths);
        m_mcpSession.project = m_project.Get();
        // The operations run on THIS application's services, so an agent's cook, import or
        // export takes the same background paths the menus do and the editor stays live.
        EditorProjectOperationsSeams seams;
        seams.allocator = &m_editorAllocator;
        seams.project = m_project.Get();
        seams.context = &m_context;
        seams.cook = &m_cookService;
        seams.jobs = &m_jobService;
        seams.builders = &m_builders;
        seams.hostToolDir = GetExecutableDirectory();
        seams.templatesRoot = TemplatesRoot();
        seams.dataRoot = m_config.dataRoot;
        seams.onCreated = [this](const pipeline::AssetCreator& creator,
                                 foundation::content::Instance& instance)
        { AfterCreate(creator, instance); };
        seams.onDelete = [this](const Guid& id)
        { return m_assetsView.Get() != nullptr && m_assetsView->DeleteForAgent(id); };
        seams.onImported = [this](foundation::content::Instance& primary,
                                  const pipeline::ImportOptions* options)
        {
            if (m_assetsView.Get() != nullptr)
            {
                m_assetsView->AfterImport(primary, options);
            }
        };
        m_mcpOperations = MakeUnique<EditorProjectOperations>(m_editorAllocator, Move(seams));
        m_mcpHost = MakeUnique<EditorMcpHost>(
            m_editorAllocator, m_editorAllocator, m_context, m_mcpSession, *m_config.logBuffer,
            m_builders, m_context.Importers(), m_context.Creators(), paths, *m_mcpOperations,
            String(reinterpret_cast<const char8_t*>(BuildStamp())));
        m_mcpHost->OnToolFinished = [this](StringView tool, bool isError)
        { m_context.SetStatus(Format(u8"MCP: {} {}", tool, isError ? u8"failed" : u8"done")); };
        // This host's live additions: the pages, over the same open/close paths the tabs take.
        PageToolSeams pageSeams;
        pageSeams.context = &m_context;
        pageSeams.openPage = [this](const Guid& id) -> editor::EditorPage*
        {
            if (!m_project)
            {
                return nullptr;
            }
            foundation::content::Instance* instance = m_project->SourceDb().GetInstance(id);
            return instance != nullptr ? OpenInstancePage(*instance) : nullptr;
        };
        pageSeams.closePage = [this](editor::EditorPage* page)
        {
            for (const PagePanel& entry : m_pagePanels)
            {
                if (static_cast<editor::EditorPage*>(entry.page) == page)
                {
                    ClosePanelAndPage(entry);
                    return;
                }
            }
        };
        RegisterPageTools(m_mcpHost->Server(), Move(pageSeams));
        // editor_screenshot, over the same capture as View > Screenshot.
        WindowToolSeams windowSeams;
        windowSeams.windows = [this]() { return CapturableWindows(); };
        windowSeams.request = [this](Span<const WindowShotRequest> requests)
        { m_windowCapture.Request(requests); };
        windowSeams.shots = [this]() { return m_windowCapture.Shots(); };
        RegisterWindowTools(m_mcpHost->Server(), Move(windowSeams));
        // The action bridge: everything a user can do by command, unattended (the dialogs an
        // action opens are suppressed and reported).
        ActionToolSeams actionSeams;
        actionSeams.context = &m_context;
        actionSeams.ui = &m_uiHost->Context();
        RegisterActionTools(m_mcpHost->Server(), Move(actionSeams));
        EditorMcpHostConfig config;
        config.port = static_cast<u16>(m_config.mcpPort != 0 ? m_config.mcpPort : settings.port);
        config.token = settings.token;
        config.tokenFileDirectory = GetUserDataDirectory();
        if (!m_mcpHost->Start(config))
        {
            LOG_WARNING(u8"Editor",
                        u8"MCP: could not listen on 127.0.0.1:{} - another editor may hold the "
                        u8"port; pass --mcp-port <n> or change it in Preferences",
                        config.port);
            m_mcpHost = nullptr;
            return;
        }
        LOG_INFO(u8"Editor", u8"MCP: listening on 127.0.0.1:{} (the token is in <user-data>/{})",
                 m_mcpHost->BoundPort(), EditorMcpHost::kTokenFileName);
        m_context.SetStatus(Format(u8"MCP host on 127.0.0.1:{}", m_mcpHost->BoundPort()));
    }

    void EditorApplication::StopMcpHost()
    {
        m_mcpHost = nullptr; // Stop + release; a waiting agent sees its connection close
        m_mcpOperations = nullptr;
        m_mcpSession.project = nullptr;
    }

    void EditorApplication::CloseProject()
    {
        if (!m_project)
        {
            EnterManagerMode();
            return;
        }
        StopMcpHost();            // no agent call may run against services that are going away
        m_cookService.Shutdown(); // joins any in-flight cook before the DBs go away
        m_thumbnailStage = {};      // unstages + drops GPU objects while the renderer is alive
        m_thumbnailService.Reset(); // in-flight slots outlive harmlessly; entries drop
        SaveLayout();             // pages.bin + layout.xml for the next open
        // Close every page: the panel-and-page pair, applied to all. ClosePage erases the
        // entry from m_pagePanels, so drain from the front.
        while (!m_pagePanels.IsEmpty())
        {
            ClosePanelAndPage(m_pagePanels[0]);
        }
        m_gamePage = nullptr;
        m_shell.SetAssetsContent(nullptr);
        m_assetsView = nullptr;
        m_context.OnCookRequested = {};
        m_context.CookBusy = {};
        m_context.OnFavoritesChanged = {};
        // Native game teardown FIRST: OnUnload deregisters the module's subsystems while
        // the embedded runtime and resources are still alive (game-native-code.md N2).
        if (m_gamePlugins.Get() != nullptr)
        {
            m_gamePlugins->UnloadAll();
            m_gamePlugins = nullptr;
        }
        // Detach the per-project resources from the embedded runtime BEFORE destroying them
        // (its Resources() consumers are lazy and null-tolerant between projects).
        if (m_embeddedApp)
        {
            m_embeddedApp->AttachResourceManager(nullptr, *m_embeddedHost);
            // The source db belongs to m_project (destroyed below); null is the safe idle state
            // so a later frame or run never dereferences a freed db between projects.
            m_embeddedApp->SetContentDatabase(nullptr);
            if (m_embeddedApp->UI() != nullptr)
            {
                m_embeddedApp->UI()->SetDefaultTheme(nullptr);
            }
        }
        m_context.SetResources(nullptr);
        m_resources = nullptr;
        m_context.SetProjectEditorSettings(nullptr);
        m_context.OnProjectEditorSettingsSaveRequested = {};
        m_projectEditorSettings = nullptr;
        m_context.SetProject(nullptr);
        m_project = nullptr;
        m_context.SetStatus(u8"Project closed.");
        EnterManagerMode();
    }

    void EditorApplication::EnterManagerMode()
    {
        graphics::RenderWindow* mainRw = m_host != nullptr ? m_host->MainRenderWindow() : nullptr;
        if (mainRw == nullptr)
        {
            return;
        }
        if (!m_managerView)
        {
            m_managerView = MakeUnique<ProjectManagerView>(m_editorAllocator, m_editorAllocator);
            // Open/Create swap the window's root (detaching the manager view whose button is
            // mid-dispatch) - defer through the UI mutation queue, like Close Project.
            m_managerView->OnOpenProject = [this](StringView dir)
            {
                const String path(dir);
                m_uiHost->Context().MutationQueueRef().QueueAction(
                    Function<void()>{[this, path]() { OpenFromManager(path.AsView()); }});
            };
            m_managerView->OnCreateProject = [this](StringView dir, StringView name)
            {
                const String path(dir);
                const String projectName(name);
                m_uiHost->Context().MutationQueueRef().QueueAction(Function<void()>{
                    [this, path, projectName]()
                    { CreateFromManager(path.AsView(), projectName.AsView()); }});
            };
            m_managerView->OnStoreChanged = [this]()
            { (void)editor::SaveEditorSettingsToUserData(m_editorSettings); };
            m_managerView->Build(m_projectManager, m_host->Shell()->Dialogs(),
                                 &m_uiHost->Context(), mainRw->Window().Width(),
                                 mainRw->Window().Height());
        }
        else
        {
            m_managerView->Rebuild(); // returning from a project: re-probe the rows
        }
        if (!m_inManagerMode)
        {
            m_uiHost->DetachWindow(mainRw);
            m_uiHost->AttachWindow(mainRw, RefPtr<foundation::ui::RootView>(m_managerView->Root()));
            m_inManagerMode = true;
        }
    }

    void EditorApplication::OpenFromManager(StringView directory)
    {
        // The controller decides (probe + version relation + prompt copy); this method only
        // renders dialogs for the prompt gates and runs the open it owns.
        editor::ProjectManagerController::OpenDecision decision;
        m_projectManager.DecideOpen(directory, decision);
        if (decision.gate == editor::ProjectOpenGate::NotAProject)
        {
            if (m_managerView)
            {
                m_managerView->SetStatus(u8"Not a project (no Project.xml there).");
            }
            return;
        }
        if (decision.gate == editor::ProjectOpenGate::OpenDirectly)
        {
            OpenProjectAt(directory);
            return;
        }

        const String dir(directory);
        RefPtr<foundation::ui::Dialog> dialog =
            MakeRef<foundation::ui::Dialog>(m_editorAllocator, decision.promptTitle.AsView());
        RefPtr<foundation::ui::Label> label =
            MakeRef<foundation::ui::Label>(m_editorAllocator, decision.promptBody.AsView());
        label->WordWrap.SetValue(true);
        dialog->SetContent(label.Get());
        foundation::ui::Dialog* rawDialog = dialog.Get();

        if (decision.gate == editor::ProjectOpenGate::PromptNewerEngine)
        {
            foundation::ui::Button* openAnyway =
                dialog->AddButton(u8"Open Anyway", foundation::ui::DialogResult::None);
            openAnyway->OnClick.Add(
                [this, rawDialog, dir](foundation::ui::ButtonBase*)
                {
                    rawDialog->Close(foundation::ui::DialogResult::OK);
                    OpenProjectAt(dir.AsView());
                });
        }
        else // PromptOlderBackup: the Godot-style backup-and-upgrade prompt
        {
            foundation::ui::Button* backupOpen =
                dialog->AddButton(u8"Back Up && Open", foundation::ui::DialogResult::None);
            backupOpen->OnClick.Add(
                [this, rawDialog, dir](foundation::ui::ButtonBase*)
                {
                    rawDialog->Close(foundation::ui::DialogResult::OK);
                    if (!m_projectManager.BackupManifest(dir.AsView()).HasValue())
                    {
                        if (m_managerView)
                        {
                            m_managerView->SetStatus(u8"Backup FAILED - not opening.");
                        }
                        return;
                    }
                    OpenProjectAt(dir.AsView());
                    if (m_project)
                    {
                        (void)m_project->SaveSettings(); // re-stamp to this engine immediately
                    }
                });
            foundation::ui::Button* openOnly =
                dialog->AddButton(u8"Open Without Backup", foundation::ui::DialogResult::None);
            openOnly->OnClick.Add(
                [this, rawDialog, dir](foundation::ui::ButtonBase*)
                {
                    rawDialog->Close(foundation::ui::DialogResult::OK);
                    OpenProjectAt(dir.AsView());
                    if (m_project)
                    {
                        (void)m_project->SaveSettings();
                    }
                });
        }
        dialog->AddButton(u8"Cancel", foundation::ui::DialogResult::Cancel);
        dialog->Show(&m_uiHost->Context());
    }

    void EditorApplication::CreateFromManager(StringView directory, StringView name)
    {
        const Status created = m_projectManager.Create(directory, name);
        if (created.IsOk())
        {
            m_seedAfterOpen = true; // starter content lands once the fresh project opens
        }
        if (!created.IsOk())
        {
            if (m_managerView)
            {
                m_managerView->SetStatus(created.Code() == ErrorCode::AlreadyExists
                                             ? StringView(u8"That directory is already a project.")
                                             : StringView(u8"Create failed (path writable?)."));
            }
            return;
        }
        OpenProjectAt(directory);
    }

    void EditorApplication::ConfirmCloseProjectThen()
    {
        if (!m_project || m_inManagerMode)
        {
            return;
        }
        // CloseProject destroys panels/pages - never run it mid-event-dispatch (the
        // ui-mutation-queue rule); every path below queues it.
        const auto queueClose = [this]()
        {
            m_uiHost->Context().MutationQueueRef().QueueAction(
                Function<void()>{[this]() { CloseProject(); }});
        };

        usize dirtyCount = 0;
        for (const PagePanel& entry : m_pagePanels)
        {
            if (entry.page->IsDirty())
            {
                ++dirtyCount;
            }
        }
        if (dirtyCount == 0)
        {
            queueClose();
            return;
        }

        String message;
        AppendCountTo(message, dirtyCount);
        message += (dirtyCount == 1) ? StringView(u8" page has unsaved changes.")
                                     : StringView(u8" pages have unsaved changes.");
        RefPtr<foundation::ui::Dialog> dialog =
            MakeRef<foundation::ui::Dialog>(m_editorAllocator, StringView(u8"Unsaved changes"));
        RefPtr<foundation::ui::Label> label =
            MakeRef<foundation::ui::Label>(m_editorAllocator, message.AsView());
        label->WordWrap.SetValue(true);
        dialog->SetContent(label.Get());

        foundation::ui::Dialog* rawDialog = dialog.Get();
        foundation::ui::Button* saveAll =
            dialog->AddButton(u8"Save All && Close Project", foundation::ui::DialogResult::None);
        saveAll->OnClick.Add(
            [this, rawDialog, queueClose](foundation::ui::ButtonBase*)
            {
                bool allSaved = true;
                for (const PagePanel& entry : m_pagePanels)
                {
                    if (entry.page->IsDirty() && !entry.page->Save().IsOk())
                    {
                        allSaved = false;
                    }
                }
                if (allSaved)
                {
                    queueClose();
                }
                else
                {
                    m_context.Notify(editor::NoticeKind::Error,
                                     u8"Save FAILED (see console) - staying open.");
                }
                rawDialog->Close(allSaved ? foundation::ui::DialogResult::OK
                                          : foundation::ui::DialogResult::Cancel);
            });
        foundation::ui::Button* discard =
            dialog->AddButton(u8"Close Without Saving", foundation::ui::DialogResult::None);
        discard->OnClick.Add(
            [rawDialog, queueClose](foundation::ui::ButtonBase*)
            {
                queueClose();
                rawDialog->Close(foundation::ui::DialogResult::OK);
            });
        dialog->AddButton(u8"Cancel", foundation::ui::DialogResult::Cancel);
        dialog->Show(&m_uiHost->Context());
    }

    void EditorApplication::SaveLayout()
    {
        if (!m_project || !m_projectEditorSettings)
        {
            return;
        }
        Array<Guid> pages;
        Guid activePage;
        for (const PagePanel& entry : m_pagePanels)
        {
            // Instance-less pages (the Game tab) don't persist in the page set - a
            // nil guid would just fail the restore lookup.
            if (entry.page->InstanceId().IsNil())
            {
                continue;
            }
            pages.PushBack(entry.page->InstanceId());
            if (m_context.ActivePage() == entry.page)
            {
                activePage = entry.page->InstanceId();
            }
        }
        CaptureOpenPages(*m_projectEditorSettings, pages, activePage);
        if (m_shell.Docks() != nullptr)
        {
            (void)m_shell.SaveLayout(*m_projectEditorSettings);
        }
        (void)SaveProjectEditorSettings(*m_projectEditorSettings,
                                        m_project->EditorStateRoot().AsView());
    }

    // Bake (or RE-bake, on a DPI change) the editor icon set at chrome sizes scaled to
    // device pixels. Old atlases stay owned by the UIHost until shutdown - re-bakes are
    // rare (monitor moves), the atlases are small, and in-flight frames may still sample
    // the previous one.
    void EditorApplication::BakeEditorIcons(f32 contentScale)
    {
        EditorIcons& icons = EditorIcons::Get();
        const f32 scale = (contentScale > 0.1f) ? contentScale : 1.0f;
        const foundation::core::u32 kBaseSizes[] = {10, 12, 14, 16, 20, 24, 32};
        foundation::core::Array<foundation::core::u32> sizes;
        for (foundation::core::u32 base : kBaseSizes)
        {
            const foundation::core::u32 scaled = static_cast<foundation::core::u32>(
                static_cast<f32>(base) * scale + 0.5f);
            if (sizes.IsEmpty() || sizes[sizes.Size() - 1] != scaled)
            {
                sizes.PushBack(scaled);
            }
        }
        const auto bakeable = icons.Bakeable();
        (void)m_uiHost->BakeSvgDrawables(
            foundation::core::Span<foundation::ui::BakedSVGDrawable* const>(bakeable.Data(),
                                                                        bakeable.Size()),
            foundation::core::Span<const foundation::core::u32>(sizes.Data(), sizes.Size()));
        // The SHARED theme chrome glyphs (dock/tab close, chevrons, checkmark) bake at the
        // same scale - they are framework-owned now (ThemeIconSet), not editor-only.
        (void)m_uiHost->BakeThemeIcons(scale);
        m_iconBakeScale = scale;
    }

    void EditorApplication::ReportResourceMemory()
    {
        foundation::resource::ResourceManager* resources = Resources();
        if (resources == nullptr)
        {
            LOG_INFO(u8"Editor", u8"resource report: no resource manager (no project open)");
            return;
        }
        Array<foundation::resource::ResourceManager::LiveProductRow> rows;
        resources->ReportLiveProducts(rows);
        usize totalLive = 0;
        usize totalUnreferenced = 0;
        for (const auto& row : rows)
        {
            const char* name = row.type != nullptr ? row.type->name : "<unresolved>";
            LOG_INFO(u8"Editor",
                     u8"resource report: {} - live {}, pending {}, failed {}, "
                     u8"unreferenced {}",
                     StringView(reinterpret_cast<const char8_t*>(name)),
                     static_cast<u64>(row.live), static_cast<u64>(row.pending),
                     static_cast<u64>(row.failed), static_cast<u64>(row.unreferenced));
            totalLive += row.live;
            totalUnreferenced += row.unreferenced;
        }
        LOG_INFO(u8"Editor",
                 u8"resource report: {} live products total, {} unreferenced (cache-only - "
                 u8"what a purge would release); sizes arrive with allocator tagging (I5)",
                 static_cast<u64>(totalLive), static_cast<u64>(totalUnreferenced));
    }

    void EditorApplication::RegisterActions()
    {
        // The editor-wide actions, declared once here and served everywhere from the registry:
        // the menu bar and the global shortcuts are generated from them (BuildMenus), the
        // palette and the MCP bridge read them. Registered in menu order - the bar lists
        // menus in the order their names first appear. Each domain's RegisterEditor adds its
        // own set beside these.
        EditorActionRegistry& actions = m_context.Actions();
        const auto Declare = [](StringView id, StringView label, StringView description,
                                StringView menuPath, i32 order)
        {
            EditorActionDeclaration d;
            d.id = String(id);
            d.label = String(label);
            d.description = String(description);
            d.menuPath = String(menuPath);
            d.menuOrder = order;
            return d;
        };

        {
            EditorActionDeclaration d =
                Declare(u8"file.save", u8"Save", u8"Save the active page to its source asset",
                        u8"File/Save", 100);
            d.enabled = [](editor::EditorPage* page) { return page != nullptr && page->IsDirty(); };
            d.shortcut =
                EditorShortcut{foundation::ui::KeyCode::S, foundation::ui::KeyModifiers::Ctrl};
            d.execute = [this](editor::EditorPage* page) { SavePage(*page); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"file.saveAs", u8"Save As...",
                        u8"Save the active page as a new source asset", u8"File/Save As...", 101);
            d.enabled = [](editor::EditorPage* page) { return page != nullptr; };
            d.execute = [this](editor::EditorPage* page) { SavePageAs(*page); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"file.saveLayout", u8"Save Layout",
                        u8"Save the panel layout as the default for this editor",
                        u8"File/Save Layout", 200);
            d.execute = [this](editor::EditorPage*)
            {
                SaveLayout();
                m_context.SetStatus(u8"Layout saved.");
            };
            (void)actions.Register(Move(d));
        }
        // Only meaningful when the manager launched us; a CLI-opened editor keeps its
        // single-project lifecycle (Exit is the way out).
        if (m_config.startInProjectManager)
        {
            {
                EditorActionDeclaration d =
                    Declare(u8"file.closeProject", u8"Close Project",
                            u8"Close the project and return to the project manager",
                            u8"File/Close Project", 300);
                d.execute = [this](editor::EditorPage*) { ConfirmCloseProjectThen(); };
                (void)actions.Register(Move(d));
            }
        }
        {
            EditorActionDeclaration d =
                Declare(u8"file.exit", u8"Exit", u8"Exit the editor", u8"File/Exit", 301);
            d.execute = [this](editor::EditorPage*)
            {
                if (m_host != nullptr && ConfirmExitAllowed())
                {
                    m_host->RequestExit();
                }
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"edit.undo", u8"Undo", u8"Undo the active page's last edit",
                        u8"Edit/Undo", 100);
            d.enabled = [](editor::EditorPage* page) { return page != nullptr && page->Commands().CanUndo(); };
            d.shortcut =
                EditorShortcut{foundation::ui::KeyCode::Z, foundation::ui::KeyModifiers::Ctrl};
            d.execute = [](editor::EditorPage* page) { page->Commands().Undo(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"edit.redo", u8"Redo", u8"Redo the active page's last undone edit",
                        u8"Edit/Redo", 101);
            d.enabled = [](editor::EditorPage* page) { return page != nullptr && page->Commands().CanRedo(); };
            d.shortcut = EditorShortcut{foundation::ui::KeyCode::Z,
                                    foundation::ui::KeyModifiers::Ctrl |
                                        foundation::ui::KeyModifiers::Shift};
            d.alternateShortcut =
                EditorShortcut{foundation::ui::KeyCode::Y, foundation::ui::KeyModifiers::Ctrl};
            d.execute = [](editor::EditorPage* page) { page->Commands().Redo(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(u8"page.discardChanges", u8"Discard Changes",
                                                u8"Revert the page's unsaved edits", u8"", 0);
            d.enabled = [](editor::EditorPage* page) { return page != nullptr && page->IsDirty(); };
            d.execute = [](editor::EditorPage* page) { page->DiscardChanges(); };
            (void)actions.Register(Move(d));
        }
        RegisterPlaybackActions(actions);
        {
            EditorActionDeclaration d =
                Declare(u8"edit.preferences", u8"Preferences...",
                        u8"Open the per-user editor preferences", u8"Edit/Preferences...", 200);
            d.execute = [this](editor::EditorPage*)
            {
                auto dialog = MakeRef<EditorPreferencesDialog>(
                    m_editorAllocator, m_context, m_editorSettings);
                // UI scale applies LIVE: host scale (roots pick it up
                // next frame) + icon re-bake at the effective scale.
                dialog->OnUiScaleApplied = [this](f32 uiScale)
                {
                    m_uiHost->SetUiScale(uiScale);
                    graphics::RenderWindow* mainRw =
                        m_host != nullptr ? m_host->MainRenderWindow() : nullptr;
                    const f32 content =
                        mainRw != nullptr ? mainRw->Window().ContentScale()
                                          : 1.0f;
                    BakeEditorIcons(content * uiScale);
                };
                // The MCP host follows the saved preference at once: started,
                // moved to the new port, or stopped.
                dialog->OnMcpSettingsApplied = [this]() { StartMcpHost(); };
                dialog->Show(&m_uiHost->Context());
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"project.settings", u8"Project Settings...",
                        u8"Open the project settings", u8"Project/Project Settings...", 100);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*)
            {
                if (m_project)
                {
                    auto dialog = MakeRef<ProjectSettingsDialog>(
                        m_editorAllocator, m_context);
                    dialog->Show(&m_uiHost->Context());
                }
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"project.addNativeCode", u8"Add Native Code...",
                        u8"Scaffold a native code module under Native/",
                        u8"Project/Add Native Code...", 200);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*)
            {
                if (!m_project)
                {
                    return;
                }
                if (!m_project->Settings().nativeModule.IsEmpty())
                {
                    m_context.Notify(editor::NoticeKind::Info,
                                     u8"This project already has native code "
                                     u8"(see Project Settings).");
                    return;
                }
                const Status scaffolded =
                    editor::ScaffoldNativeModule(*m_project);
                if (scaffolded.IsOk())
                {
                    m_context.Notify(
                        editor::NoticeKind::Success,
                        u8"Native code scaffolded in Native/ - use Project > "
                        u8"Build Native Module, then Reload.");
                    LOG_INFO(u8"Editor",
                             u8"native scaffold: Native/ created, manifest "
                             u8"nativeModule = '{}'",
                             m_project->Settings().nativeModule);
                }
                else if (scaffolded.Code() == ErrorCode::AlreadyExists)
                {
                    m_context.Notify(editor::NoticeKind::Warning,
                                     u8"Native/ already exists - not touching "
                                     u8"it (set Project Settings > Native "
                                     u8"module manually).");
                }
                else
                {
                    m_context.Notify(editor::NoticeKind::Error,
                                     u8"Native scaffold failed (see Console).");
                }
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"project.buildNativeModule", u8"Build Native Module",
                        u8"Build the project's native module on the job service",
                        u8"Project/Build Native Module", 201);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*)
            {
                if (!m_project || m_project->Settings().nativeModule.IsEmpty())
                {
                    m_context.Notify(editor::NoticeKind::Info,
                                     u8"No native module declared - Project > "
                                     u8"Add Native Code... first.");
                    return;
                }
                editor::EditorProject* project = m_project.Get();
                m_jobService.Submit(
                    u8"Native Build",
                    [this, project](editor::JobContext&) -> Status
                    {
                        const Status built =
                            editor::BuildDevNativeModule(*project);
                        if (built.IsOk())
                        {
                            m_context.Notify(
                                editor::NoticeKind::Success,
                                u8"Native module built - Project > Reload "
                                u8"Native Module to pick it up.");
                        }
                        else
                        {
                            m_context.Notify(
                                editor::NoticeKind::Error,
                                u8"Native build failed (see Console).");
                        }
                        return built;
                    });
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"project.reloadNativeModule", u8"Reload Native Module",
                        u8"Reload the project's built native module",
                        u8"Project/Reload Native Module", 202);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*) { ReloadNativeModule(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"project.export", u8"Export...", u8"Open the export presets panel",
                        u8"Project/Export...", 300);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*) { OpenExportPresetsPanel(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"project.manageTemplates", u8"Manage Templates...",
                        u8"Manage the export templates", u8"Project/Manage Templates...", 301);
            d.execute = [this](editor::EditorPage*) { OpenTemplatesManager(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"project.reportResourceMemory", u8"Report Resource Memory",
                        u8"Log the resident resources by type (unreferenced = purge candidates)",
                        u8"Project/Report Resource Memory", 400);
            d.readOnly = true;
            d.execute = [this](editor::EditorPage*) { ReportResourceMemory(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"build.cookAll", u8"Cook All",
                        u8"Cook the dirty assets into the cooked database", u8"Build/Cook All",
                        100);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*) { m_cookService.RequestCook(false); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"build.rebuildAll", u8"Rebuild All", u8"Cook every asset again",
                        u8"Build/Rebuild All", 101);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*) { m_cookService.RequestCook(true); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"game.play", u8"Play", u8"Play the project in the Game page",
                        u8"Game/Play", 100);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*) { OpenGamePage(false); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"game.playNewInstance", u8"Play New Instance",
                        u8"Play the project in a fresh Game page", u8"Game/Play New Instance", 101);
            d.enabled = [this](editor::EditorPage*) { return static_cast<bool>(m_project); };
            d.execute = [this](editor::EditorPage*) { OpenGamePage(true); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"view.resetLayout", u8"Reset Layout",
                        u8"Reset the panel layout to the default", u8"View/Reset Layout", 100);
            d.execute = [this](editor::EditorPage*)
            {
                m_shell.ResetLayout();
                m_context.SetStatus(u8"Layout reset to default.");
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"view.screenshot", u8"Screenshot Editor",
                        u8"Write the editor's main window, as it looks now, to a PNG under "
                        u8"<user-data>/screenshots",
                        u8"View/Screenshot Editor", 300);
            d.shortcut = EditorShortcut{ui::KeyCode::F12, ui::KeyModifiers::None};
            d.readOnly = true; // a file outside the project; nothing in it changes
            d.execute = [this](editor::EditorPage*) { TakeEditorScreenshot(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(u8"view.commandPalette", u8"Command Palette...",
                                                u8"Every action by name: type to filter, Enter runs it",
                                                u8"View/Command Palette...", 200);
            d.shortcut = EditorShortcut{ui::KeyCode::P, ui::KeyModifiers::Ctrl | ui::KeyModifiers::Shift};
            d.readOnly = true; // the palette changes nothing itself
            d.execute = [this](editor::EditorPage*)
            {
                auto palette = MakeRef<CommandPaletteDialog>(m_editorAllocator, m_context.Actions());
                palette->Show(&m_uiHost->Context());
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(u8"help.about", u8"About", u8"The editor's version", u8"Help/About", 100);
            d.readOnly = true;
            d.execute = [this](editor::EditorPage*)
            {
                RefPtr<ui::Dialog> dialog = MakeRef<ui::Dialog>(
                    m_editorAllocator, StringView(u8"About Editor"));
                auto column = MakeRef<ui::FlexLayout>(m_editorAllocator);
                column->Direction = ui::Orientation::Vertical;
                column->Spacing = 8;

                auto title =
                    MakeRef<ui::Label>(m_editorAllocator, StringView(u8"Editor"));
                title->FontSize.SetValue(Optional<f32>{18.0f});
                column->AddView(title.Get());

                String version;
                version += StringView(u8"Version ");
                version += StringView(reinterpret_cast<const char8_t*>(BuildStamp()));
                auto versionLabel =
                    MakeRef<ui::Label>(m_editorAllocator, version.AsView());
                versionLabel->WordWrap.SetValue(true);
                column->AddView(versionLabel.Get());

                dialog->SetContent(column.Get());
                dialog->AddButton(u8"OK", ui::DialogResult::OK);
                dialog->Show(&m_uiHost->Context());
            };
            (void)actions.Register(Move(d));
        }
    }

    void EditorApplication::BuildMenus()
    {
        // The bar and the global shortcuts are GENERATED from the action registry (see
        // RegisterActions and each domain's RegisterEditor); a menu's items are rebuilt when
        // it opens, so enabled states are the registry's answer at that moment. The one
        // list-driven set, File > New <creator>, is a registry of its own and leads the File
        // menu. Shortcuts dispatch AFTER the focused view, and text controls mark their
        // key-downs handled - a focused textbox keeps Ctrl+Z for its own text undo.
        m_actionMenus = MakeUnique<ActionMenuBar>(m_editorAllocator, *m_shell.Menus(),
                                                  m_context.Actions());
        m_actionMenus->AddLeadingItems(
            u8"File",
            [this](foundation::ui::ContextMenu& file)
            {
                // File > New <creator> from the registry (per-subsystem editor modules).
                // Categorized creators (e.g. "Primitives") nest in a submenu of that name.
                Array<StringView> categories;
                for (const pipeline::AssetCreator& creator : m_context.Creators().All())
                {
                    if (creator.category.IsEmpty())
                    {
                        String label(u8"New ");
                        label += creator.label;
                        const auto* entry = &creator;
                        file.AddItem(label.AsView(), [this, entry]() { CreateAndOpen(*entry); });
                        continue;
                    }
                    bool seen = false;
                    for (StringView c : categories)
                    {
                        if (c == creator.category.AsView())
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        categories.PushBack(creator.category.AsView());
                    }
                }
                categories.Sort([](StringView a, StringView b) { return a.Compare(b) < 0; });
                for (StringView category : categories)
                {
                    foundation::ui::MenuItem* submenuItem = file.AddSubmenu(category);
                    auto* submenu = Cast<foundation::ui::ContextMenu>(submenuItem->Submenu.Get());
                    if (submenu == nullptr)
                    {
                        continue;
                    }
                    for (const pipeline::AssetCreator& creator : m_context.Creators().All())
                    {
                        if (creator.category.AsView() != category)
                        {
                            continue;
                        }
                        const auto* entry = &creator;
                        submenu->AddItem(creator.label.AsView(),
                                         [this, entry]() { CreateAndOpen(*entry); });
                    }
                }
            });
        m_actionShortcuts = MakeUnique<ActionShortcuts>(
            m_editorAllocator, *m_uiHost->Context().GetShortcuts(), m_context.Actions());
    }
}

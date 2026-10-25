// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :session partition.
//
// EditorSession: the project the editor has open, out of the application
// (Documentation/Specs/editor-gui.md, 5.1, layer 3). Opening one (or, from the command line,
// scaffolding a bare folder into one) sets up what lives as long as it does: the context's
// project, the per-project editor store (dock layout, favorites, open pages), the resources over
// its cooked database attached to the embedded runtime, its native module, the cook service and
// the hooks the context runs it through, the thumbnails, and the game UI's defaults; it records
// the open in the user's project registry and seeds a new project's starter content. Closing
// takes them down in the reverse order. What the views do at those moments (the pages, the
// layout, the MCP host) goes through the hooks the application gives it.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:session;

import foundation.core;
import foundation.content;
import foundation.fonts;
import foundation.fonts.resource; // Font (the game UI's default and other fonts)
import foundation.resource;
import foundation.runtime;
import foundation.runtime.client; // IApplicationHost
import foundation.settings;
import foundation.ui.resource; // UITheme: the game UI's default theme (Engine.UI, the runtime's)
import pipeline.core;
import engine.project;    // kProjectCacheDir
import engine.defaultapp; // the embedded game application
import engine.ui;         // UISubsystem (the game UI's defaults)
import engine.gui;        // GuiSubsystem (the engine GUI's fonts and sheets)
import editor.core;
import editor.mcp;        // SourceAssetTypesFor
import editor.preview;    // ThumbnailStage (the GPU half of the thumbnails)
import :native_module;
import :layout; // the per-project store's sections and files

using namespace foundation::core;

export namespace editor::app
{
    class EditorSession
    {
    public:
        /// What it works over: the application's services, the embedded runtime the project's
        /// resources attach to, and what the application does when a project opens or closes.
        struct Services
        {
            editor::EditorContext* context = nullptr;
            editor::EditorCookService* cook = nullptr;
            editor::EditorJobService* jobs = nullptr;
            pipeline::BuilderRegistry* builders = nullptr;
            editor::ThumbnailService* thumbnails = nullptr;
            foundation::settings::Settings* userSettings = nullptr;
            editor::ProjectManagerController* projects = nullptr;
            foundation::runtime::Context* runtime = nullptr;
            foundation::runtime::IApplicationHost* embeddedHost = nullptr;
            engine::runtime::DefaultApplication* embeddedApp = nullptr;
            EditorNativeModule* native = nullptr;
            /// Seeds a new project's starter content (the executable composes it); null: none.
            Function<void(editor::EditorContext&, editor::EditorProject&)> seedNewProject;
            /// The project opened, its services up: the application brings its views up (the
            /// MCP host, the pages, the layout).
            Function<void(editor::EditorProject&)> opened;
            /// The project is about to close, its services still up: the views go first.
            Function<void()> closing;
            /// A cook finished (its summary on the cook service): the views refresh, the user hears.
            Function<void()> cookFinished;
            /// The views' state into the store before it is written (the dock's layout).
            Function<void(foundation::settings::Settings&)> captureState;
        };

        EditorSession(IAllocator& allocator, Services services) : m_allocator(allocator), m_services(Move(services)) {}
        EditorSession(const EditorSession&) = delete;
        EditorSession& operator=(const EditorSession&) = delete;
        ~EditorSession() { Close(); }

        struct OpenOptions
        {
            /// A folder with no project becomes one (the command line's fallback; the manager
            /// scaffolds only through its New Project flow).
            bool scaffold = false;
            String scaffoldName{u8"Untitled"};
            /// Seed the starter content into a project this open created.
            bool seed = false;
        };

        /// Opens the project in `directory`: false (said on the status line) when there is none
        /// there and none may be made.
        bool Open(StringView directory, const OpenOptions& options)
        {
            editor::EditorContext& context = *m_services.context;
            if (directory.IsEmpty())
            {
                context.SetStatus(u8"No project directory - pass one on the command line.");
                return false;
            }
            Close();
            bool seed = false;
            m_project = editor::EditorProject::Open(editor::EditorRootAllocator(), directory);
            if (!m_project && options.scaffold)
                if (editor::EditorProject::Create(editor::EditorRootAllocator(), directory, options.scaffoldName.AsView()).IsOk())
                {
                    m_project = editor::EditorProject::Open(editor::EditorRootAllocator(), directory);
                    seed = m_project && options.seed;
                }
            if (!m_project)
            {
                context.SetStatus(Format(u8"Failed to open project: {}", directory).AsView());
                return false;
            }
            editor::EditorProject& project = *m_project;
            context.SetProject(&project);
            ConfigureThumbnails(directory);
            {
                // What the open-time header scans cost (the parse volume decides the partial-parse work).
                const auto& source = project.SourceDb().LastScanStats();
                const auto& cooked = project.CookedDb().LastScanStats();
                LOG_INFO(u8"Editor", u8"project open scan: sources {} envelopes / {} KB parsed, cooked {} envelopes / {} KB parsed",
                         static_cast<u64>(source.envelopes), source.bytesOpened / 1024, static_cast<u64>(cooked.envelopes),
                         cooked.bytesOpened / 1024);
            }
            OpenProjectStore();
            AttachResources();
            // The GPU half of the thumbnails, once the resources exist (the stage keeps them: a null
            // one stalls every queued job), on the embedded host whose context holds the scene and
            // render subsystems. It goes before the service resets, so a job in flight unstages.
            if (m_services.embeddedHost != nullptr && m_services.thumbnails != nullptr)
                m_thumbnailStage = MakeUnique<editor::ThumbnailStage>(m_allocator, *m_services.embeddedHost, *m_services.thumbnails, m_resources.Get());
            if (m_services.native != nullptr)
                m_services.native->Load(project, context, *m_services.runtime);
            if (m_services.embeddedApp != nullptr)
                m_services.embeddedApp->SetContentDatabase(&project.SourceDb()); // scripted scene loads read the sources
            ApplyGameUiDefaults();
            if (m_services.embeddedApp != nullptr && m_services.embeddedApp->UI() != nullptr && project.Settings().defaultUiFontId.IsNil() &&
                HasFontAssets(project.SourceDb().RootGroup()))
                // A project with fonts and no default renders no game-UI text: name the fix.
                LOG_WARNING(u8"UI",
                            u8"game UI has no default font - set Project Settings > Default UI font (the project has fonts, but none "
                            u8"is the default, so game-UI text will not render)");
            EditorSession* self = this;
            context.OnProjectSettingsChanged = [self]() { self->ApplyGameUiDefaults(); };
            StartCook();

            if (m_services.opened)
                m_services.opened(project);
            if (m_services.projects != nullptr)
            {
                m_services.projects->NoteOpened(directory, project.Name(), project.Settings().engineVersion.AsView());
                if (m_services.userSettings != nullptr)
                    (void)editor::SaveEditorSettingsToUserData(*m_services.userSettings);
            }
            if (seed && m_services.seedNewProject)
            {
                m_services.seedNewProject(context, project);
                (void)project.SaveSettings(); // the seed set the manifest's defaults
                m_services.cook->RequestCook(false);
                context.SetStatus(u8"Project created with starter content.");
            }
            else
                context.SetStatus(Format(u8"Project: {}  ({})", project.Name(), project.Directory()).AsView());
            return true;
        }

        /// Closes the project: the views first (the hook), then the cook, the thumbnails, the
        /// native module, the resources and the store, the project last.
        void Close()
        {
            if (!m_project)
                return;
            editor::EditorContext& context = *m_services.context;
            SaveStore(); // the views' state, while they are up
            // The pages close next, and a page saves its own state as it goes (a scene its view),
            // which asks for a save: that writes what they add without capturing the views again,
            // or the open pages would be written as they close, the last leaving none to reopen.
            m_closingViews = true;
            if (m_services.closing)
                m_services.closing();
            m_closingViews = false;
            m_services.cook->Shutdown(); // joins a cook in flight before the databases go
            m_thumbnailStage.Reset(); // unstages and drops its GPU objects while the renderer lives
            m_services.thumbnails->Reset();
            context.OnCookRequested = {};
            context.CookBusy = {};
            context.OnFavoritesChanged = {};
            context.OnProjectSettingsChanged = {};
            if (m_services.native != nullptr)
                m_services.native->Unload(); // its systems withdraw while the runtime and resources live
            if (m_services.embeddedApp != nullptr)
            {
                m_services.embeddedApp->AttachResourceManager(nullptr, *m_services.embeddedHost);
                m_services.embeddedApp->SetContentDatabase(nullptr); // the sources go with the project
                if (m_services.embeddedApp->UI() != nullptr)
                    m_services.embeddedApp->UI()->SetDefaultTheme(nullptr);
            }
            context.SetResources(nullptr);
            m_resources.Reset();
            context.SetProjectEditorSettings(nullptr);
            context.OnProjectEditorSettingsSaveRequested = {};
            m_store.Reset();
            context.SetProject(nullptr);
            m_project.Reset();
            context.SetStatus(u8"Project closed.");
        }

        /// The embedded runtime's host and application, once they are up (the application starts
        /// them after this is made).
        void SetEmbedded(foundation::runtime::IApplicationHost* host, engine::runtime::DefaultApplication* app) noexcept
        {
            m_services.embeddedHost = host;
            m_services.embeddedApp = app;
        }

        [[nodiscard]] editor::EditorProject* Project() const noexcept { return m_project.Get(); }
        /// The per-project editor store (dock layout, favorites, open pages); null with no project.
        [[nodiscard]] foundation::settings::Settings* Store() const noexcept { return m_store.Get(); }
        [[nodiscard]] foundation::resource::ResourceManager* Resources() const noexcept { return m_resources.Get(); }
        /// The thumbnails' GPU stage while a project is open with a renderer's runtime: the
        /// application stages a job each frame and renders it in the scene renderer's bracket.
        [[nodiscard]] editor::ThumbnailStage* Stage() const noexcept { return m_thumbnailStage.Get(); }

        /// Writes the per-project store, the views' state captured into it first (not while they
        /// close: what they were is already in it).
        void SaveStore()
        {
            if (!m_project || !m_store)
                return;
            if (m_services.captureState && !m_closingViews)
                m_services.captureState(*m_store);
            (void)SaveProjectEditorSettings(*m_store, m_project->EditorStateRoot().AsView());
        }

        /// The game UI's defaults from the project's settings: its theme, its default and other
        /// fonts, the engine GUI's sheets. Again after every cook and settings save: a bind that
        /// misses (a fresh checkout, nothing cooked yet) heals then.
        void ApplyGameUiDefaults()
        {
            engine::runtime::DefaultApplication* app = m_services.embeddedApp;
            if (!m_project || !m_resources || app == nullptr)
                return;
            // Bound only once cooked: a bind of a product not there yet warns of a broken reference.
            foundation::content::ContentDatabase& cooked = m_project->CookedDb();
            const auto ready = [&cooked](const Guid& id) { return !id.IsNil() && cooked.GetInstance(id) != nullptr; };
            const Guid themeId = m_project->Settings().defaultUiThemeId;
            if (ready(themeId) && app->UI() != nullptr)
                if (auto theme = m_resources->Bind<foundation::ui::UITheme>(themeId))
                    app->UI()->SetDefaultTheme(theme.Get());
            const Guid fontId = m_project->Settings().defaultUiFontId;
            const foundation::fonts::Font* defaultFont = nullptr;
            if (ready(fontId))
                if (auto font = m_resources->Bind<foundation::fonts::Font>(fontId))
                {
                    defaultFont = font.Get();
                    if (app->UI() != nullptr)
                        app->UI()->SetDefaultFont(defaultFont);
                }
            // The other UI fonts, each a family beside the default; always set, so one taken off the
            // list leaves the game UI too.
            Array<const foundation::fonts::Font*> extras;
            for (const Guid& id : m_project->Settings().uiFontIds)
                if (ready(id))
                    if (auto extra = m_resources->Bind<foundation::fonts::Font>(id))
                        extras.PushBack(extra.Get());
            const Span<const foundation::fonts::Font* const> extraSpan(extras.Data(), extras.Size());
            if (app->UI() != nullptr)
                app->UI()->SetExtraFonts(extraSpan);
            if (app->GUI() != nullptr)
            {
                app->GUI()->SetFonts(defaultFont, extraSpan);
                const Array<Guid>& sheets = m_project->Settings().guiStyleSheetIds;
                app->GUI()->SetStyleSheets(Span<const Guid>(sheets.Data(), sheets.Size()));
            }
        }

    private:
        // Whether the sources hold a font anywhere.
        [[nodiscard]] static bool HasFontAssets(foundation::content::Group* group)
        {
            if (group == nullptr)
                return false;
            for (foundation::content::Instance* instance : group->Instances())
                if (instance->TypeName() == StringView(u8"FontAsset"))
                    return true;
            for (foundation::content::Group* child : group->Groups())
                if (HasFontAssets(child))
                    return true;
            return false;
        }

        void ConfigureThumbnails(StringView directory)
        {
            const String thumbsDir = Format(u8"{}/{}/thumbs", directory, engine::project::kProjectCacheDir);
            (void)CreateDirectory(thumbsDir.AsView());
            editor::EditorProject* project = m_project.Get();
            editor::EditorCookService* cook = m_services.cook;
            m_services.thumbnails->Configure(
                thumbsDir.AsView(),
                Function<foundation::content::Instance*(const Guid&)>{[project](const Guid& id) { return project->SourceDb().GetInstance(id); }},
                m_services.jobs, Function<u64(const Guid&)>{[cook](const Guid& id) { return cook->RecipeHashFor(id); }},
                m_project->SourcesRoot().AsView());
        }

        // The per-project store: absent on a fresh project, its sections then read as defaults.
        void OpenProjectStore()
        {
            editor::EditorContext& context = *m_services.context;
            m_store = MakeUnique<foundation::settings::Settings>(m_allocator, m_allocator);
            const Status loaded = LoadProjectEditorSettings(*m_store, m_project->EditorStateRoot().AsView());
            if (!loaded.IsOk() && loaded.Code() != ErrorCode::NotFound)
                // A partial load aborted at a section it could not make; a later save would rewrite
                // the file from what is left, losing the layout, favorites or open pages.
                LOG_ERROR(u8"Editor",
                          u8"editor.project.settings.xml load FAILED (partial store) - check for unregistered section types; later "
                          u8"saves may drop sections");
            context.SetProjectEditorSettings(m_store.Get());
            EditorSession* self = this;
            context.OnProjectEditorSettingsSaveRequested = [self]() { self->SaveStore(); };
            ApplyFavorites(context, *m_store);
            context.OnFavoritesChanged = [self]()
            {
                if (self->m_project && self->m_store)
                {
                    CaptureFavorites(*self->m_services.context, *self->m_store);
                    self->m_services.context->RequestProjectEditorSettingsSave();
                }
            };
        }

        // The project's resources over its cooked database, attached to the embedded runtime (which
        // registers the engine's factories into it), decoding on the global job system when there is one.
        void AttachResources()
        {
            m_resources = MakeUnique<foundation::resource::ResourceManager>(m_allocator, m_allocator, m_project->CookedDb(),
                                                                             HasGlobalJobSystem() ? &GlobalJobs() : nullptr);
            m_services.context->SetResources(m_resources.Get());
            if (m_services.embeddedApp != nullptr)
                m_services.embeddedApp->AttachResourceManager(m_resources.Get(), *m_services.embeddedHost);
        }

        // The cook over the project, and how the context reaches it.
        void StartCook()
        {
            editor::EditorContext& context = *m_services.context;
            editor::EditorCookService* cook = m_services.cook;
            editor::EditorJobService* jobs = m_services.jobs;
            pipeline::BuilderRegistry* builders = m_services.builders;
            cook->Initialize(*m_project, *builders);
            context.OnCookRequested = [cook](bool rebuild) { cook->RequestCook(rebuild); };
            // Cook-gated starts (play waits for the cook): anything in flight, or a request still waiting.
            context.CookBusy = [cook]() { return cook->IsReady() && !cook->IsIdle(); };
            context.SourceAssetTypesOf = [builders](const TypeInfo& product) { return editor::mcp::SourceAssetTypesFor(*builders, product); };
            // A background job (an export) reads the databases: mutations and new cooks wait, as during a cook.
            cook->ExternalMutationLock = [jobs]() { return jobs->IsBusy(); };
            context.SetJobs(jobs);
            EditorSession* self = this;
            cook->OnCookFinished = [self]() { self->CookFinished(); };
        }

        // Rebuilt products swap in behind their handles (live scenes see them, no reopen), then the
        // game UI binds again (a fresh checkout's first cook makes what the open-time bind missed;
        // after the reload, since the game UI borrows the font product itself).
        void CookFinished()
        {
            m_services.thumbnails->InvalidateAll(); // recipe hashes moved; unchanged ones load from disk
            if (m_resources)
                for (const Guid& product : m_services.cook->LastCookedProducts())
                    (void)m_resources->Reload(product);
            ApplyGameUiDefaults();
            if (m_services.cookFinished)
                m_services.cookFinished();
        }

        IAllocator& m_allocator;
        Services m_services;
        UniquePtr<editor::EditorProject> m_project;
        UniquePtr<foundation::settings::Settings> m_store;
        bool m_closingViews = false; // Close is closing the views: a save writes, captures nothing
        UniquePtr<foundation::resource::ResourceManager> m_resources;
        UniquePtr<editor::ThumbnailStage> m_thumbnailStage;
    };
}

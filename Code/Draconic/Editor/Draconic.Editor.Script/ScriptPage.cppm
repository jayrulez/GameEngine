// Draconic::EditorScript - the `draconic.editor.script` module.
//
// ScriptEditorPage (scripting.md §5, "ScriptPage - phase 2"): an in-editor code editor for a
// ScriptClassAsset's behavior source, built on ui::toolkit::CodeEditView (code-editor.md P1):
// monospace virtualized editing, a native gutter whose Breakpoint markers write through to the
// shared EditorContext store (a Game run applies the same set to its debugger), and compile
// errors mapped onto their lines as Error markers. Save writes the source back + nudges the
// SAME recook the external-file edit takes (EditorContext::RequestCook), so a running/
// simulating behavior hot-reloads through the existing ScriptSceneSystem product-swap path.
// A failing compile keeps the last-good cooked product (the builder never writes on failure).
//
// Backend-neutral: the page never names a language. It resolves the cook + New-Asset starter
// through the registries by the asset's language id, so it edits Wren and AngelScript alike.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.script;

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.runtime.client;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.script;
import draconic.script.editor;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace ui = draconic::ui;
    namespace content = draconic::content;

    // The backend's ACTUAL bound API for one language (IScriptManager::DescribeBoundApi),
    // built ONCE per page and shared by every consumer - completion AND the API browser read
    // this one source, so they can never disagree. The build replays the runtime's exact
    // registration sequence against a throwaway manager (RegisterFoundationTypes + facade
    // reflection + CreateScriptManagerForLanguage + RegisterReflectedTypes) - so what the
    // page shows is exactly what a run can call, spelled the language's way. Lazy: nothing
    // runs until the first Types() call; an unknown language stays empty.
    class ScriptApiSurface final
    {
    public:
        void SetLanguage(StringView languageId) { m_language = String(languageId); }
        [[nodiscard]] const Array<draconic::script::ScriptApiType>& Types() const;

    private:
        String m_language;
        mutable bool m_built = false;
        mutable Array<draconic::script::ScriptApiType> m_types;
    };

    // Completion over the shared surface: type/namespace names at top level, and a type's
    // members right after `Type.` (the receiver word left of the trigger dot).
    class ScriptApiCompletionProvider final : public ui::toolkit::ICompletionProvider
    {
    public:
        void SetSurface(const ScriptApiSurface* surface) { m_surface = surface; }

        void Collect(const ui::toolkit::CodeDocument& document,
                     ui::toolkit::CodePosition cursor, StringView prefix,
                     Array<ui::toolkit::CompletionCandidate>& out) override;

    private:
        const ScriptApiSurface* m_surface = nullptr; // borrowed (page-owned)
    };

    // True when a binding's reflected type was registered under a non-Runtime domain
    // (TypeRegistry::DomainOf) - callable wherever the editor runs (including
    // play-in-editor), but ABSENT from a shipped player. Completion and the API browser
    // both mark such bindings " [editor]" so a player-bound script's author is warned.
    [[nodiscard]] inline bool IsEditorOnlyBinding(TypeId typeId)
    {
        return typeId != 0 && !(GlobalTypeRegistry().DomainOf(typeId) == kRuntimeTypeDomain);
    }

    // One row of the API browser tree: a type/namespace (depth 0) or a member (depth 1).
    struct ScriptApiTreeNode
    {
        String label;      // row text: type name, or the member's language-formatted signature
        String insertText; // what activating the row types into the editor
        i32 depth = 0;
        Array<i32> children; // indices into ScriptApiTree::nodes (type rows only)
    };

    struct ScriptApiTree
    {
        Array<ScriptApiTreeNode> nodes;
        Array<i32> roots; // depth-0 node indices, display order
    };

    // Builds the browser tree from a bound-API surface: types alphabetical, each type's
    // members alphabetical under it. `filter` is a case-insensitive substring match: a
    // matching TYPE name keeps the whole type; otherwise only matching members (and their
    // type row) survive. Empty filter keeps everything.
    [[nodiscard]] ScriptApiTree
    BuildScriptApiTree(const Array<draconic::script::ScriptApiType>& types, StringView filter);

    // The openable API browser panel: a filter box over a namespace/class > members tree of
    // the language's bound API, fed by the SAME shared surface completion uses.
    // Double-clicking a row hands its insert text to OnInsert (the page types it into the
    // editor at the cursor). Filter edits only mark the tree dirty; the rebuild (view churn
    // inside TreeView::SetAdapter) runs in Update() from the page's OnUpdate - never
    // mid-event-dispatch. A hidden panel never builds, so the one-time surface build is
    // paid on first open.
    class ScriptApiBrowserView final
    {
    public:
        ScriptApiBrowserView();
        ~ScriptApiBrowserView();

        void SetSurface(const ScriptApiSurface* surface) { m_surface = surface; }
        [[nodiscard]] ui::View* Root() const { return m_root.Get(); }
        void Update(); // deferred-rebuild bracket (call once per frame)

        Function<void(StringView)> OnInsert;

    private:
        class TreeAdapter;

        void Rebuild();

        const ScriptApiSurface* m_surface = nullptr; // borrowed (page-owned)
        ScriptApiTree m_tree;
        bool m_dirty = true;
        RefPtr<ui::View> m_root;
        RefPtr<ui::EditText> m_filter;
        RefPtr<ui::TreeView> m_treeView;
        UniquePtr<TreeAdapter> m_adapter;
    };

    // The in-editor script text page. Pure UI over ScriptSourceDocument (the headless save +
    // compile-check model): the page owns the widget tree and forwards edits/Save to the model.
    class ScriptEditorPage final : public app::UIEditorPage
    {
    public:
        ScriptEditorPage(EditorContext& context, content::Instance& instance)
            : m_context(&context), m_title(instance.Name())
        {
            SetInstanceId(instance.Id());

            // The asset carries only the file name + language; the source lives in Sources/.
            String sourcesRoot;
            if (m_context->Project() != nullptr)
            {
                sourcesRoot = m_context->Project()->SourcesRoot();
            }
            String language;
            RefPtr<ISerializable> object = instance.ReadObject();
            if (auto* asset = Cast<draconic::script::ScriptClassAsset>(object.Get()))
            {
                m_doc.Bind(sourcesRoot.AsView(), asset->fileName.View(),
                           asset->language.AsView());
                language = String(asset->language.AsView());
            }
            (void)m_doc.Load(); // an unreadable file just leaves an empty buffer

            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 4.0f;

            // The source editor: CodeEditView owns the gutter, markers, undo, and completion
            // (document-word provider; richer language providers arrive with code-editor P4).
            m_editor = MakeRef<ui::toolkit::CodeEditView>(DefaultAllocator());
            // Lexer by language id from the registry the script plugin populated - the page
            // stays backend-neutral; an unregistered language just renders unstyled.
            m_editor->SetLexer(ui::toolkit::CodeLexerRegistry::Get().Create(language.AsView()));
            // ONE bound-API surface for the page; completion and the API browser both
            // borrow it (same data, built once, lazily).
            m_apiSurface.SetLanguage(language.AsView());
            m_apiProvider.SetSurface(&m_apiSurface);
            m_editor->AddCompletionProvider(&m_apiProvider);
            m_editor->SetText(m_doc.Source());
            ScriptEditorPage* self = this;
            m_editor->OnTextChanged.Add(
                [self]()
                {
                    self->m_doc.SetSource(self->m_editor->Text());
                    self->MarkDirty();
                    self->m_validateDelay = 0.6f; // debounce a background compile-check
                });

            // Breakpoints are NATIVE markers now: seed from the shared store, and write every
            // gutter toggle back through it (the store stays 1-based; the buffer is 0-based).
            for (const EditorContext::ScriptBreakpoint& breakpoint : context.Breakpoints())
            {
                if (breakpoint.file.AsView() == m_doc.FileName() && breakpoint.line >= 1)
                {
                    m_editor->Document().SetMarker(breakpoint.line - 1,
                                                   ui::toolkit::CodeMarkers::Breakpoint);
                }
            }
            m_editor->OnBreakpointToggled.Add(
                [self](i32 line, bool)
                { self->m_context->ToggleBreakpoint(self->m_doc.FileName(), line + 1); });

            // Hover-values while paused at a breakpoint IN THIS FILE: the Game run installs
            // the context probe; the editor supplies the hovered identifier.
            m_editor->HoverValueProvider = [self](StringView identifier) -> String
            {
                const EditorContext::ScriptExecutionPoint& point =
                    self->m_context->ScriptExecution();
                if (!point.active || point.file.AsView() != self->m_doc.FileName() ||
                    !self->m_context->ScriptValueProbe)
                {
                    return String();
                }
                return self->m_context->ScriptValueProbe(identifier);
            };

            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                lp->Width = ui::SizeSpec::Match();
                column->AddView(m_editor.Get(), lp);
            }

            // Status row: the one-line compile status (OK / N error(s) / no cook) + the
            // API-browser toggle on the right.
            {
                auto statusRow = MakeRef<ui::FlexLayout>(DefaultAllocator());
                statusRow->Direction = ui::Orientation::Horizontal;
                statusRow->Spacing = 6.0f;

                m_status = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8""));
                m_status->FontSize.SetValue(12.0f);
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    statusRow->AddView(m_status.Get(), lp);
                }

                auto apiToggle = MakeRef<ui::ToggleButton>(DefaultAllocator(),
                                                                    StringView(u8"API"));
                apiToggle->OnCheckedChanged.Add(
                    [self](ui::ToggleButton*, bool checked)
                    {
                        // Visibility flip only - no view churn, safe mid-dispatch. The
                        // browser's first Update() after this builds the tree.
                        self->m_apiBrowser.Root()->Visibility =
                            checked ? ui::Visibility::Visible : ui::Visibility::Gone;
                        self->m_apiBrowser.Root()->Invalidate();
                    });
                statusRow->AddView(apiToggle.Get());

                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(statusRow.Get(), lp);
            }

            // The compile-error list: a read-only multi-line view (file:line + message).
            m_errorView = MakeRef<ui::EditText>(DefaultAllocator());
            m_errorView->Multiline.SetValue(true);
            m_errorView->IsReadOnly.SetValue(true);
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 0.35f;
                lp->Width = ui::SizeSpec::Match();
                column->AddView(m_errorView.Get(), lp);
            }

            // Root: the editor column + the (initially hidden) API browser side panel.
            auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
            row->Direction = ui::Orientation::Horizontal;
            row->Spacing = 4.0f;
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Grow = 1.0f;
                lp->Height = ui::SizeSpec::Match();
                row->AddView(column.Get(), lp);
            }
            m_apiBrowser.SetSurface(&m_apiSurface);
            m_apiBrowser.OnInsert = [self](StringView text)
            { self->m_editor->InsertAtCursor(text); };
            m_apiBrowser.Root()->Visibility = ui::Visibility::Gone;
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(300));
                lp->Height = ui::SizeSpec::Match();
                row->AddView(m_apiBrowser.Root(), lp);
            }

            m_content = row;
            RefreshCompileStatus(); // initial pass so the page opens with live state
        }

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }

        [[nodiscard]] Status Save() override;

        void OnUpdate(draconic::runtime::IApplicationHost&, f32 dt) override;

    private:
        // Compile-check the current buffer, repaint the status line + error list, and project
        // the errors onto the editor's lines as Error markers. Never touches view identity, so
        // it is safe to call from an event dispatch without the UI mutation queue.
        void RefreshCompileStatus();

        static void AppendCount(String& out, usize value);

        EditorContext* m_context = nullptr;
        draconic::script::ScriptSourceDocument m_doc;
        String m_title;
        f32 m_validateDelay = 0.0f;
        u64 m_executionVersionSeen = static_cast<u64>(-1); // poll stamp (ExecutionLine sync)
        ScriptApiSurface m_apiSurface; // the one bound-API source (completion + browser)
        ScriptApiCompletionProvider m_apiProvider; // outlives the editor that borrows it
        ScriptApiBrowserView m_apiBrowser;
        RefPtr<ui::View> m_content;
        RefPtr<ui::toolkit::CodeEditView> m_editor;
        RefPtr<ui::Label> m_status;
        RefPtr<ui::EditText> m_errorView;
    };

    class ScriptClassPageFactory final : public IEditorPageFactory
    {
    public:
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context,
                                                       content::Instance& instance) override;
    };

    // Seeds a fresh script asset: a starter source file (the language cook's NewAssetTemplate -
    // never hardcoded text) copied into Sources/, plus a ScriptClassAsset recording its file +
    // language. Backend-neutral: the caller passes the language id + extension from the backend
    // registry, so this works for whichever language the New Asset item is for.
    inline content::Instance* CreateScriptInstance(EditorContext& context, content::Group* group,
                                                   StringView languageId, StringView extension,
                                                   draconic::script::ScriptTier tier,
                                                   StringView baseName)
    {
        if (context.Project() == nullptr)
        {
            return nullptr;
        }
        content::Group* target =
            group != nullptr ? group : context.Project()->SourceDb().RootGroup();

        const String name = target->UniqueInstanceName(baseName);

        String fileName(name.AsView());
        fileName.PushBack(utf8char('.'));
        fileName.Append(extension);

        draconic::script::IScriptLanguageCook* cook =
            draconic::script::ScriptLanguageCookRegistry::Get().FindByLanguage(languageId);
        if (cook == nullptr)
        {
            return nullptr;
        }
        const StringView starter = cook->NewAssetTemplate(tier);

        const String path = PathJoin(context.Project()->SourcesRoot().AsView(), fileName.AsView());
        if (!WriteFile(
                 path.AsView(),
                 Span<const byte>(reinterpret_cast<const byte*>(starter.Data()), starter.Size()))
                 .IsOk())
        {
            return nullptr;
        }

        content::Instance* instance =
            target->CreateInstance(name.AsView(), draconic::script::ScriptClassAsset::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }
        draconic::script::ScriptClassAsset asset;
        asset.fileName = draconic::vfs::SourcePath(fileName.AsView());
        asset.language = String(languageId);
        if (!instance->WriteObject(asset).IsOk())
        {
            return nullptr;
        }
        context.RequestCook(false); // cook now so the new class is pickable + attachable
        return instance;
    }

    /// The editor executable's entry point for the script plugin: registers the ScriptPage
    /// factory + one New-Asset creator per registered script backend (Wren, AngelScript, ...).
    /// Language SYNTAX (lexer tables) is not registered here - each backend's editor-UI
    /// module does that (RegisterWrenEditorUI / RegisterAngelScriptEditorUI), keeping this
    /// page module backend-neutral.
    inline void RegisterScriptEditor(EditorContext& context)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<ScriptClassPageFactory>(), DefaultAllocator()));

        const auto backends = draconic::script::ScriptBackendRegistry::Get().All();
        DRACONIC_LOG_INFO(u8"Editor",
                          u8"RegisterScriptEditor: {} script backend(s) in the registry",
                          backends.Size());
        foundation::u32 registeredCreators = 0;
        for (const draconic::script::ScriptBackendDesc& backend : backends)
        {
            // A backend with no cook (compile/harvest) cannot seed a starter - skip it.
            if (draconic::script::ScriptLanguageCookRegistry::Get().FindByLanguage(
                    backend.languageId.AsView()) == nullptr)
            {
                DRACONIC_LOG_WARNING(
                    u8"Editor",
                    u8"  script backend '{}' has NO registered cook - no New-Asset creator",
                    backend.languageId);
                continue;
            }
            const String extension = backend.fileExtensions.IsEmpty()
                                         ? String(backend.languageId.AsView())
                                         : String(backend.fileExtensions[0].AsView());
            const StringView displayName = backend.displayName.IsEmpty()
                                               ? backend.languageId.AsView()
                                               : backend.displayName.AsView();

            // One New-Asset creator per tier (Behavior / Level / Game), each seeded from the
            // cook's tier starter. Labels read "<Language> <Tier>" under the Scripts category.
            struct TierDesc
            {
                draconic::script::ScriptTier tier;
                StringView suffix;   // label suffix
                StringView baseName; // unique-name stem
            };
            const TierDesc kTiers[] = {
                {draconic::script::ScriptTier::Behavior, u8"Behavior", u8"NewBehavior"},
                {draconic::script::ScriptTier::Level, u8"Level", u8"NewLevel"},
                {draconic::script::ScriptTier::Game, u8"Game", u8"NewGame"},
            };
            for (const TierDesc& t : kTiers)
            {
                String label(displayName);
                label.PushBack(utf8char(' '));
                label.Append(t.suffix);

                EditorContext::AssetCreator creator;
                creator.label = Move(label);
                creator.category = String(u8"Scripts");
                String languageId(backend.languageId.AsView());
                String extensionCopy(extension.AsView());
                String baseName(t.baseName);
                creator.create = [languageId = Move(languageId), extension = Move(extensionCopy),
                                  tier = t.tier, baseName = Move(baseName)](
                                     EditorContext& ctx, content::Group* g)
                {
                    return CreateScriptInstance(ctx, g, languageId.AsView(), extension.AsView(),
                                                tier, baseName.AsView());
                };
                context.RegisterCreator(Move(creator));
                ++registeredCreators;
            }
        }
        DRACONIC_LOG_INFO(u8"Editor",
                          u8"RegisterScriptEditor: {} script New-Asset creator(s) registered",
                          registeredCreators);
    }
}

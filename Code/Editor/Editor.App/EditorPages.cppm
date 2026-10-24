// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :pages partition.
//
// EditorPages: the open pages in the dock (Documentation/Specs/editor-gui.md, 5.1, layer 6). A page
// is a panel of its own, by its asset's guid (a Game tab by its play id: `game-page`,
// `game-page-1`, ...), the ids a saved layout and the MCP layout tools name; it opens in the central
// stack, or where a restored layout kept its place. Its tab says its asset's name, a `*` and the
// `dirty` class while it has unsaved changes, and the `active` class on the page the editor acts on
// (Save, the inspector): the chosen tab of the stack the focus was last in. Closing a page with
// unsaved changes asks first (Save, Discard, Cancel); a close always runs at the next frame, never
// inside the event that asked for it, and takes the page's panel with it. The open pages and the
// active one are saved with the project and reopen with it.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:pages;

import foundation.core;
import foundation.graphics;
import foundation.runtime.client; // IApplicationHost
import foundation.content;
import foundation.resource;
import foundation.settings;
import foundation.gui;
import foundation.gui.docking;
import editor.core;
import :ui_page;
import :editor_dock;
import :layout;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class EditorPages
    {
    public:
        /// What it works over.
        struct Services
        {
            editor::EditorContext* context = nullptr;
            EditorDock* dock = nullptr;
            editor::EditorCookService* cook = nullptr; // null: a page's missing products are not cooked
            /// The open project (its sources name the pages' assets); null: none.
            Function<editor::EditorProject*()> project;
            /// The project's resources (unresolved references cook with the page); null: none.
            Function<foundation::resource::ResourceManager*()> resources;
            /// Where its dialogs show; null: none shows (a dirty page then stays open).
            Function<gui::SceneRoot*()> dialogRoot;
        };

        EditorPages(IAllocator& allocator, Services services) : m_allocator(allocator), m_services(Move(services)), m_open(allocator), m_closing(allocator)
        {
            EditorPages* self = this;
            m_services.dock->Controller().SetOnActiveChanged([self](StringView panel) { self->NoteActivePanel(panel); });
            m_services.context->OnRevealPage = [self](editor::EditorPage* page) { self->Reveal(page); };
        }
        EditorPages(const EditorPages&) = delete;
        EditorPages& operator=(const EditorPages&) = delete;
        ~EditorPages()
        {
            m_services.dock->Controller().SetOnActiveChanged({});
            m_services.context->OnRevealPage = {};
        }

        /// Opens (or brings forward) the page for `instance`; null, said to the user, when no page
        /// edits its type.
        UIEditorPage* Open(foundation::content::Instance& instance)
        {
            editor::EditorContext& context = *m_services.context;
            const usize before = context.OpenPages().Size();
            editor::EditorPage* page = context.OpenPage(instance);
            if (page == nullptr)
            {
                LOG_WARNING(u8"Editor", u8"no editor page for asset '{}' - stored type '{}'::'{}', guid {} (unknown type name, or no page factory registered)",
                            instance.Name(), instance.TypeNamespace(), instance.TypeName(), instance.Id());
                context.Notify(editor::NoticeKind::Warning, u8"No editor registered for this asset type.");
                return nullptr;
            }
            // Every factory in this application makes a UIEditorPage (:ui_page's contract).
            auto* uiPage = static_cast<UIEditorPage*>(page);
            if (context.OpenPages().Size() == before)
            {
                Reveal(uiPage); // open already: its tab to front
                return uiPage;
            }
            Dock(*uiPage, PanelIdOf(instance.Id()));
            CookMissing(instance);
            return uiPage;
        }
        /// Opens (or brings forward) the page for the source asset `id`; null when there is none.
        UIEditorPage* Open(const Guid& id)
        {
            foundation::content::ContentDatabase* sources = Sources();
            foundation::content::Instance* instance = sources != nullptr ? sources->GetInstance(id) : nullptr;
            return instance != nullptr ? Open(*instance) : nullptr;
        }

        /// The Game tab (`newInstance`: another one beside it, with a game instance of its own).
        void OpenGame(bool newInstance)
        {
            editor::EditorContext& context = *m_services.context;
            if (!newInstance && m_gamePage != nullptr)
            {
                Reveal(m_gamePage);
                return;
            }
            if (!context.GamePageFactory)
            {
                context.Notify(editor::NoticeKind::Info, u8"No game page registered in this build.");
                return;
            }
            const String id = newInstance ? Format(u8"game-page-{}", m_gamePages + 1) : String(u8"game-page");
            UniquePtr<editor::EditorPage> made = context.GamePageFactory(newInstance, id.AsView());
            if (!made)
                return;
            auto* page = static_cast<UIEditorPage*>(context.AdoptPage(Move(made)));
            if (page == nullptr)
                return;
            if (newInstance)
                ++m_gamePages;
            else
                m_gamePage = page;
            Dock(*page, id);
        }

        /// Closes a page as its tab does: one with unsaved changes asks first.
        void RequestClose(editor::EditorPage* page)
        {
            if (const OpenPage* open = Find(page))
                (void)m_services.dock->Controller().Close(open->panel.AsView());
        }
        /// Closes a page for good at the next frame, without asking (a deleted asset's, an agent's).
        void Close(editor::EditorPage* page)
        {
            if (Find(page) != nullptr && !IsClosing(page))
                m_closing.PushBack(page);
        }
        /// Closes every page now (the project closing): their resources go while the device lives.
        void CloseAll()
        {
            m_closing.Clear();
            while (!m_open.IsEmpty())
                CloseNow(m_open.Back().page);
        }

        /// Saves a page, then what live tools edited in cooked products (any save flushes them).
        void Save(editor::EditorPage& page)
        {
            foundation::content::ContentDatabase* sources = Sources();
            editor::EditorContext& context = *m_services.context;
            if (page.Save().IsOk())
                context.Notify(editor::NoticeKind::Success, Format(u8"Saved '{}'.", page.Title()).AsView());
            else
                context.Notify(editor::NoticeKind::Error, u8"Save FAILED (see Console).");
            if (sources != nullptr && context.HasPendingAssetEdits())
            {
                if (context.DrainAssetEdits(*sources).IsOk())
                    context.Notify(editor::NoticeKind::Success, u8"Persisted live asset edits.");
                else
                    context.Notify(editor::NoticeKind::Error, u8"Asset-edit persist FAILED (see Console).");
            }
        }

        /// One frame, after the GUI laid out: Tick, then each page's frame hook.
        void Update(foundation::runtime::IApplicationHost& host, f32 deltaTime)
        {
            Tick();
            for (const OpenPage& open : m_open)
                open.page->OnUpdate(host, deltaTime);
        }
        /// The closes asked for run, and the tabs say each page's name, unsaved mark and active mark.
        void Tick()
        {
            RunCloses();
            SyncTabs();
        }
        /// Each page's offscreen content (inside the scene renderer's bracket), then its overlays.
        void Render(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame)
        {
            for (const OpenPage& open : m_open)
                open.page->OnRenderWindow(host, frame);
        }
        void AfterSceneRender(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame)
        {
            for (const OpenPage& open : m_open)
                open.page->OnAfterSceneRender(host, frame);
        }

        /// The open pages with an asset, and the active one, into the store.
        void Capture(foundation::settings::Settings& store) const
        {
            Array<Guid> pages;
            Guid active;
            for (const OpenPage& open : m_open)
            {
                if (open.page->InstanceId().IsNil())
                    continue; // a Game tab: nothing to reopen
                pages.PushBack(open.page->InstanceId());
                if (m_services.context->ActivePage() == open.page)
                    active = open.page->InstanceId();
            }
            CaptureOpenPages(store, pages, active);
        }
        /// The store's open pages opened again (`progress` hears each: the splash), the active
        /// one active. False when none were saved (a project's first open).
        bool Restore(foundation::settings::Settings& store, const Function<void(StringView, f32)>& progress = {})
        {
            Array<Guid> pages;
            Guid active;
            foundation::content::ContentDatabase* sources = Sources();
            if (sources == nullptr || !ApplyOpenPages(store, pages, active).IsOk())
                return false;
            UIEditorPage* toActivate = nullptr;
            for (usize i = 0; i < pages.Size(); ++i)
                if (foundation::content::Instance* instance = sources->GetInstance(pages[i]))
                {
                    if (progress)
                        progress(instance->Name(), static_cast<f32>(i) / static_cast<f32>(pages.Size()));
                    UIEditorPage* page = Open(*instance);
                    if (page != nullptr && pages[i] == active)
                        toActivate = page;
                }
            if (toActivate != nullptr)
                Reveal(toActivate);
            return true;
        }

        /// Brings a page's tab to front (a background tab's viewport never renders) and makes it
        /// the active page.
        void Reveal(editor::EditorPage* page)
        {
            const OpenPage* open = Find(page);
            if (open == nullptr)
                return;
            const String panel = open->panel;
            (void)m_services.dock->Controller().Edit([&panel](gui::DockLayout& layout) { return layout.Activate(panel.AsView()); });
            m_services.context->SetActivePage(page);
        }

        [[nodiscard]] usize Count() const noexcept { return m_open.Size(); }
        /// The panel id a page is docked by; null for a page not open here.
        [[nodiscard]] const String* PanelOf(const editor::EditorPage* page) const
        {
            const OpenPage* open = Find(page);
            return open != nullptr ? &open->panel : nullptr;
        }
        /// The panel id of an asset's page.
        [[nodiscard]] static String PanelIdOf(const Guid& id)
        {
            utf8char text[37];
            id.ToChars(text);
            return String(StringView(text, 36));
        }
        /// How many pages have unsaved changes.
        [[nodiscard]] usize DirtyCount() const
        {
            usize count = 0;
            for (const OpenPage& open : m_open)
                if (open.page->IsDirty())
                    ++count;
            return count;
        }

    private:
        struct OpenPage
        {
            UIEditorPage* page = nullptr; // the context owns it
            String panel;
        };

        // A page's panel: its content the page's, closing it asks the page.
        void Dock(UIEditorPage& page, const String& panel)
        {
            gui::DockController& dock = m_services.dock->Controller();
            if (dock.Describe(panel.AsView()) != nullptr)
                (void)dock.Unregister(panel.AsView()); // a page closed this frame, open again
            m_open.PushBack(OpenPage{&page, panel});
            EditorPages* self = this;
            UIEditorPage* raw = &page;
            gui::DockPanelDescription description;
            description.Id = panel;
            description.Title = String(page.Title());
            description.Close = gui::DockClosePolicy::Destroy;
            description.Default.Central = true;
            description.Factory = [raw]() { return raw->Content(); };
            // Closing always comes back here: the page asks (unsaved changes) and goes at the next
            // frame, its panel with it.
            description.CanClose = [self, raw]()
            {
                self->AskToClose(raw);
                return false;
            };
            (void)dock.Register(Move(description));
            if (dock.GetLayout().StackOf(panel.AsView()) == gui::kNoDock)
                (void)dock.Show(panel.AsView()); // else a restored layout placed it
            Reveal(raw);
        }

        void AskToClose(UIEditorPage* page)
        {
            if (!page->IsDirty())
            {
                Close(page);
                return;
            }
            gui::SceneRoot* root = m_services.dialogRoot ? m_services.dialogRoot() : nullptr;
            if (root == nullptr)
                return; // nothing to ask over: it stays open
            auto box = MakeRef<gui::MessageBox>(m_allocator);
            box->Configure(u8"Unsaved changes", Format(u8"'{}' has unsaved changes.", page->Title()).AsView(), gui::MessageBox::Buttons::Ok);
            box->ClearButtons();
            (void)box->AddButton(u8"Discard", u8"discard", gui::DialogRole::Destructive);
            (void)box->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
            (void)box->AddButton(u8"Save", u8"save", gui::DialogRole::Primary);
            EditorPages* self = this;
            gui::MessageBox* raw = box.Get();
            (void)box->AddEventListener(gui::EventType::Changed, [self, raw, page](const gui::Event&) { self->Answered(page, raw->GetResultName().AsView()); });
            (void)box->OpenModal(*root);
        }
        void Answered(UIEditorPage* page, StringView answer)
        {
            if (Find(page) == nullptr)
                return; // closed meanwhile
            if (answer == StringView(u8"save"))
            {
                if (!page->Save().IsOk())
                {
                    m_services.context->Notify(editor::NoticeKind::Error, u8"Save FAILED (see Console) - the page stays open.");
                    return;
                }
                Close(page);
            }
            else if (answer == StringView(u8"discard"))
                Close(page);
        }

        void RunCloses()
        {
            Array<editor::EditorPage*> closing = Move(m_closing);
            m_closing = Array<editor::EditorPage*>(m_allocator);
            for (editor::EditorPage* page : closing)
                CloseNow(page);
        }
        void CloseNow(editor::EditorPage* page)
        {
            for (usize i = 0; i < m_open.Size(); ++i)
                if (m_open[i].page == page)
                {
                    const String panel = m_open[i].panel;
                    m_open.RemoveAt(i);
                    if (page == m_gamePage)
                        m_gamePage = nullptr;
                    (void)m_services.dock->Controller().Unregister(panel.AsView()); // its content out of the GUI first
                    static_cast<UIEditorPage*>(page)->OnClose(); // its resources while the device and window live
                    m_services.context->ClosePage(page);         // destroys it
                    return;
                }
        }

        void SyncTabs()
        {
            foundation::content::ContentDatabase* sources = Sources();
            gui::DockController& dock = m_services.dock->Controller();
            const editor::EditorPage* active = m_services.context->ActivePage();
            for (const OpenPage& open : m_open)
            {
                foundation::content::Instance* instance =
                    sources != nullptr && !open.page->InstanceId().IsNil() ? sources->GetInstance(open.page->InstanceId()) : nullptr;
                String title(instance != nullptr ? instance->Name() : open.page->Title());
                if (open.page->IsDirty())
                    title += u8" *";
                (void)dock.SetTitle(open.panel.AsView(), title.AsView());
                const bool dirty = open.page->IsDirty();
                const bool isActive = open.page == active;
                const StringView classes = dirty && isActive ? StringView(u8"dirty active")
                                           : dirty           ? StringView(u8"dirty")
                                           : isActive        ? StringView(u8"active")
                                                             : StringView{};
                (void)dock.SetTabClasses(open.panel.AsView(), classes);
            }
        }

        // The dock's active panel changed: a page's makes it the active page; another panel's
        // (Assets, the Console) leaves the active page as it was.
        void NoteActivePanel(StringView panel)
        {
            for (const OpenPage& open : m_open)
                if (open.panel.AsView() == panel)
                {
                    m_services.context->SetActivePage(open.page);
                    return;
                }
        }

        // Its uncooked dependencies cook without a step of the user's.
        void CookMissing(foundation::content::Instance& instance)
        {
            if (m_services.cook == nullptr)
                return;
            Array<Guid> roots;
            if (foundation::resource::ResourceManager* resources = m_services.resources ? m_services.resources() : nullptr)
            {
                Array<Guid> unresolved;
                resources->CollectUnresolved(unresolved);
                // Only ids with a live source can cook: a reference to a deleted asset stays
                // unresolved, and must not ask for a cook on every open.
                for (const Guid& id : unresolved)
                    if (Sources() != nullptr && Sources()->GetInstance(id) != nullptr)
                        roots.PushBack(id);
            }
            const editor::CookBadge badge = m_services.cook->BadgeFor(instance);
            if (badge == editor::CookBadge::Missing || badge == editor::CookBadge::Failed)
                roots.PushBack(instance.Id());
            if (!roots.IsEmpty())
                m_services.cook->RequestCookFor(Move(roots), false);
        }

        [[nodiscard]] foundation::content::ContentDatabase* Sources() const
        {
            editor::EditorProject* project = m_services.project ? m_services.project() : nullptr;
            return project != nullptr ? &project->SourceDb() : nullptr;
        }
        [[nodiscard]] const OpenPage* Find(const editor::EditorPage* page) const
        {
            for (const OpenPage& open : m_open)
                if (open.page == page)
                    return &open;
            return nullptr;
        }
        [[nodiscard]] bool IsClosing(const editor::EditorPage* page) const
        {
            for (const editor::EditorPage* closing : m_closing)
                if (closing == page)
                    return true;
            return false;
        }

        IAllocator& m_allocator;
        Services m_services;
        Array<OpenPage> m_open;
        Array<editor::EditorPage*> m_closing; // closes asked for, run at the next frame
        UIEditorPage* m_gamePage = nullptr;   // the primary Game tab
        u32 m_gamePages = 0;                  // the Game tabs beside it, ever
    };
}

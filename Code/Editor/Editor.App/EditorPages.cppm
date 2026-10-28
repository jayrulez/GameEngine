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
import :asset_create_dialog; // AssetNames: the rule a new name keeps

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class EditorPages
    {
    public:
        /// Save As's dialog, under the data root.
        static constexpr const char8_t* kSaveAsPath = u8"Editor/SaveAs.xml";

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
            /// A dialog's markup by its path under the data root (Save As's); null: none.
            Function<RefPtr<gui::Node>(StringView)> loadMarkup;
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
        /// A user's open of the source asset `id` (the browser's, a slot's Open): an editor that
        /// claims it takes it first (the scene page on screen takes a property animation clip into
        /// its panel); else its page opens. False when nothing took it.
        bool OpenForUser(const Guid& id)
        {
            foundation::content::ContentDatabase* sources = Sources();
            foundation::content::Instance* instance = sources != nullptr ? sources->GetInstance(id) : nullptr;
            if (instance == nullptr)
                return false;
            if (m_services.context->TryInterceptOpenAsset(*instance))
                return true;
            return Open(*instance) != nullptr;
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
        /// Closes every page editing `asset` now, without asking: what deletes the asset calls it
        /// before the instance goes, outside an event's dispatch.
        void CloseAssetNow(const Guid& asset)
        {
            Array<editor::EditorPage*> pages(m_allocator);
            for (const OpenPage& open : m_open)
                if (open.page->InstanceId() == asset)
                    pages.PushBack(open.page);
            for (editor::EditorPage* page : pages)
            {
                for (usize i = 0; i < m_closing.Size(); ++i)
                    if (m_closing[i] == page)
                    {
                        m_closing.RemoveAt(i);
                        break;
                    }
                CloseNow(page);
            }
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

        /// The unsaved pages, before `proceed` (an exit, a project's close): with none it runs now;
        /// else the user is asked to save them all, keep working or go on without saving
        /// (`discardVerb`: "Exit Without Saving"), and it runs only when they chose to go on or
        /// every page saved. Answers whether it ran now.
        bool ConfirmUnsavedThen(StringView saveVerb, StringView discardVerb, Function<void()> proceed)
        {
            const usize dirty = DirtyCount();
            if (dirty == 0)
            {
                proceed();
                return true;
            }
            gui::SceneRoot* root = m_services.dialogRoot ? m_services.dialogRoot() : nullptr;
            if (root == nullptr)
                return false; // nothing to ask over: the unsaved work stays
            // Each named, as the page's tab and its panels name them (a count says nothing of where).
            Array<String> lines(m_allocator);
            for (const OpenPage& open : m_open)
                UnsavedLines(*open.page, lines);
            const String message = UnsavedText(lines);
            auto box = MakeRef<gui::MessageBox>(m_allocator);
            box->Configure(u8"Unsaved changes", message.AsView(), gui::MessageBox::Buttons::Ok);
            box->ClearButtons();
            (void)box->AddButton(discardVerb, u8"discard", gui::DialogRole::Destructive);
            (void)box->AddButton(u8"Cancel", u8"cancel", gui::DialogRole::Cancel);
            (void)box->AddButton(saveVerb, u8"save", gui::DialogRole::Primary);
            EditorPages* self = this;
            gui::MessageBox* raw = box.Get();
            auto then = MakeRef<Pending>(m_allocator, foundation::core::Move(proceed));
            (void)box->AddEventListener(gui::EventType::Changed, [self, raw, then](const gui::Event&)
                                        {
                                            const StringView answer = raw->GetResultName().AsView();
                                            if (answer == StringView(u8"discard"))
                                                then->proceed();
                                            else if (answer == StringView(u8"save"))
                                            {
                                                if (self->SaveAll())
                                                    then->proceed();
                                                else
                                                    self->m_services.context->Notify(editor::NoticeKind::Error, u8"Save FAILED (see Console) - nothing closed.");
                                            }
                                        });
            box->OpenModal(*root);
            return false;
        }
        /// Saves every page with unsaved changes; false when one failed (the rest still tried).
        bool SaveAll()
        {
            bool all = true;
            for (const OpenPage& open : m_open)
            {
                if (open.page->IsDirty() && !open.page->Save().IsOk())
                    all = false;
                if (open.page->HasUnsavedWork() && !open.page->SaveBesides().IsOk())
                    all = false; // a clip its panel edits
            }
            return all;
        }

        /// Save As: the page's content into a new asset beside its own, named in a dialog (a name
        /// empty or taken keeps the dialog open, saying why); the page edits the new asset after.
        void SaveAs(editor::EditorPage& page)
        {
            editor::EditorContext& context = *m_services.context;
            foundation::content::ContentDatabase* sources = Sources();
            foundation::content::Instance* original = sources != nullptr ? sources->GetInstance(page.InstanceId()) : nullptr;
            if (original == nullptr)
            {
                context.Notify(editor::NoticeKind::Warning, u8"This page has no source asset to copy.");
                return;
            }
            gui::SceneRoot* root = m_services.dialogRoot ? m_services.dialogRoot() : nullptr;
            RefPtr<gui::Node> body = m_services.loadMarkup ? m_services.loadMarkup(kSaveAsPath) : RefPtr<gui::Node>();
            if (root == nullptr || !body)
                return;
            foundation::content::Group* group = &original->OwningGroup();
            String suggested = Format(u8"{} Copy", original->Name());
            while (group->GetInstance(suggested.AsView()) != nullptr)
                suggested += u8" Copy";
            auto window = MakeRef<gui::Window>(m_allocator);
            window->SetTitle(u8"Save As");
            window->AddClass(u8"save-as-window");
            window->SetSize(Float2{kSaveAsWidth, kSaveAsHeight});
            window->GetContent()->AddChild(body.Get());
            auto* prompt = Cast<gui::Label>(FindIn(*body, u8"save-as-prompt"));
            auto* name = Cast<gui::TextField>(FindIn(*body, u8"save-as-name"));
            auto* error = Cast<gui::Label>(FindIn(*body, u8"save-as-error"));
            auto* save = Cast<gui::Button>(FindIn(*body, u8"save-as-save"));
            auto* cancel = Cast<gui::Button>(FindIn(*body, u8"save-as-cancel"));
            if (name == nullptr || save == nullptr)
                return;
            if (prompt != nullptr)
                prompt->SetText(Format(u8"A new asset beside '{}', named:", original->Name()).AsView());
            name->SetText(suggested.AsView());
            const Guid pageId = page.InstanceId();
            EditorPages* self = this;
            gui::Window* raw = window.Get();
            const auto commit = [self, raw, name, error, pageId, group]()
            {
                const StringView newName = name->GetText();
                const String refused = AssetNames::Refusal(*group, newName); // the rule Create Asset keeps
                if (!refused.IsEmpty())
                {
                    if (error != nullptr)
                        error->SetText(refused.AsView());
                    return; // the dialog stays for the retry
                }
                self->SaveAsNamed(pageId, *group, newName);
                raw->Close();
            };
            save->SetOnClick(commit);
            (void)name->AddEventListener(gui::EventType::Submit, [commit](const gui::Event&) { commit(); });
            if (cancel != nullptr)
                cancel->SetOnClick([raw]() { raw->Close(); });
            window->OpenModal(*root);
            name->RequestFocus();
            name->SelectAll();
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
        /// What is unsaved on `page`, a line each: its own changes by its name ("Level1"), the work
        /// it holds besides with its name ("The clip 'Bounce', in Level1").
        static void UnsavedLines(const editor::EditorPage& page, Array<String>& out)
        {
            if (page.IsDirty())
                out.PushBack(String(page.Title()));
            Array<String> besides;
            page.ListUnsavedBesides(besides);
            for (const String& work : besides)
                out.PushBack(Format(u8"{}, in {}", work, page.Title()));
        }
        /// The prompt's words for `lines`: one in a sentence, several listed.
        [[nodiscard]] static String UnsavedText(const Array<String>& lines)
        {
            if (lines.Size() == 1)
                return Format(u8"{} has unsaved changes.", lines[0]);
            String text(u8"These have unsaved changes:");
            for (const String& line : lines)
                text += Format(u8"\n    {}", line).AsView();
            return text;
        }

        /// How many pages have unsaved changes, or hold unsaved work of their own (a clip).
        [[nodiscard]] usize DirtyCount() const
        {
            usize count = 0;
            for (const OpenPage& open : m_open)
                if (open.page->HasUnsavedWork())
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
            if (!page->HasUnsavedWork())
            {
                Close(page);
                return;
            }
            gui::SceneRoot* root = m_services.dialogRoot ? m_services.dialogRoot() : nullptr;
            if (root == nullptr)
                return; // nothing to ask over: it stays open
            auto box = MakeRef<gui::MessageBox>(m_allocator);
            // What is unsaved, each named: the page's own changes, the work it holds besides (a clip).
            Array<String> lines(m_allocator);
            UnsavedLines(*page, lines);
            box->Configure(u8"Unsaved changes", UnsavedText(lines).AsView(), gui::MessageBox::Buttons::Ok);
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
                if ((page->IsDirty() && !page->Save().IsOk()) || !page->SaveBesides().IsOk())
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

        // What a confirmation goes on to, kept alive by the dialog's listener.
        struct Pending final : RefCounted
        {
            explicit Pending(Function<void()> run) : proceed(foundation::core::Move(run)) {}
            Function<void()> proceed;
        };

        // The page `pageId` (if still open) saved into a new asset `newName` in `group`.
        void SaveAsNamed(const Guid& pageId, foundation::content::Group& group, StringView newName)
        {
            editor::EditorContext& context = *m_services.context;
            editor::EditorPage* target = nullptr;
            for (const OpenPage& open : m_open)
                if (open.page->InstanceId() == pageId)
                    target = open.page;
            foundation::content::ContentDatabase* sources = Sources();
            foundation::content::Instance* original = sources != nullptr ? sources->GetInstance(pageId) : nullptr;
            if (target == nullptr || original == nullptr)
                return; // closed meanwhile
            const TypeInfo* type = GlobalTypeRegistry().FindByName(reinterpret_cast<const char*>(String(original->TypeNamespace()).CStr()),
                                                                   reinterpret_cast<const char*>(String(original->TypeName()).CStr()));
            foundation::content::Instance* fresh = type != nullptr ? group.CreateInstance(newName, *type) : nullptr;
            if (fresh == nullptr)
            {
                context.Notify(editor::NoticeKind::Error, u8"Not saved: the new asset could not be made.");
                return;
            }
            target->OnSavedAs(*fresh);
            // The page's panel follows its new asset.
            for (OpenPage& open : m_open)
                if (open.page == target)
                {
                    const String panel = PanelIdOf(fresh->Id());
                    (void)m_services.dock->Controller().Unregister(open.panel.AsView());
                    m_open.RemoveAt(static_cast<usize>(&open - m_open.Data()));
                    Dock(*static_cast<UIEditorPage*>(target), panel);
                    break;
                }
            if (target->Save().IsOk())
                context.Notify(editor::NoticeKind::Success, Format(u8"Saved as '{}'.", fresh->Name()).AsView());
            else
                context.Notify(editor::NoticeKind::Error, u8"Save As FAILED (see Console).");
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

        static constexpr f32 kSaveAsWidth = 420.0f;
        static constexpr f32 kSaveAsHeight = 200.0f;

        IAllocator& m_allocator;
        Services m_services;
        Array<OpenPage> m_open;
        Array<editor::EditorPage*> m_closing; // closes asked for, run at the next frame
        UIEditorPage* m_gamePage = nullptr;   // the primary Game tab
        u32 m_gamePages = 0;                  // the Game tabs beside it, ever
    };
}

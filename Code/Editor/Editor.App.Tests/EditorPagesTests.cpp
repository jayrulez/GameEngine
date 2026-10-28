// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorPages over a real EditorContext and EditorDock with a test page factory: a
// page docked by its asset's guid in the central stack (and brought forward when opened again),
// its tab's name, unsaved mark and active mark; a close run at the next frame, a page with unsaved
// changes asking first (Save, Discard, Cancel); the active page following the dock; the Game tabs;
// the open pages through the store and back; the unsaved-pages gate before an exit; Save As.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.gui;
import foundation.gui.docking;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace content = foundation::content;
namespace gui = foundation::gui;
using editor::app::EditorDock;
using editor::app::EditorPages;

namespace
{
    class PagesAsset : public ISerializable
    {
        RTTI_OBJECT(PagesAsset, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    class TestPage final : public editor::app::UIEditorPage
    {
    public:
        explicit TestPage(StringView title) : UIEditorPage(DefaultAllocator()), m_title(title) {}
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override
        {
            ++saves;
            ClearDirty();
            return Status{};
        }
        [[nodiscard]] RefPtr<gui::Node> Content() override
        {
            if (!m_content)
                m_content = MakeRef<gui::Panel>(DefaultAllocator());
            return RefPtr<gui::Node>(m_content);
        }
        void OnClose() override
        {
            if (closed != nullptr)
                ++*closed;
        }
        // Work it holds besides its asset (a scene page's clip).
        void ListUnsavedBesides(Array<String>& out) const override
        {
            if (!besides.IsEmpty())
                out.PushBack(besides);
        }
        Status SaveBesides() override
        {
            if (besides.IsEmpty())
                return Status{};
            ++besidesSaves;
            besides = String{};
            return Status{};
        }
        int saves = 0;
        int* closed = nullptr;
        String besides;
        int besidesSaves = 0;

    private:
        String m_title;
        RefPtr<gui::Panel> m_content;
    };

    class TestPageFactory final : public editor::IEditorPageFactory
    {
    public:
        explicit TestPageFactory(int& closed) : m_closed(&closed) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override { return &PagesAsset::StaticType(); }
        [[nodiscard]] UniquePtr<editor::EditorPage> CreatePage(editor::EditorContext&, content::Instance& instance) override
        {
            auto* page = DefaultAllocator().New<TestPage>(instance.Name());
            page->closed = m_closed;
            return UniquePtr<editor::EditorPage>(page, DefaultAllocator());
        }

    private:
        int* m_closed;
    };

    gui::MessageBox* FindBox(gui::Node& node)
    {
        if (auto* box = Cast<gui::MessageBox>(&node); box != nullptr && box->IsOpen())
            return box;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (gui::MessageBox* found = FindBox(*node.GetChildAt(i)))
                return found;
        return nullptr;
    }
    gui::Button* ButtonNamed(gui::MessageBox& box, StringView text)
    {
        for (usize i = 0; i < box.ButtonCount(); ++i)
            if (box.ButtonAt(i)->GetText() == text)
                return box.ButtonAt(i);
        return nullptr;
    }

    RefPtr<gui::Node> LoadMarkup(StringView path)
    {
        const String full = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), path);
        Result<Array<byte>> bytes = ReadFile(full.AsView());
        if (!bytes.HasValue())
            return {};
        gui::RegisterGuiElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    T* FindId(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }
    gui::Window* FindWindow(gui::Node& node)
    {
        if (auto* window = Cast<gui::Window>(&node); window != nullptr && window->IsOpen() && Cast<gui::MessageBox>(&node) == nullptr)
            return window;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (gui::Window* found = FindWindow(*node.GetChildAt(i)))
                return found;
        return nullptr;
    }

    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        content::Instance* one = nullptr;
        content::Instance* two = nullptr;
        editor::EditorContext context{DefaultAllocator()};
        int closed = 0;
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::Panel> into = MakeRef<gui::Panel>(DefaultAllocator());
        EditorDock dock{DefaultAllocator()};
        UniquePtr<EditorPages> pages;

        Bed()
        {
            std::error_code ec;
            std::filesystem::remove_all("editor_pages_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"editor_pages_project", u8"Pages").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"editor_pages_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(PagesAsset::StaticType());
            one = project->SourceDb().RootGroup()->CreateInstance(u8"One", PagesAsset::StaticType());
            two = project->SourceDb().RootGroup()->CreateInstance(u8"Two", PagesAsset::StaticType());
            REQUIRE(one != nullptr);
            REQUIRE(two != nullptr);
            context.Pages().Register(UniquePtr<editor::IEditorPageFactory>(DefaultAllocator().New<TestPageFactory>(closed), DefaultAllocator()));

            root->SetSize(Float2{800.0f, 600.0f});
            root->AddChild(into.Get());
            dock.Open(*into, nullptr, EditorDock::Contents{});
            EditorPages::Services services;
            services.context = &context;
            services.dock = &dock;
            Bed* self = this;
            services.project = [self]() { return self->project.Get(); };
            services.dialogRoot = [self]() { return self->root.Get(); };
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            pages = MakeUnique<EditorPages>(DefaultAllocator(), DefaultAllocator(), Move(services));
        }
        ~Bed()
        {
            pages.Reset();
            std::error_code ec;
            std::filesystem::remove_all("editor_pages_project", ec);
        }
        void Frame()
        {
            (void)root->UpdateLayout();
            pages->Tick();
        }
        [[nodiscard]] const gui::DockLayout& Layout() { return dock.Controller().GetLayout(); }
        [[nodiscard]] String Panel(content::Instance& instance) const { return EditorPages::PanelIdOf(instance.Id()); }
    };
}

RTTI_DEFINE_OBJECT(PagesAsset, "rtti::editor::app::pagestest")

TEST_CASE("editor pages: a page docks by its asset's guid in the central stack, once; its tab says its name, unsaved and active marks")
{
    Bed bed;
    editor::app::UIEditorPage* one = bed.pages->Open(*bed.one);
    REQUIRE(one != nullptr);
    const String panel = bed.Panel(*bed.one);
    CHECK(bed.Layout().StackOf(panel.AsView()) == bed.Layout().CentralStack());
    CHECK(bed.context.ActivePage() == one);
    CHECK(bed.dock.Controller().Content(panel.AsView()).Get() == one->Content().Get());

    editor::app::UIEditorPage* two = bed.pages->Open(*bed.two);
    REQUIRE(two != nullptr);
    CHECK(bed.pages->Open(*bed.one) == one); // open already: brought forward, not opened again
    CHECK(bed.pages->Count() == 2u);
    CHECK(bed.context.ActivePage() == one);

    one->MarkDirty();
    bed.Frame();
    const gui::DockPanelDescription* described = bed.dock.Controller().Describe(panel.AsView());
    REQUIRE(described != nullptr);
    CHECK(described->Title == String(u8"One *"));
    CHECK(described->TabClasses == String(u8"dirty active"));
    CHECK(bed.dock.Controller().Describe(bed.Panel(*bed.two).AsView())->TabClasses.IsEmpty());

    REQUIRE(bed.project->SourceDb().RenameInstance(bed.one->Id(), u8"First").IsOk()); // the asset renamed: its tab follows
    one->ClearDirty();
    bed.Frame();
    CHECK(described->Title == String(u8"First"));
    CHECK(described->TabClasses == String(u8"active"));
}

TEST_CASE("editor pages: a user's open goes first to an editor that claims the asset, else its page opens")
{
    Bed bed;
    Array<Guid> claimed;
    const u64 interceptor = bed.context.AddOpenAssetInterceptor([&claimed, &bed](foundation::content::Instance& instance)
                                                                {
                                                                    if (instance.Id() != bed.two->Id())
                                                                        return false;
                                                                    claimed.PushBack(instance.Id());
                                                                    return true;
                                                                });
    CHECK(bed.pages->OpenForUser(bed.two->Id()));
    REQUIRE(claimed.Size() == 1u);
    CHECK(bed.pages->Count() == 0u); // taken: no page
    CHECK(bed.pages->OpenForUser(bed.one->Id()));
    CHECK(bed.pages->Count() == 1u); // not claimed: its page
    CHECK_FALSE(bed.pages->OpenForUser(Guid{7u, 7u})); // no such asset
    bed.context.RemoveOpenAssetInterceptor(interceptor);
    CHECK(bed.pages->Open(*bed.two) != nullptr); // a page open (MCP's page_open, a restore) is never claimed
}

TEST_CASE("editor pages: a clean page closes at the next frame with its panel; one with unsaved changes asks: Cancel, Discard, Save")
{
    Bed bed;
    editor::app::UIEditorPage* one = bed.pages->Open(*bed.one);
    REQUIRE(one != nullptr);
    const String panel = bed.Panel(*bed.one);
    CHECK_FALSE(bed.dock.Controller().Close(panel.AsView()).Ok()); // its tab's close: the page decides
    CHECK(bed.pages->Count() == 1u);                                  // not inside the event
    bed.Frame();
    CHECK(bed.pages->Count() == 0u);
    CHECK(bed.closed == 1);
    CHECK(bed.context.OpenPages().Size() == 0u);
    CHECK(bed.dock.Controller().Describe(panel.AsView()) == nullptr);
    CHECK(bed.Layout().StackOf(panel.AsView()) == gui::kNoDock);

    auto* page = static_cast<TestPage*>(bed.pages->Open(*bed.one));
    REQUIRE(page != nullptr);
    page->MarkDirty();
    const auto ask = [&]() -> gui::MessageBox&
    {
        bed.pages->RequestClose(page);
        (void)bed.root->UpdateLayout();
        gui::MessageBox* box = FindBox(*bed.root);
        REQUIRE(box != nullptr);
        return *box;
    };
    gui::MessageBox* box = &ask();
    REQUIRE(ButtonNamed(*box, u8"Cancel") != nullptr);
    CHECK(ButtonNamed(*box, u8"Cancel")->HandleActivate());
    bed.Frame();
    CHECK(bed.pages->Count() == 1u); // kept

    box = &ask();
    CHECK(ButtonNamed(*box, u8"Save")->HandleActivate());
    CHECK(page->saves == 1);
    bed.Frame();
    CHECK(bed.pages->Count() == 0u);
    CHECK(bed.closed == 2);

    page = static_cast<TestPage*>(bed.pages->Open(*bed.two));
    REQUIRE(page != nullptr);
    page->MarkDirty();
    box = &ask();
    CHECK(ButtonNamed(*box, u8"Discard")->HandleActivate());
    CHECK(page->saves == 0);
    bed.Frame();
    CHECK(bed.pages->Count() == 0u);
    CHECK(bed.closed == 3);
}

TEST_CASE("editor pages: a page holding unsaved work besides its asset is asked about on close and at exit, that work saved with it")
{
    Bed bed;
    auto* page = static_cast<TestPage*>(bed.pages->Open(*bed.one));
    REQUIRE(page != nullptr);
    page->besides = String(u8"The clip 'Bounce'");
    CHECK_FALSE(page->IsDirty()); // its own mark says nothing of the clip
    bed.pages->RequestClose(page);
    (void)bed.root->UpdateLayout();
    gui::MessageBox* box = FindBox(*bed.root);
    REQUIRE(box != nullptr);
    CHECK(box->GetMessage() == StringView(u8"The clip 'Bounce', in One has unsaved changes."));
    CHECK(ButtonNamed(*box, u8"Save")->HandleActivate());
    CHECK(page->besidesSaves == 1);
    CHECK(page->saves == 0); // nothing of its own to save
    bed.Frame();
    CHECK(bed.pages->Count() == 0u);

    // The exit gate counts it, and Save All saves it.
    page = static_cast<TestPage*>(bed.pages->Open(*bed.two));
    REQUIRE(page != nullptr);
    page->besides = String(u8"The clip 'Spin'");
    page->MarkDirty();
    int proceeded = 0;
    CHECK_FALSE(bed.pages->ConfirmUnsavedThen(u8"Save All and Exit", u8"Exit Without Saving", [&proceeded]() { ++proceeded; }));
    (void)bed.root->UpdateLayout();
    box = FindBox(*bed.root);
    REQUIRE(box != nullptr);
    // Each named, the page's own changes and its clip: never a count of pages.
    CHECK(box->GetMessage() == StringView(u8"These have unsaved changes:\n    Two\n    The clip 'Spin', in Two"));
    CHECK(ButtonNamed(*box, u8"Save All and Exit")->HandleActivate());
    CHECK(page->besidesSaves == 1);
    CHECK(proceeded == 1);
}

TEST_CASE("editor pages: the active page follows the dock's active panel; another panel leaves it; reveal brings a tab forward")
{
    Bed bed;
    editor::app::UIEditorPage* one = bed.pages->Open(*bed.one);
    editor::app::UIEditorPage* two = bed.pages->Open(*bed.two);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    CHECK(bed.context.ActivePage() == two);

    gui::DockController& dock = bed.dock.Controller();
    const gui::DockNodeId central = bed.Layout().CentralStack();
    dock.NoteFocused(central); // the focus into the documents: their chosen tab
    dock.NoteActive(bed.Panel(*bed.one).AsView());
    CHECK(bed.context.ActivePage() == one);
    dock.NoteFocused(bed.Layout().StackOf(editor::app::kPanelConsole)); // the Console: the page stays
    CHECK(bed.context.ActivePage() == one);

    bed.context.RevealPage(two);
    const gui::DockNode* node = bed.Layout().FindNode(central);
    REQUIRE(node != nullptr);
    CHECK(node->Panels[node->Active] == bed.Panel(*bed.two));
    CHECK(bed.context.ActivePage() == two);
}

TEST_CASE("editor pages: the Game tab opens once and is brought forward after; a new instance opens beside it by its own id")
{
    Bed bed;
    int made = 0;
    bed.context.GamePageFactory = [&made](bool, StringView) -> UniquePtr<editor::EditorPage>
    {
        ++made;
        return UniquePtr<editor::EditorPage>(DefaultAllocator().New<TestPage>(u8"Game"), DefaultAllocator());
    };
    bed.pages->OpenGame(false);
    bed.pages->OpenGame(false);
    CHECK(made == 1);
    CHECK(bed.Layout().StackOf(u8"game-page") != gui::kNoDock);
    bed.pages->OpenGame(true);
    CHECK(made == 2);
    CHECK(bed.Layout().StackOf(u8"game-page-1") != gui::kNoDock);
    bed.pages->CloseAll();
    CHECK(bed.pages->Count() == 0u);
}

TEST_CASE("editor pages: the open pages and the active one through the store and back; Game tabs are not saved")
{
    Bed bed;
    bed.context.GamePageFactory = [](bool, StringView) -> UniquePtr<editor::EditorPage>
    { return UniquePtr<editor::EditorPage>(DefaultAllocator().New<TestPage>(u8"Game"), DefaultAllocator()); };
    foundation::settings::Settings store(DefaultAllocator());
    CHECK_FALSE(bed.pages->Restore(store)); // none saved: the caller opens the default
    editor::app::UIEditorPage* one = bed.pages->Open(*bed.one);
    REQUIRE(bed.pages->Open(*bed.two) != nullptr);
    bed.pages->OpenGame(false);
    bed.context.RevealPage(one);
    bed.pages->Capture(store);
    bed.pages->CloseAll();
    REQUIRE(bed.pages->Count() == 0u);

    REQUIRE(bed.pages->Restore(store));
    CHECK(bed.pages->Count() == 2u);
    REQUIRE(bed.context.ActivePage() != nullptr);
    CHECK(bed.context.ActivePage()->InstanceId() == bed.one->Id());
}

TEST_CASE("editor pages: the unsaved-pages gate goes on at once with none; with some it asks: Cancel stays, the other two go on, saving first")
{
    Bed bed;
    int proceeded = 0;
    const auto proceed = [&proceeded]() { ++proceeded; };
    CHECK(bed.pages->ConfirmUnsavedThen(u8"Save All and Exit", u8"Exit Without Saving", proceed));
    CHECK(proceeded == 1);

    auto* one = static_cast<TestPage*>(bed.pages->Open(*bed.one));
    auto* two = static_cast<TestPage*>(bed.pages->Open(*bed.two));
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    one->MarkDirty();
    two->MarkDirty();
    const auto ask = [&]() -> gui::MessageBox&
    {
        CHECK_FALSE(bed.pages->ConfirmUnsavedThen(u8"Save All and Exit", u8"Exit Without Saving", proceed));
        (void)bed.root->UpdateLayout();
        gui::MessageBox* box = FindBox(*bed.root);
        REQUIRE(box != nullptr);
        return *box;
    };
    gui::MessageBox* box = &ask();
    REQUIRE(ButtonNamed(*box, u8"Exit Without Saving") != nullptr);
    CHECK(ButtonNamed(*box, u8"Cancel")->HandleActivate());
    CHECK(proceeded == 1);
    box = &ask();
    CHECK(ButtonNamed(*box, u8"Save All and Exit")->HandleActivate());
    CHECK(one->saves == 1);
    CHECK(two->saves == 1);
    CHECK(proceeded == 2);
    one->MarkDirty();
    box = &ask();
    CHECK(ButtonNamed(*box, u8"Exit Without Saving")->HandleActivate());
    CHECK(one->saves == 1); // not saved
    CHECK(proceeded == 3);
}

TEST_CASE("editor pages: Save As names a new asset beside the page's; an empty or taken name keeps the dialog open; the page and its panel follow the new asset")
{
    Bed bed;
    auto* page = static_cast<TestPage*>(bed.pages->Open(*bed.one));
    REQUIRE(page != nullptr);
    const Guid original = bed.one->Id();
    bed.pages->SaveAs(*page);
    (void)bed.root->UpdateLayout();
    gui::Window* window = FindWindow(*bed.root);
    REQUIRE(window != nullptr);
    auto* name = FindId<gui::TextField>(*window, u8"save-as-name");
    auto* error = FindId<gui::Label>(*window, u8"save-as-error");
    auto* save = FindId<gui::Button>(*window, u8"save-as-save");
    REQUIRE(name != nullptr);
    REQUIRE(error != nullptr);
    REQUIRE(save != nullptr);
    CHECK(name->GetText() == StringView(u8"One Copy"));

    name->SetText(StringView{});
    CHECK(save->HandleActivate());
    CHECK(window->IsOpen());
    CHECK(error->GetText() == StringView(u8"Enter a name."));
    name->SetText(u8"Two"); // taken
    CHECK(save->HandleActivate());
    CHECK(window->IsOpen());
    CHECK(error->GetText() == StringView(u8"Two is already in Content."));

    name->SetText(u8"One Again");
    CHECK(save->HandleActivate());
    CHECK_FALSE(window->IsOpen());
    content::Instance* made = bed.project->SourceDb().RootGroup()->GetInstance(u8"One Again");
    REQUIRE(made != nullptr);
    CHECK(page->InstanceId() == made->Id());
    CHECK(page->saves == 1);
    CHECK(bed.Layout().StackOf(EditorPages::PanelIdOf(made->Id()).AsView()) != gui::kNoDock);
    CHECK(bed.Layout().StackOf(EditorPages::PanelIdOf(original).AsView()) == gui::kNoDock);
    CHECK(bed.pages->PanelOf(page) != nullptr);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the project manager over its markup (Data/Editor/ProjectManager.xml). The
// recent projects show as a list or a grid (the choice is remembered), each with its picture (the
// thumbnail in its editor folder, an image chosen for it, or its initial); missing ones in a section
// of their own with Locate... and no Open. Also covers the pin, the sort and the search, the keys and
// the focus ring, the first run's welcome, a dropped folder, each project's menu and what it runs,
// and the New Project dialog.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.settings;
import foundation.image;
import foundation.image.io;
import foundation.gui;
import editor.core;
import editor.app;
import foundation.net;
import foundation.http.client;

#include "LoopbackFiles.h"

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
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
    gui::Button* ButtonText(gui::Node& node, StringView text)
    {
        if (auto* button = Cast<gui::Button>(&node); button != nullptr && button->GetText() == text)
            return button;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (gui::Button* found = ButtonText(*node.GetChildAt(i), text))
                return found;
        return nullptr;
    }

    // The editor's sheet as its window builds it (EditorWindow::SheetFor, at the default
    // density), its @import of the tokens loaded from Data/ like the window's: without them every
    // var() declaration (borders, colours, spacing) would be dropped.
    struct EditorSheet
    {
        editor::EditorResources resources{DefaultAllocator(), PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView()};
        gui::StyleManager styles;
        EditorSheet()
        {
            app::EditorLook look;
            look.density = String(u8"default");
            styles.SetResourceProvider(&resources);
            styles.SetStyleSheet(gui::CSSParser::Parse(app::EditorWindow::SheetFor(resources, look, nullptr).AsView(), StringView(u8"Editor/editor.css")));
        }
    };

    struct Launcher
    {
        String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_launcher");
        String first = PathJoin(root.AsView(), u8"First");
        String second = PathJoin(root.AsView(), u8"Second");
        String gone = PathJoin(root.AsView(), u8"Gone");
        foundation::settings::Settings store{DefaultAllocator()};
        UniquePtr<ProjectManagerController> controller;
        RefPtr<gui::SceneRoot> scene = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        UniquePtr<app::ProjectManagerView> view;
        u32 saves = 0;
        Array<String> opened;
        Array<String> created;

        /// Adds `more` older projects after the three, which exist on disk when `real` is set;
        /// `configure` adds to the view's services (the Samples tab's client).
        explicit Launcher(int more = 0, bool real = false, const Function<void(app::ProjectManagerView::Services&)>& configure = {})
        {
            RegisterProjectRegistryTypes();
            (void)RemoveDirectoryRecursive(root.AsView());
            REQUIRE(CreateDirectories(root.AsView()));
            REQUIRE(EditorProject::Create(DefaultAllocator(), first.AsView(), u8"First").IsOk());
            REQUIRE(EditorProject::Create(DefaultAllocator(), second.AsView(), u8"Second").IsOk());
            for (int i = 0; i < more; ++i) // older projects: enough to scroll, after the three
            {
                const String dir = PathJoin(root.AsView(), Format(u8"More{}", i).AsView());
                if (real)
                    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), Format(u8"More{}", i).AsView()).IsOk());
                TouchRecentProject(store, dir.AsView(), Format(u8"More{}", i).AsView(), u8"0.1.0");
            }
            TouchRecentProject(store, gone.AsView(), u8"Gone", u8"0.1.0"); // no project there
            TouchRecentProject(store, second.AsView(), u8"Second", u8"0.1.0");
            TouchRecentProject(store, first.AsView(), u8"First", u8"0.1.0");
            controller = MakeUnique<ProjectManagerController>(DefaultAllocator(), store);
            app::ProjectManagerView::Services services;
            services.controller = controller.Get();
            Launcher* self = this;
            services.open = [self](StringView path) { self->opened.PushBack(String(path)); };
            services.create = [self](StringView directory, StringView) { self->created.PushBack(String(directory)); };
            services.storeChanged = [self]() { ++self->saves; };
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            if (configure)
                configure(services);
            scene->SetSize(Float2{1280.0f, 800.0f});
            view = MakeUnique<app::ProjectManagerView>(DefaultAllocator(), DefaultAllocator(), Move(services), LoadMarkup(app::kProjectManagerViewPath));
            REQUIRE(view->Content().Get() != nullptr);
            scene->AddChild(view->Content().Get());
        }
        ~Launcher()
        {
            view.Reset();
            (void)RemoveDirectoryRecursive(root.AsView());
        }
        /// Runs the GUI's next update, so work a click deferred now happens.
        void Drain() { scene->GetContext().Update(Duration::FromSeconds(1.0 / 60.0)); }

        /// A small RGBA picture written as `name` under the scratch root.
        String WritePicture(StringView name)
        {
            foundation::image::Image picture(16, 9, foundation::image::PixelFormat::RGBA8);
            const String path = PathJoin(root.AsView(), name);
            REQUIRE(foundation::image::io::SaveImage(picture, path.AsView(), foundation::image::io::ImageFileFormat::PNG).IsOk());
            return path;
        }
    };
}

TEST_CASE("launcher: the projects as a list or a grid, the choice remembered; a missing one in its own section, with no Open")
{
    Launcher l;
    CHECK_FALSE(l.view->IsGridView());
    CHECK_FALSE(l.view->IsWelcome());
    CHECK(l.view->ShownCount() == 3u);
    CHECK(l.view->ThumbnailCount() == 0u); // no pictures yet: initials
    gui::Node* list = FindId<gui::Node>(*l.view->Content(), u8"manager-projects");
    gui::Node* missing = FindId<gui::Node>(*l.view->Content(), u8"manager-missing-list");
    REQUIRE(list != nullptr);
    REQUIRE(missing != nullptr);
    // First, then Second (most recent first); Gone is missing, so it is listed apart, with Locate...
    // and no Open.
    REQUIRE(list->ChildCount() == 2u);
    REQUIRE(missing->ChildCount() == 1u);
    CHECK(FindId<gui::UIWidget>(*l.view->Content(), u8"manager-missing")->IsDisplayed());
    CHECK(FindId<gui::Label>(*l.view->Content(), u8"manager-count")->GetText() == StringView(u8"2 projects"));
    CHECK(ButtonText(*list->GetChildAt(0), u8"Open") != nullptr);
    CHECK(ButtonText(*missing->GetChildAt(0), u8"Open") == nullptr);
    CHECK(ButtonText(*missing->GetChildAt(0), u8"Locate...") != nullptr);
    CHECK(Cast<gui::UIWidget>(missing->GetChildAt(0))->HasClass(u8"missing"));
    CHECK(ButtonText(*list->GetChildAt(0), u8"Open")->HandleActivate());
    REQUIRE(l.opened.Size() == 1u);
    CHECK(l.opened[0] == l.first);

    CHECK(FindId<gui::Button>(*l.view->Content(), u8"manager-grid")->HandleActivate());
    l.Drain();
    CHECK(l.view->IsGridView());
    CHECK(l.view->ShownCount() == 3u);
    CHECK(Cast<gui::UIWidget>(list)->HasClass(u8"grid"));
    CHECK(Cast<gui::UIWidget>(list->GetChildAt(0))->HasClass(u8"manager-tile"));
    CHECK(l.saves == 1u);
    CHECK(l.controller->Entries().gridView); // kept with the recent projects
    l.view->SetGridView(true);
    CHECK(l.saves == 1u); // no change, nothing to save
}

namespace
{
    // The names of the projects listed, in their order.
    Array<String> ListedNames(Launcher& l)
    {
        Array<String> names;
        gui::Node* list = FindId<gui::Node>(*l.view->Content(), u8"manager-projects");
        for (usize i = 0; i < list->ChildCount(); ++i)
        {
            Array<gui::Node*> stack;
            stack.PushBack(list->GetChildAt(i));
            while (!stack.IsEmpty())
            {
                gui::Node* n = stack[stack.Size() - 1];
                stack.PopBack();
                if (auto* label = Cast<gui::Label>(n); label != nullptr && label->HasClass(u8"manager-name"))
                {
                    names.PushBack(String(label->GetText()));
                    break;
                }
                for (usize c = n->ChildCount(); c-- > 0;)
                    stack.PushBack(n->GetChildAt(c));
            }
        }
        return names;
    }
}

TEST_CASE("launcher: a pinned project comes first whatever the sort; the sort and the search arrange what was probed")
{
    Launcher l;
    Array<String> names = ListedNames(l);
    REQUIRE(names.Size() == 2u);
    CHECK(names[0] == StringView(u8"First"));

    // Pinning Second puts it first, and is saved.
    l.view->SetFavourite(l.second.AsView(), true);
    l.Drain();
    CHECK(l.saves == 1u);
    names = ListedNames(l);
    CHECK(names[0] == StringView(u8"Second"));
    CHECK(l.view->FocusedPath() == l.second.AsView()); // the ring went with it
    l.view->SetFavourite(l.second.AsView(), false);
    l.Drain();
    CHECK(ListedNames(l)[0] == StringView(u8"First"));

    // By name: First before Second still, then the opposite order of recency shows it sorts.
    TouchRecentProject(l.store, l.second.AsView(), u8"Second", u8"0.1.0");
    l.view->Rebuild();
    CHECK(ListedNames(l)[0] == StringView(u8"Second"));
    l.view->SetSortByName(true);
    l.Drain();
    CHECK(l.controller->SortByName());
    CHECK(ListedNames(l)[0] == StringView(u8"First"));

    // The search matches the name or the folder, any case; the count says how many of how many.
    l.view->SetQuery(u8"sec");
    CHECK(ListedNames(l).Size() == 1u);
    CHECK(FindId<gui::Label>(*l.view->Content(), u8"manager-count")->GetText() == StringView(u8"1 of 2"));
    CHECK_FALSE(FindId<gui::UIWidget>(*l.view->Content(), u8"manager-missing")->IsDisplayed()); // Gone does not match
    l.view->SetQuery(u8"nothing like it");
    CHECK(ListedNames(l).IsEmpty());
    CHECK(FindId<gui::UIWidget>(*l.view->Content(), u8"manager-no-match")->IsDisplayed());
    l.view->SetQuery(u8"");
    CHECK(ListedNames(l).Size() == 2u);
    CHECK_FALSE(FindId<gui::UIWidget>(*l.view->Content(), u8"manager-no-match")->IsDisplayed());
}

TEST_CASE("launcher: Up and Down move the ring, Enter opens its project, Del takes it off the list; Ctrl+N is New Project")
{
    Launcher l;
    l.view->Shown();
    gui::EventDispatcher& d = *l.scene->GetEventDispatcher();
    CHECK(l.view->FocusedPath() == l.first.AsView());
    CHECK(d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Down)));
    CHECK(l.view->FocusedPath() == l.second.AsView());
    CHECK(d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Down)));
    CHECK(l.view->FocusedPath() == l.gone.AsView()); // the missing come after the rest
    CHECK(d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Down)));
    CHECK(l.view->FocusedPath() == l.gone.AsView()); // the end holds
    CHECK(d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Up)));
    CHECK(d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Return)));
    REQUIRE(l.opened.Size() == 1u);
    CHECK(l.opened[0] == l.second);

    CHECK(d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Delete)));
    l.Drain();
    CHECK(l.saves == 1u);
    CHECK(l.view->ShownCount() == 2u);
    CHECK(l.view->Status() == StringView(u8"Second was taken off the list; its files are untouched."));
    CHECK(FileExists(PathJoin(l.second.AsView(), u8"Project.xml").AsView())); // only the list changed

    // In the search, Del edits the text; Enter still opens the ring's project.
    l.view->HandleKey(static_cast<u32>(gui::KeyCode::F), gui::KeyModCtrl);
    auto* search = FindId<gui::TextField>(*l.view->Content(), u8"manager-search");
    REQUIRE(search != nullptr);
    CHECK(d.GetFocusNode() == search);
    CHECK_FALSE(l.view->HandleKey(static_cast<u32>(gui::KeyCode::Delete), 0));

    CHECK(l.view->CreateDialog() == nullptr);
    CHECK(l.view->HandleKey(static_cast<u32>(gui::KeyCode::N), gui::KeyModCtrl));
    CHECK(l.view->CreateDialog() != nullptr);
}

TEST_CASE("launcher: Locate... points a missing project at its new folder; Remove missing takes the rest off, files untouched")
{
    Launcher l;
    // A folder with no project is refused.
    CHECK_FALSE(l.view->Locate(l.gone.AsView(), l.root.AsView()));
    CHECK(l.view->ShownCount() == 3u);
    // Gone was moved: its project is at Moved now.
    const String moved = PathJoin(l.root.AsView(), u8"Moved");
    REQUIRE(EditorProject::Create(DefaultAllocator(), moved.AsView(), u8"Gone").IsOk());
    CHECK(l.view->Locate(l.gone.AsView(), moved.AsView()));
    l.Drain();
    CHECK(l.saves == 1u);
    CHECK_FALSE(FindId<gui::UIWidget>(*l.view->Content(), u8"manager-missing")->IsDisplayed());
    CHECK(ListedNames(l).Size() == 3u);

    // Two missing, then both off the list at once.
    TouchRecentProject(l.store, PathJoin(l.root.AsView(), u8"Lost1").AsView(), u8"Lost1", u8"0.1.0");
    TouchRecentProject(l.store, PathJoin(l.root.AsView(), u8"Lost2").AsView(), u8"Lost2", u8"0.1.0");
    l.view->Rebuild();
    CHECK(FindId<gui::Node>(*l.view->Content(), u8"manager-missing-list")->ChildCount() == 2u);
    CHECK(ButtonText(*l.view->Content(), u8"Remove missing from list")->HandleActivate());
    l.Drain();
    CHECK(l.saves == 2u);
    CHECK(l.view->ShownCount() == 3u);
    CHECK(l.view->Status() == StringView(u8"2 missing projects were taken off the list; no files were touched."));
}

TEST_CASE("launcher: with no projects, a welcome with New Project and Open Folder stands in for the lists")
{
    RegisterProjectRegistryTypes();
    foundation::settings::Settings store{DefaultAllocator()};
    ProjectManagerController controller(store);
    app::ProjectManagerView::Services services;
    services.controller = &controller;
    services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
    RefPtr<gui::SceneRoot> scene = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    app::ProjectManagerView view(DefaultAllocator(), Move(services), LoadMarkup(app::kProjectManagerViewPath));
    scene->AddChild(view.Content().Get());
    CHECK(view.IsWelcome());
    CHECK(FindId<gui::UIWidget>(*view.Content(), u8"manager-welcome")->IsDisplayed());
    CHECK_FALSE(FindId<gui::UIWidget>(*view.Content(), u8"manager-projects-page")->IsDisplayed());
    CHECK(FindId<gui::Button>(*view.Content(), u8"manager-welcome-new")->HandleActivate());
    CHECK(view.CreateDialog() != nullptr);

    // The Samples tab replaces the welcome while it is chosen.
    view.ShowSamples(true);
    CHECK(view.IsShowingSamples());
    CHECK_FALSE(FindId<gui::UIWidget>(*view.Content(), u8"manager-welcome")->IsDisplayed());
    CHECK(FindId<gui::UIWidget>(*view.Content(), u8"manager-samples-page")->IsDisplayed());
    CHECK(FindId<gui::Button>(*view.Content(), u8"manager-tab-samples")->IsChecked());
}

TEST_CASE("launcher: a folder dropped on the manager opens its project, or the status says it holds none")
{
    Launcher l;
    Array<String> dropped;
    dropped.PushBack(l.root);
    CHECK(l.view->OpenDropped(dropped.AsSpan()));
    CHECK(l.opened.IsEmpty());
    CHECK(l.view->Status() == StringView(u8"scratch_launcher holds no Project.xml, so it is not a project. New Project makes one."));
    // Laying the page out again shows the same words, read from the view's own copy.
    l.view->Rebuild();
    l.view->Rebuild();
    CHECK(FindId<gui::Label>(*l.view->Content(), u8"manager-status")->GetText() == l.view->Status());
    dropped[0] = l.second;
    CHECK(l.view->OpenDropped(dropped.AsSpan()));
    REQUIRE(l.opened.Size() == 1u);
    CHECK(l.opened[0] == l.second);
}

TEST_CASE("launcher: a project's picture is its thumbnail, an image chosen for it, or none")
{
    Launcher l;
    // A captured thumbnail in the project's editor folder shows.
    const String captured = l.WritePicture(u8"captured.png");
    REQUIRE(CreateDirectories(PathParent(ProjectThumbnailPath(l.first.AsView()).AsView())));
    foundation::image::Image picture;
    REQUIRE(foundation::image::io::LoadImage(captured.AsView(), picture).IsOk());
    REQUIRE(foundation::image::io::SaveImage(picture, ProjectThumbnailPath(l.first.AsView()).AsView(), foundation::image::io::ImageFileFormat::PNG).IsOk());
    l.view->Rebuild();
    CHECK(l.view->ThumbnailCount() == 1u);

    // An image chosen for the second project becomes its thumbnail.
    const String chosen = l.WritePicture(u8"chosen.png");
    CHECK(l.view->SetThumbnailFromImage(l.second.AsView(), chosen.AsView()));
    l.Drain();
    CHECK(FileExists(ProjectThumbnailPath(l.second.AsView()).AsView()));
    CHECK(l.view->ThumbnailCount() == 2u);
    CHECK_FALSE(l.view->SetThumbnailFromImage(l.second.AsView(), u8"/nowhere/at/all.png"));
    CHECK(l.view->Status() == StringView(u8"That image could not be read."));
}

TEST_CASE("launcher: each project's menu holds what applies to it; Remove from List takes it off and saves")
{
    Launcher l;
    Array<String> items = app::ProjectManagerView::MenuItemsFor(l.first.AsView());
    REQUIRE(items.Size() == 5u);
    CHECK(items[0] == StringView(u8"Open"));
    CHECK(items[1] == StringView(u8"Show in Folder"));
    CHECK(items[2] == StringView(u8"Copy Path"));
    CHECK(items[3] == StringView(u8"Choose Thumbnail Image..."));
    CHECK(items[4] == StringView(u8"Remove from List"));

    // With a thumbnail, it can be cleared.
    CHECK(l.view->SetThumbnailFromImage(l.first.AsView(), l.WritePicture(u8"p.png").AsView()));
    items = app::ProjectManagerView::MenuItemsFor(l.first.AsView());
    CHECK(items[4] == StringView(u8"Clear Thumbnail"));

    // A missing project offers only the items that still apply.
    items = app::ProjectManagerView::MenuItemsFor(l.gone.AsView());
    REQUIRE(items.Size() == 3u);
    CHECK(items[0] == StringView(u8"Show in Folder"));
    CHECK(items[2] == StringView(u8"Remove from List"));

    l.Drain();
    const u32 before = l.saves;
    l.view->Run(l.gone.AsView(), u8"Remove from List");
    l.Drain();
    CHECK(l.saves == before + 1u);
    CHECK(l.view->ShownCount() == 2u);
    CHECK(l.view->Status() == StringView(u8"Gone was taken off the list; its files are untouched."));
}

TEST_CASE("launcher: New Project starts where the last project went, checks as it is typed, and makes a folder of its name")
{
    Launcher l;
    l.controller->SetLastLocation(l.root.AsView());
    l.view->ShowCreateDialog();
    gui::Window* dialog = l.view->CreateDialog();
    REQUIRE(dialog != nullptr);
    auto* name = FindId<gui::TextField>(*dialog, u8"new-project-name");
    auto* parent = FindId<gui::TextField>(*dialog, u8"new-project-parent");
    auto* ownFolder = FindId<gui::CheckBox>(*dialog, u8"new-project-own-folder");
    auto* line = FindId<gui::UIWidget>(*dialog, u8"new-project-check");
    auto* where = FindId<gui::Label>(*dialog, u8"new-project-where");
    auto* create = FindId<gui::Button>(*dialog, u8"new-project-create");
    REQUIRE(name != nullptr);
    REQUIRE(parent != nullptr);
    REQUIRE(ownFolder != nullptr);
    REQUIRE(line != nullptr);
    REQUIRE(create != nullptr);
    CHECK(parent->GetText() == l.root.AsView()); // the last location
    CHECK(ownFolder->IsChecked());               // a folder of its name, unless turned off
    CHECK_FALSE(create->IsEnabled());
    CHECK(line->HasClass(u8"error"));
    CHECK(where->GetText() == StringView(u8"Give the project a name."));
    // The window is as tall as what it holds: it ends just under its buttons.
    (void)l.scene->UpdateLayout();
    const gui::Rect window = dialog->GetScreenBounds();
    const gui::Rect button = create->GetScreenBounds();
    CHECK(button.y + button.height <= window.y + window.height + 0.5f);
    CHECK(window.y + window.height - (button.y + button.height) < 40.0f);

    gui::EventDispatcher& d = *l.scene->GetEventDispatcher();
    name->RequestFocus();
    d.InjectText(u8"Harbour");
    CHECK(create->IsEnabled());
    CHECK_FALSE(line->HasClass(u8"error"));
    CHECK_FALSE(line->HasClass(u8"warning"));

    // Without a folder of its name, the location itself must be empty: the scratch root is not.
    CHECK(ownFolder->HandleActivate());
    CHECK_FALSE(ownFolder->IsChecked());
    CHECK_FALSE(create->IsEnabled());
    CHECK(line->HasClass(u8"error"));
    CHECK(ownFolder->HandleActivate());
    CHECK(create->IsEnabled());

    const u32 before = l.saves;
    CHECK(create->HandleActivate());
    CHECK(l.view->CreateDialog() == nullptr);
    REQUIRE(l.created.Size() == 1u);
    CHECK(l.created[0] == PathJoin(l.root.AsView(), u8"Harbour"));
    CHECK(l.controller->LastLocation() == l.root.AsView()); // remembered for the next one
    CHECK(l.saves == before + 1u);
}

namespace
{
    // With the pointer held still over the first card and frames passing between notches (long
    // enough for a tooltip), each notch keeps scrolling the grid, whatever card comes under the
    // pointer.
    void WheelOverCards(Launcher& l)
    {
        // The editor's sheet, as the window has it, so the grid wraps and scrolls as it does there.
        EditorSheet sheet;
        l.view->SetGridView(true);
        l.Drain();
        sheet.styles.ApplyTree(*l.scene);
        (void)l.scene->UpdateLayout();
        gui::Node* list = FindId<gui::Node>(*l.view->Content(), u8"manager-projects");
        REQUIRE(list != nullptr);
        gui::Node* scroller = FindId<gui::Node>(*l.view->Content(), u8"manager-scroll");
        REQUIRE(scroller != nullptr);
        REQUIRE(scroller->IsScrollContainer());
        const gui::Rect card = list->GetChildAt(0)->GetScreenBounds();
        const Float2 at{card.x + card.width * 0.5f, card.y + card.height * 0.4f};
        gui::EventDispatcher& d = *l.scene->GetEventDispatcher();
        d.InjectMouseMove(at);
        f32 last = scroller->GetScrollOffset().y;
        for (int step = 0; step < 8 && last < scroller->GetScrollRange().y; ++step)
        {
            CAPTURE(step);
            (void)d.InjectMouseWheel(at, Float2{0.0f, -1.0f});
            for (int frame = 0; frame < 30; ++frame)
            {
                l.Drain();
                (void)l.scene->UpdateLayout();
            }
            const f32 now = scroller->GetScrollOffset().y;
            CHECK(now > last);
            last = now;
        }
    }
}

TEST_CASE("launcher: the wheel over the grid's cards scrolls notch after notch, the pointer held still, with missing projects listed")
{
    Launcher l(18);
    WheelOverCards(l);
}

TEST_CASE("launcher: the wheel over the grid's cards scrolls notch after notch, over projects that are there")
{
    Launcher l(18, true);
    WheelOverCards(l);
}

TEST_CASE("launcher: under the editor's sheet a card's picture fills it edge to edge inside its border, at 16:9")
{
    Launcher l;
    EditorSheet sheet;
    l.view->SetGridView(true);
    l.Drain();
    sheet.styles.ApplyTree(*l.scene);
    (void)l.scene->UpdateLayout();
    gui::Node* list = FindId<gui::Node>(*l.view->Content(), u8"manager-projects");
    REQUIRE(list != nullptr);
    gui::Node& card = *list->GetChildAt(0);
    gui::Node* button = card.GetChildAt(0);
    REQUIRE(Cast<gui::Button>(button) != nullptr);
    REQUIRE(button->ChildCount() > 0);
    gui::Node* picture = nullptr;
    for (usize i = 0; i < button->ChildCount() && picture == nullptr; ++i)
        if (auto* widget = Cast<gui::UIWidget>(button->GetChildAt(i)); widget != nullptr && widget->HasClass(u8"manager-tile-picture"))
            picture = button->GetChildAt(i);
    REQUIRE(picture != nullptr);
    const gui::Rect b = button->GetScreenBounds();
    const gui::Rect p = picture->GetScreenBounds();
    const gui::Rect c = card.GetScreenBounds();
    const f32 left = p.x - b.x, right = b.x + b.width - (p.x + p.width);
    const f32 top = p.y - b.y, bottom = b.y + b.height - (p.y + p.height);
    CHECK(left == doctest::Approx(top));
    CHECK(right == doctest::Approx(bottom));
    CHECK(left == doctest::Approx(right));
    // Inside the card's 1px border (var(--border), so the tokens reached the sheet), edge to edge.
    CHECK(b.x == doctest::Approx(c.x + 1.0f));
    CHECK(b.x + b.width == doctest::Approx(c.x + c.width - 1.0f));
    CHECK(p.width / p.height == doctest::Approx(16.0f / 9.0f).epsilon(0.01)); // whatever its column's width
}

TEST_CASE("launcher: under the editor's sheet a row stretches across the window, less the page's margins")
{
    Launcher l;
    EditorSheet sheet;
    sheet.styles.ApplyTree(*l.scene);
    (void)l.scene->UpdateLayout();
    gui::Node* list = FindId<gui::Node>(*l.view->Content(), u8"manager-projects");
    REQUIRE(list != nullptr);
    const gui::Rect row = list->GetChildAt(0)->GetScreenBounds();
    CHECK(row.x == doctest::Approx(40.0f));
    CHECK(row.width > 1100.0f); // 1280 less 40 each side and a scroll bar at most
    const gui::Rect missing = FindId<gui::Node>(*l.view->Content(), u8"manager-missing-list")->GetChildAt(0)->GetScreenBounds();
    CHECK(missing.width == doctest::Approx(row.width));
}

TEST_CASE("launcher: the Samples tab lists the release's samples, and one downloaded joins the projects")
{
    UniquePtr<foundation::http::IHttpClient> client = foundation::http::CreateNativeHttpClient(DefaultAllocator(), nullptr);
    LoopbackFiles server;
    Launcher l(0, false, [&](app::ProjectManagerView::Services& services)
               {
                   services.http = client.Get();
                   services.releaseFile = [&server](StringView file) { return server.Url(Format(u8"/{}", file).AsView()); };
               });
    // The release: its samples.json, one sample's zip (a project's manifest at its root) and picture.
    Result<Array<byte>> zip = ReadFile(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data/Tests/Zip/sample.zip").AsView());
    REQUIRE(zip.HasValue());
    Result<Array<byte>> picture = ReadFile(l.WritePicture(u8"picture.png").AsView());
    REQUIRE(picture.HasValue());
    server.Serve(u8"/samples.json",
                 Format(u8R"({{"version": "0.1.0", "samples": [{{"folder": "NativeSample", "name": "NativeSample", "description": "A game with native code.",
                         "archive": "sample-NativeSample.zip", "size": {}, "sha256": "{}", "picture": "sample-NativeSample.png"}}]}})",
                        zip.Value().Size(), Sha256::Of(zip.Value().AsSpan()).ToHex())
                     .AsView());
    server.Serve(u8"/sample-NativeSample.zip", Move(zip.Value()));
    server.Serve(u8"/sample-NativeSample.png", Move(picture.Value()));
    const auto run = [&](const Function<bool()>& until)
    {
        for (u32 i = 0; i < 5000 && !until(); ++i)
        {
            server.Pump();
            l.view->Tick();
            l.Drain();
            SleepMilliseconds(1);
        }
    };

    l.view->ShowSamples(true);
    run([&]() { return l.view->SampleCatalogState() == app::ProjectManagerView::CatalogState::Loaded; });
    REQUIRE(l.view->SampleCatalogState() == app::ProjectManagerView::CatalogState::Loaded);
    REQUIRE(l.view->Samples().samples.Size() == 1u);
    gui::Node* grid = FindId<gui::Node>(*l.view->Content(), u8"manager-samples");
    REQUIRE(grid != nullptr);
    REQUIRE(grid->ChildCount() == 1u);
    gui::Button* download = ButtonText(*grid, u8"Download...");
    REQUIRE(download != nullptr);
    CHECK_FALSE(l.view->InProjects(l.view->Samples().samples[0]));

    // Download... asks where; the location starts where the last project went.
    l.controller->SetLastLocation(l.root.AsView());
    CHECK(download->HandleActivate());
    gui::Window* dialog = l.view->DownloadDialog();
    REQUIRE(dialog != nullptr);
    CHECK(FindId<gui::Label>(*dialog, u8"download-sample-where")->GetText().StartsWith(u8"Downloads into"));
    // It opens on choosing where: the download's and the end's panels and buttons are hidden.
    const auto shown = [&](const char8_t* id) { return FindId<gui::UIWidget>(*dialog, id)->IsDisplayed(); };
    CHECK(shown(u8"download-sample-choose"));
    CHECK(shown(u8"download-sample-go"));
    CHECK(shown(u8"download-sample-cancel"));
    CHECK_FALSE(shown(u8"download-sample-progress"));
    CHECK_FALSE(shown(u8"download-sample-result"));
    CHECK_FALSE(shown(u8"download-sample-close"));
    CHECK_FALSE(shown(u8"download-sample-retry"));
    CHECK_FALSE(shown(u8"download-sample-open"));
    gui::Button* go = FindId<gui::Button>(*dialog, u8"download-sample-go");
    REQUIRE(go != nullptr);
    CHECK(go->IsEnabled());
    CHECK(go->HandleActivate());
    // The dialog stays, showing the download, then how it ended.
    REQUIRE(l.view->DownloadDialog() != nullptr);
    CHECK(FindId<gui::UIWidget>(*dialog, u8"download-sample-progress")->IsDisplayed());
    CHECK_FALSE(FindId<gui::UIWidget>(*dialog, u8"download-sample-choose")->IsDisplayed());
    l.Drain();
    run([&]() { return l.view->DownloadOf(u8"NativeSample") == nullptr; });
    REQUIRE(l.view->DownloadDialog() != nullptr);
    CHECK(FindId<gui::UIWidget>(*dialog, u8"download-sample-result")->IsDisplayed());
    CHECK(FindId<gui::Label>(*dialog, u8"download-sample-result-text")->GetText().ContainsIgnoreCase(u8"is in your projects"));
    gui::Button* openIt = FindId<gui::Button>(*dialog, u8"download-sample-open");
    REQUIRE(openIt != nullptr);
    CHECK(openIt->IsDisplayed());
    CHECK(openIt->HandleActivate()); // opens the new project
    CHECK(l.view->DownloadDialog() == nullptr);
    REQUIRE(l.opened.Size() == 1u);
    CHECK(l.opened[0] == PathJoin(l.root.AsView(), u8"NativeSample"));

    // It is one of the projects now, and its card says so.
    const String target = PathJoin(l.root.AsView(), u8"NativeSample");
    INFO(reinterpret_cast<const char*>(String(l.view->Status()).CStr()));
    CHECK(FileExists(PathJoin(target.AsView(), u8"Project.xml").AsView()));
    bool listed = false;
    for (const RecentProjectEntry& e : l.controller->Entries().entries)
        listed = listed || e.path == target;
    CHECK(listed);
    l.Drain();
    CHECK(l.view->InProjects(l.view->Samples().samples[0]));
    gui::Node* card = FindId<gui::Node>(*l.view->Content(), u8"manager-samples")->GetChildAt(0);
    bool saysSo = false;
    Array<gui::Node*> stack;
    stack.PushBack(card);
    while (!stack.IsEmpty())
    {
        gui::Node* n = stack[stack.Size() - 1];
        stack.PopBack();
        if (auto* label = Cast<gui::Label>(n))
            saysSo = saysSo || label->GetText() == StringView(u8"In your projects");
        for (usize i = 0; i < n->ChildCount(); ++i)
            stack.PushBack(n->GetChildAt(i));
    }
    CHECK(saysSo);
    CHECK(l.view->Status().ContainsIgnoreCase(u8"in your projects"));
}

TEST_CASE("launcher: the Samples tab says when the release has no samples, and when GitHub is not reached")
{
    UniquePtr<foundation::http::IHttpClient> client = foundation::http::CreateNativeHttpClient(DefaultAllocator(), nullptr);
    LoopbackFiles server; // serves nothing: samples.json is a 404
    Launcher l(0, false, [&](app::ProjectManagerView::Services& services)
               {
                   services.http = client.Get();
                   services.releaseFile = [&server](StringView file) { return server.Url(Format(u8"/{}", file).AsView()); };
               });
    l.view->ShowSamples(true);
    for (u32 i = 0; i < 5000 && l.view->SampleCatalogState() == app::ProjectManagerView::CatalogState::Fetching; ++i)
    {
        server.Pump();
        l.view->Tick();
        l.Drain();
        SleepMilliseconds(1);
    }
    CHECK(l.view->SampleCatalogState() == app::ProjectManagerView::CatalogState::NotPublished);
    CHECK(FindId<gui::UIWidget>(*l.view->Content(), u8"manager-samples-state")->IsDisplayed());
    CHECK(FindId<gui::Label>(*l.view->Content(), u8"manager-samples-state-title")->GetText() == StringView(u8"No samples for this version yet"));
    CHECK_FALSE(FindId<gui::UIWidget>(*l.view->Content(), u8"manager-samples-retry")->IsDisplayed());

    // With no client at all, it cannot be reached, and Retry is offered.
    Launcher offline;
    offline.view->ShowSamples(true);
    CHECK(offline.view->SampleCatalogState() == app::ProjectManagerView::CatalogState::Failed);
    CHECK(FindId<gui::UIWidget>(*offline.view->Content(), u8"manager-samples-retry")->IsDisplayed());
}

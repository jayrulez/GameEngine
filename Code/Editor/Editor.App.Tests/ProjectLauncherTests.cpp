// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the project manager over its markup (Data/Editor/ProjectManager.gml): the
// recent projects as a list or a grid (remembered), each by its picture (the thumbnail in its
// editor folder, an image chosen for it, or its initial), a missing one with no Open, each
// project's menu and what it runs, and New Project's dialog.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>
#include <string>

import foundation.core;
import foundation.settings;
import foundation.image;
import foundation.image.io;
import foundation.gui;
import editor.core;
import editor.app;

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

        /// `more` older projects after the three, there (`real`) or no longer there.
        explicit Launcher(int more = 0, bool real = false)
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
            scene->SetSize(Float2{1280.0f, 800.0f});
            view = MakeUnique<app::ProjectManagerView>(DefaultAllocator(), DefaultAllocator(), Move(services), LoadMarkup(app::ProjectManagerView::kPath));
            REQUIRE(view->Content().Get() != nullptr);
            scene->AddChild(view->Content().Get());
        }
        ~Launcher()
        {
            view.Reset();
            (void)RemoveDirectoryRecursive(root.AsView());
        }
        /// The GUI's next drain: what a click asked to happen later happens.
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

TEST_CASE("launcher: the projects as a list or a grid, the choice remembered; a missing one has no Open")
{
    Launcher l;
    CHECK_FALSE(l.view->IsGridView());
    CHECK(l.view->ShownCount() == 3u);
    CHECK(l.view->ThumbnailCount() == 0u); // no pictures yet: initials
    gui::Node* list = FindId<gui::Node>(*l.view->Content(), u8"manager-projects");
    REQUIRE(list != nullptr);
    REQUIRE(list->ChildCount() == 3u);
    // First, then Second, then Gone (most recent first); Gone missing: no Open.
    CHECK(ButtonText(*list->GetChildAt(0), u8"Open") != nullptr);
    CHECK(ButtonText(*list->GetChildAt(2), u8"Open") == nullptr);
    CHECK(Cast<gui::UIWidget>(list->GetChildAt(2))->HasClass(u8"missing"));
    CHECK(ButtonText(*list->GetChildAt(0), u8"Open")->HandleActivate());
    REQUIRE(l.opened.Size() == 1u);
    CHECK(l.opened[0] == l.first);

    CHECK(FindId<gui::Button>(*l.view->Content(), u8"manager-grid")->HandleActivate());
    l.Drain();
    CHECK(l.view->IsGridView());
    CHECK(l.view->ShownCount() == 3u);
    CHECK(Cast<gui::UIWidget>(list)->HasClass(u8"grid"));
    CHECK(l.saves == 1u);
    CHECK(l.controller->Entries().gridView); // kept with the recent projects
    l.view->SetGridView(true);
    CHECK(l.saves == 1u); // no change, nothing to save
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

    // A missing project: only what still makes sense.
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
}

TEST_CASE("launcher: New Project waits for a name and a folder, then makes it in a folder of its name")
{
    Launcher l;
    l.view->ShowCreateDialog();
    gui::Window* dialog = l.view->CreateDialog();
    REQUIRE(dialog != nullptr);
    auto* name = FindId<gui::TextField>(*dialog, u8"new-project-name");
    auto* parent = FindId<gui::TextField>(*dialog, u8"new-project-parent");
    auto* create = FindId<gui::Button>(*dialog, u8"new-project-create");
    REQUIRE(name != nullptr);
    REQUIRE(parent != nullptr);
    REQUIRE(create != nullptr);
    CHECK_FALSE(create->IsEnabled());
    gui::EventDispatcher& d = *l.scene->GetEventDispatcher();
    name->RequestFocus();
    d.InjectText(u8"Harbour");
    CHECK_FALSE(create->IsEnabled()); // no folder yet
    parent->RequestFocus();
    d.InjectText(l.root.AsView());
    CHECK(create->IsEnabled());
    CHECK(FindId<gui::Label>(*dialog, u8"new-project-where")->GetText() == PathJoin(l.root.AsView(), u8"Harbour").AsView());
    CHECK(create->HandleActivate());
    CHECK(l.view->CreateDialog() == nullptr);
    REQUIRE(l.created.Size() == 1u);
    CHECK(l.created[0] == PathJoin(l.root.AsView(), u8"Harbour"));
}

namespace
{
    // Over the first card, held still, frames passing between the notches (a tooltip's time): each
    // notch scrolls the grid on, over whatever card comes under the pointer.
    void WheelOverCards(Launcher& l)
    {
        // The editor's own sheet, as the window has it: the grid wraps and scrolls as it shows.
        String sheet;
        for (const char8_t* file : {u8"Editor/editor.css", u8"Editor/density-default.css"})
        {
            Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
            REQUIRE(bytes.HasValue());
            sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
            sheet += u8"\n";
        }
        gui::StyleManager styles;
        styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
        l.view->SetGridView(true);
        l.Drain();
        styles.ApplyTree(*l.scene);
        (void)l.scene->UpdateLayout();
        gui::Node* list = FindId<gui::Node>(*l.view->Content(), u8"manager-projects");
        REQUIRE(list != nullptr);
        gui::Node* scroller = list->GetParent();
        while (scroller != nullptr && Cast<gui::ScrollView>(scroller) == nullptr)
            scroller = scroller->GetParent();
        REQUIRE(scroller != nullptr);
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

TEST_CASE("launcher: the wheel over the grid's cards scrolls notch after notch, the pointer held still, over a missing project's card too")
{
    Launcher l(18);
    WheelOverCards(l);
}

TEST_CASE("launcher: the wheel over the grid's cards scrolls notch after notch, over projects that are there")
{
    Launcher l(18, true);
    WheelOverCards(l);
}

TEST_CASE("launcher: under the editor's sheet a card's picture button rings it evenly, inside the card")
{
    Launcher l;
    String sheet;
    for (const char8_t* file : {u8"Editor/editor.css", u8"Editor/density-default.css"})
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
        REQUIRE(bytes.HasValue());
        sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        sheet += u8"\n";
    }
    gui::StyleManager styles;
    styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
    l.view->SetGridView(true);
    l.Drain();
    styles.ApplyTree(*l.scene);
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
    CHECK(b.x >= c.x); // inside the card
    CHECK(b.x + b.width <= c.x + c.width + 0.5f);
}

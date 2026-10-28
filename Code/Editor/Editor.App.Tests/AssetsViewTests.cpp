// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Assets panel over its markup (Data/Editor/Assets.xml) and a scratch project: the group tree
// and the breadcrumb following where it is; a group's subgroups then its assets, each by name, what
// it is and its star; entering a group and opening an asset; the search over every group saying
// where each match is, and the type filter, each with its state when nothing is left and the way
// back; tiles or a list kept with the project; building again when the database's shape moves;
// Reveal; and a drag of the selection carrying its assets.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>
#include <initializer_list>

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import pipeline.core; // AssetCreator

using namespace foundation::core;
using editor::app::AssetCell;
using editor::app::AssetDrag;
using editor::app::AssetsView;
namespace gui = foundation::gui;
namespace content = foundation::content;

namespace
{
    class BrowserMesh : public ISerializable
    {
        RTTI_OBJECT(BrowserMesh, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };
    class BrowserSound : public ISerializable
    {
        RTTI_OBJECT(BrowserSound, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    RefPtr<gui::Node> LoadAssets()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(AssetsView::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    // Content (Crate, Models (Town (Barrel, Bell)), Sounds).
    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        foundation::settings::Settings store{DefaultAllocator()};
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        UniquePtr<editor::app::AssetOperations> ops;
        UniquePtr<AssetsView> view;
        Array<String> created; // what New ran, by its label
        Guid pictured;                     // the asset whose thumbnail is ready (none: nil)
        RefPtr<gui::Drawable> picture;     // what it shows
        content::Group* top = nullptr;
        content::Group* models = nullptr;
        content::Group* town = nullptr;
        content::Instance* crate = nullptr;
        content::Instance* barrel = nullptr;
        Array<Guid> opened;

        Bed()
        {
            std::error_code ec;
            std::filesystem::remove_all("assets_view_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"assets_view_project", u8"Browse").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"assets_view_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(BrowserMesh::StaticType());
            GlobalTypeRegistry().Register(BrowserSound::StaticType());
            top = project->SourceDb().RootGroup();
            crate = top->CreateInstance(u8"Crate", BrowserMesh::StaticType());
            models = top->CreateGroup(u8"Models");
            town = models->CreateGroup(u8"Town");
            barrel = town->CreateInstance(u8"Barrel", BrowserMesh::StaticType());
            REQUIRE(town->CreateInstance(u8"Bell", BrowserSound::StaticType()) != nullptr);
            REQUIRE(top->CreateGroup(u8"Sounds") != nullptr);
            REQUIRE(crate != nullptr);
            REQUIRE(barrel != nullptr);
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            root->SetSize(Float2{900.0f, 600.0f});
            AssetsView::Services services;
            services.context = &context;
            Bed* self = this;
            services.open = [self](content::Instance& instance) { self->opened.PushBack(instance.Id()); };
            editor::app::AssetOperations::Services operations;
            operations.context = &context; // no cook, no later: at once
            ops = MakeUnique<editor::app::AssetOperations>(DefaultAllocator(), Move(operations));
            services.operations = ops.Get();
            services.create = [self](const pipeline::AssetCreator& creator, content::Group*) { self->created.PushBack(creator.label); };
            services.picture = [self](const Guid& id) { return id == self->pictured ? self->picture : RefPtr<gui::Drawable>{}; };
            view = MakeUnique<AssetsView>(DefaultAllocator(), DefaultAllocator(), Move(services), LoadAssets());
            RefPtr<gui::Node> panel = view->Content();
            REQUIRE(panel);
            Cast<gui::UIWidget>(panel.Get())->EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Width = gui::LayoutLength::Px(900.0f);
                s.Height = gui::LayoutLength::Px(600.0f);
            });
            root->AddChild(panel.Get());
            // No sheet here: the list its own room (the editor's sheet grows it).
            view->List()->EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Width = gui::LayoutLength::Px(600.0f);
                s.Height = gui::LayoutLength::Px(400.0f);
            });
            view->Rebuild();
            (void)root->UpdateLayout();
        }
        ~Bed()
        {
            view.Reset();
            ops.Reset();
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            std::error_code ec;
            std::filesystem::remove_all("assets_view_project", ec);
        }
        [[nodiscard]] String Rows() const
        {
            String out;
            for (usize i = 0; i < view->RowCount(); ++i)
            {
                if (!out.IsEmpty())
                    out += u8" ";
                if (view->RowGroup(i) != nullptr)
                    out += u8"/";
                out += view->RowName(i);
            }
            return out;
        }
        [[nodiscard]] i32 RowOf(StringView name) const
        {
            for (usize i = 0; i < view->RowCount(); ++i)
                if (view->RowName(i) == name)
                    return static_cast<i32>(i);
            return -1;
        }
        // The cell a row is drawn by, once laid out.
        AssetCell* Cell(i32 row)
        {
            (void)root->UpdateLayout();
            Array<gui::Node*> pending;
            pending.PushBack(view->List());
            while (!pending.IsEmpty())
            {
                gui::Node* node = pending.Back();
                pending.PopBack();
                if (auto* holder = Cast<gui::ItemRow>(node); holder != nullptr && holder->GetItemIndex() == row)
                    return Cast<AssetCell>(holder->GetContent());
                for (usize i = 0; i < node->ChildCount(); ++i)
                    pending.PushBack(node->GetChildAt(i));
            }
            return nullptr;
        }
    };
}

RTTI_DEFINE_OBJECT(BrowserMesh, "rtti::tests::assetsview")
RTTI_DEFINE_OBJECT(BrowserSound, "rtti::tests::assetsview")

TEST_CASE("assets: a group's subgroups then its assets; entering a group moves the breadcrumb and the tree; an asset opens")
{
    Bed bed;
    CHECK(bed.view->CurrentGroup() == bed.top);
    CHECK(bed.Rows() == String(u8"/Models /Sounds Crate"));
    REQUIRE(bed.view->Crumbs() != nullptr);
    CHECK(bed.view->Crumbs()->ItemCount() == 1u);
    CHECK(bed.view->Crumbs()->ItemAt(0).Text == String(u8"Content"));

    bed.view->OpenRow(bed.RowOf(u8"Models")); // as Enter or a double click
    CHECK(bed.view->CurrentGroup() == bed.models);
    bed.view->OpenRow(bed.RowOf(u8"Town"));
    CHECK(bed.Rows() == String(u8"Barrel Bell"));
    REQUIRE(bed.view->Crumbs()->ItemCount() == 3u);
    CHECK(bed.view->Crumbs()->ItemAt(2).Text == String(u8"Town"));
    gui::TreeView& tree = *bed.view->Tree();
    CHECK(tree.GetSelectedItem() >= 0);
    CHECK(tree.GetItemText(tree.GetSelectedItem()) == String(u8"Town"));

    bed.view->OpenRow(bed.RowOf(u8"Barrel"));
    REQUIRE(bed.opened.Size() == 1u);
    CHECK(bed.opened[0] == bed.barrel->Id());

    // The breadcrumb climbs back.
    REQUIRE(bed.view->Crumbs()->Navigate(0));
    CHECK(bed.view->CurrentGroup() == bed.top);
}

TEST_CASE("assets: a search looks in the group shown and under it, saying where each match is, kept as the group changes; nothing matching says so and clears")
{
    Bed bed;
    bed.view->SetTiles(false);
    bed.view->SetSearch(u8"BAR");
    CHECK(bed.Rows() == String(u8"Barrel"));
    AssetCell* cell = bed.Cell(0);
    REQUIRE(cell != nullptr);
    CHECK(cell->Kind() == StringView(u8"BrowserMesh in Content/Models/Town"));

    // A guid's start finds it too.
    utf8char chars[37];
    bed.crate->Id().ToChars(chars);
    bed.view->SetSearch(StringView(chars, 8));
    CHECK(bed.Rows() == String(u8"Crate"));

    // Another group chosen keeps the search, looking there and under it.
    bed.view->SetSearch(u8"e");
    CHECK(bed.Rows() == String(u8"Crate Barrel Bell"));
    bed.view->Navigate(*bed.models);
    CHECK(bed.view->SearchText() == StringView(u8"e"));
    CHECK(bed.Rows() == String(u8"Barrel Bell"));
    bed.view->SetSearch(u8"crate");
    CHECK(bed.view->StateTitle() == StringView(u8"Nothing matches crate in Content/Models"));
    bed.view->Navigate(*bed.top);
    CHECK(bed.Rows() == String(u8"Crate"));
    // Reveal shows an asset where it lives, so it ends the search.
    REQUIRE(bed.view->Reveal(bed.barrel->Id()));
    CHECK(bed.view->SearchText().IsEmpty());
    CHECK(bed.view->CurrentGroup() == bed.town);
    bed.view->Navigate(*bed.top);

    bed.view->SetSearch(u8"crate4");
    CHECK(bed.view->RowCount() == 0u);
    CHECK(bed.view->ShowsState());
    CHECK(bed.view->StateTitle() == StringView(u8"Nothing matches crate4"));
    bed.view->SetSearch(StringView{});
    CHECK_FALSE(bed.view->ShowsState());
    CHECK(bed.Rows() == String(u8"/Models /Sounds Crate"));
}

TEST_CASE("assets: the type filter keeps one type, from the types the project has; an empty group says so")
{
    Bed bed;
    bed.view->OpenRow(bed.RowOf(u8"Models"));
    bed.view->OpenRow(bed.RowOf(u8"Town"));
    bed.view->SetTypeFilter(u8"BrowserSound");
    CHECK(bed.Rows() == String(u8"Bell"));
    bed.view->Navigate(*bed.top);
    CHECK(bed.view->RowCount() == 0u); // no sounds, and no groups while filtering
    CHECK(bed.view->StateTitle() == StringView(u8"No BrowserSound here"));
    bed.view->SetTypeFilter(StringView{});
    CHECK(bed.Rows() == String(u8"/Models /Sounds Crate"));

    content::Group* sounds = bed.top->GetGroup(u8"Sounds");
    REQUIRE(sounds != nullptr);
    bed.view->Navigate(*sounds);
    CHECK(bed.view->StateTitle() == StringView(u8"Nothing in Content/Sounds yet"));
}

TEST_CASE("assets: tiles or a list, kept with the project; a favourite starred; a group by its name alone")
{
    Bed bed;
    CHECK(bed.view->ShowsTiles()); // tiles until the project says otherwise
    bed.view->SetTiles(false);
    const editor::app::EditorAssetBrowserSettings* saved = bed.store.Find<editor::app::EditorAssetBrowserSettings>();
    REQUIRE(saved != nullptr);
    CHECK_FALSE(saved->gridMode);
    bed.context.ToggleFavorite(bed.crate->Id());
    bed.view->Rebuild();
    AssetCell* cell = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(cell != nullptr);
    CHECK_FALSE(cell->IsTile());
    CHECK(cell->ShowsStar());
    AssetCell* group = bed.Cell(bed.RowOf(u8"Models"));
    REQUIRE(group != nullptr);
    CHECK(group->Kind().IsEmpty()); // its folder says it is a group: no words for it
}

TEST_CASE("assets: it builds again when the database's shape moves, however it moved; Reveal shows an asset where it lives")
{
    Bed bed;
    REQUIRE(bed.top->CreateInstance(u8"Anvil", BrowserMesh::StaticType()) != nullptr); // as an agent's write would
    bed.view->Refresh();
    CHECK(bed.Rows() == String(u8"/Models /Sounds Anvil Crate"));

    REQUIRE(bed.view->Reveal(bed.barrel->Id()));
    CHECK(bed.view->CurrentGroup() == bed.town);
    REQUIRE(bed.view->List()->GetSelectedItem() >= 0);
    CHECK(bed.view->RowName(static_cast<usize>(bed.view->List()->GetSelectedItem())) == StringView(u8"Barrel"));
    CHECK_FALSE(bed.view->Reveal(Guid{9u, 9u}));
}

TEST_CASE("assets: a drag of the selection carries its assets")
{
    Bed bed;
    bed.view->SetTiles(false);
    bed.view->OpenRow(bed.RowOf(u8"Models"));
    bed.view->OpenRow(bed.RowOf(u8"Town"));
    (void)bed.root->UpdateLayout();
    gui::ListView& list = *bed.view->List();
    list.SelectAll();
    AssetCell* first = bed.Cell(0);
    REQUIRE(first != nullptr);
    const gui::Rect at = first->GetScreenBounds();
    const Float2 press{at.x + 20.0f, at.y + at.height * 0.5f};
    gui::EventDispatcher& d = *bed.root->GetEventDispatcher();
    d.InjectMouseDown(press, gui::MouseButton::Left);
    d.InjectMouseMove(Float2{press.x + 12.0f, press.y + 30.0f});
    d.InjectMouseMove(Float2{press.x + 40.0f, press.y + 80.0f});
    Array<AssetDrag> carried;
    REQUIRE(AssetDrag::ReadAll(d.GetDragPayload(), carried));
    REQUIRE(carried.Size() == 2u);
    CHECK(carried[0].Name == String(u8"Barrel"));
    CHECK(carried[1].Name == String(u8"Bell"));
    CHECK(d.GetDragPayload().Label == String(u8"2 assets"));
    d.InjectMouseUp(Float2{press.x + 40.0f, press.y + 80.0f}, gui::MouseButton::Left);
}

TEST_CASE("assets: under the editor's sheet the toolbar keeps its height however full the body; the type menu opens below its button")
{
    Bed bed;
    String sheet;
    for (const char8_t* file : {u8"Editor/tokens.css", u8"Editor/editor.css", u8"Editor/density-default.css"}) // the tokens first: a parsed string follows no @import
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
        REQUIRE(bytes.HasValue());
        sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        sheet += u8"\n";
    }
    gui::StyleManager styles;
    styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
    // A short panel: the body wants more room than it has.
    Cast<gui::UIWidget>(bed.view->Content().Get())->EditLayoutStyle([](gui::LayoutStyle& s) { s.Height = gui::LayoutLength::Px(120.0f); });
    styles.ApplyTree(*bed.root);
    (void)bed.root->UpdateLayout();
    gui::Node* bar = bed.view->Crumbs()->GetParent();
    REQUIRE(bar != nullptr);
    CHECK(bar->GetSize().y == doctest::Approx(34.0f)); // the density's --bar, its search inside it with room above and below

    gui::Node& panel = *bed.view->Content();
    gui::Button* types = nullptr;
    Array<gui::Node*> pending;
    pending.PushBack(&panel);
    while (!pending.IsEmpty() && types == nullptr)
    {
        gui::Node* node = pending.Back();
        pending.PopBack();
        if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == StringView(u8"assets-types"))
            types = Cast<gui::Button>(node);
        for (usize i = 0; i < node->ChildCount(); ++i)
            pending.PushBack(node->GetChildAt(i));
    }
    REQUIRE(types != nullptr);
    CHECK(types->HandleActivate());
    (void)bed.root->UpdateLayout();
    REQUIRE(bed.view->TypeMenu() != nullptr);
    const gui::Rect button = types->GetScreenBounds();
    const gui::Rect menu = bed.view->TypeMenu()->GetScreenBounds();
    CHECK(menu.y >= button.y + button.height - 1.0f); // below the button, not at the window's corner
    CHECK(menu.x >= button.x - menu.width);
}

TEST_CASE("assets: under the editor's sheet a list row's kind is on its name's baseline and ends where every other row's does")
{
    Bed bed;
    String sheet;
    for (const char8_t* file : {u8"Editor/tokens.css", u8"Editor/editor.css", u8"Editor/density-default.css"}) // the tokens first: a parsed string follows no @import
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
        REQUIRE(bytes.HasValue());
        sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        sheet += u8"\n";
    }
    gui::StyleManager styles;
    styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
    bed.view->SetTiles(false);
    bed.context.ToggleFavorite(bed.barrel->Id()); // one row starred, one not
    bed.ops->ToggleAlwaysExport(bed.barrel->Id()); // and always exported, one not
    bed.view->OpenRow(bed.RowOf(u8"Models"));
    bed.view->OpenRow(bed.RowOf(u8"Town"));
    styles.ApplyTree(*bed.root);
    (void)bed.root->UpdateLayout();
    styles.ApplyTree(*bed.root);
    (void)bed.root->UpdateLayout();
    AssetCell* barrel = bed.Cell(bed.RowOf(u8"Barrel"));
    AssetCell* bell = bed.Cell(bed.RowOf(u8"Bell"));
    REQUIRE(barrel != nullptr);
    REQUIRE(bell != nullptr);
    CHECK(barrel->ShowsStar());
    CHECK_FALSE(bell->ShowsStar());
    const auto kindRight = [](AssetCell& cell)
    {
        const gui::Node* kind = cell.KindLabel();
        const gui::Rect r = kind->GetScreenBounds();
        return r.x + r.width;
    };
    // Every row's Always Export and star in the same columns, each an icon's width (a class of
    // another dialog's once stretched the row's).
    for (usize i = 0; i < barrel->ChildCount() && i < bell->ChildCount(); ++i)
    {
        const gui::Rect a = barrel->GetChildAt(i)->GetScreenBounds();
        const gui::Rect b = bell->GetChildAt(i)->GetScreenBounds();
        CHECK(a.x == doctest::Approx(b.x));
        CHECK(a.width == doctest::Approx(b.width));
    }
    CHECK(barrel->GetChildAt(barrel->ChildCount() - 2)->GetSize().x == doctest::Approx(14.0f)); // Always Export
    CHECK(barrel->ShowsAlwaysExport());
    CHECK_FALSE(bell->ShowsAlwaysExport());
    // Lit, a favourite's star is gold (its token defined: an undefined one left it white), and
    // Always Export green; greyed when not.
    const auto starOf = [](AssetCell& cell) { return Cast<gui::Image>(cell.GetChildAt(cell.ChildCount() - 1)); };
    REQUIRE(starOf(*barrel) != nullptr);
    const Color gold = starOf(*barrel)->GetImageColor();
    CHECK(gold.r > gold.b + 0.3f); // yellow: red and green well over blue
    CHECK(gold.g > gold.b + 0.2f);
    CHECK(starOf(*bell)->GetImageColor().r < gold.r);
    CHECK(kindRight(*barrel) == doctest::Approx(kindRight(*bell))); // the star's and the export badge's columns kept on the row without them
    // On one baseline: the name's and the kind's text baselines at the same height.
    const auto baseline = [](const gui::Node& label) { return label.GetScreenBounds().y + label.GetBaseline(); };
    CHECK(baseline(*barrel->NameLabel()) == doctest::Approx(baseline(*barrel->KindLabel())).epsilon(0.02));
}

namespace
{
    // The menu's item by its text, in it or its submenus' rows.
    gui::MenuItem* ItemNamed(gui::Node& menu, StringView text)
    {
        for (usize i = 0; i < menu.ChildCount(); ++i)
        {
            if (auto* item = Cast<gui::MenuItem>(menu.GetChildAt(i)); item != nullptr && item->GetText() == text)
                return item;
            if (gui::MenuItem* found = ItemNamed(*menu.GetChildAt(i), text))
                return found;
        }
        return nullptr;
    }
}

TEST_CASE("assets: an asset's menu, a group's, and the shown group's with New; Delete asks naming what goes, then deletes")
{
    Bed bed;
    gui::ListView& list = *bed.view->List();

    // An asset's row.
    const i32 crate = bed.RowOf(u8"Crate");
    list.SetSelectedItem(crate);
    bed.view->ShowMenuFor(crate);
    gui::Menu* menu = bed.view->OpenMenu();
    REQUIRE(menu != nullptr);
    for (const char8_t* verb : {u8"Open", u8"Rename", u8"Duplicate", u8"Copy GUID", u8"Copy Path", u8"Pin as Favourite", u8"Always Export", u8"Cook", u8"Rebuild", u8"Delete..."})
    {
        CAPTURE(reinterpret_cast<const char*>(verb));
        CHECK(ItemNamed(*menu, verb) != nullptr);
    }
    menu->Close();

    // A group's row, and the group shown (the empty space): New and New Group there.
    bed.view->ShowMenuFor(bed.RowOf(u8"Models"));
    menu = bed.view->OpenMenu();
    REQUIRE(menu != nullptr);
    CHECK(ItemNamed(*menu, u8"Delete Group...") != nullptr);
    CHECK(ItemNamed(*menu, u8"Always Export Its Contents") != nullptr);
    menu->Close();
    bed.view->ShowMenuFor(-1);
    menu = bed.view->OpenMenu();
    REQUIRE(menu != nullptr);
    CHECK(ItemNamed(*menu, u8"New") != nullptr);
    gui::MenuItem* newGroup = ItemNamed(*menu, u8"New Group");
    REQUIRE(newGroup != nullptr);
    CHECK(ItemNamed(*menu, u8"Cook All") != nullptr); // the top group's: all
    CHECK(ItemNamed(*menu, u8"Delete Group...") == nullptr); // Content is not deleted
    CHECK(newGroup->HandleActivate());
    CHECK(bed.view->CurrentGroup() != nullptr);
    CHECK(bed.view->CurrentGroup()->Name() == StringView(u8"Group")); // made and shown
    bed.view->Navigate(*bed.top);

    // Delete on the selection: asked, naming it; Delete deletes.
    list.SetSelectedItem(bed.RowOf(u8"Crate"));
    list.RequestFocus();
    bed.root->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(gui::KeyCode::Delete));
    gui::MessageBox* confirm = bed.view->OpenConfirm();
    REQUIRE(confirm != nullptr);
    CHECK(confirm->GetMessage().ContainsIgnoreCase(u8"Delete Crate?"));
    const Guid id = bed.crate->Id();
    REQUIRE(confirm->ButtonAt(1) != nullptr);
    CHECK(confirm->ButtonAt(1)->HandleActivate()); // Delete
    CHECK(bed.project->SourceDb().GetInstance(id) == nullptr);
    bed.view->Refresh();
    CHECK(bed.RowOf(u8"Crate") < 0);
}

TEST_CASE("assets: a row's name and a tree group's edited in place rename them, a refused name saying why and kept")
{
    Bed bed;
    gui::IModel& rows = *bed.view->List()->GetModel();
    const i32 crate = bed.RowOf(u8"Crate");
    CHECK(rows.IsEditable(gui::ModelIndex{crate, 0}));
    CHECK(rows.SetData(gui::ModelIndex{crate, 0}, gui::Variant(StringView(u8"Box"))));
    CHECK(bed.crate->Name() == StringView(u8"Box"));
    CHECK_FALSE(rows.SetData(gui::ModelIndex{crate, 0}, gui::Variant(StringView(u8"a:b")))); // refused: the editor stays
    CHECK(bed.crate->Name() == StringView(u8"Box"));

    gui::TreeView& tree = *bed.view->Tree();
    gui::IModel& groups = *tree.GetModel();
    i32 townItem = -1;
    for (i32 item = 0; item < static_cast<i32>(tree.GetItemCount()); ++item)
        if (tree.GetItemText(item) == String(u8"Town"))
            townItem = item;
    REQUIRE(townItem >= 0);
    CHECK_FALSE(groups.IsEditable(tree.GetItemIndex(0))); // Content
    CHECK(groups.SetData(tree.GetItemIndex(townItem), gui::Variant(StringView(u8"Village"))));
    CHECK(bed.town->Name() == StringView(u8"Village"));
}

TEST_CASE("assets: assets dropped on a group's row or a group in the tree move into it; one already there or taken there is refused, saying why")
{
    Bed bed;
    gui::IModel& rows = *bed.view->List()->GetModel();
    const gui::DragPayload crate = AssetDrag::Payload(bed.crate->Id(), u8"BrowserMesh", u8"Crate");

    // Onto the Models row: moved.
    gui::ModelDrop drop;
    drop.Payload = &crate;
    drop.Index = gui::ModelIndex{bed.RowOf(u8"Models"), 0};
    drop.Place = gui::DropPlace::On;
    String why;
    CHECK(rows.CanDrop(drop, why) == gui::DropEffect::Move);
    CHECK(why == String(u8"Move into Models"));
    drop.Place = gui::DropPlace::After; // between rows: not a move
    CHECK(rows.CanDrop(drop, why) == gui::DropEffect::None);
    drop.Place = gui::DropPlace::On;
    CHECK(rows.Drop(drop));
    CHECK(&bed.crate->OwningGroup() == bed.models);
    CHECK(bed.crate->Path() == String(u8"Models/Crate"));

    // Into Town through the tree; then back where it is: refused.
    gui::TreeView& tree = *bed.view->Tree();
    gui::IModel& groups = *tree.GetModel();
    i32 townItem = -1, modelsItem = -1;
    for (i32 item = 0; item < static_cast<i32>(tree.GetItemCount()); ++item)
    {
        if (tree.GetItemText(item) == String(u8"Town"))
            townItem = item;
        if (tree.GetItemText(item) == String(u8"Models"))
            modelsItem = item;
    }
    REQUIRE(townItem >= 0);
    gui::ModelDrop onto;
    onto.Payload = &crate;
    onto.Index = tree.GetItemIndex(townItem);
    onto.Place = gui::DropPlace::On;
    CHECK(groups.Drop(onto));
    CHECK(&bed.crate->OwningGroup() == bed.town);
    CHECK(groups.CanDrop(onto, why) == gui::DropEffect::None);
    CHECK(why == String(u8"Already in Models/Town"));

    // A name taken where it would go: refused, saying so.
    REQUIRE(bed.models->CreateInstance(u8"Barrel", BrowserMesh::StaticType()) != nullptr);
    const gui::DragPayload barrel = AssetDrag::Payload(bed.barrel->Id(), u8"BrowserMesh", u8"Barrel");
    onto.Payload = &barrel;
    onto.Index = tree.GetItemIndex(modelsItem);
    CHECK(groups.CanDrop(onto, why) == gui::DropEffect::None);
    CHECK(why == String(u8"Barrel is already in Models"));
}

TEST_CASE("assets: a tile shows its type's glyph until its thumbnail is ready, then the thumbnail; a row, small")
{
    Bed bed;
    CHECK(bed.view->ShowsTiles());
    AssetCell* crate = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(crate != nullptr);
    CHECK_FALSE(crate->ShowsPicture());
    bed.picture = RefPtr<gui::Drawable>(gui::SvgDrawable::FromText(DefaultAllocator(), u8"<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><rect width='8' height='8'/></svg>").Get());
    REQUIRE(bed.picture);
    bed.pictured = bed.crate->Id();
    bed.context.NotifyThumbnailReady(bed.crate->Id()); // as the thumbnail service tells it
    crate = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(crate != nullptr);
    CHECK(crate->ShowsPicture());
    bed.view->SetTiles(false);
    (void)bed.root->UpdateLayout();
    crate = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(crate != nullptr);
    CHECK_FALSE(crate->IsTile());
    CHECK(crate->ShowsPicture()); // in its icon's place
}

TEST_CASE("assets: a right click on a group in the tree opens its menu; a browser with no verbs opens none (a crash, 2026-10-25)")
{
    Bed bed;
    gui::TreeView& tree = *bed.view->Tree();
    i32 town = -1;
    for (i32 item = 0; item < static_cast<i32>(tree.GetItemCount()); ++item)
        if (tree.GetItemText(item) == String(u8"Town"))
            town = item;
    REQUIRE(town >= 0);
    tree.EditLayoutStyle([](gui::LayoutStyle& s)
    {
        s.Width = gui::LayoutLength::Px(200.0f);
        s.Height = gui::LayoutLength::Px(300.0f);
    });
    (void)bed.root->UpdateLayout();
    const gui::Rect box = tree.GetScreenBounds();
    const auto rightClick = [&](f32 y)
    {
        gui::EventDispatcher& d = *bed.root->GetEventDispatcher();
        const Float2 at{box.x + 40.0f, y};
        d.InjectMouseMove(at);
        d.InjectMouseDown(at, gui::MouseButton::Right);
        d.InjectMouseUp(at, gui::MouseButton::Right);
    };
    // Each row its row height: Town is the third (Content, Models, Town).
    const f32 rowHeight = tree.GetRowHeight() > 0.0f ? tree.GetRowHeight() : 26.0f;
    rightClick(box.y + rowHeight * (static_cast<f32>(town) + 0.5f));
    REQUIRE(bed.view->OpenMenu() != nullptr);
    CHECK(bed.view->CurrentGroup() == bed.town);
    CHECK(ItemNamed(*bed.view->OpenMenu(), u8"Delete Group...") != nullptr);
    bed.view->OpenMenu()->Close();

    // The same browser with no verbs: no menu, and no crash.
    AssetsView::Services bare;
    bare.context = &bed.context;
    AssetsView plain(DefaultAllocator(), Move(bare), LoadAssets());
    RefPtr<gui::Node> panel = plain.Content();
    REQUIRE(panel);
    bed.root->RemoveChild(bed.view->Content().Get());
    Cast<gui::UIWidget>(panel.Get())->EditLayoutStyle([](gui::LayoutStyle& s)
    {
        s.Width = gui::LayoutLength::Px(900.0f);
        s.Height = gui::LayoutLength::Px(600.0f);
    });
    bed.root->AddChild(panel.Get());
    plain.Rebuild();
    plain.Tree()->EditLayoutStyle([](gui::LayoutStyle& s)
    {
        s.Width = gui::LayoutLength::Px(200.0f);
        s.Height = gui::LayoutLength::Px(300.0f);
    });
    (void)bed.root->UpdateLayout();
    const gui::Rect plainBox = plain.Tree()->GetScreenBounds();
    gui::EventDispatcher& d = *bed.root->GetEventDispatcher();
    const Float2 at{plainBox.x + 40.0f, plainBox.y + rowHeight * 0.5f};
    d.InjectMouseMove(at);
    d.InjectMouseDown(at, gui::MouseButton::Right);
    d.InjectMouseUp(at, gui::MouseButton::Right);
    CHECK(plain.OpenMenu() == nullptr);
    bed.root->RemoveChild(panel.Get());
}

TEST_CASE("assets: an asset or a group always exported is marked: a tile's corner mark, a row's badge; the menu's toggle redraws it")
{
    Bed bed;
    CHECK(bed.view->ShowsTiles());
    AssetCell* crate = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(crate != nullptr);
    CHECK_FALSE(crate->ShowsAlwaysExport());

    // Through the asset's menu, as a user does: the cell redraws with its mark.
    bed.view->ShowMenuFor(bed.RowOf(u8"Crate"));
    REQUIRE(bed.view->OpenMenu() != nullptr);
    gui::MenuItem* always = ItemNamed(*bed.view->OpenMenu(), u8"Always Export");
    REQUIRE(always != nullptr);
    CHECK(always->HandleActivate());
    crate = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(crate != nullptr);
    CHECK(crate->ShowsAlwaysExport());

    // A group's contents, and the list's badge.
    bed.ops->ToggleAlwaysExport(*bed.models);
    bed.view->SetTiles(false);
    AssetCell* models = bed.Cell(bed.RowOf(u8"Models"));
    REQUIRE(models != nullptr);
    CHECK_FALSE(models->IsTile());
    CHECK(models->ShowsAlwaysExport());
    crate = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(crate != nullptr);
    CHECK(crate->ShowsAlwaysExport());

    bed.ops->ToggleAlwaysExport(bed.crate->Id()); // off again
    bed.view->Rebuild();
    crate = bed.Cell(bed.RowOf(u8"Crate"));
    REQUIRE(crate != nullptr);
    CHECK_FALSE(crate->ShowsAlwaysExport());
}

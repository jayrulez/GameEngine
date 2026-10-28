// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The asset picker over its markup (Data/Editor/AssetPicker.xml) and a scratch project: the groups a
// tree (Favourites and All first, then the project's, Content at the top); the current value's group
// chosen and its row lit, the foot saying Esc keeps it; a group holding only what the slot takes; a
// search over every group, each match saying where it lives; Favourites; tiles or a list kept with
// the project for the next pick; Enter picking and closing; None picking nothing.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::AssetCell;
using editor::app::AssetPickerWindow;
using editor::app::AssetPickRequest;
namespace gui = foundation::gui;
namespace content = foundation::content;

namespace
{
    class PickerMesh : public ISerializable
    {
        RTTI_OBJECT(PickerMesh, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };
    class PickerSound : public ISerializable
    {
        RTTI_OBJECT(PickerSound, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    RefPtr<gui::Node> LoadAssetPicker()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(AssetPickerWindow::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    // Content (Crate, Models (Town (Barrel, Bell, Bucket)), Sounds).
    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        foundation::settings::Settings store{DefaultAllocator()};
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        UniquePtr<AssetPickerWindow> picker;
        content::Instance* crate = nullptr;
        content::Instance* barrel = nullptr;
        content::Instance* bucket = nullptr;
        Array<Guid> chosen;
        usize saves = 0;
        RefPtr<gui::Drawable> picture; // the crate's thumbnail, once "generated"

        Bed()
        {
            std::error_code ec;
            std::filesystem::remove_all("asset_picker_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"asset_picker_project", u8"Pick").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"asset_picker_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(PickerMesh::StaticType());
            GlobalTypeRegistry().Register(PickerSound::StaticType());
            content::Group* top = project->SourceDb().RootGroup();
            crate = top->CreateInstance(u8"Crate", PickerMesh::StaticType());
            content::Group* town = top->CreateGroup(u8"Models")->CreateGroup(u8"Town");
            barrel = town->CreateInstance(u8"Barrel", PickerMesh::StaticType());
            REQUIRE(town->CreateInstance(u8"Bell", PickerSound::StaticType()) != nullptr);
            bucket = town->CreateInstance(u8"Bucket", PickerMesh::StaticType());
            REQUIRE(top->CreateGroup(u8"Sounds") != nullptr);
            REQUIRE(crate != nullptr);
            REQUIRE(barrel != nullptr);
            REQUIRE(bucket != nullptr);
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            Bed* self = this;
            context.OnProjectEditorSettingsSaveRequested = [self]() { ++self->saves; };
            root->SetSize(Float2{900.0f, 600.0f});
            AssetPickerWindow::Services services;
            services.context = &context;
            services.picture = [self](const Guid& id) { return id == self->crate->Id() ? self->picture : RefPtr<gui::Drawable>{}; };
            picker = MakeUnique<AssetPickerWindow>(DefaultAllocator(), DefaultAllocator(), Move(services), LoadAssetPicker());
        }
        ~Bed()
        {
            picker.Reset();
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            std::error_code ec;
            std::filesystem::remove_all("asset_picker_project", ec);
        }
        // A mesh slot holding `current`.
        void Open(const Guid& current)
        {
            AssetPickRequest request;
            request.types.PushBack(String(reinterpret_cast<const utf8char*>(PickerMesh::StaticType().name)));
            request.current = current;
            Bed* self = this;
            request.chosen = [self](const Guid& id) { self->chosen.PushBack(id); };
            REQUIRE(picker->Open(*root, Move(request)));
            (void)root->UpdateLayout();
        }
        void Key(gui::KeyCode code) { root->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(code)); }
        // The cell a row is drawn by, once laid out.
        AssetCell* Cell(i32 row)
        {
            (void)root->UpdateLayout();
            Array<gui::Node*> pending;
            pending.PushBack(picker->List());
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

namespace
{
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

RTTI_DEFINE_OBJECT(PickerMesh, "rtti::tests::assetpicker")
RTTI_DEFINE_OBJECT(PickerSound, "rtti::tests::assetpicker")

TEST_CASE("asset picker: the groups a tree, Favourites and All first; the current value's group chosen, its row lit")
{
    Bed bed;
    bed.Open(bed.barrel->Id());
    CHECK(bed.picker->IsOpen());
    CHECK(bed.picker->Groups() == String(u8"Favourites | All PickerMeshes | Content | Models | Town | Sounds"));
    CHECK(bed.picker->Rows() == String(u8"Barrel | Bucket")); // the sound is not one it takes
    CHECK(bed.picker->LitName() == StringView(u8"Barrel"));
    CHECK(bed.picker->Where() == StringView(u8"Town  ·  2 pickermeshes"));
    CHECK(bed.picker->Hint() == StringView(u8"Enter picks, Esc keeps Barrel"));
}

TEST_CASE("asset picker: a group holds what it holds itself; All every group's; a search every group's, saying where each lives")
{
    Bed bed;
    bed.Open(Guid{});
    CHECK(bed.picker->Rows() == String(u8"Crate | Barrel | Bucket")); // nothing current: All
    CHECK(bed.picker->LitName() == StringView(u8"Crate"));
    REQUIRE(bed.picker->ChooseSource(u8"Content"));
    CHECK(bed.picker->Rows() == String(u8"Crate"));
    REQUIRE(bed.picker->ChooseSource(u8"Sounds"));
    CHECK(bed.picker->Rows().IsEmpty());
    bed.picker->Search(u8"bu");
    CHECK(bed.picker->Rows() == String(u8"Bucket"));
    CHECK(bed.picker->Where() == StringView(u8"In every group  ·  1 pickermeshes"));
    REQUIRE(bed.picker->List() != nullptr);
    bed.picker->List()->EditLayoutStyle([](gui::LayoutStyle& s)
    {
        s.Width = gui::LayoutLength::Px(400.0f);
        s.Height = gui::LayoutLength::Px(300.0f);
    });
    AssetCell* cell = bed.Cell(0);
    REQUIRE(cell != nullptr);
    CHECK(cell->Kind() == StringView(u8"PickerMesh in Content/Models/Town"));
    CHECK(cell->GetTooltipText() == StringView(u8"Content/Models/Town/Bucket (PickerMesh)")); // hovered: the whole of where it lives
    bed.picker->Search(StringView{});
    CHECK(bed.picker->Rows().IsEmpty()); // the chosen group again
}

TEST_CASE("asset picker: Favourites holds the starred assets it takes")
{
    Bed bed;
    bed.context.ToggleFavorite(bed.bucket->Id());
    bed.Open(Guid{});
    REQUIRE(bed.picker->ChooseSource(u8"Favourites"));
    CHECK(bed.picker->Rows() == String(u8"Bucket"));
}

TEST_CASE("asset picker: a row's menu pins it as a favourite, or unpins it")
{
    Bed bed;
    bed.Open(Guid{});
    REQUIRE(bed.picker->ChooseSource(u8"Favourites"));
    CHECK(bed.picker->Rows().IsEmpty());
    REQUIRE(bed.picker->ChooseSource(u8"All PickerMeshes"));
    bed.picker->ShowRowMenu(0, Float2{20.0f, 10.0f}); // Crate
    gui::Menu* menu = bed.picker->RowMenu();
    REQUIRE(menu != nullptr);
    gui::MenuItem* pin = ItemNamed(*menu, u8"Pin as Favourite");
    REQUIRE(pin != nullptr);
    CHECK(pin->HandleActivate());
    CHECK(bed.context.IsFavorite(bed.crate->Id()));
    REQUIRE(bed.picker->ChooseSource(u8"Favourites"));
    CHECK(bed.picker->Rows() == String(u8"Crate"));
    bed.picker->ShowRowMenu(0, Float2{20.0f, 10.0f});
    menu = bed.picker->RowMenu();
    REQUIRE(menu != nullptr);
    gui::MenuItem* unpin = ItemNamed(*menu, u8"Unpin Favourite");
    REQUIRE(unpin != nullptr);
    CHECK(unpin->HandleActivate());
    CHECK_FALSE(bed.context.IsFavorite(bed.crate->Id()));
    CHECK(bed.picker->Rows().IsEmpty()); // it left the Favourites shown
}

TEST_CASE("asset picker: a whole guid of an asset the slot does not take says what it is")
{
    Bed bed;
    bed.Open(Guid{});
    content::Instance* bell = nullptr;
    for (content::Instance* instance : bed.barrel->OwningGroup().Instances())
        if (instance->Name() == StringView(u8"Bell"))
            bell = instance;
    REQUIRE(bell != nullptr);
    utf8char text[37];
    bell->Id().ToChars(text);
    bed.picker->Search(StringView(text, 36));
    CHECK(bed.picker->Rows().IsEmpty());
    CHECK(bed.picker->State() == StringView(u8"'Bell' is a PickerSound, which this slot does not take."));
    bed.picker->Search(u8"nothing like it");
    CHECK(bed.picker->State() == StringView(u8"No pickermeshes match \"nothing like it\"."));
}

TEST_CASE("asset picker: a row shows the asset's thumbnail small in its icon's place, as a tile does")
{
    Bed bed;
    bed.picture = RefPtr<gui::Drawable>(gui::SvgDrawable::FromText(DefaultAllocator(), u8"<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><rect width='8' height='8'/></svg>").Get());
    bed.Open(Guid{});
    REQUIRE(bed.picker->ChooseSource(u8"All PickerMeshes"));
    CHECK_FALSE(bed.picker->ShowsTiles());
    AssetCell* crate = bed.Cell(0);
    REQUIRE(crate != nullptr);
    CHECK(crate->ShowsPicture());
    AssetCell* barrel = bed.Cell(1);
    REQUIRE(barrel != nullptr);
    CHECK_FALSE(barrel->ShowsPicture()); // none yet: its glyph
}

TEST_CASE("asset picker: tiles or a list, kept with the project for the next pick")
{
    Bed bed;
    bed.Open(Guid{});
    CHECK_FALSE(bed.picker->ShowsTiles()); // a list until the project says otherwise
    bed.picker->SetTiles(true);
    const editor::app::EditorAssetBrowserSettings* saved = bed.store.Find<editor::app::EditorAssetBrowserSettings>();
    REQUIRE(saved != nullptr);
    CHECK(saved->pickerGridMode);
    CHECK(bed.saves == 1u);
    bed.picker->Close();
    bed.Open(Guid{});
    CHECK(bed.picker->ShowsTiles());
}

TEST_CASE("asset picker: Down then Enter picks the next row and closes; its close keeps the value")
{
    Bed bed;
    bed.Open(bed.barrel->Id());
    bed.Key(gui::KeyCode::Down);
    CHECK(bed.picker->LitName() == StringView(u8"Bucket"));
    bed.Key(gui::KeyCode::Return);
    REQUIRE(bed.chosen.Size() == 1u);
    CHECK(bed.chosen[0] == bed.bucket->Id());
    CHECK_FALSE(bed.picker->IsOpen());

    bed.Open(bed.barrel->Id());
    REQUIRE(bed.picker->CloseButton() != nullptr);
    CHECK(bed.picker->CloseButton()->HandleActivate());
    CHECK(bed.chosen.Size() == 1u); // nothing picked: the value stays
    CHECK_FALSE(bed.picker->IsOpen());
}

TEST_CASE("asset picker: a long where gives way, never the name, nor drawn over it; each where starts in one column")
{
    Bed bed;
    content::Group* deep = bed.project->SourceDb().RootGroup()->CreateGroup(u8"Models");
    for (const char8_t* name : {u8"Mechanics", u8"Bridge_Modular_With_A_Long_Name", u8"Bridge_Modular_Pieces_Also_Long"})
        deep = deep->CreateGroup(StringView(name));
    REQUIRE(bed.project->SourceDb().RootGroup()->CreateInstance(u8"Cube.051", PickerMesh::StaticType()) != nullptr);
    REQUIRE(deep->CreateInstance(u8"Cube.051", PickerMesh::StaticType()) != nullptr);

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
    bed.Open(Guid{});
    bed.picker->Search(u8"Cube"); // every group's, each row saying where it lives
    REQUIRE(bed.picker->Rows() == String(u8"Cube.051 | Cube.051"));
    styles.ApplyTree(*bed.root);
    AssetCell* near = bed.Cell(0); // the rows made, then styled too
    AssetCell* far = bed.Cell(1);
    REQUIRE(near != nullptr);
    REQUIRE(far != nullptr);
    styles.ApplyTree(*bed.root);
    REQUIRE(far->Kind().Size() > near->Kind().Size()); // the deep one's where is the long one
    // A real font after the sheet, so the texts have widths (the editor's own face).
    foundation::fonts::TrueTypeFonts::Initialize();
    foundation::fonts::TrueTypeFontService fonts{DefaultAllocator()};
    const String data = foundation::vfs::FindDataRoot();
    REQUIRE(fonts.LoadFont(u8"Inter", foundation::vfs::DataPath(data.AsView(), u8"Assets/fonts/inter/Inter-Regular.ttf").AsView()) == foundation::fonts::FontLoadResult::Success);
    far->NameLabel()->SetFont(fonts.GetFont(u8"Inter", 13.0f));
    far->KindLabel()->SetFont(fonts.GetFont(u8"Inter", 11.0f));
    (void)bed.root->UpdateLayout();

    const gui::Rect farName = far->NameLabel()->GetScreenBounds();
    const gui::Rect farKind = far->KindLabel()->GetScreenBounds();
    const gui::Rect farRow = far->GetScreenBounds();
    REQUIRE(far->KindLabel()->MeasureText().x > farKind.width); // the where is cut: it did not fit
    CHECK(farName.width >= far->NameLabel()->MeasureText().x - 0.5f); // the name keeps its room
    CHECK(farKind.x >= farName.x + farName.width - 0.5f);             // the where after it, not over it
    CHECK(farKind.x + farKind.width <= farRow.x + farRow.width + 0.5f);
    // A column: where each asset lives starts at one x, whatever its name or its path.
    CHECK(near->KindLabel()->GetScreenBounds().x == doctest::Approx(farKind.x));
    bed.picker->Close();
}

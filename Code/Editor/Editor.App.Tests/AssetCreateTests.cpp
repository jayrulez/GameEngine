// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The group picker, the rule a new asset's name keeps, and Create Asset over its markup
// (Data/Editor/AssetCreate.xml): the groups in the tree's order, each by name and what holds it,
// a key finding its group again; a name refused when empty, holding what a file cannot, spaced at
// an end or taken, saying why; Create waiting while refused, the line under the name saying why
// or what it makes, the group field picking another group, and Create answering the group and
// the name, then closing.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::AssetCreate;
using editor::app::AssetNames;
using editor::app::GroupPicker;
using editor::app::Picker;
using editor::app::PickerRequest;
namespace gui = foundation::gui;
namespace content = foundation::content;

namespace
{
    class CreateTestAsset : public ISerializable
    {
        RTTI_OBJECT(CreateTestAsset, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    RefPtr<gui::Node> Load(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
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

    // A project of groups: Content (Crate), Models (Town (Barrel)), Sounds.
    struct Project
    {
        UniquePtr<editor::EditorProject> project;
        content::Group* top = nullptr;
        content::Group* models = nullptr;
        content::Group* town = nullptr;
        Project()
        {
            std::error_code ec;
            std::filesystem::remove_all("asset_create_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"asset_create_project", u8"Create").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"asset_create_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(CreateTestAsset::StaticType());
            top = project->SourceDb().RootGroup();
            REQUIRE(top != nullptr);
            REQUIRE(top->CreateInstance(u8"Crate", CreateTestAsset::StaticType()) != nullptr);
            models = top->CreateGroup(u8"Models");
            REQUIRE(models != nullptr);
            town = models->CreateGroup(u8"Town");
            REQUIRE(town != nullptr);
            REQUIRE(town->CreateInstance(u8"Barrel", CreateTestAsset::StaticType()) != nullptr);
            REQUIRE(top->CreateGroup(u8"Sounds") != nullptr);
        }
        ~Project()
        {
            project.Reset();
            std::error_code ec;
            std::filesystem::remove_all("asset_create_project", ec);
        }
        [[nodiscard]] content::ContentDatabase& Sources() { return project->SourceDb(); }
    };
}

RTTI_DEFINE_OBJECT(CreateTestAsset, "rtti::tests::assetcreate")

TEST_CASE("group picker: the groups a tree in its order, each by name at its depth; a key finds its group again")
{
    Project bed;
    const PickerRequest request = GroupPicker::Request(bed.Sources(), bed.town);
    CHECK(request.Tree);
    REQUIRE(request.Items.Size() == 4u);
    CHECK(request.Items[0].Name == String(u8"Content"));
    CHECK(request.Items[0].Depth == 0u);
    CHECK(request.Items[1].Name == String(u8"Models"));
    CHECK(request.Items[1].Depth == 1u);
    CHECK(request.Items[2].Name == String(u8"Town"));
    CHECK(request.Items[2].Depth == 2u);
    CHECK(request.Items[3].Name == String(u8"Sounds"));
    CHECK(request.Items[3].Depth == 1u);
    CHECK(request.Current == String(u8"Content/Models/Town"));
    CHECK(GroupPicker::Find(bed.Sources(), request.Items[2].Key.AsView()) == bed.town);
    CHECK(GroupPicker::Find(bed.Sources(), u8"Content") == bed.top);
    CHECK(GroupPicker::Find(bed.Sources(), u8"Content/Nowhere") == nullptr);
    CHECK(GroupPicker::Find(bed.Sources(), u8"Elsewhere") == nullptr);
}

TEST_CASE("asset names: refused when empty, holding what a file cannot, spaced at an end or taken, saying why")
{
    Project bed;
    CHECK(AssetNames::Refusal(*bed.top, u8"") == String(u8"Enter a name."));
    CHECK(AssetNames::Refusal(*bed.top, u8"a/b") == String(u8"A name cannot hold /."));
    CHECK(AssetNames::Refusal(*bed.top, u8"what?") == String(u8"A name cannot hold ?."));
    CHECK(AssetNames::Refusal(*bed.top, u8" Crate") == String(u8"A name cannot start or end with a space, or end with a dot."));
    CHECK(AssetNames::Refusal(*bed.top, u8"Crate.") == String(u8"A name cannot start or end with a space, or end with a dot."));
    CHECK(AssetNames::Refusal(*bed.top, u8"Crate") == String(u8"Crate is already in Content."));
    CHECK(AssetNames::Refusal(*bed.town, u8"Barrel") == String(u8"Barrel is already in Models/Town."));
    CHECK(AssetNames::Refusal(*bed.town, u8"Crate").IsEmpty()); // in another group it is free
}

TEST_CASE("create asset: the groups a tree in it, a click choosing one; Create waits while the name is refused, saying why; Create answers and closes")
{
    Project bed;
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{800.0f, 600.0f});
    content::Group* madeIn = nullptr;
    String madeName;
    RefPtr<gui::Window> window =
        AssetCreate::Open(DefaultAllocator(), *root, Load(StringView(editor::app::kAssetCreatePath)), bed.Sources(), *bed.top, u8"New Asset", u8"Crate",
                          [&madeIn, &madeName](content::Group& group, StringView name)
                          {
                              madeIn = &group;
                              madeName = String(name);
                          });
    REQUIRE(window);
    (void)root->UpdateLayout();
    CHECK(window->IsOpen());
    auto* name = FindId<gui::TextField>(*window, u8"asset-create-name");
    auto* why = FindId<gui::Label>(*window, u8"asset-create-why");
    auto* create = FindId<gui::Button>(*window, u8"asset-create-create");
    auto* tree = FindId<gui::TreeView>(*window, u8"asset-create-groups");
    REQUIRE(name != nullptr);
    REQUIRE(why != nullptr);
    REQUIRE(create != nullptr);
    REQUIRE(tree != nullptr);
    i32 town = -1;
    i32 chosen = -1;
    for (i32 item = 0; item < static_cast<i32>(tree->GetItemCount()); ++item)
    {
        if (tree->GetItemText(item) == String(u8"Town"))
            town = item;
        if (tree->IsItemSelected(item))
            chosen = item;
    }
    REQUIRE(town >= 0); // every group shown, open
    CHECK(tree->GetItemText(chosen) == String(u8"Content")); // the group it starts in
    CHECK_FALSE(create->IsEnabled()); // the suggestion is taken here
    CHECK(why->GetText() == StringView(u8"Crate is already in Content."));
    CHECK(why->HasClass(u8"refused"));

    // Another group, clicked in its tree: the same name is free there.
    {
        (void)root->UpdateLayout();
        gui::ItemRow* row = tree->RowForItem(town);
        REQUIRE(row != nullptr);
        const gui::Rect box = row->GetScreenBounds();
        const Float2 at{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
        root->GetEventDispatcher()->InjectMouseDown(at, gui::MouseButton::Left);
        root->GetEventDispatcher()->InjectMouseUp(at, gui::MouseButton::Left);
    }
    CHECK(create->IsEnabled());
    CHECK(why->GetText() == StringView(u8"Makes Crate in Models/Town."));
    CHECK_FALSE(why->HasClass(u8"refused"));

    const auto type = [&root, name](StringView text) // as the user types: over what is there
    {
        name->RequestFocus();
        name->SelectAll();
        (void)root->GetEventDispatcher()->InjectText(text);
    };
    type(u8"Barrel"); // taken in Town
    CHECK_FALSE(create->IsEnabled());
    CHECK(why->GetText() == StringView(u8"Barrel is already in Models/Town."));
    type(u8"Lamp");
    CHECK(create->HandleActivate());
    CHECK(madeIn == bed.town);
    CHECK(madeName == String(u8"Lamp"));
    CHECK_FALSE(window->IsOpen());
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The inspector's asset reference on the property grid: a Reference row is an asset slot taking
// the types its services name for what the reference points at, showing the asset's name and
// glyph (None for nothing, "(missing)" for a dangling id, Mixed over objects that differ); a pick,
// a drop and a clear are one write each; open and reveal name the asset; a read-only one only
// opens; and the context's table asks the builders' join (else takes the type itself) and routes
// open and reveal.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::AssetDrag;
using editor::app::ResourceRefEditor;
using editor::app::ResourceRefServices;
using editor::app::ResourceRefShown;
namespace gui = foundation::gui;

namespace
{
    struct TestMesh
    {
        int payload = 0;
    };
    // The source assets a project of the test holds.
    class RefMeshAsset : public ISerializable
    {
        RTTI_OBJECT(RefMeshAsset, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };
    class RefSoundAsset : public ISerializable
    {
        RTTI_OBJECT(RefSoundAsset, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    String Text(const Guid& id)
    {
        utf8char chars[37];
        id.ToChars(chars);
        return String(StringView(chars, 36));
    }

    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::PropertyGrid> grid = MakeRef<gui::PropertyGrid>(DefaultAllocator());
        gui::PropertyBag bag;
        ResourceRefServices services;
        const Guid wood{0x1234u, 0x5678u};
        const Guid lost{0x9u, 0x9u};
        Array<Guid> opened;
        Array<Guid> revealed;
        Function<void(const Guid&)> chosen; // the picker's answer, held until the test gives it
        Array<String> pickedFrom;
        Guid pickedOver;
        int commits = 0;
        RefPtr<gui::Drawable> woodPicture; // its thumbnail, once "generated"

        explicit Bed(usize objects = 1) : bag(DefaultAllocator(), objects)
        {
            root->SetSize(Float2{400.0f, 300.0f});
            root->EditLayoutStyle([](gui::LayoutStyle& s) { s.AlignItems = gui::Align::Start; });
            grid->EditLayoutStyle([](gui::LayoutStyle& s)
            {
                s.Width = gui::LayoutLength::Px(300.0f);
                s.Height = gui::LayoutLength::Px(240.0f);
            });
            root->AddChild(grid.Get());
            services.AcceptedTypes = [](const TypeInfo& target)
            {
                Array<String> names;
                if (&target == &TypeOf<TestMesh>())
                {
                    names.PushBack(String(u8"ModelAsset"));
                    names.PushBack(String(u8"MeshAsset"));
                }
                return names;
            };
            services.Describe = [this](const Guid& id, ResourceRefShown& out)
            {
                if (id != wood)
                    return false;
                out.Name = String(u8"Wood");
                out.Glyph = String(u8"Editor/Icons/Types/MeshAsset.svg");
                out.GlyphClass = String(u8"cat-rendering");
                out.Picture = woodPicture;
                return true;
            };
            services.Pick = [this](Span<const String> types, const Guid& current, Function<void(const Guid&)> answer)
            {
                pickedOver = current;
                pickedFrom.Clear();
                for (const String& type : types)
                    pickedFrom.PushBack(type);
                chosen = Move(answer);
            };
            services.Open = [this](const Guid& id) { opened.PushBack(id); };
            services.Reveal = [this](const Guid& id) { revealed.PushBack(id); };
            ResourceRefEditor::Register(grid->GetEditors(), services);
            bag.SetOnCommit([this](const gui::PropertyBag::Commit&) { ++commits; });
        }
        ~Bed() { grid->SetSource(nullptr); }
        gui::PropertyInfo& Add(const char8_t* path, const Guid& value)
        {
            gui::PropertyInfo& info = bag.Add(StringView(path), gui::PropertyKind::Reference, gui::Variant(value.IsNil() ? StringView{} : Text(value).AsView()));
            info.ReferenceType = &TypeOf<TestMesh>();
            return info;
        }
        void Show()
        {
            grid->SetSource(&bag);
            (void)root->UpdateLayout();
        }
        ResourceRefEditor& Editor(const char8_t* path)
        {
            ResourceRefEditor* editor = Cast<ResourceRefEditor>(grid->FindEditor(StringView(path)));
            REQUIRE(editor != nullptr);
            return *editor;
        }
    };
}

RTTI_DEFINE_OBJECT(RefMeshAsset, "rtti::tests::resourceref")
RTTI_DEFINE_OBJECT(RefSoundAsset, "rtti::tests::resourceref")

TEST_CASE("resource-ref: a reference row is a slot taking the types for its target; None, then the asset's name and glyph")
{
    Bed bed;
    bed.Add(u8"mesh", Guid{});
    bed.Add(u8"other", bed.lost);
    bed.Show();
    ResourceRefEditor& mesh = bed.Editor(u8"mesh");
    editor::app::AssetPickerSlot& slot = *mesh.Slot();
    REQUIRE(slot.AcceptedTypes().Size() == 2u);
    CHECK(slot.AcceptedTypes()[0] == String(u8"ModelAsset"));
    CHECK(slot.ShownName() == StringView(u8"Not set"));
    CHECK(slot.PickButton()->IsDisplayed());
    CHECK_FALSE(slot.ClearButton()->IsDisplayed());
    CHECK(bed.Editor(u8"other").Slot()->ShownName() == ResourceRefEditor::kMissingText); // dangling: still cleared
    CHECK(bed.Editor(u8"other").Slot()->ClearButton()->IsDisplayed());

    // A pick: over the row's types, one write, the slot showing the asset.
    slot.Pick();
    REQUIRE(bed.chosen);
    CHECK(bed.pickedFrom.Size() == 2u);
    CHECK(bed.pickedOver.IsNil());
    bed.chosen(bed.wood);
    (void)bed.root->UpdateLayout();
    CHECK(bed.commits == 1);
    CHECK(bed.bag.GetValue(StringView(u8"mesh"), 0).AsString() == Text(bed.wood).AsView());
    CHECK(mesh.Current() == bed.wood);
    CHECK(slot.ShownName() == StringView(u8"Wood"));

    // Open and reveal name it; a clear is one write to nothing.
    CHECK(slot.OpenButton()->HandleActivate());
    CHECK(slot.RevealButton()->HandleActivate());
    REQUIRE(bed.opened.Size() == 1u);
    CHECK(bed.opened[0] == bed.wood);
    CHECK(bed.revealed.Size() == 1u);
    CHECK(slot.ClearButton()->HandleActivate());
    (void)bed.root->UpdateLayout();
    CHECK(bed.commits == 2);
    CHECK(bed.bag.GetValue(StringView(u8"mesh"), 0).AsString().IsEmpty());
    CHECK(slot.ShownName() == StringView(u8"Not set"));
}

TEST_CASE("resource-ref: the slot draws the asset's thumbnail in its glyph's place once there is one")
{
    Bed bed;
    bed.Add(u8"mesh", bed.wood);
    bed.Show();
    editor::app::AssetPickerSlot& slot = *bed.Editor(u8"mesh").Slot();
    CHECK(slot.Picture() == nullptr); // none generated yet: the type's glyph
    bed.woodPicture = RefPtr<gui::Drawable>(gui::SvgDrawable::FromText(DefaultAllocator(), u8"<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><rect width='8' height='8'/></svg>").Get());
    bed.grid->SetSource(nullptr);
    bed.Show();
    editor::app::AssetPickerSlot& again = *bed.Editor(u8"mesh").Slot();
    CHECK(again.Picture() == bed.woodPicture.Get());
    CHECK(again.ShownName() == StringView(u8"Wood"));
    again.SetPicture({});
    CHECK(again.Picture() == nullptr); // back to the glyph
}

TEST_CASE("resource-ref: a dropped asset of a type it takes is one write; a read-only one only opens; objects that differ are Mixed")
{
    Bed bed(2);
    bed.Add(u8"mesh", Guid{});
    bed.Add(u8"fixed", bed.wood).ReadOnly = true;
    bed.Add(u8"both", Guid{});
    bed.bag.SetValue(StringView(u8"both"), 1, gui::Variant(Text(bed.wood).AsView()));
    bed.Show();

    // A drop goes through the slot's drop target, the same write as a pick.
    gui::Node& node = *bed.Editor(u8"mesh").Slot();
    gui::EventDispatcher& d = *bed.root->GetEventDispatcher();
    RefPtr<gui::Panel> source = MakeRef<gui::Panel>(DefaultAllocator());
    bed.root->AddChild(source.Get());
    (void)bed.root->UpdateLayout();
    const gui::Rect at = node.GetScreenBounds();
    const Float2 over{at.x + 10.0f, at.y + at.height * 0.5f};
    gui::DragRequest request;
    request.Payload = AssetDrag::Payload(bed.wood, u8"MeshAsset", u8"Wood");
    d.InjectMouseMove(over);
    d.StartDrag(*source, Move(request));
    d.RequeryDrag();
    CHECK(d.GetDropReason() == StringView(u8"Use Wood"));
    d.InjectMouseUp(over, gui::MouseButton::Left);
    (void)bed.root->UpdateLayout();
    CHECK(bed.commits == 1);
    CHECK(bed.bag.GetValue(StringView(u8"mesh"), 1).AsString() == Text(bed.wood).AsView()); // each object

    editor::app::AssetPickerSlot& fixed = *bed.Editor(u8"fixed").Slot();
    CHECK(fixed.ShownName() == StringView(u8"Wood"));
    CHECK_FALSE(fixed.PickButton()->IsDisplayed());
    CHECK_FALSE(fixed.ClearButton()->IsDisplayed());
    CHECK(fixed.OpenButton()->IsDisplayed());

    editor::app::AssetPickerSlot& both = *bed.Editor(u8"both").Slot();
    CHECK(both.ShownName() == gui::MixedValueText());
    CHECK_FALSE(both.OpenButton()->IsDisplayed()); // which one?
    CHECK(both.ClearButton()->IsDisplayed());
}

TEST_CASE("resource-ref: the context's table asks the builders' join for the types and routes open and reveal")
{
    editor::EditorContext context{DefaultAllocator()};
    editor::EditorIcons icons(DefaultAllocator(), [](StringView) { return false; });
    context.SourceAssetTypesOf = [](const TypeInfo& product)
    {
        Array<const TypeInfo*> assets;
        if (&product == &TypeOf<TestMesh>())
            assets.PushBack(&TypeOf<int>()); // any type stands for an asset type here: its name is what is asked
        return assets;
    };
    Array<Guid> opened;
    Array<Guid> revealed;
    context.OpenAsset = [&opened](const Guid& id) { opened.PushBack(id); };
    context.RevealAsset = [&revealed](const Guid& id) { revealed.PushBack(id); };
    const ResourceRefServices services = ResourceRefServices::From(context, icons, {});
    const Array<String> types = services.AcceptedTypes(TypeOf<TestMesh>());
    REQUIRE(types.Size() == 1u);
    CHECK(types[0] == String(StringView(reinterpret_cast<const utf8char*>(TypeOf<int>().name))));
    const Array<String> itself = services.AcceptedTypes(TypeOf<float>()); // no builder makes it: a source type named itself
    REQUIRE(itself.Size() == 1u);
    CHECK(itself[0] == String(StringView(reinterpret_cast<const utf8char*>(TypeOf<float>().name))));
    ResourceRefShown shown;
    CHECK_FALSE(services.Describe(Guid{1u, 2u}, shown)); // no project: nothing is there
    CHECK_FALSE(static_cast<bool>(services.Pick));       // no picker to open: no pick
    services.Open(Guid{1u, 2u});
    services.Reveal(Guid{3u, 4u});
    REQUIRE(opened.Size() == 1u);
    CHECK(opened[0] == Guid{1u, 2u});
    REQUIRE(revealed.Size() == 1u);
    CHECK(revealed[0] == Guid{3u, 4u});
}

TEST_CASE("resource-ref: the context's pick asks the asset picker for the slot's types and value; a pick or None answers")
{
    std::error_code ec;
    std::filesystem::remove_all("resource_ref_project", ec);
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"resource_ref_project", u8"Refs").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), u8"resource_ref_project");
    REQUIRE(project);
    GlobalTypeRegistry().Register(RefMeshAsset::StaticType());
    GlobalTypeRegistry().Register(RefSoundAsset::StaticType());
    foundation::content::Group& top = *project->SourceDb().RootGroup();
    foundation::content::Instance* crate = top.CreateInstance(u8"Crate", RefMeshAsset::StaticType());
    foundation::content::Group* town = top.CreateGroup(u8"Town");
    REQUIRE(town != nullptr);
    foundation::content::Instance* barrel = town->CreateInstance(u8"Barrel", RefMeshAsset::StaticType());
    (void)town->CreateInstance(u8"Bell", RefSoundAsset::StaticType());
    REQUIRE(crate != nullptr);
    REQUIRE(barrel != nullptr);

    editor::EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    context.ToggleFavorite(barrel->Id());
    editor::EditorIcons icons(DefaultAllocator(), [](StringView) { return false; });
    editor::app::AssetPickRequest opened;
    const ResourceRefServices services = ResourceRefServices::From(context, icons, [&opened](editor::app::AssetPickRequest request) { opened = Move(request); });
    REQUIRE(static_cast<bool>(services.Pick));
    Array<Guid> chosen;
    const String meshes[] = {String(reinterpret_cast<const utf8char*>(RefMeshAsset::StaticType().name))};
    services.Pick(Span<const String>(meshes, 1u), crate->Id(), [&chosen](const Guid& id) { chosen.PushBack(id); });
    REQUIRE(opened.types.Size() == 1u); // the asset picker shows them (AssetPickerWindowTests)
    CHECK(opened.types[0] == meshes[0]);
    CHECK(opened.current == crate->Id());
    REQUIRE(static_cast<bool>(opened.chosen));
    opened.chosen(barrel->Id());
    opened.chosen(Guid{}); // None
    REQUIRE(chosen.Size() == 2u);
    CHECK(chosen[0] == barrel->Id());
    CHECK(chosen[1].IsNil());

    context.SetProject(nullptr);
    project.Reset();
    std::filesystem::remove_all("resource_ref_project", ec);
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Generic asset page tests (headless): the serialize-driven scan/patch machinery is pure over
// any ISerializable, so it is covered with a synthetic asset exercising scalars, strings,
// guids, blobs, arrays, a version-gated field and a value-conditional one. Then the page from its
// markup (Data/Editor/AssetForm.xml) over that asset in a project: its rows, an edit and its undo,
// a change of shape, Save, and an asset that does not read.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.generic;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    class FormProbeAsset final : public ISerializable
    {
        RTTI_OBJECT(FormProbeAsset, ISerializable)
    public:
        f32 friction = 0.5f;
        i32 group = 3;
        bool enabled = true;
        String note{u8"hello"};
        Guid mesh{0xAB, 0x12};
        Array<f32> weights; // unkeyed scalar array
        u8 blob[4] = {1, 2, 3, 4};
        bool hasExtra = false; // gates `extra` below (value-conditional shape)
        f32 extra = 9.0f;
        f32 gated = 7.0f; // behind ar.Version() >= 2 (the type's data version is 2)

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "friction", friction);
            foundation::core::Serialize(ar, "group", group);
            foundation::core::Serialize(ar, "enabled", enabled);
            foundation::core::Serialize(ar, "note", note);
            foundation::core::Serialize(ar, "mesh", mesh);
            foundation::core::Serialize(ar, "weights", weights);
            ar.Key("blob");
            ar.Blob(blob, sizeof(blob));
            foundation::core::Serialize(ar, "hasExtra", hasExtra);
            if (hasExtra)
            {
                foundation::core::Serialize(ar, "extra", extra);
            }
            if (ar.Version() >= 2)
            {
                foundation::core::Serialize(ar, "gated", gated);
            }
        }
    };
    REFLECT_MEMBERS(FormProbeAsset, "rtti::editor::editor::tests")
    {
        builder.DataVersion(2).Attribute("displayName", String(u8"Form Probe")); // the kind its page names
    }

    [[nodiscard]] i32 IndexOf(const Array<AssetFormField>& fields, StringView label)
    {
        for (usize i = 0; i < fields.Size(); ++i)
        {
            if (fields[i].label.AsView() == label)
            {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }
}

TEST_CASE("asset form: scan records every field kind, incl. version-gated + array elements")
{
    FormProbeAsset asset;
    asset.weights.PushBack(0.25f);
    asset.weights.PushBack(0.75f);

    Array<AssetFormField> fields;
    REQUIRE(ScanAssetForm(asset, fields).IsOk());

    const i32 friction = IndexOf(fields, u8"friction");
    REQUIRE(friction >= 0);
    CHECK(fields[static_cast<usize>(friction)].kind == AssetFormFieldKind::Scalar);
    CHECK(fields[static_cast<usize>(friction)].floatValue == doctest::Approx(0.5));

    const i32 note = IndexOf(fields, u8"note");
    REQUIRE(note >= 0);
    CHECK(fields[static_cast<usize>(note)].textValue.AsView() == u8"hello");

    const i32 mesh = IndexOf(fields, u8"mesh");
    REQUIRE(mesh >= 0);
    CHECK(fields[static_cast<usize>(mesh)].kind == AssetFormFieldKind::Guid);
    CHECK(fields[static_cast<usize>(mesh)].guidValue == Guid{0xAB, 0x12});

    // Unkeyed array elements label as weights[i].
    CHECK(IndexOf(fields, u8"weights[0]") >= 0);
    CHECK(IndexOf(fields, u8"weights[1]") >= 0);

    const i32 blob = IndexOf(fields, u8"blob");
    REQUIRE(blob >= 0);
    CHECK(fields[static_cast<usize>(blob)].kind == AssetFormFieldKind::Blob);
    CHECK(fields[static_cast<usize>(blob)].blobValue.Size() == 4u);
    CHECK_FALSE(fields[static_cast<usize>(blob)].Editable());

    // The version-gated field appears (the scan pushes the current data version).
    CHECK(IndexOf(fields, u8"gated") >= 0);
    // The value-conditional field does not (hasExtra is false).
    CHECK(IndexOf(fields, u8"extra") < 0);
}

TEST_CASE("asset form: a patch changes exactly the target field")
{
    FormProbeAsset asset;
    asset.weights.PushBack(0.25f);
    Array<AssetFormField> fields;
    REQUIRE(ScanAssetForm(asset, fields).IsOk());

    const i32 friction = IndexOf(fields, u8"friction");
    REQUIRE(friction >= 0);
    AssetFormField patch = fields[static_cast<usize>(friction)];
    patch.floatValue = 0.9;
    REQUIRE(ApplyAssetFormField(asset, fields, static_cast<usize>(friction), patch).IsOk());

    CHECK(asset.friction == doctest::Approx(0.9f)); // patched
    CHECK(asset.group == 3);                        // everything else untouched
    CHECK(asset.enabled);
    CHECK(asset.note.AsView() == u8"hello");
    CHECK(asset.mesh == Guid{0xAB, 0x12});
    REQUIRE(asset.weights.Size() == 1u);
    CHECK(asset.weights[0] == doctest::Approx(0.25f));
    CHECK(asset.gated == doctest::Approx(7.0f));
    CHECK(asset.blob[2] == 3);
}

TEST_CASE("asset form: a conditional-branch patch changes the shape (rescan detects it)")
{
    FormProbeAsset asset;
    Array<AssetFormField> fields;
    REQUIRE(ScanAssetForm(asset, fields).IsOk());
    const i32 hasExtra = IndexOf(fields, u8"hasExtra");
    REQUIRE(hasExtra >= 0);

    AssetFormField patch = fields[static_cast<usize>(hasExtra)];
    patch.intValue = 1; // true: Serialize then includes `extra`
    (void)ApplyAssetFormField(asset, fields, static_cast<usize>(hasExtra), patch);
    CHECK(asset.hasExtra);

    Array<AssetFormField> rescanned;
    REQUIRE(ScanAssetForm(asset, rescanned).IsOk());
    CHECK_FALSE(AssetFormShapeEquals(fields, rescanned)); // `extra` appeared
    CHECK(IndexOf(rescanned, u8"extra") >= 0);
}

TEST_CASE("asset form: the fallback factory routes ANY serializable; bespoke pages win")
{
    const editor::app::AssetPageServicesSource none; // no services, only the registration; declared first so it outlives the context
    editor::EditorContext context{DefaultAllocator()};
    RegisterGenericAssetEditor(context, none);
    // The probe type routes to the fallback via its ISerializable base.
    editor::IEditorPageFactory* found =
        context.Pages().FindFactory(FormProbeAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &ISerializable::StaticType());
}

// Fields inside an array carry the outermost array's key as their category, so the page groups
// them under a collapsed group and a large array costs nothing until opened; top-level fields
// carry none (they stay in the always-visible "Properties" section).
TEST_CASE("asset form: array elements carry the array key as their category")
{
    FormProbeAsset asset;
    asset.weights.PushBack(1.0f);
    asset.weights.PushBack(2.0f);
    Array<AssetFormField> fields;
    REQUIRE(ScanAssetForm(asset, fields).IsOk());

    const i32 top = IndexOf(fields, u8"friction");
    REQUIRE(top >= 0);
    CHECK(fields[static_cast<usize>(top)].category.IsEmpty()); // top-level: no category

    const i32 w0 = IndexOf(fields, u8"weights[0]");
    const i32 w1 = IndexOf(fields, u8"weights[1]");
    REQUIRE(w0 >= 0);
    REQUIRE(w1 >= 0);
    CHECK(fields[static_cast<usize>(w0)].category.AsView() == StringView(u8"weights"));
    CHECK(fields[static_cast<usize>(w1)].category.AsView() == StringView(u8"weights"));
}

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    // A project holding the probe ("Probe", two weights) and an asset with nothing written
    // ("Unread"), the page over either from its markup.
    struct Bed
    {
        String dir = String(u8"scratch_generic_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* probe = nullptr;
        foundation::content::Instance* unread = nullptr;

        Bed()
        {
            // The probe read back by its type, as an asset type registers itself.
            GlobalTypeRegistry().Register(FormProbeAsset::StaticType());
            RegisterSerializable<FormProbeAsset>();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            probe = top->CreateInstance(u8"Probe", FormProbeAsset::StaticType());
            unread = top->CreateInstance(u8"Unread", FormProbeAsset::StaticType());
            REQUIRE(probe != nullptr);
            REQUIRE(unread != nullptr);
            FormProbeAsset asset;
            asset.weights.PushBack(1.0f);
            asset.weights.PushBack(2.0f);
            REQUIRE(probe->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<GenericAssetEditorPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<GenericAssetEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };

    usize FieldIndex(const GenericAssetEditorPage& page, StringView label)
    {
        const Span<const AssetFormField> fields = page.Fields();
        for (usize i = 0; i < fields.Size(); ++i)
            if (fields[i].label.AsView() == label)
                return i;
        return fields.Size();
    }

    String PathOf(const GenericAssetEditorPage& page, StringView label)
    {
        return String(page.PathOf(FieldIndex(page, label)));
    }
}

TEST_CASE("generic page: the fields as rows by their keys, an array's folded under its key, a blob read only, a guid a reference")
{
    Bed bed;
    UniquePtr<GenericAssetEditorPage> page = bed.Open(*bed.probe);
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Content());
    REQUIRE(page->Grid() != nullptr);
    REQUIRE(page->Bag() != nullptr);
    CHECK(page->Grid()->GetSource() == page->Bag());
    auto* kind = app::FindById<gui::Label>(*page->Content(), u8"form-kind");
    REQUIRE(kind != nullptr);
    CHECK(kind->GetText() == StringView(u8"Form Probe")); // its display name, not its type's
    CHECK_FALSE(app::FindById<gui::Node>(*page->Content(), u8"form-state")->IsDisplayed());

    gui::PropertyBag& bag = *page->Bag();
    const String friction = PathOf(*page, u8"friction");
    REQUIRE_FALSE(friction.IsEmpty());
    CHECK(bag.FindInfo(friction.AsView())->Kind == gui::PropertyKind::Float);
    CHECK(bag.FindInfo(friction.AsView())->ShownName() == StringView(u8"Friction"));
    CHECK(bag.FindInfo(friction.AsView())->Description == StringView(u8"friction")); // the key as saved
    const String group = PathOf(*page, u8"group");
    CHECK(bag.FindInfo(group.AsView())->Kind == gui::PropertyKind::Integer);
    CHECK(bag.FindInfo(group.AsView())->HasRange);
    CHECK(bag.FindInfo(PathOf(*page, u8"enabled").AsView())->Kind == gui::PropertyKind::Bool);
    CHECK(bag.FindInfo(PathOf(*page, u8"note").AsView())->Kind == gui::PropertyKind::String);

    // The guid: one reference row, its id as text.
    const String mesh = PathOf(*page, u8"mesh");
    CHECK(bag.FindInfo(mesh.AsView())->Kind == gui::PropertyKind::Reference);
    CHECK_FALSE(bag.GetValue(mesh.AsView(), 0).AsString().IsEmpty());

    // The blob: its size, read only.
    const String blob = PathOf(*page, u8"blob");
    CHECK(bag.FindInfo(blob.AsView())->ReadOnly);
    CHECK(bag.GetValue(blob.AsView(), 0).AsString() == StringView(u8"4 bytes"));

    // The array: its elements under one group named by its key, folded; its count has no row.
    const String first = PathOf(*page, u8"weights[0]");
    const String second = PathOf(*page, u8"weights[1]");
    const StringView parent = gui::ParentPropertyPath(first.AsView());
    REQUIRE_FALSE(parent.IsEmpty());
    CHECK(gui::ParentPropertyPath(second.AsView()) == parent);
    CHECK(bag.FindInfo(parent)->Kind == gui::PropertyKind::Group);
    CHECK(bag.FindInfo(parent)->ShownName() == StringView(u8"Weights"));
    CHECK_FALSE(page->Grid()->IsOpen(parent));
    CHECK(page->PathOf(FieldIndex(*page, u8"weights")).IsEmpty());
}

TEST_CASE("generic page: an edit is one undo step, a flipped branch builds the rows again, and Save writes the asset")
{
    Bed bed;
    UniquePtr<GenericAssetEditorPage> page = bed.Open(*bed.probe);
    const String friction = PathOf(*page, u8"friction");
    page->Edit(friction.AsView(), gui::Variant(0.25));
    CHECK(page->IsDirty());
    CHECK(page->Fields()[FieldIndex(*page, u8"friction")].floatValue == doctest::Approx(0.25));
    page->Commands().Undo();
    page->Refresh();
    CHECK(page->Fields()[FieldIndex(*page, u8"friction")].floatValue == doctest::Approx(0.5));
    CHECK(page->Bag()->GetValue(friction.AsView(), 0).AsFloat() == doctest::Approx(0.5));
    page->Commands().Redo();
    page->Refresh();
    CHECK(page->Bag()->GetValue(friction.AsView(), 0).AsFloat() == doctest::Approx(0.25));

    // A guid that is not one leaves the field as it was.
    const String mesh = PathOf(*page, u8"mesh");
    const String before(page->Bag()->GetValue(mesh.AsView(), 0).AsString());
    page->Edit(mesh.AsView(), gui::Variant(StringView(u8"not a guid")));
    page->Refresh();
    CHECK(page->Bag()->GetValue(mesh.AsView(), 0).AsString() == before.AsView());

    // hasExtra on: Serialize writes `extra` now, so the rows are built again with it.
    CHECK(page->PathOf(FieldIndex(*page, u8"extra")).IsEmpty());
    page->Edit(PathOf(*page, u8"hasExtra").AsView(), gui::Variant(true));
    page->Refresh();
    CHECK_FALSE(PathOf(*page, u8"extra").IsEmpty());
    CHECK(page->Grid()->GetSource() == page->Bag());

    REQUIRE(page->Save().IsOk());
    CHECK_FALSE(page->IsDirty());
    RefPtr<ISerializable> saved = bed.probe->ReadObject();
    REQUIRE(saved.Get() != nullptr);
    auto* probe = Cast<FormProbeAsset>(saved.Get());
    REQUIRE(probe != nullptr);
    CHECK(probe->friction == doctest::Approx(0.25f));
    CHECK(probe->hasExtra);
}

TEST_CASE("generic page: an asset that does not read says so in place of the rows")
{
    Bed bed;
    UniquePtr<GenericAssetEditorPage> page = bed.Open(*bed.unread);
    CHECK(page->LoadFailed());
    REQUIRE(page->Content());
    CHECK(app::FindById<gui::Node>(*page->Content(), u8"form-state")->IsDisplayed());
    CHECK_FALSE(page->Grid()->IsDisplayed());
    CHECK(page->Fields().IsEmpty());
    CHECK_FALSE(page->Save().IsOk());
}

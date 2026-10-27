// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// MaterialEditorPage tests: which row a property gets is a pure function of its declared type;
// then the page from the preview pages' markup over a PBR material in a project, without a host
// (no preview): its rows laid out from its shader's table (the material's, its properties, its
// textures), an edit one undo step, and the preview's shape kept per material.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.materials;
import foundation.materials.resource; // MaterialSource (what the tests read back)
import foundation.texture.resource;
import materials.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.materials;

using namespace foundation::core;
namespace materials = foundation::materials;
using editor::MaterialPropertyRow;
using editor::MaterialPropertyRowFor;

TEST_CASE("material page: only a declared colour gets a colour picker")
{
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Color) ==
          MaterialPropertyRow::Color);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::ColorHdr) ==
          MaterialPropertyRow::ColorWithIntensity);
    // A Float4 that is not declared a colour is four numbers, not a swatch.
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float4) ==
          MaterialPropertyRow::Numbers4);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float) ==
          MaterialPropertyRow::Number);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Texture2D) ==
          MaterialPropertyRow::Texture);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Sampler) ==
          MaterialPropertyRow::None);
}

// Every value a shader's properties block can declare has a row of its own size: a custom shader's
// float2, float3 and int are edited as what they are, not as four floats.
TEST_CASE("material page: a shader's float2, float3 and int get rows of their own")
{
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float2) ==
          MaterialPropertyRow::Numbers2);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float3) ==
          MaterialPropertyRow::Numbers3);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Int) ==
          MaterialPropertyRow::Integer);

}

TEST_CASE("material page: the builtin PBR template's colours land on colour rows")
{
    RefPtr<materials::Material> pbr = materials::BuiltinMaterialTemplate(u8"forward");
    REQUIRE(pbr);
    CHECK(MaterialPropertyRowFor(pbr->FindProperty(u8"BaseColor")->type) ==
          MaterialPropertyRow::Color);
    CHECK(MaterialPropertyRowFor(pbr->FindProperty(u8"EmissiveColor")->type) ==
          MaterialPropertyRow::ColorWithIntensity);
}

namespace
{
    namespace gui = foundation::gui;

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

    // A float property's value in the material's source (what the preview material is built from).
    f32 SourceFloat(const materials::MaterialSource& s, StringView name)
    {
        for (usize i = 0; i < s.propertyNames.Size(); ++i)
            if (s.propertyNames[i].AsView() == name && i < s.propertyOffsets.Size() && s.propertyOffsets[i] + sizeof(f32) <= s.uniformDefaults.Size())
            {
                f32 v = 0.0f;
                MemCopy(&v, s.uniformDefaults.Data() + s.propertyOffsets[i], sizeof(v));
                return v;
            }
        return -1.0f;
    }

    // A project with a PBR material ("Wood") and its editor settings.
    struct Bed
    {
        String dir = String(u8"scratch_material_page_project");
        UniquePtr<editor::EditorProject> project;
        foundation::settings::Settings store{DefaultAllocator()};
        editor::app::AssetPageServicesSource none; // the registration's: no services, outliving the context below
        editor::EditorContext context{DefaultAllocator()};
        foundation::content::Instance* wood = nullptr;

        Bed()
        {
            editor::RegisterMaterialEditor(context, none); // the asset's types and the prefs' section
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            wood = project->SourceDb().RootGroup()->CreateInstance(u8"Wood", pipeline::MaterialAsset::StaticType());
            REQUIRE(wood != nullptr);
            pipeline::MaterialAsset asset;
            pipeline::SetMaterialBuiltInShader(asset.source, false);
            REQUIRE(wood->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<editor::MaterialEditorPage> Open()
        {
            editor::app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<editor::MaterialEditorPage>(DefaultAllocator(), context, Move(services), *wood);
        }
    };
}

TEST_CASE("material page: its rows are its shader's table: the material's, each property by its type, each texture a slot")
{
    Bed bed;
    UniquePtr<editor::MaterialEditorPage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Source() != nullptr);
    CHECK(page->Sections().SectionCount() == 5u); // notices, the material, properties, textures, facts (no reflected rows)

    gui::PropertyBag& material = page->MaterialRows();
    REQUIRE(material.FindInfo(u8"shader") != nullptr);
    CHECK(material.FindInfo(u8"shader")->Kind == gui::PropertyKind::Reference);
    CHECK(material.FindInfo(u8"builtIn")->Visible); // on no shader asset: a built-in
    CHECK(material.GetValue(u8"builtIn", 0).AsInt() == 0); // PBR
    CHECK(material.FindInfo(u8"blend")->Choices.Size() == 6u);

    gui::PropertyBag& properties = page->PropertyRows();
    REQUIRE(properties.FindInfo(u8"BaseColor") != nullptr);
    CHECK(properties.FindInfo(u8"BaseColor")->Kind == gui::PropertyKind::Color);
    REQUIRE(properties.FindInfo(u8"EmissiveColor") != nullptr);
    CHECK(properties.FindInfo(u8"EmissiveColor")->Hint == StringView(u8"ColorHdr"));
    CHECK(properties.FindInfo(u8"EmissiveColor.intensity") != nullptr);
    REQUIRE(properties.FindInfo(u8"Metallic") != nullptr);
    CHECK(properties.FindInfo(u8"Metallic")->HasRange);
    CHECK(properties.FindInfo(u8"Metallic")->Max == doctest::Approx(1.0));
    CHECK(properties.FindInfo(u8"Metallic")->Hint == StringView(u8"slider")); // a slider over its range

    Array<gui::PropertyInfo> textures;
    page->TextureRows().GetProperties(StringView{}, textures);
    REQUIRE_FALSE(textures.IsEmpty());
    for (const gui::PropertyInfo& texture : textures)
    {
        CHECK(texture.Kind == gui::PropertyKind::Reference);
        CHECK(texture.ReferenceType == &foundation::texture::Texture::StaticType()); // a texture or a render texture
    }
}

TEST_CASE("material page: a built-in switched lays the rows out as its own, and its undo as they were")
{
    Bed bed;
    UniquePtr<editor::MaterialEditorPage> page = bed.Open();
    REQUIRE(page->PropertyRows().FindInfo(u8"Metallic") != nullptr);
    page->EditRow(page->MaterialRows(), u8"builtIn", gui::Variant(static_cast<i64>(1))); // Unlit
    page->Refresh();
    CHECK(page->Source()->shaderName == StringView(u8"unlit"));
    CHECK(page->PropertyRows().FindInfo(u8"Metallic") == nullptr); // PBR's rows gone with it
    CHECK(page->PropertyRows().FindInfo(u8"BaseColor") != nullptr);
    page->Commands().Undo();
    page->Refresh();
    CHECK(page->PropertyRows().FindInfo(u8"Metallic") != nullptr);
}

TEST_CASE("material page: an edit is one undo step, the row following, Discard takes them all back; the preview's shape or mesh is kept per material")
{
    Bed bed;
    UniquePtr<editor::MaterialEditorPage> page = bed.Open();
    gui::PropertyBag& properties = page->PropertyRows();
    const f64 before = properties.GetValue(u8"Metallic", 0).AsFloat();
    page->EditRow(properties, u8"Metallic", gui::Variant(0.75));
    page->Refresh();
    CHECK(page->IsDirty());
    CHECK(page->PropertyRows().GetValue(u8"Metallic", 0).AsFloat() == doctest::Approx(0.75));
    page->Commands().Undo();
    page->Refresh();
    CHECK(page->PropertyRows().GetValue(u8"Metallic", 0).AsFloat() == doctest::Approx(before));

    // A colour's part and the pipeline state, each one step.
    page->EditRow(page->MaterialRows(), u8"cull", gui::Variant(static_cast<i64>(0)));
    page->Refresh();
    CHECK(page->MaterialRows().GetValue(u8"cull", 0).AsInt() == 0);
    page->Commands().Undo();
    page->Refresh();
    CHECK(page->MaterialRows().GetValue(u8"cull", 0).AsInt() != 0);

    // Discard Changes (the bar's, as the old toolbar had it) takes every unsaved edit back.
    page->EditRow(properties, u8"Metallic", gui::Variant(0.25));
    page->EditRow(page->MaterialRows(), u8"cull", gui::Variant(static_cast<i64>(0)));
    page->DiscardChanges();
    page->Refresh();
    CHECK_FALSE(page->IsDirty());
    CHECK_FALSE(page->Commands().CanUndo());
    CHECK(page->PropertyRows().GetValue(u8"Metallic", 0).AsFloat() == doctest::Approx(before));
    CHECK(page->MaterialRows().GetValue(u8"cull", 0).AsInt() != 0);

    page->SetPreviewShape(2); // the plane
    page.Reset();
    UniquePtr<editor::MaterialEditorPage> again = bed.Open();
    CHECK(again->PreviewShape() == 2u);

    // A mesh picked is kept too: the page showing the kept choice on its bar (the shape box set
    // from code) is no shape picked, which would put the mesh away.
    const Guid mesh{0x1234u, 0x5678u};
    again->SetPreviewMesh(mesh);
    again.Reset();
    UniquePtr<editor::MaterialEditorPage> third = bed.Open();
    CHECK(third->PreviewMesh() == mesh);
    third->Refresh(); // the bar shown again
    third.Reset();
    CHECK(bed.Open()->PreviewMesh() == mesh);
}

TEST_CASE("material page: a gesture's previews reach the source as they go, one undo step at its commit; a cancel takes them back")
{
    Bed bed;
    UniquePtr<editor::MaterialEditorPage> page = bed.Open();
    gui::PropertyBag& properties = page->PropertyRows();
    const f32 before = SourceFloat(*page->Source(), u8"Roughness");
    // As a slider's drag: begun, previewed twice, committed.
    properties.BeginEdit(u8"Roughness");
    properties.PreviewEdit(u8"Roughness", gui::PropertyWrite{gui::Variant(0.3)});
    CHECK(SourceFloat(*page->Source(), u8"Roughness") == doctest::Approx(0.3f)); // live: the preview built from it
    properties.PreviewEdit(u8"Roughness", gui::PropertyWrite{gui::Variant(0.4)});
    CHECK(SourceFloat(*page->Source(), u8"Roughness") == doctest::Approx(0.4f));
    CHECK_FALSE(page->Commands().CanUndo()); // no step while it goes
    properties.CommitEdit(u8"Roughness", gui::PropertyWrite{gui::Variant(0.4)});
    CHECK(SourceFloat(*page->Source(), u8"Roughness") == doctest::Approx(0.4f));
    page->Commands().Undo(); // the gesture's one step, back to before it
    CHECK(SourceFloat(*page->Source(), u8"Roughness") == doctest::Approx(before));
    CHECK_FALSE(page->Commands().CanUndo());

    // Given up with Escape: the source as it was.
    properties.BeginEdit(u8"Roughness");
    properties.PreviewEdit(u8"Roughness", gui::PropertyWrite{gui::Variant(0.9)});
    CHECK(SourceFloat(*page->Source(), u8"Roughness") == doctest::Approx(0.9f));
    properties.CancelEdit(u8"Roughness");
    CHECK(SourceFloat(*page->Source(), u8"Roughness") == doctest::Approx(before));
    CHECK_FALSE(page->Commands().CanUndo());
}

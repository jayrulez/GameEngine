// Generic asset page tests (headless): the serialize-driven scan/patch machinery is pure over
// any ISerializable, so it is covered with a synthetic asset exercising scalars, strings,
// guids, blobs, arrays, and a version-gated + a value-conditional field. The grid wiring needs
// a live harness (editor app).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.editor.core;
import draconic.editor.generic;

using namespace draconic::foundation;
using namespace draconic::editor;

namespace
{
    class FormProbeAsset final : public ISerializable
    {
        DRACONIC_OBJECT(FormProbeAsset, ISerializable)
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
            draconic::foundation::Serialize(ar, "friction", friction);
            draconic::foundation::Serialize(ar, "group", group);
            draconic::foundation::Serialize(ar, "enabled", enabled);
            draconic::foundation::Serialize(ar, "note", note);
            draconic::foundation::Serialize(ar, "mesh", mesh);
            draconic::foundation::Serialize(ar, "weights", weights);
            ar.Key("blob");
            ar.Blob(blob, sizeof(blob));
            draconic::foundation::Serialize(ar, "hasExtra", hasExtra);
            if (hasExtra)
            {
                draconic::foundation::Serialize(ar, "extra", extra);
            }
            if (ar.Version() >= 2)
            {
                draconic::foundation::Serialize(ar, "gated", gated);
            }
        }
    };
    DRACONIC_DEFINE_OBJECT_VERSIONED(FormProbeAsset, "draconic::editor::tests", 2)

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

    // The version-gated field appears (the scan pushes the CURRENT data version).
    CHECK(IndexOf(fields, u8"gated") >= 0);
    // The value-conditional field does NOT (hasExtra is false).
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
    patch.intValue = 1; // true -> Serialize now includes `extra`
    (void)ApplyAssetFormField(asset, fields, static_cast<usize>(hasExtra), patch);
    CHECK(asset.hasExtra);

    Array<AssetFormField> rescanned;
    REQUIRE(ScanAssetForm(asset, rescanned).IsOk());
    CHECK_FALSE(AssetFormShapeEquals(fields, rescanned)); // `extra` appeared
    CHECK(IndexOf(rescanned, u8"extra") >= 0);
}

TEST_CASE("asset form: the fallback factory routes ANY serializable; bespoke pages win")
{
    draconic::editor::EditorContext context;
    RegisterGenericAssetEditor(context);
    // The probe type routes to the fallback via its ISerializable base.
    draconic::editor::IEditorPageFactory* found =
        context.Pages().FindFactory(FormProbeAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &ISerializable::StaticType());
}

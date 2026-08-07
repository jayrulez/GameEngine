// ImageEditorPage tests (headless): factory type-dispatch + the asset blob round-trip the
// page's undo snapshots ride. The preview + grid need a live harness (editor app).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.image;
import draconic.image.editor;
import draconic.vfs;
import draconic.editor.image;
import draconic.editor.core;

using namespace draconic::foundation;
namespace image = draconic::image;

TEST_CASE("ImageEditorPageFactory reports the ImageAsset primary type")
{
    draconic::editor::ImageEditorPageFactory factory;
    CHECK(factory.PrimaryType() == &image::ImageAsset::StaticType());
}

TEST_CASE("ImageEditor registers a factory the registry routes for ImageAsset")
{
    draconic::editor::EditorContext context;
    draconic::editor::RegisterImageEditor(context);

    draconic::editor::IEditorPageFactory* found =
        context.Pages().FindFactory(image::ImageAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &image::ImageAsset::StaticType());
}

TEST_CASE("ImageAsset blob snapshot round-trips fileName + color space (undo path)")
{
    image::ImageAsset a;
    a.fileName = draconic::vfs::SourcePath(u8"Textures/rock.png");
    a.colorSpace = image::ImageColorSpace::Linear;

    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        a.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    image::ImageAsset b;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        b.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(b.fileName.View() == u8"Textures/rock.png");
    CHECK(b.colorSpace == image::ImageColorSpace::Linear);
}

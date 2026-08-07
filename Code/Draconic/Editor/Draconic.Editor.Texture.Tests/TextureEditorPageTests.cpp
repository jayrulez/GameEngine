// TextureEditorPage tests (headless): the factory routes TextureAsset through the page
// registry (nearest-type dispatch), and the whole-asset blob snapshot the page's undo
// commands ride round-trips every import setting. The page's widget tree needs a live UI
// context, so it is exercised in the editor app, not here.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.image;
import draconic.texture;
import draconic.texture.editor;
import draconic.editor.core;
import draconic.editor.texture;

using namespace draconic::foundation;
namespace texture = draconic::texture;
namespace image = draconic::image;

TEST_CASE("TextureEditorPageFactory reports the TextureAsset primary type")
{
    draconic::editor::TextureEditorPageFactory factory;
    CHECK(factory.PrimaryType() == &texture::TextureAsset::StaticType());
}

TEST_CASE("TextureEditor registers a factory that the registry routes for TextureAsset")
{
    draconic::editor::EditorContext context;
    draconic::editor::RegisterTextureEditor(context);

    draconic::editor::IEditorPageFactory* found =
        context.Pages().FindFactory(texture::TextureAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &texture::TextureAsset::StaticType());
}

TEST_CASE("TextureAsset blob snapshot round-trips every import setting")
{
    // The exact path TextureEditorPage::Snapshot/ApplyBlob use for undo.
    texture::TextureAsset original;
    original.fileName = draconic::vfs::SourcePath(u8"Textures/brick.png");
    original.colorSpace = image::ImageColorSpace::Linear;
    original.shape = texture::TextureShape::Cubemap;
    original.minFilter = texture::TextureFilter::MipmapNearest;
    original.magFilter = texture::TextureFilter::Nearest;
    original.wrapU = texture::TextureWrap::MirroredRepeat;
    original.wrapV = texture::TextureWrap::ClampToBorder;
    original.wrapW = texture::TextureWrap::ClampToEdge;
    original.generateMipmaps = false;
    original.anisotropy = 8.0f;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        original.Serialize(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    texture::TextureAsset restored;
    BinarySerializer reader(stream, SerializeMode::Read);
    restored.Serialize(reader);

    CHECK(restored.fileName.View() == original.fileName.View());
    CHECK(restored.colorSpace == image::ImageColorSpace::Linear);
    CHECK(restored.shape == texture::TextureShape::Cubemap);
    CHECK(restored.minFilter == texture::TextureFilter::MipmapNearest);
    CHECK(restored.magFilter == texture::TextureFilter::Nearest);
    CHECK(restored.wrapU == texture::TextureWrap::MirroredRepeat);
    CHECK(restored.wrapV == texture::TextureWrap::ClampToBorder);
    CHECK(restored.wrapW == texture::TextureWrap::ClampToEdge);
    CHECK(restored.generateMipmaps == false);
    CHECK(restored.anisotropy == doctest::Approx(8.0f));
}

TEST_CASE("TextureAsset presets move the sampler settings a page preset row would apply")
{
    texture::TextureAsset asset;
    asset.SetupFor3D();
    CHECK(asset.generateMipmaps == true);
    CHECK(asset.anisotropy == doctest::Approx(16.0f));
    CHECK(asset.wrapU == texture::TextureWrap::Repeat);

    asset.SetupForUI();
    CHECK(asset.generateMipmaps == false);
    CHECK(asset.wrapU == texture::TextureWrap::ClampToEdge);
    CHECK(asset.minFilter == texture::TextureFilter::Linear);

    asset.SetupForSprite();
    CHECK(asset.minFilter == texture::TextureFilter::Nearest);
    CHECK(asset.magFilter == texture::TextureFilter::Nearest);
}

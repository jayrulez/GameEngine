// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// TextureEditorPage tests: the factory routes TextureAsset through the page registry
// (nearest-type dispatch), the asset's serialized state round-trips every import setting (what a
// preset's undo step keeps), the presets and the cook policy; then the page from the image pages'
// markup over a texture in a project: a preset from its bar's menu one undo step, Usage setting the
// colour space with it, the notice when they disagree, and its facts.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.image;
import foundation.texture;
import texture.pipeline;
import texture.compression;
import foundation.rhi;
import foundation.content;
import foundation.image.io;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.texture;

using namespace foundation::core;
namespace texture = foundation::texture;
namespace image = foundation::image;

TEST_CASE("TextureEditor registers a factory that the registry routes for TextureAsset")
{
    const editor::app::AssetPageServicesSource none; // no services: the registration only, before the context it outlives
    editor::EditorContext context{DefaultAllocator()};
    editor::RegisterTextureEditor(context, none);

    editor::IEditorPageFactory* found =
        context.Pages().FindFactory(pipeline::TextureAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::TextureAsset::StaticType());
}

TEST_CASE("TextureAsset blob snapshot round-trips every import setting")
{
    // The exact path TextureEditorPage::Snapshot/ApplyBlob use for undo.
    pipeline::TextureAsset original;
    original.fileName = foundation::vfs::SourcePath(u8"Textures/brick.png");
    original.colorSpace = image::ImageColorSpace::Linear;
    original.shape = texture::TextureShape::Cubemap;
    original.minFilter = texture::TextureFilter::MipmapNearest;
    original.magFilter = texture::TextureFilter::Nearest;
    original.wrapU = texture::TextureWrap::MirroredRepeat;
    original.wrapV = texture::TextureWrap::ClampToBorder;
    original.wrapW = texture::TextureWrap::ClampToEdge;
    original.generateMipmaps = false;
    original.anisotropy = 8.0f;
    original.usage = texcomp::TextureUsage::Normal;
    original.compression = texcomp::CompressionChoice::Quality;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        // The page snapshots under the VERSIONED scope (TextureAsset gates its v2 fields on
        // ar.Version(); a scope-less serialize is v0 and drops them by design - that is the
        // pre-variants-envelope migration fix).
        BeginVersionedPayload(writer, pipeline::TextureAsset::StaticType());
        original.Serialize(writer);
        EndVersionedPayload(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    pipeline::TextureAsset restored;
    BinarySerializer reader(stream, SerializeMode::Read);
    BeginVersionedPayload(reader, pipeline::TextureAsset::StaticType());
    restored.Serialize(reader);
    EndVersionedPayload(reader);

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
    CHECK(restored.usage == texcomp::TextureUsage::Normal);
    CHECK(restored.compression == texcomp::CompressionChoice::Quality);
}

TEST_CASE("TextureAsset presets move the sampler settings a page preset row would apply")
{
    pipeline::TextureAsset asset;
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

TEST_CASE("the Cooks-to row's policy expectations (desktop profile)")
{
    // The page renders ResolveCompressedFormat verbatim; pin the spec's two anchor cases plus
    // the usage-driven families the lint/derivation flow leads authors into.
    using texcomp::ResolveCompressedFormat;
    const auto desktop = texcomp::DesktopProfile();
    const auto rgba8 = foundation::rhi::TextureFormat::RGBA8Unorm;

    // Color with alpha -> BC7 sRGB.
    CHECK(ResolveCompressedFormat(texcomp::TextureUsage::Color, true, true, false,
                                  texcomp::CompressionChoice::Default, 512, 512, desktop,
                                  rgba8) == foundation::rhi::TextureFormat::BC7RGBAUnormSrgb);
    // Compression = None -> uncompressed passthrough.
    CHECK(ResolveCompressedFormat(texcomp::TextureUsage::Color, true, true, false,
                                  texcomp::CompressionChoice::None, 512, 512, desktop,
                                  rgba8) == rgba8);
    // The derived pairs: Normal -> BC7-LINEAR (never BC5 - the shaders decode rgb * 2 - 1),
    // gray Mask -> BC4, multichannel (ORM-shaped) Mask -> BC7-linear.
    CHECK(ResolveCompressedFormat(texcomp::TextureUsage::Normal, false, false, false,
                                  texcomp::CompressionChoice::Default, 512, 512, desktop,
                                  rgba8) == foundation::rhi::TextureFormat::BC7RGBAUnorm);
    CHECK(ResolveCompressedFormat(texcomp::TextureUsage::Mask, false, false, false,
                                  texcomp::CompressionChoice::Default, 512, 512, desktop,
                                  rgba8) == foundation::rhi::TextureFormat::BC4RUnorm);
    CHECK(ResolveCompressedFormat(texcomp::TextureUsage::Mask, false, false, true,
                                  texcomp::CompressionChoice::Default, 512, 512, desktop,
                                  rgba8) == foundation::rhi::TextureFormat::BC7RGBAUnorm);
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

    // A project with a 4x4 opaque picture ("bricks.png") under a colour texture ("Bricks").
    struct Bed
    {
        String dir = String(u8"scratch_texture_page_project");
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        foundation::content::Instance* bricks = nullptr;

        Bed()
        {
            pipeline::RegisterTextureAsset(); // read back by its type, as the editor registers it
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            image::Image picture(4, 4, image::PixelFormat::RGBA8);
            Span<u8> pixels = picture.PixelDataMut();
            for (usize i = 0; i < pixels.Size(); ++i)
                pixels[i] = (i % 4) == 3 ? 255 : static_cast<u8>(i * 7);
            REQUIRE(image::io::SaveImage(picture, PathJoin(project->SourcesRoot().AsView(), u8"bricks.png").AsView(), image::io::ImageFileFormat::PNG).IsOk());
            bricks = project->SourceDb().RootGroup()->CreateInstance(u8"Bricks", pipeline::TextureAsset::StaticType());
            REQUIRE(bricks != nullptr);
            pipeline::TextureAsset asset;
            asset.fileName = foundation::vfs::SourcePath(u8"bricks.png");
            asset.SetupFor3D();
            REQUIRE(bricks->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<editor::TextureEditorPage> Open()
        {
            editor::app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<editor::TextureEditorPage>(DefaultAllocator(), context, Move(services), *bricks);
        }
    };
}

TEST_CASE("texture page: a preset from the bar's menu is one undo step however many settings it moves")
{
    Bed bed;
    UniquePtr<editor::TextureEditorPage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->PresetButton() != nullptr);
    CHECK(page->PresetButton()->GetMenu().ItemCount() == editor::TextureEditorPage::Presets().Size());
    auto* asset = Cast<pipeline::TextureAsset>(page->Object());
    REQUIRE(asset != nullptr);
    CHECK(asset->generateMipmaps);
    CHECK(asset->wrapU == texture::TextureWrap::Repeat);

    REQUIRE(page->ApplyPreset(u8"UI"));
    CHECK_FALSE(asset->generateMipmaps);
    CHECK(asset->wrapU == texture::TextureWrap::ClampToEdge);
    CHECK(page->IsDirty());
    CHECK(page->Fact(u8"Mip Levels") == StringView(u8"1"));
    page->Commands().Undo();
    CHECK(asset->generateMipmaps);
    CHECK(asset->wrapU == texture::TextureWrap::Repeat);
    CHECK(page->Fact(u8"Mip Levels") == StringView(u8"3")); // 4x4, 2x2, 1x1
    CHECK_FALSE(page->Commands().CanUndo());
    CHECK_FALSE(page->ApplyPreset(u8"No Such Preset"));
}

TEST_CASE("texture page: Usage sets the colour space in the same step; a texture whose two disagree says so")
{
    Bed bed;
    UniquePtr<editor::TextureEditorPage> page = bed.Open();
    auto* asset = Cast<pipeline::TextureAsset>(page->Object());
    REQUIRE(asset != nullptr);
    CHECK(asset->colorSpace == image::ImageColorSpace::Srgb);
    CHECK(page->Notice(u8"colorSpace").IsEmpty());

    gui::PropertyWrite usage;
    usage.Value = gui::Variant(static_cast<i64>(texcomp::TextureUsage::Normal));
    page->Edit(u8"usage", usage);
    CHECK(asset->usage == texcomp::TextureUsage::Normal);
    CHECK(asset->colorSpace == image::ImageColorSpace::Linear);
    CHECK(page->Notice(u8"colorSpace").IsEmpty());

    // The colour space overridden to sRGB on a normal map: the notice says why not.
    gui::PropertyWrite colorSpace;
    colorSpace.Value = gui::Variant(static_cast<i64>(image::ImageColorSpace::Srgb));
    page->Edit(u8"colorSpace", colorSpace);
    CHECK_FALSE(page->Notice(u8"colorSpace").IsEmpty());

    // Each was one step: two undos put both back.
    page->Commands().Undo();
    CHECK(asset->colorSpace == image::ImageColorSpace::Linear);
    CHECK(page->Notice(u8"colorSpace").IsEmpty());
    page->Commands().Undo();
    CHECK(asset->usage == texcomp::TextureUsage::Color);
    CHECK(asset->colorSpace == image::ImageColorSpace::Srgb);
    CHECK_FALSE(page->Commands().CanUndo());

    CHECK(page->SourceFile() == StringView(u8"bricks.png"));
    CHECK(page->Fact(u8"Size") == StringView(u8"4 x 4  (LDR)"));
    CHECK_FALSE(page->Fact(u8"Cooks To").IsEmpty());
}

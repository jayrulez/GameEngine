// Draconic::TextureEditor - the `draconic.texture.editor` module (tooling).
//
// Source-side texture authoring + cook:
//   * TextureAsset (editor::Asset): references an image file + the GPU-texture
//     intent (color space, shape, sampler state). Presets mirror Sedulous's.
//   * TextureAssetBuilder (DefaultAssetBuilder): cooks a TextureAsset into a
//     runtime TextureResource - decode the file, resolve the RHI format from the
//     pixel format + color space, write the cooked record + "data" pixel stream.
//   * TextureImporter: an authoring helper that produces a TextureAsset for an
//     image file with a sensible preset (2D / equirectangular sky).
//
// Never linked by the runtime (an authoring/cook-seam helper). Cubemap import loads + validates +
// combines 6 face files; cooking a cubemap TextureResource through the builder is still deferred.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <initializer_list>

export module draconic.texture.editor;

import draconic.foundation;
import draconic.editor;
import draconic.editor.core;
import draconic.rhi;
import draconic.texture;
import draconic.texture.resource;
import draconic.image;
import draconic.image.io;
import draconic.content;

using namespace draconic::foundation;

export namespace draconic::texture
{
    namespace image = draconic::image;
    namespace content = draconic::content;

    // Source asset: an image file + how it should become a GPU texture.
    class TextureAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(TextureAsset, draconic::editor::Asset)
    public:
        image::ImageColorSpace colorSpace = image::ImageColorSpace::Srgb;
        // Embedded mode (model imports): fileName empty + width/height set; the RGBA8 pixels
        // live in the source instance's "pixels" data stream instead of an external file.
        u32 embeddedWidth = 0;
        u32 embeddedHeight = 0;
        TextureShape shape = TextureShape::Texture2D;
        TextureFilter minFilter = TextureFilter::Linear;
        TextureFilter magFilter = TextureFilter::Linear;
        TextureWrap wrapU = TextureWrap::Repeat;
        TextureWrap wrapV = TextureWrap::Repeat;
        TextureWrap wrapW = TextureWrap::Repeat;
        bool generateMipmaps = true;
        f32 anisotropy = 1.0f;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName
            draconic::foundation::Serialize(ar, "colorSpace", colorSpace);
            draconic::foundation::Serialize(ar, "embeddedWidth", embeddedWidth);
            draconic::foundation::Serialize(ar, "embeddedHeight", embeddedHeight);
            draconic::foundation::Serialize(ar, "shape", shape);
            draconic::foundation::Serialize(ar, "minFilter", minFilter);
            draconic::foundation::Serialize(ar, "magFilter", magFilter);
            draconic::foundation::Serialize(ar, "wrapU", wrapU);
            draconic::foundation::Serialize(ar, "wrapV", wrapV);
            draconic::foundation::Serialize(ar, "wrapW", wrapW);
            draconic::foundation::Serialize(ar, "generateMipmaps", generateMipmaps);
            draconic::foundation::Serialize(ar, "anisotropy", anisotropy);
        }

        // Presets (subset of Sedulous's).
        void SetupForUI()
        {
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::Linear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
        void SetupForSprite()
        {
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::Nearest;
            magFilter = TextureFilter::Nearest;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
        void SetupFor3D()
        {
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::MipmapLinear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::Repeat;
            wrapV = TextureWrap::Repeat;
            generateMipmaps = true;
            anisotropy = 16.0f;
        }
        void SetupForEquirectangularSkybox()
        {
            colorSpace = image::ImageColorSpace::Linear;
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::Linear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            wrapW = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
        void SetupForCubemapSkybox()
        {
            shape = TextureShape::Cubemap;
            minFilter = TextureFilter::Linear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            wrapW = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
    };

    // Authoring helper: configure a TextureAsset for an image file with a preset.
    // (Asset is a non-copyable Object, so the result is filled in place.)
    class TextureImporter
    {
    public:
        // A standard 2D texture (3D preset: mips + anisotropy).
        static void Import2D(StringView path, image::ImageColorSpace colorSpace,
                             TextureAsset& outAsset)
        {
            outAsset.fileName = draconic::vfs::SourcePath(path);
            outAsset.SetupFor3D();
            outAsset.colorSpace = colorSpace;
        }

        // An HDR equirectangular sky (linear, clamped, no mips).
        static void ImportEquirectangular(StringView path, TextureAsset& outAsset)
        {
            outAsset.fileName = draconic::vfs::SourcePath(path);
            outAsset.SetupForEquirectangularSkybox();
        }

        // A cubemap sky from 6 face files (the first is stored as the asset source).
        static void ImportCubemap(StringView firstFacePath, TextureAsset& outAsset)
        {
            outAsset.fileName = draconic::vfs::SourcePath(firstFacePath);
            outAsset.SetupForCubemapSkybox();
        }

        // Load 6 cubemap faces into one combined buffer, faces concatenated in +X,-X,+Y,-Y,+Z,-Z order
        // (the layout a 6-layer cube texture expects). All faces must be square, the same size, and the
        // same format; the caller supplies the explicit paths. `outPixels` = 6 * faceSize*faceSize*bpp.
        [[nodiscard]] static Status LoadCubemap(Span<const StringView> facePaths,
                                                Array<u8>& outPixels, u32& outFaceSize)
        {
            if (facePaths.Size() != 6)
            {
                return ErrorCode::InvalidArgument;
            }
            image::Image faces[6];
            u32 faceSize = 0;
            usize faceBytes = 0;
            for (usize i = 0; i < 6; ++i)
            {
                if (!image::io::LoadImage(facePaths[i], faces[i]).IsOk())
                {
                    return ErrorCode::Unknown;
                }
                if (faces[i].Width() != faces[i].Height())
                {
                    return ErrorCode::Unknown;
                } // cube faces are square
                if (i == 0)
                {
                    faceSize = faces[0].Width();
                    faceBytes = faces[0].PixelData().Size();
                }
                else if (faces[i].Width() != faceSize || faces[i].PixelData().Size() != faceBytes ||
                         faces[i].Format() != faces[0].Format())
                {
                    return ErrorCode::Unknown;
                } // all faces must match
            }
            if (faceSize == 0 || faceBytes == 0)
            {
                return ErrorCode::Unknown;
            }
            outPixels.Resize(faceBytes * 6u);
            for (usize i = 0; i < 6; ++i)
            {
                MemCopy(outPixels.Data() + faceBytes * i, faces[i].PixelData().Data(), faceBytes);
            }
            outFaceSize = faceSize;
            return Status{};
        }

        // Given ONE face path (e.g. ".../sky_px.png"), derive all 6 face paths by matching a common
        // naming convention (px/nx/..., _posx/..., right/left/...) and rebuilding the set in
        // +X,-X,+Y,-Y,+Z,-Z order. Pure string derivation (no filesystem); pair with LoadCubemap, which
        // validates the files actually load. Returns Unknown if the path matches no known convention.
        [[nodiscard]] static Status DetectCubemapFaces(StringView oneFacePath,
                                                       Array<String>& outPaths)
        {
            const StringView dir = PathParent(oneFacePath);
            const StringView stem = PathStem(oneFacePath);
            const StringView ext = PathExtension(oneFacePath);
            static const StringView conv[5][6] = {
                {u8"px", u8"nx", u8"py", u8"ny", u8"pz", u8"nz"},
                {u8"_px", u8"_nx", u8"_py", u8"_ny", u8"_pz", u8"_nz"},
                {u8"_posx", u8"_negx", u8"_posy", u8"_negy", u8"_posz", u8"_negz"},
                {u8"_right", u8"_left", u8"_top", u8"_bottom", u8"_front", u8"_back"},
                {u8"right", u8"left", u8"top", u8"bottom", u8"front", u8"back"},
            };
            for (const auto& c : conv)
            {
                int matched = -1;
                for (int i = 0; i < 6; ++i)
                {
                    if (EndsWithCI(stem, c[i]))
                    {
                        matched = i;
                        break;
                    }
                }
                if (matched < 0)
                {
                    continue;
                }
                const StringView prefix = stem.SubStr(0, stem.Size() - c[matched].Size());
                outPaths.Clear();
                for (int i = 0; i < 6; ++i)
                {
                    String name{prefix};
                    name.Append(c[i]);
                    name.Append(ext);
                    outPaths.PushBack(dir.IsEmpty() ? name : PathJoin(dir, name.AsView()));
                }
                return Status{};
            }
            return ErrorCode::Unknown;
        }

    private:
        [[nodiscard]] static bool EndsWithCI(StringView s, StringView suffix) noexcept
        {
            if (s.Size() < suffix.Size())
            {
                return false;
            }
            const usize off = s.Size() - suffix.Size();
            for (usize i = 0; i < suffix.Size(); ++i)
            {
                utf8char a = s[off + i], b = suffix[i];
                if (a >= u8'A' && a <= u8'Z')
                {
                    a = static_cast<utf8char>(a + 32);
                }
                if (b >= u8'A' && b <= u8'Z')
                {
                    b = static_cast<utf8char>(b + 32);
                }
                if (a != b)
                {
                    return false;
                }
            }
            return true;
        }
    };

    // Cooks a TextureAsset -> TextureResource (decode + resolve RHI format ->
    // cooked record + "data" pixel stream).
    class TextureAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &TextureAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &TextureResource::StaticType();
        }

        // Embedded-mode textures read the "pixels" sidecar stream - declare it so the recipe
        // hash chains its bytes (the envelope hash doesn't cover sidecars).
        void ScanDependencies(const draconic::editor::Asset& asset,
                              draconic::editor::AssetBuildContext&,
                              draconic::editor::AssetDependencies& out) override
        {
            const TextureAsset& ta = static_cast<const TextureAsset&>(asset);
            if (ta.fileName.IsEmpty() && ta.embeddedWidth > 0)
            {
                out.sourceStreams.PushBack(String(u8"pixels"));
            }
            // Cubemaps: fileName is the +X face (the implicit dep); the other 5 faces must
            // chain into the recipe hash too, or editing one never re-cooks the cube.
            if (ta.shape == TextureShape::Cubemap && !ta.fileName.IsEmpty())
            {
                Array<String> faces;
                if (TextureImporter::DetectCubemapFaces(ta.fileName.View(), faces).IsOk())
                {
                    for (usize i = 0; i < faces.Size(); ++i)
                    {
                        if (faces[i].AsView() != ta.fileName.View())
                        {
                            out.files.PushBack(draconic::vfs::SourcePath(faces[i].AsView()));
                        }
                    }
                }
            }
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const TextureAsset& ta =
                static_cast<const TextureAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            // Embedded mode: the pixels stream IS the decoded RGBA8 image (model imports).
            if (ta.fileName.IsEmpty() && ta.embeddedWidth > 0 && ta.embeddedHeight > 0)
            {
                return BuildEmbedded(ta, ctx);
            }

            // Cube-shaped assets load 6 face files (fileName = the +X face; the rest derive
            // from its naming convention) and cook them concatenated +X,-X,+Y,-Y,+Z,-Z.
            if (ta.shape == TextureShape::Cubemap)
            {
                return BuildCubemap(ta, ctx);
            }

            Result<Array<byte>> bytes = ReadSourceBytes(ctx, ta.fileName.View());
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            image::Image image;
            const Status loaded = image::io::LoadImageFromMemory(
                Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                               bytes.Value().Size()),
                image);
            if (!loaded.IsOk())
            {
                return loaded;
            }

            TextureResource resource;
            resource.width = image.Width();
            resource.height = image.Height();
            resource.depthOrArrayLayers = 1;
            resource.mipLevels = 1;
            resource.format = TextureFormatUtils::Convert(image.Format(), ta.colorSpace);
            resource.shape = ta.shape;
            resource.minFilter = ta.minFilter;
            resource.magFilter = ta.magFilter;
            resource.wrapU = ta.wrapU;
            resource.wrapV = ta.wrapV;
            resource.wrapW = ta.wrapW;
            resource.generateMipmaps = ta.generateMipmaps;
            resource.anisotropy = ta.anisotropy;

            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }

            const Span<const u8> px = image.PixelData();
            return ctx.output->WriteData(
                u8"data", Span<const byte>(reinterpret_cast<const byte*>(px.Data()), px.Size()));
        }

    private:
        // Cook a 6-face cubemap: derive the face paths from the +X face's naming convention,
        // load each through the VFS, validate (square, matching size/format), concatenate.
        [[nodiscard]] static Status BuildCubemap(const TextureAsset& ta,
                                                 draconic::editor::AssetBuildContext& ctx)
        {
            Array<String> facePaths;
            if (!TextureImporter::DetectCubemapFaces(ta.fileName.View(), facePaths).IsOk() ||
                facePaths.Size() != 6)
            {
                return Status{ErrorCode::InvalidArgument}; // fileName matches no face convention
            }

            image::Image faces[6];
            u32 faceSize = 0;
            usize faceBytes = 0;
            for (usize i = 0; i < 6; ++i)
            {
                Result<Array<byte>> bytes = ReadSourceBytes(ctx, facePaths[i].AsView());
                if (!bytes.HasValue())
                {
                    return Status{bytes.Error()};
                }
                const Status loaded = image::io::LoadImageFromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                   bytes.Value().Size()),
                    faces[i]);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                if (faces[i].Width() != faces[i].Height())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                if (i == 0)
                {
                    faceSize = faces[0].Width();
                    faceBytes = faces[0].PixelData().Size();
                }
                else if (faces[i].Width() != faceSize || faces[i].PixelData().Size() != faceBytes ||
                         faces[i].Format() != faces[0].Format())
                {
                    return Status{ErrorCode::InvalidArgument}; // all faces must match
                }
            }
            if (faceSize == 0 || faceBytes == 0)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            TextureResource resource;
            resource.width = faceSize;
            resource.height = faceSize;
            resource.depthOrArrayLayers = 6;
            resource.mipLevels = 1;
            resource.format = TextureFormatUtils::Convert(faces[0].Format(), ta.colorSpace);
            resource.shape = ta.shape;
            resource.minFilter = ta.minFilter;
            resource.magFilter = ta.magFilter;
            resource.wrapU = ta.wrapU;
            resource.wrapV = ta.wrapV;
            resource.wrapW = ta.wrapW;
            resource.generateMipmaps = ta.generateMipmaps;
            resource.anisotropy = ta.anisotropy;

            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }

            Array<byte> pixels;
            pixels.Resize(faceBytes * 6u);
            for (usize i = 0; i < 6; ++i)
            {
                MemCopy(pixels.Data() + faceBytes * i, faces[i].PixelData().Data(), faceBytes);
            }
            return ctx.output->WriteData(u8"data", Span<const byte>(pixels.Data(), pixels.Size()));
        }

        [[nodiscard]] static Status BuildEmbedded(const TextureAsset& ta,
                                                  draconic::editor::AssetBuildContext& ctx)
        {
            if (ctx.source == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            UniquePtr<IStream> stream = ctx.source->ReadData(u8"pixels");
            if (stream.Get() == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            const i64 size = stream->Size();
            const i64 expected = static_cast<i64>(ta.embeddedWidth) * ta.embeddedHeight * 4;
            if (size != expected)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            Array<byte> pixels;
            pixels.Resize(static_cast<usize>(size));
            if (stream->Read(pixels.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
            {
                return Status{ErrorCode::Unknown};
            }

            TextureResource resource;
            resource.width = ta.embeddedWidth;
            resource.height = ta.embeddedHeight;
            resource.depthOrArrayLayers = 1;
            resource.mipLevels = 1;
            resource.format = (ta.colorSpace == image::ImageColorSpace::Srgb)
                                  ? rhi::TextureFormat::RGBA8UnormSrgb
                                  : rhi::TextureFormat::RGBA8Unorm;
            resource.shape = ta.shape;
            resource.minFilter = ta.minFilter;
            resource.magFilter = ta.magFilter;
            resource.wrapU = ta.wrapU;
            resource.wrapV = ta.wrapV;
            resource.wrapW = ta.wrapW;
            resource.generateMipmaps = ta.generateMipmaps;
            resource.anisotropy = ta.anisotropy;

            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            return ctx.output->WriteData(u8"data", Span<const byte>(pixels.Data(), pixels.Size()));
        }
    };

    // OS-file importer (editor drag-drop): copies the image into Sources/ and creates a
    // TextureAsset instance named after the file stem in the target group. Preset by intent:
    //   - .hdr                                  -> equirectangular sky (linear/clamped/no mips)
    //   - a cube-face name (sky_px.png etc.) with all 6 sibling faces present
    //                                           -> ONE cube asset (all 6 faces copied)
    //   - anything else                         -> the standard 3D preset
    class TextureFileImporter final : public draconic::editor::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Texture"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            for (StringView ext : {u8"png", u8"jpg", u8"jpeg", u8"tga", u8"bmp", u8"hdr"})
            {
                if (extension == ext)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, draconic::editor::EditorProject& project,
               content::Group& group, const draconic::editor::ImportOptions*, Object*,
               Array<draconic::editor::DeferredImportWrite>*) override
        {
            // Cubemap intent: the dropped file's stem matches a face convention (px/nx/...,
            // _posx/..., right/left/...) AND all 6 sibling faces exist beside it. Any one
            // face can be dropped; the asset stores the +X face (the rest derive at cook).
            {
                Array<String> facePaths;
                if (TextureImporter::DetectCubemapFaces(sourcePath, facePaths).IsOk())
                {
                    bool allPresent = facePaths.Size() == 6;
                    for (const String& face : facePaths)
                    {
                        if (!FileExists(face.AsView()))
                        {
                            allPresent = false;
                            break;
                        }
                    }
                    if (allPresent)
                    {
                        return ImportCube(facePaths, project, group);
                    }
                }
            }

            Result<String> fileName = draconic::editor::CopyIntoSources(project, sourcePath);
            if (!fileName.HasValue())
            {
                return Err(fileName.Error());
            }

            const StringView stem = draconic::editor::FileStemOf(fileName.Value().AsView());
            content::Instance* instance = group.CreateInstance(stem, TextureAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }

            TextureAsset asset;
            asset.fileName = draconic::vfs::SourcePath(fileName.Value().AsView());
            if (draconic::editor::FileExtensionLower(sourcePath) == u8"hdr")
            {
                asset.SetupForEquirectangularSkybox(); // .hdr = an environment, not a surface map
            }
            else
            {
                asset.SetupFor3D();
            }
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }

    private:
        // Copy all 6 faces into Sources/ and create ONE cube TextureAsset. fileName = the
        // +X face; the builder re-derives the face set from its naming convention at cook.
        [[nodiscard]] static Result<content::Instance*>
        ImportCube(const Array<String>& facePaths, draconic::editor::EditorProject& project,
                   content::Group& group)
        {
            String posXName;
            for (usize i = 0; i < facePaths.Size(); ++i)
            {
                Result<String> copied =
                    draconic::editor::CopyIntoSources(project, facePaths[i].AsView());
                if (!copied.HasValue())
                {
                    return Err(copied.Error());
                }
                if (i == 0)
                {
                    posXName = copied.Value();
                }
            }

            // "sky_px" -> "sky" (strip the face suffix + a trailing separator); fall back to
            // the full stem when the convention leaves nothing.
            const StringView posXStem = draconic::editor::FileStemOf(posXName.AsView());
            Array<String> derived;
            String name;
            if (TextureImporter::DetectCubemapFaces(posXName.AsView(), derived).IsOk())
            {
                // The convention suffix is whatever the +X path ends with beyond the shared prefix.
                usize common = 0;
                const StringView a = derived[0].AsView();
                const StringView b = derived[1].AsView();
                while (common < a.Size() && common < b.Size() && a[common] == b[common])
                {
                    ++common;
                }
                StringView prefix = draconic::editor::FileStemOf(a.SubStr(0, common));
                while (!prefix.IsEmpty() && (prefix[prefix.Size() - 1] == utf8char('_') ||
                                             prefix[prefix.Size() - 1] == utf8char('-')))
                {
                    prefix = prefix.SubStr(0, prefix.Size() - 1);
                }
                name = String(prefix);
            }
            if (name.IsEmpty())
            {
                name = String(posXStem);
            }

            content::Instance* instance =
                group.CreateInstance(name.AsView(), TextureAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            TextureAsset asset;
            asset.fileName = draconic::vfs::SourcePath(posXName.AsView());
            asset.SetupForCubemapSkybox();
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // Registers TextureAsset for content-DB construction + deserialization. Also registers the
    // enum reflection its properties reference (owning modules; idempotent) so the generic asset
    // page can render enum-by-name dropdowns. TextureAsset's OWN reflection body (properties +
    // attributes) is TextureAsset::StaticType(), defined in TextureAssetImpl.cpp.
    inline void RegisterTextureAsset()
    {
        RegisterTextureReflection();      // TextureShape / TextureFilter / TextureWrap names
        image::RegisterImageReflection(); // ImageColorSpace names
        GlobalTypeRegistry().Register(TextureAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<TextureAsset>();
    }
}

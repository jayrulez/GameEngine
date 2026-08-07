// Draconic::ShaderEditor - the `draconic.shaders.editor` module (tooling).
//
// Source-side shader authoring + cook:
//   * ShaderAsset (editor::Asset): a shader name + the two HLSL source files
//     (vertex via the inherited fileName, fragment alongside). v1 is two-stage to
//     match ShaderSource; more stages can be added the same way.
//   * ShaderAssetBuilder (DefaultAssetBuilder): reads both .hlsl files and cooks a
//     runtime ShaderSource (name + inline per-stage HLSL) into the output DB.
//   * ShaderImporter: an authoring helper that fills a ShaderAsset from file paths.
//
// Reads the .hlsl text through the build context's sources mount (all file access
// via the VFS). Never linked by the runtime.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.shaders.editor;

import draconic.foundation;
import draconic.editor;
import draconic.vfs;
import draconic.shaders.resource;
import draconic.content;

using namespace draconic::foundation;

namespace draconic::shaders
{
}

export namespace draconic::shaders
{
    // Source asset: a shader name + the two HLSL files (vertex = inherited fileName,
    // fragment alongside). Paths are relative to the sources mount at cook time.
    class ShaderAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(ShaderAsset, draconic::editor::Asset)
    public:
        String name;         // logical shader name (how materials reference it)
        String fragmentFile; // fragment-stage HLSL file (vertex = fileName)

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName (vertex source)
            draconic::foundation::Serialize(ar, "name", name);
            draconic::foundation::Serialize(ar, "fragmentFile", fragmentFile);
        }
    };

    // Cooks a ShaderAsset -> ShaderSource (read both .hlsl files -> inline source).
    class ShaderAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ShaderAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ShaderSource::StaticType();
        }

        // The fragment file is a second source input (vertex = the implicit fileName): editing
        // it must dirty this shader's recipe hash.
        void ScanDependencies(const draconic::editor::Asset& asset,
                              draconic::editor::AssetBuildContext&,
                              draconic::editor::AssetDependencies& out) override
        {
            const ShaderAsset& sa = static_cast<const ShaderAsset&>(asset);
            if (!sa.fragmentFile.IsEmpty())
            {
                out.files.PushBack(draconic::vfs::SourcePath(sa.fragmentFile.AsView()));
            }
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const ShaderAsset& sa =
                static_cast<const ShaderAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            ShaderSource source;
            source.name = sa.name;

            Status read = ReadSourceText(ctx, sa.fileName.View(), source.vertexSource);
            if (!read.IsOk())
            {
                return read;
            }
            read = ReadSourceText(ctx, sa.fragmentFile.AsView(), source.fragmentSource);
            if (!read.IsOk())
            {
                return read;
            }

            return ctx.output->WriteObject(source);
        }
    };

    // Authoring helper: fill a ShaderAsset from a name + the two source-file paths.
    class ShaderImporter
    {
    public:
        static void Import(StringView name, StringView vertexFile, StringView fragmentFile,
                           ShaderAsset& outAsset)
        {
            outAsset.name = String(name);
            outAsset.fileName = draconic::vfs::SourcePath(vertexFile);
            outAsset.fragmentFile = String(fragmentFile);
        }
    };

    // Registers ShaderAsset for content-DB construction + deserialization.
    inline void RegisterShaderAsset()
    {
        GlobalTypeRegistry().Register(ShaderAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ShaderAsset>();
    }

    // ShaderAsset::StaticType() is defined WITH reflected properties in ShaderAssetImpl.cpp
    // (reflection track P1).
}

// Draconic::Editor - the `draconic.editor` module.
//
// The tooling/authoring base for the asset pipeline (docs/design/asset-pipeline.md): an `Asset`
// is the editor/source object (references an external source file + import settings) and an
// `IAssetBuilder` cooks it into a runtime *resource* written to the output content database.
// The runtime never links this module - it loads only cooked resources. (Asset = source/editor;
// Resource = runtime/cooked.)
//
// Builder API v2 (pipeline phase 6a):
//   - source files are read through the VFS (`AssetBuildContext::sources` mount), never raw
//     paths - builders use ReadSourceBytes/ReadSourceText;
//   - `Version()`: bump when cook logic changes so exactly this builder's products re-cook
//     (folded into the recipe hash - see the design doc; forgetting the bump is the known
//     failure mode, Build > Rebuild All is the big hammer);
//   - `ScanDependencies()`: declares what a build consumes beyond the implicit Asset::fileName -
//     extra FILES, content READS of other instances (hash-chained: editing them re-cooks this),
//     and runtime REFERENCES (existence-only: never re-cook on content change);
//   - `BuilderRegistry`: asset type -> builder routing for the cook driver.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

export module draconic.editor;

import draconic.core;
import draconic.content;
export import draconic.vfs; // Asset::fileName is vfs::SourcePath

using namespace draconic::core;

export namespace draconic::editor
{
    // Source/authoring asset: a serializable that references an external source
    // file (relative to the sources mount). Concrete assets derive this and
    // add their import settings; call Asset::Serialize for the file name.
    class Asset : public ISerializable
    {
        DRACONIC_OBJECT(Asset, ISerializable)
    public:
        // Source file, relative to the sources mount (empty = embedded data). Typed:
        // normalization guarantees forward-slash relative form in cooked data - a
        // Windows-authored backslash path heals on load instead of breaking the VFS.
        draconic::vfs::SourcePath fileName;

        void Serialize(ISerializer& ar) override
        {
            draconic::core::Serialize(ar, "fileName", fileName);
        }
    };

    // Inputs a builder cooks against: the sources mount (all file access through the VFS), the
    // output instance to write the cooked resource into, and the content DB (so a builder can
    // resolve cross-asset references during the bake).
    struct AssetBuildContext
    {
        draconic::vfs::IFileSystem* sources = nullptr; // mount for Asset::fileName + extra files
        draconic::content::Instance* source =
            nullptr; // the SOURCE instance being cooked (embedded data streams)
        draconic::content::Instance* output = nullptr;     // cooked resource is written here
        draconic::content::IContentDatabase* db = nullptr; // for resolving referenced assets
    };

    // What one build consumes beyond the implicit Asset::fileName. The cook driver hashes files
    // and chains `reads`; `references` only order the cook (see the design doc, §3).
    struct AssetDependencies
    {
        Array<draconic::vfs::SourcePath> files; // extra source files read (mount-relative)
        Array<String> sourceStreams; // the source instance's data streams the build reads
                                     // (embedded payloads live in SIDECAR files the envelope
                                     // hash doesn't cover - declaring them chains their bytes)
        Array<Guid> reads;           // instances whose CONTENT this build consumes (hash-chained)
        Array<Guid> references;      // instances the product refers to at runtime (existence only)
    };

    // Cooks one source asset type into a runtime resource (source -> product).
    // Runs in tooling only; writes to ctx.output (WriteObject + WriteData).
    class IAssetBuilder
    {
    public:
        virtual ~IAssetBuilder() = default;

        // The source Asset type this builder handles.
        [[nodiscard]] virtual const TypeInfo* AssetType() const = 0;

        // The cooked resource type this builder writes (the cook driver stamps output
        // instances with it).
        [[nodiscard]] virtual const TypeInfo* ProductType() const = 0;

        // Cook-logic version: BUMP whenever Build()'s output changes for the same inputs.
        [[nodiscard]] virtual u32 Version() const { return 1; }

        // Declare extra dependencies (Asset::fileName is implicit). Default: none.
        virtual void ScanDependencies(const Asset& asset, AssetBuildContext& ctx,
                                      AssetDependencies& out)
        {
            (void)asset;
            (void)ctx;
            (void)out;
        }

        // Cook `asset` into ctx.output. Returns Ok or a failure status.
        [[nodiscard]] virtual Status Build(const Asset& asset, AssetBuildContext& ctx) = 0;
    };

    // Convenience base: shared helpers for concrete builders.
    class DefaultAssetBuilder : public IAssetBuilder
    {
    public:
        // Read a whole source file (mount-relative) through the VFS.
        [[nodiscard]] static Result<Array<byte>> ReadSourceBytes(const AssetBuildContext& ctx,
                                                                 StringView fileName)
        {
            if (ctx.sources == nullptr)
            {
                return Err(ErrorCode::InvalidArgument);
            }
            UniquePtr<IStream> stream = ctx.sources->Open(fileName, FileMode::Read);
            if (stream.Get() == nullptr)
            {
                return Err(ErrorCode::NotFound);
            }

            const i64 size = stream->Size();
            if (size < 0)
            {
                return Err(ErrorCode::Unknown);
            }
            Array<byte> buf;
            buf.Resize(static_cast<usize>(size));
            if (size > 0 &&
                stream->Read(buf.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
            {
                return Err(ErrorCode::Unknown);
            }
            return buf;
        }

        // Read a whole source text file (mount-relative) through the VFS.
        [[nodiscard]] static Status ReadSourceText(const AssetBuildContext& ctx,
                                                   StringView fileName, String& out)
        {
            Result<Array<byte>> bytes = ReadSourceBytes(ctx, fileName);
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()),
                                    bytes.Value().Size()));
            return Status{};
        }
    };

    // Asset type -> builder routing for the cook driver. Modules register their builders here
    // (the executable assembles the set, mirroring the page-factory/creator registries).
    class BuilderRegistry
    {
    public:
        void Register(UniquePtr<IAssetBuilder> builder)
        {
            if (builder)
            {
                m_builders.PushBack(Move(builder));
            }
        }

        [[nodiscard]] IAssetBuilder* Find(const TypeInfo* assetType) const
        {
            for (const UniquePtr<IAssetBuilder>& b : m_builders)
            {
                if (b->AssetType() == assetType)
                {
                    return b.Get();
                }
            }
            return nullptr;
        }

        [[nodiscard]] IAssetBuilder* FindByTypeName(StringView typeName) const
        {
            for (const UniquePtr<IAssetBuilder>& b : m_builders)
            {
                const TypeInfo* type = b->AssetType();
                if (type != nullptr && type->name != nullptr &&
                    StringView(reinterpret_cast<const utf8char*>(type->name)) == typeName)
                {
                    return b.Get();
                }
            }
            return nullptr;
        }

        [[nodiscard]] usize Count() const noexcept { return m_builders.Size(); }

    private:
        Array<UniquePtr<IAssetBuilder>> m_builders;
    };

    // Reflects the Asset base (its fileName property) + SourcePath, so EVERY concrete asset
    // surfaces its source file through the base chain (FindProperty walks bases). Idempotent.
    // Asset::StaticType() itself is defined WITH the fileName property in AssetImpl.cpp
    // (reflection track P1).
    void RegisterAssetReflection();
}

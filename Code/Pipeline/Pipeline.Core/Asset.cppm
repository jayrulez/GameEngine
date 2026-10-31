// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Core - the `editor.core` module.
//
// The tooling and authoring base for the asset pipeline: an `Asset` is the editor's source object
// (it references an external source file and import settings), and an `IAssetBuilder` cooks it
// into a runtime resource written to the output content database. The runtime never links this
// module; it loads only cooked resources. (Asset is source and editor; Resource is runtime and
// cooked.)
//
// Builder API:
//   - source files are read through the VFS (`AssetBuildContext::sources` mount), never raw
//     paths; builders use ReadSourceBytes and ReadSourceText;
//   - `Version()`: bump when cook logic changes so exactly this builder's products re-cook
//     (it is folded into the recipe hash; a forgotten bump leaves stale products, and
//     Build > Rebuild All re-cooks everything);
//   - `ScanDependencies()`: declares what a build consumes beyond the implicit Asset::fileName:
//     extra files, content reads of other instances (hash-chained: editing them re-cooks this),
//     and runtime references (existence only: never re-cook on content change);
//   - `BuilderRegistry`: routes each asset type to its builder for the cook driver.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module pipeline.core;

import foundation.core;
import foundation.content;
export import foundation.vfs; // Asset::fileName is vfs::SourcePath
export import :asset_creator; // AssetCreator, AssetCreationContext, AssetCreatorRegistry

using namespace foundation::core;

export namespace pipeline
{
    // The availability domain for the Pipeline collection's authoring types (asset classes and
    // import settings), answering which processes have the type. The player does not have them (it
    // loads cooked products, never authoring assets), so Runtime would be wrong; the headless
    // cook, CLI and MCP hosts do have them without the editor, so Editor is wrong too. The player
    // has Runtime only; the pipeline, CLI and MCP have Runtime and Pipeline; the editor has all
    // three. The set of domains is open (foundation.core owns only the default Runtime domain).
    inline constexpr TypeDomain kPipelineTypeDomain{StringView(u8"Pipeline")};

    // Source/authoring asset: a serializable that references an external source
    // file (relative to the sources mount). Concrete assets derive this and
    // add their import settings; call Asset::Serialize for the file name.
    class Asset : public ISerializable
    {
        RTTI_OBJECT(Asset, ISerializable)
    public:
        // Source file, relative to the sources mount (empty = embedded data). Typed:
        // normalization guarantees forward-slash relative form in cooked data, so a
        // Windows-authored backslash path heals on load instead of breaking the VFS.
        foundation::vfs::SourcePath fileName;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "fileName", fileName);
        }
    };

    // The export target a cook is producing for. `id` names the target: it is both the
    // per-target cooked-DB directory name and the recipe platform salt (so a variant product
    // recooks when the target changes). The capability booleans describe the compressed-texture
    // families the target's devices support; a variant builder maps them to its encoder profile
    // and never keys on the id or platform directly. HostTarget() is the always-warm desktop view
    // the editor cooks against.
    struct CookTarget
    {
        String id;         // "host", "web-bc", "web-astc", ... (DB dir and recipe salt)
        bool bc = true;    // BC1-BC7 (desktop and desktop browsers)
        bool astc = false; // ASTC (mobile browsers)
        bool etc2 = false; // ETC2 (not supported)
    };

    // The default always-warm target the editor dev loop cooks against (BC-capable desktop).
    [[nodiscard]] inline CookTarget HostTarget()
    {
        return CookTarget{String(u8"host"), true, false, false};
    }

    // Resolve a target id to its capability profile. The web export splits into
    // two variant targets: "web-bc" (desktop browsers) and "web-astc" (mobile browsers). Everything
    // else ("host", desktop platforms, unknown ids) is BC-capable desktop. New targets
    // (consoles, ETC2 budget mobiles) extend this; the resolver keys on capability, never on platform.
    [[nodiscard]] inline CookTarget CookTargetFor(StringView id)
    {
        if (id == StringView(u8"web-astc"))
        {
            return CookTarget{String(id), /*bc*/ false, /*astc*/ true, /*etc2*/ false};
        }
        return CookTarget{String(id), /*bc*/ true, /*astc*/ false, /*etc2*/ false};
    }

    // Inputs a builder cooks against: the sources mount (all file access through the VFS), the
    // output instance to write the cooked resource into, and the content DB (so a builder can
    // resolve cross-asset references during the bake).
    struct AssetBuildContext
    {
        // The cook's allocator (required; the driver or the editor cook service decides):
        // everything a Build() allocates for its products comes from here.
        explicit AssetBuildContext(IAllocator& alloc) noexcept : allocator(&alloc) {}

        IAllocator* allocator;
        foundation::vfs::IFileSystem* sources = nullptr; // mount for Asset::fileName + extra files
        foundation::content::Instance* source =
            nullptr; // the source instance being cooked (embedded data streams)
        foundation::content::Instance* output = nullptr;     // cooked resource is written here
        foundation::content::IContentDatabase* db = nullptr; // for resolving referenced assets
        // The source database (asset envelopes and raw sidecars). At cook time `db` is the
        // cooked-products view, so a builder that needs another asset's source form (its
        // envelope's fileName, embedded pixels, ...) must read through this one; casting a cooked
        // product to its Asset envelope fails.
        foundation::content::IContentDatabase* sourceDb = nullptr;
        // The export target being produced. Null = the host target (a variant
        // builder falls back to the desktop BC profile).
        const CookTarget* target = nullptr;
        // The cook's job system for data-parallel work inside one build (a texture's block rows:
        // a 4k texture's BC7 encode is slow on one core). Null runs inline.
        JobSystem* jobs = nullptr;
    };

    // What one build consumes beyond the implicit Asset::fileName. The cook driver hashes files
    // and chains `reads`; `references` only order the cook.
    struct AssetDependencies
    {
        Array<foundation::vfs::SourcePath> files; // extra source files read (mount-relative)
        Array<String> sourceStreams; // the source instance's data streams the build reads
                                     // (embedded payloads live in sidecar files the envelope
                                     // hash doesn't cover; declaring them chains their bytes)
        Array<Guid> reads;           // instances whose content this build consumes (hash-chained)
        Array<Guid> references;      // instances the product refers to at runtime (existence only)
    };

    // Whether a builder's output can differ per export target. The cook treats invariant products
    // as platform-agnostic: they are cooked once into the host DB and copied forward into every
    // target DB (never recooked), and their recipe carries no platform salt. Variant builders
    // (textures: BC or ASTC) cook per target and their recipe is salted by the target, so the same
    // source produces a distinct product per target. Invariant is the default.
    enum class BuildVariance : u8
    {
        PlatformInvariant,
        PlatformVariant,
    };

    // Cooks one source asset type into a runtime resource (source to product).
    // Runs in tooling only; writes to ctx.output (WriteObject and WriteData).
    class IAssetBuilder
    {
    public:
        virtual ~IAssetBuilder() = default;

        // The source Asset type this builder handles.
        [[nodiscard]] virtual const TypeInfo* AssetType() const = 0;

        // The cooked resource type this builder writes (the cook driver stamps output
        // instances with it).
        [[nodiscard]] virtual const TypeInfo* ProductType() const = 0;

        // Cook-logic version: bump whenever Build()'s output changes for the same inputs.
        [[nodiscard]] virtual u32 Version() const { return 1; }

        // Whether this builder's product varies per export target. Default invariant (see BuildVariance).
        [[nodiscard]] virtual BuildVariance Variance() const { return BuildVariance::PlatformInvariant; }

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

    // Routes each asset type to its builder for the cook driver. Modules register their builders
    // here (the executable assembles the set, as with the page-factory and creator registries).
    class BuilderRegistry
    {
    public:
        // The allocator (required; the owner decides) backs registered builders.
        explicit BuilderRegistry(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

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

        /// Visit every registered builder (the registration tripwires audit the whole set, such
        /// as "every ProductType() is a registered serializable").
        template <typename F> void ForEach(F&& fn) const
        {
            for (const UniquePtr<IAssetBuilder>& b : m_builders)
            {
                fn(*b);
            }
        }

    private:
        IAllocator* m_allocator;
        Array<UniquePtr<IAssetBuilder>> m_builders;
    };

    // Reflects the Asset base (its fileName property) and SourcePath, so every concrete asset
    // surfaces its source file through the base chain (FindProperty walks bases). Idempotent.
    // Asset::StaticType() itself is defined with the fileName property in AssetImpl.cpp.
    void RegisterAssetReflection();

    /// A file-backed creation: `starter` written to "<name><extension>" in the context's
    /// sources folder, and a uniquely named instance of `type` whose `asset` links that file
    /// through fileName (a script, a UI document). Null when there is no sources folder or a
    /// write is refused.
    [[nodiscard]] inline foundation::content::Instance*
    CreateLinkedTextAsset(const AssetCreationContext& context, StringView baseName,
                          StringView extension, StringView starter, const TypeInfo& type,
                          Asset& asset)
    {
        foundation::content::Group* target = context.Target();
        if (target == nullptr || context.sourcesRoot.IsEmpty())
        {
            return nullptr;
        }
        const String name = target->UniqueInstanceName(context.NameOr(baseName));
        String fileName(name.AsView());
        fileName.Append(extension);
        const String path = PathJoin(context.sourcesRoot, fileName.AsView());
        if (!WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(starter.Data()),
                                                       starter.Size()))
                 .IsOk())
        {
            return nullptr;
        }
        foundation::content::Instance* instance = target->CreateInstance(name.AsView(), type);
        if (instance == nullptr)
        {
            return nullptr;
        }
        asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
        return instance->WriteObject(asset).IsOk() ? instance : nullptr;
    }
}

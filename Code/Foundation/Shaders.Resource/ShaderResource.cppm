// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Shaders.Resource - the `foundation.shaders.resource` module.
///
/// Shaders as resources: a `ShaderSource` (authored content: name + per-stage HLSL)
/// is built by `ShaderFactory` into a runtime `ShaderResource`. The factory
/// registers the sources with the ShaderSystem and bumps the shader's version, so
/// a reload (via the resource manager) propagates: PSO caches see the version
/// change and rebuild, and a material that depends on this ShaderResource is a
/// normal resource→resource edge. Variants themselves live in the (shared)
/// ShaderSystem; this product is the resource-system handle + version accessor.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.shaders.resource;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::resource;
namespace rhi = foundation::rhi;

export namespace foundation::shaders
{

    // One program of a shader: what the shader system registers under one name.
    struct ShaderProgram
    {
        String name; // the shader's name plus the program's suffix ("lava", "lava.depth")
        StringView vertexSource;
        StringView fragmentSource;
        StringView vertexEntryPoint;
        StringView fragmentEntryPoint;
        ShaderFlags vertexVariantMask = ShaderFlags::None;
        ShaderFlags fragmentVariantMask = ShaderFlags::None;
    };

    // The suffixes of a shader's own depth-like programs: the depth prepass and shadow maps, and
    // the editor's pick.
    inline constexpr StringView kDepthProgramSuffix = u8".depth";
    inline constexpr StringView kPickProgramSuffix = u8".pick";

    // A cooked shader: a name + per-stage HLSL (the cook's expanded source: a properties block
    // already replaced by its set-2 declarations), the entry point each stage's function is called
    // in it, each stage's declared variant lattice, and the material property table the properties
    // block declared (the layout every material on this shader is cooked with). A surface shader
    // carries more programs beside its main one (its depth and pick), each registered under the
    // shader's name plus its suffix.
    class ShaderSource final : public ISerializable
    {
        RTTI_OBJECT(ShaderSource, ISerializable)
    public:
        String name;
        String vertexSource;
        String fragmentSource;
        String vertexEntryPoint;
        String fragmentEntryPoint;
        u32 vertexVariantMask = 0;   // ShaderFlags the vertex stage declares
        u32 fragmentVariantMask = 0; // ShaderFlags the fragment stage declares
        // The property table, as parallel arrays (the primitive Array<T> serialization path).
        Array<String> propertyNames;
        Array<u8> propertyTypes;     // ShaderPropertyType
        Array<f32> propertyDefaults; // 4 per property
        Array<u8> propertyHasRange;
        Array<f32> propertyRanges;   // min, max per property
        Array<u32> propertyOffsets;
        Array<u32> propertySizes;
        Array<u32> propertyBindings;
        u32 uniformSize = 0;
        // The programs beside the main one, as parallel arrays: a suffix, both stages' source,
        // entries and lattices.
        Array<String> programSuffixes;
        Array<String> programVertexSources;
        Array<String> programFragmentSources;
        Array<String> programVertexEntryPoints;
        Array<String> programFragmentEntryPoints;
        Array<u32> programVertexMasks;
        Array<u32> programFragmentMasks;
        // The depth-like programs replace the built-ins for every material on the shader (it
        // moves its vertices), not only for a masked one (whose cut-out is its own).
        bool depthProgramsAlways = false;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "vertexSource", vertexSource);
            foundation::core::Serialize(ar, "fragmentSource", fragmentSource);
            foundation::core::Serialize(ar, "vertexEntryPoint", vertexEntryPoint);
            foundation::core::Serialize(ar, "fragmentEntryPoint", fragmentEntryPoint);
            foundation::core::Serialize(ar, "vertexVariantMask", vertexVariantMask);
            foundation::core::Serialize(ar, "fragmentVariantMask", fragmentVariantMask);
            foundation::core::Serialize(ar, "propertyNames", propertyNames);
            foundation::core::Serialize(ar, "propertyTypes", propertyTypes);
            foundation::core::Serialize(ar, "propertyDefaults", propertyDefaults);
            foundation::core::Serialize(ar, "propertyHasRange", propertyHasRange);
            foundation::core::Serialize(ar, "propertyRanges", propertyRanges);
            foundation::core::Serialize(ar, "propertyOffsets", propertyOffsets);
            foundation::core::Serialize(ar, "propertySizes", propertySizes);
            foundation::core::Serialize(ar, "propertyBindings", propertyBindings);
            foundation::core::Serialize(ar, "uniformSize", uniformSize);
            foundation::core::Serialize(ar, "programSuffixes", programSuffixes);
            foundation::core::Serialize(ar, "programVertexSources", programVertexSources);
            foundation::core::Serialize(ar, "programFragmentSources", programFragmentSources);
            foundation::core::Serialize(ar, "programVertexEntryPoints", programVertexEntryPoints);
            foundation::core::Serialize(ar, "programFragmentEntryPoints", programFragmentEntryPoints);
            foundation::core::Serialize(ar, "programVertexMasks", programVertexMasks);
            foundation::core::Serialize(ar, "programFragmentMasks", programFragmentMasks);
            foundation::core::Serialize(ar, "depthProgramsAlways", depthProgramsAlways);
        }

        // Adds a program beside the main one, registered as the shader's name plus `suffix`.
        void AddProgram(StringView suffix, StringView vertex, StringView vertexEntry,
                        ShaderFlags vertexMask, StringView fragment, StringView fragmentEntry,
                        ShaderFlags fragmentMask)
        {
            programSuffixes.PushBack(String(suffix));
            programVertexSources.PushBack(String(vertex));
            programFragmentSources.PushBack(String(fragment));
            programVertexEntryPoints.PushBack(String(vertexEntry));
            programFragmentEntryPoints.PushBack(String(fragmentEntry));
            programVertexMasks.PushBack(static_cast<u32>(vertexMask));
            programFragmentMasks.PushBack(static_cast<u32>(fragmentMask));
        }

        // Every program, the main one first: what the shader system registers and an export
        // cooks. Views into this source. A program whose arrays disagree (a damaged product) is
        // left out.
        [[nodiscard]] Array<ShaderProgram> Programs() const
        {
            Array<ShaderProgram> out;
            ShaderProgram main;
            main.name = name;
            main.vertexSource = vertexSource.AsView();
            main.fragmentSource = fragmentSource.AsView();
            main.vertexEntryPoint = vertexEntryPoint.AsView();
            main.fragmentEntryPoint = fragmentEntryPoint.AsView();
            main.vertexVariantMask = static_cast<ShaderFlags>(vertexVariantMask);
            main.fragmentVariantMask = static_cast<ShaderFlags>(fragmentVariantMask);
            out.PushBack(Move(main));
            usize count = programSuffixes.Size();
            count = Min(count, Min(programVertexSources.Size(), programFragmentSources.Size()));
            count = Min(count, Min(programVertexEntryPoints.Size(),
                                   programFragmentEntryPoints.Size()));
            count = Min(count, Min(programVertexMasks.Size(), programFragmentMasks.Size()));
            for (usize i = 0; i < count; ++i)
            {
                ShaderProgram p;
                p.name = name;
                p.name += programSuffixes[i];
                p.vertexSource = programVertexSources[i].AsView();
                p.fragmentSource = programFragmentSources[i].AsView();
                p.vertexEntryPoint = programVertexEntryPoints[i].AsView();
                p.fragmentEntryPoint = programFragmentEntryPoints[i].AsView();
                p.vertexVariantMask = static_cast<ShaderFlags>(programVertexMasks[i]);
                p.fragmentVariantMask = static_cast<ShaderFlags>(programFragmentMasks[i]);
                out.PushBack(Move(p));
            }
            return out;
        }

        [[nodiscard]] bool HasProgram(StringView suffix) const noexcept
        {
            for (const String& s : programSuffixes)
            {
                if (s.AsView() == suffix)
                {
                    return true;
                }
            }
            return false;
        }

        // Strips every program's HLSL (an export that ships bytecode only).
        void StripSources()
        {
            vertexSource = String();
            fragmentSource = String();
            for (String& s : programVertexSources)
            {
                s = String();
            }
            for (String& s : programFragmentSources)
            {
                s = String();
            }
        }

        // Records a parsed properties block's table.
        void SetProperties(const ShaderPropertiesBlock& block)
        {
            propertyNames.Clear();
            propertyTypes.Clear();
            propertyDefaults.Clear();
            propertyHasRange.Clear();
            propertyRanges.Clear();
            propertyOffsets.Clear();
            propertySizes.Clear();
            propertyBindings.Clear();
            for (const ShaderProperty& p : block.properties)
            {
                propertyNames.PushBack(p.name);
                propertyTypes.PushBack(static_cast<u8>(p.type));
                propertyDefaults.PushBack(p.value.x);
                propertyDefaults.PushBack(p.value.y);
                propertyDefaults.PushBack(p.value.z);
                propertyDefaults.PushBack(p.value.w);
                propertyHasRange.PushBack(p.hasRange ? 1u : 0u);
                propertyRanges.PushBack(p.rangeMin);
                propertyRanges.PushBack(p.rangeMax);
                propertyOffsets.PushBack(p.offset);
                propertySizes.PushBack(p.size);
                propertyBindings.PushBack(p.binding);
            }
            uniformSize = block.uniformSize;
        }

        // The property table, as SetProperties recorded it. A table whose arrays disagree in
        // length (a damaged product) yields what the shortest covers.
        [[nodiscard]] Array<ShaderProperty> Properties() const
        {
            Array<ShaderProperty> out;
            usize count = propertyNames.Size();
            count = Min(count, propertyTypes.Size());
            count = Min(count, propertyDefaults.Size() / 4u);
            count = Min(count, propertyHasRange.Size());
            count = Min(count, propertyRanges.Size() / 2u);
            count = Min(count, Min(propertyOffsets.Size(), Min(propertySizes.Size(),
                                                               propertyBindings.Size())));
            for (usize i = 0; i < count; ++i)
            {
                ShaderProperty p;
                p.name = propertyNames[i];
                p.type = static_cast<ShaderPropertyType>(propertyTypes[i]);
                p.value = Float4{propertyDefaults[i * 4], propertyDefaults[i * 4 + 1],
                                 propertyDefaults[i * 4 + 2], propertyDefaults[i * 4 + 3]};
                p.hasRange = propertyHasRange[i] != 0u;
                p.rangeMin = propertyRanges[i * 2];
                p.rangeMax = propertyRanges[i * 2 + 1];
                p.offset = propertyOffsets[i];
                p.size = propertySizes[i];
                p.binding = propertyBindings[i];
                out.PushBack(Move(p));
            }
            return out;
        }
    };

    // Runtime handle for a shader: its name + the ShaderSystem it lives in. Version()
    // is the reload signal (bumped each rebuild); GetVariant compiles-on-demand.
    class ShaderResource final : public Object
    {
        RTTI_OBJECT(ShaderResource, Object)
    public:
        void Init(ShaderSystem* system, StringView name)
        {
            m_system = system;
            m_name = String(name);
        }

        // Records the shader's own depth-like programs (a ShaderSource's .depth and .pick), and
        // whether they replace the built-ins for every material or only for a masked one.
        void SetDepthPrograms(bool depth, bool pick, bool always)
        {
            m_depthName = String();
            m_pickName = String();
            if (depth)
            {
                m_depthName = m_name;
                m_depthName += kDepthProgramSuffix;
            }
            if (pick)
            {
                m_pickName = m_name;
                m_pickName += kPickProgramSuffix;
            }
            m_depthProgramsAlways = always;
        }

        // The program a material on this shader draws the depth prepass and shadows with, and its
        // pick with: the shader's own when it has one and needs it (it moves its vertices, or the
        // material is masked and cuts out with the shader's alpha); empty = the built-in.
        [[nodiscard]] StringView DepthProgram(bool masked) const noexcept
        {
            return (m_depthProgramsAlways || masked) ? m_depthName.AsView() : StringView{};
        }
        [[nodiscard]] StringView PickProgram(bool masked) const noexcept
        {
            return (m_depthProgramsAlways || masked) ? m_pickName.AsView() : StringView{};
        }

        // The variant flags the shader's main vertex stage declares (its cooked lattice): what a
        // material on it may turn on, so which effect groups it carries (an effect's flag here,
        // `WIND`, gives the material the effect's group; Specs/wind-for-every-shader.md).
        void SetVertexVariants(ShaderFlags mask) noexcept { m_vertexVariants = mask; }
        [[nodiscard]] ShaderFlags VertexVariants() const noexcept { return m_vertexVariants; }
        // The shader system the shader lives in (borrowed; null for a handle never initialised):
        // where the effects' tables are served.
        [[nodiscard]] ShaderSystem* System() const noexcept { return m_system; }

        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        [[nodiscard]] u64 Version()
        {
            return (m_system != nullptr) ? m_system->Version(m_name.AsView()) : 0ull;
        }
        [[nodiscard]] rhi::ShaderModule* GetVariant(ShaderStage stage, ShaderFlags flags)
        {
            return (m_system != nullptr) ? m_system->GetVariant(m_name.AsView(), stage, flags)
                                         : nullptr;
        }

    private:
        ShaderSystem* m_system = nullptr; // borrowed
        String m_name;
        String m_depthName;
        String m_pickName;
        bool m_depthProgramsAlways = false;
        ShaderFlags m_vertexVariants = ShaderFlags::None;
    };

    // Builds a ShaderResource from a ShaderSource: registers the sources with the
    // ShaderSystem and invalidates (clears stale variants + bumps version) so a reload
    // is observable. Constructed with the ShaderSystem the shaders live in.
    class ShaderFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required - the composition
        // that creates the factory decides); the shader system is borrowed from the host.
        ShaderFactory(IAllocator& allocator, ShaderSystem& system) noexcept
            : m_allocator(&allocator), m_system(&system)
        {
        }

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ShaderResource::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &ShaderSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
            ShaderSource* source = Cast<ShaderSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }

            // Each program, each stage with its entry point and the variant lattice the cook
            // compiled, so a request canonicalizes onto a variant the cook proved (as the corpus's
            // do).
            for (const ShaderProgram& program : source->Programs())
            {
                m_system->RegisterSource(program.name.AsView(), ShaderStage::Vertex,
                                         program.vertexSource, program.vertexEntryPoint,
                                         program.vertexVariantMask);
                m_system->RegisterSource(program.name.AsView(), ShaderStage::Fragment,
                                         program.fragmentSource, program.fragmentEntryPoint,
                                         program.fragmentVariantMask);
                m_system->InvalidateShader(program.name.AsView()); // drop stale variants, bump version
            }

            RefPtr<ShaderResource> res = MakeRef<ShaderResource>(*m_allocator);
            res->Init(m_system, source->name.AsView());
            res->SetDepthPrograms(source->HasProgram(kDepthProgramSuffix),
                                  source->HasProgram(kPickProgramSuffix),
                                  source->depthProgramsAlways);
            res->SetVertexVariants(static_cast<ShaderFlags>(source->vertexVariantMask));
            return res;
        }

    private:
        IAllocator* m_allocator;
        ShaderSystem* m_system; // borrowed
    };

    RTTI_DEFINE_OBJECT(ShaderSource, "rtti::shaders")
    RTTI_DEFINE_OBJECT(ShaderResource, "rtti::shaders")

} // namespace foundation::shaders

export namespace foundation::shaders
{
    /// Registers the cooked record + product (content-DB construction by type name). Idempotent.
    inline void RegisterShaderResourceTypes()
    {
        GlobalTypeRegistry().Register(ShaderSource::StaticType());
        RegisterSerializable<ShaderSource>();
        GlobalTypeRegistry().Register(ShaderResource::StaticType());
    }

    /// The shaders resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kShadersResourceFactories[] = {
        foundation::resource::FactoryWithService<ShaderResource, ShaderSource, ShaderFactory, ShaderSystem>(),
    };
    inline constexpr foundation::resource::ResourceModule kShadersResourceModule{
        u8"shaders", &RegisterShaderResourceTypes, kShadersResourceFactories,
        sizeof(kShadersResourceFactories) / sizeof(kShadersResourceFactories[0])};
}

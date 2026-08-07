/// Draconic::ShaderResource - the `draconic.shaders.resource` module.
///
/// Shaders as resources: a `ShaderSource` (authored content: name + per-stage HLSL)
/// is built by `ShaderFactory` into a runtime `ShaderResource`. The factory
/// registers the sources with the ShaderSystem and bumps the shader's version, so
/// a reload (via the resource manager) propagates: PSO caches see the version
/// change and rebuild, and a material that depends on this ShaderResource is a
/// normal resource→resource edge. Variants themselves live in the (shared)
/// ShaderSystem; this product is the resource-system handle + version accessor.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.shaders.resource;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.shaders.system;
import draconic.resource;
import draconic.content;

using namespace draconic::foundation;
using namespace draconic::resource;
namespace rhi = draconic::rhi;

export namespace draconic::shaders
{

    // Authored shader: a name + per-stage HLSL. (v1 carries inline source; a path /
    // cooked-bytecode variant can replace the strings later.)
    class ShaderSource final : public ISerializable
    {
        DRACONIC_OBJECT(ShaderSource, ISerializable)
    public:
        String name;
        String vertexSource;
        String fragmentSource;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "name", name);
            draconic::foundation::Serialize(ar, "vertexSource", vertexSource);
            draconic::foundation::Serialize(ar, "fragmentSource", fragmentSource);
        }
    };

    // Runtime handle for a shader: its name + the ShaderSystem it lives in. Version()
    // is the reload signal (bumped each rebuild); GetVariant compiles-on-demand.
    class ShaderResource final : public Object
    {
        DRACONIC_OBJECT(ShaderResource, Object)
    public:
        void Init(ShaderSystem* system, StringView name)
        {
            m_system = system;
            m_name = String(name);
        }

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
    };

    // Builds a ShaderResource from a ShaderSource: registers the sources with the
    // ShaderSystem and invalidates (clears stale variants + bumps version) so a reload
    // is observable. Constructed with the ShaderSystem the shaders live in.
    class ShaderFactory final : public IResourceFactory
    {
    public:
        explicit ShaderFactory(ShaderSystem& system) noexcept : m_system(&system) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ShaderResource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            draconic::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
            ShaderSource* source = Cast<ShaderSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }

            m_system->RegisterSource(source->name.AsView(), ShaderStage::Vertex,
                                     source->vertexSource.AsView());
            m_system->RegisterSource(source->name.AsView(), ShaderStage::Fragment,
                                     source->fragmentSource.AsView());
            m_system->InvalidateShader(source->name.AsView()); // drop stale variants + bump version

            RefPtr<ShaderResource> res = MakeRef<ShaderResource>(DefaultAllocator());
            res->Init(m_system, source->name.AsView());
            return res;
        }

    private:
        ShaderSystem* m_system; // borrowed
    };

    DRACONIC_DEFINE_OBJECT(ShaderSource, "draconic::shaders")
    DRACONIC_DEFINE_OBJECT(ShaderResource, "draconic::shaders")

} // namespace draconic::shaders

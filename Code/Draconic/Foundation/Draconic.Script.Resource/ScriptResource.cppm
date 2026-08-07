// Draconic::ScriptResource - the `draconic.script.resource` module.
//
// Cooked script classes (docs/design/scripting.md §4): a ScriptClass is SOURCE TEXT +
// harvested metadata - never bytecode (Wren has no stable serialized form; compilation
// is fast and happens per script CONTEXT on first use, cached there).
//   * ScriptClassSource - the cooked record: language + class name + source + the
//     property/handler metadata the cook harvested, so the EDITOR renders the
//     inspector without a VM and the RUNTIME dispatches handlers without per-frame
//     method-missing probes.
//   * ScriptClass - the runtime product a component's Ref<ScriptClass> binds.
//   * ScriptClassFactory - metadata parse only; no VM involved here.
//
// Property values (the v1 type set: float, int, bool, string, color, vec3, entity,
// asset:<TypeName>) are carried by ScriptPropertyValue - a small tagged value that is
// the WIRE currency for both harvested defaults and per-behavior overrides (the
// ScriptComponent's hash-keyed override blobs). Entity references are Guids, remapped
// by the prefab machinery like every other entity ref in a payload.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.script.resource;

import draconic.foundation;
import draconic.resource;
import draconic.content;

using namespace draconic::foundation;

export namespace draconic::script
{
    // ---- property values ----

    enum class ScriptPropertyType : u8
    {
        None = 0,
        Float,
        Int,
        Bool,
        String,
        Color,
        Vec3,
        Entity, // Guid, remapped like other entity refs in serialized payloads
        Asset,  // typed resource reference (asset:<TypeName>); Guid identity
    };

    /// Stable name hash for property overrides (rename-safe re-apply keys).
    [[nodiscard]] inline u64 ScriptPropertyNameHash(StringView name) noexcept
    {
        return HashBytes(name.Data(), name.Size());
    }

    /// The parsed "float" / "asset:AudioClip" type string -> kind (+ asset type name).
    [[nodiscard]] inline bool ParseScriptPropertyType(StringView text, ScriptPropertyType& outKind,
                                                      String& outAssetType)
    {
        outAssetType = String{};
        if (text == u8"float")
        {
            outKind = ScriptPropertyType::Float;
            return true;
        }
        if (text == u8"int")
        {
            outKind = ScriptPropertyType::Int;
            return true;
        }
        if (text == u8"bool")
        {
            outKind = ScriptPropertyType::Bool;
            return true;
        }
        if (text == u8"string")
        {
            outKind = ScriptPropertyType::String;
            return true;
        }
        if (text == u8"color")
        {
            outKind = ScriptPropertyType::Color;
            return true;
        }
        if (text == u8"vec3")
        {
            outKind = ScriptPropertyType::Vec3;
            return true;
        }
        if (text == u8"entity")
        {
            outKind = ScriptPropertyType::Entity;
            return true;
        }
        const StringView assetPrefix = u8"asset:";
        if (text.Size() > assetPrefix.Size() && text.SubStr(0, assetPrefix.Size()) == assetPrefix)
        {
            outKind = ScriptPropertyType::Asset;
            outAssetType =
                String(text.SubStr(assetPrefix.Size(), text.Size() - assetPrefix.Size()));
            return true;
        }
        return false;
    }

    // One tagged property value: harvested default OR a component override. `kind`
    // selects which payload field is meaningful; the rest stay at their defaults.
    struct ScriptPropertyValue
    {
        ScriptPropertyType kind = ScriptPropertyType::None;
        f64 number = 0.0;                    // Float / Int
        bool boolean = false;                // Bool
        String text;                         // String
        Color color{1.0f, 1.0f, 1.0f, 1.0f}; // Color
        Float3 vector{0.0f, 0.0f, 0.0f};     // Vec3
        Guid guid;                           // Entity / Asset
    };

    // The tag is written FIRST, so both directions branch on the same kind - the
    // writer/reader stay symmetric by construction (one switch, two modes).
    inline void Serialize(ISerializer& ar, ScriptPropertyValue& v)
    {
        u8 kind = static_cast<u8>(v.kind);
        draconic::foundation::Serialize(ar, "kind", kind);
        v.kind = static_cast<ScriptPropertyType>(kind);
        switch (v.kind)
        {
        case ScriptPropertyType::Float:
        case ScriptPropertyType::Int:
            draconic::foundation::Serialize(ar, "number", v.number);
            break;
        case ScriptPropertyType::Bool:
            draconic::foundation::Serialize(ar, "boolean", v.boolean);
            break;
        case ScriptPropertyType::String:
            draconic::foundation::Serialize(ar, "text", v.text);
            break;
        case ScriptPropertyType::Color:
            draconic::foundation::Serialize(ar, "color", v.color);
            break;
        case ScriptPropertyType::Vec3:
            draconic::foundation::Serialize(ar, "vector", v.vector);
            break;
        case ScriptPropertyType::Entity:
        case ScriptPropertyType::Asset:
            draconic::foundation::Serialize(ar, "guid", v.guid);
            break;
        case ScriptPropertyType::None:
        default:
            break;
        }
    }

    [[nodiscard]] inline bool ScriptPropertyValuesEqual(const ScriptPropertyValue& a,
                                                        const ScriptPropertyValue& b) noexcept
    {
        if (a.kind != b.kind)
        {
            return false;
        }
        switch (a.kind)
        {
        case ScriptPropertyType::Float:
        case ScriptPropertyType::Int:
            return a.number == b.number;
        case ScriptPropertyType::Bool:
            return a.boolean == b.boolean;
        case ScriptPropertyType::String:
            return a.text == b.text;
        case ScriptPropertyType::Color:
            return a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b &&
                   a.color.a == b.color.a;
        case ScriptPropertyType::Vec3:
            return a.vector.x == b.vector.x && a.vector.y == b.vector.y && a.vector.z == b.vector.z;
        case ScriptPropertyType::Entity:
        case ScriptPropertyType::Asset:
            return a.guid == b.guid;
        case ScriptPropertyType::None:
        default:
            return true;
        }
    }

    // ---- harvested metadata ----

    struct ScriptPropertyDesc
    {
        String name;
        u64 hash = 0; // ScriptPropertyNameHash(name)
        ScriptPropertyType type = ScriptPropertyType::None;
        String assetType; // Asset only: the product type name ("AudioClip")
        ScriptPropertyValue defaultValue;
        String description; // inspector tooltip
    };

    inline void Serialize(ISerializer& ar, ScriptPropertyDesc& d)
    {
        draconic::foundation::Serialize(ar, "name", d.name);
        draconic::foundation::Serialize(ar, "hash", d.hash);
        u8 type = static_cast<u8>(d.type);
        draconic::foundation::Serialize(ar, "type", type);
        d.type = static_cast<ScriptPropertyType>(type);
        draconic::foundation::Serialize(ar, "assetType", d.assetType);
        draconic::foundation::Serialize(ar, "default", d.defaultValue);
        draconic::foundation::Serialize(ar, "description", d.description);
    }

    // ---- the cooked record ----

    class ScriptClassSource : public ISerializable
    {
        DRACONIC_OBJECT(ScriptClassSource, ISerializable)
    public:
        String language;   // backend id ("wren"); resolved via the ScriptBackendRegistry
        String className;  // empty = a ScriptClass-less utility module
        String sourceName; // the source file identity ("Mover.as"): the AngelScript section
                           // name + the editor's breakpoint key - cook-stamped = asset fileName
        String source;     // full script source text (no bytecode - see the header note)
        Array<ScriptPropertyDesc> properties;
        Array<String> handlers; // declared lifecycle/event handlers (dispatch gate)
        bool usesCoroutines =
            false; // harvested: the class starts coroutines (cancel-on-teardown gate)

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "language", language);
            draconic::foundation::Serialize(ar, "className", className);
            draconic::foundation::Serialize(ar, "sourceName", sourceName);
            draconic::foundation::Serialize(ar, "source", source);
            draconic::foundation::Serialize(ar, "properties", properties);
            draconic::foundation::Serialize(ar, "handlers", handlers);
            draconic::foundation::Serialize(ar, "usesCoroutines", usesCoroutines);
        }
    };

    // ---- the runtime product ----

    class ScriptClass final : public Object
    {
        DRACONIC_OBJECT(ScriptClass, Object)
    public:
        String language;
        String className;
        String sourceName; // the source file identity: the AngelScript section name a
                           // breakpoint keys on (== EditorContext::ScriptBreakpoint.file)
        String source;
        Array<ScriptPropertyDesc> properties;
        Array<String> handlers;
        bool usesCoroutines = false; // the class starts coroutines (cancel on disable/destroy)

        [[nodiscard]] bool HasHandler(StringView name) const
        {
            for (const String& handler : handlers)
            {
                if (handler.AsView() == name)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] const ScriptPropertyDesc* FindProperty(u64 nameHash) const
        {
            for (const ScriptPropertyDesc& property : properties)
            {
                if (property.hash == nameHash)
                {
                    return &property;
                }
            }
            return nullptr;
        }

        /// Stable profiler label ("Script " + class name) - the profiler stores the
        /// POINTER, and this product outlives the frames it labels.
        [[nodiscard]] const char* ProfileName() const noexcept
        {
            return reinterpret_cast<const char*>(m_profileName.CStr());
        }
        void BuildProfileName()
        {
            m_profileName = String(u8"Script ");
            m_profileName += className.IsEmpty() ? StringView(u8"(module)") : className.AsView();
        }

    private:
        String m_profileName;
    };

    class ScriptClassFactory final : public draconic::resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ScriptClass::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(draconic::resource::ResourceManager&,
                                            draconic::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            ScriptClassSource* source = Cast<ScriptClassSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<ScriptClass> product = MakeRef<ScriptClass>(DefaultAllocator());
            product->language = source->language;
            product->className = source->className;
            product->sourceName = source->sourceName;
            product->source = source->source;
            product->properties = source->properties;
            product->handlers = source->handlers;
            product->usesCoroutines = source->usesCoroutines;
            product->BuildProfileName();
            return product;
        }
    };

    // Registers the cooked record + product types (content-DB construction by type name).
    inline void RegisterScriptResource()
    {
        GlobalTypeRegistry().Register(ScriptClassSource::StaticType());
        RegisterSerializable<ScriptClassSource>();
        GlobalTypeRegistry().Register(ScriptClass::StaticType());
    }

    DRACONIC_DEFINE_OBJECT(ScriptClassSource, "draconic::script")
    DRACONIC_DEFINE_OBJECT(ScriptClass, "draconic::script")
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.Resource - the `foundation.script.resource` module.
//
// Cooked script classes: a ScriptClass is source text and harvested metadata, plus compiled
// bytecode when the language has a stable bytecode and the cook produced it.
//   * ScriptClassSource - the cooked record: language, class name, source, bytecode and the
//     property and handler metadata the cook harvested, so the editor renders the
//     inspector without a VM and the runtime dispatches handlers without per-frame
//     method-missing probes.
//   * ScriptClass - the runtime product a component's Ref<ScriptClass> binds.
//   * ScriptClassFactory - metadata parse only; no VM involved here.
//
// Property values (float, int, bool, string, color, vec3, entity, asset:<TypeName>) are
// carried by ScriptPropertyValue, a small tagged value that is the serialized form of
// both harvested defaults and per-behavior overrides (the
// ScriptComponent's hash-keyed override blobs). Entity references are Guids, remapped
// by the prefab machinery like every other entity ref in a payload.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.script.resource;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;

export namespace foundation::script
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

    /// One authored type name and the kind it means. The one table both directions read: the
    /// parser below and anything that lists the kinds (the scene format reference).
    struct ScriptPropertyTypeEntry
    {
        StringView name;
        ScriptPropertyType kind;
    };

    /// The asset kind is authored as "asset:<TypeName>": this prefix, then the product type name.
    inline constexpr StringView kScriptAssetTypePrefix = u8"asset:";

    /// Every authored kind, in declaration order (`None` has no spelling).
    [[nodiscard]] inline Span<const ScriptPropertyTypeEntry> ScriptPropertyTypeNames() noexcept
    {
        static constexpr ScriptPropertyTypeEntry kNames[] = {
            {u8"float", ScriptPropertyType::Float},   {u8"int", ScriptPropertyType::Int},
            {u8"bool", ScriptPropertyType::Bool},     {u8"string", ScriptPropertyType::String},
            {u8"color", ScriptPropertyType::Color},   {u8"vec3", ScriptPropertyType::Vec3},
            {u8"entity", ScriptPropertyType::Entity}, {u8"asset", ScriptPropertyType::Asset},
        };
        return Span<const ScriptPropertyTypeEntry>{kNames, sizeof(kNames) / sizeof(kNames[0])};
    }

    /// The authored spelling of `kind` ("float", "asset" for the prefixed form); empty for None.
    [[nodiscard]] inline StringView ScriptPropertyTypeName(ScriptPropertyType kind) noexcept
    {
        for (const ScriptPropertyTypeEntry& entry : ScriptPropertyTypeNames())
        {
            if (entry.kind == kind)
            {
                return entry.name;
            }
        }
        return {};
    }

    /// Parses a "float" or "asset:AudioClip" type string into its kind (and asset type name).
    [[nodiscard]] inline bool ParseScriptPropertyType(StringView text, ScriptPropertyType& outKind,
                                                      String& outAssetType)
    {
        outAssetType = String{};
        for (const ScriptPropertyTypeEntry& entry : ScriptPropertyTypeNames())
        {
            if (entry.kind == ScriptPropertyType::Asset)
            {
                const StringView prefix = kScriptAssetTypePrefix;
                if (text.Size() > prefix.Size() && text.SubStr(0, prefix.Size()) == prefix)
                {
                    outKind = ScriptPropertyType::Asset;
                    outAssetType = String(text.SubStr(prefix.Size(), text.Size() - prefix.Size()));
                    return true;
                }
                continue;
            }
            if (text == entry.name)
            {
                outKind = entry.kind;
                return true;
            }
        }
        return false;
    }

    // One tagged property value: a harvested default or a component override. `kind`
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

    // The tag is written first, so both directions branch on the same kind and the
    // writer and reader stay symmetric (one switch, two modes).
    inline void Serialize(ISerializer& ar, ScriptPropertyValue& v)
    {
        u8 kind = static_cast<u8>(v.kind);
        foundation::core::Serialize(ar, "kind", kind);
        v.kind = static_cast<ScriptPropertyType>(kind);
        switch (v.kind)
        {
        case ScriptPropertyType::Float:
        case ScriptPropertyType::Int:
            foundation::core::Serialize(ar, "number", v.number);
            break;
        case ScriptPropertyType::Bool:
            foundation::core::Serialize(ar, "boolean", v.boolean);
            break;
        case ScriptPropertyType::String:
            foundation::core::Serialize(ar, "text", v.text);
            break;
        case ScriptPropertyType::Color:
            foundation::core::Serialize(ar, "color", v.color);
            break;
        case ScriptPropertyType::Vec3:
            foundation::core::Serialize(ar, "vector", v.vector);
            break;
        case ScriptPropertyType::Entity:
        case ScriptPropertyType::Asset:
            foundation::core::Serialize(ar, "guid", v.guid);
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
        // Kept the same on every peer of a networked game: the host sends the live value of a
        // networked entity's behaviour, clients write it in (AngelScript: a `replicated` token in the
        // field's metadata; Luau: the name in the class's `replicated` list). Numbers, bools, text,
        // colours and vectors; an entity or asset reference is refused at cook.
        bool replicated = false;
    };

    inline void Serialize(ISerializer& ar, ScriptPropertyDesc& d)
    {
        foundation::core::Serialize(ar, "name", d.name);
        foundation::core::Serialize(ar, "hash", d.hash);
        u8 type = static_cast<u8>(d.type);
        foundation::core::Serialize(ar, "type", type);
        d.type = static_cast<ScriptPropertyType>(type);
        foundation::core::Serialize(ar, "assetType", d.assetType);
        foundation::core::Serialize(ar, "default", d.defaultValue);
        foundation::core::Serialize(ar, "description", d.description);
        foundation::core::Serialize(ar, "replicated", d.replicated);
    }

    // ---- the cooked record ----

    class ScriptClassSource : public ISerializable
    {
        RTTI_OBJECT(ScriptClassSource, ISerializable)
    public:
        String language;   // backend id ("angelscript"); resolved via the ScriptBackendRegistry
        String className;  // empty = a ScriptClass-less utility module
        String sourceName; // the source file identity ("Mover.as"): the AngelScript section
                           // name and the editor's breakpoint key; the cook stamps the asset fileName
        String source;     // full script source text (kept for dev-mode hot reload)
        // Compiled bytecode for the pack (the player loads this, it has no compiler), when the
        // language has a stable bytecode (ScriptCapabilities::Bytecode) and it cooked; empty
        // for source-only backends or a cook that produced none. Version-locked:
        // the cook fingerprint carries the compiler version so a vendor bump recooks.
        Array<byte> bytecode;
        Array<ScriptPropertyDesc> properties;
        Array<String> handlers; // declared lifecycle/event handlers (dispatch gate)
        bool usesCoroutines =
            false; // harvested: the class starts coroutines (cancel-on-teardown gate)

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "language", language);
            foundation::core::Serialize(ar, "className", className);
            foundation::core::Serialize(ar, "sourceName", sourceName);
            foundation::core::Serialize(ar, "source", source);
            foundation::core::Serialize(ar, "bytecode", bytecode);
            foundation::core::Serialize(ar, "properties", properties);
            foundation::core::Serialize(ar, "handlers", handlers);
            foundation::core::Serialize(ar, "usesCoroutines", usesCoroutines);
        }
    };

    // ---- the runtime product ----

    class ScriptClass final : public Object
    {
        RTTI_OBJECT(ScriptClass, Object)
    public:
        String language;
        String className;
        String sourceName; // the source file identity: the AngelScript section name a
                           // breakpoint keys on (== EditorContext::ScriptBreakpoint.file)
        String source;
        Array<byte> bytecode;        // compiled bytecode (empty = source-only); see ScriptClassSource
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

        /// Stable profiler label ("Script " and the class name): the profiler stores the
        /// pointer, and this product outlives the frames it labels.
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

    class ScriptClassFactory final : public foundation::resource::IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required; the
        // application that registers the factory decides).
        explicit ScriptClassFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ScriptClass::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &ScriptClassSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(foundation::resource::ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            ScriptClassSource* source = Cast<ScriptClassSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<ScriptClass> product = MakeRef<ScriptClass>((*m_allocator));
            product->language = source->language;
            product->className = source->className;
            product->sourceName = source->sourceName;
            product->source = source->source;
            product->bytecode = source->bytecode;
            product->properties = source->properties;
            product->handlers = source->handlers;
            product->usesCoroutines = source->usesCoroutines;
            product->BuildProfileName();
            return product;
        }
    
    private:
        IAllocator* m_allocator;
    };

    // Registers the cooked record and product types (content-DB construction by type name).
    inline void RegisterScriptResource()
    {
        // Data version 1: each property's replicated flag. A record cooked before it is in
        // another layout; the strict reader refuses it and the script recooks. No REFLECT block
        // owns this type, so the version is patched on the TypeInfo.
        const_cast<TypeInfo&>(ScriptClassSource::StaticType()).dataVersion = 1;
        GlobalTypeRegistry().Register(ScriptClassSource::StaticType());
        RegisterSerializable<ScriptClassSource>();
        GlobalTypeRegistry().Register(ScriptClass::StaticType());
    }

    RTTI_DEFINE_OBJECT(ScriptClassSource, "rtti::script")
    RTTI_DEFINE_OBJECT(ScriptClass, "rtti::script")
}

export namespace foundation::script
{
    /// The script resource module: the factories the engine composition takes from this
    /// library.
    inline constexpr foundation::resource::ResourceFactoryDesc kScriptResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<ScriptClass, ScriptClassSource, ScriptClassFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kScriptResourceModule{
        u8"script", &RegisterScriptResource, kScriptResourceFactories,
        sizeof(kScriptResourceFactories) / sizeof(kScriptResourceFactories[0])};
}

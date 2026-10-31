// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Json - :value partition
//
// JsonValue: a hand-rolled JSON DOM value (null, bool, number, string, array, object), the
// engine's boundary codec for JSON (the MCP wire protocol and game-chosen data interchange). Value
// semantics throughout: accessors return owned copies and mutators copy in, so no interior pointer
// into a reallocatable document ever escapes. Numbers are f64, matching both script backends'
// number model. Objects preserve insertion order (parallel key/value arrays), which JSON-RPC/MCP
// determinism wants; lookups are linear (fine for wire-sized objects).
//
// Not an engine data format: there is no ISerializer backend (unlike foundation.xml).

module;
#include "Core/Prelude.h"

export module foundation.json:value;

import foundation.core;

using namespace foundation::core;

export namespace foundation::json
{
    enum class JsonType : u8
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    class JsonValue
    {
        JsonType m_type = JsonType::Null;
        bool m_bool = false;
        f64 m_number = 0.0;
        String m_string;
        Array<JsonValue> m_items; // Array elements OR (when Object) values, parallel to m_keys
        Array<String> m_keys;     // Object member names (empty unless Object)

    public:
        JsonValue() = default;

        // --- Factories (also the parser's building blocks; script-facing) ------------------------
        [[nodiscard]] static JsonValue MakeNull() noexcept { return JsonValue(); }
        [[nodiscard]] static JsonValue MakeBool(bool b);
        [[nodiscard]] static JsonValue MakeNumber(f64 n);
        [[nodiscard]] static JsonValue MakeString(String s);
        [[nodiscard]] static JsonValue MakeArray();
        [[nodiscard]] static JsonValue MakeObject();

        // --- Type -------------------------------------------------------------------------------
        [[nodiscard]] JsonType Type() const noexcept { return m_type; }
        [[nodiscard]] bool IsNull() const noexcept { return m_type == JsonType::Null; }
        [[nodiscard]] bool IsBool() const noexcept { return m_type == JsonType::Bool; }
        [[nodiscard]] bool IsNumber() const noexcept { return m_type == JsonType::Number; }
        [[nodiscard]] bool IsString() const noexcept { return m_type == JsonType::String; }
        [[nodiscard]] bool IsArray() const noexcept { return m_type == JsonType::Array; }
        [[nodiscard]] bool IsObject() const noexcept { return m_type == JsonType::Object; }

        // --- Scalar accessors (typed, with a fallback) ------------------------------------------
        [[nodiscard]] bool AsBool(bool fallback = false) const noexcept
        {
            return m_type == JsonType::Bool ? m_bool : fallback;
        }
        [[nodiscard]] f64 AsNumber(f64 fallback = 0.0) const noexcept
        {
            return m_type == JsonType::Number ? m_number : fallback;
        }
        [[nodiscard]] i64 AsInt(i64 fallback = 0) const noexcept
        {
            return m_type == JsonType::Number ? static_cast<i64>(m_number) : fallback;
        }
        // The string value as an owned copy ("" if not a string). Owned rather than a view so it
        // marshals cleanly to script and never aliases the document.
        [[nodiscard]] String AsString() const { return m_type == JsonType::String ? m_string : String(); }

        // --- Array / Object shared: element count -----------------------------------------------
        [[nodiscard]] i64 Count() const noexcept { return static_cast<i64>(m_items.Size()); }

        // --- Array ------------------------------------------------------------------------------
        // Element by index as an owned copy (null if out of range), never an interior reference.
        [[nodiscard]] JsonValue At(i64 index) const;
        // Append (coerces a null/non-array into an empty array first).
        void Add(JsonValue value);

        // --- Object -----------------------------------------------------------------------------
        [[nodiscard]] bool Has(String key) const;
        // Member by name as an owned copy (null if absent).
        [[nodiscard]] JsonValue Get(String key) const;
        // Set/overwrite a member (coerces a null/non-object into an empty object first).
        void Set(String key, JsonValue value);
        // Member names in insertion order (empty unless Object).
        [[nodiscard]] const Array<String>& Keys() const noexcept { return m_keys; }
        // Member name by index as an owned copy ("" if out of range): the script-friendly iterator
        // (pair with Count()) that avoids marshaling the whole key array.
        [[nodiscard]] String KeyAt(i64 index) const;

        // --- Convenience (defined in ReflectionImpl.cpp; forward to :writer and :parser) --------
        // Serialize to a JSON string (compact, or 2-space pretty).
        [[nodiscard]] String ToString(bool pretty = false) const;
        // Parse JSON text; returns a Null value on any error (script never sees a half-value). Use
        // foundation::json::Parse for the erroring wire path.
        [[nodiscard]] static JsonValue Parse(String text);

        // --- Internal (parser/writer build directly; not part of the script surface) ------------
        [[nodiscard]] const Array<JsonValue>& Items() const noexcept { return m_items; }

    private:
        [[nodiscard]] usize IndexOfKey(StringView key) const noexcept;
    };

    // ---- Out-of-line: everything that constructs/copies a JsonValue element (needs the complete
    //      type, so it cannot sit in the class body where JsonValue is still incomplete). --------

    inline JsonValue JsonValue::MakeBool(bool b)
    {
        JsonValue v;
        v.m_type = JsonType::Bool;
        v.m_bool = b;
        return v;
    }
    inline JsonValue JsonValue::MakeNumber(f64 n)
    {
        JsonValue v;
        v.m_type = JsonType::Number;
        v.m_number = n;
        return v;
    }
    inline JsonValue JsonValue::MakeString(String s)
    {
        JsonValue v;
        v.m_type = JsonType::String;
        v.m_string = Move(s);
        return v;
    }
    inline JsonValue JsonValue::MakeArray()
    {
        JsonValue v;
        v.m_type = JsonType::Array;
        return v;
    }
    inline JsonValue JsonValue::MakeObject()
    {
        JsonValue v;
        v.m_type = JsonType::Object;
        return v;
    }

    inline JsonValue JsonValue::At(i64 index) const
    {
        if (m_type != JsonType::Array || index < 0 || static_cast<usize>(index) >= m_items.Size())
        {
            return JsonValue();
        }
        return m_items[static_cast<usize>(index)];
    }
    inline void JsonValue::Add(JsonValue value)
    {
        if (m_type != JsonType::Array)
        {
            *this = JsonValue();
            m_type = JsonType::Array;
        }
        m_items.PushBack(Move(value));
    }

    inline usize JsonValue::IndexOfKey(StringView key) const noexcept
    {
        for (usize i = 0; i < m_keys.Size(); ++i)
        {
            if (m_keys[i].AsView() == key)
            {
                return i;
            }
        }
        return static_cast<usize>(-1);
    }
    inline bool JsonValue::Has(String key) const
    {
        return m_type == JsonType::Object && IndexOfKey(key.AsView()) != static_cast<usize>(-1);
    }
    inline JsonValue JsonValue::Get(String key) const
    {
        if (m_type != JsonType::Object)
        {
            return JsonValue();
        }
        const usize i = IndexOfKey(key.AsView());
        return i == static_cast<usize>(-1) ? JsonValue() : m_items[i];
    }
    inline void JsonValue::Set(String key, JsonValue value)
    {
        if (m_type != JsonType::Object)
        {
            *this = JsonValue();
            m_type = JsonType::Object;
        }
        const usize i = IndexOfKey(key.AsView());
        if (i == static_cast<usize>(-1))
        {
            m_keys.PushBack(Move(key));
            m_items.PushBack(Move(value));
        }
        else
        {
            m_items[i] = Move(value);
        }
    }

    inline String JsonValue::KeyAt(i64 index) const
    {
        if (m_type != JsonType::Object || index < 0 || static_cast<usize>(index) >= m_keys.Size())
        {
            return String();
        }
        return m_keys[static_cast<usize>(index)];
    }
}

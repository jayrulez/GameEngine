// Draconic Foundation - :enum_reflection partition
//
// EnumBuilder patches an enum's TypeOf<E>() TypeInfo in place (name/id +
// enumerator list); plus IsEnum / Enumerators / EnumValueName / EnumValueByName.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:enum_reflection;

import :base;
import :type_info;
import :array;
import :span;

export namespace draconic::foundation
{
    // =======================================================================
    // Enum reflection (phase e). EnumBuilder patches the enum's TypeOf<E>()
    // TypeInfo in place (adding enumerators + a qualified name/id), so property
    // types that point at it gain the enumerator list regardless of order.
    // =======================================================================
    namespace detail
    {
        template <typename E>
        [[nodiscard]] Array<EnumValue>& EnumStorage() noexcept
        {
            static Array<EnumValue> values;
            return values;
        }

        [[nodiscard]] inline bool NameEquals(const char* a, const char* b) noexcept
        {
            usize i = 0;
            while (a[i] != '\0' && a[i] == b[i])
            {
                ++i;
            }
            return a[i] == b[i];
        }
    }

    template <typename E>
    class EnumBuilder
    {
    public:
        EnumBuilder(const char* name, const char* namespaceName) noexcept
            : m_name(name), m_namespace(namespaceName)
        {
        }

        EnumBuilder& Value(const char* name, E value)
        {
            m_values.PushBack(EnumValue{name, static_cast<i64>(value)});
            return *this;
        }

        void Build()
        {
            Array<EnumValue>& storage = detail::EnumStorage<E>();
            storage = Move(m_values);

            TypeInfo& info = const_cast<TypeInfo&>(TypeOf<E>());
            info.name = m_name;
            info.namespaceName = m_namespace;
            info.id = ComputeTypeId(m_namespace, m_name);
            info.enumerators = storage.Data();
            info.enumeratorCount = static_cast<u32>(storage.Size());
        }

    private:
        const char* m_name;
        const char* m_namespace;
        Array<EnumValue> m_values;
    };

    [[nodiscard]] inline bool IsEnum(const TypeInfo& type) noexcept
    {
        return type.enumeratorCount > 0;
    }

    [[nodiscard]] inline Span<const EnumValue> Enumerators(const TypeInfo& type) noexcept
    {
        return Span<const EnumValue>{type.enumerators, type.enumeratorCount};
    }

    [[nodiscard]] inline usize EnumeratorCount(const TypeInfo& type) noexcept
    {
        return type.enumeratorCount;
    }
    [[nodiscard]] inline const EnumValue& EnumeratorAt(const TypeInfo& type, usize index) noexcept
    {
        DRACONIC_ASSERT(index < type.enumeratorCount);
        return type.enumerators[index];
    }

    // Name for an enum value, or nullptr if not found.
    [[nodiscard]] inline const char* EnumValueName(const TypeInfo& type, i64 value) noexcept
    {
        for (u32 i = 0; i < type.enumeratorCount; ++i)
        {
            if (type.enumerators[i].value == value)
            {
                return type.enumerators[i].name;
            }
        }
        return nullptr;
    }

    // Looks up the integer value for an enumerator name; false if not found.
    [[nodiscard]] inline bool EnumValueByName(const TypeInfo& type, const char* name,
                                              i64& outValue) noexcept
    {
        for (u32 i = 0; i < type.enumeratorCount; ++i)
        {
            if (detail::NameEquals(type.enumerators[i].name, name))
            {
                outValue = type.enumerators[i].value;
                return true;
            }
        }
        return false;
    }
}

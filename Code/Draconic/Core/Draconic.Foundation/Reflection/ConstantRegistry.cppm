// Draconic Foundation - :constant_registry partition
//
// A registry of named constants that aren't members of a struct - e.g.
// namespace-level math constants (draconic::foundation::kPi, kEpsilon). Per-type
// constants live on the TypeInfo (see ConstantInfo / TypeBuilder::Constant);
// this is for the free-standing ones. Registered explicitly; queryable by
// qualified name and enumerable for binding generators.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:constant_registry;

import :base;
import :array;
import :span;
import :type_info;
import :variant;

export namespace draconic::foundation
{
    struct NamedConstant
    {
        const char* name;
        const char* namespaceName;
        const TypeInfo* type;
        Variant value;
    };

    class ConstantRegistry
    {
    public:
        // Idempotent by qualified name.
        void Register(const char* namespaceName, const char* name, const TypeInfo* type,
                      Variant value)
        {
            if (Find(namespaceName, name) != nullptr)
            {
                return;
            }
            m_constants.PushBack(NamedConstant{name, namespaceName, type, Move(value)});
        }

        [[nodiscard]] const NamedConstant* Find(const char* namespaceName,
                                                const char* name) const noexcept
        {
            for (usize i = 0; i < m_constants.Size(); ++i)
            {
                const NamedConstant& c = m_constants[i];
                if (NameEquals(c.namespaceName, namespaceName) && NameEquals(c.name, name))
                {
                    return &c;
                }
            }
            return nullptr;
        }

        [[nodiscard]] usize Count() const noexcept { return m_constants.Size(); }
        [[nodiscard]] const NamedConstant& At(usize index) const noexcept
        {
            DRACONIC_ASSERT(index < m_constants.Size());
            return m_constants[index];
        }
        [[nodiscard]] Span<const NamedConstant> All() const noexcept
        {
            return Span<const NamedConstant>{m_constants.Data(), m_constants.Size()};
        }

    private:
        [[nodiscard]] static bool NameEquals(const char* a, const char* b) noexcept
        {
            usize i = 0;
            while (a[i] != '\0' && a[i] == b[i])
            {
                ++i;
            }
            return a[i] == b[i];
        }

        Array<NamedConstant> m_constants;
    };

    [[nodiscard]] ConstantRegistry& GlobalConstantRegistry() noexcept
    {
        static ConstantRegistry instance;
        return instance;
    }
}

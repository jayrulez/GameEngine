// Draconic Foundation - :variant partition
//
// Variant  - an owned, type-erased value (small-buffer optimized) used for
//            property values, method args/returns. Two modes:
//              * value mode  - owns a copy of any value type T (SBO + heap).
//              * object mode - owns a RefPtr<Object> and reports the object's
//                dynamic GetType() (so scripting can wrap it as the right type).
// Instance - a borrowed { void*, TypeInfo* } target for member access. Variant
//            ALWAYS owns its value (no reference mode); see §4.10.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <cstddef>
#include <cstring>
#include <type_traits>

export module draconic.foundation:variant;

import :base;
import :allocator;
import :ref_counted;
import :type_info;
import :object;

namespace draconic::foundation::detail
{
    template <typename T>
    struct VariantOps
    {
        static void Copy(void* dst, const void* src)
        {
            Construct<T>(dst, *static_cast<const T*>(src));
        }
        static void Move(void* dst, void* src)
        {
            Construct<T>(dst, draconic::foundation::Move(*static_cast<T*>(src)));
        }
        static void Destroy(void* obj) { Destruct(static_cast<T*>(obj)); }
    };

    struct VariantVTable
    {
        void (*copy)(void* dst, const void* src);
        void (*move)(void* dst, void* src);
        void (*destroy)(void* obj);
        const TypeInfo* (*typeInfo)();
        u32 size;
        u32 align;
    };

    template <typename T>
    const TypeInfo* VariantTypeInfo() noexcept
    {
        return &TypeOf<T>();
    }

    template <typename T>
    inline constexpr VariantVTable kVariantVTable{
        &VariantOps<T>::Copy, &VariantOps<T>::Move,        &VariantOps<T>::Destroy,
        &VariantTypeInfo<T>,  static_cast<u32>(sizeof(T)), static_cast<u32>(alignof(T))};

    // Detects RefPtr<U> where U derives Object - routed to Variant's object mode.
    template <typename T>
    struct ObjectRef
    {
        static constexpr bool value = false;
    };
    template <typename U>
    struct ObjectRef<RefPtr<U>>
    {
        static constexpr bool value = std::is_base_of_v<Object, U>;
        using Pointee = U;
    };
}

export namespace draconic::foundation
{
    // Reflection mutation generation: bumped whenever a reflected container is STRUCTURALLY mutated
    // (create/emplace/remove/move - the reflection wrappers bump it centrally so registrants cannot
    // forget). A Variant borrow captures the current value and revalidates on every dereference; any
    // structural mutation between capture and use invalidates ALL live borrows (coarse by design - a
    // borrow is a cached raw pointer, same rule as the bind-group-cache generation). Main-thread:
    // reflection mutation is main-thread by the async rules. Lives here (the lowest partition both the
    // Variant borrow and the :reflection container wrappers share) to avoid a partition cycle.
    [[nodiscard]] inline u64& ReflectionMutationGenerationRef() noexcept
    {
        static u64 generation = 1; // start at 1 so a default (0) borrow generation never matches
        return generation;
    }
    [[nodiscard]] inline u64 GlobalReflectionMutationGeneration() noexcept
    {
        return ReflectionMutationGenerationRef();
    }
    inline void BumpReflectionMutationGeneration() noexcept { ++ReflectionMutationGenerationRef(); }

    class Variant
    {
    public:
        Variant() noexcept = default;

        template <typename T>
        [[nodiscard]] static Variant From(T value)
        {
            if constexpr (std::is_same_v<T, Variant>)
            {
                // Pass-through: a reflected method that returns a Variant hands back a runtime-typed
                // value (e.g. entity.get(Type) -> a RESOLVE-mode component ref). Preserve its dynamic
                // type instead of boxing a Variant inside a Variant, so the backend wraps it as the
                // right script class.
                return value;
            }
            else if constexpr (detail::ObjectRef<T>::value)
            {
                using U = typename detail::ObjectRef<T>::Pointee;
                Variant v;
                // Dynamic type for non-null; static type as a fallback for null.
                v.m_dynamicType =
                    (value.Get() != nullptr) ? value.Get()->GetType() : &U::StaticType();
                v.m_vtable = &detail::kVariantVTable<RefPtr<Object>>;
                void* dst = v.AllocateStorage(sizeof(RefPtr<Object>), alignof(RefPtr<Object>));
                Construct<RefPtr<Object>>(dst, RefPtr<Object>(value));
                return v;
            }
            else
            {
                Variant v;
                v.m_vtable = &detail::kVariantVTable<T>;
                void* dst = v.AllocateStorage(sizeof(T), alignof(T));
                Construct<T>(dst, Move(value));
                return v;
            }
        }

        // Wrap an object (owning). Reports the object's dynamic type.
        [[nodiscard]] static Variant FromObject(const RefPtr<Object>& object)
        {
            return From<RefPtr<Object>>(object);
        }

        // A BORROW: a handle over a member/element ADDRESS (`addr`, reflected `type`) that pins its
        // owning object graph alive via `parent`'s root and revalidates against the mutation
        // generation on every dereference. Internally an object-mode Variant whose SBO holds the
        // KEEP-ALIVE root RefPtr; `m_borrowAddr` overrides ToInstance to point at the borrowed member.
        // The root is `parent`'s object (object-mode parent) or `parent`'s own root (borrow-mode
        // parent - so N-level descent pins the same root). A value-mode parent has no object to pin,
        // so the borrow is REFUSED (empty) - no script author can reason about an engine-temporary's
        // lifetime, and all facade roots are object-mode anyway (Fable ruling).
        [[nodiscard]] static Variant Borrow(void* addr, const TypeInfo* type, const Variant& parent)
        {
            if (addr == nullptr || type == nullptr)
            {
                return Variant{};
            }
            Variant v;
            if (parent.IsBorrow())
            {
                v = parent; // copies the keep-alive root RefPtr (and vtable/dynamic type)
            }
            else if (parent.m_dynamicType != nullptr) // owned object (not a borrow): pin it
            {
                v = From<RefPtr<Object>>(RefPtr<Object>(parent.AsObject()));
            }
            else
            {
                return Variant{}; // value-mode parent: refuse
            }
            v.m_borrowAddr = addr;
            v.m_dynamicType = type; // Type() reports the borrowed member's type
            v.m_borrowGeneration = GlobalReflectionMutationGeneration();
            return v;
        }

        // A RE-RESOLVING handle: `type` is the reflected type; `resolver(v)` returns the value's
        // CURRENT address each deref (or null if gone), reading its context from `v`'s inline storage
        // (seeded from `ctx`). For LIVE scene state a borrow cannot safely hold: the component pool
        // swap-removes, so the address must be recomputed, never cached (Fable Correction 1). `Ctx`
        // is a trivially-copyable POD <= kInlineSize (e.g. {Scene*, EntityHandle, ComponentManagerBase*});
        // the resolver, defined in the OWNING layer (foundation stays generic), casts ResolveContext() back
        // to Ctx. Weak by design: a dead entity resolves to null -> the deref is a clean no-op.
        template <typename Ctx>
        [[nodiscard]] static Variant Resolving(const TypeInfo* type,
                                               void* (*resolver)(const Variant&), const Ctx& ctx)
        {
            static_assert(std::is_trivially_copyable_v<Ctx>,
                          "resolve context must be trivially copyable");
            static_assert(sizeof(Ctx) <= kInlineSize && alignof(Ctx) <= kInlineAlign,
                          "resolve context must fit Variant inline storage");
            if (type == nullptr || resolver == nullptr)
            {
                return Variant{};
            }
            Variant v;
            v.m_dynamicType = type;
            v.m_resolver = resolver;
            std::memcpy(v.m_storage.inlineBytes, &ctx, sizeof(Ctx));
            return v;
        }

        // Resolve mode (see Resolving): a live re-resolving handle over external value state.
        [[nodiscard]] bool IsResolving() const noexcept { return m_resolver != nullptr; }
        // Raw resolve-context bytes; the owning layer casts back to its Ctx. Valid in resolve mode.
        [[nodiscard]] const void* ResolveContext() const noexcept { return m_storage.inlineBytes; }
        // The value's CURRENT address, or null (dead). Valid in resolve mode; null otherwise.
        [[nodiscard]] void* Resolve() const
        {
            return m_resolver != nullptr ? m_resolver(*this) : nullptr;
        }

        Variant(const Variant& other)
            : m_dynamicType(other.m_dynamicType), m_vtable(other.m_vtable),
              m_borrowAddr(other.m_borrowAddr), m_borrowGeneration(other.m_borrowGeneration),
              m_resolver(other.m_resolver)
        {
            if (m_vtable != nullptr)
            {
                void* dst = AllocateStorage(m_vtable->size, m_vtable->align);
                m_vtable->copy(dst, other.Data());
            }
            else if (m_resolver != nullptr)
            {
                m_storage = other.m_storage; // resolve mode: trivially copy the inline context
            }
        }

        Variant(Variant&& other) noexcept
            : m_dynamicType(other.m_dynamicType), m_vtable(other.m_vtable),
              m_borrowAddr(other.m_borrowAddr), m_borrowGeneration(other.m_borrowGeneration),
              m_resolver(other.m_resolver)
        {
            if (m_vtable != nullptr)
            {
                if (other.m_isHeap)
                {
                    m_isHeap = true;
                    m_storage.heap = other.m_storage.heap; // steal
                }
                else
                {
                    void* dst = AllocateStorage(m_vtable->size, m_vtable->align);
                    m_vtable->move(dst, other.Data());
                    m_vtable->destroy(other.Data());
                }
            }
            else if (m_resolver != nullptr)
            {
                m_storage = other.m_storage; // resolve mode: POD context, move == copy
            }
            other.m_vtable = nullptr;
            other.m_isHeap = false;
            other.m_dynamicType = nullptr;
            other.m_borrowAddr = nullptr;
            other.m_borrowGeneration = 0;
            other.m_resolver = nullptr;
        }

        Variant& operator=(const Variant& other)
        {
            if (this != &other)
            {
                Reset();
                m_dynamicType = other.m_dynamicType;
                m_vtable = other.m_vtable;
                m_borrowAddr = other.m_borrowAddr;
                m_borrowGeneration = other.m_borrowGeneration;
                m_resolver = other.m_resolver;
                if (m_vtable != nullptr)
                {
                    void* dst = AllocateStorage(m_vtable->size, m_vtable->align);
                    m_vtable->copy(dst, other.Data());
                }
                else if (m_resolver != nullptr)
                {
                    m_storage = other.m_storage;
                }
            }
            return *this;
        }

        Variant& operator=(Variant&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                m_dynamicType = other.m_dynamicType;
                m_vtable = other.m_vtable;
                m_borrowAddr = other.m_borrowAddr;
                m_borrowGeneration = other.m_borrowGeneration;
                m_resolver = other.m_resolver;
                if (m_vtable != nullptr)
                {
                    if (other.m_isHeap)
                    {
                        m_isHeap = true;
                        m_storage.heap = other.m_storage.heap;
                    }
                    else
                    {
                        void* dst = AllocateStorage(m_vtable->size, m_vtable->align);
                        m_vtable->move(dst, other.Data());
                        m_vtable->destroy(other.Data());
                    }
                }
                else if (m_resolver != nullptr)
                {
                    m_storage = other.m_storage;
                }
                other.m_vtable = nullptr;
                other.m_isHeap = false;
                other.m_dynamicType = nullptr;
                other.m_borrowAddr = nullptr;
                other.m_borrowGeneration = 0;
                other.m_resolver = nullptr;
            }
            return *this;
        }

        ~Variant() { Reset(); }

        void Reset() noexcept
        {
            if (m_vtable != nullptr)
            {
                m_vtable->destroy(Data());
                if (m_isHeap)
                {
                    DefaultAllocator().Free(m_storage.heap);
                }
            }
            m_vtable = nullptr;
            m_isHeap = false;
            m_dynamicType = nullptr;
            m_borrowAddr = nullptr;
            m_borrowGeneration = 0;
            m_resolver = nullptr; // resolve mode owns no heap/value; just drop the handle
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_vtable == nullptr && m_resolver == nullptr;
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_vtable != nullptr || m_resolver != nullptr;
        }

        // True if this holds an OWNED object (RefPtr<Object>), not a plain value and NOT a borrow.
        // A borrow's SBO also holds a RefPtr (its keep-alive root), but that root is NOT the value the
        // borrow represents - so a borrow is excluded here, or AsObject()-style extraction would hand
        // back the wrong object (the root) and marshal it as the borrowed type (Fable ruling).
        [[nodiscard]] bool IsObject() const noexcept
        {
            return m_dynamicType != nullptr && m_borrowAddr == nullptr && m_resolver == nullptr;
        }

        // True if this is a borrow (a handle over a member/element address; see Borrow()).
        [[nodiscard]] bool IsBorrow() const noexcept { return m_borrowAddr != nullptr; }

        // The borrowed address (borrows only; null otherwise).
        [[nodiscard]] void* BorrowAddress() const noexcept { return m_borrowAddr; }

        // Whether a borrow is still live: no structural reflection mutation has happened since capture.
        // A stale borrow (owner element removed / array reallocated) fails this - callers then treat it
        // as empty rather than dereferencing a dangling address.
        [[nodiscard]] bool BorrowValid() const noexcept
        {
            return m_borrowAddr != nullptr &&
                   m_borrowGeneration == GlobalReflectionMutationGeneration();
        }

        // The keep-alive root object a borrow pins (borrows only; null otherwise). Explicitly named so
        // nothing mistakes it for the borrowed value (which is a subobject at BorrowAddress()).
        [[nodiscard]] Object* BorrowRoot() const noexcept
        {
            return (m_borrowAddr != nullptr && m_dynamicType != nullptr)
                       ? static_cast<const RefPtr<Object>*>(Data())->Get()
                       : nullptr;
        }

        [[nodiscard]] const TypeInfo* Type() const noexcept
        {
            if (m_dynamicType != nullptr)
            {
                return m_dynamicType;
            } // object / borrow: the (dynamic / borrowed) type
            return m_vtable != nullptr ? m_vtable->typeInfo() : nullptr;
        }

        // Borrowed view of the held OWNED object, or null if empty / not an owned object. A borrow
        // returns null here (its stored RefPtr is the keep-alive root, not the borrowed value).
        [[nodiscard]] Object* AsObject() const noexcept
        {
            if (m_dynamicType == nullptr || m_borrowAddr != nullptr || m_resolver != nullptr)
            {
                return nullptr;
            }
            return static_cast<const RefPtr<Object>*>(Data())->Get();
        }

        // Borrowed, down-cast view; null if not an object or not a T.
        template <typename T>
        [[nodiscard]] T* AsObject() const noexcept
        {
            return Cast<T>(AsObject());
        }

        // Address of the stored value (value mode). For the reflection/binding
        // layer to build an Instance over a value a Variant owns. For objects use
        // AsObject() instead (this returns the RefPtr storage, not the object).
        [[nodiscard]] void* ValuePointer() noexcept { return Data(); }

        template <typename T>
        [[nodiscard]] bool Is() const noexcept
        {
            return m_vtable == &detail::kVariantVTable<T>;
        }

        template <typename T>
        [[nodiscard]] T* TryGet() noexcept
        {
            return Is<T>() ? static_cast<T*>(Data()) : nullptr;
        }

        template <typename T>
        [[nodiscard]] const T* TryGet() const noexcept
        {
            return Is<T>() ? static_cast<const T*>(Data()) : nullptr;
        }

        template <typename T>
        [[nodiscard]] T& Get() noexcept
        {
            DRACONIC_ASSERT_MSG(Is<T>(), "Variant::Get<T>() type mismatch");
            return *static_cast<T*>(Data());
        }

        template <typename T>
        [[nodiscard]] const T& Get() const noexcept
        {
            DRACONIC_ASSERT_MSG(Is<T>(), "Variant::Get<T>() type mismatch");
            return *static_cast<const T*>(Data());
        }

        // The underlying integer of an ENUM-typed VALUE Variant (Type() reports an enum). Enums cross
        // to script as their underlying int - Wren has no enum type, and an AngelScript enum is
        // int-backed - so the backends read the value with this instead of a typed TryGet<E>() (which
        // they cannot spell without the C++ enum type). Reads the stored value by its byte width.
        [[nodiscard]] i64 AsEnumInt() const noexcept
        {
            const void* p = Data();
            const TypeInfo* t = Type();
            switch (t != nullptr ? t->size : 0)
            {
            case 1:
                return *static_cast<const i8*>(p);
            case 2:
                return *static_cast<const i16*>(p);
            case 4:
                return *static_cast<const i32*>(p);
            case 8:
                return *static_cast<const i64*>(p);
            default:
                return 0;
            }
        }

    private:
        static constexpr usize kInlineSize = 3 * sizeof(void*);
        static constexpr usize kInlineAlign = alignof(std::max_align_t);

        void* AllocateStorage(usize size, usize align)
        {
            if (size <= kInlineSize && align <= kInlineAlign)
            {
                m_isHeap = false;
                return &m_storage.inlineBytes;
            }
            m_isHeap = true;
            m_storage.heap = DefaultAllocator().Allocate(size, align);
            return m_storage.heap;
        }

        [[nodiscard]] void* Data() noexcept
        {
            return m_isHeap ? m_storage.heap : static_cast<void*>(&m_storage.inlineBytes);
        }
        [[nodiscard]] const void* Data() const noexcept
        {
            return m_isHeap ? m_storage.heap : static_cast<const void*>(&m_storage.inlineBytes);
        }

        union Storage
        {
            alignas(kInlineAlign) unsigned char inlineBytes[kInlineSize];
            void* heap;
        };

        Storage m_storage{};
        bool m_isHeap = false;
        const TypeInfo* m_dynamicType = nullptr; // non-null => object OR borrow (dynamic/borrowed type)
        const detail::VariantVTable* m_vtable = nullptr;
        void* m_borrowAddr = nullptr; // non-null => BORROW mode; overrides ToInstance to this address
        u64 m_borrowGeneration = 0;   // mutation generation captured at Borrow(); revalidated on deref
        // non-null => RESOLVE mode: recompute the value's CURRENT address on every deref (live scene
        // state a borrow cannot safely hold). Context lives in m_storage's inline bytes; no owned
        // value (m_vtable == nullptr), no heap, weak (no keep-alive). See Resolving() / ToInstance.
        void* (*m_resolver)(const Variant&) = nullptr;
    };
}

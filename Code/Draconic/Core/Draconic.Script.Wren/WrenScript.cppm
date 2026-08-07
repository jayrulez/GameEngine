// Draconic::ScriptWren - Wren VM backend (draconic.script.wren).
//
// Implements Draconic::Script on Wren and binds reflected types into the VM:
// each registered type with a constructor becomes a Wren `foreign class` whose
// allocate/getters/setters route through reflection (Construct / GetProperty /
// SetProperty), with values held in a Variant as the foreign instance data.
//
// Wren foreign callbacks are plain C function pointers (no captures), so method
// dispatch uses a fixed pool of trampolines: each Trampoline<I> forwards to
// g_bindings[I], assigned when Wren asks us to bind a (class, signature).
//
// This slice: construction + property get/set for value types. Reflected
// methods and object/struct returns (wrapping a Variant back into a foreign
// instance) come next.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WrenInclude.h"
#include <utility>

export module draconic.script.wren;

import draconic.foundation;
import draconic.script;
import draconic.script.facades; // BehaviorFacadeNames() - the prelude's import list

namespace foundation = draconic::foundation;

namespace draconic::script::wren
{
    // The OPTIONAL Wren `Behavior` base class (scripting.md §3.3 coroutines), injected
    // into every behaviors module right after the facade prelude. A behavior opts in with
    // `class Mover is Behavior { ... }` to get startCoroutine(fn) / wait(seconds) /
    // waitUntil(fn); plain classes that do not extend it are untouched. The base owns the
    // instance's coroutine-id list and routes register/unregister to the manager's
    // host-side scheduler through two foreign methods (bound by name, below).
    inline constexpr foundation::StringView kBehaviorBaseSource =
        u8"class Behavior {\n"
        u8"  construct new(entity) {\n"
        u8"    _entity = entity\n"
        u8"    _drCoroutines = []\n"
        u8"  }\n"
        u8"  entity { _entity }\n"
        u8"  startCoroutine(fn) {\n"
        u8"    var fiber = Fiber.new(fn)\n"
        u8"    var w = fiber.call()\n"
        u8"    if (fiber.isDone) return -1\n"
        u8"    if (!(w is Num)) w = 0\n"
        u8"    var id = drRegisterCoroutine(fiber, w)\n"
        u8"    _drCoroutines.add(id)\n"
        u8"    return id\n"
        u8"  }\n"
        u8"  wait(seconds) { Fiber.yield(seconds) }\n"
        u8"  waitUntil(fn) {\n"
        u8"    while (!fn.call()) {\n"
        u8"      Fiber.yield(0)\n"
        u8"    }\n"
        u8"  }\n"
        u8"  foreign drRegisterCoroutine(fiber, w)\n"
        u8"  foreign drUnregisterCoroutine(id)\n"
        u8"  drCancelCoroutines() {\n"
        u8"    for (id in _drCoroutines) {\n"
        u8"      drUnregisterCoroutine(id)\n"
        u8"    }\n"
        u8"    _drCoroutines.clear()\n"
        u8"  }\n"
        u8"}\n";

    // Reflected classes live in the "main" module; behavior modules are separate, so the
    // module is framed with ONE prelude line importing the facade names (built from the
    // neutral BehaviorFacadeNames list) followed by the Behavior base. Appended to `out`.
    inline void AppendBehaviorPrelude(foundation::String& out)
    {
        out += u8"import \"main\" for ";
        const foundation::Span<const foundation::StringView> facades = BehaviorFacadeNames();
        const foundation::Span<const foundation::StringView> extras =
            ExtraFacadeNames(); // out-of-tree facades (e.g. Net)
        bool first = true;
        for (foundation::usize i = 0; i < facades.Size(); ++i)
        {
            if (!first)
            {
                out += u8", ";
            }
            out += facades[i];
            first = false;
        }
        for (foundation::usize i = 0; i < extras.Size(); ++i)
        {
            if (!first)
            {
                out += u8", ";
            }
            out += extras[i];
            first = false;
        }
        out += u8"\n";
        out += kBehaviorBaseSource;
    }

    inline const char* CStr(const foundation::String& s) noexcept
    {
        return reinterpret_cast<const char*>(s.CStr());
    }

    inline void AppendAscii(foundation::String& s, const char* text)
    {
        s.Append(foundation::StringView(reinterpret_cast<const foundation::utf8char*>(text)));
    }

    inline void AppendUint(foundation::String& s, foundation::u32 n)
    {
        char buf[16];
        int i = 0;
        if (n == 0)
        {
            buf[i++] = '0';
        }
        else
        {
            char tmp[16];
            int j = 0;
            while (n != 0)
            {
                tmp[j++] = static_cast<char>('0' + (n % 10));
                n /= 10;
            }
            while (j != 0)
            {
                buf[i++] = tmp[--j];
            }
        }
        buf[i] = '\0';
        AppendAscii(s, buf);
    }

    inline bool NameEq(const char* a, const char* b) noexcept
    {
        foundation::usize i = 0;
        while (a[i] != '\0' && a[i] == b[i])
        {
            ++i;
        }
        return a[i] == b[i];
    }

    inline void WriteUtf8(void (*sink)(foundation::StringView) noexcept, const char* text)
    {
        if (text != nullptr)
        {
            sink(foundation::StringView(reinterpret_cast<const foundation::utf8char*>(text)));
        }
    }

    // --- marshalling -------------------------------------------------------
    inline constexpr const char* kModule = "main"; // module the foreign classes live in

    inline foundation::StringView AsciiView(const char* text) noexcept
    {
        return (text != nullptr) ? foundation::StringView(reinterpret_cast<const foundation::utf8char*>(text))
                                 : foundation::StringView{};
    }

    // The Wren call spelling of a method: `name(_,_,...)` with one `_` per parameter.
    inline foundation::String WrenCallSignatureOf(const char* name, foundation::u32 paramCount)
    {
        foundation::String sig(AsciiView(name));
        sig += u8"(";
        for (foundation::u32 i = 0; i < paramCount; ++i)
        {
            if (i != 0)
            {
                sig += u8",";
            }
            sig += u8"_";
        }
        sig += u8")";
        return sig;
    }

    // A script function argument (a Wren Fn/closure) -> a WrenScriptDelegate wrapping it,
    // as an object-mode Variant. Defined after WrenContext (it registers with the owning
    // context for lifetime); forward-declared so MarshalIn can wrap a delegate parameter.
    foundation::Variant WrapWrenDelegateArg(WrenVM* vm, int slot);

    // Engine value -> Wren slot for primitives; true if handled (slot untouched
    // and false otherwise, so the caller can try a foreign wrap).
    inline bool TryPrimitiveOut(WrenVM* vm, int slot, const foundation::Variant& value)
    {
        if (const bool* b = value.TryGet<bool>())
        {
            wrenSetSlotBool(vm, slot, *b);
            return true;
        }
        if (const foundation::f64* d = value.TryGet<foundation::f64>())
        {
            wrenSetSlotDouble(vm, slot, *d);
            return true;
        }
        if (const foundation::f32* f = value.TryGet<foundation::f32>())
        {
            wrenSetSlotDouble(vm, slot, static_cast<double>(*f));
            return true;
        }
        if (const foundation::i32* i = value.TryGet<foundation::i32>())
        {
            wrenSetSlotDouble(vm, slot, static_cast<double>(*i));
            return true;
        }
        if (const foundation::i64* i = value.TryGet<foundation::i64>())
        {
            wrenSetSlotDouble(vm, slot, static_cast<double>(*i));
            return true;
        }
        if (const foundation::u32* u = value.TryGet<foundation::u32>())
        {
            wrenSetSlotDouble(vm, slot, static_cast<double>(*u));
            return true;
        }
        if (const foundation::u64* u = value.TryGet<foundation::u64>())
        {
            wrenSetSlotDouble(vm, slot, static_cast<double>(*u));
            return true;
        }
        if (const foundation::String* s = value.TryGet<foundation::String>())
        {
            // Engine String is UTF-8, as are Wren strings - pass bytes directly.
            wrenSetSlotBytes(vm, slot, CStr(*s), s->Size());
            return true;
        }
        return false;
    }

    // Engine value -> Wren slot. Primitives go directly; a reflected value/object
    // is wrapped into a new Wren foreign instance of its class (if one is bound),
    // sharing/copying the Variant; otherwise null.
    inline void MarshalOut(WrenVM* vm, int slot, const foundation::Variant& value)
    {
        if (TryPrimitiveOut(vm, slot, value))
        {
            return;
        }

        const foundation::TypeInfo* type = value.Type();
        // An enum crosses to Wren as its underlying number (Wren has no enum type; enums are excluded
        // from foreign-class emission). The author compares/assigns integers (0, 1, ...).
        if (type != nullptr && type->enumeratorCount > 0)
        {
            wrenSetSlotDouble(vm, slot, static_cast<double>(value.AsEnumInt()));
            return;
        }
        if (type != nullptr && type->name != nullptr && wrenHasModule(vm, kModule) &&
            wrenHasVariable(vm, kModule, type->name))
        {
            const int classSlot = wrenGetSlotCount(vm);
            wrenEnsureSlots(vm, classSlot + 1);
            wrenGetVariable(vm, kModule, type->name, classSlot);
            void* data = wrenSetSlotNewForeign(vm, slot, classSlot, sizeof(foundation::Variant*));
            *static_cast<foundation::Variant**>(data) =
                foundation::DefaultAllocator().New<foundation::Variant>(value);
            return;
        }
        wrenSetSlotNull(vm, slot);
    }

    inline foundation::Variant SlotToVariant(WrenVM* vm, int slot)
    {
        switch (wrenGetSlotType(vm, slot))
        {
        case WREN_TYPE_BOOL:
            return foundation::Variant::From<bool>(wrenGetSlotBool(vm, slot));
        case WREN_TYPE_NUM:
            return foundation::Variant::From<foundation::f64>(wrenGetSlotDouble(vm, slot));
        case WREN_TYPE_FOREIGN:
            return **static_cast<foundation::Variant**>(wrenGetSlotForeign(vm, slot));
        case WREN_TYPE_STRING:
        {
            int length = 0;
            const char* bytes = wrenGetSlotBytes(vm, slot, &length);
            return foundation::Variant::From<foundation::String>(foundation::String(foundation::StringView(
                reinterpret_cast<const foundation::utf8char*>(bytes), static_cast<foundation::usize>(length))));
        }
        default:
            return foundation::Variant{};
        }
    }

    // Wren slot -> engine Variant of the expected reflected type (coerces Wren
    // numbers to the target scalar; foreign slots carry a Variant already).
    inline foundation::Variant MarshalIn(WrenVM* vm, int slot, const foundation::TypeInfo* expected)
    {
        // A delegate parameter (RefPtr<IScriptDelegate>): the argument is a Wren fn/closure,
        // not a reflected foreign value - wrap it into a WrenScriptDelegate holding a handle
        // to the fn (grabbed regardless of slot type, the coroutine-registration recipe).
        if (expected != nullptr && foundation::IsDerivedFrom(expected, &IScriptDelegate::StaticType()))
        {
            return WrapWrenDelegateArg(vm, slot);
        }
        // An enum parameter/setter takes a Wren number; carry it as an i64 (the property setter /
        // enum-arg path casts it to the enum). Enums have no Wren foreign class to marshal through.
        if (expected != nullptr && expected->enumeratorCount > 0 &&
            wrenGetSlotType(vm, slot) == WREN_TYPE_NUM)
        {
            return foundation::Variant::From<foundation::i64>(static_cast<foundation::i64>(wrenGetSlotDouble(vm, slot)));
        }
        switch (wrenGetSlotType(vm, slot))
        {
        case WREN_TYPE_FOREIGN:
            return **static_cast<foundation::Variant**>(wrenGetSlotForeign(vm, slot)); // boxed Variant*
        case WREN_TYPE_BOOL:
            return foundation::Variant::From<bool>(wrenGetSlotBool(vm, slot));
        case WREN_TYPE_NUM:
        {
            const double d = wrenGetSlotDouble(vm, slot);
            if (expected == &foundation::TypeOf<foundation::f32>())
            {
                return foundation::Variant::From<foundation::f32>(static_cast<foundation::f32>(d));
            }
            if (expected == &foundation::TypeOf<foundation::i32>())
            {
                return foundation::Variant::From<foundation::i32>(static_cast<foundation::i32>(d));
            }
            if (expected == &foundation::TypeOf<foundation::i64>())
            {
                return foundation::Variant::From<foundation::i64>(static_cast<foundation::i64>(d));
            }
            if (expected == &foundation::TypeOf<foundation::u32>())
            {
                return foundation::Variant::From<foundation::u32>(static_cast<foundation::u32>(d));
            }
            if (expected == &foundation::TypeOf<foundation::u64>())
            {
                return foundation::Variant::From<foundation::u64>(static_cast<foundation::u64>(d));
            }
            return foundation::Variant::From<foundation::f64>(d);
        }
        case WREN_TYPE_STRING:
        {
            int length = 0;
            const char* bytes = wrenGetSlotBytes(vm, slot, &length);
            (void)expected; // engine String is UTF-8, like Wren strings
            return foundation::Variant::From<foundation::String>(foundation::String(foundation::StringView(
                reinterpret_cast<const foundation::utf8char*>(bytes), static_cast<foundation::usize>(length))));
        }
        default:
            return foundation::Variant{};
        }
    }

    // Builds a Wren call signature "name(_,_,...)" with `argc` parameter slots.
    inline void BuildSignature(char* out, foundation::usize capacity, const char* name, foundation::usize argc)
    {
        foundation::usize pos = 0;
        for (const char* p = name; *p != '\0' && pos + 1 < capacity; ++p)
        {
            out[pos++] = *p;
        }
        if (pos + 1 < capacity)
        {
            out[pos++] = '(';
        }
        for (foundation::usize i = 0; i < argc && pos + 2 < capacity; ++i)
        {
            out[pos++] = '_';
            if (i + 1 < argc)
            {
                out[pos++] = ',';
            }
        }
        if (pos + 1 < capacity)
        {
            out[pos++] = ')';
        }
        out[pos] = '\0';
    }

    // --- overload resolution (by argument type) ----------------------------
    // Is the value in arg slot `p+1` acceptable for parameter type `pt`?
    inline bool SlotMatchesParam(WrenVM* vm, int slot, const foundation::TypeInfo* pt)
    {
        // A Variant parameter is the generic payload sink: it accepts ANY script value (number,
        // string, bool, or a boxed reflected value), which MarshalIn boxes straight into a Variant.
        // Declared last among overloads, so a specific typed overload still wins when one matches.
        if (pt == &foundation::TypeOf<foundation::Variant>())
        {
            return true;
        }
        // An enum parameter takes a Wren number (enums cross as their underlying int).
        if (pt != nullptr && pt->enumeratorCount > 0)
        {
            return wrenGetSlotType(vm, slot) == WREN_TYPE_NUM;
        }
        switch (wrenGetSlotType(vm, slot))
        {
        case WREN_TYPE_FOREIGN:
        {
            const foundation::Variant* v = *static_cast<foundation::Variant**>(wrenGetSlotForeign(vm, slot));
            return foundation::IsDerivedFrom(v->Type(), pt); // exact, or object covariance
        }
        case WREN_TYPE_NUM:
            return pt == &foundation::TypeOf<foundation::f32>() || pt == &foundation::TypeOf<foundation::f64>() ||
                   pt == &foundation::TypeOf<foundation::i32>() || pt == &foundation::TypeOf<foundation::i64>() ||
                   pt == &foundation::TypeOf<foundation::u32>() || pt == &foundation::TypeOf<foundation::u64>();
        case WREN_TYPE_BOOL:
            return pt == &foundation::TypeOf<bool>();
        case WREN_TYPE_STRING:
            return pt == &foundation::TypeOf<foundation::String>() || pt == &foundation::TypeOf<foundation::String>();
        default:
            // A Fn/closure (WREN_TYPE_UNKNOWN) matches a delegate parameter.
            return pt != nullptr && foundation::IsDerivedFrom(pt, &IScriptDelegate::StaticType());
        }
    }

    // Among `type`'s methods sharing the bound method's name/arity/static-ness,
    // pick the first whose parameters match the actual argument slots. Falls
    // back to the bound method (e.g. when nothing matches better).
    inline const foundation::MethodInfo*
    ResolveOverload(const foundation::TypeInfo& type, const foundation::MethodInfo& bound, WrenVM* vm, int argc)
    {
        for (foundation::usize i = 0; i < foundation::MethodCount(type); ++i)
        {
            const foundation::MethodInfo& m = foundation::MethodAt(type, i);
            if (m.isStatic != bound.isStatic || m.paramCount != static_cast<foundation::u32>(argc) ||
                !NameEq(m.name, bound.name))
            {
                continue;
            }
            bool match = true;
            for (int p = 0; p < argc; ++p)
            {
                if (!SlotMatchesParam(vm, p + 1, m.params[p].type()))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return &m;
            }
        }
        return &bound;
    }

    // --- foreign-binding dispatch pool -------------------------------------
    // Sized for the reachable reflected surface (every distinct get/set/method across all emitted
    // foreign classes), not the number of contexts - Reserve() dedups VM-independent bindings. The
    // collections lift emits constructor-less handle types (e.g. every particle module reachable from
    // a facade), so the pool is larger than the facade-only era needed.
    inline constexpr int kMaxBindings = 1024;
    inline constexpr int kMaxArgs = 8;

    enum class BindKind
    {
        Constructor,
        PropertyGet,
        PropertySet,
        Method,
        // Container-member ops, bound as synthesized methods on the OWNER (e.g. `behaviors_at(i)`):
        // computed transiently on `self` each call, so element handles come back OWNED (an object
        // element addrefs; a value element copies) - no borrowed-handle lifetime machinery. `property`
        // is the container member. Non-Object value elements (reached only by address) are not
        // returned here - that is the borrow follow-up.
        ContainerCount,
        ContainerAt,
        ContainerAdd,
        ContainerRemoveAt,
        ContainerMove,
        // A plain nested-VALUE member (`emitter`, a curve): getter returns a BORROW handle over the
        // member address (edited in place; owner pinned + generation-guarded). `property` is the member.
        NestedGet
    };

    struct Binding
    {
        BindKind kind;
        const foundation::TypeInfo* type;
        const foundation::PropertyInfo* property;
        const foundation::MethodInfo* method;
    };

    Binding g_bindings[kMaxBindings];
    int g_bindingCount = 0;

    void Dispatch(WrenVM* vm, const Binding& binding); // defined below

    template <int I>
    void Trampoline(WrenVM* vm)
    {
        Dispatch(vm, g_bindings[I]);
    }

    WrenForeignMethodFn g_table[kMaxBindings];

    template <int... Is>
    void FillTable(std::integer_sequence<int, Is...>)
    {
        ((g_table[Is] = &Trampoline<Is>), ...);
    }

    // Fill the trampoline table once, on first use (a function-local static is
    // reliably initialized; a namespace-scope static initializer is not, under
    // GCC's module semantics).
    inline void EnsureTable()
    {
        static const bool ready =
            (FillTable(std::make_integer_sequence<int, kMaxBindings>{}), true);
        (void)ready;
    }

    // A foreign instance stores a POINTER to a heap-allocated Variant: Wren's
    // foreign storage isn't aligned to alignof(Variant) (max_align_t), so the
    // Variant itself can't live there. The pointer is properly aligned.
    inline foundation::Variant* SelfOf(WrenVM* vm)
    {
        return *static_cast<foundation::Variant**>(wrenGetSlotForeign(vm, 0));
    }

    inline void FinalizeVariant(void* data)
    {
        foundation::DefaultAllocator().Delete(*static_cast<foundation::Variant**>(data));
    }

    // Reserve a trampoline for a binding. Identical bindings (same kind/type/
    // member) dispatch identically and are VM-independent, so they're deduped and
    // share a slot - keeping the pool bounded by the distinct reflected surface
    // rather than the number of contexts created. Returns null if full.
    inline WrenForeignMethodFn Reserve(const Binding& binding)
    {
        EnsureTable();
        for (int i = 0; i < g_bindingCount; ++i)
        {
            const Binding& e = g_bindings[i];
            if (e.kind == binding.kind && e.type == binding.type &&
                e.property == binding.property && e.method == binding.method)
            {
                return g_table[i];
            }
        }
        if (g_bindingCount >= kMaxBindings)
        {
            return nullptr;
        }
        const int slot = g_bindingCount++;
        g_bindings[slot] = binding;
        return g_table[slot];
    }

    // A reflected type has a "bindable surface" (worth an emitted foreign class) if it carries any
    // constructor, property, or method. Scalars/strings that slipped into the registry have none.
    [[nodiscard]] inline bool HasBindableSurface(const foundation::TypeInfo& t) noexcept
    {
        return foundation::ConstructorCount(t) > 0 || foundation::PropertyCount(t) > 0 ||
               foundation::MethodCount(t) > 0;
    }

    // The object types worth emitting as Wren foreign classes: those a script can actually receive a
    // handle to. Seed with constructor-having types (script-constructable), then close over the
    // reflected object graph - a bound method's return / param types, a property's type, and a
    // container element's base type PLUS every concrete type deriving it (a polymorphic container can
    // hold any of them). This bounds the emitted set - and the binding pool - to the reachable
    // surface instead of the whole registry (enums / containers / primitives never qualify). `out`
    // is the emit order (seeds first). Used by both the source emitter and the API-describe mirror.
    inline void CollectEmittableTypes(foundation::Span<const foundation::TypeInfo* const> allTypes,
                                      foundation::Array<const foundation::TypeInfo*>& out)
    {
        auto managed = [&](const foundation::TypeInfo* t) -> bool
        {
            if (t == nullptr || t->name == nullptr || t->enumeratorCount > 0 ||
                t->container != nullptr || !HasBindableSurface(*t))
            {
                return false;
            }
            for (const foundation::TypeInfo* e : allTypes)
            {
                if (e == t)
                {
                    return true;
                }
            }
            return false;
        };
        auto has = [&](const foundation::TypeInfo* t) -> bool
        {
            for (const foundation::TypeInfo* e : out)
            {
                if (e == t)
                {
                    return true;
                }
            }
            return false;
        };
        foundation::Array<const foundation::TypeInfo*> work;
        auto push = [&](const foundation::TypeInfo* t)
        {
            if (managed(t) && !has(t))
            {
                out.PushBack(t);
                work.PushBack(t);
            }
        };
        for (const foundation::TypeInfo* t : allTypes)
        {
            if (t != nullptr && foundation::ConstructorCount(*t) > 0)
            {
                push(t);
            }
        }
        // Additional emission roots registered by other modules: types reached only via a factory
        // whose DECLARED return is that type (e.g. component types via RigidBody.of(entity)), which
        // no static signature names. Seed them exactly like constructor-seeded types.
        for (const foundation::TypeInfo* t : draconic::script::ExtraScriptRootTypes())
        {
            push(t);
        }
        auto edge = [&](const foundation::TypeInfo* u)
        {
            if (u == nullptr)
            {
                return;
            }
            if (u->container != nullptr) // a container-typed member: reach its element type(s)
            {
                const foundation::TypeInfo* el = u->container->elementType;
                push(el);
                if (el != nullptr)
                {
                    foundation::Array<const foundation::TypeInfo*> derived;
                    foundation::EnumerateDerived(*el, derived);
                    for (const foundation::TypeInfo* d : derived)
                    {
                        push(d);
                    }
                }
                return;
            }
            push(u);
        };
        while (!work.IsEmpty())
        {
            const foundation::TypeInfo* t = work[work.Size() - 1];
            work.RemoveAt(work.Size() - 1);
            for (foundation::usize i = 0; i < foundation::MethodCount(*t); ++i)
            {
                const foundation::MethodInfo& m = foundation::MethodAt(*t, i);
                edge(m.returnType != nullptr ? m.returnType() : nullptr);
                for (foundation::u32 p = 0; p < m.paramCount; ++p)
                {
                    edge(m.params[p].type());
                }
            }
            for (foundation::usize i = 0; i < foundation::PropertyCount(*t); ++i)
            {
                edge(foundation::PropertyAt(*t, i).type);
            }
        }
    }

    // Defined after WrenContext (the user data holds a WrenContext*).
    [[nodiscard]] IScriptContext* OwningContext(WrenVM* vm);

    // Resolve a polymorphic container's add-by-name to a concrete derived type: match the given name
    // against each creatable derived type's `displayName` attribute, then its bare type name.
    inline const foundation::TypeInfo* ResolveElementType(const foundation::TypeInfo& base, foundation::StringView name)
    {
        foundation::Array<const foundation::TypeInfo*> derived;
        foundation::EnumerateDerived(base, derived);
        for (const foundation::TypeInfo* t : derived)
        {
            if (foundation::TypeAttrString(*t, "displayName", foundation::StringView{}) == name)
            {
                return t;
            }
        }
        for (const foundation::TypeInfo* t : derived)
        {
            if (AsciiView(t->name) == name)
            {
                return t;
            }
        }
        return nullptr;
    }

    inline foundation::usize AsciiLen(const char* s) noexcept
    {
        foundation::usize n = 0;
        while (s[n] != '\0')
        {
            ++n;
        }
        return n;
    }

    // The Variant to hand a script for container element `index`: an object / value element via getAt
    // (owned); a NON-Object value element (getAt empty) via a BORROW over its address, pinned to the
    // container owner `parent` and generation-guarded. Empty if out of range / no element.
    inline foundation::Variant ContainerElementVariant(const foundation::ContainerInfo& ci,
                                                 const foundation::Instance& container, foundation::usize index,
                                                 const foundation::Variant& parent)
    {
        foundation::Variant element = foundation::ContainerGetAt(ci, container, index);
        if (element.IsEmpty())
        {
            const foundation::Instance addr = foundation::ContainerAddressAt(ci, container, index);
            if (addr.Pointer() != nullptr)
            {
                element = foundation::Variant::Borrow(addr.Pointer(), addr.Type(), parent);
            }
        }
        return element;
    }

    // A synthesized container-op method name ("behaviors_at") decodes to {op, container property}.
    // The suffix identifies the op; the prefix must name a container property on `type` (else this is
    // an ordinary member and we fall through). "_removeAt" is tested before "_at" (distinct anchors).
    inline const foundation::PropertyInfo* MatchContainerOp(const foundation::TypeInfo& type, const char* name,
                                                      BindKind& outKind)
    {
        struct Op
        {
            const char* suffix;
            BindKind kind;
        };
        static const Op ops[] = {{"_removeAt", BindKind::ContainerRemoveAt},
                                 {"_count", BindKind::ContainerCount},
                                 {"_at", BindKind::ContainerAt},
                                 {"_add", BindKind::ContainerAdd},
                                 {"_move", BindKind::ContainerMove}};
        const foundation::usize nlen = AsciiLen(name);
        for (const Op& op : ops)
        {
            const foundation::usize slen = AsciiLen(op.suffix);
            if (nlen <= slen)
            {
                continue;
            }
            bool match = true;
            for (foundation::usize i = 0; i < slen; ++i)
            {
                if (name[nlen - slen + i] != op.suffix[i])
                {
                    match = false;
                    break;
                }
            }
            if (!match)
            {
                continue;
            }
            char base[64];
            const foundation::usize blen = nlen - slen;
            if (blen >= sizeof(base))
            {
                return nullptr;
            }
            for (foundation::usize i = 0; i < blen; ++i)
            {
                base[i] = name[i];
            }
            base[blen] = '\0';
            const foundation::PropertyInfo* prop = foundation::FindProperty(type, base);
            if (prop != nullptr && prop->type != nullptr && prop->type->container != nullptr)
            {
                outKind = op.kind;
                return prop;
            }
            return nullptr; // suffix matched but not a container property - not a container op
        }
        return nullptr;
    }

    void Dispatch(WrenVM* vm, const Binding& binding)
    {
        // Every reflected call runs under its context: native facades resolve their
        // per-context services through CurrentScriptContext().
        ScriptCallScope scope(OwningContext(vm));
        switch (binding.kind)
        {
        case BindKind::Constructor:
        {
            const int argc = wrenGetSlotCount(vm) - 1;
            const foundation::ConstructorInfo* ctor = nullptr;
            for (foundation::usize i = 0; i < foundation::ConstructorCount(*binding.type); ++i)
            {
                const foundation::ConstructorInfo& candidate = foundation::ConstructorAt(*binding.type, i);
                if (static_cast<int>(candidate.paramCount) == argc)
                {
                    ctor = &candidate;
                    break;
                }
            }
            foundation::Variant args[kMaxArgs];
            for (int i = 0; i < argc && i < kMaxArgs; ++i)
            {
                const foundation::TypeInfo* expected =
                    (ctor != nullptr) ? ctor->params[i].type() : nullptr;
                args[i] = MarshalIn(vm, i + 1, expected);
            }
            foundation::Result<foundation::Variant> created = foundation::Construct(
                *binding.type, foundation::Span<foundation::Variant>{args, static_cast<foundation::usize>(argc)});
            foundation::Variant* boxed = foundation::DefaultAllocator().New<foundation::Variant>(
                created.HasValue() ? foundation::Move(created.Value()) : foundation::Variant{});
            void* data = wrenSetSlotNewForeign(vm, 0, 0, sizeof(foundation::Variant*));
            *static_cast<foundation::Variant**>(data) = boxed;
            break;
        }
        case BindKind::PropertyGet:
        {
            foundation::Instance instance = foundation::ToInstance(*SelfOf(vm));
            const foundation::Variant result = foundation::GetProperty(*binding.property, instance);
            MarshalOut(vm, 0, result);
            break;
        }
        case BindKind::PropertySet:
        {
            foundation::Instance instance = foundation::ToInstance(*SelfOf(vm));
            const foundation::Variant value = MarshalIn(vm, 1, binding.property->type);
            (void)foundation::SetProperty(*binding.property, instance, value);
            break;
        }
        case BindKind::Method:
        {
            const int argc = wrenGetSlotCount(vm) - 1;
            const foundation::MethodInfo* method =
                ResolveOverload(*binding.type, *binding.method, vm, argc);
            foundation::Variant args[kMaxArgs];
            for (int i = 0; i < argc && i < kMaxArgs; ++i)
            {
                const foundation::TypeInfo* expected =
                    (i < static_cast<int>(method->paramCount)) ? method->params[i].type() : nullptr;
                args[i] = MarshalIn(vm, i + 1, expected);
            }
            const foundation::Span<foundation::Variant> argSpan{args, static_cast<foundation::usize>(argc)};
            if (method->isStatic)
            {
                const foundation::Result<foundation::Variant> r = foundation::InvokeStatic(*method, argSpan);
                if (r.HasValue())
                {
                    MarshalOut(vm, 0, r.Value());
                }
                else
                {
                    wrenSetSlotNull(vm, 0);
                }
            }
            else
            {
                const foundation::Result<foundation::Variant> r =
                    foundation::InvokeMethod(*method, foundation::ToInstance(*SelfOf(vm)), argSpan);
                if (r.HasValue())
                {
                    MarshalOut(vm, 0, r.Value());
                }
                else
                {
                    wrenSetSlotNull(vm, 0);
                }
            }
            break;
        }
        case BindKind::ContainerCount:
        case BindKind::ContainerAt:
        case BindKind::ContainerAdd:
        case BindKind::ContainerRemoveAt:
        case BindKind::ContainerMove:
        {
            const foundation::Instance owner = foundation::ToInstance(*SelfOf(vm));
            const foundation::Instance container(binding.property->address(owner), binding.property->type);
            const foundation::ContainerInfo& ci = *binding.property->type->container;
            const foundation::usize size = foundation::ContainerSize(ci, container);
            switch (binding.kind)
            {
            case BindKind::ContainerCount:
                wrenSetSlotDouble(vm, 0, static_cast<double>(size));
                break;
            case BindKind::ContainerAt:
            {
                const foundation::usize idx = static_cast<foundation::usize>(wrenGetSlotDouble(vm, 1));
                MarshalOut(vm, 0,
                           idx < size
                               ? ContainerElementVariant(ci, container, idx, *SelfOf(vm))
                               : foundation::Variant{});
                break;
            }
            case BindKind::ContainerAdd:
            {
                if (foundation::IsPolymorphicContainer(ci)) // add by element-type name
                {
                    const foundation::Variant nameV = MarshalIn(vm, 1, &foundation::TypeOf<foundation::String>());
                    const foundation::String* typeName = nameV.TryGet<foundation::String>();
                    const foundation::TypeInfo* elem =
                        (typeName != nullptr && ci.elementType != nullptr)
                            ? ResolveElementType(*ci.elementType, typeName->AsView())
                            : nullptr;
                    if (elem == nullptr ||
                        foundation::ContainerCreateElement(ci, container, size, *elem).Pointer() == nullptr)
                    {
                        wrenSetSlotNull(vm, 0);
                        break;
                    }
                }
                else if (foundation::ContainerEmplaceDefault(ci, container, size).Pointer() == nullptr)
                {
                    wrenSetSlotNull(vm, 0);
                    break;
                }
                MarshalOut(vm, 0, ContainerElementVariant(ci, container, size, *SelfOf(vm)));
                break;
            }
            case BindKind::ContainerRemoveAt:
                (void)foundation::ContainerRemoveAt(ci, container,
                                              static_cast<foundation::usize>(wrenGetSlotDouble(vm, 1)));
                wrenSetSlotNull(vm, 0);
                break;
            case BindKind::ContainerMove:
                (void)foundation::ContainerMoveElement(ci, container,
                                                 static_cast<foundation::usize>(wrenGetSlotDouble(vm, 1)),
                                                 static_cast<foundation::usize>(wrenGetSlotDouble(vm, 2)));
                wrenSetSlotNull(vm, 0);
                break;
            default:
                break;
            }
            break;
        }
        case BindKind::NestedGet:
        {
            foundation::Variant* self = SelfOf(vm);
            const foundation::Instance owner = foundation::ToInstance(*self);
            void* addr = (owner.Pointer() != nullptr) ? binding.property->address(owner) : nullptr;
            MarshalOut(vm, 0,
                       addr != nullptr
                           ? foundation::Variant::Borrow(addr, binding.property->type, *self)
                           : foundation::Variant{});
            break;
        }
        }
    }

    // --- signature parsing (Wren -> reflected member) ----------------------
    inline bool IsSetterSig(const char* sig)
    {
        for (const char* p = sig; p[0] != '\0'; ++p)
        {
            if (p[0] == '=' && p[1] == '(')
            {
                return true;
            }
        }
        return false;
    }
    inline bool HasParens(const char* sig)
    {
        for (const char* p = sig; *p != '\0'; ++p)
        {
            if (*p == '(')
            {
                return true;
            }
        }
        return false;
    }
    // Member name = signature up to the first '(' or '='.
    inline void MemberName(const char* sig, char* out, int capacity)
    {
        int n = 0;
        for (const char* p = sig; *p != '\0' && *p != '(' && *p != '=' && n < capacity - 1; ++p)
        {
            out[n++] = *p;
        }
        out[n] = '\0';
    }

    // Forward-declared so WrenContext's config can reference them; defined after.
    WrenForeignClassMethods BindForeignClass(WrenVM* vm, const char* module, const char* className);
    WrenForeignMethodFn BindForeignMethod(WrenVM* vm, const char* module, const char* className,
                                          bool isStatic, const char* signature);

    // The coroutine primitives the Wren `Behavior` base declares as foreign methods
    // (scripting.md §3.3). Bound by name (independent of the reflected-type pool), they
    // route the fiber/id to the manager's host-side scheduler. Defined after WrenManager.
    void CoroutineRegisterForeign(WrenVM* vm);   // drRegisterCoroutine(fiber, wait) -> id
    void CoroutineUnregisterForeign(WrenVM* vm); // drUnregisterCoroutine(id)
    inline constexpr const char* kBehaviorClassName = "Behavior";

    // A live instance of a script-defined Wren class. Holds a handle to the
    // object plus a strong reference to its owning context (keeping the VM alive),
    // and dispatches Invoke() by building the method's Wren call signature.
    class WrenScriptObject final : public ScriptObject
    {
    public:
        WrenScriptObject(foundation::RefPtr<IScriptContext> owner, WrenVM* vm,
                         WrenHandle* instance) noexcept
            : m_owner(foundation::Move(owner)), m_vm(vm), m_instance(instance)
        {
        }

        ~WrenScriptObject() override
        {
            if (m_vm != nullptr && m_instance != nullptr)
            {
                wrenReleaseHandle(m_vm, m_instance);
            }
        }

        WrenScriptObject(const WrenScriptObject&) = delete;
        WrenScriptObject& operator=(const WrenScriptObject&) = delete;

        [[nodiscard]] foundation::Result<foundation::Variant> Invoke(foundation::StringView method,
                                                         foundation::Span<foundation::Variant> args) override
        {
            const foundation::usize argc = args.Size();
            wrenEnsureSlots(m_vm, static_cast<int>(argc) + 1);
            wrenSetSlotHandle(m_vm, 0, m_instance); // receiver
            for (foundation::usize i = 0; i < argc; ++i)
            {
                MarshalOut(m_vm, static_cast<int>(i) + 1, args[i]);
            }

            const foundation::String name(method);
            char signature[96];
            BuildSignature(signature, sizeof(signature), CStr(name), argc);
            WrenHandle* call = wrenMakeCallHandle(m_vm, signature);
            const WrenInterpretResult result = wrenCall(m_vm, call);
            wrenReleaseHandle(m_vm, call);
            if (result != WREN_RESULT_SUCCESS)
            {
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            return SlotToVariant(m_vm, 0);
        }

    private:
        foundation::RefPtr<IScriptContext> m_owner;
        WrenVM* m_vm;
        WrenHandle* m_instance;
    };

    class WrenManager; // forward: WrenContext keeps its manager (the coroutine scheduler) alive
    class WrenScriptDelegate; // forward: WrenContext tracks its live delegates for teardown detach

    class WrenContext final : public IScriptContext
    {
    public:
        WrenContext(foundation::Span<const foundation::TypeInfo* const> types,
                    foundation::RefPtr<IScriptManager> manager)
            : m_manager(foundation::Move(manager))
        {
            for (const foundation::TypeInfo* t : types)
            {
                m_types.PushBack(t);
            }

            WrenConfiguration config;
            wrenInitConfiguration(&config);
            config.writeFn = &OnWrite;
            config.errorFn = &OnError;
            config.bindForeignClassFn = &BindForeignClass;
            config.bindForeignMethodFn = &BindForeignMethod;
            m_vm = wrenNewVM(&config);
            wrenSetUserData(m_vm, this); // OwningContext() maps a vm back to us

            m_module = foundation::String(reinterpret_cast<const foundation::utf8char*>("main"));
            GenerateForeignClasses();
        }

        ~WrenContext() override; // drops the VM's coroutines, then frees the VM

        WrenContext(const WrenContext&) = delete;
        WrenContext& operator=(const WrenContext&) = delete;

        // The owning manager (holds the host-side coroutine scheduler). Defined after
        // WrenManager; foreign coroutine callbacks route registration through it.
        [[nodiscard]] WrenManager& Manager() const noexcept;
        [[nodiscard]] WrenVM* Vm() const noexcept { return m_vm; }

        // A live WrenScriptDelegate tracks itself here so that, when this context's VM is
        // freed, we can detach each delegate (null its handle) BEFORE wrenFreeVM releases
        // the fn handles - releasing a handle after wrenFreeVM would be a use-after-free.
        // A delegate held only by native code is a safe no-op once detached.
        void RegisterDelegate(WrenScriptDelegate* delegate) { m_delegates.PushBack(delegate); }
        void UnregisterDelegate(WrenScriptDelegate* delegate)
        {
            for (foundation::usize i = 0; i < m_delegates.Size(); ++i)
            {
                if (m_delegates[i] == delegate)
                {
                    m_delegates.RemoveAt(i);
                    return;
                }
            }
        }

        [[nodiscard]] const foundation::TypeInfo* FindType(const char* className) const
        {
            for (const foundation::TypeInfo* t : m_types)
            {
                if (t != nullptr && t->name != nullptr && NameEq(t->name, className))
                {
                    return t;
                }
            }
            return nullptr;
        }

        foundation::Status Load(foundation::StringView source, foundation::StringView chunkName) override
        {
            const foundation::String src(source);
            const foundation::String name(chunkName);
            const WrenInterpretResult result = wrenInterpret(m_vm, CStr(name), CStr(src));
            switch (result)
            {
            case WREN_RESULT_SUCCESS:
                m_module = name;
                return foundation::Status{};
            case WREN_RESULT_COMPILE_ERROR:
                return foundation::Status{foundation::ErrorCode::InvalidArgument};
            case WREN_RESULT_RUNTIME_ERROR:
                return foundation::Status{foundation::ErrorCode::Internal};
            }
            return foundation::Status{foundation::ErrorCode::Unknown};
        }

        // Wren has no debug API (its debugger is deferred - script-debugger.md P1), so the
        // per-class `name` is unused: this stays BYTE-IDENTICAL to the old flat-assemble path -
        // the manager frames the facade prelude + coroutine base + concatenated sources, and
        // it loads as one chunk named `moduleName`, exactly as before. Defined out-of-line
        // (below WrenManager) because it dereferences the manager.
        foundation::Status LoadBehaviorModule(foundation::Span<const BehaviorModuleClass> classes,
                                        foundation::StringView moduleName) override;

        void SetErrorHandler(IScriptErrorHandler* handler) override { m_errorHandler = handler; }

        void SetGlobal(foundation::StringView, const foundation::Variant&) override {}

        [[nodiscard]] foundation::Variant GetGlobal(foundation::StringView name) override
        {
            if (!HasVariable(name))
            {
                return foundation::Variant{};
            }
            const foundation::String nm(name);
            wrenEnsureSlots(m_vm, 1);
            wrenGetVariable(m_vm, CStr(m_module), CStr(nm), 0);
            return SlotToVariant(m_vm, 0);
        }

        [[nodiscard]] bool HasFunction(foundation::StringView name) const override
        {
            return HasVariable(name);
        }

        [[nodiscard]] foundation::Result<foundation::Variant> Call(foundation::StringView function,
                                                       foundation::Span<foundation::Variant> args) override
        {
            if (!HasVariable(function))
            {
                return foundation::Err(foundation::ErrorCode::NotFound);
            }
            const foundation::usize argc = args.Size();
            wrenEnsureSlots(m_vm, static_cast<int>(argc) + 1);
            const foundation::String nm(function);
            wrenGetVariable(m_vm, CStr(m_module), CStr(nm), 0);
            for (foundation::usize i = 0; i < argc; ++i)
            {
                MarshalOut(m_vm, static_cast<int>(i) + 1, args[i]);
            }

            char signature[64];
            BuildCallSignature(signature, sizeof(signature), argc);
            WrenHandle* handle = wrenMakeCallHandle(m_vm, signature);
            const WrenInterpretResult result = wrenCall(m_vm, handle);
            wrenReleaseHandle(m_vm, handle);
            if (result != WREN_RESULT_SUCCESS)
            {
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            return SlotToVariant(m_vm, 0);
        }

        [[nodiscard]] foundation::RefPtr<ScriptObject>
        CreateInstance(foundation::StringView className, foundation::Span<foundation::Variant> args) override
        {
            if (!HasVariable(className))
            {
                return nullptr;
            }

            const foundation::usize argc = args.Size();
            wrenEnsureSlots(m_vm, static_cast<int>(argc) + 1);
            const foundation::String cls(className);
            wrenGetVariable(m_vm, CStr(m_module), CStr(cls), 0); // class object -> slot 0
            for (foundation::usize i = 0; i < argc; ++i)
            {
                MarshalOut(m_vm, static_cast<int>(i) + 1, args[i]);
            }

            char signature[64];
            BuildSignature(signature, sizeof(signature), "new", argc);
            WrenHandle* call = wrenMakeCallHandle(m_vm, signature);
            const WrenInterpretResult result = wrenCall(m_vm, call);
            wrenReleaseHandle(m_vm, call);
            if (result != WREN_RESULT_SUCCESS)
            {
                return nullptr;
            }

            WrenHandle* instance = wrenGetSlotHandle(m_vm, 0);
            return foundation::RefPtr<ScriptObject>(foundation::MakeRef<WrenScriptObject>(
                foundation::DefaultAllocator(), foundation::RefPtr<IScriptContext>(this), m_vm, instance));
        }

    private:
        static void BuildCallSignature(char* out, foundation::usize capacity, foundation::usize argc)
        {
            foundation::usize pos = 0;
            for (const char* p = "call("; *p != '\0' && pos + 1 < capacity; ++p)
            {
                out[pos++] = *p;
            }
            for (foundation::usize i = 0; i < argc && pos + 2 < capacity; ++i)
            {
                out[pos++] = '_';
                if (i + 1 < argc)
                {
                    out[pos++] = ',';
                }
            }
            if (pos + 1 < capacity)
            {
                out[pos++] = ')';
            }
            out[pos] = '\0';
        }

        // Emits a `foreign class` per registered, constructible type and runs it
        // into the "main" module so user scripts can use the reflected types.
        void GenerateForeignClasses()
        {
            foundation::String src;
            foundation::Array<const foundation::TypeInfo*> emit;
            CollectEmittableTypes(foundation::Span<const foundation::TypeInfo* const>{m_types.Data(),
                                                                         m_types.Size()},
                                  emit);
            for (const foundation::TypeInfo* t : emit)
            {
                AppendClass(src, *t);
            }
            if (!src.IsEmpty())
            {
                (void)wrenInterpret(m_vm, "main", CStr(src));
            }
        }

        // Emit a container member as ops ON THE OWNER: `<name>_count` (getter), `<name>_at(i)`,
        // `<name>_add(name)` for a polymorphic list / `<name>_add()` for a homogeneous one,
        // `<name>_removeAt(i)`, `<name>_move(from, to)`. Named methods (Wren foreign classes have no
        // subscript operator); a script wrapper can dress them up if desired.
        static void AppendContainerMethods(foundation::String& src, const foundation::PropertyInfo& prop)
        {
            const bool polymorphic = foundation::IsPolymorphicContainer(*prop.type->container);
            AppendAscii(src, "  foreign ");
            AppendAscii(src, prop.name);
            AppendAscii(src, "_count\n");
            AppendAscii(src, "  foreign ");
            AppendAscii(src, prop.name);
            AppendAscii(src, "_at(a0)\n");
            AppendAscii(src, "  foreign ");
            AppendAscii(src, prop.name);
            AppendAscii(src, polymorphic ? "_add(a0)\n" : "_add()\n");
            AppendAscii(src, "  foreign ");
            AppendAscii(src, prop.name);
            AppendAscii(src, "_removeAt(a0)\n");
            AppendAscii(src, "  foreign ");
            AppendAscii(src, prop.name);
            AppendAscii(src, "_move(a0, a1)\n");
        }

        static void AppendClass(foundation::String& src, const foundation::TypeInfo& type)
        {
            AppendAscii(src, "foreign class ");
            AppendAscii(src, type.name);
            AppendAscii(src, " {\n");
            for (foundation::usize i = 0; i < foundation::ConstructorCount(type); ++i)
            {
                const foundation::u32 arity = foundation::ConstructorAt(type, i).paramCount;
                AppendAscii(src, "  construct new(");
                for (foundation::u32 p = 0; p < arity; ++p)
                {
                    AppendAscii(src, "a");
                    AppendUint(src, p);
                    if (p + 1 < arity)
                    {
                        AppendAscii(src, ", ");
                    }
                }
                AppendAscii(src, ") {}\n");
            }
            for (foundation::usize i = 0; i < foundation::PropertyCount(type); ++i)
            {
                const foundation::PropertyInfo& prop = foundation::PropertyAt(type, i);
                if (foundation::IsNested(prop))
                {
                    // A container member is bound as synthesized ops on the owner (count / at / add /
                    // removeAt / move); a plain nested-VALUE member gets a getter that returns a borrow
                    // handle over its address (edited in place, not reassigned - getter only).
                    if (prop.type != nullptr && prop.type->container != nullptr)
                    {
                        AppendContainerMethods(src, prop);
                    }
                    else if (prop.type != nullptr)
                    {
                        AppendAscii(src, "  foreign ");
                        AppendAscii(src, prop.name);
                        AppendAscii(src, "\n");
                    }
                    continue;
                }
                AppendAscii(src, "  foreign ");
                AppendAscii(src, prop.name);
                AppendAscii(src, "\n");
                AppendAscii(src, "  foreign ");
                AppendAscii(src, prop.name);
                AppendAscii(src, "=(value)\n");
            }
            for (foundation::usize i = 0; i < foundation::MethodCount(type); ++i)
            {
                const foundation::MethodInfo& method = foundation::MethodAt(type, i);
                // Wren overloads only by name+arity, so a same-(name,arity,static)
                // overload is emitted once; the binding picks the first match.
                bool duplicate = false;
                for (foundation::usize j = 0; j < i; ++j)
                {
                    const foundation::MethodInfo& earlier = foundation::MethodAt(type, j);
                    if (earlier.paramCount == method.paramCount &&
                        earlier.isStatic == method.isStatic && NameEq(earlier.name, method.name))
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                {
                    continue;
                }

                AppendAscii(src, "  foreign ");
                if (method.isStatic)
                {
                    AppendAscii(src, "static ");
                }
                AppendAscii(src, method.name);
                AppendAscii(src, "(");
                for (foundation::u32 p = 0; p < method.paramCount; ++p)
                {
                    AppendAscii(src, "a");
                    AppendUint(src, p);
                    if (p + 1 < method.paramCount)
                    {
                        AppendAscii(src, ", ");
                    }
                }
                AppendAscii(src, ")\n");
            }
            AppendAscii(src, "}\n");
        }

        [[nodiscard]] bool HasVariable(foundation::StringView name) const
        {
            if (m_module.IsEmpty() || !wrenHasModule(m_vm, CStr(m_module)))
            {
                return false;
            }
            const foundation::String nm(name);
            return wrenHasVariable(m_vm, CStr(m_module), CStr(nm));
        }

        static void OnWrite(WrenVM*, const char* text) { WriteUtf8(&foundation::ConsoleWrite, text); }
        static void OnError(WrenVM* vm, WrenErrorType type, const char* module, int line,
                            const char* message)
        {
            WrenContext* self = static_cast<WrenContext*>(wrenGetUserData(vm));
            if (self != nullptr && self->m_errorHandler != nullptr)
            {
                // Stack-trace frames follow a runtime error; surface the message kinds.
                if (type == WREN_ERROR_COMPILE || type == WREN_ERROR_RUNTIME)
                {
                    const foundation::StringView mod =
                        (module != nullptr)
                            ? foundation::StringView(reinterpret_cast<const foundation::utf8char*>(module))
                            : foundation::StringView{};
                    const foundation::StringView msg =
                        (message != nullptr)
                            ? foundation::StringView(reinterpret_cast<const foundation::utf8char*>(message))
                            : foundation::StringView{};
                    const ScriptError error{(type == WREN_ERROR_COMPILE) ? ScriptErrorKind::Compile
                                                                         : ScriptErrorKind::Runtime,
                                            mod, static_cast<foundation::i32>(line), msg};
                    self->m_errorHandler->OnError(error);
                }
                return;
            }
            WriteUtf8(&foundation::ConsoleWriteError, message);
        }

        // Detach every live delegate (defined after WrenScriptDelegate); called from the
        // destructor before wrenFreeVM.
        void DetachAllDelegates();

        WrenVM* m_vm = nullptr;
        IScriptErrorHandler* m_errorHandler = nullptr;
        foundation::String m_module;
        foundation::Array<const foundation::TypeInfo*> m_types;
        foundation::RefPtr<IScriptManager> m_manager; // keeps the manager (scheduler) alive
        foundation::Array<WrenScriptDelegate*>
            m_delegates; // live delegates (non-owning; detached on teardown)
    };

    // --- foreign bind callbacks (defined after WrenContext) ----------------
    WrenForeignClassMethods BindForeignClass(WrenVM* vm, const char*, const char* className)
    {
        WrenForeignClassMethods methods{};
        const WrenContext* ctx = static_cast<const WrenContext*>(wrenGetUserData(vm));
        const foundation::TypeInfo* type = (ctx != nullptr) ? ctx->FindType(className) : nullptr;
        if (type != nullptr)
        {
            // Constructor-less handle types (reachable but not script-constructable) still need a
            // finalizer - their instances are created natively (a facade return wraps a heap Variant)
            // and Wren GCs them like any other. allocate is bound only when a reflected constructor
            // exists (else `Type.new(...)` correctly has no allocator and stays unconstructable).
            methods.finalize = &FinalizeVariant;
            if (foundation::ConstructorCount(*type) > 0)
            {
                methods.allocate = Reserve(Binding{BindKind::Constructor, type, nullptr, nullptr});
            }
        }
        return methods;
    }

    // First method on `type` matching name + static-ness (Wren tells us which).
    inline const foundation::MethodInfo* FindMethodMatching(const foundation::TypeInfo& type, const char* name,
                                                      bool isStatic)
    {
        for (foundation::usize i = 0; i < foundation::MethodCount(type); ++i)
        {
            const foundation::MethodInfo& m = foundation::MethodAt(type, i);
            if (m.isStatic == isStatic && NameEq(m.name, name))
            {
                return &m;
            }
        }
        return nullptr;
    }

    WrenForeignMethodFn BindForeignMethod(WrenVM* vm, const char*, const char* className,
                                          bool isStatic, const char* signature)
    {
        char name[64];
        MemberName(signature, name, sizeof(name));

        // The `Behavior` base's coroutine primitives (a plain Wren class with foreign
        // methods - not a reflected type), bound by declaring-class name + method name.
        if (NameEq(className, kBehaviorClassName))
        {
            if (NameEq(name, "drRegisterCoroutine"))
            {
                return &CoroutineRegisterForeign;
            }
            if (NameEq(name, "drUnregisterCoroutine"))
            {
                return &CoroutineUnregisterForeign;
            }
        }

        const WrenContext* ctx = static_cast<const WrenContext*>(wrenGetUserData(vm));
        const foundation::TypeInfo* type = (ctx != nullptr) ? ctx->FindType(className) : nullptr;
        if (type == nullptr)
        {
            return nullptr;
        }

        // A synthesized container op (`<container>_at`, `_count`, ...) - matched before the ordinary
        // property/method routing since `_count` is a bare-name getter that would otherwise miss.
        BindKind containerKind;
        if (const foundation::PropertyInfo* containerProp = MatchContainerOp(*type, name, containerKind))
        {
            return Reserve(Binding{containerKind, type, containerProp, nullptr});
        }

        if (IsSetterSig(signature))
        {
            const foundation::PropertyInfo* prop = foundation::FindProperty(*type, name);
            return (prop != nullptr) ? Reserve(Binding{BindKind::PropertySet, type, prop, nullptr})
                                     : nullptr;
        }
        if (!HasParens(signature))
        {
            const foundation::PropertyInfo* prop = foundation::FindProperty(*type, name);
            if (prop == nullptr)
            {
                return nullptr;
            }
            // A nested-value member's getter returns a borrow handle; a leaf property reads by value.
            const BindKind kind =
                foundation::IsNested(*prop) ? BindKind::NestedGet : BindKind::PropertyGet;
            return Reserve(Binding{kind, type, prop, nullptr});
        }
        const foundation::MethodInfo* method = FindMethodMatching(*type, name, isStatic);
        return (method != nullptr) ? Reserve(Binding{BindKind::Method, type, nullptr, method})
                                   : nullptr;
    }

    class WrenManager final : public IScriptManager
    {
    public:
        ~WrenManager() override
        {
            // Every coroutine belongs to a context's VM, and a context holds a strong
            // ref to this manager - so by the time we're destroyed all contexts (and
            // their VMs) are gone and each already called ForgetCoroutinesForVm. The
            // list is empty here; drop it without touching any freed handle.
            m_coroutines.Clear();
        }

        void RegisterType(const foundation::TypeInfo& type) override { m_types.PushBack(&type); }

        [[nodiscard]] foundation::RefPtr<IScriptContext> CreateContext() override
        {
            const foundation::Span<const foundation::TypeInfo* const> types{m_types.Data(), m_types.Size()};
            return foundation::RefPtr<IScriptContext>(foundation::MakeRef<WrenContext>(
                foundation::DefaultAllocator(), types, foundation::RefPtr<IScriptManager>(this)));
        }

        [[nodiscard]] ScriptCapabilities Capabilities() const override
        {
            // Wren fibers back the coroutine scheduler; fn handles back the delegate seam.
            return ScriptCapabilities::Coroutines | ScriptCapabilities::Delegates;
        }

        // The ACTUAL Wren-callable surface: one entry per constructible reflected type (the
        // foreign classes GenerateForeignClasses emits), with Wren-spelled member signatures.
        [[nodiscard]] foundation::Array<ScriptApiType> DescribeBoundApi() const override
        {
            foundation::Array<ScriptApiType> result;
            foundation::Array<const foundation::TypeInfo*> emit;
            CollectEmittableTypes(foundation::Span<const foundation::TypeInfo* const>{m_types.Data(),
                                                                          m_types.Size()},
                                  emit);
            for (const foundation::TypeInfo* t : emit)
            {
                ScriptApiType api;
                api.scriptName = foundation::String(AsciiView(t->name));
                api.typeId = t->id;
                api.isNamespace = false;
                for (foundation::usize i = 0; i < foundation::PropertyCount(*t); ++i)
                {
                    const foundation::PropertyInfo& p = foundation::PropertyAt(*t, i);
                    if (foundation::IsNested(p))
                    {
                        continue; // nested structures are recursed by tooling, not scriptable
                    }
                    ScriptApiMember member;
                    member.name = foundation::String(AsciiView(p.name));
                    member.signature = member.name; // Wren getter/setter share the bare name
                    member.kind = ScriptApiMemberKind::Property;
                    api.members.PushBack(foundation::Move(member));
                }
                for (foundation::usize i = 0; i < foundation::MethodCount(*t); ++i)
                {
                    const foundation::MethodInfo& m = foundation::MethodAt(*t, i);
                    ScriptApiMember member;
                    member.name = foundation::String(AsciiView(m.name));
                    member.signature = WrenCallSignatureOf(m.name, m.paramCount);
                    member.isStatic = m.isStatic;
                    member.kind = ScriptApiMemberKind::Method;
                    api.members.PushBack(foundation::Move(member));
                }
                result.PushBack(foundation::Move(api));
            }
            return result;
        }

        // The Wren behavior module: the facade `import "main" for ...` prelude + the
        // coroutine Behavior base, then the concatenated class sources. This is the ONLY
        // place the Wren behavior-module syntax lives (scripting.md §7.5).
        [[nodiscard]] foundation::String
        AssembleBehaviorModuleSource(foundation::Span<const foundation::StringView> classSources) const override
        {
            foundation::String moduleSource;
            AppendBehaviorPrelude(moduleSource);
            for (const foundation::StringView& source : classSources)
            {
                moduleSource += source;
                moduleSource += u8"\n";
            }
            return moduleSource;
        }

        // ---- the host-side coroutine scheduler (Wren fibers) ----

        /// A live coroutine: the fiber handle we own (release on drop), its VM, and the
        /// seconds still to wait before the next resume.
        struct WrenCoroutine
        {
            foundation::i32 id = 0;
            WrenVM* vm = nullptr;
            WrenHandle* fiber = nullptr;
            foundation::f64 wait = 0.0;
        };

        /// Foreign `__registerCoroutine`: takes ownership of the fiber handle, stores it
        /// with its initial wait, returns the id the Behavior base records.
        [[nodiscard]] foundation::i32 RegisterCoroutine(WrenVM* vm, WrenHandle* fiber, foundation::f64 wait)
        {
            const foundation::i32 id = ++m_nextCoroutineId;
            m_coroutines.PushBack(WrenCoroutine{id, vm, fiber, wait});
            return id;
        }

        /// Foreign `__unregisterCoroutine`: cancel by id (releases the fiber handle).
        void UnregisterCoroutine(foundation::i32 id)
        {
            for (foundation::usize i = 0; i < m_coroutines.Size(); ++i)
            {
                if (m_coroutines[i].id == id)
                {
                    if (m_coroutines[i].fiber != nullptr)
                    {
                        wrenReleaseHandle(m_coroutines[i].vm, m_coroutines[i].fiber);
                    }
                    m_coroutines.RemoveAt(i);
                    return;
                }
            }
        }

        /// A context's VM is being freed: drop its coroutines WITHOUT releasing the
        /// fiber handles (wrenFreeVM frees them - releasing here would double-free).
        void ForgetCoroutinesForVm(WrenVM* vm)
        {
            for (foundation::usize i = m_coroutines.Size(); i-- > 0;)
            {
                if (m_coroutines[i].vm == vm)
                {
                    m_coroutines.RemoveAt(i);
                }
            }
        }

        void AdvanceCoroutines(foundation::f64 deltaSeconds) override
        {
            // Snapshot the due ids first: a resumed fiber may start MORE coroutines
            // (append) - those are not advanced this frame - and by resuming through the
            // id (re-found after the call) a nested cancel can never touch a dead entry.
            for (WrenCoroutine& co : m_coroutines)
            {
                co.wait -= deltaSeconds;
            }
            m_dueScratch.Clear();
            for (const WrenCoroutine& co : m_coroutines)
            {
                if (co.wait <= kDueEpsilon)
                {
                    m_dueScratch.PushBack(co.id);
                }
            }
            for (const foundation::i32 id : m_dueScratch)
            {
                const foundation::i32 index = FindCoroutineIndex(id);
                if (index < 0)
                {
                    continue;
                }
                WrenVM* vm = m_coroutines[static_cast<foundation::usize>(index)].vm;
                WrenHandle* fiber = m_coroutines[static_cast<foundation::usize>(index)].fiber;

                // Resume: fiber.call(dt) runs it to its next Fiber.yield(seconds); the
                // yielded number lands in slot 0 as the next wait.
                wrenEnsureSlots(vm, 2);
                wrenSetSlotHandle(vm, 0, fiber);
                wrenSetSlotDouble(vm, 1, deltaSeconds);
                WrenHandle* call = wrenMakeCallHandle(vm, "call(_)");
                const WrenInterpretResult result = wrenCall(vm, call);
                wrenReleaseHandle(vm, call);

                bool drop = false;
                foundation::f64 nextWait = 0.0;
                if (result != WREN_RESULT_SUCCESS)
                {
                    drop = true; // the fiber faulted; the error already went to the sink
                }
                else
                {
                    if (wrenGetSlotType(vm, 0) == WREN_TYPE_NUM)
                    {
                        nextWait = wrenGetSlotDouble(vm, 0);
                    }
                    // isDone getter (reuses the slots we just read from).
                    wrenEnsureSlots(vm, 1);
                    wrenSetSlotHandle(vm, 0, fiber);
                    WrenHandle* done = wrenMakeCallHandle(vm, "isDone");
                    if (wrenCall(vm, done) == WREN_RESULT_SUCCESS &&
                        wrenGetSlotType(vm, 0) == WREN_TYPE_BOOL && wrenGetSlotBool(vm, 0))
                    {
                        drop = true;
                    }
                    wrenReleaseHandle(vm, done);
                }

                const foundation::i32 after = FindCoroutineIndex(id);
                if (after < 0)
                {
                    continue;
                } // a nested cancel already removed it
                if (drop)
                {
                    wrenReleaseHandle(m_coroutines[static_cast<foundation::usize>(after)].vm,
                                      m_coroutines[static_cast<foundation::usize>(after)].fiber);
                    m_coroutines.RemoveAt(static_cast<foundation::usize>(after));
                }
                else
                {
                    m_coroutines[static_cast<foundation::usize>(after)].wait = nextWait;
                }
            }
        }

        void CancelCoroutinesFor(ScriptObject& instance) override
        {
            // The Behavior base owns its own id list; asking it to cancel routes back
            // through drUnregisterCoroutine (which releases each fiber). Missing method
            // (a non-Behavior instance) just returns an error - a safe no-op.
            (void)instance.Invoke(u8"drCancelCoroutines", foundation::Span<foundation::Variant>{});
        }

    private:
        [[nodiscard]] foundation::i32 FindCoroutineIndex(foundation::i32 id) const
        {
            for (foundation::usize i = 0; i < m_coroutines.Size(); ++i)
            {
                if (m_coroutines[i].id == id)
                {
                    return static_cast<foundation::i32>(i);
                }
            }
            return -1;
        }

        static constexpr foundation::f64 kDueEpsilon = 1e-4;

        foundation::Array<const foundation::TypeInfo*> m_types;
        foundation::Array<WrenCoroutine> m_coroutines;
        foundation::Array<foundation::i32> m_dueScratch; // reused per-frame due-id snapshot
        foundation::i32 m_nextCoroutineId = 0;
    };

    // ---- script delegate (a Wren fn held as a native callback) ----

    // Wraps a Wren fn/closure handle so native code can call back into script through the
    // neutral IScriptDelegate seam. Non-owning of its context (holding it strongly would
    // cycle: VM -> foreign object -> delegate -> context -> VM). Instead it registers with
    // the context, which detaches it on VM teardown - a delegate outliving its context is a
    // safe no-op. The fn handle keeps the closure alive across Wren GC (the GC-safe promise).
    class WrenScriptDelegate final : public IScriptDelegate
    {
    public:
        WrenScriptDelegate(WrenContext* context, WrenHandle* fn) noexcept
            : m_context(context), m_fn(fn)
        {
            if (m_context != nullptr)
            {
                m_context->RegisterDelegate(this);
            }
        }

        ~WrenScriptDelegate() override
        {
            if (m_context != nullptr)
            {
                m_context->UnregisterDelegate(this);
                if (m_fn != nullptr)
                {
                    wrenReleaseHandle(m_context->Vm(), m_fn);
                }
            }
        }

        WrenScriptDelegate(const WrenScriptDelegate&) = delete;
        WrenScriptDelegate& operator=(const WrenScriptDelegate&) = delete;

        // The VM is being freed (wrenFreeVM releases the fn handle itself): drop our
        // references without touching them, so the destructor becomes a no-op.
        void Detach() noexcept
        {
            m_context = nullptr;
            m_fn = nullptr;
        }

        [[nodiscard]] foundation::Result<foundation::Variant> Invoke(foundation::Span<foundation::Variant> args) override
        {
            if (m_context == nullptr || m_fn == nullptr)
            {
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            WrenVM* vm = m_context->Vm();
            const foundation::usize argc = args.Size();
            wrenEnsureSlots(vm, static_cast<int>(argc) + 1);
            wrenSetSlotHandle(vm, 0, m_fn); // the receiver is the fn itself
            for (foundation::usize i = 0; i < argc; ++i)
            {
                MarshalOut(vm, static_cast<int>(i) + 1, args[i]);
            }

            char signature[64];
            BuildSignature(signature, sizeof(signature), "call", argc);
            WrenHandle* call = wrenMakeCallHandle(vm, signature);
            const WrenInterpretResult result = wrenCall(vm, call);
            wrenReleaseHandle(vm, call);
            if (result != WREN_RESULT_SUCCESS)
            {
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            return SlotToVariant(vm, 0);
        }

    private:
        WrenContext* m_context;
        WrenHandle* m_fn;
    };

    foundation::Variant WrapWrenDelegateArg(WrenVM* vm, int slot)
    {
        WrenContext* context = static_cast<WrenContext*>(OwningContext(vm));
        // A Fn is not a reflected foreign type; grab its handle regardless of slot type.
        WrenHandle* fn = wrenGetSlotHandle(vm, slot);
        foundation::RefPtr<IScriptDelegate> delegate(
            foundation::MakeRef<WrenScriptDelegate>(foundation::DefaultAllocator(), context, fn));
        return foundation::Variant::From(delegate);
    }

    void WrenContext::DetachAllDelegates()
    {
        for (WrenScriptDelegate* delegate : m_delegates)
        {
            delegate->Detach();
        }
        m_delegates.Clear();
    }

    // ---- coroutine foreign callbacks (defined after WrenManager) ----

    foundation::Status WrenContext::LoadBehaviorModule(foundation::Span<const BehaviorModuleClass> classes,
                                                 foundation::StringView moduleName)
    {
        foundation::Array<foundation::StringView> sources;
        sources.Reserve(classes.Size());
        for (const BehaviorModuleClass& entry : classes)
        {
            sources.PushBack(entry.source);
        }
        const foundation::String moduleSource = Manager().AssembleBehaviorModuleSource(
            foundation::Span<const foundation::StringView>{sources.Data(), sources.Size()});
        return Load(moduleSource.AsView(), moduleName);
    }

    WrenManager& WrenContext::Manager() const noexcept
    {
        return *static_cast<WrenManager*>(m_manager.Get());
    }

    WrenContext::~WrenContext()
    {
        if (m_vm != nullptr)
        {
            // Detach delegates and drop this VM's coroutines BEFORE freeing it (the manager
            // outlives us - we hold a strong ref); wrenFreeVM then frees the fn/fiber handles.
            DetachAllDelegates();
            Manager().ForgetCoroutinesForVm(m_vm);
            wrenFreeVM(m_vm);
        }
    }

    void CoroutineRegisterForeign(WrenVM* vm)
    {
        WrenContext* ctx = static_cast<WrenContext*>(wrenGetUserData(vm));
        // A Fiber is not a reflected foreign type, so grab its handle regardless of the
        // slot type (the documented recipe). Slot 2 is the initial wait (seconds).
        WrenHandle* fiber = wrenGetSlotHandle(vm, 1);
        const foundation::f64 wait =
            (wrenGetSlotType(vm, 2) == WREN_TYPE_NUM) ? wrenGetSlotDouble(vm, 2) : 0.0;
        const foundation::i32 id = ctx->Manager().RegisterCoroutine(vm, fiber, wait);
        wrenSetSlotDouble(vm, 0, static_cast<double>(id));
    }

    void CoroutineUnregisterForeign(WrenVM* vm)
    {
        WrenContext* ctx = static_cast<WrenContext*>(wrenGetUserData(vm));
        const foundation::i32 id = (wrenGetSlotType(vm, 1) == WREN_TYPE_NUM)
                                 ? static_cast<foundation::i32>(wrenGetSlotDouble(vm, 1))
                                 : -1;
        ctx->Manager().UnregisterCoroutine(id);
        wrenSetSlotNull(vm, 0);
    }

    [[nodiscard]] IScriptContext* OwningContext(WrenVM* vm)
    {
        return static_cast<WrenContext*>(wrenGetUserData(vm));
    }
}

export namespace draconic::script::wren
{
    [[nodiscard]] foundation::RefPtr<IScriptManager> CreateScriptManager()
    {
        return foundation::RefPtr<IScriptManager>(foundation::MakeRef<WrenManager>(foundation::DefaultAllocator()));
    }

    /// Registers Wren with the backend registry (scripting.md B1) - the ONE line that
    /// makes a language available; consumers resolve by extension, never by type.
    inline void RegisterWrenScriptBackend()
    {
        ScriptBackendDesc desc;
        desc.languageId = foundation::String(u8"wren");
        desc.displayName = foundation::String(u8"Wren");
        desc.fileExtensions.PushBack(foundation::String(u8"wren"));
        desc.create = []() { return CreateScriptManager(); };
        ScriptBackendRegistry::Get().Register(foundation::Move(desc));
    }
}

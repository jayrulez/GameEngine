// Draconic::ScriptAngelScript - implementation (module IMPLEMENTATION unit).
//
// ALL AngelScript SDK contact lives here: the interface unit stays engine-types-only
// (GCC module hygiene - the SDK header never sits in an interface unit's global
// fragment). See AngelScriptScript.cppm for the emission mapping.
//
// Architecture:
//  - One asIScriptEngine per AngelScriptManager. RegisterType COLLECTS TypeInfos;
//    FinalizeTypes emits in TWO PHASES: RegisterObjectType for every collected type
//    first, then behaviours/properties/methods - declaration strings may therefore
//    reference any reflected type (the reason IScriptManager::FinalizeTypes exists).
//  - Every reflected instance visible to script is a BoxedVariant: a refcounted box
//    holding the engine Variant (value or Object), registered as asOBJ_REF with
//    generic factory/addref/release behaviours.
//  - An IScriptContext is a family of asIScriptModules on the shared engine (one
//    fresh module per Load; lookups target the most recent). Execution borrows
//    asIScriptContexts from the engine's context pool (nesting-safe).
//  - ScriptCallScope brackets EVERY dispatch into script (Build's global
//    initializers, Call, Invoke, factories) so native facades can resolve their
//    per-context services through CurrentScriptContext().

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

#include <angelscript.h>
#include <scriptstdstring/scriptstdstring.h>

#include <new>
#include <string>

module draconic.script.angelscript;

import draconic.foundation;
import draconic.script;

namespace foundation = draconic::foundation;

namespace draconic::script::angelscript
{
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

    inline bool IsValidIdentifier(const char* name) noexcept
    {
        if (name == nullptr || name[0] == '\0')
        {
            return false;
        }
        const auto alpha = [](char c)
        { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
        if (!alpha(name[0]))
        {
            return false;
        }
        for (const char* p = name + 1; *p != '\0'; ++p)
        {
            if (!alpha(*p) && !(*p >= '0' && *p <= '9'))
            {
                return false;
            }
        }
        return true;
    }

    inline foundation::StringView ViewOfAscii(const char* text) noexcept
    {
        return (text != nullptr) ? foundation::StringView(reinterpret_cast<const foundation::utf8char*>(text))
                                 : foundation::StringView{};
    }

    inline foundation::String StringFromStd(const std::string& s)
    {
        return foundation::String(
            foundation::StringView(reinterpret_cast<const foundation::utf8char*>(s.c_str()), s.size()));
    }

    inline std::string StdFromVariantString(const foundation::Variant& value)
    {
        if (const foundation::String* s = value.TryGet<foundation::String>())
        {
            return std::string(reinterpret_cast<const char*>(s->CStr()), s->Size());
        }
        return std::string();
    }

    // Numeric value of a Variant as double; `ok` false when it holds no number.
    inline double NumericOf(const foundation::Variant& value, bool& ok) noexcept
    {
        ok = true;
        if (const foundation::f64* d = value.TryGet<foundation::f64>())
        {
            return *d;
        }
        if (const foundation::f32* f = value.TryGet<foundation::f32>())
        {
            return static_cast<double>(*f);
        }
        if (const foundation::i32* i = value.TryGet<foundation::i32>())
        {
            return static_cast<double>(*i);
        }
        if (const foundation::i64* i = value.TryGet<foundation::i64>())
        {
            return static_cast<double>(*i);
        }
        if (const foundation::u32* u = value.TryGet<foundation::u32>())
        {
            return static_cast<double>(*u);
        }
        if (const foundation::u64* u = value.TryGet<foundation::u64>())
        {
            return static_cast<double>(*u);
        }
        if (const foundation::i16* i = value.TryGet<foundation::i16>())
        {
            return static_cast<double>(*i);
        }
        if (const foundation::u16* u = value.TryGet<foundation::u16>())
        {
            return static_cast<double>(*u);
        }
        if (const foundation::i8* i = value.TryGet<foundation::i8>())
        {
            return static_cast<double>(*i);
        }
        if (const foundation::u8* u = value.TryGet<foundation::u8>())
        {
            return static_cast<double>(*u);
        }
        if (const bool* b = value.TryGet<bool>())
        {
            return *b ? 1.0 : 0.0;
        }
        ok = false;
        return 0.0;
    }

    // A number, coerced into a Variant of the reflected type a callee expects
    // (default f64, mirroring the Wren backend's marshalling currency).
    inline foundation::Variant CoerceNumber(double d, const foundation::TypeInfo* expected)
    {
        using namespace foundation;
        if (expected == &TypeOf<f32>())
        {
            return Variant::From<f32>(static_cast<f32>(d));
        }
        if (expected == &TypeOf<i32>())
        {
            return Variant::From<i32>(static_cast<i32>(d));
        }
        if (expected == &TypeOf<i64>())
        {
            return Variant::From<i64>(static_cast<i64>(d));
        }
        if (expected == &TypeOf<u32>())
        {
            return Variant::From<u32>(static_cast<u32>(d));
        }
        if (expected == &TypeOf<u64>())
        {
            return Variant::From<u64>(static_cast<u64>(d));
        }
        if (expected == &TypeOf<i16>())
        {
            return Variant::From<i16>(static_cast<i16>(d));
        }
        if (expected == &TypeOf<u16>())
        {
            return Variant::From<u16>(static_cast<u16>(d));
        }
        if (expected == &TypeOf<i8>())
        {
            return Variant::From<i8>(static_cast<i8>(d));
        }
        if (expected == &TypeOf<u8>())
        {
            return Variant::From<u8>(static_cast<u8>(d));
        }
        if (expected == &TypeOf<bool>())
        {
            return Variant::From<bool>(d != 0.0);
        }
        return Variant::From<f64>(d);
    }

    // An already-typed 64-bit integer Variant (i64/u64) coerced to the reflected type the callee
    // wants, WITHOUT a double stopover - so values above 2^53 round-trip exactly (CoerceNumber
    // funnels through double and would corrupt them). Integer targets cast integer->integer on the
    // two's-complement bit pattern; float targets convert honoring the source's signedness. Only
    // valid when `src` holds i64 or u64 (the caller guarantees it). expected==null keeps the source.
    inline foundation::Variant CoerceInteger(const foundation::Variant& src, const foundation::TypeInfo* expected)
    {
        using namespace foundation;
        if (expected == nullptr || src.Type() == expected)
        {
            return src;
        }
        const u64* asU = src.TryGet<u64>();
        const bool isUnsigned = (asU != nullptr);
        const u64 bits = isUnsigned ? *asU : static_cast<u64>(*src.TryGet<i64>());
        if (expected == &TypeOf<i8>())
        {
            return Variant::From<i8>(static_cast<i8>(bits));
        }
        if (expected == &TypeOf<u8>())
        {
            return Variant::From<u8>(static_cast<u8>(bits));
        }
        if (expected == &TypeOf<i16>())
        {
            return Variant::From<i16>(static_cast<i16>(bits));
        }
        if (expected == &TypeOf<u16>())
        {
            return Variant::From<u16>(static_cast<u16>(bits));
        }
        if (expected == &TypeOf<i32>())
        {
            return Variant::From<i32>(static_cast<i32>(bits));
        }
        if (expected == &TypeOf<u32>())
        {
            return Variant::From<u32>(static_cast<u32>(bits));
        }
        if (expected == &TypeOf<i64>())
        {
            return Variant::From<i64>(static_cast<i64>(bits));
        }
        if (expected == &TypeOf<u64>())
        {
            return Variant::From<u64>(bits);
        }
        if (expected == &TypeOf<bool>())
        {
            return Variant::From<bool>(bits != 0);
        }
        if (expected == &TypeOf<f32>())
        {
            return Variant::From<f32>(isUnsigned ? static_cast<f32>(bits)
                                                 : static_cast<f32>(static_cast<i64>(bits)));
        }
        if (expected == &TypeOf<f64>())
        {
            return Variant::From<f64>(isUnsigned ? static_cast<f64>(bits)
                                                 : static_cast<f64>(static_cast<i64>(bits)));
        }
        return src;
    }

    // Reflected scalar/string -> AngelScript type name (null when `type` is not a
    // primitive - i.e. it needs an object-type registration instead).
    inline const char* PrimitiveDeclName(const foundation::TypeInfo* type) noexcept
    {
        using namespace foundation;
        if (type == &TypeOf<f32>())
        {
            return "float";
        }
        if (type == &TypeOf<f64>())
        {
            return "double";
        }
        if (type == &TypeOf<bool>())
        {
            return "bool";
        }
        if (type == &TypeOf<i32>())
        {
            return "int";
        }
        if (type == &TypeOf<i64>())
        {
            return "int64";
        }
        if (type == &TypeOf<u32>())
        {
            return "uint";
        }
        if (type == &TypeOf<u64>())
        {
            return "uint64";
        }
        if (type == &TypeOf<i16>())
        {
            return "int16";
        }
        if (type == &TypeOf<u16>())
        {
            return "uint16";
        }
        if (type == &TypeOf<i8>())
        {
            return "int8";
        }
        if (type == &TypeOf<u8>())
        {
            return "uint8";
        }
        if (type == &TypeOf<String>())
        {
            return "string";
        }
        return nullptr;
    }

    // A short display string for a captured Variant (a debugger local / object member):
    // strings quoted inline, bools as true/false, any number decimal, an object/value type
    // rendered as its type name (its fields fetched lazily via CaptureObject).
    inline foundation::String DebugValueText(const foundation::Variant& value)
    {
        if (value.IsEmpty())
        {
            return foundation::String(u8"null");
        }
        if (const foundation::String* s = value.TryGet<foundation::String>())
        {
            foundation::String out(u8"\"");
            out += *s;
            out += u8"\"";
            return out;
        }
        if (const bool* b = value.TryGet<bool>())
        {
            return foundation::String(*b ? u8"true" : u8"false");
        }
        bool ok = false;
        const double number = NumericOf(value, ok);
        if (ok)
        {
            return foundation::Format(u8"{}", number);
        }
        const foundation::TypeInfo* type = value.Type();
        return foundation::String(ViewOfAscii(type != nullptr ? type->name : "object"));
    }

    inline constexpr int kMaxArgs = 8;

    // Every reflected instance held by script: a refcounted box around a Variant.
    struct BoxedVariant
    {
        foundation::Variant value;
        foundation::i32 refCount = 1;
    };

    inline BoxedVariant* NewBox(foundation::Variant value)
    {
        BoxedVariant* box = foundation::DefaultAllocator().New<BoxedVariant>();
        box->value = foundation::Move(value);
        return box;
    }

    inline void ReleaseBox(BoxedVariant* box) noexcept
    {
        if (box != nullptr && --box->refCount == 0)
        {
            foundation::DefaultAllocator().Delete(box);
        }
    }

    class AngelScriptManager;

    // Auxiliary payload for every generic registration (AngelScript hands it back
    // via asIScriptGeneric::GetAuxiliary - no trampoline pool needed).
    struct Binding
    {
        enum class Kind
        {
            Constructor,
            PropertyGet,
            PropertySet,
            Method,
            // Container-member ops, registered as methods on the OWNER (`behaviors_at(uint)` etc.):
            // computed transiently on `self` each call, so an object element handle comes back OWNED
            // (a boxed dynamic-type Variant). `property` is the container member. Non-Object value
            // elements (reached only by address) are not surfaced here - that is the borrow follow-up.
            ContainerCount,
            ContainerAt,
            ContainerAdd,
            ContainerRemoveAt,
            ContainerMove,
            // A plain nested-VALUE member (`emitter`, a curve): getter returns a BORROW handle over the
            // member address (edited in place; owner pinned + generation-guarded). `property` = member.
            NestedGet
        };
        Kind kind;
        AngelScriptManager* manager;
        const foundation::TypeInfo* type;
        const foundation::ConstructorInfo* constructor;
        const foundation::PropertyInfo* property;
        const foundation::MethodInfo* method;
    };

    void FactoryDispatch(asIScriptGeneric* gen);
    void ContainerDispatch(asIScriptGeneric* gen); // container-member ops (count/at/add/removeAt/move)
    void NestedGetDispatch(asIScriptGeneric* gen); // nested-value member getter -> borrow handle
    void AssignDispatch(asIScriptGeneric* gen); // value assignment (T& opAssign(const T&in))
    void AddRefDispatch(asIScriptGeneric* gen);
    void ReleaseDispatch(asIScriptGeneric* gen);
    void PropertyGetDispatch(asIScriptGeneric* gen);
    void PropertySetDispatch(asIScriptGeneric* gen);
    void MethodDispatch(asIScriptGeneric* gen);
    void CoroutineStartDispatch(asIScriptGeneric* gen); // startCoroutine(ScriptCoroutine@)
    void CoroutineWaitDispatch(asIScriptGeneric* gen);  // wait(float seconds)

    // The suspension-based step debugger (implements IScriptDebugger). Defined after the
    // context class; forward-declared so the manager can hold + hand out the active one, and
    // ExecuteCall can arm/adopt it. Its line callback is a plain CDECL function taking the
    // debugger back as `param` (AngelScript's SetLineCallback contract).
    class AngelScriptDebugger;
    void DebuggerLineCallback(asIScriptContext* ctx, void* param);
    // Arm a debugger's line callback on a pooled executor before Execute (owner = the
    // IScriptContext scoped for that call, re-established when the debugger resumes it).
    void ArmDebugger(AngelScriptDebugger& debugger, asIScriptContext* ctx, IScriptContext* owner);
    // After Execute returned SUSPENDED: did THIS debugger cause it (a breakpoint/step on
    // `ctx`)? If so it adopts the context (owns it until resume) and fires its listener.
    [[nodiscard]] bool AdoptDebuggerSuspension(AngelScriptDebugger& debugger,
                                               asIScriptContext* ctx);

    // A script funcdef-handle argument -> an AngelScriptDelegate wrapping it, as an
    // object-mode Variant (defined after AngelScriptDelegate). Forward-declared so
    // AngelScriptManager::ValueFromArg can wrap a delegate parameter.
    foundation::Variant MakeAngelScriptDelegateVariant(asIScriptFunction* function);

    // Reflected RefPtr<IScriptDelegate> parameters are spelled `?&in` (AppendDeclType), so a
    // script may pass a funcdef handle of ANY signature; ValueFromArg detects the funcdef handle
    // and wraps it, and Invoke marshals against the handler's ACTUAL params. `ScriptDelegate`
    // (double(double)) is the engine-provided args+return shape; `void Action()` (RegisterDelegate
    // Surface) is the common void callback. Kept as a NAMED handle for the rare non-param position.
    inline constexpr const char* kScriptDelegateFuncdef = "double ScriptDelegate(double)";
    inline constexpr const char* kScriptDelegateTypeName = "ScriptDelegate";

    // The one-line coroutine support prelude AngelScript modules get (a separate script
    // section, so it never shifts the user source's error line numbers). `wait` and the
    // funcdefs are host-registered engine-globally; only waitUntil needs a script body,
    // and it polls in-script so the host scheduler only ever deals with numeric waits.
    // It lives in a `Coroutine` NAMESPACE (authored as `Coroutine::waitUntil(...)`) so it
    // can never redefine-collide with a user's own global `waitUntil`; the global `wait`
    // and CoroutinePredicate funcdef still resolve from inside the namespace.
    inline constexpr const char* kCoroutinePreludeSection =
        "namespace Coroutine { void waitUntil(CoroutinePredicate@ pred)"
        " { while (!pred()) { wait(0.0f); } } }\n";

    class AngelScriptManager final : public IScriptManager
    {
    public:
        AngelScriptManager()
        {
            m_engine = asCreateScriptEngine();
            m_engine->SetMessageCallback(asFUNCTION(&AngelScriptManager::OnMessage), this,
                                         asCALL_CDECL);
            RegisterStdString(m_engine);
            m_stringTypeId = m_engine->GetTypeIdByDecl("string");
            RegisterCoroutineSurface();
            RegisterDelegateSurface();
        }

        ~AngelScriptManager() override
        {
            // Coroutine contexts belong to the engine - drop them (and their owner refs)
            // BEFORE the engine is torn down.
            DropAllCoroutines();
            if (m_engine != nullptr)
            {
                m_engine->ShutDownAndRelease();
            }
            for (Binding* binding : m_bindings)
            {
                foundation::DefaultAllocator().Delete(binding);
            }
        }

        AngelScriptManager(const AngelScriptManager&) = delete;
        AngelScriptManager& operator=(const AngelScriptManager&) = delete;

        // ---- IScriptManager --------------------------------------------------
        void RegisterType(const foundation::TypeInfo& type) override
        {
            for (const foundation::TypeInfo* existing : m_types)
            {
                if (existing == &type)
                {
                    return;
                }
            }
            m_types.PushBack(&type);
            if (m_finalized)
            {
                // Late registration after finalize: run both phases for this type
                // alone (everything else is already declared).
                DeclareType(type);
                DeclareReferencedEnums(type);
                BindType(type);
            }
        }

        void FinalizeTypes() override
        {
            if (m_finalized)
            {
                return;
            }
            m_finalized = true;
            // Phase 1: DECLARE every collected type - after this, any declaration
            // string may reference any reflected type.
            for (const foundation::TypeInfo* type : m_types)
            {
                DeclareType(*type);
            }
            // ...plus any enum a member references (enums ride a member's type, not the registry).
            for (const foundation::TypeInfo* type : m_types)
            {
                DeclareReferencedEnums(*type);
            }
            // Phase 2: bind members (factories, properties, methods, statics).
            for (const foundation::TypeInfo* type : m_types)
            {
                BindType(*type);
            }
        }

        [[nodiscard]] foundation::RefPtr<IScriptContext> CreateContext() override;

        void CollectGarbage() override
        {
            // AngelScript's GC is incremental; one full pass keeps the battery's
            // "callable any time" promise while staying frame-budget friendly.
            if (m_engine != nullptr)
            {
                m_engine->GarbageCollect(asGC_FULL_CYCLE);
            }
        }

        [[nodiscard]] ScriptCapabilities Capabilities() const override
        {
            // host-side asIScriptContext scheduler + funcdef-handle-backed delegate seam +
            // suspension-based step debugger (context Suspend + AS introspection).
            return ScriptCapabilities::Coroutines | ScriptCapabilities::Delegates |
                   ScriptCapabilities::Debugger;
        }

        // A step debugger over this engine's contexts (suspension breakpoints + AS
        // introspection). One at a time; it registers itself as the active debugger on
        // construction (ExecuteCall consults it) and unregisters on destruction.
        [[nodiscard]] foundation::UniquePtr<IScriptDebugger> CreateDebugger() override;

        /// The active debugger the dispatch path arms/adopts, or null (the default). The
        /// debugger sets this from its ctor/dtor - one per manager.
        void SetActiveDebugger(AngelScriptDebugger* debugger) noexcept { m_debugger = debugger; }
        [[nodiscard]] AngelScriptDebugger* ActiveDebugger() const noexcept { return m_debugger; }

        // The ACTUAL AngelScript-callable surface: one entry per declared object type, with
        // members spelled the way AngelScript presents them (statics as `Type::name(...)`,
        // properties as `get_`/`set_` accessors). A member whose declaration the backend
        // could not express (and therefore did not register) is omitted, so the surface
        // matches what BindType actually bound - which is exactly what the reflection diff
        // needs to catch a silently-unbound type.
        [[nodiscard]] foundation::Array<ScriptApiType> DescribeBoundApi() const override
        {
            foundation::Array<ScriptApiType> result;
            for (const RegisteredType& entry : m_registered)
            {
                const foundation::TypeInfo& type = *entry.type;
                ScriptApiType api;
                api.scriptName = foundation::String(ViewOfAscii(type.name));
                api.typeId = type.id;
                api.isNamespace = false;
                for (foundation::usize i = 0; i < foundation::PropertyCount(type); ++i)
                {
                    const foundation::PropertyInfo& property = foundation::PropertyAt(type, i);
                    if (!IsValidIdentifier(property.name) || foundation::IsNested(property))
                    {
                        continue; // nested structures are not scriptable leaf values
                    }
                    ScriptApiMember member;
                    member.name = foundation::String(ViewOfAscii(property.name));
                    foundation::String signature(ViewOfAscii(type.name));
                    AppendAscii(signature, ".");
                    AppendAscii(signature, property.name);
                    member.signature = foundation::Move(signature);
                    member.kind = ScriptApiMemberKind::Property;
                    api.members.PushBack(foundation::Move(member));
                }
                for (foundation::usize i = 0; i < foundation::MethodCount(type); ++i)
                {
                    const foundation::MethodInfo& method = foundation::MethodAt(type, i);
                    if (!IsValidIdentifier(method.name))
                    {
                        continue;
                    }
                    foundation::String signature;
                    if (!BuildMemberSignature(signature, type, method))
                    {
                        continue;
                    } // not bound
                    ScriptApiMember member;
                    member.name = foundation::String(ViewOfAscii(method.name));
                    member.signature = foundation::Move(signature);
                    member.isStatic = method.isStatic;
                    member.kind = ScriptApiMemberKind::Method;
                    api.members.PushBack(foundation::Move(member));
                }
                result.PushBack(foundation::Move(api));
            }
            return result;
        }

        // Invoke a script funcdef handle (an AngelScriptDelegate's stored function) from
        // native code with reflected args: runs on a pooled context, marshalling against the
        // funcdef's actual parameters, and returns its result. Handles a delegate-to-method
        // (bound object) as well as a plain function handle.
        [[nodiscard]] foundation::Result<foundation::Variant> ExecuteDelegate(asIScriptFunction* delegate,
                                                                  foundation::Span<foundation::Variant> args)
        {
            if (delegate == nullptr || m_engine == nullptr)
            {
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            asIScriptFunction* func = delegate;
            asIScriptObject* object = nullptr;
            if (delegate->GetFuncType() == asFUNC_DELEGATE)
            {
                object = static_cast<asIScriptObject*>(delegate->GetDelegateObject());
                func = delegate->GetDelegateFunction();
            }
            if (func == nullptr)
            {
                return foundation::Err(foundation::ErrorCode::Internal);
            }

            asIScriptContext* executor = m_engine->RequestContext();
            if (executor == nullptr || executor->Prepare(func) < 0)
            {
                if (executor != nullptr)
                {
                    m_engine->ReturnContext(executor);
                }
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            if (object != nullptr)
            {
                (void)executor->SetObject(object);
            }

            std::string stringTemps[kMaxArgs];
            BoxedVariant* boxTemps[kMaxArgs] = {};
            BindArgsInto(executor, func, args, stringTemps, boxTemps);
            const int result = executor->Execute();
            for (BoxedVariant* box : boxTemps)
            {
                ReleaseBox(box);
            }

            foundation::Result<foundation::Variant> outcome = foundation::Err(foundation::ErrorCode::Internal);
            if (result == asEXECUTION_FINISHED)
            {
                const int returnTypeId = func->GetReturnTypeId();
                outcome = (returnTypeId == asTYPEID_VOID)
                              ? foundation::Result<foundation::Variant>(foundation::Variant{})
                              : foundation::Result<foundation::Variant>(VariantFromTypedAddress(
                                    returnTypeId, executor->GetAddressOfReturnValue()));
            }
            m_engine->ReturnContext(executor);
            return outcome;
        }

        // The AngelScript behavior module: just the concatenated class sources. AngelScript
        // needs NO import prelude - reflected types and the coroutine surface (startCoroutine/
        // wait) are registered engine-globally, so every reflected type is already visible.
        // This keeps the language framing in the backend, off the neutral libs (§7.5).
        [[nodiscard]] foundation::String
        AssembleBehaviorModuleSource(foundation::Span<const foundation::StringView> classSources) const override
        {
            foundation::String moduleSource;
            for (const foundation::StringView& source : classSources)
            {
                moduleSource += source;
                moduleSource += u8"\n";
            }
            return moduleSource;
        }

        // ---- the from-scratch coroutine scheduler (one asIScriptContext each) ----

        /// One live coroutine: its own execution context, the seconds still to wait, and
        /// the owning behavior instance (AddRef'd) so CancelCoroutinesFor can drop by owner.
        struct Coroutine
        {
            asIScriptContext* ctx = nullptr;
            foundation::f64 wait = 0.0;
            asIScriptObject* owner = nullptr;
        };

        /// `startCoroutine(fn)`: spins up a dedicated context for the coroutine function
        /// (a delegate carries its owner), runs it to the first `wait`/suspend, and keeps
        /// it if it suspended. Called from inside a script Execute - a nested Execute on a
        /// fresh context is fine (AngelScript contexts are independent).
        void StartCoroutine(asIScriptFunction* fn)
        {
            if (fn == nullptr || m_engine == nullptr)
            {
                return;
            }
            asIScriptFunction* func = fn;
            asIScriptObject* owner = nullptr;
            if (fn->GetFuncType() == asFUNC_DELEGATE)
            {
                owner = static_cast<asIScriptObject*>(fn->GetDelegateObject());
                func = fn->GetDelegateFunction();
            }
            if (func == nullptr)
            {
                return;
            }

            asIScriptContext* co = m_engine->CreateContext();
            if (co == nullptr || co->Prepare(func) < 0)
            {
                if (co != nullptr)
                {
                    co->Release();
                }
                return;
            }
            if (owner != nullptr)
            {
                co->SetObject(owner);
                owner->AddRef();
            }

            // Record BEFORE Execute so the `wait` host call can find it (by active ctx).
            m_coroutines.PushBack(Coroutine{co, 0.0, owner});
            const int result = co->Execute();
            ResolveCoroutineExecution(co, result);
        }

        /// The `wait(seconds)` host function, running on the coroutine's own context:
        /// records the wait on it and requests a suspend (returns to AdvanceCoroutines).
        void CoroutineWaitCurrent(float seconds)
        {
            asIScriptContext* active = asGetActiveContext();
            if (active == nullptr)
            {
                return;
            }
            for (Coroutine& co : m_coroutines)
            {
                if (co.ctx == active)
                {
                    co.wait = static_cast<foundation::f64>(seconds);
                    break;
                }
            }
            (void)active->Suspend();
        }

        void AdvanceCoroutines(foundation::f64 deltaSeconds) override
        {
            for (Coroutine& co : m_coroutines)
            {
                co.wait -= deltaSeconds;
            }
            // Snapshot due contexts (stable pointers): a resumed coroutine may start more
            // (append) - not advanced this frame - and re-finding by ctx tolerates a
            // nested cancel dropping an entry mid-loop.
            m_dueScratch.Clear();
            for (const Coroutine& co : m_coroutines)
            {
                if (co.wait <= kDueEpsilon)
                {
                    m_dueScratch.PushBack(co.ctx);
                }
            }
            for (asIScriptContext* ctx : m_dueScratch)
            {
                if (FindCoroutine(ctx) < 0)
                {
                    continue;
                } // a nested cancel removed it
                const int result = ctx->Execute();
                ResolveCoroutineExecution(ctx, result);
            }
        }

        void
        CancelCoroutinesFor(ScriptObject& instance) override; // by owner; after AngelScriptObject

        // ---- shared services for contexts / dispatchers ---------------------
        [[nodiscard]] asIScriptEngine* Engine() const noexcept { return m_engine; }
        [[nodiscard]] int StringTypeId() const noexcept { return m_stringTypeId; }

        // The reflected type behind an AngelScript type id (handle flags ignored);
        // null when the id is not one of OUR registered object types.
        [[nodiscard]] const foundation::TypeInfo* TypeInfoForTypeId(int typeId) const noexcept
        {
            const int base = typeId & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
            for (const RegisteredType& entry : m_registered)
            {
                if (entry.typeId == base)
                {
                    return entry.type;
                }
            }
            return nullptr;
        }

        // Script argument -> engine Variant (generic calling convention).
        [[nodiscard]] foundation::Variant ValueFromArg(asIScriptGeneric* gen, asUINT index,
                                                 const foundation::TypeInfo* expected) const
        {
            const int typeId = gen->GetArgTypeId(index);
            switch (typeId)
            {
            case asTYPEID_BOOL:
            {
                const bool b = gen->GetArgByte(index) != 0;
                return (expected == nullptr || expected == &foundation::TypeOf<bool>())
                           ? foundation::Variant::From<bool>(b)
                           : CoerceNumber(b ? 1.0 : 0.0, expected);
            }
            case asTYPEID_INT8:
                return CoerceNumber(static_cast<foundation::i8>(gen->GetArgByte(index)), expected);
            case asTYPEID_UINT8:
                return CoerceNumber(gen->GetArgByte(index), expected);
            case asTYPEID_INT16:
                return CoerceNumber(static_cast<foundation::i16>(gen->GetArgWord(index)), expected);
            case asTYPEID_UINT16:
                return CoerceNumber(gen->GetArgWord(index), expected);
            case asTYPEID_INT32:
                return CoerceNumber(static_cast<foundation::i32>(gen->GetArgDWord(index)), expected);
            case asTYPEID_UINT32:
                return CoerceNumber(gen->GetArgDWord(index), expected);
            // 64-bit integers carry their exact type across (never via double) so values
            // above 2^53 survive - the reason CoerceInteger exists.
            case asTYPEID_INT64:
                return CoerceInteger(
                    foundation::Variant::From<foundation::i64>(static_cast<foundation::i64>(gen->GetArgQWord(index))),
                    expected);
            case asTYPEID_UINT64:
                return CoerceInteger(foundation::Variant::From<foundation::u64>(gen->GetArgQWord(index)),
                                     expected);
            case asTYPEID_FLOAT:
                return CoerceNumber(gen->GetArgFloat(index), expected);
            case asTYPEID_DOUBLE:
                return CoerceNumber(gen->GetArgDouble(index), expected);
            default:
                break;
            }
            if (typeId == m_stringTypeId)
            {
                const std::string* s = static_cast<const std::string*>(gen->GetArgAddress(index));
                return (s != nullptr) ? foundation::Variant::From<foundation::String>(StringFromStd(*s))
                                      : foundation::Variant{};
            }
            // A native enum argument: read its int32 value and carry it as an i64 (the property setter
            // / enum-arg path casts it to the enum - enums cross as their underlying int).
            if (const foundation::TypeInfo* enumType = TypeInfoForTypeId(typeId);
                enumType != nullptr && enumType->enumeratorCount > 0)
            {
                return foundation::Variant::From<foundation::i64>(
                    static_cast<foundation::i64>(gen->GetArgDWord(index)));
            }
            if ((typeId & asTYPEID_OBJHANDLE) != 0 && TypeInfoForTypeId(typeId) != nullptr)
            {
                const BoxedVariant* box =
                    static_cast<const BoxedVariant*>(gen->GetArgObject(index));
                return (box != nullptr) ? box->value : foundation::Variant{};
            }
            // A funcdef handle (a delegate parameter): wrap the function into a script delegate.
            // A generic IScriptDelegate param is spelled `?&in` (by reference), so the handle
            // arrives as a reference to the caller's variable (asIScriptFunction**); a by-value
            // funcdef handle (e.g. a coroutine's ScriptCoroutine@) arrives as the object directly.
            if ((typeId & asTYPEID_OBJHANDLE) != 0 && IsFuncdefTypeId(typeId))
            {
                asIScriptFunction* fn = nullptr;
                if (ArgIsInReference(gen, index))
                {
                    void* addr = gen->GetAddressOfArg(index);
                    fn = (addr != nullptr) ? *static_cast<asIScriptFunction**>(addr) : nullptr;
                }
                else
                {
                    fn = static_cast<asIScriptFunction*>(gen->GetArgObject(index));
                }
                return MakeAngelScriptDelegateVariant(fn);
            }
            return foundation::Variant{};
        }

        // True when parameter `index` of the executing generic function is an IN-reference
        // (`&in` / `?&in`): its value arrives as a reference to the caller's variable, not by
        // value, and the callee does NOT own a reference to it.
        [[nodiscard]] static bool ArgIsInReference(asIScriptGeneric* gen, asUINT index) noexcept
        {
            asIScriptFunction* self = (gen != nullptr) ? gen->GetFunction() : nullptr;
            if (self == nullptr)
            {
                return false;
            }
            asDWORD flags = 0;
            if (self->GetParam(index, nullptr, &flags) < 0)
            {
                return false;
            }
            return (flags & asTM_INREF) != 0;
        }

        // True when `typeId` is a handle to a funcdef (a callable delegate type).
        [[nodiscard]] bool IsFuncdefTypeId(int typeId) const noexcept
        {
            if (m_engine == nullptr)
            {
                return false;
            }
            asITypeInfo* info =
                m_engine->GetTypeInfoById(typeId & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST));
            return info != nullptr && info->GetFuncdefSignature() != nullptr;
        }

        // Engine Variant -> the generic call's declared return slot. An empty
        // Variant produces the type's zero value (null handle / empty string).
        void SetGenericReturn(asIScriptGeneric* gen, const foundation::Variant& value) const
        {
            const int typeId = gen->GetReturnTypeId();
            if (typeId == asTYPEID_VOID)
            {
                return;
            }
            // A native enum return (a getter for an enum property): AngelScript enums are int32-backed,
            // so return the enum's underlying value as a DWord under its enum typeId.
            if (const foundation::TypeInfo* enumType = TypeInfoForTypeId(typeId);
                enumType != nullptr && enumType->enumeratorCount > 0)
            {
                gen->SetReturnDWord(static_cast<asDWORD>(static_cast<foundation::i32>(value.AsEnumInt())));
                return;
            }
            bool ok = false;
            const double number = NumericOf(value, ok);
            switch (typeId)
            {
            case asTYPEID_BOOL:
                gen->SetReturnByte(number != 0.0 ? 1 : 0);
                return;
            case asTYPEID_INT8:
            case asTYPEID_UINT8:
                gen->SetReturnByte(static_cast<asBYTE>(static_cast<foundation::i64>(number)));
                return;
            case asTYPEID_INT16:
            case asTYPEID_UINT16:
                gen->SetReturnWord(static_cast<asWORD>(static_cast<foundation::i64>(number)));
                return;
            case asTYPEID_INT32:
            case asTYPEID_UINT32:
                gen->SetReturnDWord(static_cast<asDWORD>(static_cast<foundation::i64>(number)));
                return;
            case asTYPEID_INT64:
            case asTYPEID_UINT64:
            {
                // Prefer the Variant's exact 64-bit value (a facade returning i64/u64); only a
                // float source falls back through `number`, which is the correct currency there.
                if (const foundation::i64* iv = value.TryGet<foundation::i64>())
                {
                    gen->SetReturnQWord(static_cast<asQWORD>(*iv));
                    return;
                }
                if (const foundation::u64* uv = value.TryGet<foundation::u64>())
                {
                    gen->SetReturnQWord(static_cast<asQWORD>(*uv));
                    return;
                }
                gen->SetReturnQWord(static_cast<asQWORD>(static_cast<foundation::i64>(number)));
                return;
            }
            case asTYPEID_FLOAT:
                gen->SetReturnFloat(static_cast<float>(number));
                return;
            case asTYPEID_DOUBLE:
                gen->SetReturnDouble(number);
                return;
            default:
                break;
            }
            if (typeId == m_stringTypeId)
            {
                new (gen->GetAddressOfReturnLocation()) std::string(StdFromVariantString(value));
                return;
            }
            if ((typeId & asTYPEID_OBJHANDLE) != 0)
            {
                BoxedVariant* box = (!value.IsEmpty() && TypeInfoForTypeId(typeId) != nullptr)
                                        ? NewBox(value)
                                        : nullptr;
                *static_cast<void**>(gen->GetAddressOfReturnLocation()) = box;
            }
        }

        // Typed script storage (module global / finished call's return register)
        // -> engine Variant. Numbers surface uniformly as f64, strings as String,
        // handles to OUR types as a copy of the boxed Variant (Wren parity).
        [[nodiscard]] foundation::Variant VariantFromTypedAddress(int typeId, void* address) const
        {
            if (address == nullptr)
            {
                return foundation::Variant{};
            }
            switch (typeId)
            {
            case asTYPEID_BOOL:
                return foundation::Variant::From<bool>(*static_cast<bool*>(address));
            case asTYPEID_INT8:
                return foundation::Variant::From<foundation::f64>(*static_cast<foundation::i8*>(address));
            case asTYPEID_UINT8:
                return foundation::Variant::From<foundation::f64>(*static_cast<foundation::u8*>(address));
            case asTYPEID_INT16:
                return foundation::Variant::From<foundation::f64>(*static_cast<foundation::i16*>(address));
            case asTYPEID_UINT16:
                return foundation::Variant::From<foundation::f64>(*static_cast<foundation::u16*>(address));
            case asTYPEID_INT32:
                return foundation::Variant::From<foundation::f64>(*static_cast<foundation::i32*>(address));
            case asTYPEID_UINT32:
                return foundation::Variant::From<foundation::f64>(*static_cast<foundation::u32*>(address));
            case asTYPEID_INT64:
                return foundation::Variant::From<foundation::f64>(
                    static_cast<foundation::f64>(*static_cast<foundation::i64*>(address)));
            case asTYPEID_UINT64:
                return foundation::Variant::From<foundation::f64>(
                    static_cast<foundation::f64>(*static_cast<foundation::u64*>(address)));
            case asTYPEID_FLOAT:
                return foundation::Variant::From<foundation::f64>(*static_cast<float*>(address));
            case asTYPEID_DOUBLE:
                return foundation::Variant::From<foundation::f64>(*static_cast<double*>(address));
            default:
                break;
            }
            if (typeId == m_stringTypeId)
            {
                return foundation::Variant::From<foundation::String>(
                    StringFromStd(*static_cast<const std::string*>(address)));
            }
            if ((typeId & asTYPEID_OBJHANDLE) != 0 && TypeInfoForTypeId(typeId) != nullptr)
            {
                const BoxedVariant* box = *static_cast<const BoxedVariant* const*>(address);
                return (box != nullptr) ? box->value : foundation::Variant{};
            }
            return foundation::Variant{};
        }

        // Engine Variant -> typed script storage (SetGlobal). False when the
        // slot's type cannot take the value.
        bool WriteTypedAddress(int typeId, void* address, const foundation::Variant& value) const
        {
            if (address == nullptr)
            {
                return false;
            }
            bool ok = false;
            const double number = NumericOf(value, ok);
            switch (typeId)
            {
            case asTYPEID_BOOL:
                if (ok)
                {
                    *static_cast<bool*>(address) = number != 0.0;
                }
                return ok;
            case asTYPEID_INT8:
                if (ok)
                {
                    *static_cast<foundation::i8*>(address) = static_cast<foundation::i8>(number);
                }
                return ok;
            case asTYPEID_UINT8:
                if (ok)
                {
                    *static_cast<foundation::u8*>(address) = static_cast<foundation::u8>(number);
                }
                return ok;
            case asTYPEID_INT16:
                if (ok)
                {
                    *static_cast<foundation::i16*>(address) = static_cast<foundation::i16>(number);
                }
                return ok;
            case asTYPEID_UINT16:
                if (ok)
                {
                    *static_cast<foundation::u16*>(address) = static_cast<foundation::u16>(number);
                }
                return ok;
            case asTYPEID_INT32:
                if (ok)
                {
                    *static_cast<foundation::i32*>(address) = static_cast<foundation::i32>(number);
                }
                return ok;
            case asTYPEID_UINT32:
                if (ok)
                {
                    *static_cast<foundation::u32*>(address) = static_cast<foundation::u32>(number);
                }
                return ok;
            case asTYPEID_INT64:
                if (ok)
                {
                    *static_cast<foundation::i64*>(address) = static_cast<foundation::i64>(number);
                }
                return ok;
            case asTYPEID_UINT64:
                if (ok)
                {
                    *static_cast<foundation::u64*>(address) = static_cast<foundation::u64>(number);
                }
                return ok;
            case asTYPEID_FLOAT:
                if (ok)
                {
                    *static_cast<float*>(address) = static_cast<float>(number);
                }
                return ok;
            case asTYPEID_DOUBLE:
                if (ok)
                {
                    *static_cast<double*>(address) = number;
                }
                return ok;
            default:
                break;
            }
            if (typeId == m_stringTypeId)
            {
                if (value.TryGet<foundation::String>() == nullptr)
                {
                    return false;
                }
                *static_cast<std::string*>(address) = StdFromVariantString(value);
                return true;
            }
            // A reflected-type member (every reflected type is registered as asOBJ_REF - a
            // BoxedVariant box). A HANDLE member (`Type@ m`, OBJHANDLE set) stores a BoxedVariant* we
            // can set, so a resource id / reflected value applies straight into it. A plain VALUE
            // member (`Type m`, OBJHANDLE clear) is owned by AngelScript, which lazily (re)constructs
            // it - a native slot write does not survive to the first script access - so we reject it
            // and the caller guides the author to use a handle (see SetMemberField). Wren has no such
            // split because its property setter runs in-VM.
            if ((typeId & asTYPEID_OBJHANDLE) != 0 && TypeInfoForTypeId(typeId) != nullptr)
            {
                BoxedVariant** slot = static_cast<BoxedVariant**>(address);
                ReleaseBox(*slot);
                *slot = value.IsEmpty() ? nullptr : NewBox(value);
                return true;
            }
            return false;
        }

        // ---- compile-message routing ----------------------------------------
        // Build both compiles AND runs global initializers, and errors from either
        // phase arrive through the one engine message callback. Buffer during
        // Build; the caller classifies by Build's return code (compile failure vs
        // asINIT_GLOBAL_VARS_FAILED) and flushes with the right ScriptErrorKind.
        void BeginMessageCapture()
        {
            m_capturedMessages.Clear();
            m_capturing = true;
        }

        void EndMessageCapture(IScriptErrorHandler* handler, ScriptErrorKind kind)
        {
            m_capturing = false;
            for (const CapturedMessage& message : m_capturedMessages)
            {
                if (handler != nullptr)
                {
                    const ScriptError error{kind, foundation::StringView(message.section), message.row,
                                            foundation::StringView(message.text)};
                    handler->OnError(error);
                }
                else
                {
                    foundation::ConsoleWriteError(foundation::StringView(message.text));
                }
            }
            m_capturedMessages.Clear();
        }

        [[nodiscard]] Binding* MakeBinding(Binding binding)
        {
            Binding* stored = foundation::DefaultAllocator().New<Binding>(binding);
            m_bindings.PushBack(stored);
            return stored;
        }

    private:
        // Registers the coroutine surface once: the funcdefs (a coroutine body and a
        // bool predicate) plus the `startCoroutine`/`wait` host functions. `waitUntil` is
        // script-side (kCoroutinePreludeSection), added per module in Load.
        void RegisterCoroutineSurface()
        {
            if (m_engine == nullptr)
            {
                return;
            }
            (void)m_engine->RegisterFuncdef("void ScriptCoroutine()");
            (void)m_engine->RegisterFuncdef("bool CoroutinePredicate()");
            (void)m_engine->RegisterGlobalFunction("void startCoroutine(ScriptCoroutine@ fn)",
                                                   asFUNCTION(CoroutineStartDispatch),
                                                   asCALL_GENERIC, this);
            (void)m_engine->RegisterGlobalFunction("void wait(float seconds)",
                                                   asFUNCTION(CoroutineWaitDispatch),
                                                   asCALL_GENERIC, this);
        }

        // Registers the delegate funcdef surface. Reflected RefPtr<IScriptDelegate> parameters are
        // spelled `?&in` (see AppendDeclType), so a script may pass a funcdef handle of ANY shape -
        // including a user-declared funcdef. These are the engine-provided common shapes so authors
        // rarely need to declare their own: `Action` (void() - a click/notify handler) and
        // `ScriptDelegate` (double(double) - the args+return case). The backend wraps whichever
        // funcdef handle arrives; the invocation marshals against the handler's actual signature.
        void RegisterDelegateSurface()
        {
            if (m_engine == nullptr)
            {
                return;
            }
            (void)m_engine->RegisterFuncdef("void Action()");
            (void)m_engine->RegisterFuncdef(kScriptDelegateFuncdef);
        }

        // Marshals `args` into a prepared context's argument slots against `function`'s
        // declared parameters. Shared by ExecuteDelegate (and mirrors the context's own
        // BindArgs); boxTemps hold object references released by the caller after Execute.
        void BindArgsInto(asIScriptContext* executor, asIScriptFunction* function,
                          foundation::Span<foundation::Variant> args, std::string* stringTemps,
                          BoxedVariant** boxTemps)
        {
            const foundation::usize limit = function->GetParamCount();
            for (foundation::usize i = 0;
                 i < args.Size() && i < limit && i < static_cast<foundation::usize>(kMaxArgs); ++i)
            {
                const asUINT arg = static_cast<asUINT>(i);
                int typeId = 0;
                (void)function->GetParam(arg, &typeId);
                const foundation::Variant& value = args[i];
                bool ok = false;
                const double number = NumericOf(value, ok);
                switch (typeId)
                {
                case asTYPEID_BOOL:
                    (void)executor->SetArgByte(arg, number != 0.0 ? 1 : 0);
                    continue;
                case asTYPEID_INT8:
                case asTYPEID_UINT8:
                    (void)executor->SetArgByte(arg,
                                               static_cast<asBYTE>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_INT16:
                case asTYPEID_UINT16:
                    (void)executor->SetArgWord(arg,
                                               static_cast<asWORD>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_INT32:
                case asTYPEID_UINT32:
                    (void)executor->SetArgDWord(
                        arg, static_cast<asDWORD>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_INT64:
                case asTYPEID_UINT64:
                    (void)executor->SetArgQWord(
                        arg, static_cast<asQWORD>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_FLOAT:
                    (void)executor->SetArgFloat(arg, static_cast<float>(number));
                    continue;
                case asTYPEID_DOUBLE:
                    (void)executor->SetArgDouble(arg, number);
                    continue;
                default:
                    break;
                }
                if (typeId == m_stringTypeId)
                {
                    stringTemps[i] = StdFromVariantString(value);
                    (void)executor->SetArgObject(arg, &stringTemps[i]);
                    continue;
                }
                if ((typeId & asTYPEID_OBJHANDLE) != 0 && TypeInfoForTypeId(typeId) != nullptr &&
                    !value.IsEmpty())
                {
                    boxTemps[i] = NewBox(value);
                    (void)executor->SetArgObject(arg, boxTemps[i]);
                    continue;
                }
            }
        }

        // Builds the AngelScript declaration string of a method for the introspection
        // surface (return name(params), statics as Type::name). False when a return/param
        // type is not expressible - i.e. BindType never registered it either.
        [[nodiscard]] bool BuildMemberSignature(foundation::String& out, const foundation::TypeInfo& type,
                                                const foundation::MethodInfo& method) const
        {
            const foundation::TypeInfo* returnType =
                (method.returnType != nullptr) ? method.returnType() : nullptr;
            if (returnType == nullptr)
            {
                AppendAscii(out, "void");
            }
            else if (!AppendDeclType(out, returnType, /*isParam*/ false))
            {
                return false;
            }
            AppendAscii(out, " ");
            if (method.isStatic)
            {
                AppendAscii(out, type.name);
                AppendAscii(out, "::");
            }
            AppendAscii(out, method.name);
            AppendAscii(out, "(");
            if (!AppendParams(out, method.params, method.paramCount))
            {
                return false;
            }
            AppendAscii(out, ")");
            return true;
        }

        [[nodiscard]] int FindCoroutine(asIScriptContext* ctx) const
        {
            for (foundation::usize i = 0; i < m_coroutines.Size(); ++i)
            {
                if (m_coroutines[i].ctx == ctx)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        // Release a coroutine's context (unwinding a suspended call stack) and its owner ref.
        void DropCoroutineAt(foundation::usize index)
        {
            Coroutine& co = m_coroutines[index];
            if (co.ctx != nullptr)
            {
                (void)co.ctx->Abort();
                co.ctx->Release();
            }
            if (co.owner != nullptr)
            {
                co.owner->Release();
            }
            m_coroutines.RemoveAt(index);
        }

        void DropAllCoroutines()
        {
            while (m_coroutines.Size() > 0)
            {
                DropCoroutineAt(m_coroutines.Size() - 1);
            }
        }

        // After an Execute: keep it if it suspended (its next wait is already recorded);
        // drop it (finished/faulted) otherwise.
        void ResolveCoroutineExecution(asIScriptContext* ctx, int result)
        {
            if (result == asEXECUTION_SUSPENDED)
            {
                return;
            }
            const int index = FindCoroutine(ctx);
            if (index >= 0)
            {
                DropCoroutineAt(static_cast<foundation::usize>(index));
            }
        }

        static constexpr foundation::f64 kDueEpsilon = 1e-4;

        struct RegisteredType
        {
            const foundation::TypeInfo* type;
            int typeId;
        };

        struct CapturedMessage
        {
            foundation::String section;
            foundation::i32 row;
            foundation::String text;
        };

        static void OnMessage(const asSMessageInfo* message, void* param)
        {
            AngelScriptManager* self = static_cast<AngelScriptManager*>(param);
            if (message->type != asMSGTYPE_ERROR)
            {
                return;
            } // warnings/info: quiet
            if (self->m_capturing)
            {
                CapturedMessage captured;
                captured.section = foundation::String(ViewOfAscii(message->section));
                captured.row = static_cast<foundation::i32>(message->row);
                captured.text = foundation::String(ViewOfAscii(message->message));
                self->m_capturedMessages.PushBack(foundation::Move(captured));
                return;
            }
            foundation::ConsoleWriteError(ViewOfAscii(message->message));
        }

        [[nodiscard]] bool IsDeclared(const foundation::TypeInfo* type) const noexcept
        {
            for (const RegisteredType& entry : m_registered)
            {
                if (entry.type == type)
                {
                    return true;
                }
            }
            return false;
        }

        // Appends the AngelScript declaration name for a reflected type: scalars/
        // string by value (strings as `const string &in` in parameter position),
        // declared object types as handles. False = not expressible, skip member.
        bool AppendDeclType(foundation::String& out, const foundation::TypeInfo* type, bool isParam) const
        {
            if (type == nullptr)
            {
                return false;
            }
            // A delegate parameter accepts ANY callable. IScriptDelegate is a GENERIC callback
            // (Invoke marshals dynamic Variants against the handler's actual arity), so the honest
            // AngelScript type is the variable-type in-reference `?&in`: a script passes a funcdef
            // handle of any signature (a void() click handler, a double(double) value callback, a
            // user-declared funcdef) and the generic dispatch detects the funcdef handle and wraps
            // it. A single fixed funcdef would force one signature. Return position (a facade never
            // returns a delegate) falls back to a concrete handle spelling.
            if (type == &IScriptDelegate::StaticType())
            {
                if (isParam)
                {
                    AppendAscii(out, "?&in");
                }
                else
                {
                    AppendAscii(out, kScriptDelegateTypeName);
                    AppendAscii(out, "@");
                }
                return true;
            }
            // A Variant parameter is the generic payload sink (scene.events.emit(name, anyValue)):
            // spelled `?&in` so a script may pass ANY value - number, string, bool, or a boxed
            // reflected handle - and ValueFromArg boxes whatever arrives straight into a Variant.
            // Declared last among overloads, so a specific typed overload still wins on exact match.
            // Return position never carries a raw Variant (the ReturnType-override path handles that).
            if (type == &foundation::TypeOf<foundation::Variant>())
            {
                if (!isParam)
                {
                    return false;
                }
                AppendAscii(out, "?&in");
                return true;
            }
            if (const char* primitive = PrimitiveDeclName(type))
            {
                if (isParam && type == &foundation::TypeOf<foundation::String>())
                {
                    AppendAscii(out, "const string &in");
                    return true;
                }
                AppendAscii(out, primitive);
                return true;
            }
            // A native AngelScript enum: an int-backed VALUE type, spelled by name with no handle.
            if (type->enumeratorCount > 0)
            {
                AppendAscii(out, type->name);
                return true;
            }
            if (!IsDeclared(type))
            {
                return false;
            }
            AppendAscii(out, type->name);
            AppendAscii(out, "@");
            return true;
        }

        // Phase 1: declare the object type (skips scalars/string/enums/containers
        // and anything AngelScript's own registry rejects, e.g. name collisions).
        void DeclareType(const foundation::TypeInfo& type)
        {
            if (!IsValidIdentifier(type.name))
            {
                return;
            }
            if (PrimitiveDeclName(&type) != nullptr)
            {
                return;
            } // scalar/string currency
            if (type.enumeratorCount > 0)
            {
                DeclareEnum(type); // AngelScript has native enums - register it + its values
                return;
            }
            if (type.container != nullptr)
            {
                return;
            }
            if (IsDeclared(&type))
            {
                return;
            }
            const int typeId = m_engine->RegisterObjectType(type.name, 0, asOBJ_REF);
            if (typeId < 0)
            {
                DRACONIC_LOG_DEBUG(u8"Script",
                                   u8"AngelScript: could not declare reflected type '{}' ({})",
                                   ViewOfAscii(type.name), typeId);
                return;
            }
            m_registered.PushBack(RegisteredType{&type, typeId});
        }

        // AngelScript has native enums: register the enum type + each enumerator (a named int32), so
        // a script writes `NetworkAuthority::Server` and an enum-typed property/param binds. Recorded
        // in m_registered like object types, so TypeInfoForTypeId maps the enum typeId back for
        // marshalling (enum values cross as their underlying int - the property setter casts).
        void DeclareEnum(const foundation::TypeInfo& type)
        {
            if (IsDeclared(&type))
            {
                return;
            }
            const int enumTypeId = m_engine->RegisterEnum(type.name);
            if (enumTypeId < 0)
            {
                DRACONIC_LOG_DEBUG(u8"Script", u8"AngelScript: could not declare enum '{}' ({})",
                                   ViewOfAscii(type.name), enumTypeId);
                return;
            }
            for (const foundation::EnumValue& value : foundation::Enumerators(type))
            {
                m_engine->RegisterEnumValue(type.name, value.name,
                                            static_cast<int>(value.value));
            }
            // Record RegisterEnum's OWN typeId (not GetTypeIdByDecl, which may not resolve a bare
            // enum name) so TypeInfoForTypeId maps the return/arg enum typeId back at marshal time -
            // else SetGenericReturn misses the enum branch and boxes an int as an object (a crash).
            m_registered.PushBack(RegisteredType{&type, enumTypeId});
        }

        // Declare any ENUM referenced by a collected type's members (property types + constructor/
        // method params). An enum is not separately registered - it rides a member's type - so we
        // harvest them in Phase 1 so a member declaration string can name the enum (AngelScript
        // otherwise rejects a decl that references an undeclared enum, and a broken property binding
        // crashes at dispatch). Idempotent (DeclareEnum no-ops on a re-seen enum).
        void DeclareReferencedEnums(const foundation::TypeInfo& type)
        {
            auto maybe = [&](const foundation::TypeInfo* t)
            {
                if (t != nullptr && t->enumeratorCount > 0)
                {
                    DeclareEnum(*t);
                }
            };
            for (foundation::usize i = 0; i < foundation::PropertyCount(type); ++i)
            {
                maybe(foundation::PropertyAt(type, i).type);
            }
            for (foundation::usize i = 0; i < foundation::MethodCount(type); ++i)
            {
                const foundation::MethodInfo& method = foundation::MethodAt(type, i);
                for (foundation::u32 p = 0; p < method.paramCount; ++p)
                {
                    maybe(method.params[p].type());
                }
            }
            for (foundation::usize i = 0; i < foundation::ConstructorCount(type); ++i)
            {
                const foundation::ConstructorInfo& constructor = foundation::ConstructorAt(type, i);
                for (foundation::u32 p = 0; p < constructor.paramCount; ++p)
                {
                    maybe(constructor.params[p].type());
                }
            }
        }

        // Bind a container member as owner methods: `<name>_count() -> uint`, `<name>_at(uint) ->
        // Elem@`, `<name>_add(const string &in) -> Elem@` (polymorphic) / `<name>_add() -> Elem@`
        // (homogeneous), `<name>_removeAt(uint)`, `<name>_move(uint, uint)`. The element handle
        // spelling comes from the container's static element type; at/add are skipped if it is not
        // expressible (count/removeAt/move still register).
        void RegisterContainerMethods(const char* name, const foundation::TypeInfo& type,
                                      const foundation::PropertyInfo& property)
        {
            const foundation::ContainerInfo& ci = *property.type->container;
            const bool polymorphic = foundation::IsPolymorphicContainer(ci);
            foundation::String elemDecl;
            const bool elemSpellable = AppendDeclType(elemDecl, ci.elementType, /*isParam*/ false);

            auto reg = [&](const foundation::String& decl, Binding::Kind kind)
            {
                Binding* binding =
                    MakeBinding(Binding{kind, this, &type, nullptr, &property, nullptr});
                (void)m_engine->RegisterObjectMethod(name, CStr(decl), asFUNCTION(ContainerDispatch),
                                                     asCALL_GENERIC, binding);
            };
            {
                foundation::String d;
                AppendAscii(d, "uint ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_count()");
                reg(d, Binding::Kind::ContainerCount);
            }
            if (elemSpellable)
            {
                foundation::String d = elemDecl;
                AppendAscii(d, " ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_at(uint)");
                reg(d, Binding::Kind::ContainerAt);

                foundation::String a = elemDecl;
                AppendAscii(a, " ");
                AppendAscii(a, property.name);
                AppendAscii(a, polymorphic ? "_add(const string &in)" : "_add()");
                reg(a, Binding::Kind::ContainerAdd);
            }
            {
                foundation::String d;
                AppendAscii(d, "void ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_removeAt(uint)");
                reg(d, Binding::Kind::ContainerRemoveAt);
            }
            {
                foundation::String d;
                AppendAscii(d, "void ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_move(uint, uint)");
                reg(d, Binding::Kind::ContainerMove);
            }
        }

        // Bind a nested-VALUE member as a read getter returning a borrow handle: `<NestedType>@
        // get_<name>() property`. Skipped if the nested type is not expressible as a handle (not
        // declared). No setter - a nested value is edited in place through the borrow, not reassigned.
        void RegisterNestedGetter(const char* name, const foundation::TypeInfo& type,
                                  const foundation::PropertyInfo& property)
        {
            foundation::String decl;
            if (!AppendDeclType(decl, property.type, /*isParam*/ false))
            {
                return;
            }
            AppendAscii(decl, " get_");
            AppendAscii(decl, property.name);
            AppendAscii(decl, "() property");
            Binding* binding = MakeBinding(
                Binding{Binding::Kind::NestedGet, this, &type, nullptr, &property, nullptr});
            (void)m_engine->RegisterObjectMethod(name, CStr(decl), asFUNCTION(NestedGetDispatch),
                                                 asCALL_GENERIC, binding);
        }

        // Phase 2: bind the declared type's members.
        void BindType(const foundation::TypeInfo& type)
        {
            // An enum is fully declared by DeclareEnum (a native enum + its values); it has no object
            // members. Binding object behaviours (opAssign, factories, ...) onto an enum name crashes
            // AngelScript - so skip it here. (Also skips scalars/containers, which have no bindings.)
            if (type.enumeratorCount > 0)
            {
                return;
            }
            if (!IsDeclared(&type))
            {
                return;
            }
            const char* name = type.name;

            (void)m_engine->RegisterObjectBehaviour(name, asBEHAVE_ADDREF, "void f()",
                                                    asFUNCTION(AddRefDispatch), asCALL_GENERIC);
            (void)m_engine->RegisterObjectBehaviour(name, asBEHAVE_RELEASE, "void f()",
                                                    asFUNCTION(ReleaseDispatch), asCALL_GENERIC);

            // Value assignment so `Float3 p = expr;` works (copies the boxed value). All reflected
            // types are asOBJ_REF boxes, so AngelScript otherwise reports no opAssign.
            {
                foundation::String decl;
                AppendAscii(decl, name);
                AppendAscii(decl, "& opAssign(const ");
                AppendAscii(decl, name);
                AppendAscii(decl, "&in)");
                (void)m_engine->RegisterObjectMethod(name, CStr(decl), asFUNCTION(AssignDispatch),
                                                     asCALL_GENERIC);
            }

            foundation::Array<foundation::String> used; // exact-declaration dedupe

            // Factories: one per reflected constructor (`builder.Constructor()` is
            // the contract's constructibility requirement).
            for (foundation::usize i = 0; i < foundation::ConstructorCount(type); ++i)
            {
                const foundation::ConstructorInfo& constructor = foundation::ConstructorAt(type, i);
                foundation::String decl;
                AppendAscii(decl, name);
                AppendAscii(decl, "@ f(");
                if (!AppendParams(decl, constructor.params, constructor.paramCount))
                {
                    continue;
                }
                AppendAscii(decl, ")");
                if (IsUsed(used, decl))
                {
                    continue;
                }
                Binding* binding = MakeBinding(Binding{Binding::Kind::Constructor, this, &type,
                                                       &constructor, nullptr, nullptr});
                if (m_engine->RegisterObjectBehaviour(name, asBEHAVE_FACTORY, CStr(decl),
                                                      asFUNCTION(FactoryDispatch), asCALL_GENERIC,
                                                      binding) >= 0)
                {
                    used.PushBack(foundation::Move(decl));
                }
            }

            // Properties -> virtual property accessors (`v.x`, `v.x = 9`).
            for (foundation::usize i = 0; i < foundation::PropertyCount(type); ++i)
            {
                const foundation::PropertyInfo& property = foundation::PropertyAt(type, i);
                if (!IsValidIdentifier(property.name))
                {
                    continue;
                }
                if (foundation::IsNested(property))
                {
                    // A container member binds as owner methods (count/at/add/removeAt/move); a plain
                    // nested-VALUE member binds as a getter returning a borrow handle (edited in place).
                    if (property.type != nullptr && property.type->container != nullptr)
                    {
                        RegisterContainerMethods(name, type, property);
                    }
                    else if (property.type != nullptr)
                    {
                        RegisterNestedGetter(name, type, property);
                    }
                    continue;
                }
                {
                    foundation::String decl;
                    if (!AppendDeclType(decl, property.type, /*isParam*/ false))
                    {
                        continue;
                    }
                    AppendAscii(decl, " get_");
                    AppendAscii(decl, property.name);
                    AppendAscii(decl, "() property");
                    Binding* binding = MakeBinding(Binding{Binding::Kind::PropertyGet, this, &type,
                                                           nullptr, &property, nullptr});
                    (void)m_engine->RegisterObjectMethod(
                        name, CStr(decl), asFUNCTION(PropertyGetDispatch), asCALL_GENERIC, binding);
                }
                const bool readOnly = (static_cast<foundation::u32>(property.flags) &
                                       static_cast<foundation::u32>(foundation::PropertyFlags::ReadOnly)) != 0;
                if (!readOnly)
                {
                    foundation::String decl;
                    AppendAscii(decl, "void set_");
                    AppendAscii(decl, property.name);
                    AppendAscii(decl, "(");
                    if (!AppendDeclType(decl, property.type, /*isParam*/ true))
                    {
                        continue;
                    }
                    AppendAscii(decl, ") property");
                    Binding* binding = MakeBinding(Binding{Binding::Kind::PropertySet, this, &type,
                                                           nullptr, &property, nullptr});
                    (void)m_engine->RegisterObjectMethod(
                        name, CStr(decl), asFUNCTION(PropertySetDispatch), asCALL_GENERIC, binding);
                }
            }

            // Methods. AngelScript overloads by full signature, so EVERY reflected
            // overload registers distinctly (no Wren-style arity collapsing).
            // Statics become global functions in a namespace named after the class
            // - script calls read `Float3::Dot(a, b)`.
            foundation::Array<foundation::String> usedStatics;
            for (foundation::usize i = 0; i < foundation::MethodCount(type); ++i)
            {
                const foundation::MethodInfo& method = foundation::MethodAt(type, i);
                if (!IsValidIdentifier(method.name))
                {
                    continue;
                }
                const foundation::TypeInfo* returnType =
                    (method.returnType != nullptr) ? method.returnType() : nullptr;
                foundation::String decl;
                if (returnType == nullptr)
                {
                    AppendAscii(decl, "void");
                }
                else if (!AppendDeclType(decl, returnType, /*isParam*/ false))
                {
                    continue;
                }
                AppendAscii(decl, " ");
                AppendAscii(decl, method.name);
                AppendAscii(decl, "(");
                if (!AppendParams(decl, method.params, method.paramCount))
                {
                    continue;
                }
                AppendAscii(decl, ")");

                foundation::Array<foundation::String>& dedupe = method.isStatic ? usedStatics : used;
                if (IsUsed(dedupe, decl))
                {
                    continue;
                }
                Binding* binding = MakeBinding(
                    Binding{Binding::Kind::Method, this, &type, nullptr, nullptr, &method});
                int r;
                if (method.isStatic)
                {
                    (void)m_engine->SetDefaultNamespace(name);
                    r = m_engine->RegisterGlobalFunction(CStr(decl), asFUNCTION(MethodDispatch),
                                                         asCALL_GENERIC, binding);
                    (void)m_engine->SetDefaultNamespace("");
                }
                else
                {
                    r = m_engine->RegisterObjectMethod(name, CStr(decl), asFUNCTION(MethodDispatch),
                                                       asCALL_GENERIC, binding);
                }
                if (r >= 0)
                {
                    dedupe.PushBack(foundation::Move(decl));
                }
            }
        }

        bool AppendParams(foundation::String& decl, const foundation::ParamInfo* params, foundation::u32 count) const
        {
            for (foundation::u32 p = 0; p < count; ++p)
            {
                if (!AppendDeclType(decl, params[p].type != nullptr ? params[p].type() : nullptr,
                                    /*isParam*/ true))
                {
                    return false;
                }
                if (p + 1 < count)
                {
                    AppendAscii(decl, ", ");
                }
            }
            return true;
        }

        [[nodiscard]] static bool IsUsed(const foundation::Array<foundation::String>& used,
                                         const foundation::String& decl) noexcept
        {
            for (const foundation::String& existing : used)
            {
                if (existing == decl)
                {
                    return true;
                }
            }
            return false;
        }

        asIScriptEngine* m_engine = nullptr;
        int m_stringTypeId = -1;
        bool m_finalized = false;
        bool m_capturing = false;
        foundation::u32 m_nextContextId = 0;
        foundation::Array<const foundation::TypeInfo*> m_types;
        foundation::Array<RegisteredType> m_registered;
        foundation::Array<Binding*> m_bindings;
        foundation::Array<CapturedMessage> m_capturedMessages;
        foundation::Array<Coroutine> m_coroutines;
        foundation::Array<asIScriptContext*> m_dueScratch; // reused per-frame due snapshot
        AngelScriptDebugger* m_debugger = nullptr; // borrowed; the active debugger (self-registers)
    };

    // ---- generic dispatchers (run DURING script execution; the surrounding
    // Call/Load/Invoke already pushed the ScriptCallScope) ---------------------

    // Generic-call handle ownership (asEP_GENERIC_CALL_MODE == 1, the modern
    // default): a plain `Type@` parameter arrives with a reference the CALLEE
    // owns and must release - the engine adds no cleanup instruction for it.
    // Every dispatcher that takes arguments calls this after marshalling (the
    // Variants hold copies by then). Missing this leaks one reference per
    // object argument (found by ASAN).
    inline void ReleaseHandleArgs(asIScriptGeneric* gen, const AngelScriptManager* manager)
    {
        const asUINT argc = gen->GetArgCount();
        for (asUINT i = 0; i < argc; ++i)
        {
            const int typeId = gen->GetArgTypeId(i);
            if ((typeId & asTYPEID_OBJHANDLE) == 0)
            {
                continue;
            }
            if (manager->TypeInfoForTypeId(typeId) != nullptr)
            {
                ReleaseBox(static_cast<BoxedVariant*>(gen->GetArgObject(i)));
            }
            else if (manager->IsFuncdefTypeId(typeId) && !AngelScriptManager::ArgIsInReference(gen, i))
            {
                // A BY-VALUE funcdef-handle arg (e.g. a coroutine's ScriptCoroutine@): the callee
                // owns this reference. The wrapper AddRef'd its own; release the incoming one. A
                // `?&in` delegate handle is BORROWED (the caller owns its temporary) - skipped.
                if (asIScriptFunction* fn = static_cast<asIScriptFunction*>(gen->GetArgObject(i)))
                {
                    fn->Release();
                }
            }
        }
    }
    void FactoryDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        int argc = static_cast<int>(gen->GetArgCount());
        if (argc > kMaxArgs)
        {
            argc = kMaxArgs;
        }
        foundation::Variant args[kMaxArgs];
        for (int i = 0; i < argc; ++i)
        {
            const foundation::ParamInfo& param = binding->constructor->params[i];
            args[i] = binding->manager->ValueFromArg(
                gen, static_cast<asUINT>(i), param.type != nullptr ? param.type() : nullptr);
        }
        ReleaseHandleArgs(gen, binding->manager);
        foundation::Result<foundation::Variant> created = binding->constructor->invoke(
            foundation::Span<foundation::Variant>{args, static_cast<foundation::usize>(argc)});
        if (!created.HasValue())
        {
            *static_cast<void**>(gen->GetAddressOfReturnLocation()) = nullptr;
            if (asIScriptContext* active = asGetActiveContext())
            {
                active->SetException("reflected constructor failed");
            }
            return;
        }
        *static_cast<void**>(gen->GetAddressOfReturnLocation()) =
            NewBox(foundation::Move(created.Value()));
    }

    // Value assignment (T& opAssign(const T&in other)): every reflected type is an asOBJ_REF box, so
    // without this AngelScript rejects `Float3 p = expr;` with "no opAssign for value assignment".
    // Copies the source box's Variant into this one and returns *this (a reference).
    void AssignDispatch(asIScriptGeneric* gen)
    {
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        const BoxedVariant* other = static_cast<const BoxedVariant*>(gen->GetArgObject(0));
        if (self != nullptr && other != nullptr)
        {
            self->value = other->value;
        }
        gen->SetReturnAddress(self);
    }

    void AddRefDispatch(asIScriptGeneric* gen)
    {
        ++static_cast<BoxedVariant*>(gen->GetObject())->refCount;
    }

    void ReleaseDispatch(asIScriptGeneric* gen)
    {
        ReleaseBox(static_cast<BoxedVariant*>(gen->GetObject()));
    }

    void PropertyGetDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        foundation::Instance instance = foundation::ToInstance(self->value);
        binding->manager->SetGenericReturn(gen, foundation::GetProperty(*binding->property, instance));
    }

    void PropertySetDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        foundation::Instance instance = foundation::ToInstance(self->value);
        const foundation::Variant value = binding->manager->ValueFromArg(gen, 0, binding->property->type);
        ReleaseHandleArgs(gen, binding->manager);
        (void)foundation::SetProperty(*binding->property, instance, value);
    }

    // Resolve a polymorphic container's add-by-name to a concrete derived type: match the name
    // against each creatable derived type's `displayName` attribute, then its bare type name.
    const foundation::TypeInfo* ResolveElementType(const foundation::TypeInfo& base, foundation::StringView name)
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
            if (foundation::StringView(reinterpret_cast<const foundation::utf8char*>(t->name)) == name)
            {
                return t;
            }
        }
        return nullptr;
    }

    // The Variant to hand a script for container element `index`: an object / value element via getAt
    // (owned); a NON-Object value element (getAt empty) via a BORROW over its address, pinned to the
    // container owner `parent` and generation-guarded. Empty if out of range / no element.
    foundation::Variant ContainerElementVariant(const foundation::ContainerInfo& ci,
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

    // A nested-VALUE member getter: returns a borrow handle over the member address (edited in place).
    void NestedGetDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        foundation::Instance owner = foundation::ToInstance(self->value);
        void* addr =
            (owner.Pointer() != nullptr) ? binding->property->address(owner) : nullptr;
        binding->manager->SetGenericReturn(
            gen, addr != nullptr
                     ? foundation::Variant::Borrow(addr, binding->property->type, self->value)
                     : foundation::Variant{});
    }

    // One dispatcher for every container op; the Binding::Kind selects which. The owner is `self`,
    // the container member is reached transiently via property.address. Object / value elements come
    // back owned; a non-Object value element comes back as a borrow (pinned to the owner).
    void ContainerDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        foundation::Instance owner = foundation::ToInstance(self->value);
        foundation::Instance container(binding->property->address(owner), binding->property->type);
        const foundation::ContainerInfo& ci = *binding->property->type->container;
        const foundation::usize size = foundation::ContainerSize(ci, container);
        switch (binding->kind)
        {
        case Binding::Kind::ContainerCount:
            gen->SetReturnDWord(static_cast<asDWORD>(size));
            break;
        case Binding::Kind::ContainerAt:
        {
            const foundation::usize idx = static_cast<foundation::usize>(gen->GetArgDWord(0));
            binding->manager->SetGenericReturn(
                gen, idx < size ? ContainerElementVariant(ci, container, idx, self->value)
                                : foundation::Variant{});
            break;
        }
        case Binding::Kind::ContainerAdd:
        {
            if (foundation::IsPolymorphicContainer(ci)) // add by element-type name
            {
                const foundation::Variant nameV =
                    binding->manager->ValueFromArg(gen, 0, &foundation::TypeOf<foundation::String>());
                ReleaseHandleArgs(gen, binding->manager);
                const foundation::String* typeName = nameV.TryGet<foundation::String>();
                const foundation::TypeInfo* elem =
                    (typeName != nullptr && ci.elementType != nullptr)
                        ? ResolveElementType(*ci.elementType, typeName->AsView())
                        : nullptr;
                if (elem == nullptr ||
                    foundation::ContainerCreateElement(ci, container, size, *elem).Pointer() == nullptr)
                {
                    binding->manager->SetGenericReturn(gen, foundation::Variant{});
                    break;
                }
            }
            else if (foundation::ContainerEmplaceDefault(ci, container, size).Pointer() == nullptr)
            {
                binding->manager->SetGenericReturn(gen, foundation::Variant{});
                break;
            }
            binding->manager->SetGenericReturn(gen,
                                               ContainerElementVariant(ci, container, size,
                                                                       self->value));
            break;
        }
        case Binding::Kind::ContainerRemoveAt:
            (void)foundation::ContainerRemoveAt(ci, container,
                                          static_cast<foundation::usize>(gen->GetArgDWord(0)));
            break;
        case Binding::Kind::ContainerMove:
            (void)foundation::ContainerMoveElement(ci, container,
                                             static_cast<foundation::usize>(gen->GetArgDWord(0)),
                                             static_cast<foundation::usize>(gen->GetArgDWord(1)));
            break;
        default:
            break;
        }
    }

    void MethodDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        const foundation::MethodInfo& method = *binding->method;
        int argc = static_cast<int>(gen->GetArgCount());
        if (argc > kMaxArgs)
        {
            argc = kMaxArgs;
        }
        foundation::Variant args[kMaxArgs];
        for (int i = 0; i < argc; ++i)
        {
            const foundation::TypeInfo* expected =
                (i < static_cast<int>(method.paramCount) && method.params[i].type != nullptr)
                    ? method.params[i].type()
                    : nullptr;
            args[i] = binding->manager->ValueFromArg(gen, static_cast<asUINT>(i), expected);
        }
        ReleaseHandleArgs(gen, binding->manager);
        const foundation::Span<foundation::Variant> argSpan{args, static_cast<foundation::usize>(argc)};
        foundation::Result<foundation::Variant> result = foundation::Err(foundation::ErrorCode::Internal);
        if (method.isStatic)
        {
            result = foundation::InvokeStatic(method, argSpan);
        }
        else
        {
            BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
            result = foundation::InvokeMethod(method, foundation::ToInstance(self->value), argSpan);
        }
        binding->manager->SetGenericReturn(gen,
                                           result.HasValue() ? result.Value() : foundation::Variant{});
    }

    // ---- coroutine host functions (auxiliary = the manager) ----
    void CoroutineStartDispatch(asIScriptGeneric* gen)
    {
        AngelScriptManager* manager = static_cast<AngelScriptManager*>(gen->GetAuxiliary());
        asIScriptFunction* fn = static_cast<asIScriptFunction*>(gen->GetArgObject(0));
        if (manager != nullptr)
        {
            manager->StartCoroutine(fn);
        }
    }

    void CoroutineWaitDispatch(asIScriptGeneric* gen)
    {
        AngelScriptManager* manager = static_cast<AngelScriptManager*>(gen->GetAuxiliary());
        if (manager != nullptr)
        {
            manager->CoroutineWaitCurrent(gen->GetArgFloat(0));
        }
    }

    // ---- context -------------------------------------------------------------
    class AngelScriptContext final : public IScriptContext
    {
    public:
        AngelScriptContext(foundation::RefPtr<AngelScriptManager> manager, foundation::u32 id)
            : m_manager(foundation::Move(manager))
        {
            AppendAscii(m_namePrefix, "ctx");
            AppendUint(m_namePrefix, id);
        }

        ~AngelScriptContext() override
        {
            // No ScriptObject outlives its context (they hold a strong ref), so
            // discarding this context's modules is safe here.
            for (asIScriptModule* module : m_ownedModules)
            {
                module->Discard();
            }
        }

        AngelScriptContext(const AngelScriptContext&) = delete;
        AngelScriptContext& operator=(const AngelScriptContext&) = delete;

        void SetErrorHandler(IScriptErrorHandler* handler) override { m_errorHandler = handler; }

        foundation::Status Load(foundation::StringView source, foundation::StringView chunkName) override
        {
            asIScriptEngine* engine = m_manager->Engine();
            foundation::String moduleName = m_namePrefix;
            AppendAscii(moduleName, ":");
            AppendUint(moduleName, m_loadCounter++);
            asIScriptModule* module = engine->GetModule(CStr(moduleName), asGM_ALWAYS_CREATE);
            if (module == nullptr)
            {
                return foundation::Status{foundation::ErrorCode::Internal};
            }

            const foundation::String section(chunkName);
            (void)module->AddScriptSection(
                CStr(section), reinterpret_cast<const char*>(source.Data()), source.Size());
            // The in-script `waitUntil` helper, in its OWN section so it never shifts the
            // user source's error line numbers (the funcdefs + `wait` are engine-global).
            (void)module->AddScriptSection("__coroutine_support", kCoroutinePreludeSection);

            // Build compiles AND runs global initializers; classify buffered errors
            // by the return code (compile failure vs failed global init).
            int result;
            m_manager->BeginMessageCapture();
            {
                ScriptCallScope scope(this);
                result = module->Build();
            }
            const bool initFailed = (result == asINIT_GLOBAL_VARS_FAILED);
            m_manager->EndMessageCapture(m_errorHandler, initFailed ? ScriptErrorKind::Runtime
                                                                    : ScriptErrorKind::Compile);
            if (result < 0)
            {
                module->Discard();
                return foundation::Status{initFailed ? foundation::ErrorCode::Internal
                                               : foundation::ErrorCode::InvalidArgument};
            }

            m_ownedModules.PushBack(module);
            m_module = module;

            // Top-level entry convention: a module-level `void main()` runs at load
            // (AngelScript has no top-level statements; this is the runtime-fault
            // and script-setup seam the contract's Load semantics map onto).
            if (asIScriptFunction* entry = module->GetFunctionByName("main"))
            {
                foundation::Result<foundation::Variant> ran =
                    ExecuteCall(entry, nullptr, foundation::Span<foundation::Variant>{});
                if (!ran.HasValue())
                {
                    return foundation::Status{foundation::ErrorCode::Internal};
                }
            }
            return foundation::Status{};
        }

        // The behaviors module, loaded with each class in its OWN script section named by
        // its sourceName - so GetLineNumber reports (sourceFile, sourceLine) and an editor
        // breakpoint keyed on the file lines up (script-debugger.md P1.5). Same framing +
        // build/classify path as Load; only the section split differs (Load uses one section).
        foundation::Status LoadBehaviorModule(foundation::Span<const BehaviorModuleClass> classes,
                                        foundation::StringView moduleName) override
        {
            asIScriptEngine* engine = m_manager->Engine();
            const foundation::String moduleNameStr(moduleName);
            asIScriptModule* module = engine->GetModule(CStr(moduleNameStr), asGM_ALWAYS_CREATE);
            if (module == nullptr)
            {
                return foundation::Status{foundation::ErrorCode::Internal};
            }

            // One section PER CLASS, named by its sourceName (the editor's breakpoint key).
            // A class with no sourceName falls back to the module name (still compiles; only
            // its breakpoints won't line up - the cook always stamps sourceName).
            for (const BehaviorModuleClass& entry : classes)
            {
                const foundation::String section(entry.name.IsEmpty() ? moduleName : entry.name);
                (void)module->AddScriptSection(CStr(section),
                                               reinterpret_cast<const char*>(entry.source.Data()),
                                               entry.source.Size());
            }
            // The in-script `waitUntil` helper, in its OWN section (line numbers unaffected).
            (void)module->AddScriptSection("__coroutine_support", kCoroutinePreludeSection);

            int result;
            m_manager->BeginMessageCapture();
            {
                ScriptCallScope scope(this);
                result = module->Build();
            }
            const bool initFailed = (result == asINIT_GLOBAL_VARS_FAILED);
            m_manager->EndMessageCapture(m_errorHandler, initFailed ? ScriptErrorKind::Runtime
                                                                    : ScriptErrorKind::Compile);
            if (result < 0)
            {
                module->Discard();
                return foundation::Status{initFailed ? foundation::ErrorCode::Internal
                                               : foundation::ErrorCode::InvalidArgument};
            }

            m_ownedModules.PushBack(module);
            m_module = module;

            if (asIScriptFunction* entry = module->GetFunctionByName("main"))
            {
                foundation::Result<foundation::Variant> ran =
                    ExecuteCall(entry, nullptr, foundation::Span<foundation::Variant>{});
                if (!ran.HasValue())
                {
                    return foundation::Status{foundation::ErrorCode::Internal};
                }
            }
            return foundation::Status{};
        }

        void SetGlobal(foundation::StringView name, const foundation::Variant& value) override
        {
            if (m_module == nullptr)
            {
                return;
            }
            const foundation::String globalName(name);
            const int index = m_module->GetGlobalVarIndexByName(CStr(globalName));
            if (index < 0)
            {
                return;
            }
            int typeId = 0;
            (void)m_module->GetGlobalVar(static_cast<asUINT>(index), nullptr, nullptr, &typeId,
                                         nullptr);
            (void)m_manager->WriteTypedAddress(
                typeId, m_module->GetAddressOfGlobalVar(static_cast<asUINT>(index)), value);
        }

        [[nodiscard]] foundation::Variant GetGlobal(foundation::StringView name) override
        {
            if (m_module == nullptr)
            {
                return foundation::Variant{};
            }
            const foundation::String globalName(name);
            const int index = m_module->GetGlobalVarIndexByName(CStr(globalName));
            if (index < 0)
            {
                return foundation::Variant{};
            }
            int typeId = 0;
            (void)m_module->GetGlobalVar(static_cast<asUINT>(index), nullptr, nullptr, &typeId,
                                         nullptr);
            return m_manager->VariantFromTypedAddress(
                typeId, m_module->GetAddressOfGlobalVar(static_cast<asUINT>(index)));
        }

        [[nodiscard]] bool HasFunction(foundation::StringView name) const override
        {
            return FindFunction(name, -1) != nullptr;
        }

        [[nodiscard]] foundation::Result<foundation::Variant> Call(foundation::StringView function,
                                                       foundation::Span<foundation::Variant> args) override
        {
            // Strict arity: never Execute with unset argument slots.
            asIScriptFunction* target = FindFunction(function, static_cast<int>(args.Size()));
            if (target == nullptr || target->GetParamCount() != args.Size())
            {
                return foundation::Err(foundation::ErrorCode::NotFound);
            }
            return ExecuteCall(target, nullptr, args);
        }

        [[nodiscard]] foundation::RefPtr<ScriptObject>
        CreateInstance(foundation::StringView className, foundation::Span<foundation::Variant> args) override;

        // Prepare + execute a script function on the engine's pooled contexts.
        // The public seam AngelScriptObject::Invoke dispatches through as well.
        [[nodiscard]] foundation::Result<foundation::Variant>
        ExecuteCall(asIScriptFunction* function, void* object, foundation::Span<foundation::Variant> args)
        {
            asIScriptEngine* engine = m_manager->Engine();
            asIScriptContext* executor = engine->RequestContext();
            if (executor == nullptr || executor->Prepare(function) < 0)
            {
                if (executor != nullptr)
                {
                    engine->ReturnContext(executor);
                }
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            if (object != nullptr)
            {
                (void)executor->SetObject(object);
            }

            std::string stringTemps[kMaxArgs];
            BoxedVariant* boxTemps[kMaxArgs] = {};
            BindArgs(executor, function, args, stringTemps, boxTemps);

            // Arm the step debugger (if attached) on this executor: its line callback calls
            // ctx->Suspend() on a breakpoint/step line, unwinding back here as SUSPENDED.
            AngelScriptDebugger* debugger = m_manager->ActiveDebugger();
            if (debugger != nullptr)
            {
                ArmDebugger(*debugger, executor, this);
            }

            int result;
            {
                ScriptCallScope scope(this);
                result = executor->Execute();
            }

            // A DEBUGGER suspension is distinct from a coroutine suspend (which happens on a
            // coroutine's OWN dedicated context, never this pooled executor). When the debugger
            // caused it, it now OWNS the suspended context for inspection + resume: do NOT
            // return it to the pool, and report a paused status the subsystem recognizes (via
            // the run host's pause flag) - never a fault, never completion. Value args only
            // (lifecycle handlers take a numeric dt or nothing), so releasing box args is safe.
            if (result == asEXECUTION_SUSPENDED && debugger != nullptr &&
                AdoptDebuggerSuspension(*debugger, executor))
            {
                for (BoxedVariant* box : boxTemps)
                {
                    ReleaseBox(box);
                }
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            for (BoxedVariant* box : boxTemps)
            {
                ReleaseBox(box);
            }

            foundation::Result<foundation::Variant> outcome = foundation::Err(foundation::ErrorCode::Internal);
            if (result == asEXECUTION_FINISHED)
            {
                const int returnTypeId = function->GetReturnTypeId();
                outcome = (returnTypeId == asTYPEID_VOID)
                              ? foundation::Result<foundation::Variant>(foundation::Variant{})
                              : foundation::Result<foundation::Variant>(m_manager->VariantFromTypedAddress(
                                    returnTypeId, executor->GetAddressOfReturnValue()));
            }
            else if (result == asEXECUTION_EXCEPTION)
            {
                ReportException(executor);
            }
            // Never return a context to the pool carrying a stale line callback.
            if (debugger != nullptr)
            {
                executor->ClearLineCallback();
            }
            engine->ReturnContext(executor);
            return outcome;
        }

        [[nodiscard]] AngelScriptManager& Manager() noexcept { return *m_manager; }

    private:
        [[nodiscard]] asIScriptFunction* FindFunction(foundation::StringView name, int argc) const
        {
            if (m_module == nullptr)
            {
                return nullptr;
            }
            const foundation::String functionName(name);
            asIScriptFunction* byName = nullptr;
            for (asUINT i = 0; i < m_module->GetFunctionCount(); ++i)
            {
                asIScriptFunction* candidate = m_module->GetFunctionByIndex(i);
                const char* candidateName = candidate->GetName();
                if (candidateName == nullptr || !NameEq(candidateName, CStr(functionName)))
                {
                    continue;
                }
                if (argc < 0 || static_cast<int>(candidate->GetParamCount()) == argc)
                {
                    return candidate;
                }
                if (byName == nullptr)
                {
                    byName = candidate;
                }
            }
            return byName;
        }

        void BindArgs(asIScriptContext* executor, asIScriptFunction* function,
                      foundation::Span<foundation::Variant> args, std::string* stringTemps,
                      BoxedVariant** boxTemps)
        {
            const foundation::usize limit = function->GetParamCount();
            for (foundation::usize i = 0;
                 i < args.Size() && i < limit && i < static_cast<foundation::usize>(kMaxArgs); ++i)
            {
                const asUINT arg = static_cast<asUINT>(i);
                int typeId = 0;
                (void)function->GetParam(arg, &typeId);
                const foundation::Variant& value = args[i];
                bool ok = false;
                const double number = NumericOf(value, ok);
                switch (typeId)
                {
                case asTYPEID_BOOL:
                    (void)executor->SetArgByte(arg, number != 0.0 ? 1 : 0);
                    continue;
                case asTYPEID_INT8:
                case asTYPEID_UINT8:
                    (void)executor->SetArgByte(arg,
                                               static_cast<asBYTE>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_INT16:
                case asTYPEID_UINT16:
                    (void)executor->SetArgWord(arg,
                                               static_cast<asWORD>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_INT32:
                case asTYPEID_UINT32:
                    (void)executor->SetArgDWord(
                        arg, static_cast<asDWORD>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_INT64:
                case asTYPEID_UINT64:
                    (void)executor->SetArgQWord(
                        arg, static_cast<asQWORD>(static_cast<foundation::i64>(number)));
                    continue;
                case asTYPEID_FLOAT:
                    (void)executor->SetArgFloat(arg, static_cast<float>(number));
                    continue;
                case asTYPEID_DOUBLE:
                    (void)executor->SetArgDouble(arg, number);
                    continue;
                default:
                    break;
                }
                if (typeId == m_manager->StringTypeId())
                {
                    stringTemps[i] = StdFromVariantString(value);
                    // Copied for by-value params; referenced (temps outlive Execute)
                    // for &in params.
                    (void)executor->SetArgObject(arg, &stringTemps[i]);
                    continue;
                }
                if ((typeId & asTYPEID_OBJHANDLE) != 0 &&
                    m_manager->TypeInfoForTypeId(typeId) != nullptr && !value.IsEmpty())
                {
                    boxTemps[i] =
                        NewBox(value); // SetArgObject AddRefs; ours released after Execute
                    (void)executor->SetArgObject(arg, boxTemps[i]);
                    continue;
                }
            }
        }

        void ReportException(asIScriptContext* executor)
        {
            const char* section = nullptr;
            const int line = executor->GetExceptionLineNumber(nullptr, &section);
            if (m_errorHandler != nullptr)
            {
                const ScriptError error{ScriptErrorKind::Runtime, ViewOfAscii(section),
                                        static_cast<foundation::i32>(line),
                                        ViewOfAscii(executor->GetExceptionString())};
                m_errorHandler->OnError(error);
                return;
            }
            foundation::ConsoleWriteError(ViewOfAscii(executor->GetExceptionString()));
        }

        foundation::RefPtr<AngelScriptManager> m_manager;
        IScriptErrorHandler* m_errorHandler = nullptr;
        foundation::String m_namePrefix;
        foundation::u32 m_loadCounter = 0;
        asIScriptModule* m_module = nullptr; // most recent successful Load
        foundation::Array<asIScriptModule*> m_ownedModules;
    };

    // A live instance of a script-declared class. Holds the asIScriptObject plus a
    // strong reference to its owning context (which keeps the manager - and thus
    // the engine - alive).
    class AngelScriptObject final : public ScriptObject
    {
    public:
        AngelScriptObject(foundation::RefPtr<AngelScriptContext> owner,
                          asIScriptObject* instance) noexcept
            : m_owner(foundation::Move(owner)), m_instance(instance)
        {
        }

        ~AngelScriptObject() override
        {
            if (m_instance != nullptr)
            {
                m_instance->Release();
            }
        }

        AngelScriptObject(const AngelScriptObject&) = delete;
        AngelScriptObject& operator=(const AngelScriptObject&) = delete;

        /// The underlying script object (CancelCoroutinesFor matches coroutines by owner).
        [[nodiscard]] asIScriptObject* ScriptInstance() const noexcept { return m_instance; }

        [[nodiscard]] foundation::Result<foundation::Variant> Invoke(foundation::StringView method,
                                                         foundation::Span<foundation::Variant> args) override
        {
            asITypeInfo* type = m_instance->GetObjectType();
            if (type == nullptr)
            {
                return foundation::Err(foundation::ErrorCode::NotFound);
            }

            // The neutral property-apply path Invokes the setter as `<name>=` with one
            // argument (the Wren `name=(v)` setter convention). AngelScript has no method
            // by that name; its editor properties are plain member FIELDS. So a trailing
            // `=` with exactly one arg writes the same-named member field directly - the
            // "settable member" AngelScript exposes for harvested behavior properties.
            if (args.Size() == 1 && !method.IsEmpty() && method[method.Size() - 1] == u8'=')
            {
                const foundation::StringView fieldName = method.SubStr(0, method.Size() - 1);
                if (SetMemberField(fieldName, args[0]))
                {
                    return foundation::Variant{};
                }
                return foundation::Err(foundation::ErrorCode::NotFound);
            }

            const foundation::String methodName(method);
            asIScriptFunction* target = nullptr;
            for (asUINT i = 0; i < type->GetMethodCount(); ++i)
            {
                asIScriptFunction* candidate = type->GetMethodByIndex(i);
                const char* candidateName = candidate->GetName();
                if (candidateName != nullptr && NameEq(candidateName, CStr(methodName)) &&
                    candidate->GetParamCount() == args.Size())
                {
                    target = candidate;
                    break;
                }
            }
            if (target == nullptr)
            {
                return foundation::Err(foundation::ErrorCode::NotFound);
            }
            return m_owner->ExecuteCall(target, m_instance, args);
        }

    private:
        // Writes the instance member field named `fieldName` from `value` (the harvested
        // editor-property apply path). Marshals through the manager's typed-address writer,
        // so scalars/string/reflected-handle members all take the value uniformly. False
        // when no such (writable) member exists or the value's type cannot fill it.
        [[nodiscard]] bool SetMemberField(foundation::StringView fieldName, const foundation::Variant& value)
        {
            const foundation::String name(fieldName);
            const asUINT count = m_instance->GetPropertyCount();
            for (asUINT i = 0; i < count; ++i)
            {
                const char* memberName = m_instance->GetPropertyName(i);
                if (memberName == nullptr || !NameEq(memberName, CStr(name)))
                {
                    continue;
                }
                const int typeId = m_instance->GetPropertyTypeId(i);
                if (m_owner->Manager().WriteTypedAddress(
                        typeId, m_instance->GetAddressOfProperty(i), value))
                {
                    return true;
                }
                // A reflected/resource property (asset:X, or any reflected value) declared as a
                // plain VALUE member cannot be set from native (AngelScript owns its lifecycle).
                // Guide the author to use a handle - the idiomatic AngelScript spelling for a
                // reference type - which DOES take the value.
                if ((typeId & asTYPEID_MASK_OBJECT) != 0 && (typeId & asTYPEID_OBJHANDLE) == 0)
                {
                    DRACONIC_LOG_WARNING(
                        u8"Script",
                        u8"AngelScript property '{}' is a value member; declare it as a handle "
                        u8"('Type@ {}') to receive a reflected/resource value",
                        memberName, memberName);
                }
                return false;
            }
            return false;
        }

        foundation::RefPtr<AngelScriptContext> m_owner;
        asIScriptObject* m_instance;
    };

    // A script function/closure held as a native callback. Holds the manager (keeps the
    // engine alive) and one AddRef on the funcdef handle (the GC-safe promise); Invoke runs
    // it on a pooled context. It references the MANAGER, not the owning context, so it never
    // cycles with a context that stores it back (e.g. through a reflected event object).
    class AngelScriptDelegate final : public IScriptDelegate
    {
    public:
        AngelScriptDelegate(foundation::RefPtr<AngelScriptManager> manager,
                            asIScriptFunction* function) noexcept
            : m_manager(foundation::Move(manager)), m_function(function)
        {
            if (m_function != nullptr)
            {
                m_function->AddRef();
            }
        }

        ~AngelScriptDelegate() override
        {
            if (m_function != nullptr)
            {
                m_function->Release();
            }
        }

        AngelScriptDelegate(const AngelScriptDelegate&) = delete;
        AngelScriptDelegate& operator=(const AngelScriptDelegate&) = delete;

        [[nodiscard]] foundation::Result<foundation::Variant> Invoke(foundation::Span<foundation::Variant> args) override
        {
            if (m_manager.Get() == nullptr || m_function == nullptr)
            {
                return foundation::Err(foundation::ErrorCode::Internal);
            }
            return m_manager->ExecuteDelegate(m_function, args);
        }

    private:
        foundation::RefPtr<AngelScriptManager> m_manager;
        asIScriptFunction* m_function;
    };

    foundation::Variant MakeAngelScriptDelegateVariant(asIScriptFunction* function)
    {
        // The wrapping happens inside a reflected dispatch, so the executing context (and its
        // manager) is the current script context.
        IScriptContext* current = CurrentScriptContext();
        if (current == nullptr || function == nullptr)
        {
            return foundation::Variant{};
        }
        AngelScriptContext* context = static_cast<AngelScriptContext*>(current);
        foundation::RefPtr<AngelScriptManager> manager(&context->Manager());
        foundation::RefPtr<IScriptDelegate> delegate(foundation::MakeRef<AngelScriptDelegate>(
            foundation::DefaultAllocator(), foundation::Move(manager), function));
        return foundation::Variant::From(delegate);
    }

    void AngelScriptManager::CancelCoroutinesFor(ScriptObject& instance)
    {
        // We hold the owner pointer directly - drop every coroutine started by this
        // behavior instance (its context is aborted + released, its owner ref freed).
        asIScriptObject* owner = static_cast<AngelScriptObject&>(instance).ScriptInstance();
        for (foundation::usize i = m_coroutines.Size(); i-- > 0;)
        {
            if (m_coroutines[i].owner == owner)
            {
                DropCoroutineAt(i);
            }
        }
    }

    // ---- the suspension-based step debugger --------------------------------------
    //
    // Single-threaded, non-blocking: a breakpoint is a line callback that calls
    // ctx->Suspend(), unwinding execution back to ExecuteCall (which reports it up as a
    // paused status). While suspended the context is fully inspectable (GetCallstackSize /
    // GetVar / GetAddressOfVar). Continue/Step re-Execute the SAME held context. The editor
    // UI + a future remote transport drive only the neutral IScriptDebugger - nothing here
    // leaks to the contract.
    class AngelScriptDebugger final : public IScriptDebugger
    {
    public:
        explicit AngelScriptDebugger(AngelScriptManager* manager) noexcept : m_manager(manager)
        {
            if (m_manager != nullptr)
            {
                m_manager->SetActiveDebugger(this);
            }
        }

        ~AngelScriptDebugger() override
        {
            // A held (paused) context is aborted + returned so the engine tears down cleanly.
            if (m_pausedContext != nullptr)
            {
                (void)m_pausedContext->Abort();
                m_pausedContext->ClearLineCallback();
                if (m_manager != nullptr && m_manager->Engine() != nullptr)
                {
                    m_manager->Engine()->ReturnContext(m_pausedContext);
                }
                m_pausedContext = nullptr;
            }
            if (m_manager != nullptr)
            {
                m_manager->SetActiveDebugger(nullptr);
            }
        }

        AngelScriptDebugger(const AngelScriptDebugger&) = delete;
        AngelScriptDebugger& operator=(const AngelScriptDebugger&) = delete;

        // ---- IScriptDebugger -------------------------------------------------
        void SetBreakpoint(foundation::StringView file, foundation::i32 line) override
        {
            for (const Breakpoint& breakpoint : m_breakpoints)
            {
                if (breakpoint.line == line && breakpoint.file.AsView() == file)
                {
                    return;
                }
            }
            m_breakpoints.PushBack(Breakpoint{foundation::String(file), line});
        }

        void RemoveBreakpoint(foundation::StringView file, foundation::i32 line) override
        {
            for (foundation::usize i = 0; i < m_breakpoints.Size(); ++i)
            {
                if (m_breakpoints[i].line == line && m_breakpoints[i].file.AsView() == file)
                {
                    m_breakpoints.RemoveAt(i);
                    return;
                }
            }
        }

        // Suspend at the next executed line (a manual pause). Honoured on the next line
        // callback of whatever context is currently executing.
        void Break() override { m_breakNext = true; }

        void Continue() override { Resume(StepMode::None); }
        void StepInto() override { Resume(StepMode::Into); }
        void StepOver() override { Resume(StepMode::Over); }

        [[nodiscard]] foundation::Array<ScriptStackFrame> CaptureStackFrames() override
        {
            foundation::Array<ScriptStackFrame> frames;
            if (m_pausedContext == nullptr)
            {
                return frames;
            }
            const asUINT size = m_pausedContext->GetCallstackSize();
            for (asUINT level = 0; level < size; ++level) // level 0 = innermost
            {
                ScriptStackFrame frame;
                const char* section = nullptr;
                frame.line = m_pausedContext->GetLineNumber(level, nullptr, &section);
                frame.file = foundation::String(ViewOfAscii(section));
                asIScriptFunction* function = m_pausedContext->GetFunction(level);
                frame.function = foundation::String(
                    ViewOfAscii(function != nullptr ? function->GetDeclaration() : "?"));
                frames.PushBack(foundation::Move(frame));
            }
            return frames;
        }

        [[nodiscard]] foundation::Array<ScriptVariable> CaptureLocals(foundation::u32 depth) override
        {
            foundation::Array<ScriptVariable> locals;
            if (m_pausedContext == nullptr)
            {
                return locals;
            }
            const int count = m_pausedContext->GetVarCount(depth);
            for (int i = 0; i < count; ++i)
            {
                const char* name = nullptr;
                int typeId = 0;
                if (m_pausedContext->GetVar(static_cast<asUINT>(i), depth, &name, &typeId) < 0)
                {
                    continue;
                }
                if (name == nullptr || name[0] == '\0')
                {
                    continue;
                } // unnamed temporary
                ScriptVariable variable;
                variable.name = foundation::String(ViewOfAscii(name));
                const char* declaration =
                    m_pausedContext->GetVarDeclaration(static_cast<asUINT>(i), depth, false);
                variable.typeName = foundation::String(ViewOfAscii(declaration));
                void* address = m_pausedContext->GetAddressOfVar(static_cast<asUINT>(i), depth);
                if (address == nullptr)
                {
                    variable.value = foundation::String(u8"<uninitialized>");
                    locals.PushBack(foundation::Move(variable));
                    continue;
                }
                foundation::Variant value = m_manager->VariantFromTypedAddress(typeId, address);
                DescribeValue(variable, value);
                locals.PushBack(foundation::Move(variable));
            }
            return locals;
        }

        [[nodiscard]] foundation::Array<ScriptVariable> CaptureObject(foundation::u64 objectRef) override
        {
            foundation::Array<ScriptVariable> members;
            foundation::Variant* stored = FindObject(objectRef);
            if (stored == nullptr)
            {
                return members;
            }
            const foundation::TypeInfo* type = stored->Type();
            if (type == nullptr)
            {
                return members;
            }
            foundation::Instance instance = foundation::ToInstance(*stored);
            for (foundation::usize i = 0; i < foundation::PropertyCount(*type); ++i)
            {
                const foundation::PropertyInfo& property = foundation::PropertyAt(*type, i);
                if (foundation::IsNested(property))
                {
                    continue; // no by-value read for a nested structure (empty Variant)
                }
                ScriptVariable variable;
                variable.name = foundation::String(ViewOfAscii(property.name));
                variable.typeName =
                    foundation::String(ViewOfAscii(property.type != nullptr ? property.type->name : "?"));
                foundation::Variant value = foundation::GetProperty(property, instance);
                DescribeValue(variable, value);
                members.PushBack(foundation::Move(variable));
            }
            return members;
        }

        void SetListener(IScriptDebuggerListener* listener) override { m_listener = listener; }

        // ---- called by the dispatch path (via the free helpers below) --------

        void ArmOn(asIScriptContext* ctx, IScriptContext* owner)
        {
            m_owner = owner;
            (void)ctx->SetLineCallback(asFUNCTION(DebuggerLineCallback), this, asCALL_CDECL);
        }

        // True (and adopts the context) when this debugger's line callback suspended `ctx`.
        [[nodiscard]] bool Adopt(asIScriptContext* ctx)
        {
            if (m_pausedContext != ctx)
            {
                return false;
            }
            m_paused = true;
            FireState(m_cause == Cause::Step ? ScriptDebuggerState::Stepped
                                             : ScriptDebuggerState::Breakpoint);
            return true;
        }

        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }

        // The line callback (control is INSIDE Execute here): suspend when the current line is
        // a breakpoint, a satisfied step, or a pending manual Break.
        void OnLine(asIScriptContext* ctx)
        {
            // One held context at a time: while paused, a re-entrant script call (an event
            // handler firing during the pause) runs through without suspending - adopting a
            // second context would orphan the held one.
            if (m_pausedContext != nullptr && m_pausedContext != ctx)
            {
                return;
            }
            const char* section = nullptr;
            const int line = ctx->GetLineNumber(0, nullptr, &section);
            Cause cause = Cause::Breakpoint;
            bool suspend = false;
            if (m_breakNext)
            {
                suspend = true;
                cause = Cause::Step;
                m_breakNext = false;
            }
            else if (m_stepArmed)
            {
                const int depth = static_cast<int>(ctx->GetCallstackSize());
                const bool depthOk = (m_stepMode == StepMode::Into) || (depth <= m_stepBaseDepth);
                const bool moved = (line != m_stepFromLine) || (depth != m_stepFromDepth);
                if (depthOk && moved)
                {
                    suspend = true;
                    cause = Cause::Step;
                }
            }
            if (!suspend && IsBreakpoint(section, line))
            {
                suspend = true;
                cause = Cause::Breakpoint;
            }
            if (!suspend)
            {
                return;
            }
            m_pausedContext = ctx;
            m_cause = cause;
            m_stepArmed = false;
            (void)ctx->Suspend();
        }

    private:
        enum class StepMode
        {
            None,
            Into,
            Over
        };
        enum class Cause
        {
            Breakpoint,
            Step
        };

        struct Breakpoint
        {
            foundation::String file;
            foundation::i32 line = -1;
        };

        struct CapturedObject
        {
            foundation::u64 ref = 0;
            foundation::Variant value;
        };

        [[nodiscard]] bool IsBreakpoint(const char* section, int line) const
        {
            const foundation::StringView sectionView = ViewOfAscii(section);
            for (const Breakpoint& breakpoint : m_breakpoints)
            {
                if (breakpoint.line == line && breakpoint.file.AsView() == sectionView)
                {
                    return true;
                }
            }
            return false;
        }

        // Fill a ScriptVariable's display text + expandability from a captured Variant: a
        // reflected object/value with properties gets a non-zero objectRef for lazy expansion.
        void DescribeValue(ScriptVariable& variable, const foundation::Variant& value)
        {
            const foundation::TypeInfo* type = value.Type();
            const bool expandable = type != nullptr && foundation::PropertyCount(*type) > 0 &&
                                    PrimitiveDeclName(type) == nullptr;
            if (expandable)
            {
                variable.objectRef = StoreObject(value);
            }
            variable.value = DebugValueText(value);
        }

        [[nodiscard]] foundation::u64 StoreObject(const foundation::Variant& value)
        {
            const foundation::u64 ref = m_nextObjectRef++;
            m_objects.PushBack(CapturedObject{ref, value});
            return ref;
        }

        [[nodiscard]] foundation::Variant* FindObject(foundation::u64 ref)
        {
            for (CapturedObject& object : m_objects)
            {
                if (object.ref == ref)
                {
                    return &object.value;
                }
            }
            return nullptr;
        }

        // Re-Execute the held context. None = run to the next breakpoint/completion;
        // Into/Over arm a one-line step. Re-establishes the owning context's call scope so
        // resumed facade calls still resolve their per-context services.
        void Resume(StepMode mode)
        {
            if (m_pausedContext == nullptr)
            {
                return;
            }
            asIScriptContext* ctx = m_pausedContext;
            if (mode == StepMode::None)
            {
                m_stepArmed = false;
            }
            else
            {
                m_stepArmed = true;
                m_stepMode = mode;
                m_stepFromDepth = static_cast<int>(ctx->GetCallstackSize());
                m_stepBaseDepth = m_stepFromDepth;
                m_stepFromLine = ctx->GetLineNumber(0, nullptr, nullptr);
            }
            m_paused = false;
            m_pausedContext = nullptr; // re-set by the line callback if it suspends again
            m_objects.Clear();         // object refs are valid only within one break
            m_nextObjectRef = 1;
            FireState(ScriptDebuggerState::Running);

            int result;
            {
                ScriptCallScope scope(m_owner);
                result = ctx->Execute();
            }
            if (result == asEXECUTION_SUSPENDED && m_pausedContext == ctx)
            {
                m_paused = true;
                FireState(m_cause == Cause::Step ? ScriptDebuggerState::Stepped
                                                 : ScriptDebuggerState::Breakpoint);
                return; // still holding the context, paused again
            }
            // Ran to completion (or faulted): release the context back to the pool.
            ctx->ClearLineCallback();
            if (m_manager != nullptr && m_manager->Engine() != nullptr)
            {
                m_manager->Engine()->ReturnContext(ctx);
            }
            FireState(ScriptDebuggerState::Terminated);
        }

        void FireState(ScriptDebuggerState state)
        {
            if (m_listener != nullptr)
            {
                m_listener->OnDebuggerStateChanged(state);
            }
        }

        AngelScriptManager* m_manager = nullptr;
        IScriptDebuggerListener* m_listener = nullptr;
        IScriptContext* m_owner = nullptr; // call scope for a resumed context
        asIScriptContext* m_pausedContext =
            nullptr; // the held suspended context (owned while paused)
        foundation::Array<Breakpoint> m_breakpoints;
        foundation::Array<CapturedObject> m_objects; // lazily-expandable handles for this break
        foundation::u64 m_nextObjectRef = 1;         // 0 = a leaf scalar
        Cause m_cause = Cause::Breakpoint;
        StepMode m_stepMode = StepMode::None;
        int m_stepFromLine = -1;
        int m_stepFromDepth = 0;
        int m_stepBaseDepth = 0;
        bool m_paused = false;
        bool m_stepArmed = false;
        bool m_breakNext = false;
    };

    void DebuggerLineCallback(asIScriptContext* ctx, void* param)
    {
        static_cast<AngelScriptDebugger*>(param)->OnLine(ctx);
    }

    void ArmDebugger(AngelScriptDebugger& debugger, asIScriptContext* ctx, IScriptContext* owner)
    {
        debugger.ArmOn(ctx, owner);
    }

    bool AdoptDebuggerSuspension(AngelScriptDebugger& debugger, asIScriptContext* ctx)
    {
        return debugger.Adopt(ctx);
    }

    foundation::UniquePtr<IScriptDebugger> AngelScriptManager::CreateDebugger()
    {
        return foundation::MakeUnique<AngelScriptDebugger>(foundation::DefaultAllocator(), this);
    }

    foundation::RefPtr<IScriptContext> AngelScriptManager::CreateContext()
    {
        // Defensive finalize (the documented contract): a context may be created
        // without RegisterReflectedTypes having driven the two-phase emission.
        FinalizeTypes();
        return foundation::RefPtr<IScriptContext>(foundation::MakeRef<AngelScriptContext>(
            foundation::DefaultAllocator(), foundation::RefPtr<AngelScriptManager>(this), m_nextContextId++));
    }

    foundation::RefPtr<ScriptObject> AngelScriptContext::CreateInstance(foundation::StringView className,
                                                                  foundation::Span<foundation::Variant> args)
    {
        if (m_module == nullptr)
        {
            return nullptr;
        }
        const foundation::String name(className);
        asITypeInfo* type = m_module->GetTypeInfoByDecl(CStr(name));
        if (type == nullptr)
        {
            return nullptr;
        }
        asIScriptFunction* factory = nullptr;
        for (asUINT i = 0; i < type->GetFactoryCount(); ++i)
        {
            asIScriptFunction* candidate = type->GetFactoryByIndex(i);
            if (candidate->GetParamCount() != args.Size())
            {
                continue;
            }
            if (args.Size() == 1)
            {
                // Skip the implicit copy factory (one self-typed parameter) - a
                // Variant argument can never be a live script object.
                int paramTypeId = 0;
                (void)candidate->GetParam(0, &paramTypeId);
                const int baseId = paramTypeId & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST);
                if (baseId == type->GetTypeId())
                {
                    continue;
                }
            }
            factory = candidate;
            break;
        }
        if (factory == nullptr)
        {
            return nullptr;
        }

        asIScriptEngine* engine = m_manager->Engine();
        asIScriptContext* executor = engine->RequestContext();
        if (executor == nullptr || executor->Prepare(factory) < 0)
        {
            if (executor != nullptr)
            {
                engine->ReturnContext(executor);
            }
            return nullptr;
        }
        std::string stringTemps[kMaxArgs];
        BoxedVariant* boxTemps[kMaxArgs] = {};
        BindArgs(executor, factory, args, stringTemps, boxTemps);
        int result;
        {
            ScriptCallScope scope(this);
            result = executor->Execute();
        }
        for (BoxedVariant* box : boxTemps)
        {
            ReleaseBox(box);
        }
        asIScriptObject* instance = nullptr;
        if (result == asEXECUTION_FINISHED)
        {
            instance = static_cast<asIScriptObject*>(executor->GetReturnObject());
            if (instance != nullptr)
            {
                instance->AddRef();
            } // before the pool reuses the context
        }
        else if (result == asEXECUTION_EXCEPTION)
        {
            ReportException(executor);
        }
        engine->ReturnContext(executor);
        if (instance == nullptr)
        {
            return nullptr;
        }
        return foundation::RefPtr<ScriptObject>(foundation::MakeRef<AngelScriptObject>(
            foundation::DefaultAllocator(), foundation::RefPtr<AngelScriptContext>(this), instance));
    }

    foundation::RefPtr<IScriptManager> CreateScriptManager()
    {
        return foundation::RefPtr<IScriptManager>(
            foundation::MakeRef<AngelScriptManager>(foundation::DefaultAllocator()));
    }

    foundation::StringView AngelScriptCoroutineModulePrelude() noexcept
    {
        return ViewOfAscii(kCoroutinePreludeSection);
    }

    void* AngelScriptEngineHandle(IScriptManager& manager) noexcept
    {
        // Only this backend produces AngelScriptManager instances, and the editor cook
        // resolves the manager by language id "angelscript" before calling - so the
        // static_cast is safe. The engine already carries the reflected types the cook
        // registered, which is exactly what CScriptBuilder needs to build a behavior.
        return static_cast<AngelScriptManager&>(manager).Engine();
    }

    void RegisterAngelScriptBackend()
    {
        ScriptBackendDesc desc;
        desc.languageId = foundation::String(u8"angelscript");
        desc.displayName = foundation::String(u8"AngelScript");
        desc.fileExtensions.PushBack(foundation::String(u8"as"));
        desc.create = []() { return CreateScriptManager(); };
        ScriptBackendRegistry::Get().Register(foundation::Move(desc));
    }
}

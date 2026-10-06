// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.AngelScript - implementation (module IMPLEMENTATION unit).
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
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

#include <angelscript.h>
#include <scriptarray/scriptarray.h>
#include <scriptstdstring/scriptstdstring.h>

#include <new>
#include <string>

module foundation.script.angelscript;

import foundation.core;
import foundation.script;

namespace core = foundation::core;

namespace foundation::script::angelscript
{
    inline const char* CStr(const core::String& s) noexcept
    {
        return reinterpret_cast<const char*>(s.CStr());
    }

    inline void AppendAscii(core::String& s, const char* text)
    {
        s.Append(core::StringView(reinterpret_cast<const core::utf8char*>(text)));
    }

    inline void AppendUint(core::String& s, core::u32 n)
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

    // A compiled AngelScript bytecode unit (the Bytecode capability). CompileToBlob produces one
    // at COOK via asIScriptModule::SaveByteCode; LoadBlob feeds it to LoadByteCode in the PLAYER.
    // AngelScript bytecode is tied to the engine's TYPE REGISTRATION and library version, so the
    // runtime must register the same reflected surface (it does - the shared global registry) and
    // the cook fingerprint carries ANGELSCRIPT_VERSION. Opaque per IScriptBlob (no RTTI node) -
    // only this backend produces/consumes it, so LoadBlob downcasts by static_cast.
    class AngelScriptScriptBlob final : public IScriptBlob
    {
    public:
        [[nodiscard]] core::Array<core::byte>& Bytes() noexcept { return m_bytecode; }
        [[nodiscard]] const core::Array<core::byte>& Bytes() const noexcept { return m_bytecode; }

        void Serialize(core::ISerializer& ar) override
        {
            core::Serialize(ar, "bytecode", m_bytecode);
        }

    private:
        core::Array<core::byte> m_bytecode;
    };

    // asIBinaryStream over a byte buffer: WRITE appends (SaveByteCode), READ advances a cursor
    // (LoadByteCode). AngelScript's Read/Write return 0 on success, negative on error.
    class ByteBufferStream final : public asIBinaryStream
    {
    public:
        explicit ByteBufferStream(core::Array<core::byte>& buffer) noexcept : m_buffer(buffer) {}

        int Write(const void* ptr, asUINT size) override
        {
            if (size == 0)
            {
                return 0;
            }
            const core::usize begin = m_buffer.Size();
            m_buffer.Resize(begin + size);
            core::MemCopy(m_buffer.Data() + begin, ptr, size);
            return 0;
        }
        int Read(void* ptr, asUINT size) override
        {
            if (size == 0)
            {
                return 0;
            }
            if (m_read + size > m_buffer.Size())
            {
                return -1;
            }
            core::MemCopy(ptr, m_buffer.Data() + m_read, size);
            m_read += size;
            return 0;
        }

    private:
        core::Array<core::byte>& m_buffer;
        core::usize m_read = 0;
    };

    inline bool NameEq(const char* a, const char* b) noexcept
    {
        core::usize i = 0;
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

    inline core::StringView ViewOfAscii(const char* text) noexcept
    {
        return (text != nullptr) ? core::StringView(reinterpret_cast<const core::utf8char*>(text))
                                 : core::StringView{};
    }

    inline core::String StringFromStd(const std::string& s)
    {
        return core::String(
            core::StringView(reinterpret_cast<const core::utf8char*>(s.c_str()), s.size()));
    }

    inline std::string StdFromVariantString(const core::Variant& value)
    {
        if (const core::String* s = value.TryGet<core::String>())
        {
            return std::string(reinterpret_cast<const char*>(s->CStr()), s->Size());
        }
        return std::string();
    }

    // Numeric value of a Variant as double; `ok` false when it holds no number.
    inline double NumericOf(const core::Variant& value, bool& ok) noexcept
    {
        ok = true;
        if (const core::f64* d = value.TryGet<core::f64>())
        {
            return *d;
        }
        if (const core::f32* f = value.TryGet<core::f32>())
        {
            return static_cast<double>(*f);
        }
        if (const core::i32* i = value.TryGet<core::i32>())
        {
            return static_cast<double>(*i);
        }
        if (const core::i64* i = value.TryGet<core::i64>())
        {
            return static_cast<double>(*i);
        }
        if (const core::u32* u = value.TryGet<core::u32>())
        {
            return static_cast<double>(*u);
        }
        if (const core::u64* u = value.TryGet<core::u64>())
        {
            return static_cast<double>(*u);
        }
        if (const core::i16* i = value.TryGet<core::i16>())
        {
            return static_cast<double>(*i);
        }
        if (const core::u16* u = value.TryGet<core::u16>())
        {
            return static_cast<double>(*u);
        }
        if (const core::i8* i = value.TryGet<core::i8>())
        {
            return static_cast<double>(*i);
        }
        if (const core::u8* u = value.TryGet<core::u8>())
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
    // (default f64, the default marshalling currency).
    inline core::Variant CoerceNumber(double d, const core::TypeInfo* expected)
    {
        using namespace core;
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
    inline core::Variant CoerceInteger(const core::Variant& src, const core::TypeInfo* expected)
    {
        using namespace core;
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
    inline const char* PrimitiveDeclName(const core::TypeInfo* type) noexcept
    {
        using namespace core;
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
    inline core::String DebugValueText(const core::Variant& value)
    {
        if (value.IsEmpty())
        {
            return core::String(u8"null");
        }
        if (const core::String* s = value.TryGet<core::String>())
        {
            core::String out(u8"\"");
            out += *s;
            out += u8"\"";
            return out;
        }
        if (const bool* b = value.TryGet<bool>())
        {
            return core::String(*b ? u8"true" : u8"false");
        }
        bool ok = false;
        const double number = NumericOf(value, ok);
        if (ok)
        {
            return core::Format(u8"{}", number);
        }
        const core::TypeInfo* type = value.Type();
        return core::String(ViewOfAscii(type != nullptr ? type->name : "object"));
    }

    // Max arguments a reflected facade method / script call marshals (fixed-size temp arrays below).
    // 16, not 8: facade methods take flat scalar args and can exceed 8 (e.g. DebugDraw.line = 6
    // coords + 3 color = 9); a smaller cap silently truncates an over-cap call's trailing args.
    inline constexpr int kMaxArgs = 16;

    // Every reflected instance held by script: a refcounted box around a Variant.
    struct BoxedVariant
    {
        core::Variant value;
        core::i32 refCount = 1;
    };

    // Boxes cross the AS engine through captureless generic-call thunks (no owner
    // reachable at release) - a documented process-scope bound, like the reflection
    // thunk ABI.
    inline BoxedVariant* NewBox(core::Variant value)
    {
        BoxedVariant* box = core::DefaultAllocator().New<BoxedVariant>();
        box->value = core::Move(value);
        return box;
    }

    inline void ReleaseBox(BoxedVariant* box) noexcept
    {
        if (box != nullptr && --box->refCount == 0)
        {
            core::DefaultAllocator().Delete(box);
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
            // TODO: surface non-Object value elements (reached only by address); not surfaced here.
            ContainerCount,
            ContainerAt,
            ContainerAdd,
            ContainerRemoveAt,
            ContainerMove,
            // A plain nested-VALUE member (`emitter`, a curve): getter returns a BORROW handle over the
            // member address (edited in place; owner pinned + generation-guarded). `property` = member.
            NestedGet,
            // A reflected operator (MethodInfo::op) as the object's opAdd/opSub/opMul/opDiv/opNeg/
            // opEquals: the static invoked with `self` as its left operand. `method` = the static.
            Operator,
            // Its compound form (opAddAssign ...): the result written back into `self`.
            OperatorAssign
        };
        Kind kind;
        AngelScriptManager* manager;
        const core::TypeInfo* type;
        const core::ConstructorInfo* constructor;
        const core::PropertyInfo* property;
        const core::MethodInfo* method;
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
    void OperatorDispatch(asIScriptGeneric* gen); // a reflected operator (Binding::Kind::Operator*)
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
    core::Variant MakeAngelScriptDelegateVariant(asIScriptFunction* function);

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
            // Pin the PROCESS-GLOBAL thread manager for the process lifetime. AngelScript
            // refcounts it across engines (first engine creates, last release frees); a
            // multi-engine process (the cook builds one engine per script) can otherwise
            // interleave into a double-unprepare that asserts at teardown
            // (asCThreadManager::Unprepare 'threadManager' failed). One deliberate extra
            // prepare keeps the manager alive until process exit - the upstream idiom for
            // multi-engine hosts. Never unprepared: the OS reclaims at exit.
            static const int threadManagerPin = asPrepareMultithread();
            (void)threadManagerPin;
            m_engine = asCreateScriptEngine();
            m_engine->SetMessageCallback(asFUNCTION(&AngelScriptManager::OnMessage), this,
                                         asCALL_CDECL);
            RegisterStdString(m_engine);
            // NOTE: RegisterStdString already registers the number-concat operators
            // (string opAdd(int64/double/bool/...)), so `"Score: " + n` works. The formatInt/
            // formatFloat GLOBALS (RegisterStdStringUtils) are NOT vendored (their upstream
            // scriptstdstring_utils.cpp is absent); add them only when precision/padding is needed.
            m_stringTypeId = m_engine->GetTypeIdByDecl("string");
            // The native `array<T>` type. `defaultArray=true` also enables the `T[]` sugar. A facade
            // that returns an engine Array<T> is rendered as a CScriptArray on the way out
            // (SetGenericReturn / BuildScriptArray).
            RegisterScriptArray(m_engine, true);
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
                MemoryAllocator().Delete(binding);
            }
        }

        AngelScriptManager(const AngelScriptManager&) = delete;
        AngelScriptManager& operator=(const AngelScriptManager&) = delete;

        // ---- IScriptManager --------------------------------------------------
        void RegisterType(const core::TypeInfo& type) override
        {
            for (const core::TypeInfo* existing : m_types)
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
            // The overload contract: fail loudly if any type binds two methods to the same script
            // name at the same arity (see foundation.script ValidateScriptMethodNames).
            ValidateScriptMethodNames(
                core::Span<const core::TypeInfo* const>{m_types.Data(), m_types.Size()});
            // ...and fail loudly if two types bind to the same script-facing name (class name or
            // scriptName alias) - the alias mechanism's collision guard.
            ValidateScriptTypeNames(
                core::Span<const core::TypeInfo* const>{m_types.Data(), m_types.Size()});
            // Phase 1: DECLARE every collected type - after this, any declaration
            // string may reference any reflected type.
            for (const core::TypeInfo* type : m_types)
            {
                DeclareType(*type);
            }
            // ...plus any enum a member references (enums ride a member's type, not the registry).
            for (const core::TypeInfo* type : m_types)
            {
                DeclareReferencedEnums(*type);
            }
            // Phase 2: bind members (factories, properties, methods, statics).
            for (const core::TypeInfo* type : m_types)
            {
                BindType(*type);
            }
        }

        [[nodiscard]] core::RefPtr<IScriptContext> CreateContext() override;

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
                   ScriptCapabilities::Debugger | ScriptCapabilities::Bytecode;
        }

        // Compile source to an AngelScript bytecode blob (the cook side of the Bytecode seam):
        // build a throwaway module on the shared engine, SaveByteCode it (debug info kept so the
        // player reports source lines), and discard the module. The engine already carries the
        // reflected surface (FinalizeTypes), which the bytecode references.
        [[nodiscard]] core::Result<core::RefPtr<IScriptBlob>>
        CompileToBlob(core::StringView source, core::StringView chunkName) override
        {
            if (m_engine == nullptr)
            {
                return core::Err(core::ErrorCode::Internal);
            }
            asIScriptModule* module = m_engine->GetModule("__blobcompile", asGM_ALWAYS_CREATE);
            if (module == nullptr)
            {
                return core::Err(core::ErrorCode::Internal);
            }
            const core::String section(chunkName);
            (void)module->AddScriptSection(CStr(section),
                                           reinterpret_cast<const char*>(source.Data()),
                                           source.Size());
            (void)module->AddScriptSection("__coroutine_support", kCoroutinePreludeSection);
            BeginMessageCapture();
            const int built = module->Build();
            EndMessageCapture(nullptr, ScriptErrorKind::Compile); // cook validates + reports itself
            if (built < 0)
            {
                module->Discard();
                return core::Err(core::ErrorCode::InvalidArgument);
            }
            core::RefPtr<AngelScriptScriptBlob> blob =
                core::MakeRef<AngelScriptScriptBlob>(MemoryAllocator());
            ByteBufferStream stream(blob->Bytes());
            const int saved = module->SaveByteCode(&stream, /*stripDebugInfo=*/false);
            module->Discard();
            if (saved < 0)
            {
                return core::Err(core::ErrorCode::Internal);
            }
            return core::RefPtr<IScriptBlob>(blob.Get());
        }

        // Create an empty blob to deserialize a stored one into (see IScriptManager::CreateBlob).
        [[nodiscard]] core::RefPtr<IScriptBlob> CreateBlob() override
        {
            return core::RefPtr<IScriptBlob>(
                core::MakeRef<AngelScriptScriptBlob>(MemoryAllocator()).Get());
        }

        // A step debugger over this engine's contexts (suspension breakpoints + AS
        // introspection). One at a time; it registers itself as the active debugger on
        // construction (ExecuteCall consults it) and unregisters on destruction.
        [[nodiscard]] core::UniquePtr<IScriptDebugger> CreateDebugger() override;

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
        [[nodiscard]] core::Array<ScriptApiType> DescribeBoundApi() const override
        {
            core::Array<ScriptApiType> result;
            for (const RegisteredType& entry : m_registered)
            {
                const core::TypeInfo& type = *entry.type;
                const char* const scriptName = ScriptTypeName(type); // the script-facing name (alias)
                ScriptApiType api;
                api.scriptName = core::String(ViewOfAscii(scriptName));
                api.typeId = type.id;
                api.isNamespace = false;
                for (core::usize i = 0; i < core::PropertyCount(type); ++i)
                {
                    const core::PropertyInfo& property = core::PropertyAt(type, i);
                    if (!IsValidIdentifier(property.name) || core::IsNested(property))
                    {
                        continue; // nested structures are not scriptable leaf values
                    }
                    ScriptApiMember member;
                    member.name = core::String(ViewOfAscii(property.name));
                    member.readOnly = (static_cast<core::u32>(property.flags) &
                                       static_cast<core::u32>(core::PropertyFlags::ReadOnly)) != 0;
                    core::String signature(ViewOfAscii(scriptName));
                    AppendAscii(signature, ".");
                    AppendAscii(signature, property.name);
                    if (member.readOnly)
                    {
                        AppendAscii(signature, " (read only)");
                    }
                    member.signature = core::Move(signature);
                    member.kind = ScriptApiMemberKind::Property;
                    api.members.PushBack(core::Move(member));
                }
                for (core::usize i = 0; i < core::MethodCount(type); ++i)
                {
                    const core::MethodInfo& method = core::MethodAt(type, i);
                    if (!IsValidIdentifier(method.name))
                    {
                        continue;
                    }
                    core::String signature;
                    if (!BuildMemberSignature(signature, type, method))
                    {
                        continue;
                    } // not bound
                    ScriptApiMember member;
                    member.name = core::String(ViewOfAscii(ScriptMethodName(method)));
                    member.signature = core::Move(signature);
                    member.isStatic = method.isStatic;
                    member.kind = ScriptApiMemberKind::Method;
                    api.members.PushBack(core::Move(member));
                    // The operator it also binds as (RegisterOperator), in the script's spelling.
                    core::String operatorSignature;
                    if (BuildOperatorSignature(operatorSignature, type, method))
                    {
                        ScriptApiMember op;
                        op.name = core::String(OperatorSymbol(method.op));
                        op.signature = core::Move(operatorSignature);
                        op.kind = ScriptApiMemberKind::Operator;
                        api.members.PushBack(core::Move(op));
                    }
                }
                result.PushBack(core::Move(api));
            }
            return result;
        }

        // Invoke a script funcdef handle (an AngelScriptDelegate's stored function) from
        // native code with reflected args: runs on a pooled context, marshalling against the
        // funcdef's actual parameters, and returns its result. Handles a delegate-to-method
        // (bound object) as well as a plain function handle.
        [[nodiscard]] core::Result<core::Variant> ExecuteDelegate(asIScriptFunction* delegate,
                                                                  core::Span<core::Variant> args)
        {
            if (delegate == nullptr || m_engine == nullptr)
            {
                return core::Err(core::ErrorCode::Internal);
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
                return core::Err(core::ErrorCode::Internal);
            }

            asIScriptContext* executor = m_engine->RequestContext();
            if (executor == nullptr || executor->Prepare(func) < 0)
            {
                if (executor != nullptr)
                {
                    m_engine->ReturnContext(executor);
                }
                return core::Err(core::ErrorCode::Internal);
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

            core::Result<core::Variant> outcome = core::Err(core::ErrorCode::Internal);
            if (result == asEXECUTION_FINISHED)
            {
                const int returnTypeId = func->GetReturnTypeId();
                outcome = (returnTypeId == asTYPEID_VOID)
                              ? core::Result<core::Variant>(core::Variant{})
                              : core::Result<core::Variant>(VariantFromTypedAddress(
                                    returnTypeId, executor->GetAddressOfReturnValue()));
            }
            m_engine->ReturnContext(executor);
            return outcome;
        }

        // The AngelScript behavior module: just the concatenated class sources. AngelScript
        // needs NO import prelude - reflected types and the coroutine surface (startCoroutine/
        // wait) are registered engine-globally, so every reflected type is already visible.
        // This keeps the language framing in the backend, off the neutral libs.
        [[nodiscard]] core::String
        AssembleBehaviorModuleSource(core::Span<const core::StringView> classSources) const override
        {
            core::String moduleSource;
            for (const core::StringView& source : classSources)
            {
                moduleSource += source;
                moduleSource += u8"\n";
            }
            return moduleSource;
        }

        // ---- the from-scratch coroutine scheduler (one asIScriptContext each) ----

        /// One live coroutine: its own execution context, the seconds still to wait, the owning
        /// behavior instance (AddRef'd) so CancelCoroutinesFor can drop by owner, and the HOME
        /// script context it was started from - so a resume from AdvanceCoroutines (outside any
        /// script call) re-establishes CurrentScriptContext and the coroutine's facade calls
        /// (scene.spawn, entity.send, ...) resolve the right per-context services.
        struct Coroutine
        {
            asIScriptContext* ctx = nullptr;
            core::f64 wait = 0.0;
            asIScriptObject* owner = nullptr;
            IScriptContext* home = nullptr;
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

            // Record BEFORE Execute so the `wait` host call can find it (by active ctx). Capture
            // the home context (this initial run is already inside the caller's ScriptCallScope;
            // a resume from AdvanceCoroutines re-pushes it - see there).
            m_coroutines.PushBack(Coroutine{co, 0.0, owner, CurrentScriptContext()});
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
                    co.wait = static_cast<core::f64>(seconds);
                    break;
                }
            }
            (void)active->Suspend();
        }

        void AdvanceCoroutines(core::f64 deltaSeconds) override
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
                const int index = FindCoroutine(ctx);
                if (index < 0)
                {
                    continue;
                } // a nested cancel removed it
                // Re-establish the home script context: the resume runs outside any script call
                // (from the run-host tick), so the coroutine's facade calls resolve per-context
                // services (scene.spawn / entity.send) instead of no-opping on a null context.
                ScriptCallScope scope(m_coroutines[static_cast<core::usize>(index)].home);
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
        [[nodiscard]] const core::TypeInfo* TypeInfoForTypeId(int typeId) const noexcept
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

        // A script array argument (`const array<Elem> &in`) -> an Array<Variant> of its elements,
        // each narrowed to the container's element type (the reflection call converts the list
        // into the parameter's Array<E>). Numbers, bools, strings and our reflected handles.
        [[nodiscard]] core::Variant ListFromArg(asIScriptGeneric* gen, asUINT index,
                                                const core::TypeInfo& containerType) const
        {
            const CScriptArray* arr = static_cast<const CScriptArray*>(gen->GetArgAddress(index));
            core::Array<core::Variant> list;
            if (arr == nullptr)
            {
                return core::Variant::From<core::Array<core::Variant>>(core::Move(list));
            }
            const core::TypeInfo* elementType = containerType.container->elementType;
            const int subId = arr->GetElementTypeId();
            list.Reserve(arr->GetSize());
            for (asUINT i = 0; i < arr->GetSize(); ++i)
            {
                list.PushBack(ValueFromAddress(subId, arr->At(i), elementType));
            }
            return core::Variant::From<core::Array<core::Variant>>(core::Move(list));
        }

        // A value at `addr` of AngelScript type `typeId` (an array element) -> a Variant narrowed to
        // `expected`.
        [[nodiscard]] core::Variant ValueFromAddress(int typeId, const void* addr,
                                                     const core::TypeInfo* expected) const
        {
            if (addr == nullptr)
            {
                return core::Variant{};
            }
            switch (typeId)
            {
            case asTYPEID_BOOL:
            {
                const bool b = *static_cast<const bool*>(addr);
                return (expected == nullptr || expected == &core::TypeOf<bool>())
                           ? core::Variant::From<bool>(b)
                           : CoerceNumber(b ? 1.0 : 0.0, expected);
            }
            case asTYPEID_INT8:
                return CoerceNumber(*static_cast<const core::i8*>(addr), expected);
            case asTYPEID_UINT8:
                return CoerceNumber(*static_cast<const core::u8*>(addr), expected);
            case asTYPEID_INT16:
                return CoerceNumber(*static_cast<const core::i16*>(addr), expected);
            case asTYPEID_UINT16:
                return CoerceNumber(*static_cast<const core::u16*>(addr), expected);
            case asTYPEID_INT32:
                return CoerceNumber(*static_cast<const core::i32*>(addr), expected);
            case asTYPEID_UINT32:
                return CoerceNumber(*static_cast<const core::u32*>(addr), expected);
            case asTYPEID_INT64:
                return CoerceInteger(core::Variant::From<core::i64>(*static_cast<const core::i64*>(addr)),
                                     expected);
            case asTYPEID_UINT64:
                return CoerceInteger(core::Variant::From<core::u64>(*static_cast<const core::u64*>(addr)),
                                     expected);
            case asTYPEID_FLOAT:
                return CoerceNumber(*static_cast<const float*>(addr), expected);
            case asTYPEID_DOUBLE:
                return CoerceNumber(*static_cast<const double*>(addr), expected);
            default:
                break;
            }
            if (typeId == m_stringTypeId)
            {
                return core::Variant::From<core::String>(StringFromStd(*static_cast<const std::string*>(addr)));
            }
            if ((typeId & asTYPEID_OBJHANDLE) != 0 && TypeInfoForTypeId(typeId) != nullptr)
            {
                const BoxedVariant* box = *static_cast<const BoxedVariant* const*>(addr);
                return (box != nullptr) ? box->value : core::Variant{};
            }
            return core::Variant{};
        }

        // Script argument -> engine Variant (generic calling convention).
        [[nodiscard]] core::Variant ValueFromArg(asIScriptGeneric* gen, asUINT index,
                                                 const core::TypeInfo* expected) const
        {
            const int typeId = gen->GetArgTypeId(index);

            // A list for an Array<E> parameter (declared `const array<Elem> &in`).
            if (expected != nullptr && core::IsContainer(*expected) && expected->container->elementType != nullptr)
            {
                return ListFromArg(gen, index, *expected);
            }

            // A `?&in` variable-type arg (the emit/send Variant-payload sink, and any `&in`
            // primitive) arrives BY REFERENCE: the arg slot holds a POINTER to the caller's
            // value, so the by-value accessors below (GetArgDouble/GetArgDWord/...) would read
            // the pointer bits as the value. Read a primitive through its address instead. Object
            // handles + strings keep the by-value paths below (GetArgObject/GetArgAddress already
            // return the referenced value for a reference arg); funcdef handles are handled there.
            if (ArgIsInReference(gen, index))
            {
                // GetArgAddress returns the VALUE pointer for a reference arg (like the string
                // case below), unlike GetAddressOfArg which returns the slot (a T** here).
                if (const void* addr = gen->GetArgAddress(index))
                {
                    switch (typeId)
                    {
                    case asTYPEID_BOOL:
                    {
                        const bool b = *static_cast<const bool*>(addr);
                        return (expected == nullptr || expected == &core::TypeOf<bool>())
                                   ? core::Variant::From<bool>(b)
                                   : CoerceNumber(b ? 1.0 : 0.0, expected);
                    }
                    case asTYPEID_INT8:
                        return CoerceNumber(*static_cast<const core::i8*>(addr), expected);
                    case asTYPEID_UINT8:
                        return CoerceNumber(*static_cast<const core::u8*>(addr), expected);
                    case asTYPEID_INT16:
                        return CoerceNumber(*static_cast<const core::i16*>(addr), expected);
                    case asTYPEID_UINT16:
                        return CoerceNumber(*static_cast<const core::u16*>(addr), expected);
                    case asTYPEID_INT32:
                        return CoerceNumber(*static_cast<const core::i32*>(addr), expected);
                    case asTYPEID_UINT32:
                        return CoerceNumber(*static_cast<const core::u32*>(addr), expected);
                    case asTYPEID_INT64:
                        return CoerceInteger(
                            core::Variant::From<core::i64>(*static_cast<const core::i64*>(addr)),
                            expected);
                    case asTYPEID_UINT64:
                        return CoerceInteger(
                            core::Variant::From<core::u64>(*static_cast<const core::u64*>(addr)),
                            expected);
                    case asTYPEID_FLOAT:
                        return CoerceNumber(*static_cast<const float*>(addr), expected);
                    case asTYPEID_DOUBLE:
                        return CoerceNumber(*static_cast<const double*>(addr), expected);
                    default:
                        break; // string / enum / handle: fall through to the by-value paths
                    }
                }
            }

            switch (typeId)
            {
            case asTYPEID_BOOL:
            {
                const bool b = gen->GetArgByte(index) != 0;
                return (expected == nullptr || expected == &core::TypeOf<bool>())
                           ? core::Variant::From<bool>(b)
                           : CoerceNumber(b ? 1.0 : 0.0, expected);
            }
            case asTYPEID_INT8:
                return CoerceNumber(static_cast<core::i8>(gen->GetArgByte(index)), expected);
            case asTYPEID_UINT8:
                return CoerceNumber(gen->GetArgByte(index), expected);
            case asTYPEID_INT16:
                return CoerceNumber(static_cast<core::i16>(gen->GetArgWord(index)), expected);
            case asTYPEID_UINT16:
                return CoerceNumber(gen->GetArgWord(index), expected);
            case asTYPEID_INT32:
                return CoerceNumber(static_cast<core::i32>(gen->GetArgDWord(index)), expected);
            case asTYPEID_UINT32:
                return CoerceNumber(gen->GetArgDWord(index), expected);
            // 64-bit integers carry their exact type across (never via double) so values
            // above 2^53 survive - the reason CoerceInteger exists.
            case asTYPEID_INT64:
                return CoerceInteger(
                    core::Variant::From<core::i64>(static_cast<core::i64>(gen->GetArgQWord(index))),
                    expected);
            case asTYPEID_UINT64:
                return CoerceInteger(core::Variant::From<core::u64>(gen->GetArgQWord(index)),
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
                return (s != nullptr) ? core::Variant::From<core::String>(StringFromStd(*s))
                                      : core::Variant{};
            }
            // A native enum argument: read its int32 value and carry it as an i64 (the property setter
            // / enum-arg path casts it to the enum - enums cross as their underlying int).
            if (const core::TypeInfo* enumType = TypeInfoForTypeId(typeId);
                enumType != nullptr && enumType->enumeratorCount > 0)
            {
                return core::Variant::From<core::i64>(
                    static_cast<core::i64>(gen->GetArgDWord(index)));
            }
            if (TypeInfoForTypeId(typeId) != nullptr && !IsFuncdefTypeId(typeId))
            {
                // A reflected value (e.g. a component `Gadget@` from `.of`) boxed as our value type.
                // Passed to a `?&in` sink (scene.events.emit / entity.send), AngelScript dereferences
                // the handle and delivers the OBJECT by reference - WITHOUT the OBJHANDLE bit - so the
                // arg-slot holds a pointer to the box; a by-value handle is the object direct.
                const BoxedVariant* box = nullptr;
                if (ArgIsInReference(gen, index))
                {
                    void* addr = gen->GetAddressOfArg(index);
                    box = (addr != nullptr) ? *static_cast<const BoxedVariant* const*>(addr) : nullptr;
                }
                else
                {
                    box = static_cast<const BoxedVariant*>(gen->GetArgObject(index));
                }
                return (box != nullptr) ? box->value : core::Variant{};
            }
            // A funcdef handle (a delegate parameter): wrap the function into a script delegate.
            // A generic IScriptDelegate param is spelled `?&in` (by reference). AngelScript does
            // NOT deliver that reference at a single fixed indirection: a plain function reference
            // (`ScriptDelegate(fn)`) is dereferenced to the OBJECT, so the arg slot holds the
            // asIScriptFunction* directly (GETOBJREF); a METHOD delegate (`Action(obj.method)`) is
            // an explicit handle, so the slot holds the ADDRESS of the temporary handle variable
            // (GETREF) - one level deeper. Both carry the OBJHANDLE bit, so the type id cannot tell
            // them apart. ResolveDelegateArgFunction peels the right number of levels safely (a
            // by-value funcdef handle, e.g. a coroutine's ScriptCoroutine@, arrives as the object
            // directly through GetArgObject).
            if ((typeId & asTYPEID_OBJHANDLE) != 0 && IsFuncdefTypeId(typeId))
            {
                asIScriptFunction* fn = nullptr;
                if (ArgIsInReference(gen, index))
                {
                    void* addr = gen->GetAddressOfArg(index);
                    asIScriptFunction* raw =
                        (addr != nullptr) ? *static_cast<asIScriptFunction**>(addr) : nullptr;
                    fn = ResolveDelegateArgFunction(gen, raw);
                }
                else
                {
                    fn = static_cast<asIScriptFunction*>(gen->GetArgObject(index));
                }
                return MakeAngelScriptDelegateVariant(fn);
            }
            return core::Variant{};
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

        // Resolve the real asIScriptFunction behind a funcdef handle read from a `?&in` arg slot.
        // AngelScript hands that slot at one of two indirection levels (see ValueFromArg): the
        // function object itself (object-ref form) or the address of a temporary handle variable
        // that holds it (handle-ref form). They are indistinguishable by type id, so tell them
        // apart by the vtable: every asIScriptFunction AngelScript produces is the same concrete
        // asCScriptFunction, so a real function shares the vtable of the currently executing one.
        // Probe against that known-good vtable rather than dereferencing through a bad one; only
        // the pointer AngelScript actually wrote is ever read, so no wild pointer is chased.
        [[nodiscard]] static asIScriptFunction*
        ResolveDelegateArgFunction(asIScriptGeneric* gen, asIScriptFunction* candidate) noexcept
        {
            if (candidate == nullptr)
            {
                return nullptr;
            }
            asIScriptFunction* known = (gen != nullptr) ? gen->GetFunction() : nullptr;
            if (known == nullptr)
            {
                return candidate; // nothing to compare against - assume the direct form
            }
            const void* const functionVTable = *reinterpret_cast<void* const*>(known);
            // Object-ref form: the slot already holds a valid function object.
            if (*reinterpret_cast<void* const*>(candidate) == functionVTable)
            {
                return candidate;
            }
            // Handle-ref form: the slot is the address of the handle variable; the function is one
            // level deeper. Accept it only if it, too, is a real function (else the handle was null
            // or unrecognized - yield nothing so the caller produces an empty delegate, no crash).
            asIScriptFunction* deeper = *reinterpret_cast<asIScriptFunction* const*>(candidate);
            if (deeper != nullptr && *reinterpret_cast<void* const*>(deeper) == functionVTable)
            {
                return deeper;
            }
            return nullptr;
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
        void SetGenericReturn(asIScriptGeneric* gen, const core::Variant& value) const
        {
            const int typeId = gen->GetReturnTypeId();
            if (typeId == asTYPEID_VOID)
            {
                return;
            }
            // A native enum return (a getter for an enum property): AngelScript enums are int32-backed,
            // so return the enum's underlying value as a DWord under its enum typeId.
            if (const core::TypeInfo* enumType = TypeInfoForTypeId(typeId);
                enumType != nullptr && enumType->enumeratorCount > 0)
            {
                gen->SetReturnDWord(static_cast<asDWORD>(static_cast<core::i32>(value.AsEnumInt())));
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
                gen->SetReturnByte(static_cast<asBYTE>(static_cast<core::i64>(number)));
                return;
            case asTYPEID_INT16:
            case asTYPEID_UINT16:
                gen->SetReturnWord(static_cast<asWORD>(static_cast<core::i64>(number)));
                return;
            case asTYPEID_INT32:
            case asTYPEID_UINT32:
                gen->SetReturnDWord(static_cast<asDWORD>(static_cast<core::i64>(number)));
                return;
            case asTYPEID_INT64:
            case asTYPEID_UINT64:
            {
                // Prefer the Variant's exact 64-bit value (a facade returning i64/u64); only a
                // float source falls back through `number`, which is the correct currency there.
                if (const core::i64* iv = value.TryGet<core::i64>())
                {
                    gen->SetReturnQWord(static_cast<asQWORD>(*iv));
                    return;
                }
                if (const core::u64* uv = value.TryGet<core::u64>())
                {
                    gen->SetReturnQWord(static_cast<asQWORD>(*uv));
                    return;
                }
                gen->SetReturnQWord(static_cast<asQWORD>(static_cast<core::i64>(number)));
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
                // A facade returning an engine Array<T> arrives as a container Variant: render it as a
                // native CScriptArray (declared `array<Elem>@`), not a boxed handle.
                if (const core::TypeInfo* vt = value.Type();
                    !value.IsEmpty() && vt != nullptr && core::IsContainer(*vt))
                {
                    asITypeInfo* arrType = m_engine->GetTypeInfoById(
                        typeId & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST));
                    *static_cast<void**>(gen->GetAddressOfReturnLocation()) =
                        BuildScriptArray(arrType, *vt, value);
                    return;
                }
                BoxedVariant* box = (!value.IsEmpty() && TypeInfoForTypeId(typeId) != nullptr)
                                        ? NewBox(value)
                                        : nullptr;
                *static_cast<void**>(gen->GetAddressOfReturnLocation()) = box;
            }
        }

        // Engine container Variant (Array<T>) -> a native CScriptArray of `arrType` (the declared
        // `array<Elem>` return). Each element is walked via reflection getAt: an object/handle element
        // is a refcounted box (SetValue AddRefs, the local ref is then dropped so the array owns the
        // only one); a numeric element is copied width-matched.
        [[nodiscard]] CScriptArray* BuildScriptArray(asITypeInfo* arrType,
                                                     const core::TypeInfo& containerType,
                                                     const core::Variant& value) const
        {
            if (arrType == nullptr || containerType.container == nullptr)
            {
                return nullptr;
            }
            const core::ContainerInfo& ci = *containerType.container;
            // Nested containers (Array<Array<T>>) are OUTSIDE the array-return contract: the
            // OBJHANDLE branch below would box the inner array as a BoxedVariant and SetValue
            // would AddRef it as the (different) declared subtype - type-confused refcounting.
            // Refuse loudly instead of corrupting.
            if (ci.elementType != nullptr && ci.elementType->container != nullptr)
            {
                LOG_WARNING(u8"Script",
                            u8"nested container return (array of arrays) is not marshaled");
                return nullptr;
            }
            core::Variant holder = value; // ToInstance needs a mutable lvalue; the copy is cheap
            const core::Instance inst = core::ToInstance(holder);
            if (inst.Pointer() == nullptr)
            {
                return CScriptArray::Create(arrType, 0u);
            }
            const core::usize n = ci.size(inst);
            CScriptArray* arr = CScriptArray::Create(arrType, static_cast<asUINT>(n));
            if (arr == nullptr)
            {
                return nullptr;
            }
            const int subId = arrType->GetSubTypeId();
            for (core::usize i = 0; i < n; ++i)
            {
                core::Variant elem = ci.getAt(inst, i);
                if ((subId & asTYPEID_OBJHANDLE) != 0)
                {
                    BoxedVariant* box = elem.IsEmpty() ? nullptr : NewBox(core::Move(elem));
                    arr->SetValue(static_cast<asUINT>(i), &box); // AddRefs (skips a null handle)
                    ReleaseBox(box);                             // drop our ref; array owns it now
                }
                else if (subId == m_stringTypeId)
                {
                    // A value-object `string` element: SetValue assign-copies from a source std::string.
                    std::string tmp = StdFromVariantString(elem);
                    arr->SetValue(static_cast<asUINT>(i), &tmp);
                }
                else
                {
                    WriteScalarElement(*arr, static_cast<asUINT>(i), subId, elem);
                }
            }
            return arr;
        }

        // A numeric / bool / enum array element: hand CScriptArray::SetValue a width-matched temporary
        // (it byte-copies per the subtype id). Enum subtype ids sort above asTYPEID_DOUBLE and are
        // int32-backed, matching the 32-bit case.
        void WriteScalarElement(CScriptArray& arr, asUINT index, int subId,
                                const core::Variant& elem) const
        {
            bool ok = false;
            const double num = NumericOf(elem, ok);
            if (!ok)
            {
                // A non-numeric element in a numeric array slot: 0 is written (below) so the
                // array stays well-formed, but say so - silence here is the no-op bug class.
                LOG_WARNING(u8"Script",
                            u8"non-numeric element written as 0 into a numeric script array");
            }
            switch (subId)
            {
            case asTYPEID_BOOL:
            case asTYPEID_INT8:
            case asTYPEID_UINT8:
            {
                asBYTE b = static_cast<asBYTE>(static_cast<core::i64>(num));
                arr.SetValue(index, &b);
                return;
            }
            case asTYPEID_INT16:
            case asTYPEID_UINT16:
            {
                asWORD w = static_cast<asWORD>(static_cast<core::i64>(num));
                arr.SetValue(index, &w);
                return;
            }
            case asTYPEID_INT64:
            case asTYPEID_UINT64:
            {
                asQWORD q = 0;
                if (const core::i64* iv = elem.TryGet<core::i64>())
                {
                    q = static_cast<asQWORD>(*iv);
                }
                else if (const core::u64* uv = elem.TryGet<core::u64>())
                {
                    q = static_cast<asQWORD>(*uv);
                }
                else
                {
                    q = static_cast<asQWORD>(static_cast<core::i64>(num));
                }
                arr.SetValue(index, &q);
                return;
            }
            case asTYPEID_FLOAT:
            {
                float f = static_cast<float>(num);
                arr.SetValue(index, &f);
                return;
            }
            case asTYPEID_DOUBLE:
            {
                double d = num;
                arr.SetValue(index, &d);
                return;
            }
            default: // INT32 / UINT32 and enum subtypes (id > asTYPEID_DOUBLE): all 32-bit
            {
                asDWORD d = static_cast<asDWORD>(static_cast<core::i64>(num));
                arr.SetValue(index, &d);
                return;
            }
            }
        }

        // Typed script storage (module global / finished call's return register)
        // -> engine Variant. Numbers surface uniformly as f64, strings as String,
        // handles to OUR types as a copy of the boxed Variant.
        [[nodiscard]] core::Variant VariantFromTypedAddress(int typeId, void* address) const
        {
            if (address == nullptr)
            {
                return core::Variant{};
            }
            switch (typeId)
            {
            case asTYPEID_BOOL:
                return core::Variant::From<bool>(*static_cast<bool*>(address));
            case asTYPEID_INT8:
                return core::Variant::From<core::f64>(*static_cast<core::i8*>(address));
            case asTYPEID_UINT8:
                return core::Variant::From<core::f64>(*static_cast<core::u8*>(address));
            case asTYPEID_INT16:
                return core::Variant::From<core::f64>(*static_cast<core::i16*>(address));
            case asTYPEID_UINT16:
                return core::Variant::From<core::f64>(*static_cast<core::u16*>(address));
            case asTYPEID_INT32:
                return core::Variant::From<core::f64>(*static_cast<core::i32*>(address));
            case asTYPEID_UINT32:
                return core::Variant::From<core::f64>(*static_cast<core::u32*>(address));
            case asTYPEID_INT64:
                return core::Variant::From<core::f64>(
                    static_cast<core::f64>(*static_cast<core::i64*>(address)));
            case asTYPEID_UINT64:
                return core::Variant::From<core::f64>(
                    static_cast<core::f64>(*static_cast<core::u64*>(address)));
            case asTYPEID_FLOAT:
                return core::Variant::From<core::f64>(*static_cast<float*>(address));
            case asTYPEID_DOUBLE:
                return core::Variant::From<core::f64>(*static_cast<double*>(address));
            default:
                break;
            }
            if (typeId == m_stringTypeId)
            {
                return core::Variant::From<core::String>(
                    StringFromStd(*static_cast<const std::string*>(address)));
            }
            if ((typeId & asTYPEID_OBJHANDLE) != 0 && TypeInfoForTypeId(typeId) != nullptr)
            {
                const BoxedVariant* box = *static_cast<const BoxedVariant* const*>(address);
                return (box != nullptr) ? box->value : core::Variant{};
            }
            // A plain reflected-type member or global (`Guid m`, OBJHANDLE clear): AngelScript
            // hands back the address of the object itself, the box (a reflected type is an
            // asOBJ_REF box), not of a handle to it. It read as empty, so a playtest probe of a
            // script's Guid field answered null.
            if (TypeInfoForTypeId(typeId) != nullptr)
            {
                return static_cast<const BoxedVariant*>(address)->value;
            }
            return core::Variant{};
        }

        // Engine Variant -> typed script storage (SetGlobal). False when the
        // slot's type cannot take the value.
        bool WriteTypedAddress(int typeId, void* address, const core::Variant& value) const
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
                    *static_cast<core::i8*>(address) = static_cast<core::i8>(number);
                }
                return ok;
            case asTYPEID_UINT8:
                if (ok)
                {
                    *static_cast<core::u8*>(address) = static_cast<core::u8>(number);
                }
                return ok;
            case asTYPEID_INT16:
                if (ok)
                {
                    *static_cast<core::i16*>(address) = static_cast<core::i16>(number);
                }
                return ok;
            case asTYPEID_UINT16:
                if (ok)
                {
                    *static_cast<core::u16*>(address) = static_cast<core::u16>(number);
                }
                return ok;
            case asTYPEID_INT32:
                if (ok)
                {
                    *static_cast<core::i32*>(address) = static_cast<core::i32>(number);
                }
                return ok;
            case asTYPEID_UINT32:
                if (ok)
                {
                    *static_cast<core::u32*>(address) = static_cast<core::u32>(number);
                }
                return ok;
            case asTYPEID_INT64:
                if (ok)
                {
                    *static_cast<core::i64*>(address) = static_cast<core::i64>(number);
                }
                return ok;
            case asTYPEID_UINT64:
                if (ok)
                {
                    *static_cast<core::u64*>(address) = static_cast<core::u64>(number);
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
                if (value.TryGet<core::String>() == nullptr)
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
            // and the caller guides the author to use a handle (see SetMemberField).
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
                    const ScriptError error{kind, core::StringView(message.section), message.row,
                                            core::StringView(message.text)};
                    handler->OnError(error);
                }
                else
                {
                    core::ConsoleWriteError(core::StringView(message.text));
                }
            }
            m_capturedMessages.Clear();
        }

        [[nodiscard]] Binding* MakeBinding(Binding binding)
        {
            Binding* stored = MemoryAllocator().New<Binding>(binding);
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
                          core::Span<core::Variant> args, std::string* stringTemps,
                          BoxedVariant** boxTemps)
        {
            const core::usize limit = function->GetParamCount();
            if (args.Size() > static_cast<core::usize>(kMaxArgs) &&
                limit > static_cast<core::usize>(kMaxArgs))
            {
                LOG_WARNING(u8"Script", u8"script call '{}' has {} args; only {} are marshaled",
                            reinterpret_cast<const char8_t*>(function->GetName()), args.Size(),
                            kMaxArgs);
            }
            for (core::usize i = 0;
                 i < args.Size() && i < limit && i < static_cast<core::usize>(kMaxArgs); ++i)
            {
                const asUINT arg = static_cast<asUINT>(i);
                int typeId = 0;
                (void)function->GetParam(arg, &typeId);
                const core::Variant& value = args[i];
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
                                               static_cast<asBYTE>(static_cast<core::i64>(number)));
                    continue;
                case asTYPEID_INT16:
                case asTYPEID_UINT16:
                    (void)executor->SetArgWord(arg,
                                               static_cast<asWORD>(static_cast<core::i64>(number)));
                    continue;
                case asTYPEID_INT32:
                case asTYPEID_UINT32:
                    (void)executor->SetArgDWord(
                        arg, static_cast<asDWORD>(static_cast<core::i64>(number)));
                    continue;
                case asTYPEID_INT64:
                case asTYPEID_UINT64:
                    (void)executor->SetArgQWord(
                        arg, static_cast<asQWORD>(static_cast<core::i64>(number)));
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
        // An operator as a script writes it: `Float3 + Float3 -> Float3 (and +=)`, `-Float3 ->
        // Float3`. False for a method that binds as no operator (RegisterOperator's rule).
        [[nodiscard]] bool BuildOperatorSignature(core::String& out, const core::TypeInfo& type,
                                                  const core::MethodInfo& method) const
        {
            const bool unary = method.op == core::MethodOperator::Negate;
            if (method.op == core::MethodOperator::None || !method.isStatic ||
                method.paramCount != (unary ? 1u : 2u) || method.params[0].type == nullptr ||
                method.params[0].type() != &type)
            {
                return false;
            }
            const core::TypeInfo* returnType =
                (method.returnType != nullptr) ? method.returnType() : nullptr;
            const char* typeName = ScriptTypeName(type);
            const core::StringView symbol = OperatorSymbol(method.op);
            if (unary)
            {
                AppendAscii(out, "-");
                AppendAscii(out, typeName);
            }
            else
            {
                AppendAscii(out, typeName);
                AppendAscii(out, " ");
                out.Append(symbol);
                AppendAscii(out, " ");
                const core::TypeInfo* right =
                    method.params[1].type != nullptr ? method.params[1].type() : nullptr;
                if (right == nullptr || !AppendDeclType(out, right, /*isParam*/ false))
                {
                    return false;
                }
            }
            AppendAscii(out, " -> ");
            if (returnType == nullptr || !AppendDeclType(out, returnType, /*isParam*/ false))
            {
                return false;
            }
            if (!unary && method.op != core::MethodOperator::Equals && returnType == &type)
            {
                AppendAscii(out, " (and ");
                out.Append(symbol);
                AppendAscii(out, "=)");
            }
            return true;
        }

        [[nodiscard]] bool BuildMemberSignature(core::String& out, const core::TypeInfo& type,
                                                const core::MethodInfo& method) const
        {
            const core::TypeInfo* returnType =
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
                AppendAscii(out, ScriptTypeName(type)); // the alias, so static-call signatures match
                AppendAscii(out, "::");
            }
            AppendAscii(out, ScriptMethodName(method)); // overload identity, not the C++ name
            AppendAscii(out, "(");
            if (!AppendParams(out, method.params, method.paramCount, /*includeNames*/ true))
            {
                return false;
            }
            AppendAscii(out, ")");
            return true;
        }

        [[nodiscard]] int FindCoroutine(asIScriptContext* ctx) const
        {
            for (core::usize i = 0; i < m_coroutines.Size(); ++i)
            {
                if (m_coroutines[i].ctx == ctx)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        // Release a coroutine's context (unwinding a suspended call stack) and its owner ref.
        void DropCoroutineAt(core::usize index)
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
                DropCoroutineAt(static_cast<core::usize>(index));
            }
        }

        static constexpr core::f64 kDueEpsilon = 1e-4;

        struct RegisteredType
        {
            const core::TypeInfo* type;
            int typeId;
        };

        struct CapturedMessage
        {
            core::String section;
            core::i32 row;
            core::String text;
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
                captured.section = core::String(ViewOfAscii(message->section));
                captured.row = static_cast<core::i32>(message->row);
                captured.text = core::String(ViewOfAscii(message->message));
                self->m_capturedMessages.PushBack(core::Move(captured));
                return;
            }
            core::ConsoleWriteError(ViewOfAscii(message->message));
        }

        [[nodiscard]] bool IsDeclared(const core::TypeInfo* type) const noexcept
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
        bool AppendDeclType(core::String& out, const core::TypeInfo* type, bool isParam) const
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
            if (type == &core::TypeOf<core::Variant>())
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
                if (isParam && type == &core::TypeOf<core::String>())
                {
                    AppendAscii(out, "const string &in");
                    return true;
                }
                AppendAscii(out, primitive);
                return true;
            }
            // A reflected container (Array<T>): the native array. The element reuses its own spelling
            // (`Entity@`, `int`, `string`, ...). A return is `array<Elem>@` (a facade returning a
            // set); a parameter is `const array<Elem> &in` (a script handing a facade a list, read
            // into an Array<Variant> by ValueFromArg).
            if (core::IsContainer(*type))
            {
                const core::TypeInfo* elem = type->container->elementType;
                core::String elemDecl;
                if (elem == nullptr || !AppendDeclType(elemDecl, elem, /*isParam*/ false))
                {
                    return false;
                }
                AppendAscii(out, isParam ? "const array<" : "array<");
                out.Append(elemDecl);
                AppendAscii(out, isParam ? "> &in" : ">@");
                return true;
            }
            // A native AngelScript enum: an int-backed VALUE type, spelled by name with no handle.
            if (type->enumeratorCount > 0)
            {
                AppendAscii(out, ScriptTypeName(*type)); // the alias, so references match the declaration
                return true;
            }
            if (!IsDeclared(type))
            {
                return false;
            }
            AppendAscii(out, ScriptTypeName(*type)); // the alias (Type@ params/returns/members)
            AppendAscii(out, "@");
            return true;
        }

        // Phase 1: declare the object type (skips scalars/string/enums/containers
        // and anything AngelScript's own registry rejects, e.g. name collisions).
        void DeclareType(const core::TypeInfo& type)
        {
            // The script-facing name: the scriptName alias when set, else the C++ name. AngelScript
            // binds + references the class under THIS (the native/wire identity stays type.name).
            const char* const scriptName = ScriptTypeName(type);
            if (!IsValidIdentifier(scriptName))
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
            const int typeId = m_engine->RegisterObjectType(scriptName, 0, asOBJ_REF);
            if (typeId < 0)
            {
                LOG_DEBUG(u8"Script",
                                   u8"AngelScript: could not declare reflected type '{}' ({})",
                                   ViewOfAscii(scriptName), typeId);
                return;
            }
            m_registered.PushBack(RegisteredType{&type, typeId});
        }

        // AngelScript has native enums: register the enum type + each enumerator (a named int32), so
        // a script writes `NetworkAuthority::Server` and an enum-typed property/param binds. Recorded
        // in m_registered like object types, so TypeInfoForTypeId maps the enum typeId back for
        // marshalling (enum values cross as their underlying int - the property setter casts).
        void DeclareEnum(const core::TypeInfo& type)
        {
            if (IsDeclared(&type))
            {
                return;
            }
            const char* const scriptName = ScriptTypeName(type); // alias when set, else the C++ name
            const int enumTypeId = m_engine->RegisterEnum(scriptName);
            if (enumTypeId < 0)
            {
                LOG_DEBUG(u8"Script", u8"AngelScript: could not declare enum '{}' ({})",
                                   ViewOfAscii(scriptName), enumTypeId);
                return;
            }
            for (const core::EnumValue& value : core::Enumerators(type))
            {
                m_engine->RegisterEnumValue(scriptName, value.name,
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
        void DeclareReferencedEnums(const core::TypeInfo& type)
        {
            auto maybe = [&](const core::TypeInfo* t)
            {
                if (t != nullptr && t->enumeratorCount > 0)
                {
                    DeclareEnum(*t);
                }
            };
            for (core::usize i = 0; i < core::PropertyCount(type); ++i)
            {
                maybe(core::PropertyAt(type, i).type);
            }
            for (core::usize i = 0; i < core::MethodCount(type); ++i)
            {
                const core::MethodInfo& method = core::MethodAt(type, i);
                for (core::u32 p = 0; p < method.paramCount; ++p)
                {
                    maybe(method.params[p].type());
                }
            }
            for (core::usize i = 0; i < core::ConstructorCount(type); ++i)
            {
                const core::ConstructorInfo& constructor = core::ConstructorAt(type, i);
                for (core::u32 p = 0; p < constructor.paramCount; ++p)
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
        void RegisterContainerMethods(const char* name, const core::TypeInfo& type,
                                      const core::PropertyInfo& property)
        {
            const core::ContainerInfo& ci = *property.type->container;
            const bool polymorphic = core::IsPolymorphicContainer(ci);
            core::String elemDecl;
            const bool elemSpellable = AppendDeclType(elemDecl, ci.elementType, /*isParam*/ false);

            auto reg = [&](const core::String& decl, Binding::Kind kind)
            {
                Binding* binding =
                    MakeBinding(Binding{kind, this, &type, nullptr, &property, nullptr});
                (void)m_engine->RegisterObjectMethod(name, CStr(decl), asFUNCTION(ContainerDispatch),
                                                     asCALL_GENERIC, binding);
            };
            {
                core::String d;
                AppendAscii(d, "uint ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_count()");
                reg(d, Binding::Kind::ContainerCount);
            }
            if (elemSpellable)
            {
                core::String d = elemDecl;
                AppendAscii(d, " ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_at(uint)");
                reg(d, Binding::Kind::ContainerAt);

                core::String a = elemDecl;
                AppendAscii(a, " ");
                AppendAscii(a, property.name);
                AppendAscii(a, polymorphic ? "_add(const string &in)" : "_add()");
                reg(a, Binding::Kind::ContainerAdd);
            }
            {
                core::String d;
                AppendAscii(d, "void ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_removeAt(uint)");
                reg(d, Binding::Kind::ContainerRemoveAt);
            }
            {
                core::String d;
                AppendAscii(d, "void ");
                AppendAscii(d, property.name);
                AppendAscii(d, "_move(uint, uint)");
                reg(d, Binding::Kind::ContainerMove);
            }
        }

        // Bind a nested-VALUE member as a read getter returning a borrow handle: `<NestedType>@
        // get_<name>() property`. Skipped if the nested type is not expressible as a handle (not
        // declared). No setter - a nested value is edited in place through the borrow, not reassigned.
        void RegisterNestedGetter(const char* name, const core::TypeInfo& type,
                                  const core::PropertyInfo& property)
        {
            core::String decl;
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
        void BindType(const core::TypeInfo& type)
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
            // Bind all members under the SCRIPT-FACING name (the scriptName alias when set) - it must
            // match the name DeclareType registered the object type under, or AngelScript cannot find it.
            const char* name = ScriptTypeName(type);

            (void)m_engine->RegisterObjectBehaviour(name, asBEHAVE_ADDREF, "void f()",
                                                    asFUNCTION(AddRefDispatch), asCALL_GENERIC);
            (void)m_engine->RegisterObjectBehaviour(name, asBEHAVE_RELEASE, "void f()",
                                                    asFUNCTION(ReleaseDispatch), asCALL_GENERIC);

            // Value assignment so `Float3 p = expr;` works (copies the boxed value). All reflected
            // types are asOBJ_REF boxes, so AngelScript otherwise reports no opAssign.
            {
                core::String decl;
                AppendAscii(decl, name);
                AppendAscii(decl, "& opAssign(const ");
                AppendAscii(decl, name);
                AppendAscii(decl, "&in)");
                (void)m_engine->RegisterObjectMethod(name, CStr(decl), asFUNCTION(AssignDispatch),
                                                     asCALL_GENERIC);
            }

            core::Array<core::String> used; // exact-declaration dedupe

            // Factories: one per reflected constructor (`builder.Constructor()` is
            // the contract's constructibility requirement).
            for (core::usize i = 0; i < core::ConstructorCount(type); ++i)
            {
                const core::ConstructorInfo& constructor = core::ConstructorAt(type, i);
                core::String decl;
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
                    used.PushBack(core::Move(decl));
                }
            }

            // Properties -> virtual property accessors (`v.x`, `v.x = 9`).
            for (core::usize i = 0; i < core::PropertyCount(type); ++i)
            {
                const core::PropertyInfo& property = core::PropertyAt(type, i);
                if (!IsValidIdentifier(property.name))
                {
                    continue;
                }
                if (core::IsNested(property))
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
                    core::String decl;
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
                const bool readOnly = (static_cast<core::u32>(property.flags) &
                                       static_cast<core::u32>(core::PropertyFlags::ReadOnly)) != 0;
                if (!readOnly)
                {
                    core::String decl;
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
            // overload registers distinctly (no arity collapsing).
            // Statics become global functions in a namespace named after the class
            // - script calls read `Float3::Dot(a, b)`.
            core::Array<core::String> usedStatics;
            for (core::usize i = 0; i < core::MethodCount(type); ++i)
            {
                const core::MethodInfo& method = core::MethodAt(type, i);
                if (!IsValidIdentifier(method.name))
                {
                    continue;
                }
                const core::TypeInfo* returnType =
                    (method.returnType != nullptr) ? method.returnType() : nullptr;
                core::String decl;
                if (returnType == nullptr)
                {
                    AppendAscii(decl, "void");
                }
                else if (!AppendDeclType(decl, returnType, /*isParam*/ false))
                {
                    continue;
                }
                AppendAscii(decl, " ");
                AppendAscii(decl, ScriptMethodName(method)); // overload identity, not the C++ name
                AppendAscii(decl, "(");
                if (!AppendParams(decl, method.params, method.paramCount))
                {
                    continue;
                }
                AppendAscii(decl, ")");

                core::Array<core::String>& dedupe = method.isStatic ? usedStatics : used;
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
                    dedupe.PushBack(core::Move(decl));
                }
                RegisterOperator(name, type, method, used);
            }
        }

        // A method marked as an operator (MethodInfo::op, a static whose first parameter is this
        // type) also binds as the object's AngelScript operator: `a + b`, `v * 2.0f`, `-v`,
        // `a == b`, and, when it yields this type, the compound `p += v`.
        void RegisterOperator(const char* name, const core::TypeInfo& type,
                              const core::MethodInfo& method, core::Array<core::String>& used)
        {
            const char* opName = nullptr;
            const char* assignName = nullptr;
            switch (method.op)
            {
            case core::MethodOperator::Add:
                opName = "opAdd";
                assignName = "opAddAssign";
                break;
            case core::MethodOperator::Subtract:
                opName = "opSub";
                assignName = "opSubAssign";
                break;
            case core::MethodOperator::Multiply:
                opName = "opMul";
                assignName = "opMulAssign";
                break;
            case core::MethodOperator::Divide:
                opName = "opDiv";
                assignName = "opDivAssign";
                break;
            case core::MethodOperator::Negate:
                opName = "opNeg";
                break;
            case core::MethodOperator::Equals:
                opName = "opEquals";
                break;
            case core::MethodOperator::None:
                return;
            }
            const bool unary = method.op == core::MethodOperator::Negate;
            if (!method.isStatic || method.paramCount != (unary ? 1u : 2u) ||
                method.params[0].type == nullptr || method.params[0].type() != &type)
            {
                return;
            }
            const core::TypeInfo* returnType =
                (method.returnType != nullptr) ? method.returnType() : nullptr;
            core::String operand; // the right operand's declaration, binary only
            if (!unary && !AppendParams(operand, method.params + 1, 1))
            {
                return;
            }
            core::String decl;
            if (returnType == nullptr || !AppendDeclType(decl, returnType, /*isParam*/ false))
            {
                return;
            }
            AppendAscii(decl, " ");
            AppendAscii(decl, opName);
            AppendAscii(decl, "(");
            decl.Append(operand.AsView());
            AppendAscii(decl, ")");
            if (!IsUsed(used, decl))
            {
                Binding* binding = MakeBinding(Binding{Binding::Kind::Operator, this, &type,
                                                       nullptr, nullptr, &method});
                if (m_engine->RegisterObjectMethod(name, CStr(decl), asFUNCTION(OperatorDispatch),
                                                   asCALL_GENERIC, binding) >= 0)
                {
                    used.PushBack(core::Move(decl));
                }
            }
            if (assignName == nullptr || returnType != &type)
            {
                return;
            }
            core::String assign;
            AppendAscii(assign, name);
            AppendAscii(assign, "& ");
            AppendAscii(assign, assignName);
            AppendAscii(assign, "(");
            assign.Append(operand.AsView());
            AppendAscii(assign, ")");
            if (!IsUsed(used, assign))
            {
                Binding* binding = MakeBinding(Binding{Binding::Kind::OperatorAssign, this, &type,
                                                       nullptr, nullptr, &method});
                if (m_engine->RegisterObjectMethod(name, CStr(assign), asFUNCTION(OperatorDispatch),
                                                   asCALL_GENERIC, binding) >= 0)
                {
                    used.PushBack(core::Move(assign));
                }
            }
        }

        // includeNames appends the reflected parameter name after each type (when one is known).
        // Off for the registration decl (AngelScript neither needs nor should reject on names);
        // on for the introspection signature so the API browser can show `Cross(Float3 a, Float3 b)`.
        bool AppendParams(core::String& decl, const core::ParamInfo* params, core::u32 count,
                          bool includeNames = false) const
        {
            for (core::u32 p = 0; p < count; ++p)
            {
                if (!AppendDeclType(decl, params[p].type != nullptr ? params[p].type() : nullptr,
                                    /*isParam*/ true))
                {
                    return false;
                }
                if (includeNames && params[p].name != nullptr && params[p].name[0] != '\0')
                {
                    AppendAscii(decl, " ");
                    AppendAscii(decl, params[p].name);
                }
                if (p + 1 < count)
                {
                    AppendAscii(decl, ", ");
                }
            }
            return true;
        }

        [[nodiscard]] static bool IsUsed(const core::Array<core::String>& used,
                                         const core::String& decl) noexcept
        {
            for (const core::String& existing : used)
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
        core::u32 m_nextContextId = 0;
        core::Array<const core::TypeInfo*> m_types;
        core::Array<RegisteredType> m_registered;
        core::Array<Binding*> m_bindings;
        core::Array<CapturedMessage> m_capturedMessages;
        core::Array<Coroutine> m_coroutines;
        core::Array<asIScriptContext*> m_dueScratch; // reused per-frame due snapshot
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
            // Loud, not silent: a dropped trailing argument is the "facade call quietly no-ops"
            // bug class (the DebugDraw.line lesson). The call still proceeds with the first
            // kMaxArgs so existing behavior is unchanged.
            LOG_WARNING(u8"Script", u8"reflected factory call has {} args; only {} are marshaled",
                        argc, kMaxArgs);
            argc = kMaxArgs;
        }
        core::Variant args[kMaxArgs];
        for (int i = 0; i < argc; ++i)
        {
            const core::ParamInfo& param = binding->constructor->params[i];
            args[i] = binding->manager->ValueFromArg(
                gen, static_cast<asUINT>(i), param.type != nullptr ? param.type() : nullptr);
        }
        ReleaseHandleArgs(gen, binding->manager);
        core::Result<core::Variant> created = binding->constructor->invoke(
            core::Span<core::Variant>{args, static_cast<core::usize>(argc)});
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
            NewBox(core::Move(created.Value()));
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
        core::Instance instance = core::ToInstance(self->value);
        binding->manager->SetGenericReturn(gen, core::GetProperty(*binding->property, instance));
    }

    void PropertySetDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        core::Instance instance = core::ToInstance(self->value);
        const core::Variant value = binding->manager->ValueFromArg(gen, 0, binding->property->type);
        ReleaseHandleArgs(gen, binding->manager);
        (void)core::SetProperty(*binding->property, instance, value);
    }

    // Resolve a polymorphic container's add-by-name to a concrete derived type: match the name
    // against each creatable derived type's `displayName` attribute, then its bare type name.
    const core::TypeInfo* ResolveElementType(const core::TypeInfo& base, core::StringView name)
    {
        core::Array<const core::TypeInfo*> derived;
        core::EnumerateDerived(base, derived);
        for (const core::TypeInfo* t : derived)
        {
            if (core::TypeAttrString(*t, "displayName", core::StringView{}) == name)
            {
                return t;
            }
        }
        for (const core::TypeInfo* t : derived)
        {
            if (core::StringView(reinterpret_cast<const core::utf8char*>(t->name)) == name)
            {
                return t;
            }
        }
        return nullptr;
    }

    // The Variant to hand a script for container element `index`: an object / value element via getAt
    // (owned); a NON-Object value element (getAt empty) via a BORROW over its address, pinned to the
    // container owner `parent` and generation-guarded. Empty if out of range / no element.
    core::Variant ContainerElementVariant(const core::ContainerInfo& ci,
                                          const core::Instance& container, core::usize index,
                                          const core::Variant& parent)
    {
        core::Variant element = core::ContainerGetAt(ci, container, index);
        if (element.IsEmpty())
        {
            const core::Instance addr = core::ContainerAddressAt(ci, container, index);
            if (addr.Pointer() != nullptr)
            {
                element = core::Variant::Borrow(addr.Pointer(), addr.Type(), parent);
            }
        }
        return element;
    }

    // A nested-VALUE member getter: returns a borrow handle over the member address (edited in place).
    void NestedGetDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        core::Instance owner = core::ToInstance(self->value);
        void* addr =
            (owner.Pointer() != nullptr) ? binding->property->address(owner) : nullptr;
        binding->manager->SetGenericReturn(
            gen, addr != nullptr
                     ? core::Variant::Borrow(addr, binding->property->type, self->value)
                     : core::Variant{});
    }

    // One dispatcher for every container op; the Binding::Kind selects which. The owner is `self`,
    // the container member is reached transiently via property.address. Object / value elements come
    // back owned; a non-Object value element comes back as a borrow (pinned to the owner).
    void ContainerDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        core::Instance owner = core::ToInstance(self->value);
        core::Instance container(binding->property->address(owner), binding->property->type);
        const core::ContainerInfo& ci = *binding->property->type->container;
        const core::usize size = core::ContainerSize(ci, container);
        switch (binding->kind)
        {
        case Binding::Kind::ContainerCount:
            gen->SetReturnDWord(static_cast<asDWORD>(size));
            break;
        case Binding::Kind::ContainerAt:
        {
            const core::usize idx = static_cast<core::usize>(gen->GetArgDWord(0));
            binding->manager->SetGenericReturn(
                gen, idx < size ? ContainerElementVariant(ci, container, idx, self->value)
                                : core::Variant{});
            break;
        }
        case Binding::Kind::ContainerAdd:
        {
            if (core::IsPolymorphicContainer(ci)) // add by element-type name
            {
                const core::Variant nameV =
                    binding->manager->ValueFromArg(gen, 0, &core::TypeOf<core::String>());
                ReleaseHandleArgs(gen, binding->manager);
                const core::String* typeName = nameV.TryGet<core::String>();
                const core::TypeInfo* elem =
                    (typeName != nullptr && ci.elementType != nullptr)
                        ? ResolveElementType(*ci.elementType, typeName->AsView())
                        : nullptr;
                if (elem == nullptr ||
                    core::ContainerCreateElement(ci, container, size, *elem).Pointer() == nullptr)
                {
                    binding->manager->SetGenericReturn(gen, core::Variant{});
                    break;
                }
            }
            else if (core::ContainerEmplaceDefault(ci, container, size).Pointer() == nullptr)
            {
                binding->manager->SetGenericReturn(gen, core::Variant{});
                break;
            }
            binding->manager->SetGenericReturn(gen,
                                               ContainerElementVariant(ci, container, size,
                                                                       self->value));
            break;
        }
        case Binding::Kind::ContainerRemoveAt:
            (void)core::ContainerRemoveAt(ci, container,
                                          static_cast<core::usize>(gen->GetArgDWord(0)));
            break;
        case Binding::Kind::ContainerMove:
            (void)core::ContainerMoveElement(ci, container,
                                             static_cast<core::usize>(gen->GetArgDWord(0)),
                                             static_cast<core::usize>(gen->GetArgDWord(1)));
            break;
        default:
            break;
        }
    }

    void MethodDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        const core::MethodInfo& method = *binding->method;
        int argc = static_cast<int>(gen->GetArgCount());
        if (argc > kMaxArgs)
        {
            LOG_WARNING(u8"Script", u8"reflected call to '{}' has {} args; only {} are marshaled",
                        reinterpret_cast<const char8_t*>(method.name), argc, kMaxArgs);
            argc = kMaxArgs;
        }
        core::Variant args[kMaxArgs];
        for (int i = 0; i < argc; ++i)
        {
            const core::TypeInfo* expected =
                (i < static_cast<int>(method.paramCount) && method.params[i].type != nullptr)
                    ? method.params[i].type()
                    : nullptr;
            args[i] = binding->manager->ValueFromArg(gen, static_cast<asUINT>(i), expected);
        }
        ReleaseHandleArgs(gen, binding->manager);
        const core::Span<core::Variant> argSpan{args, static_cast<core::usize>(argc)};
        core::Result<core::Variant> result = core::Err(core::ErrorCode::Internal);
        if (method.isStatic)
        {
            result = core::InvokeStatic(method, argSpan);
        }
        else
        {
            BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
            result = core::InvokeMethod(method, core::ToInstance(self->value), argSpan);
        }
        binding->manager->SetGenericReturn(gen,
                                           result.HasValue() ? result.Value() : core::Variant{});
    }

    void OperatorDispatch(asIScriptGeneric* gen)
    {
        const Binding* binding = static_cast<const Binding*>(gen->GetAuxiliary());
        const core::MethodInfo& method = *binding->method;
        BoxedVariant* self = static_cast<BoxedVariant*>(gen->GetObject());
        core::Variant args[2];
        args[0] = self->value; // the left operand
        const bool binary = gen->GetArgCount() == 1;
        if (binary)
        {
            const core::TypeInfo* expected =
                method.params[1].type != nullptr ? method.params[1].type() : nullptr;
            args[1] = binding->manager->ValueFromArg(gen, 0, expected);
            ReleaseHandleArgs(gen, binding->manager);
        }
        core::Result<core::Variant> result =
            core::InvokeStatic(method, core::Span<core::Variant>{args, binary ? 2u : 1u});
        if (binding->kind == Binding::Kind::OperatorAssign)
        {
            if (result.HasValue())
            {
                self->value = core::Move(result.Value());
            }
            gen->SetReturnAddress(self);
            return;
        }
        binding->manager->SetGenericReturn(gen,
                                           result.HasValue() ? result.Value() : core::Variant{});
    }

    // ---- coroutine host functions (auxiliary = the manager) ----
    void CoroutineStartDispatch(asIScriptGeneric* gen)
    {
        AngelScriptManager* manager = static_cast<AngelScriptManager*>(gen->GetAuxiliary());
        // `ScriptCoroutine@ fn` is a by-value handle argument: the generic callee OWNS this
        // reference. StartCoroutine copies what it needs (the delegate's function via Prepare's
        // AddRef, the owner via an explicit AddRef), so release our reference here or the delegate
        // (and, through it, the coroutine's owner + module) leaks.
        asIScriptFunction* fn = static_cast<asIScriptFunction*>(gen->GetArgObject(0));
        if (manager != nullptr)
        {
            manager->StartCoroutine(fn);
        }
        if (fn != nullptr)
        {
            fn->Release();
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
    // A delegate borrows its owning context (to scope CurrentScriptContext around the
    // callback); the context tracks its delegates and Detach()es them at close. Declared
    // here so AngelScriptContext can hold Array<AngelScriptDelegate*>; the methods that
    // touch its members stay out-of-line (it is defined further down).
    class AngelScriptDelegate;

    class AngelScriptContext final : public IScriptContext
    {
    public:
        AngelScriptContext(core::RefPtr<AngelScriptManager> manager, core::u32 id)
            : m_manager(core::Move(manager))
        {
            AppendAscii(m_namePrefix, "ctx");
            AppendUint(m_namePrefix, id);
        }

        // Out-of-line: the destructor Detach()es tracked delegates, which needs the
        // complete AngelScriptDelegate type (defined further down).
        ~AngelScriptContext() override;

        AngelScriptContext(const AngelScriptContext&) = delete;
        AngelScriptContext& operator=(const AngelScriptContext&) = delete;

        // A delegate wrapping one of this context's script functions registers itself here so
        // the context can Detach() it at close (after which its Invoke fails cleanly instead of
        // touching a discarded module). Borrowed - the delegate untracks itself on destruction.
        void TrackDelegate(AngelScriptDelegate* delegate) { m_delegates.PushBack(delegate); }
        void UntrackDelegate(AngelScriptDelegate* delegate)
        {
            for (core::usize i = 0; i < m_delegates.Size(); ++i)
            {
                if (m_delegates[i] == delegate)
                {
                    m_delegates.RemoveAt(i);
                    return;
                }
            }
        }

        void SetErrorHandler(IScriptErrorHandler* handler) override { m_errorHandler = handler; }

        core::Status Load(core::StringView source, core::StringView chunkName) override
        {
            asIScriptEngine* engine = m_manager->Engine();
            core::String moduleName = m_namePrefix;
            AppendAscii(moduleName, ":");
            AppendUint(moduleName, m_loadCounter++);
            asIScriptModule* module = engine->GetModule(CStr(moduleName), asGM_ALWAYS_CREATE);
            if (module == nullptr)
            {
                return core::Status{core::ErrorCode::Internal};
            }

            const core::String section(chunkName);
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
                return core::Status{initFailed ? core::ErrorCode::Internal
                                               : core::ErrorCode::InvalidArgument};
            }

            m_ownedModules.PushBack(module);
            m_module = module;

            // Top-level entry convention: a module-level `void main()` runs at load
            // (AngelScript has no top-level statements; this is the runtime-fault
            // and script-setup seam the contract's Load semantics map onto).
            if (asIScriptFunction* entry = module->GetFunctionByName("main"))
            {
                core::Result<core::Variant> ran =
                    ExecuteCall(entry, nullptr, core::Span<core::Variant>{});
                if (!ran.HasValue())
                {
                    return core::Status{core::ErrorCode::Internal};
                }
            }
            return core::Status{};
        }

        // Load a precompiled bytecode blob (the player path - LoadByteCode, never Build). Same
        // module lifecycle + main() entry convention as Load; AngelScript restores global vars
        // during LoadByteCode. The engine must carry the SAME reflected registration the blob was
        // saved against (the shared global registry guarantees it) or LoadByteCode rejects it.
        core::Status LoadBlob(IScriptBlob& blob) override
        {
            // IScriptBlob is opaque (no RTTI node); only this backend produces AngelScript blobs
            // and the run host pairs the AS context with the AS manager, so this downcast is the
            // committed pattern.
            AngelScriptScriptBlob& asBlob = static_cast<AngelScriptScriptBlob&>(blob);
            if (asBlob.Bytes().IsEmpty())
            {
                return core::Status{core::ErrorCode::InvalidArgument};
            }
            asIScriptEngine* engine = m_manager->Engine();
            core::String moduleName = m_namePrefix;
            AppendAscii(moduleName, ":");
            AppendUint(moduleName, m_loadCounter++);
            asIScriptModule* module = engine->GetModule(CStr(moduleName), asGM_ALWAYS_CREATE);
            if (module == nullptr)
            {
                return core::Status{core::ErrorCode::Internal};
            }
            ByteBufferStream stream(asBlob.Bytes());
            m_manager->BeginMessageCapture();
            int result;
            {
                ScriptCallScope scope(this);
                result = module->LoadByteCode(&stream);
            }
            m_manager->EndMessageCapture(m_errorHandler, ScriptErrorKind::Compile);
            if (result < 0)
            {
                module->Discard();
                return core::Status{core::ErrorCode::InvalidArgument};
            }
            m_ownedModules.PushBack(module);
            m_module = module;
            // Same top-level entry convention as Load: a module-level `void main()` runs at load.
            if (asIScriptFunction* entry = module->GetFunctionByName("main"))
            {
                core::Result<core::Variant> ran =
                    ExecuteCall(entry, nullptr, core::Span<core::Variant>{});
                if (!ran.HasValue())
                {
                    return core::Status{core::ErrorCode::Internal};
                }
            }
            return core::Status{};
        }

        // The behaviors module. A class carrying cooked BYTECODE loads as its OWN module via
        // LoadByteCode (the player path - no compiler; the cooked blob is a single-class module
        // that kept its debug info + sourceName section, so breakpoints still line up). Classes
        // WITHOUT bytecode (freshly authored, not yet cooked) build together into one combined
        // module, each in its own section named by its sourceName - GetLineNumber then reports
        // (sourceFile, sourceLine). CreateInstance/FindFunction search
        // ALL owned modules newest-first, so the bytecode/source split is transparent to callers.
        core::Status LoadBehaviorModule(core::Span<const BehaviorModuleClass> classes,
                                        core::StringView moduleName) override
        {
            core::Array<BehaviorModuleClass> sourceOnly;
            for (const BehaviorModuleClass& entry : classes)
            {
                if (!entry.bytecode.IsEmpty())
                {
                    const core::Status status = LoadClassBytecode(entry.bytecode);
                    if (!status.IsOk())
                    {
                        return status;
                    }
                }
                else
                {
                    sourceOnly.PushBack(entry);
                }
            }
            if (sourceOnly.IsEmpty())
            {
                return core::Status{};
            }

            asIScriptEngine* engine = m_manager->Engine();
            const core::String moduleNameStr(moduleName);
            asIScriptModule* module = engine->GetModule(CStr(moduleNameStr), asGM_ALWAYS_CREATE);
            if (module == nullptr)
            {
                return core::Status{core::ErrorCode::Internal};
            }

            // One section PER CLASS, named by its sourceName (the editor's breakpoint key).
            // A class with no sourceName falls back to the module name (still compiles; only
            // its breakpoints won't line up - the cook always stamps sourceName).
            for (const BehaviorModuleClass& entry : sourceOnly)
            {
                const core::String section(entry.name.IsEmpty() ? moduleName : entry.name);
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
                return core::Status{initFailed ? core::ErrorCode::Internal
                                               : core::ErrorCode::InvalidArgument};
            }

            m_ownedModules.PushBack(module);
            m_module = module;

            if (asIScriptFunction* entry = module->GetFunctionByName("main"))
            {
                core::Result<core::Variant> ran =
                    ExecuteCall(entry, nullptr, core::Span<core::Variant>{});
                if (!ran.HasValue())
                {
                    return core::Status{core::ErrorCode::Internal};
                }
            }
            return core::Status{};
        }

        // Reconstruct a cooked AngelScript blob from its SERIALIZED bytes (ScriptClass::bytecode -
        // AngelScriptBlobFromModule -> Serialize) and LoadByteCode it into its own module, exactly
        // as LoadBlob does (unique module name -> hot reload keeps old generations alive; main()
        // runs). The player path for a single behavior class.
        core::Status LoadClassBytecode(core::Span<const core::byte> serialized)
        {
            AngelScriptScriptBlob blob;
            core::MemoryStream stream;
            (void)stream.Write(serialized.Data(), serialized.Size());
            (void)stream.Seek(0, core::SeekOrigin::Begin);
            core::BinarySerializer reader(stream, core::SerializeMode::Read);
            blob.Serialize(reader);
            return LoadBlob(blob);
        }

        void SetGlobal(core::StringView name, const core::Variant& value) override
        {
            if (m_module == nullptr)
            {
                return;
            }
            const core::String globalName(name);
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

        [[nodiscard]] core::Variant GetGlobal(core::StringView name) override
        {
            if (m_module == nullptr)
            {
                return core::Variant{};
            }
            const core::String globalName(name);
            const int index = m_module->GetGlobalVarIndexByName(CStr(globalName));
            if (index < 0)
            {
                return core::Variant{};
            }
            int typeId = 0;
            (void)m_module->GetGlobalVar(static_cast<asUINT>(index), nullptr, nullptr, &typeId,
                                         nullptr);
            return m_manager->VariantFromTypedAddress(
                typeId, m_module->GetAddressOfGlobalVar(static_cast<asUINT>(index)));
        }

        [[nodiscard]] bool HasFunction(core::StringView name) const override
        {
            return FindFunction(name, -1) != nullptr;
        }

        [[nodiscard]] core::Result<core::Variant> Call(core::StringView function,
                                                       core::Span<core::Variant> args) override
        {
            // Strict arity: never Execute with unset argument slots.
            asIScriptFunction* target = FindFunction(function, static_cast<int>(args.Size()));
            if (target == nullptr || target->GetParamCount() != args.Size())
            {
                return core::Err(core::ErrorCode::NotFound);
            }
            return ExecuteCall(target, nullptr, args);
        }

        [[nodiscard]] core::RefPtr<ScriptObject>
        CreateInstance(core::StringView className, core::Span<core::Variant> args) override;

        // Prepare + execute a script function on the engine's pooled contexts.
        // The public seam AngelScriptObject::Invoke dispatches through as well.
        [[nodiscard]] core::Result<core::Variant>
        ExecuteCall(asIScriptFunction* function, void* object, core::Span<core::Variant> args)
        {
            asIScriptEngine* engine = m_manager->Engine();
            asIScriptContext* executor = engine->RequestContext();
            if (executor == nullptr || executor->Prepare(function) < 0)
            {
                if (executor != nullptr)
                {
                    engine->ReturnContext(executor);
                }
                return core::Err(core::ErrorCode::Internal);
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
                return core::Err(core::ErrorCode::Internal);
            }
            for (BoxedVariant* box : boxTemps)
            {
                ReleaseBox(box);
            }

            core::Result<core::Variant> outcome = core::Err(core::ErrorCode::Internal);
            if (result == asEXECUTION_FINISHED)
            {
                const int returnTypeId = function->GetReturnTypeId();
                outcome = (returnTypeId == asTYPEID_VOID)
                              ? core::Result<core::Variant>(core::Variant{})
                              : core::Result<core::Variant>(m_manager->VariantFromTypedAddress(
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
        [[nodiscard]] asIScriptFunction* FindFunction(core::StringView name, int argc) const
        {
            const core::String functionName(name);
            // Search owned modules NEWEST-first: hot reload appends a fresh generation (so the
            // newest wins, the "resolves against the last loaded module" contract), and per-class
            // bytecode modules mean a function can live in any owned module, not just m_module.
            asIScriptFunction* byName = nullptr;
            for (core::usize m = m_ownedModules.Size(); m-- > 0;)
            {
                asIScriptModule* module = m_ownedModules[m];
                for (asUINT i = 0; i < module->GetFunctionCount(); ++i)
                {
                    asIScriptFunction* candidate = module->GetFunctionByIndex(i);
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
            }
            return byName;
        }

        void BindArgs(asIScriptContext* executor, asIScriptFunction* function,
                      core::Span<core::Variant> args, std::string* stringTemps,
                      BoxedVariant** boxTemps)
        {
            const core::usize limit = function->GetParamCount();
            if (args.Size() > static_cast<core::usize>(kMaxArgs) &&
                limit > static_cast<core::usize>(kMaxArgs))
            {
                LOG_WARNING(u8"Script", u8"script call '{}' has {} args; only {} are marshaled",
                            reinterpret_cast<const char8_t*>(function->GetName()), args.Size(),
                            kMaxArgs);
            }
            for (core::usize i = 0;
                 i < args.Size() && i < limit && i < static_cast<core::usize>(kMaxArgs); ++i)
            {
                const asUINT arg = static_cast<asUINT>(i);
                int typeId = 0;
                (void)function->GetParam(arg, &typeId);
                const core::Variant& value = args[i];
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
                                               static_cast<asBYTE>(static_cast<core::i64>(number)));
                    continue;
                case asTYPEID_INT16:
                case asTYPEID_UINT16:
                    (void)executor->SetArgWord(arg,
                                               static_cast<asWORD>(static_cast<core::i64>(number)));
                    continue;
                case asTYPEID_INT32:
                case asTYPEID_UINT32:
                    (void)executor->SetArgDWord(
                        arg, static_cast<asDWORD>(static_cast<core::i64>(number)));
                    continue;
                case asTYPEID_INT64:
                case asTYPEID_UINT64:
                    (void)executor->SetArgQWord(
                        arg, static_cast<asQWORD>(static_cast<core::i64>(number)));
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
                                        static_cast<core::i32>(line),
                                        ViewOfAscii(executor->GetExceptionString())};
                m_errorHandler->OnError(error);
                return;
            }
            core::ConsoleWriteError(ViewOfAscii(executor->GetExceptionString()));
        }

        core::RefPtr<AngelScriptManager> m_manager;
        IScriptErrorHandler* m_errorHandler = nullptr;
        core::String m_namePrefix;
        core::u32 m_loadCounter = 0;
        asIScriptModule* m_module = nullptr; // most recent successful Load
        core::Array<asIScriptModule*> m_ownedModules;
        core::Array<AngelScriptDelegate*> m_delegates; // borrowed; Detach()ed at close
    };

    // A live instance of a script-declared class. Holds the asIScriptObject plus a
    // strong reference to its owning context (which keeps the manager - and thus
    // the engine - alive).
    class AngelScriptObject final : public ScriptObject
    {
    public:
        AngelScriptObject(core::RefPtr<AngelScriptContext> owner,
                          asIScriptObject* instance) noexcept
            : m_owner(core::Move(owner)), m_instance(instance)
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

        [[nodiscard]] core::Result<core::Variant> Invoke(core::StringView method,
                                                         core::Span<core::Variant> args) override
        {
            asITypeInfo* type = m_instance->GetObjectType();
            if (type == nullptr)
            {
                return core::Err(core::ErrorCode::NotFound);
            }

            // The neutral property-apply path Invokes the setter as `<name>=` with one
            // argument (the setter convention). AngelScript has no method
            // by that name; its editor properties are plain member FIELDS. So a trailing
            // `=` with exactly one arg writes the same-named member field directly - the
            // "settable member" AngelScript exposes for harvested behavior properties.
            if (args.Size() == 1 && !method.IsEmpty() && method[method.Size() - 1] == u8'=')
            {
                const core::StringView fieldName = method.SubStr(0, method.Size() - 1);
                if (SetMemberField(fieldName, args[0]))
                {
                    return core::Variant{};
                }
                return core::Err(core::ErrorCode::NotFound);
            }

            const core::String methodName(method);
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
                return core::Err(core::ErrorCode::NotFound);
            }
            return m_owner->ExecuteCall(target, m_instance, args);
        }

        [[nodiscard]] core::Result<core::Variant> GetProperty(core::StringView name) override
        {
            const core::String fieldName(name);
            const asUINT count = m_instance->GetPropertyCount();
            for (asUINT i = 0; i < count; ++i)
            {
                const char* memberName = m_instance->GetPropertyName(i);
                if (memberName != nullptr && NameEq(memberName, CStr(fieldName)))
                {
                    return m_owner->Manager().VariantFromTypedAddress(
                        m_instance->GetPropertyTypeId(i), m_instance->GetAddressOfProperty(i));
                }
            }
            return core::Err(core::ErrorCode::NotFound);
        }

    private:
        // Writes the instance member field named `fieldName` from `value` (the harvested
        // editor-property apply path). Marshals through the manager's typed-address writer,
        // so scalars/string/reflected-handle members all take the value uniformly. False
        // when no such (writable) member exists or the value's type cannot fill it.
        [[nodiscard]] bool SetMemberField(core::StringView fieldName, const core::Variant& value)
        {
            const core::String name(fieldName);
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
                    LOG_WARNING(
                        u8"Script",
                        u8"AngelScript property '{}' is a value member; declare it as a handle "
                        u8"('Type@ {}') to receive a reflected/resource value",
                        memberName, memberName);
                }
                return false;
            }
            return false;
        }

        core::RefPtr<AngelScriptContext> m_owner;
        asIScriptObject* m_instance;
    };

    // A script function/closure held as a native callback. Holds the manager (keeps the
    // engine alive) and one AddRef on the funcdef handle (the GC-safe promise); Invoke runs
    // it on a pooled context. It references the MANAGER, not the owning context, so it never
    // cycles with a context that stores it back (e.g. through a reflected event object).
    class AngelScriptDelegate final : public IScriptDelegate
    {
    public:
        AngelScriptDelegate(core::RefPtr<AngelScriptManager> manager, AngelScriptContext* context,
                            asIScriptFunction* function) noexcept
            : m_manager(core::Move(manager)), m_context(context), m_function(function)
        {
            if (m_function != nullptr)
            {
                m_function->AddRef();
            }
        }

        ~AngelScriptDelegate() override
        {
            if (m_context != nullptr)
            {
                m_context->UntrackDelegate(this);
            }
            if (m_function != nullptr)
            {
                m_function->Release();
            }
        }

        AngelScriptDelegate(const AngelScriptDelegate&) = delete;
        AngelScriptDelegate& operator=(const AngelScriptDelegate&) = delete;

        // The owning context calls this when it closes: the delegate outlives it, and its
        // function's module is discarded, so Invoke must fail cleanly afterwards.
        void Detach() noexcept { m_context = nullptr; }

        [[nodiscard]] core::Result<core::Variant> Invoke(core::Span<core::Variant> args) override
        {
            if (m_context == nullptr || m_manager.Get() == nullptr || m_function == nullptr)
            {
                return core::Err(core::ErrorCode::Internal); // the owning context is gone
            }
            // Scope the owning context around the callback so facades the handler calls
            // (ui::pop, run::loadScene) resolve their per-context services through
            // CurrentScriptContext() - mirrors LuauScriptDelegate::Invoke. ExecuteDelegate
            // itself runs the function on a pooled asIScriptContext.
            ScriptCallScope scope(m_context);
            return m_manager->ExecuteDelegate(m_function, args);
        }

    private:
        core::RefPtr<AngelScriptManager> m_manager;
        AngelScriptContext* m_context; // borrowed; nulled by Detach() at context close
        asIScriptFunction* m_function;
    };

    // Out-of-line (needs the complete AngelScriptDelegate above): Detach() every delegate that
    // wrapped one of this context's functions so a later Invoke fails cleanly, then discard the
    // owned modules. No ScriptObject outlives its context (they hold a strong ref), so the
    // module discard is safe here.
    AngelScriptContext::~AngelScriptContext()
    {
        for (AngelScriptDelegate* delegate : m_delegates)
        {
            delegate->Detach();
        }
        m_delegates.Clear();
        for (asIScriptModule* module : m_ownedModules)
        {
            module->Discard();
        }
    }

    core::Variant MakeAngelScriptDelegateVariant(asIScriptFunction* function)
    {
        // The wrapping happens inside a reflected dispatch, so the executing context (and its
        // manager) is the current script context.
        IScriptContext* current = CurrentScriptContext();
        if (current == nullptr || function == nullptr)
        {
            return core::Variant{};
        }
        AngelScriptContext* context = static_cast<AngelScriptContext*>(current);
        core::RefPtr<AngelScriptManager> manager(&context->Manager());
        core::RefPtr<AngelScriptDelegate> delegate(core::MakeRef<AngelScriptDelegate>(
            context->MemoryAllocator(), core::Move(manager), context, function));
        // Track the borrowed context pointer so the context can Detach() it at close.
        context->TrackDelegate(delegate.Get());
        return core::Variant::From(core::RefPtr<IScriptDelegate>(delegate.Get()));
    }

    void AngelScriptManager::CancelCoroutinesFor(ScriptObject& instance)
    {
        // We hold the owner pointer directly - drop every coroutine started by this
        // behavior instance (its context is aborted + released, its owner ref freed).
        asIScriptObject* owner = static_cast<AngelScriptObject&>(instance).ScriptInstance();
        for (core::usize i = m_coroutines.Size(); i-- > 0;)
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
        void SetBreakpoint(core::StringView file, core::i32 line) override
        {
            for (const Breakpoint& breakpoint : m_breakpoints)
            {
                if (breakpoint.line == line && breakpoint.file.AsView() == file)
                {
                    return;
                }
            }
            m_breakpoints.PushBack(Breakpoint{core::String(file), line});
        }

        void RemoveBreakpoint(core::StringView file, core::i32 line) override
        {
            for (core::usize i = 0; i < m_breakpoints.Size(); ++i)
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

        [[nodiscard]] core::Array<ScriptStackFrame> CaptureStackFrames() override
        {
            core::Array<ScriptStackFrame> frames;
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
                frame.file = core::String(ViewOfAscii(section));
                asIScriptFunction* function = m_pausedContext->GetFunction(level);
                frame.function = core::String(
                    ViewOfAscii(function != nullptr ? function->GetDeclaration() : "?"));
                frames.PushBack(core::Move(frame));
            }
            return frames;
        }

        [[nodiscard]] core::Array<ScriptVariable> CaptureLocals(core::u32 depth) override
        {
            core::Array<ScriptVariable> locals;
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
                variable.name = core::String(ViewOfAscii(name));
                const char* declaration =
                    m_pausedContext->GetVarDeclaration(static_cast<asUINT>(i), depth, false);
                variable.typeName = core::String(ViewOfAscii(declaration));
                void* address = m_pausedContext->GetAddressOfVar(static_cast<asUINT>(i), depth);
                if (address == nullptr)
                {
                    variable.value = core::String(u8"<uninitialized>");
                    locals.PushBack(core::Move(variable));
                    continue;
                }
                core::Variant value = m_manager->VariantFromTypedAddress(typeId, address);
                DescribeValue(variable, value);
                locals.PushBack(core::Move(variable));
            }
            return locals;
        }

        [[nodiscard]] core::Array<ScriptVariable> CaptureObject(core::u64 objectRef) override
        {
            core::Array<ScriptVariable> members;
            core::Variant* stored = FindObject(objectRef);
            if (stored == nullptr)
            {
                return members;
            }
            const core::TypeInfo* type = stored->Type();
            if (type == nullptr)
            {
                return members;
            }
            core::Instance instance = core::ToInstance(*stored);
            for (core::usize i = 0; i < core::PropertyCount(*type); ++i)
            {
                const core::PropertyInfo& property = core::PropertyAt(*type, i);
                if (core::IsNested(property))
                {
                    continue; // no by-value read for a nested structure (empty Variant)
                }
                ScriptVariable variable;
                variable.name = core::String(ViewOfAscii(property.name));
                variable.typeName =
                    core::String(ViewOfAscii(property.type != nullptr ? property.type->name : "?"));
                core::Variant value = core::GetProperty(property, instance);
                DescribeValue(variable, value);
                members.PushBack(core::Move(variable));
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
            core::String file;
            core::i32 line = -1;
        };

        struct CapturedObject
        {
            core::u64 ref = 0;
            core::Variant value;
        };

        [[nodiscard]] bool IsBreakpoint(const char* section, int line) const
        {
            const core::StringView sectionView = ViewOfAscii(section);
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
        void DescribeValue(ScriptVariable& variable, const core::Variant& value)
        {
            const core::TypeInfo* type = value.Type();
            const bool expandable = type != nullptr && core::PropertyCount(*type) > 0 &&
                                    PrimitiveDeclName(type) == nullptr;
            if (expandable)
            {
                variable.objectRef = StoreObject(value);
            }
            variable.value = DebugValueText(value);
        }

        [[nodiscard]] core::u64 StoreObject(const core::Variant& value)
        {
            const core::u64 ref = m_nextObjectRef++;
            m_objects.PushBack(CapturedObject{ref, value});
            return ref;
        }

        [[nodiscard]] core::Variant* FindObject(core::u64 ref)
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
        core::Array<Breakpoint> m_breakpoints;
        core::Array<CapturedObject> m_objects; // lazily-expandable handles for this break
        core::u64 m_nextObjectRef = 1;         // 0 = a leaf scalar
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

    core::UniquePtr<IScriptDebugger> AngelScriptManager::CreateDebugger()
    {
        return core::MakeUnique<AngelScriptDebugger>(MemoryAllocator(), this);
    }

    core::RefPtr<IScriptContext> AngelScriptManager::CreateContext()
    {
        // Defensive finalize (the documented contract): a context may be created
        // without RegisterReflectedTypes having driven the two-phase emission.
        FinalizeTypes();
        return core::RefPtr<IScriptContext>(core::MakeRef<AngelScriptContext>(
            MemoryAllocator(), core::RefPtr<AngelScriptManager>(this), m_nextContextId++));
    }

    core::RefPtr<ScriptObject> AngelScriptContext::CreateInstance(core::StringView className,
                                                                  core::Span<core::Variant> args)
    {
        const core::String name(className);
        // Search owned modules NEWEST-first (hot reload's fresh generation wins; a class can live
        // in its own per-class bytecode module, not just m_module - the same generalization as
        // FindFunction).
        asITypeInfo* type = nullptr;
        for (core::usize m = m_ownedModules.Size(); m-- > 0;)
        {
            type = m_ownedModules[m]->GetTypeInfoByDecl(CStr(name));
            if (type != nullptr)
            {
                break;
            }
        }
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
        return core::RefPtr<ScriptObject>(core::MakeRef<AngelScriptObject>(
            MemoryAllocator(), core::RefPtr<AngelScriptContext>(this), instance));
    }

    core::RefPtr<IScriptManager> CreateScriptManager(core::IAllocator& allocator)
    {
        return core::RefPtr<IScriptManager>(core::MakeRef<AngelScriptManager>(allocator));
    }

    core::StringView AngelScriptCoroutineModulePrelude() noexcept
    {
        return ViewOfAscii(kCoroutinePreludeSection);
    }

    core::u32 AngelScriptBytecodeVersion() noexcept
    {
        return static_cast<core::u32>(ANGELSCRIPT_VERSION);
    }

    core::RefPtr<IScriptBlob> AngelScriptBlobFromModule(void* modulePtr,
                                                        core::IAllocator& allocator)
    {
        asIScriptModule* module = static_cast<asIScriptModule*>(modulePtr);
        if (module == nullptr)
        {
            return core::RefPtr<IScriptBlob>{};
        }
        core::RefPtr<AngelScriptScriptBlob> blob =
            core::MakeRef<AngelScriptScriptBlob>(allocator);
        ByteBufferStream stream(blob->Bytes());
        if (module->SaveByteCode(&stream, /*stripDebugInfo=*/false) < 0)
        {
            return core::RefPtr<IScriptBlob>{};
        }
        return core::RefPtr<IScriptBlob>(blob.Get());
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
        desc.languageId = core::String(u8"angelscript");
        desc.displayName = core::String(u8"AngelScript");
        desc.fileExtensions.PushBack(core::String(u8"as"));
        desc.create = [](core::IAllocator& allocator)
        { return CreateScriptManager(allocator); };
        ScriptBackendRegistry::Get().Register(core::Move(desc));
    }
}

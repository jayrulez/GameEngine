// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script-backend CONFORMANCE BATTERY: every registered backend
// must pass this against its own language dialect - a backend is DONE when this is
// green, never "hopefully it works". Include from a doctest TU and call
// RunScriptBackendConformance inside a TEST_CASE.
//
// The battery certifies the CONTEXT contract: manager/context lifecycle, the two-phase
// type registration flow, load/compile/runtime error reporting through the handler,
// Variant marshalling both ways, script-class instantiation + method dispatch, context
// isolation, services, and GC hooks. Reflected-type EMISSION (foreign classes for
// engine types) is certified separately per backend by porting the reflected-type
// suite - class syntax differs too much per language to share source.
#pragma once

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.script;

namespace foundation::script::conformance
{
    using namespace foundation::core;

    // A real native API that takes a script function as a typed callback (the delegate
    // seam's "user"): an event a behavior subscribes to, that native code later fires.
    // Certified below on every backend that declares ScriptCapabilities::Delegates.
    class DelegateSignal : public Object
    {
        RTTI_OBJECT(DelegateSignal, Object)
    public:
        // A script function subscribes; holding the RefPtr keeps it alive across GC.
        void Connect(RefPtr<IScriptDelegate> handler) { m_handler = Move(handler); }

        // Fire the event with one value; returns the handler's result (0 if unconnected).
        f64 Emit(f64 value)
        {
            if (m_handler.Get() == nullptr)
            {
                return 0.0;
            }
            Variant args[] = {Variant::From<f64>(value)};
            Result<Variant> result = m_handler->Invoke(Span<Variant>{args, 1});
            if (!result.HasValue())
            {
                return 0.0;
            }
            const f64* returned = result.Value().TryGet<f64>();
            return (returned != nullptr) ? *returned : 0.0;
        }

        // The stored delegate (so a test can invoke it directly from C++). Not reflected -
        // returning a delegate to script is not part of the seam.
        [[nodiscard]] RefPtr<IScriptDelegate> Handler() const { return m_handler; }

    private:
        RefPtr<IScriptDelegate> m_handler;
    };

    // Overloaded methods for the overloadedName contract. `ping` is an ARITY FAMILY (0-arg and
    // 1-arg share the script name, dispatched by argument count); `combine` is a same-arity TYPE
    // overload split by a distinct overloadedName (`combine` + `combineText`). Registered on demand
    // (never a global-registry type), like DelegateSignal.
    class Overloads : public Object
    {
        RTTI_OBJECT(Overloads, Object)
    public:
        [[nodiscard]] f64 ping() const { return 1.0; }
        [[nodiscard]] f64 ping(f64 x) const { return x + 100.0; }
        [[nodiscard]] f64 combine(f64 a, f64 b) const { return a + b; }
        [[nodiscard]] f64 combine(String, f64 b) const { return b; }
    };

    REFLECT_MEMBERS(Overloads, "rtti::script::conformance")
    {
        builder.Constructor();
        builder.Method<static_cast<f64 (Overloads::*)() const>(&Overloads::ping)>("ping");
        builder.Method<static_cast<f64 (Overloads::*)(f64) const>(&Overloads::ping)>("ping");
        builder.Method<static_cast<f64 (Overloads::*)(f64, f64) const>(&Overloads::combine)>(
            "combine");
        builder.Method<static_cast<f64 (Overloads::*)(String, f64) const>(&Overloads::combine)>(
                   "combine")
            .OverloadedName("combineText");
    }

    // Violates the contract: two methods on the same (name, arity, static) with no distinct
    // overloadedName. Only the pure collision check runs on it - never registered with a manager
    // (that would trap FinalizeTypes, which is exactly the contract).
    class Colliding : public Object
    {
        RTTI_OBJECT(Colliding, Object)
    public:
        [[nodiscard]] f64 clash(f64 a) const { return a; }
        [[nodiscard]] f64 clash(String) const { return 0.0; }
    };

    REFLECT_MEMBERS(Colliding, "rtti::script::conformance")
    {
        builder.Constructor();
        builder.Method<static_cast<f64 (Colliding::*)(f64) const>(&Colliding::clash)>("clash");
        builder.Method<static_cast<f64 (Colliding::*)(String) const>(&Colliding::clash)>("clash");
    }

    // Container-member contract fixtures: a reflected Array member binds as
    // owner ops (`items_count/_at/_add/_removeAt/_move`) with ZERO-based indices and
    // WRITE-THROUGH element handles on every backend - one contract, no per-backend container
    // semantics. Registered on demand, like DelegateSignal.
    struct CrateItem
    {
        f64 size = 0.0;
    };
    class Crate : public Object
    {
        RTTI_OBJECT(Crate, Object)
    public:
        Array<UniquePtr<CrateItem>> items;
    };
    REFLECT_VALUE(CrateItem, "rtti::script::conformance")
    {
        builder.Property<&CrateItem::size>("size");
    }
    REFLECT_MEMBERS(Crate, "rtti::script::conformance")
    {
        builder.Constructor();
        builder.Nested<&Crate::items>("items");
    }

    // Gone-handle contract fixtures: a script holds a Lamp by a RESOLVE-mode handle (a slot and a
    // generation into a native table, as an element or a component is held), so the native side
    // can take the lamp away while the script still holds it. Registered on demand.
    class Lamp : public Object
    {
        RTTI_OBJECT(Lamp, Object)
    public:
        f64 level = 2.0;
        void Bump() { level += 1.0; }

        struct Slot
        {
            Lamp* lamp = nullptr;
            u32 generation = 1;
        };
        struct HandleCtx
        {
            i32 slot = 0;
            u32 generation = 0;
        };
        static Slot* Slots()
        {
            static Slot slots[2];
            return slots;
        }
        static void* Resolve(const Variant& value)
        {
            const auto* ctx = static_cast<const HandleCtx*>(value.ResolveContext());
            const Slot& slot = Slots()[ctx->slot];
            return slot.generation == ctx->generation ? slot.lamp : nullptr;
        }
        // The script's way in: a handle to the lamp in `slot` (empty when there is none).
        static Variant Take(i32 slot)
        {
            if (slot < 0 || slot > 1 || Slots()[slot].lamp == nullptr)
            {
                return Variant{};
            }
            return Variant::Resolving(&StaticType(), &Resolve, HandleCtx{slot, Slots()[slot].generation});
        }
        // The native side takes the lamp away: every handle to the slot is gone.
        static void TakeAway(i32 slot)
        {
            Slots()[slot].lamp = nullptr;
            ++Slots()[slot].generation;
        }
    };
    REFLECT_MEMBERS(Lamp, "rtti::script::conformance")
    {
        builder.Constructor();
        builder.Property<&Lamp::level>("level");
        builder.Method<&Lamp::Bump>("bump");
        builder.Method<&Lamp::Take, Lamp>("take");
    }

    /// The language-specific sources. Every snippet implements a FIXED contract:
    ///  - functionsModule: function `add(a, b)` returning a + b, function `greeting()`
    ///    returning the string "hi", a module global `answer` readable as 42.
    ///  - counterClass: class `Counter`, constructed with one number, method
    ///    `increment()` adding 1, zero-arg method `value()` returning the count.
    ///  - compileBroken: source that CANNOT compile.
    ///  - runtimeFault: source that compiles but faults at load/run time.
    ///  - coroutineClass (optional; certified only when the backend declares the
    ///    Coroutines capability): a class `Coro`, constructed with NO args, with methods
    ///    `begin()` starting a coroutine that waits ~1.0s then sets progress to 1,
    ///    `beginUntil()` starting a coroutine that waitUntil-s a predicate then sets
    ///    progress to 1, `flip()` making that predicate true, and `progress()` returning
    ///    0 (pending) or 1 (done). The coroutine belongs to the `Coro` instance.
    struct Dialect
    {
        StringView languageId;
        StringView functionsModule;
        StringView counterClass;
        StringView compileBroken;
        StringView runtimeFault;
        StringView coroutineClass; // optional - see the Coroutines section below
        // Optional (certified only when the backend declares the Delegates capability):
        // source that creates a global `signal` of the native DelegateSignal type and
        // subscribes a function computing value * 2 (via the backend's own callable syntax -
        // e.g. an AngelScript funcdef handle).
        StringView delegateModule;
        // Optional overload-contract module: constructs a global `over` of the native Overloads
        // type and computes module globals `OP0 = over.ping()` (-> 1), `OP1 = over.ping(5)` (-> 105,
        // the arity family), `OC = over.combine(2, 3)` (-> 5), `OCT = over.combineText("x", 7)`
        // (-> 7, the distinct-name overload). Via the backend's own construct + method-call syntax.
        StringView overloadModule;
        // Optional container-member module: constructs a global `crate` of the native Crate type,
        // adds two items via `items_add` setting sizes 5 and 9 THROUGH THE RETURNED HANDLES, then
        // computes module globals: KN = items_count() (-> 2), KZ = items_at(0).size (-> 5,
        // zero-based), then items_move(1, 0), KM = items_at(0).size (-> 9), then items_removeAt(0),
        // KR = items_count() (-> 1), KL = items_at(0).size (-> 5). KZ/KM/KL only pass when
        // mutations through returned handles WROTE THROUGH into the container.
        StringView containerModule;
        // Optional gone-handle module: a global `lamp` from `Lamp.take(0)` (the backend's static
        // call syntax) and four zero-arg functions: `valid()` returning 1 when `lamp:isValid()`
        // and 0 when not, `readLevel()` returning `lamp.level`, `writeLevel()` setting it to 7, and
        // `bump()` calling `lamp:bump()`. Once the lamp is taken away, every one but `valid()` is
        // the script's error, in every backend.
        StringView goneModule;
        // Optional (certified only when the backend declares the Debugger capability): a module
        // with a zero-arg entry function `debugFunction`. Execution reaching `debugBreakLine`
        // (1-based, in the section named `debugSection`) must have a local named `debugLocalName`
        // in scope whose captured display text equals `debugLocalValue`. The line above the break
        // must NOT be the last line (Step lands on a further line, then Continue completes).
        StringView debugModule;
        StringView debugSection;
        StringView debugFunction;
        StringView debugLocalName;
        StringView debugLocalValue;
        i32 debugBreakLine = 0;
    };

    // Reflected so a script can construct/subscribe it; StaticType() lives in this header
    // (single TU per test executable). It is registered with the manager on demand, never a
    // global-registry type - so the introspection diff never demands a backend bind it.
    REFLECT_MEMBERS(DelegateSignal, "rtti::script::conformance")
    {
        builder.Constructor();
        builder.Method<&DelegateSignal::Connect>("Connect");
        builder.Method<&DelegateSignal::Emit>("Emit");
    }

    // Does the backend's reported API surface contain a type spelled `scriptName`?
    [[nodiscard]] inline bool SurfaceHasType(const Array<ScriptApiType>& surface,
                                             const char* scriptName)
    {
        const StringView wanted(reinterpret_cast<const utf8char*>(scriptName));
        for (const ScriptApiType& type : surface)
        {
            if (StringView(type.scriptName) == wanted)
            {
                return true;
            }
        }
        return false;
    }

    struct CapturedErrors final : IScriptErrorHandler
    {
        int count = 0;
        ScriptErrorKind lastKind = ScriptErrorKind::Compile;
        String lastMessage;
        void OnError(const ScriptError& error) override
        {
            ++count;
            lastKind = error.kind;
            lastMessage = String(error.message);
        }
    };

    inline void RunScriptBackendConformance(const Function<RefPtr<IScriptManager>()>& factory,
                                            const Dialect& dialect)
    {
        // --- manager + two-phase type registration (collect, then finalize) ---
        RegisterCoreTypes(); // self-contained: the introspection diff below
                             // needs the global registry populated (idempotent)
        RefPtr<IScriptManager> manager = factory();
        REQUIRE(manager.Get() != nullptr);
        RegisterReflectedTypes(*manager); // walks the registry + FinalizeTypes()

        // --- context + valid load ---
        RefPtr<IScriptContext> context = manager->CreateContext();
        REQUIRE(context.Get() != nullptr);
        CHECK(context->Load(dialect.functionsModule, u8"conformance.functions").IsOk());

        // --- Variant marshalling: numbers in/out, strings out, missing = NotFound ---
        {
            Variant args[] = {Variant::From(2), Variant::From(3)};
            auto sum = context->Call(u8"add", Span<Variant>{args, 2});
            REQUIRE(sum.HasValue());
            CHECK(sum.Value().Get<f64>() == doctest::Approx(5.0));
        }
        {
            auto text = context->Call(u8"greeting", Span<Variant>{});
            REQUIRE(text.HasValue());
            CHECK(text.Value().Get<String>() == u8"hi");
        }
        CHECK(context->Call(u8"no_such_function", Span<Variant>{}).Error() == ErrorCode::NotFound);

        // --- module globals surface as Variants ---
        CHECK(context->GetGlobal(u8"answer").Get<f64>() == doctest::Approx(42.0));

        // --- script classes: construct with args, invoke methods, state persists.
        // NOTE the contract does NOT promise a later Load preserves an earlier load's
        // globals (some backends replace the module) - only that live ScriptObjects survive. ---
        CHECK(context->Load(dialect.counterClass, u8"conformance.counter").IsOk());
        RefPtr<ScriptObject> counter;
        {
            Variant ctorArgs[] = {Variant::From(10)};
            counter = context->CreateInstance(u8"Counter", Span<Variant>{ctorArgs, 1});
            REQUIRE(counter.Get() != nullptr);
            CHECK(counter->Invoke(u8"increment", Span<Variant>{}).HasValue());
            auto value = counter->Invoke(u8"value", Span<Variant>{});
            REQUIRE(value.HasValue());
            CHECK(value.Value().Get<f64>() == doctest::Approx(11.0));
        }

        // --- error reporting: compile + runtime kinds through the handler ---
        {
            RefPtr<IScriptContext> errorContext = manager->CreateContext();
            CapturedErrors errors;
            errorContext->SetErrorHandler(&errors);
            CHECK_FALSE(errorContext->Load(dialect.compileBroken, u8"conformance.bad").IsOk());
            CHECK(errors.count >= 1);
            CHECK(errors.lastKind == ScriptErrorKind::Compile);
            CHECK(!errors.lastMessage.IsEmpty());
            const int afterCompile = errors.count;
            CHECK_FALSE(errorContext->Load(dialect.runtimeFault, u8"conformance.fault").IsOk());
            CHECK(errors.count > afterCompile);
            CHECK(errors.lastKind == ScriptErrorKind::Runtime);
        }

        // --- context isolation: a second context sees none of the first's state ---
        {
            RefPtr<IScriptContext> other = manager->CreateContext();
            REQUIRE(other.Get() != nullptr);
            CHECK_FALSE(other->HasFunction(u8"add"));
        }

        // --- per-context services: pointer round-trip, unknown = null ---
        {
            int payload = 7;
            context->SetService(u8"conformance.service", &payload);
            CHECK(context->GetService(u8"conformance.service") == &payload);
            CHECK(context->GetService(u8"conformance.unknown") == nullptr);
            context->SetService(u8"conformance.service", nullptr);
            CHECK(context->GetService(u8"conformance.service") == nullptr);
        }

        // --- GC hook: callable any time; live script objects and their state survive ---
        manager->CollectGarbage();
        {
            auto value = counter->Invoke(u8"value", Span<Variant>{});
            REQUIRE(value.HasValue());
            CHECK(value.Value().Get<f64>() == doctest::Approx(11.0));
        }

        // --- Coroutines (certified ONLY when the backend advertises the capability;
        // the flag advertises, this section certifies the behavior) ---
        if (HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines) &&
            !dialect.coroutineClass.IsEmpty())
        {
            const auto progressOf = [](const RefPtr<ScriptObject>& coro) -> f64
            {
                auto p = coro->Invoke(u8"progress", Span<Variant>{});
                REQUIRE(p.HasValue());
                return p.Value().Get<f64>();
            };

            // (1) a timed wait: does NOT complete before ~1s of accumulated advance, DOES after.
            {
                RefPtr<IScriptContext> ctx = manager->CreateContext();
                REQUIRE(ctx.Get() != nullptr);
                CHECK(ctx->Load(dialect.coroutineClass, u8"conformance.coroutine").IsOk());
                RefPtr<ScriptObject> coro = ctx->CreateInstance(u8"Coro", Span<Variant>{});
                REQUIRE(coro.Get() != nullptr);
                REQUIRE(coro->Invoke(u8"begin", Span<Variant>{}).HasValue());
                for (int i = 0; i < 3; ++i)
                {
                    manager->AdvanceCoroutines(0.1);
                } // 0.3s
                CHECK(progressOf(coro) == doctest::Approx(0.0)); // still waiting
                for (int i = 0; i < 12; ++i)
                {
                    manager->AdvanceCoroutines(0.1);
                } // +1.2s past 1.0
                CHECK(progressOf(coro) == doctest::Approx(1.0)); // resumed + ran
            }

            // (2) waitUntil resumes when the predicate flips (not before).
            {
                RefPtr<IScriptContext> ctx = manager->CreateContext();
                CHECK(ctx->Load(dialect.coroutineClass, u8"conformance.coroutine").IsOk());
                RefPtr<ScriptObject> coro = ctx->CreateInstance(u8"Coro", Span<Variant>{});
                REQUIRE(coro.Get() != nullptr);
                REQUIRE(coro->Invoke(u8"beginUntil", Span<Variant>{}).HasValue());
                for (int i = 0; i < 5; ++i)
                {
                    manager->AdvanceCoroutines(0.1);
                }
                CHECK(progressOf(coro) == doctest::Approx(0.0)); // predicate false -> pending
                REQUIRE(coro->Invoke(u8"flip", Span<Variant>{}).HasValue());
                for (int i = 0; i < 3; ++i)
                {
                    manager->AdvanceCoroutines(0.1);
                }
                CHECK(progressOf(coro) == doctest::Approx(1.0)); // predicate flipped -> resumed
            }

            // (3) CancelCoroutinesFor stops a pending coroutine (it never completes after).
            {
                RefPtr<IScriptContext> ctx = manager->CreateContext();
                CHECK(ctx->Load(dialect.coroutineClass, u8"conformance.coroutine").IsOk());
                RefPtr<ScriptObject> coro = ctx->CreateInstance(u8"Coro", Span<Variant>{});
                REQUIRE(coro.Get() != nullptr);
                REQUIRE(coro->Invoke(u8"begin", Span<Variant>{}).HasValue());
                for (int i = 0; i < 3; ++i)
                {
                    manager->AdvanceCoroutines(0.1);
                } // 0.3s, pending
                CHECK(progressOf(coro) == doctest::Approx(0.0));
                manager->CancelCoroutinesFor(*coro);
                for (int i = 0; i < 20; ++i)
                {
                    manager->AdvanceCoroutines(0.1);
                } // 2s must not run
                CHECK(progressOf(coro) == doctest::Approx(0.0)); // cancelled -> never completes
            }
        }

        // --- Backend API introspection: the reflection-vs-backend diff ---
        // Every reflected engine type the manager bound must appear in the surface it
        // reports; a type present in the registry but ABSENT from the surface is a backend
        // that silently failed to bind it. Restrict to types both backends actually bind
        // (constructible, valid identifier, not a primitive / enum / container).
        {
            const Array<ScriptApiType> surface = manager->DescribeBoundApi();
            CHECK_FALSE(surface.IsEmpty()); // a backend that registered types bound something
            int checked = 0;
            for (const TypeInfo* type : GlobalTypeRegistry().All())
            {
                if (type == nullptr || type->name == nullptr)
                {
                    continue;
                }
                const bool bindable = type->constructorCount > 0 && type->enumeratorCount == 0 &&
                                      type->container == nullptr && &TypeOf<f32>() != type &&
                                      &TypeOf<f64>() != type;
                if (!bindable)
                {
                    continue;
                }
                INFO("reflected type absent from backend surface: ", type->name);
                CHECK(SurfaceHasType(surface, type->name));
                ++checked;
            }
            CHECK(checked > 0); // the diff actually exercised some types
            // Spot-check a known type is spelled with its members (not just present).
            for (const ScriptApiType& api : surface)
            {
                if (StringView(api.scriptName) != u8"Float3")
                {
                    continue;
                }
                bool hasDot = false;
                for (const ScriptApiMember& member : api.members)
                {
                    if (StringView(member.name) == u8"Dot")
                    {
                        hasDot = true;
                    }
                }
                CHECK(hasDot); // Float3::Dot is bound and reported
            }
        }

        // --- Script delegates (certified only when the backend declares the capability) ---
        if (HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Delegates) &&
            !dialect.delegateModule.IsEmpty())
        {
            manager->RegisterType(DelegateSignal::StaticType()); // late registration is supported
            RefPtr<IScriptContext> ctx = manager->CreateContext();
            REQUIRE(ctx.Get() != nullptr);
            // Some backends scope reflected foreign types to the "main" module; load there so the
            // script can see DelegateSignal. AngelScript registers types engine-globally, so
            // the chunk name is immaterial to it.
            CHECK(ctx->Load(dialect.delegateModule, u8"main").IsOk());

            Variant signalVar = ctx->GetGlobal(u8"signal");
            REQUIRE(signalVar.IsObject());
            DelegateSignal* signal = signalVar.AsObject<DelegateSignal>();
            REQUIRE(signal != nullptr);

            // (1) native code fires the event -> the subscribed script function runs.
            CHECK(signal->Emit(21.0) == doctest::Approx(42.0));

            // (2) invoke the stored delegate DIRECTLY from C++ and check the returned Variant.
            RefPtr<IScriptDelegate> handler = signal->Handler();
            REQUIRE(handler.Get() != nullptr);
            {
                Variant callArgs[] = {Variant::From<f64>(10.0)};
                Result<Variant> returned = handler->Invoke(Span<Variant>{callArgs, 1});
                REQUIRE(returned.HasValue());
                CHECK(returned.Value().Get<f64>() == doctest::Approx(20.0));
            }

            // (3) the held delegate survives garbage collection.
            manager->CollectGarbage();
            CHECK(signal->Emit(50.0) == doctest::Approx(100.0));
        }

        // --- Overloaded methods (the overloadedName contract): arity families dispatch by
        // argument COUNT, and same-arity type overloads carry distinct script names. Certified on
        // EVERY backend - one script surface, no per-backend overload resolution. ---
        if (!dialect.overloadModule.IsEmpty())
        {
            // The validator: a clean type (arity family + distinct name) has no collision; a
            // same-(name, arity, static) pair without distinct names does. Backend-independent, but
            // asserted here so every backend's suite exercises the contract.
            CHECK(FindScriptMethodNameCollision(Overloads::StaticType()) == nullptr);
            CHECK(FindScriptMethodNameCollision(Colliding::StaticType()) != nullptr);

            manager->RegisterType(Overloads::StaticType()); // late registration, like DelegateSignal
            RefPtr<IScriptContext> octx = manager->CreateContext();
            REQUIRE(octx.Get() != nullptr);
            REQUIRE(octx->Load(dialect.overloadModule, u8"main").IsOk());

            CHECK(octx->GetGlobal(u8"OP0").Get<f64>() == doctest::Approx(1.0));   // ping()  - arity 0
            CHECK(octx->GetGlobal(u8"OP1").Get<f64>() == doctest::Approx(105.0)); // ping(5) - arity 1
            CHECK(octx->GetGlobal(u8"OC").Get<f64>() == doctest::Approx(5.0));    // combine
            CHECK(octx->GetGlobal(u8"OCT").Get<f64>() == doctest::Approx(7.0));   // combineText
        }

        // --- Container members (the write-through contract). ---
        if (!dialect.containerModule.IsEmpty())
        {
            static bool crateOnce = [] {
                RttiRegisterValue_CrateItem();
                RegisterUniquePtrArrayType<CrateItem>();
                return true;
            }();
            (void)crateOnce;
            manager->RegisterType(TypeOf<CrateItem>());
            manager->RegisterType(Crate::StaticType()); // late registration, like DelegateSignal
            RefPtr<IScriptContext> cctx = manager->CreateContext();
            REQUIRE(cctx.Get() != nullptr);
            REQUIRE(cctx->Load(dialect.containerModule, u8"main").IsOk());

            CHECK(cctx->GetGlobal(u8"KN").Get<f64>() == doctest::Approx(2.0)); // two adds
            CHECK(cctx->GetGlobal(u8"KZ").Get<f64>() == doctest::Approx(5.0)); // handle wrote through
            CHECK(cctx->GetGlobal(u8"KM").Get<f64>() == doctest::Approx(9.0)); // move(1, 0)
            CHECK(cctx->GetGlobal(u8"KR").Get<f64>() == doctest::Approx(1.0)); // removeAt(0)
            CHECK(cctx->GetGlobal(u8"KL").Get<f64>() == doctest::Approx(5.0)); // survivor intact
        }

        // --- Gone handles (one rule for every handle): isValid() answers, any other use errors. ---
        if (!dialect.goneModule.IsEmpty())
        {
            Lamp lamp;
            Lamp::Slots()[0].lamp = &lamp;
            manager->RegisterType(Lamp::StaticType()); // late registration, like DelegateSignal
            RefPtr<IScriptContext> gctx = manager->CreateContext();
            REQUIRE(gctx.Get() != nullptr);
            CapturedErrors goneErrors;
            gctx->SetErrorHandler(&goneErrors);
            REQUIRE(gctx->Load(dialect.goneModule, u8"main").IsOk());
            Span<Variant> none{};
            const auto number = [&](const char8_t* function)
            {
                Result<Variant> result = gctx->Call(function, none);
                REQUIRE(result.HasValue());
                const f64* value = result.Value().TryGet<f64>();
                return value != nullptr ? *value : -1.0;
            };
            CHECK(number(u8"valid") == doctest::Approx(1.0));
            CHECK(number(u8"readLevel") == doctest::Approx(2.0));
            CHECK(gctx->Call(u8"writeLevel", none).HasValue());
            CHECK(gctx->Call(u8"bump", none).HasValue());
            CHECK(lamp.level == doctest::Approx(8.0)); // wrote through, then bumped

            Lamp::TakeAway(0);
            CHECK(number(u8"valid") == doctest::Approx(0.0));
            const char8_t* const uses[] = {u8"readLevel", u8"writeLevel", u8"bump"};
            for (const char8_t* use : uses)
            {
                INFO(reinterpret_cast<const char*>(use));
                const int before = goneErrors.count;
                CHECK_FALSE(gctx->Call(use, none).HasValue());
                CHECK(goneErrors.count > before);
                CHECK(goneErrors.lastMessage.AsView().ContainsIgnoreCase(StringView(u8"the object is gone")));
            }
            CHECK(lamp.level == doctest::Approx(8.0)); // nothing wrote into the gone lamp
            gctx->SetErrorHandler(nullptr);
        }

        // --- Committed seams (skipped when the capability is absent - the default). Turning
        // one on later immediately has a certification target here. ---
        if (HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Debugger))
        {
            UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
            REQUIRE(debugger.Get() != nullptr); // a real debugger when declared

            // LIVE certification: set a breakpoint, drive execution, assert it stops at the
            // line (state=Breakpoint), capture the call stack + a known local, step to the
            // next line (state=Stepped), continue to completion (state=Terminated). Headless -
            // the context is driven directly, no editor. (Backends without a debug dialect
            // certify only the factory above.)
            if (!dialect.debugModule.IsEmpty())
            {
                struct StateSink final : IScriptDebuggerListener
                {
                    ScriptDebuggerState last = ScriptDebuggerState::Running;
                    int changes = 0;
                    void OnDebuggerStateChanged(ScriptDebuggerState state) override
                    {
                        last = state;
                        ++changes;
                    }
                } sink;
                debugger->SetListener(&sink);

                RefPtr<IScriptContext> debugCtx = manager->CreateContext();
                REQUIRE(debugCtx.Get() != nullptr);
                REQUIRE(debugCtx->Load(dialect.debugModule, dialect.debugSection).IsOk());
                debugger->SetBreakpoint(dialect.debugSection, dialect.debugBreakLine);

                // The call SUSPENDS at the breakpoint (it does NOT run to completion): the
                // debugger now owns the context, and the listener has seen Breakpoint.
                (void)debugCtx->Call(dialect.debugFunction, Span<Variant>{});
                CHECK(sink.last == ScriptDebuggerState::Breakpoint);

                // Call stack: innermost frame sits on the break line.
                Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
                REQUIRE_FALSE(frames.IsEmpty());
                CHECK(frames[0].line == dialect.debugBreakLine);

                // A known local is captured with its expected display value.
                Array<ScriptVariable> locals = debugger->CaptureLocals(0);
                bool foundLocal = false;
                for (const ScriptVariable& local : locals)
                {
                    if (StringView(local.name) == dialect.debugLocalName)
                    {
                        foundLocal = true;
                        CHECK(StringView(local.value) == dialect.debugLocalValue);
                    }
                }
                CHECK(foundLocal);

                // Step to the next line, then continue to completion.
                debugger->StepOver();
                CHECK(sink.last == ScriptDebuggerState::Stepped);
                debugger->Continue();
                CHECK(sink.last == ScriptDebuggerState::Terminated);
                debugger->SetListener(nullptr);
            }
        }
        else
        {
            CHECK(manager->CreateDebugger().Get() == nullptr); // absent seam: null factory
        }

        if (HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Profiler))
        {
            CHECK(manager->CreateProfiler().Get() != nullptr);
        }
        else
        {
            CHECK(manager->CreateProfiler().Get() == nullptr);
        }

        if (HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Bytecode))
        {
            auto blob = manager->CompileToBlob(u8"", u8"conformance.blob");
            CHECK(blob.HasValue());
        }
        else
        {
            auto blob = manager->CompileToBlob(u8"", u8"conformance.blob");
            CHECK(blob.Error() == ErrorCode::NotSupported); // absent seam: unsupported
        }
    }
}

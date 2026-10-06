// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Luau backend against the shared conformance battery (the certification bar every
// backend meets) with the Luau dialect: metatable-OOP classes, our coroutine primitives
// (startCoroutine / waitSeconds / waitUntil), closures as delegates.
#include <doctest/doctest.h>

#include <chrono>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Script.Tests/BackendConformance.h"

import foundation.core;
import foundation.script;
import foundation.script.luau;

using namespace foundation::core;
using namespace foundation::script;

namespace
{
    constexpr StringView kFunctions = u8R"lua(
function add(a, b)
    return a + b
end
function greeting()
    return "hi"
end
answer = 42
)lua";

    constexpr StringView kCounter = u8R"lua(
Counter = {}
Counter.__index = Counter
function Counter.new(start)
    return setmetatable({ count = start }, Counter)
end
function Counter:increment()
    self.count = self.count + 1
end
function Counter:value()
    return self.count
end
)lua";

    constexpr StringView kCompileBroken = u8R"lua(
function (
)lua";

    constexpr StringView kRuntimeFault = u8R"lua(
error("conformance fault")
)lua";

    constexpr StringView kCoroutine = u8R"lua(
Coro = {}
Coro.__index = Coro
function Coro.new()
    return setmetatable({ p = 0, flag = false }, Coro)
end
function Coro:begin()
    local this = self
    startCoroutine(function()
        waitSeconds(1.0)
        this.p = 1
    end, self)
end
function Coro:beginUntil()
    local this = self
    startCoroutine(function()
        waitUntil(function()
            return this.flag
        end)
        this.p = 1
    end, self)
end
function Coro:flip()
    self.flag = true
end
function Coro:progress()
    return self.p
end
)lua";

    constexpr StringView kDelegate = u8R"lua(
signal = DelegateSignal.new()
signal:Connect(function(v)
    return v * 2
end)
)lua";

    constexpr StringView kOverload = u8R"lua(
local over = Overloads.new()
OP0 = over:ping()
OP1 = over:ping(5)
OC = over:combine(2, 3)
OCT = over:combineText("x", 7)
)lua";

    // Debugger dialect: a zero-arg entry. `tag` binds on line 3, two lines before the breakpoint
    // (line 5) so it is reliably live at the pause - the Luau pause lands one bytecode instruction
    // early (luau_callhook advances savedpc), so a local bound on the line JUST above the break may
    // not be live yet. Luau prints a string local's raw content (no quotes); step lands on line 6,
    // continue completes.
    constexpr StringView kDebug = u8R"lua(gLast = 0
function debugRun()
    local tag = "hit"
    local pad = 1
    local a = 7
    gLast = a + pad
end
)lua";

    // --- enum fixture: a reflected type with an enum property + an enum-arg/return method ------
    enum class Facing : i32
    {
        North = 0,
        East = 1,
        South = 2,
        West = 5 // non-contiguous, so a bare-ordinal assumption would fail
    };

    class Compass : public Object
    {
        RTTI_OBJECT(Compass, Object)
    public:
        Facing facing = Facing::North;
        int heading = 90; // published read-only: a script reads it, never assigns it
        [[nodiscard]] Facing opposite(Facing f) const
        {
            switch (f)
            {
            case Facing::North:
                return Facing::South;
            case Facing::South:
                return Facing::North;
            case Facing::East:
                return Facing::West;
            case Facing::West:
                return Facing::East;
            }
            return Facing::North;
        }
    };

    // A script reads/writes the enum property by the named table and calls the enum method.
    constexpr StringView kEnumProbe = u8R"lua(
Probe = {}
Probe.__index = Probe
function Probe.new()
    return setmetatable({ c = Compass.new() }, Probe)
end
function Probe:readDefault() return self.c.facing end
function Probe:westConst() return Facing.West end
function Probe:setAndRead()
    self.c.facing = Facing.West
    return self.c.facing
end
function Probe:oppositeOfWest()
    return self.c:opposite(Facing.West)
end
function Probe:readHeading() return self.c.heading end
function Probe:writeHeading()
    local ok, err = pcall(function() self.c.heading = 5 end)
    if ok then return 0 end
    return string.find(err, "read%-only") and 1 or 2
end
)lua";

    // A reflected type with a Float3 property + a Float3-arg/return method, to exercise the native
    // vector fast path through properties and methods (not just Float3.new).
    class VecHolder : public Object
    {
        RTTI_OBJECT(VecHolder, Object)
    public:
        Float3 pos = Float3(0.0f, 0.0f, 0.0f);
        [[nodiscard]] Float3 scaledBy(f32 s) const
        {
            return Float3(pos.x * s, pos.y * s, pos.z * s);
        }
    };

    constexpr StringView kVectorProbe = u8R"lua(
VecProbe = {}
VecProbe.__index = VecProbe
function VecProbe.new() return setmetatable({ h = VecHolder.new() }, VecProbe) end
function VecProbe:ctorX() return Float3.new(2, 3, 4).x end
function VecProbe:ctorZ() return Float3.new(2, 3, 4).z end
function VecProbe:arith() local v = Float3.new(1, 2, 3); return (v + v).y end
function VecProbe:setGetZ()
    self.h.pos = Float3.new(5, 6, 7)
    return self.h.pos.z
end
function VecProbe:methodRet()
    self.h.pos = Float3.new(2, 2, 2)
    return self.h:scaledBy(3).x
end
function VecProbe:dot() return Float3.Dot(Float3.new(1, 2, 3), Float3.new(4, 5, 6)) end
)lua";
}

REFLECT_MEMBERS(VecHolder, "rtti::luau::test")
{
    builder.Constructor();
    builder.Property<&VecHolder::pos>("pos");
    builder.Method<&VecHolder::scaledBy>("scaledBy");
}

TEST_CASE("script.luau: Float3 maps to Luau's native vector (fast path - fields, arithmetic, marshalling)")
{
    RegisterCoreTypes(); // reflects Float3 (patches TypeOf<Float3>)

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<Float3>()); // the class table: Float3.new / Float3.Dot / ...
    manager->RegisterType(VecHolder::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(kVectorProbe, u8"luau.vector").IsOk());
    RefPtr<ScriptObject> probe = context->CreateInstance(u8"VecProbe", Span<Variant>{});
    REQUIRE(probe.Get() != nullptr);

    // Float3.new returns a NATIVE vector: .x/.z are Luau's built-in fields, not a metatable.
    CHECK(probe->Invoke(u8"ctorX", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(2.0));
    CHECK(probe->Invoke(u8"ctorZ", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(4.0));
    // Native vector arithmetic (v + v).
    CHECK(probe->Invoke(u8"arith", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(4.0));
    // A Float3 PROPERTY round-trips as a vector (set from a vector, get returns one).
    CHECK(probe->Invoke(u8"setGetZ", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(7.0));
    // A Float3 method ARGUMENT (self) + Float3 RETURN marshal as vectors.
    CHECK(probe->Invoke(u8"methodRet", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(6.0));
    // A vector passed to a reflected static, number returned (1*4 + 2*5 + 3*6).
    CHECK(probe->Invoke(u8"dot", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(32.0));
}

// A facade Array<T> return renders as a native 1-indexed Lua table: a numeric
// array and a reflected-value array (LuauRoom boxes as a userdata element - the Entity flavor).
namespace
{
    struct LuauRoom
    {
        f64 size = 0.0;
    };

    class LuauBag : public Object
    {
        RTTI_OBJECT(LuauBag, Object)
    public:
        [[nodiscard]] Array<i32> numbers() const
        {
            Array<i32> out;
            out.PushBack(10);
            out.PushBack(20);
            out.PushBack(30);
            return out;
        }
        [[nodiscard]] Array<LuauRoom> rooms() const
        {
            Array<LuauRoom> out;
            out.PushBack(LuauRoom{3.0});
            out.PushBack(LuauRoom{4.0});
            return out;
        }
        [[nodiscard]] Array<i32> empty() const { return {}; }
        [[nodiscard]] Array<String> names() const
        {
            Array<String> out;
            out.PushBack(String(u8"ab"));
            out.PushBack(String(u8"cde"));
            return out;
        }
        f32 sum(Array<f32> values) const
        {
            f32 total = 0.0f;
            for (f32 v : values)
            {
                total += v;
            }
            return total;
        }
        i32 letters(Array<String> words) const
        {
            i32 total = 0;
            for (const String& w : words)
            {
                total += static_cast<i32>(w.Size());
            }
            return total;
        }
        Array<f32> doubled(Array<f32> values) const
        {
            for (f32& v : values)
            {
                v *= 2.0f;
            }
            return values;
        }
    };

    constexpr StringView kBagProbe = u8R"lua(
BagProbe = {}
BagProbe.__index = BagProbe
function BagProbe.new() return setmetatable({ b = LuauBag.new() }, BagProbe) end
function BagProbe:nums()
    local ns = self.b:numbers()
    return #ns + ns[1] + ns[2] + ns[3]
end
function BagProbe:roomSizes()
    local rs = self.b:rooms()
    return #rs + rs[1].size + rs[2].size
end
function BagProbe:emptyLen()
    local e = self.b:empty()
    return #e
end
function BagProbe:nameLens()
    local ss = self.b:names()
    return #ss + #ss[1] + #ss[2]
end
function BagProbe:sumOf() return self.b:sum({1.5, 2.25, -0.75}) end
function BagProbe:lettersOf() return self.b:letters({"ab", "cde"}) end
function BagProbe:sumOfNone() return self.b:sum({}) end
function BagProbe:doubledOf()
    local xs = {1.5, 2.25, -0.75}
    local twice = self.b:doubled(xs)
    return #twice + twice[1] + twice[3] + xs[1]
end
)lua";
}
REFLECT_VALUE(LuauRoom, "rtti::luau::test")
{
    builder.Property<&LuauRoom::size>("size");
}
REFLECT_MEMBERS(LuauBag, "rtti::luau::test")
{
    builder.Constructor();
    builder.Method<&LuauBag::numbers>("numbers");
    builder.Method<&LuauBag::rooms>("rooms");
    builder.Method<&LuauBag::empty>("empty");
    builder.Method<&LuauBag::names>("names");
    builder.Method<&LuauBag::sum>("sum", {"values"});
    builder.Method<&LuauBag::letters>("letters", {"words"});
    builder.Method<&LuauBag::doubled>("doubled", {"values"});
}

TEST_CASE("script.luau: a facade Array<T> return crosses as a native table (numeric + value element)")
{
    RttiRegisterValue_LuauRoom();
    RegisterArrayType<i32>();
    RegisterArrayType<LuauRoom>();
    RegisterArrayType<String>();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<LuauRoom>());
    manager->RegisterType(LuauBag::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(kBagProbe, u8"luau.bag").IsOk());
    RefPtr<ScriptObject> probe = context->CreateInstance(u8"BagProbe", Span<Variant>{});
    REQUIRE(probe.Get() != nullptr);

    CHECK(probe->Invoke(u8"nums", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(63.0));
    CHECK(probe->Invoke(u8"roomSizes", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(9.0));
    CHECK(probe->Invoke(u8"emptyLen", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(0.0));
    CHECK(probe->Invoke(u8"nameLens", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(7.0));
}

TEST_CASE("script.luau: a table crosses into a facade Array<T> parameter")
{
    RttiRegisterValue_LuauRoom();
    RegisterArrayType<i32>();
    RegisterArrayType<LuauRoom>();
    RegisterArrayType<String>();
    RegisterArrayType<f32>();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<LuauRoom>());
    manager->RegisterType(LuauBag::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(kBagProbe, u8"luau.bag").IsOk());
    RefPtr<ScriptObject> probe = context->CreateInstance(u8"BagProbe", Span<Variant>{});
    REQUIRE(probe.Get() != nullptr);

    CHECK(probe->Invoke(u8"sumOf", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(3.0));
    CHECK(probe->Invoke(u8"lettersOf", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(5.0));
    CHECK(probe->Invoke(u8"sumOfNone", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(0.0));
    CHECK(probe->Invoke(u8"doubledOf", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(6.0));
}

// Container MEMBERS bind as owner ops (`shelf:rooms_count()` / `:rooms_at(0)` / `:rooms_add()` /
// `:rooms_removeAt(0)` / `:rooms_move(a, b)`) - the AngelScript RegisterContainerMethods twin,
// ZERO-based, write-through. The bare property is NOT exposed: it would cross
// as a detached copy-table whose mutations are silently lost.
namespace
{
    class LuauShelf : public Object
    {
        RTTI_OBJECT(LuauShelf, Object)
    public:
        Array<UniquePtr<LuauRoom>> rooms;
    };
}
REFLECT_MEMBERS(LuauShelf, "rtti::luau::test")
{
    builder.Constructor();
    builder.Nested<&LuauShelf::rooms>("rooms");
}

TEST_CASE("script.luau: container members are owner ops - write-through, zero-based, no bare table")
{
    RttiRegisterValue_LuauRoom();
    RegisterUniquePtrArrayType<LuauRoom>();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<LuauRoom>());
    manager->RegisterType(LuauShelf::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> context = manager->CreateContext();
    // The C++-owned shelf crosses in as a global; script mutations must land in THIS object.
    RefPtr<LuauShelf> shelf = MakeRef<LuauShelf>(DefaultAllocator());
    context->SetGlobal(u8"shelf", Variant::FromObject(RefPtr<Object>(shelf.Get())));

    REQUIRE(context
                ->Load(u8"Bare = (shelf.rooms == nil) and 1 or 0\n" // the copy-table trap is CLOSED
                       u8"local r = shelf:rooms_add()\n"
                       u8"r.size = 42\n"                            // through the returned handle
                       u8"Cnt = shelf:rooms_count()\n"
                       u8"Z = shelf:rooms_at(0).size\n"             // zero-based, like AngelScript
                       u8"OOB = (shelf:rooms_at(5) == nil) and 1 or 0\n",
                       u8"luau.shelf")
                .IsOk());

    CHECK(context->GetGlobal(u8"Bare").Get<f64>() == doctest::Approx(1.0));
    CHECK(context->GetGlobal(u8"Cnt").Get<f64>() == doctest::Approx(1.0));
    CHECK(context->GetGlobal(u8"Z").Get<f64>() == doctest::Approx(42.0));
    CHECK(context->GetGlobal(u8"OOB").Get<f64>() == doctest::Approx(1.0));

    // The mutation is visible from C++ - the handle wrote into the REAL container, not a copy.
    REQUIRE(shelf->rooms.Size() == 1u);
    CHECK(shelf->rooms[0]->size == doctest::Approx(42.0));
}

// The ScriptName alias mechanism on the Luau backend: a "scriptName" class attribute makes the class
// table install under the alias, not the C++ name.
namespace
{
    class LuauAliasProbe : public Object
    {
        RTTI_OBJECT(LuauAliasProbe, Object)
    public:
        static i32 answer() { return 42; }
    };
}
REFLECT_MEMBERS(LuauAliasProbe, "rtti::luau::test")
{
    builder.Attribute("scriptName", "aka"); // install the class table as `aka`, not `LuauAliasProbe`
    builder.Method<&LuauAliasProbe::answer>("answer");
    builder.Constructor();
}

TEST_CASE("script.luau: a scriptName alias installs the class table under the alias, not the C++ name")
{
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(LuauAliasProbe::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(u8"Got = aka.answer()\n", u8"luau.alias").IsOk()); // the ALIAS
    CHECK(context->GetGlobal(u8"Got").Get<f64>() == doctest::Approx(42.0));

    // The C++ name is NOT installed (the alias replaces it): indexing `LuauAliasProbe` (nil) errors.
    RefPtr<IScriptContext> context2 = manager->CreateContext();
    CHECK_FALSE(context2->Load(u8"Got = LuauAliasProbe.answer()\n", u8"luau.alias2").IsOk());
}

TEST_CASE("script.luau: native-vector construct+access throughput (perf case)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<Float3>());
    manager->FinalizeTypes();
    RefPtr<IScriptContext> context = manager->CreateContext();
    // Hot loop: construct a Float3 and read all three fields each iteration - the exact case the
    // native-vector path optimizes (no per-value userdata alloc, native field reads). MEASURED
    // (RelWithDebInfo, 200k iters): ~33.9 ms native vector vs ~89.3 ms boxed - about 2.6x - and the
    // native path additionally ENABLES `v + v` vector arithmetic the boxed handle cannot do at all.
    REQUIRE(context
                ->Load(u8"function hot(n)\n"
                       u8"  local sum = 0.0\n"
                       u8"  for i = 1, n do\n"
                       u8"    local v = Float3.new(1, 2, 3)\n"
                       u8"    sum = sum + v.x + v.y + v.z\n"
                       u8"  end\n"
                       u8"  return sum\n"
                       u8"end\n",
                       u8"luau.perf")
                .IsOk());
    constexpr f64 kIterations = 200000.0;
    Variant args[] = {Variant::From<f64>(kIterations)};
    const auto start = std::chrono::steady_clock::now();
    Result<Variant> result = context->Call(u8"hot", Span<Variant>{args, 1});
    const auto finish = std::chrono::steady_clock::now();
    REQUIRE(result.HasValue());
    CHECK(result.Value().Get<f64>() == doctest::Approx(6.0 * kIterations)); // (1+2+3) per iteration
    const double ms = std::chrono::duration<double, std::milli>(finish - start).count();
    MESSAGE("Float3 construct + 3 field reads x " << static_cast<long>(kIterations) << ": " << ms
            << " ms");
}

TEST_CASE("script.luau: resumable-thread call throughput (perf case - the executor cost)")
{
    // Every script CALL runs on a POOLED lua thread via lua_resume (never lua_pcall on the
    // main state) so the debugger's lua_break can suspend it and production runs share ONE
    // executor with debugging. This records the per-CALL cost of that executor - each
    // Call is a full thread acquire (pooled, no alloc after warmup) + xmove + resume + xmove-back.
    // The claim ("resume-vs-pcall entry, no allocation") is a number, not a hope.
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(u8"function noop(x) return x + 1 end\n", u8"luau.callperf").IsOk());

    constexpr long kCalls = 200000;
    Variant args[] = {Variant::From<f64>(1.0)};
    const auto start = std::chrono::steady_clock::now();
    f64 last = 0.0;
    for (long i = 0; i < kCalls; ++i)
    {
        last = context->Call(u8"noop", Span<Variant>{args, 1}).Value().Get<f64>();
    }
    const auto finish = std::chrono::steady_clock::now();
    CHECK(last == doctest::Approx(2.0)); // the pooled thread really ran the body each time
    const double ms = std::chrono::duration<double, std::milli>(finish - start).count();
    MESSAGE("resumable-thread Call x " << kCalls << ": " << ms << " ms ("
                                       << (ms * 1e6 / static_cast<double>(kCalls)) << " ns/call)");
}

TEST_CASE("script.luau: a compile error reports its source line (not -1)")
{
    struct LineCapture final : IScriptErrorHandler
    {
        i32 line = 0;
        void OnError(const ScriptError& error) override { line = error.line; }
    } capture;

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    context->SetErrorHandler(&capture);

    // A syntax error on line 3 (the "= = =").
    const Status status =
        context->Load(u8"local a = 1\nlocal b = 2\nlocal c = = =\n", u8"linetest");
    CHECK_FALSE(status.IsOk());
    CHECK(capture.line == 3);
}

TEST_CASE("script.luau: LoadBehaviorModule loads each class as its OWN chunk (per-class identity)")
{
    struct Capture final : IScriptErrorHandler
    {
        String module;
        i32 line = 0;
        void OnError(const ScriptError& error) override
        {
            module = String(error.module);
            line = error.line;
        }
    } capture;

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    context->SetErrorHandler(&capture);

    // Two classes; the SECOND has a syntax error on its OWN line 2. Loaded as separate chunks
    // (not concatenated), the error keys on "Bad.luau" (that class's sourceName) at its own line -
    // the (file, line) identity an editor breakpoint needs, not a merged module.
    const BehaviorModuleClass classes[] = {
        {u8"Good.luau", u8"Good = {}\nfunction Good.new() return setmetatable({}, Good) end\n", {}},
        {u8"Bad.luau", u8"Bad = {}\nlocal c = = =\n", {}},
    };
    const Status status =
        context->LoadBehaviorModule(Span<const BehaviorModuleClass>{classes, 2}, u8"behaviors#1");
    CHECK_FALSE(status.IsOk());
    CHECK(capture.module == StringView(u8"Bad.luau")); // the class file, NOT "behaviors#1"
    CHECK(capture.line == 2);                          // its OWN line, not an offset in a merge
}

TEST_CASE("script.luau: step debugger breaks on a breakpoint, captures, and continues")
{
    struct StateCapture final : IScriptDebuggerListener
    {
        ScriptDebuggerState last = ScriptDebuggerState::Running;
        int paused = 0;
        int running = 0;
        void OnDebuggerStateChanged(ScriptDebuggerState s) override
        {
            last = s;
            if (s == ScriptDebuggerState::Breakpoint || s == ScriptDebuggerState::Stepped)
            {
                ++paused;
            }
            else if (s == ScriptDebuggerState::Running)
            {
                ++running;
            }
        }
    } listener;

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Debugger));
    RefPtr<IScriptContext> context = manager->CreateContext();

    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    debugger->SetListener(&listener);
    debugger->SetBreakpoint(u8"dbg.luau", 5); // the marker assignment

    REQUIRE(context
                ->Load(u8"function run(n)\n"           // 1
                       u8"    local doubled = n * 2\n" // 2
                       u8"    local extra = 100\n"     // 3
                       u8"    local sum = doubled + extra\n" // 4
                       u8"    marker = sum\n"          // 5  <- breakpoint
                       u8"    marker = sum + 1\n"      // 6
                       u8"end\n",                      // 7
                       u8"dbg.luau")
                .IsOk());

    Variant args[] = {Variant::From<f64>(21.0)};
    Result<Variant> ran = context->Call(u8"run", Span<Variant>{args, 1});
    REQUIRE(ran.HasValue()); // the break is a void-complete return, never a fault

    // Paused at the breakpoint, BEFORE the marker assignment ran.
    CHECK(listener.paused == 1);
    CHECK(listener.last == ScriptDebuggerState::Breakpoint);
    CHECK(context->GetGlobal(u8"marker").TryGet<f64>() == nullptr);

    // Stack frame keys on (file, line); locals bound earlier are visible with their values.
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE(frames.Size() >= 1);
    CHECK(frames[0].file == StringView(u8"dbg.luau"));
    CHECK(frames[0].line == 5);
    Array<ScriptVariable> locals = debugger->CaptureLocals(0);
    bool sawDoubled = false;
    bool sawExtra = false;
    for (const ScriptVariable& v : locals)
    {
        if (v.name == StringView(u8"doubled"))
        {
            sawDoubled = true;
            CHECK(v.value == StringView(u8"42"));
        }
        else if (v.name == StringView(u8"extra"))
        {
            sawExtra = true;
            CHECK(v.value == StringView(u8"100"));
        }
    }
    CHECK(sawDoubled); // local bound on line 2, live at the pause
    CHECK(sawExtra);   // local bound on line 3, live at the pause

    // Continue runs to completion; lines 5-6 execute, so marker == 143. Exactly ONE break total.
    debugger->Continue();
    CHECK(listener.running >= 1);
    CHECK(listener.paused == 1); // did NOT re-break on the same line while resuming
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(143.0));

    debugger->SetListener(nullptr);
}

// A minimal listener that just counts paused/running transitions (shared shape across the
// step + coroutine tests).
namespace
{
    struct DebuggerStateCounter final : IScriptDebuggerListener
    {
        int paused = 0;
        int running = 0;
        ScriptDebuggerState last = ScriptDebuggerState::Running;
        void OnDebuggerStateChanged(ScriptDebuggerState s) override
        {
            last = s;
            if (s == ScriptDebuggerState::Breakpoint || s == ScriptDebuggerState::Stepped)
            {
                ++paused;
            }
            else if (s == ScriptDebuggerState::Running)
            {
                ++running;
            }
        }
    };
}

TEST_CASE("script.luau: step debugger StepOver stays at the caller depth over a call")
{
    DebuggerStateCounter listener;
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    debugger->SetListener(&listener);
    debugger->SetBreakpoint(u8"step.luau", 6);

    REQUIRE(context
                ->Load(u8"function helper()\n"     // 1
                       u8"    return 7\n"          // 2
                       u8"end\n"                   // 3
                       u8"function run()\n"        // 4
                       u8"    local a = 1\n"       // 5
                       u8"    marker = helper()\n" // 6  <- breakpoint (a call site)
                       u8"    tail = a\n"          // 7
                       u8"end\n",                  // 8
                       u8"step.luau")
                .IsOk());

    Span<Variant> noArgs{};
    REQUIRE(context->Call(u8"run", noArgs).HasValue());
    REQUIRE(listener.paused == 1);
    {
        Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
        REQUIRE(frames.Size() == 1); // only run() on the stack
        CHECK(frames[0].line == 6);
    }

    // Step OVER the helper() call: it must NOT descend into helper, and lands on the next line of
    // run at the SAME depth. The call still executes (marker gets helper()'s result).
    debugger->StepOver();
    REQUIRE(listener.paused == 2);
    CHECK(listener.last == ScriptDebuggerState::Stepped);
    {
        Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
        REQUIRE(frames.Size() == 1); // stayed at the caller depth - helper never held
        CHECK(frames[0].line == 7);  // advanced past the call
    }
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(7.0)); // the call ran

    debugger->Continue();
    CHECK(listener.paused == 2); // no further break
    CHECK(context->GetGlobal(u8"tail").Get<f64>() == doctest::Approx(1.0));
    debugger->SetListener(nullptr);
}

TEST_CASE("script.luau: step debugger StepInto descends into the callee")
{
    DebuggerStateCounter listener;
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    debugger->SetListener(&listener);
    debugger->SetBreakpoint(u8"into.luau", 6);

    REQUIRE(context
                ->Load(u8"function helper()\n"     // 1
                       u8"    local h = 7\n"       // 2
                       u8"    return h\n"          // 3
                       u8"end\n"                   // 4
                       u8"function run()\n"        // 5
                       u8"    local a = 1\n"       // 6  <- breakpoint (a simple line)
                       u8"    marker = helper()\n" // 7  a call site
                       u8"end\n",                  // 8
                       u8"into.luau")
                .IsOk());

    Span<Variant> noArgs{};
    REQUIRE(context->Call(u8"run", noArgs).HasValue());
    REQUIRE(listener.paused == 1);
    {
        Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
        REQUIRE(frames.Size() == 1); // only run() on the stack
        CHECK(frames[0].line == 6);
    }

    // StepInto from a NON-call line advances to the next line at the same depth (line 7, the call).
    debugger->StepInto();
    REQUIRE(listener.paused == 2);
    CHECK(listener.last == ScriptDebuggerState::Stepped);
    {
        Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
        REQUIRE(frames.Size() == 1); // still just run()
        CHECK(frames[0].line == 7);
    }

    // StepInto AT the call descends a frame; the pause lands on helper's first line.
    debugger->StepInto();
    REQUIRE(listener.paused == 3);
    {
        Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
        REQUIRE(frames.Size() == 2); // descended: helper + run
        CHECK(frames[0].file == StringView(u8"into.luau"));
        CHECK(frames[0].line == 2); // first line inside helper
    }

    debugger->Continue();
    CHECK(listener.paused == 3); // helper returns to line 7 (no breakpoint there) - runs to the end
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(7.0));
    debugger->SetListener(nullptr);
}

TEST_CASE("script.luau: step debugger breaks inside a coroutine body, then Continue finishes it")
{
    DebuggerStateCounter listener;
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    debugger->SetListener(&listener);
    debugger->SetBreakpoint(u8"co.luau", 6);

    REQUIRE(context
                ->Load(u8"started = 0\n"                  // 1
                       u8"finished = 0\n"                 // 2
                       u8"function run()\n"               // 3
                       u8"    startCoroutine(function()\n" // 4
                       u8"        started = 1\n"          // 5
                       u8"        marker = 42\n"          // 6  <- breakpoint (in the body)
                       u8"        finished = 1\n"         // 7
                       u8"    end)\n"                     // 8
                       u8"end\n",                         // 9
                       u8"co.luau")
                .IsOk());

    // run() only SCHEDULES the coroutine - it does not run the body, so no break yet.
    Span<Variant> noArgs{};
    REQUIRE(context->Call(u8"run", noArgs).HasValue());
    CHECK(listener.paused == 0);

    // The first advance resumes the coroutine body and hits the breakpoint on its own thread.
    manager->AdvanceCoroutines(0.016);
    REQUIRE(listener.paused == 1);
    CHECK(listener.last == ScriptDebuggerState::Breakpoint);
    CHECK(context->GetGlobal(u8"started").Get<f64>() == doctest::Approx(1.0)); // line 5 ran
    CHECK(context->GetGlobal(u8"finished").Get<f64>() == doctest::Approx(0.0)); // not past line 6
    CHECK(context->GetGlobal(u8"marker").TryGet<f64>() == nullptr);
    {
        Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
        REQUIRE(frames.Size() >= 1);
        CHECK(frames[0].file == StringView(u8"co.luau"));
        CHECK(frames[0].line == 6);
    }

    // The scheduler MUST NOT resume the debugger-held coroutine thread (Fable Q5).
    manager->AdvanceCoroutines(0.016);
    CHECK(listener.paused == 1);
    CHECK(context->GetGlobal(u8"finished").Get<f64>() == doctest::Approx(0.0));

    // Continue finishes the body (lines 6-7) and drops the coroutine from the schedule.
    debugger->Continue();
    CHECK(listener.paused == 1); // no re-break on the resumed line
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(42.0));
    CHECK(context->GetGlobal(u8"finished").Get<f64>() == doctest::Approx(1.0));

    // The coroutine is gone; further advances are no-ops.
    manager->AdvanceCoroutines(0.016);
    CHECK(listener.paused == 1);
    debugger->SetListener(nullptr);
}

TEST_CASE("script.luau: a breakpoint keyed on (file, line) survives a module reload")
{
    DebuggerStateCounter listener;
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    debugger->SetListener(&listener);
    debugger->SetBreakpoint(u8"reload.luau", 3);

    REQUIRE(context
                ->Load(u8"function run()\n"   // 1
                       u8"    local a = 1\n"  // 2
                       u8"    marker = a\n"   // 3  <- breakpoint
                       u8"end\n",             // 4
                       u8"reload.luau")
                .IsOk());

    Span<Variant> noArgs{};
    REQUIRE(context->Call(u8"run", noArgs).HasValue());
    REQUIRE(listener.paused == 1);
    debugger->Continue();
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(1.0));

    // Hot-reload the SAME source file (a changed body, same chunk name). The breakpoint is stored
    // on (file, line) in the debugger, not tied to the module, so it fires again after the reload.
    REQUIRE(context
                ->Load(u8"function run()\n"   // 1
                       u8"    local a = 9\n"  // 2 (changed)
                       u8"    marker = a\n"   // 3  <- breakpoint still here
                       u8"end\n",             // 4
                       u8"reload.luau")
                .IsOk());
    REQUIRE(context->Call(u8"run", noArgs).HasValue());
    CHECK(listener.paused == 2); // broke again against the reloaded module
    debugger->Continue();
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(9.0));
    debugger->SetListener(nullptr);
}

TEST_CASE("script.luau: break in a coroutine, Continue to a wait, the wait still fires on schedule")
{
    DebuggerStateCounter listener;
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RefPtr<IScriptContext> context = manager->CreateContext();
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    debugger->SetListener(&listener);
    debugger->SetBreakpoint(u8"cowait.luau", 5);

    REQUIRE(context
                ->Load(u8"done = 0\n"                      // 1
                       u8"function run()\n"                // 2
                       u8"    startCoroutine(function()\n" // 3
                       u8"        local step = 1\n"        // 4
                       u8"        marker = step\n"         // 5  <- breakpoint (before the wait)
                       u8"        waitSeconds(1.0)\n"      // 6
                       u8"        done = 1\n"              // 7
                       u8"    end)\n"                      // 8
                       u8"end\n",                          // 9
                       u8"cowait.luau")
                .IsOk());

    Span<Variant> noArgs{};
    REQUIRE(context->Call(u8"run", noArgs).HasValue());
    manager->AdvanceCoroutines(0.1); // resumes the body -> breaks at line 5, before the wait
    REQUIRE(listener.paused == 1);
    CHECK(context->GetGlobal(u8"marker").TryGet<f64>() == nullptr);

    // Continue runs marker=step and then waitSeconds(1.0), which yields back to the scheduler -
    // the coroutine is NOT done, it is now waiting.
    debugger->Continue();
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(1.0));
    CHECK(context->GetGlobal(u8"done").Get<f64>() == doctest::Approx(0.0));

    // The 1.0s wait still elapses on schedule (the break did not perturb the timer): ~1.1s of
    // advancing completes it and the coroutine finishes on its own.
    for (int i = 0; i < 11; ++i)
    {
        manager->AdvanceCoroutines(0.1);
    }
    CHECK(context->GetGlobal(u8"done").Get<f64>() == doctest::Approx(1.0));
    CHECK(listener.paused == 1); // no further break
    debugger->SetListener(nullptr);
}

TEST_CASE("script.luau: LoadBehaviorModule consumes cooked bytecode with NO source (the player path)")
{
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());

    // COOK: compile the class to a blob and serialize it exactly as the cook stores
    // ScriptClass::bytecode (CompileToBlob -> Serialize -> the pack bytes).
    Result<RefPtr<IScriptBlob>> blob = manager->CompileToBlob(
        u8"Widget = {}\n"
        u8"Widget.__index = Widget\n"
        u8"function Widget.new() return setmetatable({ v = 41 }, Widget) end\n"
        u8"loaded = Widget.new().v + 1\n", // top-level runs at load -> 42
        u8"Widget.luau");
    REQUIRE(blob.HasValue());
    MemoryStream cooked;
    {
        BinarySerializer writer(cooked, SerializeMode::Write);
        blob.Value()->Serialize(writer);
    }
    const Span<const byte> cookedBytes = cooked.Bytes();
    Array<byte> bytecode;
    bytecode.Reserve(cookedBytes.Size());
    for (usize i = 0; i < cookedBytes.Size(); ++i)
    {
        bytecode.PushBack(cookedBytes[i]);
    }

    // PLAYER: no source at all - the class must load + run from bytecode alone (no luau_compile).
    RefPtr<IScriptContext> context = manager->CreateContext();
    const BehaviorModuleClass classes[] = {
        {u8"Widget.luau", StringView{}, Span<const byte>{bytecode.Data(), bytecode.Size()}},
    };
    REQUIRE(
        context->LoadBehaviorModule(Span<const BehaviorModuleClass>{classes, 1}, u8"behaviors#1")
            .IsOk());
    CHECK(context->GetGlobal(u8"loaded").Get<f64>() == doctest::Approx(42.0));
}

TEST_CASE("script.luau: cooked bytecode stays debuggable - breakpoint + local capture via a consumed blob")
{
    DebuggerStateCounter listener;
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());

    // COOK the debug class to bytecode. CompileToBlob uses debugLevel 2 (matching the runtime
    // source path), so local NAMES survive the cook - consuming bytecode never degrades debugging.
    Result<RefPtr<IScriptBlob>> blob = manager->CompileToBlob(u8"function widgetRun()\n"    // 1
                                                              u8"    local tag = \"hit\"\n" // 2
                                                              u8"    local pad = 1\n"       // 3
                                                              u8"    marker = pad\n" // 4  <- break
                                                              u8"end\n",             // 5
                                                              u8"Widget.luau");
    REQUIRE(blob.HasValue());
    MemoryStream cooked;
    {
        BinarySerializer writer(cooked, SerializeMode::Write);
        blob.Value()->Serialize(writer);
    }
    const Span<const byte> cookedBytes = cooked.Bytes();
    Array<byte> bytecode;
    bytecode.Reserve(cookedBytes.Size());
    for (usize i = 0; i < cookedBytes.Size(); ++i)
    {
        bytecode.PushBack(cookedBytes[i]);
    }

    RefPtr<IScriptContext> context = manager->CreateContext();
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);
    debugger->SetListener(&listener);
    debugger->SetBreakpoint(u8"Widget.luau", 4);

    const BehaviorModuleClass classes[] = {
        {u8"Widget.luau", StringView{}, Span<const byte>{bytecode.Data(), bytecode.Size()}},
    };
    REQUIRE(
        context->LoadBehaviorModule(Span<const BehaviorModuleClass>{classes, 1}, u8"behaviors#1")
            .IsOk());

    Span<Variant> noArgs{};
    REQUIRE(context->Call(u8"widgetRun", noArgs).HasValue());
    REQUIRE(listener.paused == 1);
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].file == StringView(u8"Widget.luau")); // per-class chunk identity, via bytecode
    CHECK(frames[0].line == 4);
    Array<ScriptVariable> locals = debugger->CaptureLocals(0);
    bool sawTag = false;
    for (const ScriptVariable& v : locals)
    {
        if (v.name == StringView(u8"tag"))
        {
            sawTag = true;
            CHECK(v.value == StringView(u8"hit"));
        }
    }
    CHECK(sawTag); // local names survived the cook -> the consumed bytecode is debuggable
    debugger->Continue();
    CHECK(context->GetGlobal(u8"marker").Get<f64>() == doctest::Approx(1.0));
    debugger->SetListener(nullptr);
}

TEST_CASE("script.luau: bytecode capability - compile at cook, serialize, load in the player")
{
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    // Luau ships bytecode (the cook path): the Bytecode capability is declared.
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Bytecode));

    // COOK side: compile source to an opaque blob (luau_compile + a validating load).
    Result<RefPtr<IScriptBlob>> compiled = manager->CompileToBlob(kFunctions, u8"luau.blob");
    REQUIRE(compiled.HasValue());
    REQUIRE(compiled.Value().Get() != nullptr);

    // STORE: the blob serializes to the bytes a cook writes into the pack.
    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        compiled.Value()->Serialize(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    // PLAYER side: reconstruct a blob from the stored bytes with NO compiler present ...
    RefPtr<IScriptBlob> reloaded = manager->CreateBlob();
    REQUIRE(reloaded.Get() != nullptr);
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        reloaded->Serialize(reader);
    }

    // ... and load it into a fresh context (luau_load only, never luau_compile).
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->LoadBlob(*reloaded).IsOk());
    Variant args[] = {Variant::From<f64>(2.0), Variant::From<f64>(3.0)};
    CHECK(context->Call(u8"add", Span<Variant>{args, 2}).Value().Get<f64>() ==
          doctest::Approx(5.0));
    CHECK(context->GetGlobal(u8"answer").Get<f64>() == doctest::Approx(42.0));
}

TEST_CASE("script.luau: CompileToBlob rejects a broken source at cook (never reaches the player)")
{
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    // luau_compile embeds the syntax error in the bytecode; CompileToBlob's validating load
    // catches it so the cook fails here rather than shipping an un-loadable chunk.
    CHECK_FALSE(manager->CompileToBlob(kCompileBroken, u8"luau.broken").HasValue());
}

REFLECT_ENUM(Facing, "rtti::luau::test")
{
    builder.Value("North", Facing::North);
    builder.Value("East", Facing::East);
    builder.Value("South", Facing::South);
    builder.Value("West", Facing::West);
}

REFLECT_MEMBERS(Compass, "rtti::luau::test")
{
    builder.Constructor();
    builder.Property<&Compass::facing>("facing");
    builder.Property<&Compass::heading>("heading", PropertyFlags::ReadOnly);
    builder.Method<&Compass::opposite>("opposite");
}

TEST_CASE("script.luau: reflected enums - named tables + number marshalling both ways")
{
    RttiRegisterEnum_Facing();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<Facing>());
    manager->RegisterType(Compass::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(kEnumProbe, u8"luau.enum").IsOk());
    RefPtr<ScriptObject> probe = context->CreateInstance(u8"Probe", Span<Variant>{});
    REQUIRE(probe.Get() != nullptr);

    // An enum property reads as its underlying number (North = 0).
    CHECK(probe->Invoke(u8"readDefault", Span<Variant>{}).Value().Get<f64>() ==
          doctest::Approx(0.0));
    // The named table exposes each value (non-contiguous West = 5).
    CHECK(probe->Invoke(u8"westConst", Span<Variant>{}).Value().Get<f64>() ==
          doctest::Approx(5.0));
    // Setting the property via the named table round-trips back through the getter.
    CHECK(probe->Invoke(u8"setAndRead", Span<Variant>{}).Value().Get<f64>() ==
          doctest::Approx(5.0));
    // An enum argument AND an enum return marshal through a method (opposite(West) = East = 1).
    CHECK(probe->Invoke(u8"oppositeOfWest", Span<Variant>{}).Value().Get<f64>() ==
          doctest::Approx(1.0));
}

TEST_CASE("script.luau: a read-only property reads, and an assignment raises instead of silently dropping")
{
    RttiRegisterEnum_Facing();

    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<Facing>());
    manager->RegisterType(Compass::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(kEnumProbe, u8"luau.readonly").IsOk());
    RefPtr<ScriptObject> probe = context->CreateInstance(u8"Probe", Span<Variant>{});
    REQUIRE(probe.Get() != nullptr);

    CHECK(probe->Invoke(u8"readHeading", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(90.0));
    // 1 = the assignment raised naming the property read-only (0 = silently accepted, 2 = another error).
    CHECK(probe->Invoke(u8"writeHeading", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(1.0));
    CHECK(probe->Invoke(u8"readHeading", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(90.0));
}

TEST_CASE("script.luau: backend conformance battery")
{
    conformance::Dialect dialect;
    dialect.languageId = u8"luau";
    dialect.functionsModule = kFunctions;
    dialect.counterClass = kCounter;
    dialect.compileBroken = kCompileBroken;
    dialect.runtimeFault = kRuntimeFault;
    dialect.coroutineClass = kCoroutine;
    dialect.delegateModule = kDelegate;
    dialect.overloadModule = kOverload;
    dialect.containerModule = u8"crate = Crate.new()\n"
                              u8"local a = crate:items_add()\n"
                              u8"a.size = 5\n"
                              u8"local b = crate:items_add()\n"
                              u8"b.size = 9\n"
                              u8"KN = crate:items_count()\n"
                              u8"KZ = crate:items_at(0).size\n"
                              u8"crate:items_move(1, 0)\n"
                              u8"KM = crate:items_at(0).size\n"
                              u8"crate:items_removeAt(0)\n"
                              u8"KR = crate:items_count()\n"
                              u8"KL = crate:items_at(0).size\n";
    dialect.debugModule = kDebug;
    dialect.debugSection = u8"debug.luau";
    dialect.debugFunction = u8"debugRun";
    dialect.debugBreakLine = 5;
    dialect.debugLocalName = u8"tag";
    dialect.debugLocalValue = u8"hit"; // Luau prints a string local's raw content (no quotes)

    conformance::RunScriptBackendConformance([]() { return CreateLuauScriptManager(DefaultAllocator()); },
                                             dialect);
}

TEST_CASE("script.luau: .d.luau declaration emitter - typed surface for luau-analyze")
{
    RegisterCoreTypes();       // Float3 (-> native vector)
    RttiRegisterEnum_Facing(); // the Facing enum

    const TypeInfo* types[] = {&TypeOf<Float3>(), &VecHolder::StaticType(), &TypeOf<Facing>(),
                               &Compass::StaticType()};
    const String decls = EmitLuauDeclarations(Span<const TypeInfo* const>{types, 4});

    const StringView text = decls.AsView();
    auto has = [text](StringView needle)
    {
        if (needle.Size() > text.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= text.Size(); ++i)
        {
            if (text.SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    };

    // Float3: an instance class + a value table; its constructors emit as an overload intersection
    // so both Float3.new() and Float3.new(x, y, z) type-check; a static reads the native vector.
    CHECK(has(u8"declare class Float3"));
    CHECK(has(u8"declare Float3: {"));
    CHECK(has(u8"new: (() -> Float3) & ("));    // >1 constructor -> intersection of overloads
    CHECK(has(u8"Dot: (arg0: vector, arg1: vector) -> number")); // static over native vectors
    // VecHolder: a Float3 property maps to `vector`; scaledBy(f32)->Float3 typed self+param+ret.
    CHECK(has(u8"declare class VecHolder"));
    CHECK(has(u8"pos: vector"));
    CHECK(has(u8"function scaledBy(self, arg0: number): vector")); // param name unreflected -> arg0
    CHECK(has(u8"new: () -> VecHolder"));
    // The enum surfaces as a named number table (non-contiguous West present by name).
    CHECK(has(u8"declare Facing: {"));
    CHECK(has(u8"    read heading: number")); // a read-only property carries Luau's read modifier
    CHECK(has(u8"    facing: number"));
    CHECK(has(u8"West: number"));
}

TEST_CASE("script.luau: an instance's own fields read as properties, its methods do not")
{
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->FinalizeTypes();
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(u8"Counter = {}\n"
                          u8"Counter.__index = Counter\n"
                          u8"function Counter.new() return setmetatable({ n = 2 }, Counter) end\n"
                          u8"function Counter:add(x) self.n = self.n + x end\n",
                          u8"luau.counter")
                .IsOk());
    RefPtr<ScriptObject> counter = context->CreateInstance(u8"Counter", Span<Variant>{});
    REQUIRE(counter.Get() != nullptr);
    Variant args[] = {Variant::From<f64>(3.0)};
    REQUIRE(counter->Invoke(u8"add", Span<Variant>{args, 1}).HasValue());
    CHECK(counter->GetProperty(u8"n").Value().Get<f64>() == doctest::Approx(5.0));
    CHECK_FALSE(counter->GetProperty(u8"add").HasValue());
    CHECK_FALSE(counter->GetProperty(u8"missing").HasValue());
}

// Two asset ids compare in a script, through the reflected Equals as __eq.
TEST_CASE("script.luau: Guids compare with == and ~=")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<Guid>());
    manager->FinalizeTypes();
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(u8R"lua(
Ids = {}
Ids.__index = Ids
function Ids.new() return setmetatable({}, Ids) end
function Ids:same() return Guid.new("6b0a7709-5169-4a0e-8077-c78b87a132e6") == Guid.new("6b0a7709-5169-4a0e-8077-c78b87a132e6") end
function Ids:differ() return Guid.new("6b0a7709-5169-4a0e-8077-c78b87a132e6") ~= Guid.new("0758e8d2-c6cd-47a7-bce6-9728a9501018") end
)lua",
                          u8"luau.ids")
                .IsOk());
    RefPtr<ScriptObject> ids = context->CreateInstance(u8"Ids", Span<Variant>{});
    REQUIRE(ids.Get() != nullptr);
    CHECK(ids->Invoke(u8"same", Span<Variant>{}).Value().Get<bool>());
    CHECK(ids->Invoke(u8"differ", Span<Variant>{}).Value().Get<bool>());
}

// Sedulous 22a73e31: the reflected operators bind as metamethods on the reflected value types
// (Float3 is Luau's native vector, which has its own): a + b, v * 2, -v, a == b, q * r, and
// Luau's a += b through __add.
TEST_CASE("script.luau: reflected operators bind as metamethods")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    manager->RegisterType(TypeOf<Float2>());
    manager->RegisterType(TypeOf<Color>());
    manager->RegisterType(TypeOf<Quaternion>());
    manager->FinalizeTypes();
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->Load(u8R"lua(
Ops = {}
Ops.__index = Ops
function Ops.new() return setmetatable({}, Ops) end
function Ops:sumY() return (Float2.new(1, 2) + Float2.new(3, 4)).y end
function Ops:scaled() return (Float2.new(1, 2) * 3).y end
function Ops:negated() return (-Float2.new(1, 2)).x end
function Ops:same() return Float2.new(1, 2) == Float2.new(1, 2) end
function Ops:differ() return Float2.new(1, 2) ~= Float2.new(2, 1) end
function Ops:moved() local p = Float2.new(1, 1); p += Float2.new(2, 3); return p.y end
function Ops:hue() return (Color.new(0.25, 0, 0, 1) + Color.new(0.5, 0, 0, 0)).r end
function Ops:qw() return (Quaternion.new(0, 0, 0, 1) * Quaternion.new(0, 0, 0, 1)).w end
)lua",
                          u8"luau.ops")
                .IsOk());
    RefPtr<ScriptObject> ops = context->CreateInstance(u8"Ops", Span<Variant>{});
    REQUIRE(ops.Get() != nullptr);
    const auto call = [&](StringView method) { return ops->Invoke(method, Span<Variant>{}); };
    CHECK(call(u8"sumY").Value().Get<f64>() == doctest::Approx(6.0));
    CHECK(call(u8"scaled").Value().Get<f64>() == doctest::Approx(6.0));
    CHECK(call(u8"negated").Value().Get<f64>() == doctest::Approx(-1.0));
    CHECK(call(u8"same").Value().Get<bool>());
    CHECK(call(u8"differ").Value().Get<bool>());
    CHECK(call(u8"moved").Value().Get<f64>() == doctest::Approx(4.0));
    CHECK(call(u8"hue").Value().Get<f64>() == doctest::Approx(0.75));
    CHECK(call(u8"qw").Value().Get<f64>() == doctest::Approx(1.0));

    // script_api lists them as operators.
    bool listed = false;
    for (const ScriptApiType& type : manager->DescribeBoundApi())
    {
        if (type.scriptName != u8"Float2")
        {
            continue;
        }
        for (const ScriptApiMember& member : type.members)
        {
            listed = listed || (member.kind == ScriptApiMemberKind::Operator &&
                                member.signature == u8"Float2 * number -> Float2 (and *=)");
        }
    }
    CHECK(listed);
}

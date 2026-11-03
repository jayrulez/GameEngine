// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The plugin host: which of the plugins it knows it loads for a
// project's enabled ids and its role, in what order, which it refuses and why, and that unload
// reverses everything. The plugins here are linked into the test binary the way the generated
// registry links them, plus one development package built as a shared library.

#include <doctest/doctest.h>

#include <initializer_list>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.runtime;

using namespace foundation::core;
using namespace foundation::runtime;

#ifndef TEST_PLUGIN_PART_PATH
#error "Runtime.Tests must define TEST_PLUGIN_PART_PATH (see Runtime.Tests/CMakeLists.txt)"
#endif

namespace hostprobe
{
    // A type a part registers, to show the recording reverses it on unload.
    class ProbeSettings final : public ISerializable
    {
        RTTI_OBJECT(ProbeSettings, ISerializable)
    public:
        void Serialize(ISerializer&) override {}
    };
    RTTI_DEFINE_OBJECT(ProbeSettings, "rtti::hostprobe")

    // A subsystem a part adds and its unload removes.
    class ProbeSubsystem final : public Subsystem
    {
    };
}

namespace
{
    // Every load and unload, as "load:<id>:<Kind>" and "unload:<id>:<Kind>", in call order.
    Array<String>& Log()
    {
        static Array<String> log;
        return log;
    }

    void Record(StringView what, PluginContext& context, PluginPartKind kind)
    {
        String line(what);
        line.Append(u8":");
        line.Append(context.PluginId());
        line.Append(u8":");
        line.Append(PluginPartKindName(kind));
        Log().PushBack(Move(line));
    }

    template <PluginPartKind Kind>
    void LoadLogged(PluginContext& context)
    {
        Record(u8"load", context, Kind);
    }
    template <PluginPartKind Kind>
    void UnloadLogged(PluginContext& context)
    {
        Record(u8"unload", context, Kind);
    }

    // The part of a plugin `Id` of kind `Kind`, constant like every generated entry's part.
    template <const char8_t* Id, PluginPartKind Kind>
    const PluginPart* Part()
    {
        static constexpr PluginPart part{Id, Kind, &LoadLogged<Kind>, &UnloadLogged<Kind>};
        return &part;
    }

    constexpr char8_t kAlpha[] = u8"alpha";
    constexpr char8_t kBeta[] = u8"beta";
    constexpr char8_t kGamma[] = u8"gamma";
    constexpr char8_t kLocal[] = u8"online_local";
    constexpr char8_t kSteam[] = u8"online_steam";
    constexpr char8_t kOld[] = u8"old";
    constexpr char8_t kLoopA[] = u8"loop_a";
    constexpr char8_t kLoopB[] = u8"loop_b";
    constexpr char8_t kImpostor[] = u8"impostor";
    constexpr char8_t kRegistrar[] = u8"registrar";

    // The registrar's Engine part registers a serializable type and adds a subsystem.
    void LoadRegistrar(PluginContext& context)
    {
        RegisterSerializable<hostprobe::ProbeSettings>(context.Serializables());
        context.Subsystems().AddSubsystem<hostprobe::ProbeSubsystem>();
        Record(u8"load", context, PluginPartKind::Engine);
    }
    void UnloadRegistrar(PluginContext& context)
    {
        context.Subsystems().RemoveSubsystem<hostprobe::ProbeSubsystem>();
        Record(u8"unload", context, PluginPartKind::Engine);
    }
    const PluginPart* RegistrarEngine()
    {
        static constexpr PluginPart part{kRegistrar, PluginPartKind::Engine, &LoadRegistrar, &UnloadRegistrar};
        return &part;
    }

    struct ManifestSpec
    {
        StringView id;
        StringView version = u8"0.1.0";
        StringView engineVersion = u8"0.1";
        Array<PluginDependency> dependsOn{};
        Array<String> provides{};
        Array<PluginPartKind> parts{};
    };

    [[nodiscard]] Array<byte> ManifestXml(ManifestSpec spec)
    {
        PluginManifest manifest;
        manifest.id = String(spec.id);
        manifest.name = String(spec.id);
        manifest.version = String(spec.version);
        manifest.engineVersion = String(spec.engineVersion);
        manifest.dependsOn = Move(spec.dependsOn);
        manifest.provides = Move(spec.provides);
        for (PluginPartKind kind : spec.parts)
        {
            manifest.parts.PushBack(PluginManifestPart{String(PluginPartKindName(kind))});
        }
        MemoryStream buffer;
        REQUIRE(WritePluginManifest(manifest, buffer).IsOk());
        Array<byte> bytes;
        for (byte b : buffer.Bytes())
        {
            bytes.PushBack(b);
        }
        bytes.PushBack(byte{0}); // the registry hands the manifest over as a C string
        return bytes;
    }

    [[nodiscard]] Array<PluginDependency> DependsOn(StringView id, StringView version = u8"0.1")
    {
        Array<PluginDependency> dependsOn;
        dependsOn.PushBack(PluginDependency{String(id), String(version)});
        return dependsOn;
    }

    [[nodiscard]] Array<String> Provides(StringView slot)
    {
        Array<String> provides;
        provides.PushBack(String(slot));
        return provides;
    }

    [[nodiscard]] Array<PluginPartKind> Parts(std::initializer_list<PluginPartKind> kinds)
    {
        Array<PluginPartKind> parts;
        for (PluginPartKind kind : kinds)
        {
            parts.PushBack(kind);
        }
        return parts;
    }

    // The plugins this test binary links, as the generated registry would list them.
    struct Linked
    {
        Array<Array<byte>> manifests;
        Array<Array<PluginPartEntryFn>> entries;
        Array<LinkedPlugin> plugins;

        void Add(ManifestSpec spec, std::initializer_list<PluginPartEntryFn> parts)
        {
            manifests.PushBack(ManifestXml(Move(spec)));
            Array<PluginPartEntryFn> list;
            for (PluginPartEntryFn entry : parts)
            {
                list.PushBack(entry);
            }
            entries.PushBack(Move(list));
        }

        [[nodiscard]] Span<const LinkedPlugin> List()
        {
            plugins.Clear();
            for (usize i = 0; i < manifests.Size(); ++i)
            {
                plugins.PushBack(LinkedPlugin{reinterpret_cast<const char8_t*>(manifests[i].Data()), entries[i].Data(),
                                              entries[i].Size()});
            }
            return Span<const LinkedPlugin>(plugins.Data(), plugins.Size());
        }
    };

    // alpha (all four parts) <- beta (Engine) <- gamma (Engine, Pipeline); two online backends
    // for one slot; one built for an older engine; a dependency cycle; an impostor whose entry is
    // another plugin's part; the registrar.
    void AddStandardPlugins(Linked& linked)
    {
        linked.Add({.id = kAlpha,
                    .parts = Parts({PluginPartKind::Engine, PluginPartKind::Pipeline, PluginPartKind::Tooling,
                                    PluginPartKind::Editor})},
                   {&Part<kAlpha, PluginPartKind::Engine>, &Part<kAlpha, PluginPartKind::Pipeline>,
                    &Part<kAlpha, PluginPartKind::Tooling>, &Part<kAlpha, PluginPartKind::Editor>});
        linked.Add({.id = kBeta, .dependsOn = DependsOn(kAlpha), .parts = Parts({PluginPartKind::Engine})},
                   {&Part<kBeta, PluginPartKind::Engine>});
        linked.Add({.id = kGamma, .dependsOn = DependsOn(kBeta),
                    .parts = Parts({PluginPartKind::Engine, PluginPartKind::Pipeline})},
                   {&Part<kGamma, PluginPartKind::Engine>, &Part<kGamma, PluginPartKind::Pipeline>});
        linked.Add({.id = kLocal, .provides = Provides(u8"online.backend"), .parts = Parts({PluginPartKind::Engine})},
                   {&Part<kLocal, PluginPartKind::Engine>});
        linked.Add({.id = kSteam, .provides = Provides(u8"online.backend"), .parts = Parts({PluginPartKind::Engine})},
                   {&Part<kSteam, PluginPartKind::Engine>});
        linked.Add({.id = kOld, .engineVersion = u8"0.0", .parts = Parts({PluginPartKind::Engine})},
                   {&Part<kOld, PluginPartKind::Engine>});
        linked.Add({.id = kLoopA, .dependsOn = DependsOn(kLoopB), .parts = Parts({PluginPartKind::Engine})},
                   {&Part<kLoopA, PluginPartKind::Engine>});
        linked.Add({.id = kLoopB, .dependsOn = DependsOn(kLoopA), .parts = Parts({PluginPartKind::Engine})},
                   {&Part<kLoopB, PluginPartKind::Engine>});
        linked.Add({.id = kImpostor, .parts = Parts({PluginPartKind::Engine})}, {&Part<kAlpha, PluginPartKind::Engine>});
        linked.Add({.id = kRegistrar, .parts = Parts({PluginPartKind::Engine})}, {&RegistrarEngine});
    }

    [[nodiscard]] Array<String> Strings(std::initializer_list<const char8_t*> lines)
    {
        Array<String> out;
        for (const char8_t* line : lines)
        {
            out.PushBack(String(StringView(line)));
        }
        return out;
    }

    [[nodiscard]] bool Same(const Array<String>& a, const Array<String>& b)
    {
        if (a.Size() != b.Size())
        {
            return false;
        }
        for (usize i = 0; i < a.Size(); ++i)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool RefusedWith(const PluginLoadReport& report, StringView id, StringView reason)
    {
        const PluginRefusal* refusal = report.FindRefusal(id);
        return refusal != nullptr && refusal->reason.AsView().Contains(reason);
    }

    [[nodiscard]] StringView PartLibraryPath()
    {
        return StringView{reinterpret_cast<const utf8char*>(TEST_PLUGIN_PART_PATH)};
    }
}

TEST_CASE("plugin host: dependencies load first and unload last, whatever order the project lists them in")
{
    Log().Clear();
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    AddStandardPlugins(linked);
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(linked.List()).IsOk());

    const StringView enabled[] = {kGamma, kBeta, kAlpha};
    const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 3));
    CHECK(report.refused.IsEmpty());
    CHECK(Same(report.loaded, Strings({kAlpha, kBeta, kGamma})));
    // A player loads Engine parts only: alpha's Pipeline, Tooling and Editor parts stay out.
    CHECK(Same(Log(), Strings({u8"load:alpha:Engine", u8"load:beta:Engine", u8"load:gamma:Engine"})));

    host.Unload();
    CHECK(Same(Log(), Strings({u8"load:alpha:Engine", u8"load:beta:Engine", u8"load:gamma:Engine",
                               u8"unload:gamma:Engine", u8"unload:beta:Engine", u8"unload:alpha:Engine"})));
    CHECK_FALSE(host.IsLoaded(kAlpha));
}

TEST_CASE("plugin host: each role loads the parts the table gives it, in kind order")
{
    Linked linked;
    AddStandardPlugins(linked);
    const StringView enabled[] = {kAlpha};
    struct Case
    {
        HostRole role;
        Array<String> expected;
    };
    Case cases[] = {
        {HostRole::Player, Strings({u8"load:alpha:Engine"})},
        {HostRole::Cook, Strings({u8"load:alpha:Engine", u8"load:alpha:Pipeline"})},
        {HostRole::Export, Strings({u8"load:alpha:Engine", u8"load:alpha:Pipeline", u8"load:alpha:Tooling"})},
        {HostRole::Mcp, Strings({u8"load:alpha:Engine", u8"load:alpha:Pipeline", u8"load:alpha:Tooling"})},
        {HostRole::Editor, Strings({u8"load:alpha:Engine", u8"load:alpha:Pipeline", u8"load:alpha:Tooling",
                                    u8"load:alpha:Editor"})},
    };
    for (Case& test : cases)
    {
        CAPTURE(HostRoleName(test.role));
        Log().Clear();
        SubsystemManager subsystems(DefaultAllocator());
        PluginHost host(DefaultAllocator(), subsystems, test.role, u8"0.1.0");
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 1));
        CHECK(report.loaded.Size() == 1u);
        CHECK(Same(Log(), test.expected));
    }
}

TEST_CASE("plugin host: a missing or refused dependency disables its dependents, each with a message")
{
    Log().Clear();
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    AddStandardPlugins(linked);
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(linked.List()).IsOk());

    // alpha is not enabled: beta needs it, and gamma needs beta.
    const StringView enabled[] = {kBeta, kGamma, kLocal};
    const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 3));
    CHECK(Same(report.loaded, Strings({kLocal})));
    CHECK(RefusedWith(report, kBeta, u8"needs 'alpha', which is not enabled"));
    CHECK(RefusedWith(report, kGamma, u8"needs 'beta', which was refused"));
    CHECK(Same(Log(), Strings({u8"load:online_local:Engine"})));
}

TEST_CASE("plugin host: a dependency older than asked is refused")
{
    Log().Clear();
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    linked.Add({.id = kAlpha, .version = u8"0.1.0", .parts = Parts({PluginPartKind::Engine})},
               {&Part<kAlpha, PluginPartKind::Engine>});
    linked.Add({.id = kBeta, .dependsOn = DependsOn(kAlpha, u8"0.2"), .parts = Parts({PluginPartKind::Engine})},
               {&Part<kBeta, PluginPartKind::Engine>});
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(linked.List()).IsOk());
    const StringView enabled[] = {kAlpha, kBeta};
    const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 2));
    CHECK(Same(report.loaded, Strings({kAlpha})));
    CHECK(RefusedWith(report, kBeta, u8"needs 'alpha' 0.2 or later; it is 0.1.0"));
}

TEST_CASE("plugin host: one provider per slot, the first enabled keeps it")
{
    Log().Clear();
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    AddStandardPlugins(linked);
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(linked.List()).IsOk());
    const StringView enabled[] = {kSteam, kLocal};
    const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 2));
    CHECK(Same(report.loaded, Strings({kSteam})));
    CHECK(RefusedWith(report, kLocal, u8"provides 'online.backend', which 'online_steam' already provides"));
}

TEST_CASE("plugin host: refused before anything loads - unknown, wrong engine, cycle, impostor entry")
{
    Log().Clear();
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    AddStandardPlugins(linked);
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(linked.List()).IsOk());
    const StringView enabled[] = {u8"nowhere", kOld, kLoopA, kLoopB, kImpostor};
    const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 5));
    CHECK(report.loaded.IsEmpty());
    CHECK(RefusedWith(report, u8"nowhere", u8"is not in this build"));
    CHECK(RefusedWith(report, kOld, u8"needs engine 0.0; this engine is 0.1.0"));
    CHECK(RefusedWith(report, kLoopA, u8"dependency cycle"));
    CHECK(RefusedWith(report, kLoopB, u8"dependency cycle"));
    CHECK(RefusedWith(report, kImpostor, u8"entry is not this plugin's"));
    CHECK(Log().IsEmpty());
}

TEST_CASE("plugin host: a part the manifest lists but the build lacks is refused for the roles that load it")
{
    Linked linked;
    // Lists a Pipeline part but links only its Engine part, as a player build would.
    linked.Add({.id = kAlpha, .parts = Parts({PluginPartKind::Engine, PluginPartKind::Pipeline})},
               {&Part<kAlpha, PluginPartKind::Engine>});
    const StringView enabled[] = {kAlpha};
    {
        Log().Clear();
        SubsystemManager subsystems(DefaultAllocator());
        PluginHost player(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(player.AddLinked(linked.List()).IsOk());
        CHECK(player.Load(Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
    }
    {
        Log().Clear();
        SubsystemManager subsystems(DefaultAllocator());
        PluginHost cook(DefaultAllocator(), subsystems, HostRole::Cook, u8"0.1.0");
        REQUIRE(cook.AddLinked(linked.List()).IsOk());
        const PluginLoadReport report = cook.Load(Span<const StringView>(enabled, 1));
        CHECK(RefusedWith(report, kAlpha, u8"its Pipeline part is not in this build"));
        CHECK(Log().IsEmpty());
    }
}

TEST_CASE("plugin host: a dormant plugin registers nothing; a loaded one's registrations are reversed on unload")
{
    Log().Clear();
    const TypeId probe = hostprobe::ProbeSettings::StaticType().id;
    REQUIRE_FALSE(GlobalSerializableRegistry().Contains(probe));
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    AddStandardPlugins(linked);
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        const StringView enabled[] = {kAlpha};
        CHECK(host.Load(Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
        // The registrar is linked but not enabled: nothing of it ran.
        CHECK_FALSE(GlobalSerializableRegistry().Contains(probe));
        CHECK_FALSE(subsystems.HasSubsystem<hostprobe::ProbeSubsystem>());
    }
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        const StringView enabled[] = {kRegistrar};
        CHECK(host.Load(Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
        CHECK(GlobalSerializableRegistry().Contains(probe));
        CHECK(subsystems.HasSubsystem<hostprobe::ProbeSubsystem>());
        host.Unload();
        CHECK_FALSE(GlobalSerializableRegistry().Contains(probe)); // reversed by the recording
        CHECK_FALSE(subsystems.HasSubsystem<hostprobe::ProbeSubsystem>()); // removed by its unload
    }
}

TEST_CASE("plugin host: a development package's part loads from its shared library; a linked copy wins")
{
    Log().Clear();
    SubsystemManager subsystems(DefaultAllocator());
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        UniquePtr<PluginManifest> manifest = MakeUnique<PluginManifest>(DefaultAllocator());
        manifest->id = String(u8"shared_probe");
        manifest->version = String(u8"0.1.0");
        manifest->engineVersion = String(u8"0.1");
        manifest->parts.PushBack(PluginManifestPart{String(u8"Engine")});
        const PluginLibrary libraries[] = {{PluginPartKind::Engine, String(PartLibraryPath())}};
        host.AddShared(Move(manifest), Span<const PluginLibrary>(libraries, 1));
        const StringView enabled[] = {u8"shared_probe"};
        const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 1));
        CHECK(report.refused.IsEmpty());
        REQUIRE(report.loaded.Size() == 1u);

        // The library's part counts its loads through a C symbol it exports.
        DynamicLibrary probe{PartLibraryPath()};
        REQUIRE(probe.IsLoaded());
        const auto loads = probe.GetSymbol<int (*)()>(u8"TestPluginPartLoads");
        REQUIRE(loads != nullptr);
        CHECK(loads() == 1);
        host.Unload();
        CHECK(loads() == 0);
    }
    {
        // The same id linked into the binary and added as a package: the linked part loads.
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        Linked linked;
        linked.Add({.id = kAlpha, .parts = Parts({PluginPartKind::Engine})}, {&Part<kAlpha, PluginPartKind::Engine>});
        UniquePtr<PluginManifest> manifest = MakeUnique<PluginManifest>(DefaultAllocator());
        manifest->id = String(kAlpha);
        manifest->version = String(u8"0.1.0");
        manifest->engineVersion = String(u8"0.1");
        manifest->parts.PushBack(PluginManifestPart{String(u8"Engine")});
        const PluginLibrary libraries[] = {{PluginPartKind::Engine, String(u8"./no-such-library.so")}};
        host.AddShared(Move(manifest), Span<const PluginLibrary>(libraries, 1));
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        const StringView enabled[] = {kAlpha};
        CHECK(host.Load(Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
        CHECK(Same(Log(), Strings({u8"load:alpha:Engine"})));
    }
}

TEST_CASE("plugin host: a shared part that does not load is refused with the path")
{
    SubsystemManager subsystems(DefaultAllocator());
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    UniquePtr<PluginManifest> manifest = MakeUnique<PluginManifest>(DefaultAllocator());
    manifest->id = String(u8"absent");
    manifest->version = String(u8"0.1.0");
    manifest->engineVersion = String(u8"0.1");
    manifest->parts.PushBack(PluginManifestPart{String(u8"Engine")});
    const PluginLibrary libraries[] = {{PluginPartKind::Engine, String(u8"./no-such-library.so")}};
    host.AddShared(Move(manifest), Span<const PluginLibrary>(libraries, 1));
    const StringView enabled[] = {u8"absent"};
    const PluginLoadReport report = host.Load(Span<const StringView>(enabled, 1));
    CHECK(RefusedWith(report, u8"absent", u8"did not load from './no-such-library.so'"));
}

TEST_CASE("plugin host: the roles' part table")
{
    for (HostRole role : kHostRoles)
    {
        CHECK(RoleLoads(role, PluginPartKind::Engine));
    }
    CHECK_FALSE(RoleLoads(HostRole::Player, PluginPartKind::Pipeline));
    CHECK(RoleLoads(HostRole::Cook, PluginPartKind::Pipeline));
    CHECK_FALSE(RoleLoads(HostRole::Cook, PluginPartKind::Tooling));
    CHECK(RoleLoads(HostRole::Mcp, PluginPartKind::Tooling));
    CHECK_FALSE(RoleLoads(HostRole::Mcp, PluginPartKind::Editor));
    CHECK(RoleLoads(HostRole::Editor, PluginPartKind::Editor));
    CHECK(PluginPartSymbol(u8"steam", PluginPartKind::Engine, DefaultAllocator()) == String(u8"PluginPart_steam_Engine"));
}

namespace
{
    struct ClockService
    {
        int ticks = 0;
    };
    struct AbsentService
    {
    };

    constexpr char8_t kServiceUser[] = u8"service_user";
    bool g_sawAbsent = true;

    void LoadServiceUser(PluginContext& context)
    {
        if (ClockService* clock = context.Find<ClockService>(); clock != nullptr)
        {
            ++clock->ticks;
        }
        g_sawAbsent = context.Find<AbsentService>() != nullptr;
    }
    const PluginPart* ServiceUserEngine()
    {
        static constexpr PluginPart part{kServiceUser, PluginPartKind::Engine, &LoadServiceUser, nullptr};
        return &part;
    }
}

TEST_CASE("plugin host: a part finds the services its host provides by type, and null for the rest")
{
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    linked.Add({.id = kServiceUser, .parts = Parts({PluginPartKind::Engine})}, {&ServiceUserEngine});
    ClockService clock;
    ClockService replacement;
    PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
    REQUIRE(host.AddLinked(linked.List()).IsOk());
    CHECK(host.Provide(clock));
    CHECK(host.Provide(replacement)); // one per type: this one replaces the first
    const StringView enabled[] = {kServiceUser};
    CHECK(host.Load(Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
    CHECK(clock.ticks == 0);
    CHECK(replacement.ticks == 1);
    CHECK_FALSE(g_sawAbsent);
    CHECK_FALSE(host.Provide(clock)); // loaded plugins read the table: it is fixed now
}

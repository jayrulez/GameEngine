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
import foundation.xml.serialization; // the entries' XML round trip

using namespace foundation::core;
using namespace foundation::runtime;

namespace
{
    // Loads `ids` as a project's entries naming no platforms and no settings.
    PluginLoadReport LoadIds(PluginHost& host, Span<const StringView> ids)
    {
        Array<EnabledPlugin> entries;
        for (StringView id : ids)
        {
            EnabledPlugin entry;
            entry.id = String(id);
            entries.PushBack(Move(entry));
        }
        return host.Load(Span<const EnabledPlugin>(entries.Data(), entries.Size()));
    }
}

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
        Array<String> platforms{}; // empty: Linux64, Win64 and Web
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
        if (spec.platforms.IsEmpty())
        {
            for (StringView platform : {StringView(u8"Linux64"), StringView(u8"Win64"), StringView(u8"Web")})
            {
                spec.platforms.PushBack(String(platform));
            }
        }
        for (const String& platform : spec.platforms)
        {
            manifest.platforms.PushBack(PluginPlatform{platform});
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
    const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 3));
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
        const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 1));
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
    const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 3));
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
    const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 2));
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
    const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 2));
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
    const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 5));
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
        CHECK(LoadIds(player, Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
    }
    {
        Log().Clear();
        SubsystemManager subsystems(DefaultAllocator());
        PluginHost cook(DefaultAllocator(), subsystems, HostRole::Cook, u8"0.1.0");
        REQUIRE(cook.AddLinked(linked.List()).IsOk());
        const PluginLoadReport report = LoadIds(cook, Span<const StringView>(enabled, 1));
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
        CHECK(LoadIds(host, Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
        // The registrar is linked but not enabled: nothing of it ran.
        CHECK_FALSE(GlobalSerializableRegistry().Contains(probe));
        CHECK_FALSE(subsystems.HasSubsystem<hostprobe::ProbeSubsystem>());
    }
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        const StringView enabled[] = {kRegistrar};
        CHECK(LoadIds(host, Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
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
        manifest->platforms.PushBack(PluginPlatform{String(GetHostPlatformName())});
        const PluginLibrary libraries[] = {{PluginPartKind::Engine, String(PartLibraryPath())}};
        host.AddShared(Move(manifest), Span<const PluginLibrary>(libraries, 1));
        const StringView enabled[] = {u8"shared_probe"};
        const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 1));
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
        manifest->platforms.PushBack(PluginPlatform{String(GetHostPlatformName())});
        const PluginLibrary libraries[] = {{PluginPartKind::Engine, String(u8"./no-such-library.so")}};
        host.AddShared(Move(manifest), Span<const PluginLibrary>(libraries, 1));
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        const StringView enabled[] = {kAlpha};
        CHECK(LoadIds(host, Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
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
    manifest->platforms.PushBack(PluginPlatform{String(GetHostPlatformName())});
    const PluginLibrary libraries[] = {{PluginPartKind::Engine, String(u8"./no-such-library.so")}};
    host.AddShared(Move(manifest), Span<const PluginLibrary>(libraries, 1));
    const StringView enabled[] = {u8"absent"};
    const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 1));
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
    CHECK(LoadIds(host, Span<const StringView>(enabled, 1)).loaded.Size() == 1u);
    CHECK(clock.ticks == 0);
    CHECK(replacement.ticks == 1);
    CHECK_FALSE(g_sawAbsent);
    CHECK_FALSE(host.Provide(clock)); // loaded plugins read the table: it is fixed now
}

namespace
{
    constexpr char8_t kAnywhere[] = u8"anywhere";
    constexpr char8_t kDesktop[] = u8"desktop";

    [[nodiscard]] EnabledPlugin Entry(StringView id, std::initializer_list<const char8_t*> platforms = {})
    {
        EnabledPlugin entry;
        entry.id = String(id);
        for (const char8_t* platform : platforms)
        {
            entry.platforms.PushBack(String(StringView(platform)));
        }
        return entry;
    }

    [[nodiscard]] PluginLoadReport LoadOn(StringView platform, Linked& linked, Span<const EnabledPlugin> entries)
    {
        SubsystemManager subsystems(DefaultAllocator());
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0", platform);
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        PluginLoadReport report = host.Load(entries);
        host.Unload();
        return report;
    }
}

TEST_CASE("plugin host: an entry loads where it is enabled, is absent elsewhere, and is refused where its plugin cannot run")
{
    Linked linked;
    linked.Add({.id = kAnywhere, .parts = Parts({PluginPartKind::Engine})}, {&Part<kAnywhere, PluginPartKind::Engine>});
    linked.Add({.id = kDesktop, .parts = Parts({PluginPartKind::Engine}), .platforms = Strings({u8"Linux64", u8"Win64"})},
               {&Part<kDesktop, PluginPartKind::Engine>});

    // Naming no platforms is every platform the manifest supports.
    {
        const EnabledPlugin entries[] = {Entry(kAnywhere), Entry(kDesktop)};
        const PluginLoadReport web = LoadOn(u8"Web", linked, Span<const EnabledPlugin>(entries, 2));
        CHECK(Same(web.loaded, Strings({kAnywhere})));
        CHECK(Same(web.notEnabled, Strings({kDesktop})));
        CHECK(web.refused.IsEmpty());
        const PluginLoadReport linux = LoadOn(u8"Linux64", linked, Span<const EnabledPlugin>(entries, 2));
        CHECK(Same(linux.loaded, Strings({kAnywhere, kDesktop})));
    }
    // Naming platforms enables it on those alone.
    {
        const EnabledPlugin entries[] = {Entry(kAnywhere, {u8"Win64", u8"Web"})};
        CHECK(Same(LoadOn(u8"Web", linked, Span<const EnabledPlugin>(entries, 1)).loaded, Strings({kAnywhere})));
        const PluginLoadReport linux = LoadOn(u8"Linux64", linked, Span<const EnabledPlugin>(entries, 1));
        CHECK(linux.loaded.IsEmpty());
        CHECK(Same(linux.notEnabled, Strings({kAnywhere})));
    }
    // Naming a platform the manifest does not support is refused there, and reported.
    {
        const EnabledPlugin entries[] = {Entry(kDesktop, {u8"Linux64", u8"Web"})};
        const PluginLoadReport web = LoadOn(u8"Web", linked, Span<const EnabledPlugin>(entries, 1));
        CHECK(RefusedWith(web, kDesktop, u8"enabled on Web, which it does not support"));
        CHECK(Same(LoadOn(u8"Linux64", linked, Span<const EnabledPlugin>(entries, 1)).loaded, Strings({kDesktop})));

        PluginManifest manifest;
        manifest.platforms.PushBack(PluginPlatform{String(u8"Linux64")});
        CHECK(EnablementOn(entries[0], manifest, u8"Linux64") == PluginEnablement::Enabled);
        CHECK(EnablementOn(entries[0], manifest, u8"Web") == PluginEnablement::Unsupported);
        CHECK(EnablementOn(entries[0], manifest, u8"Win64") == PluginEnablement::NotEnabled);
        CHECK(Same(UnsupportedPlatforms(entries[0], manifest, DefaultAllocator()), Strings({u8"Web"})));
    }
}

namespace hostprobe
{
    // A plugin's settings type: what its part reads from the project's entry.
    class ProbeConfig final : public ISerializable
    {
        RTTI_OBJECT(ProbeConfig, ISerializable)
    public:
        i32 volume = 3;
        String name = String(u8"default");

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "volume", volume);
            foundation::core::Serialize(ar, "name", name);
        }
    };
    RTTI_DEFINE_OBJECT_VERSIONED(ProbeConfig, "rtti::hostprobe", 1)
}

namespace
{
    constexpr char8_t kConfigured[] = u8"configured";
    i32 g_volumeAtLoad = 0;
    bool g_otherTypeRefused = false;

    void LoadConfigured(PluginContext& context)
    {
        hostprobe::ProbeConfig* config = context.Settings<hostprobe::ProbeConfig>();
        g_volumeAtLoad = config != nullptr ? config->volume : -1;
        g_otherTypeRefused = context.Settings<hostprobe::ProbeSettings>() == nullptr;
    }
    const PluginPart* ConfiguredEngine()
    {
        static constexpr PluginPart part{kConfigured, PluginPartKind::Engine, &LoadConfigured, nullptr};
        return &part;
    }

    [[nodiscard]] Array<u8> ConfigXml(i32 volume, StringView name)
    {
        hostprobe::ProbeConfig config;
        config.volume = volume;
        config.name = String(name);
        Array<u8> settings;
        REQUIRE(WritePluginSettings(config, settings, DefaultAllocator()).IsOk());
        return settings;
    }
}

TEST_CASE("plugin host: a part reads its settings from the project's entry, and an editor writes them back")
{
    SubsystemManager subsystems(DefaultAllocator());
    Linked linked;
    linked.Add({.id = kConfigured, .parts = Parts({PluginPartKind::Engine})}, {&ConfiguredEngine});

    SUBCASE("written settings are what the part reads")
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        EnabledPlugin entry = Entry(kConfigured);
        entry.settings = ConfigXml(7, u8"seven");
        REQUIRE(host.Load(Span<const EnabledPlugin>(&entry, 1)).loaded.Size() == 1u);
        CHECK(g_volumeAtLoad == 7);
        CHECK(g_otherTypeRefused); // one settings type per plugin
        CHECK(host.Report().loaded.Size() == 1u); // kept for whoever reports the project's plugins
        REQUIRE(host.Manifest(kConfigured) != nullptr);
        CHECK(host.Manifest(kConfigured)->id == String(kConfigured));
        CHECK(host.Manifest(u8"absent") == nullptr);
        CHECK(host.SettingsRead(kConfigured));

        // The editor's path: change the live object, write it into the entry, and it reads back.
        auto* live = Cast<hostprobe::ProbeConfig>(host.Settings(kConfigured));
        REQUIRE(live != nullptr);
        CHECK(live->name == String(u8"seven"));
        live->volume = 9;
        REQUIRE(WritePluginSettings(*live, entry.settings, DefaultAllocator()).IsOk());
        hostprobe::ProbeConfig reread;
        REQUIRE(ReadPluginSettings(Span<const u8>(entry.settings.Data(), entry.settings.Size()), reread,
                                   DefaultAllocator())
                    .IsOk());
        CHECK(reread.volume == 9);
        CHECK(reread.name == String(u8"seven"));
        host.Unload();
        CHECK(host.Report().loaded.IsEmpty());
        CHECK(host.Settings(kConfigured) == nullptr);
    }
    SUBCASE("an entry with no settings gives the defaults")
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        const EnabledPlugin entry = Entry(kConfigured);
        REQUIRE(host.Load(Span<const EnabledPlugin>(&entry, 1)).loaded.Size() == 1u);
        CHECK(g_volumeAtLoad == 3);
        CHECK(host.SettingsRead(kConfigured));
    }
    SUBCASE("settings that do not read as the type give the defaults, marked unread")
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(host.AddLinked(linked.List()).IsOk());
        EnabledPlugin entry = Entry(kConfigured);
        hostprobe::ProbeSettings other; // another type's payload: its data versions do not match
        REQUIRE(WritePluginSettings(other, entry.settings, DefaultAllocator()).IsOk());
        REQUIRE(host.Load(Span<const EnabledPlugin>(&entry, 1)).loaded.Size() == 1u);
        CHECK(g_volumeAtLoad == 3);
        CHECK_FALSE(host.SettingsRead(kConfigured));
    }
    SUBCASE("a plugin that is not loaded has no settings object")
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        CHECK(host.Settings(kConfigured) == nullptr);
    }
}

namespace
{
    // A document holding entries, as the project's manifest does.
    class EntryList final : public ISerializable
    {
        RTTI_OBJECT(EntryList, ISerializable)
    public:
        Array<EnabledPlugin> plugins;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "plugins", plugins); }
    };
    RTTI_DEFINE_OBJECT(EntryList, "rtti::hostprobe")

    [[nodiscard]] String WriteXml(EntryList& list)
    {
        MemoryStream buffer;
        UniquePtr<SerializerContext> ctx = foundation::xml::XmlSerializerFactory()(buffer, SerializeMode::Write);
        list.Serialize(*ctx->serializer);
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(buffer);
        return String(StringView(reinterpret_cast<const utf8char*>(buffer.Bytes().Data()), buffer.Bytes().Size()));
    }

    void ReadXml(StringView xml, EntryList& list)
    {
        MemoryStream buffer;
        (void)buffer.Write(xml.Data(), xml.Size());
        (void)buffer.Seek(0, SeekOrigin::Begin);
        UniquePtr<SerializerContext> ctx = foundation::xml::XmlSerializerFactory()(buffer, SerializeMode::Read);
        list.Serialize(*ctx->serializer);
        REQUIRE(ctx->serializer->IsOk());
    }
}

TEST_CASE("enabled plugin: an entry's settings are kept verbatim, read by nobody, through XML and binary copies")
{
    EntryList list;
    EnabledPlugin entry = Entry(u8"steam", {u8"Linux64", u8"Win64"});
    const StringView unknown = u8"<object name=\"mystery\"><i32 name=\"appId\">480</i32></object>";
    entry.settings.Resize(unknown.Size());
    MemCopy(entry.settings.Data(), unknown.Data(), unknown.Size());
    list.plugins.PushBack(Move(entry));
    EnabledPlugin local = Entry(u8"paperkid");
    local.path = String(u8"Native");
    list.plugins.PushBack(Move(local));

    const String first = WriteXml(list);
    CHECK(first.AsView().Contains(u8"<i32 name=\"appId\">480</i32>"));

    EntryList read;
    ReadXml(first.AsView(), read);
    REQUIRE(read.plugins.Size() == 2u);
    CHECK(read.plugins[0].id == String(u8"steam"));
    CHECK(read.plugins[0].platforms.Size() == 2u);
    CHECK(read.plugins[1].path == String(u8"Native"));
    CHECK(read.plugins[1].settings.IsEmpty());
    CHECK(WriteXml(read) == first);

    // Through the binary serializer, as a copy of the manifest goes.
    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        read.Serialize(writer);
        REQUIRE(writer.IsOk());
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    EntryList copied;
    BinarySerializer reader(buffer, SerializeMode::Read);
    copied.Serialize(reader);
    REQUIRE(reader.IsOk());
    CHECK(WriteXml(copied) == first);
}

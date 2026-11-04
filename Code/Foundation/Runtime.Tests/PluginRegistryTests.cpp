// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The generated plugin registry. Runtime.Tests links the fixture
// packages with util_link_plugins(ROLE cook PLUGINS probe): the registry lists probe and the
// probe_base it depends on, each with its manifest's text and the parts a cook loads, and the
// host loads them through the entries the build named from the manifests.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;

using namespace foundation::core;
using namespace foundation::runtime;

#ifndef __EMSCRIPTEN__ // the load test is not built for the web, below
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
#endif

#ifndef PLUGIN_ROLES_FILE
#error "Runtime.Tests must define PLUGIN_ROLES_FILE (see Runtime.Tests/CMakeLists.txt)"
#endif

extern "C" int ProbeBaseEngineLoads();
extern "C" int ProbeEngineLoads();
#ifndef __EMSCRIPTEN__
// A web build declares only a plugin's Engine part (no tool runs there), so the probe's Pipeline part
// is built everywhere else.
extern "C" int ProbePipelineLoads();
#endif

namespace
{
    [[nodiscard]] const LinkedPlugin* FindLinked(StringView id)
    {
        for (const LinkedPlugin& plugin : LinkedPluginList())
        {
            const StringView xml{plugin.manifestXml};
            if (xml.Contains(Format(u8"<string name=\"id\">{}</string>", id).AsView()))
            {
                return &plugin;
            }
        }
        return nullptr;
    }
}

TEST_CASE("plugin registry: the build lists the named plugin and its dependencies, with a cook's parts")
{
    const Span<const LinkedPlugin> linked = LinkedPluginList();
    REQUIRE(linked.Size() == 2u);
    const LinkedPlugin* base = FindLinked(u8"probe_base");
    const LinkedPlugin* probe = FindLinked(u8"probe");
    REQUIRE(base != nullptr);
    REQUIRE(probe != nullptr);
    CHECK(base == &linked[0]); // dependencies first

    REQUIRE(base->partCount == 1u);
    CHECK(base->parts[0]()->kind == PluginPartKind::Engine);
    // probe has an Editor part too, which a cook does not load, so the build did not link it; a web
    // build declares only the Engine part.
#ifdef __EMSCRIPTEN__
    REQUIRE(probe->partCount == 1u);
    CHECK(probe->parts[0]()->kind == PluginPartKind::Engine);
#else
    REQUIRE(probe->partCount == 2u);
    CHECK(probe->parts[0]()->kind == PluginPartKind::Engine);
    CHECK(probe->parts[1]()->kind == PluginPartKind::Pipeline);
#endif
    for (usize i = 0; i < probe->partCount; ++i)
    {
        CHECK(StringView(probe->parts[i]()->pluginId) == StringView(u8"probe"));
    }
}

#ifndef __EMSCRIPTEN__ // a cook loads the Pipeline part, which a web build does not have (no cook runs there)
TEST_CASE("plugin registry: the host reads the embedded manifests and loads the parts through their entries")
{
    SubsystemManager subsystems(DefaultAllocator());
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Cook, u8"0.1.0");
        REQUIRE(host.AddLinked(LinkedPluginList()).IsOk());
        CHECK(host.Knows(u8"probe"));
        CHECK(host.Knows(u8"probe_base"));
        CHECK(ProbeEngineLoads() == 0); // linked, not enabled: nothing ran

        const StringView enabled[] = {u8"probe", u8"probe_base"};
        const PluginLoadReport report = LoadIds(host, Span<const StringView>(enabled, 2));
        CHECK(report.refused.IsEmpty());
        REQUIRE(report.loaded.Size() == 2u);
        CHECK(report.loaded[0] == String(u8"probe_base"));
        CHECK(ProbeBaseEngineLoads() == 1);
        CHECK(ProbeEngineLoads() == 1);
        CHECK(ProbePipelineLoads() == 1);
    }
    CHECK(ProbeEngineLoads() == 0); // the host's destruction unloaded everything
    CHECK(ProbePipelineLoads() == 0);
    CHECK(ProbeBaseEngineLoads() == 0);
}
#endif

TEST_CASE("plugin registry: the build's role table is the one RoleLoads gives")
{
    const Result<Array<byte>> bytes = ReadFile(StringView{reinterpret_cast<const utf8char*>(PLUGIN_ROLES_FILE)});
    REQUIRE(bytes.HasValue());
    const StringView text{reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()};
    for (HostRole role : kHostRoles)
    {
        String expected(HostRoleName(role));
        expected.Append(u8"|");
        bool first = true;
        for (PluginPartKind kind : kPluginPartKinds)
        {
            if (RoleLoads(role, kind))
            {
                if (!first)
                {
                    expected.Append(u8",");
                }
                expected.Append(PluginPartKindName(kind));
                first = false;
            }
        }
        expected.Append(u8"\n");
        CAPTURE(HostRoleName(role));
        CHECK(text.Contains(expected.AsView()));
    }
}

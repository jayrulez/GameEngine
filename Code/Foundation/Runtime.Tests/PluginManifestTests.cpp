// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The plugin manifest: its reader and writer, the checks a host
// refuses a plugin by, and the pin that keeps the build's reader (cmake/Plugins.cmake) reading
// every manifest the way the C++ reader does.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;

using namespace foundation::core;
using namespace foundation::runtime;

#ifndef PLUGIN_FIXTURES_DIR
#error "Runtime.Tests must define PLUGIN_FIXTURES_DIR (see Runtime.Tests/CMakeLists.txt)"
#endif
#ifndef PLUGIN_MANIFESTS_READ
#error "Runtime.Tests must define PLUGIN_MANIFESTS_READ (see Runtime.Tests/CMakeLists.txt)"
#endif

namespace
{
    [[nodiscard]] StringView Narrow(const char* text)
    {
        return StringView{reinterpret_cast<const utf8char*>(text)};
    }

    [[nodiscard]] String FixturePath(StringView package)
    {
        return PathJoin(Narrow(PLUGIN_FIXTURES_DIR), package, DefaultAllocator());
    }

    // The manifest Fixtures/Everything/Plugin.xml holds, built in code.
    void Everything(PluginManifest& manifest)
    {
        manifest.id = String(u8"everything");
        manifest.name = String(u8"Every Part");
        manifest.version = String(u8"1.2.3");
        manifest.engineVersion = String(u8"0.1");
        manifest.dependsOn.PushBack(PluginDependency{String(u8"online"), String(u8"0.1")});
        manifest.dependsOn.PushBack(PluginDependency{String(u8"other_thing"), String(u8"2.0.1")});
        manifest.provides.PushBack(String(u8"online.backend"));
        for (PluginPartKind kind : kPluginPartKinds)
        {
            manifest.parts.PushBack(PluginManifestPart{String(PluginPartKindName(kind))});
        }
        PluginPlatform desktop;
        desktop.platform = String(u8"Linux64");
        desktop.deploy.PushBack(String(u8"ThirdParty/linux64/libthing.so"));
        manifest.platforms.PushBack(Move(desktop));
        PluginPlatform web;
        web.platform = String(u8"Web");
        manifest.platforms.PushBack(Move(web));
    }

    // A manifest that passes every check, for the refusal cases to break one field of.
    void Valid(PluginManifest& manifest)
    {
        manifest.id = String(u8"steam");
        manifest.name = String(u8"Steam");
        manifest.version = String(u8"0.1.0");
        manifest.engineVersion = String(u8"0.1");
        manifest.parts.PushBack(PluginManifestPart{String(u8"Engine")});
    }

    // The text without its carriage returns. A Windows checkout gives the fixtures CRLF line
    // endings, and the files the configure writes may carry them too; the line ending is not what
    // these tests pin.
    [[nodiscard]] String WithoutCarriageReturns(StringView text)
    {
        String out;
        for (usize i = 0; i < text.Size(); ++i)
        {
            if (text[i] != u8'\r')
            {
                out.Append(text.SubStr(i, 1));
            }
        }
        return out;
    }

    [[nodiscard]] String Join(const Array<String>& values)
    {
        String joined;
        for (usize i = 0; i < values.Size(); ++i)
        {
            if (i > 0)
            {
                joined.Append(u8",");
            }
            joined.Append(values[i].AsView());
        }
        return joined;
    }

    // The fields of one line `manifest|id|parts|depends` the configure wrote.
    struct ReadByCMake
    {
        String manifest;
        String id;
        String parts;
        String depends;
    };

    [[nodiscard]] Array<ReadByCMake> ManifestsReadByCMake()
    {
        Array<ReadByCMake> out;
        const Result<Array<byte>> bytes = ReadFile(Narrow(PLUGIN_MANIFESTS_READ));
        REQUIRE(bytes.HasValue());
        const String record = WithoutCarriageReturns(StringView{reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()});
        const StringView text = record.AsView();
        usize start = 0;
        while (start < text.Size())
        {
            usize end = text.Find(u8'\n', start);
            if (end == StringView::kNotFound)
            {
                end = text.Size();
            }
            const StringView line = text.SubStr(start, end - start);
            start = end + 1;
            if (line.IsEmpty())
            {
                continue;
            }
            StringView fields[4];
            usize field = 0;
            usize from = 0;
            for (usize i = 0; i <= line.Size() && field < 4; ++i)
            {
                if (i == line.Size() || line[i] == u8'|')
                {
                    fields[field++] = line.SubStr(from, i - from);
                    from = i + 1;
                }
            }
            REQUIRE(field == 4);
            out.PushBack(ReadByCMake{String(fields[0]), String(fields[1]), String(fields[2]), String(fields[3])});
        }
        return out;
    }
}

TEST_CASE("plugin manifest: written and read back, every field survives")
{
    PluginManifest written;
    Everything(written);
    MemoryStream buffer;
    REQUIRE(WritePluginManifest(written, buffer).IsOk());

    (void)buffer.Seek(0, SeekOrigin::Begin);
    PluginManifest read;
    REQUIRE(ReadPluginManifest(buffer, read).IsOk());
    CHECK(read.id == written.id);
    CHECK(read.name == written.name);
    CHECK(read.version == written.version);
    CHECK(read.engineVersion == written.engineVersion);
    REQUIRE(read.dependsOn.Size() == 2u);
    CHECK(read.dependsOn[1].id == String(u8"other_thing"));
    CHECK(read.dependsOn[1].version == String(u8"2.0.1"));
    REQUIRE(read.provides.Size() == 1u);
    CHECK(read.provides[0] == String(u8"online.backend"));
    for (PluginPartKind kind : kPluginPartKinds)
    {
        CHECK(read.HasPart(kind));
    }
    REQUIRE(read.FindPlatform(u8"Linux64") != nullptr);
    REQUIRE(read.FindPlatform(u8"Linux64")->deploy.Size() == 1u);
    CHECK(read.FindPlatform(u8"Linux64")->deploy[0] == String(u8"ThirdParty/linux64/libthing.so"));
    REQUIRE(read.FindPlatform(u8"Web") != nullptr);
    CHECK(read.FindPlatform(u8"Web")->deploy.IsEmpty());
    CHECK(read.FindPlatform(u8"Win64") == nullptr); // not listed: unsupported
}

TEST_CASE("plugin manifest: the writer's output is the fixture the build reads, byte for byte")
{
    // The CMake reader takes only the writer's shape (one element per line). The fixture is what
    // the configure read (the pin below); this proves it is also what the writer writes, so a
    // manifest the scaffold writes is one the build can read.
    PluginManifest manifest;
    Everything(manifest);
    MemoryStream buffer;
    REQUIRE(WritePluginManifest(manifest, buffer).IsOk());
    const String path = PathJoin(FixturePath(u8"Everything").AsView(), kPluginManifestFile, DefaultAllocator());
    const Result<Array<byte>> fixture = ReadFile(path.AsView());
    REQUIRE(fixture.HasValue());
    const StringView expected{reinterpret_cast<const utf8char*>(fixture.Value().Data()), fixture.Value().Size()};
    const StringView written{reinterpret_cast<const utf8char*>(buffer.Bytes().Data()), buffer.Bytes().Size()};
    // A hand-written manifest ends in a newline and the writer's does not; nothing else may differ.
    const String fixtureText = WithoutCarriageReturns(expected);
    CHECK(TrimEnd(written) == TrimEnd(fixtureText.AsView()));
}

TEST_CASE("plugin manifest: a package's Plugin.xml is loaded from its folder, NotFound without one")
{
    PluginManifest manifest;
    REQUIRE(LoadPluginManifest(FixturePath(u8"Everything").AsView(), manifest, DefaultAllocator()).IsOk());
    CHECK(manifest.id == String(u8"everything"));

    PluginManifest none;
    CHECK(LoadPluginManifest(FixturePath(u8"NoSuchPackage").AsView(), none, DefaultAllocator()).Code() ==
          ErrorCode::NotFound);
}

TEST_CASE("plugin manifest: versions are major.minor or major.minor.patch")
{
    const Optional<PluginVersion> two = ParsePluginVersion(u8"0.1");
    REQUIRE(two);
    CHECK(two->major == 0u);
    CHECK(two->minor == 1u);
    CHECK(two->patch == 0u);
    const Optional<PluginVersion> three = ParsePluginVersion(u8"12.30.4");
    REQUIRE(three);
    CHECK(three->major == 12u);
    CHECK(three->minor == 30u);
    CHECK(three->patch == 4u);

    CHECK_FALSE(ParsePluginVersion(u8""));
    CHECK_FALSE(ParsePluginVersion(u8"1"));
    CHECK_FALSE(ParsePluginVersion(u8"1."));
    CHECK_FALSE(ParsePluginVersion(u8".1"));
    CHECK_FALSE(ParsePluginVersion(u8"1..2"));
    CHECK_FALSE(ParsePluginVersion(u8"1.2.3.4"));
    CHECK_FALSE(ParsePluginVersion(u8"1.2-dev"));
    CHECK_FALSE(ParsePluginVersion(u8"-1.2"));
}

TEST_CASE("plugin manifest: the engine satisfies a plugin of its major, at or below its version; before 1.0, of its minor")
{
    CHECK(EngineSatisfies({0, 1, 0}, {0, 1, 0}));
    CHECK(EngineSatisfies({0, 1, 4}, {0, 1, 2}));
    CHECK_FALSE(EngineSatisfies({0, 1, 1}, {0, 1, 2}));
    CHECK_FALSE(EngineSatisfies({0, 2, 0}, {0, 1, 0})); // a 0.x minor may break plugins
    CHECK_FALSE(EngineSatisfies({0, 1, 0}, {0, 2, 0}));
    CHECK(EngineSatisfies({1, 3, 0}, {1, 2, 9})); // after 1.0 a newer minor keeps them
    CHECK_FALSE(EngineSatisfies({1, 2, 0}, {1, 3, 0}));
    CHECK_FALSE(EngineSatisfies({2, 0, 0}, {1, 0, 0}));
}

TEST_CASE("plugin manifest: ids are lower case C identifiers")
{
    CHECK(IsValidPluginId(u8"steam"));
    CHECK(IsValidPluginId(u8"online_local2"));
    CHECK_FALSE(IsValidPluginId(u8""));
    CHECK_FALSE(IsValidPluginId(u8"Steam"));
    CHECK_FALSE(IsValidPluginId(u8"2steam"));
    CHECK_FALSE(IsValidPluginId(u8"_steam"));
    CHECK_FALSE(IsValidPluginId(u8"steam-api"));
    CHECK_FALSE(IsValidPluginId(u8"steam.api"));
}

TEST_CASE("plugin manifest: a valid manifest passes, and each broken field is refused with its reason")
{
    String whyNot;
    PluginManifest valid;
    Valid(valid);
    CHECK(CheckPluginManifest(valid, u8"0.1.0", whyNot));
    CHECK(whyNot.IsEmpty());

    SUBCASE("bad id")
    {
        PluginManifest manifest;
        Valid(manifest);
        manifest.id = String(u8"Steam API");
        CHECK_FALSE(CheckPluginManifest(manifest, u8"0.1.0", whyNot));
        CHECK(whyNot.AsView().Contains(u8"Steam API"));
    }
    SUBCASE("unknown part kind")
    {
        PluginManifest manifest;
        Valid(manifest);
        manifest.parts.PushBack(PluginManifestPart{String(u8"Runtime")});
        CHECK_FALSE(CheckPluginManifest(manifest, u8"0.1.0", whyNot));
        CHECK(whyNot.AsView().Contains(u8"unknown part kind 'Runtime'"));
    }
    SUBCASE("a part twice")
    {
        PluginManifest manifest;
        Valid(manifest);
        manifest.parts.PushBack(PluginManifestPart{String(u8"Engine")});
        CHECK_FALSE(CheckPluginManifest(manifest, u8"0.1.0", whyNot));
        CHECK(whyNot.AsView().Contains(u8"listed twice"));
    }
    SUBCASE("engine version unmet: the message names both")
    {
        PluginManifest manifest;
        Valid(manifest);
        manifest.engineVersion = String(u8"0.2");
        CHECK_FALSE(CheckPluginManifest(manifest, u8"0.1.0", whyNot));
        CHECK(whyNot.AsView().Contains(u8"needs engine 0.2"));
        CHECK(whyNot.AsView().Contains(u8"this engine is 0.1.0"));
    }
    SUBCASE("malformed versions")
    {
        PluginManifest version;
        Valid(version);
        version.version = String(u8"one");
        CHECK_FALSE(CheckPluginManifest(version, u8"0.1.0", whyNot));
        PluginManifest engine;
        Valid(engine);
        engine.engineVersion = String(u8"latest");
        CHECK_FALSE(CheckPluginManifest(engine, u8"0.1.0", whyNot));
    }
    SUBCASE("a dependency that is not another plugin")
    {
        PluginManifest self;
        Valid(self);
        self.dependsOn.PushBack(PluginDependency{String(u8"steam"), String(u8"0.1")});
        CHECK_FALSE(CheckPluginManifest(self, u8"0.1.0", whyNot));
        PluginManifest badId;
        Valid(badId);
        badId.dependsOn.PushBack(PluginDependency{String(u8"On Line"), String(u8"0.1")});
        CHECK_FALSE(CheckPluginManifest(badId, u8"0.1.0", whyNot));
        PluginManifest badVersion;
        Valid(badVersion);
        badVersion.dependsOn.PushBack(PluginDependency{String(u8"online"), String(u8"soon")});
        CHECK_FALSE(CheckPluginManifest(badVersion, u8"0.1.0", whyNot));
    }
}

TEST_CASE("plugin manifest: the build read every manifest the way the C++ reader does")
{
    const Array<ReadByCMake> lines = ManifestsReadByCMake();
    bool sawFixture = false;
    for (const ReadByCMake& line : lines)
    {
        CAPTURE(line.manifest);
        const Result<Array<byte>> bytes = ReadFile(line.manifest.AsView());
        REQUIRE(bytes.HasValue());
        MemoryStream stream;
        (void)stream.Write(bytes.Value().Data(), bytes.Value().Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        PluginManifest manifest;
        REQUIRE(ReadPluginManifest(stream, manifest).IsOk());

        CHECK(manifest.id == line.id);
        Array<String> parts;
        for (const PluginManifestPart& part : manifest.parts)
        {
            parts.PushBack(part.kind);
        }
        CHECK(Join(parts) == line.parts);
        Array<String> depends;
        for (const PluginDependency& dependency : manifest.dependsOn)
        {
            depends.PushBack(dependency.id);
        }
        CHECK(Join(depends) == line.depends);
        sawFixture = sawFixture || manifest.id == String(u8"everything");
    }
    CHECK(sawFixture); // the four-part fixture, with dependencies, is always among them
}

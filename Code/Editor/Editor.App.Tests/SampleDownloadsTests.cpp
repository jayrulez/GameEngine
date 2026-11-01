// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App tests: the Samples tab's catalog (a release's samples.json) and one sample's
// download against a loopback server: streamed to a file, checked against its SHA-256, extracted
// and the zip deleted; a wrong digest, a missing sample and a cancel each leave nothing behind.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.http;
import foundation.http.client;
import editor.app;

#include "LoopbackFiles.h"

using namespace foundation::core;
namespace app = editor::app;

namespace
{
    doctest::String C(StringView s) { return doctest::String(reinterpret_cast<const char*>(s.Data()), static_cast<unsigned>(s.Size())); }

    struct Fixture
    {
        String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_sample_downloads");
        Array<byte> zip;
        app::SampleInfo sample;
        UniquePtr<foundation::http::IHttpClient> client = foundation::http::CreateNativeHttpClient(DefaultAllocator(), nullptr);

        Fixture()
        {
            (void)RemoveDirectoryRecursive(root.AsView());
            REQUIRE(CreateDirectories(root.AsView()));
            Result<Array<byte>> bytes = ReadFile(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data/Tests/Zip/project.zip").AsView());
            REQUIRE(bytes.HasValue());
            zip = Move(bytes.Value());
            sample.folder = String(u8"Game");
            sample.name = String(u8"Game");
            sample.archive = String(u8"sample-Game.zip");
            sample.size = zip.Size();
            sample.sha256 = Sha256::Of(zip.AsSpan()).ToHex();
        }
        ~Fixture() { (void)RemoveDirectoryRecursive(root.AsView()); }

        // Runs `download` to its end, the server and the client pumped together.
        void Run(app::SampleDownload& download, LoopbackFiles& server)
        {
            for (u32 i = 0; i < 10000 && download.GetState() == app::SampleDownload::State::Downloading; ++i)
            {
                server.Pump();
                client->Pump();
                if (download.GetState() == app::SampleDownload::State::Downloading)
                    SleepMilliseconds(1);
            }
        }
        [[nodiscard]] bool LeftNothing(StringView target) const
        {
            return !DirectoryExists(target) && !FileExists(PathJoin(root.AsView(), u8".Game.download").AsView());
        }
    };
}

TEST_CASE("sample downloads: a release's list of samples, and what refuses it")
{
    app::SampleCatalog catalog;
    String error;
    REQUIRE(app::SampleCatalog::Parse(
        u8R"({"version": "0.1.0", "samples": [{"folder": "Snowline", "name": "Snowline", "description": "A time trial.",
             "archive": "sample-Snowline.zip", "size": 1234, "unpackedSize": 5678,
             "sha256": "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "picture": "sample-Snowline.png"}]})",
        catalog, error));
    CHECK(catalog.version == StringView(u8"0.1.0"));
    REQUIRE(catalog.samples.Size() == 1u);
    CHECK(catalog.samples[0].name == StringView(u8"Snowline"));
    CHECK(catalog.samples[0].size == 1234u);
    CHECK(catalog.samples[0].unpackedSize == 5678u);

    CHECK_FALSE(app::SampleCatalog::Parse(u8"not json", catalog, error));
    CHECK(catalog.samples.IsEmpty());
    // A folder or an archive that walks out of where it goes refuses the whole list.
    CHECK_FALSE(app::SampleCatalog::Parse(
        u8R"({"samples": [{"folder": "../x", "archive": "a.zip", "sha256": "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}]})",
        catalog, error));
    CHECK_FALSE(app::SampleCatalog::Parse(u8R"({"samples": [{"folder": "x", "archive": "a.zip", "sha256": "short"}]})", catalog, error));
}

TEST_CASE("sample downloads: streamed, checked and unpacked into its folder, the zip gone after")
{
    Fixture f;
    LoopbackFiles server;
    server.Serve(u8"/sample-Game.zip", f.zip);
    const String target = PathJoin(f.root.AsView(), u8"Game");
    app::SampleDownload download(DefaultAllocator(), *f.client, f.sample, server.Url(u8"/sample-Game.zip"), target);
    download.Start();
    f.Run(download, server);
    INFO(C(download.Error()));
    REQUIRE(download.GetState() == app::SampleDownload::State::Done);
    CHECK(download.Received() == f.zip.Size());
    CHECK(download.Progress() == doctest::Approx(1.0f));
    CHECK(FileExists(PathJoin(target.AsView(), u8"Game/Project.xml").AsView())); // the fixture's own top folder
    CHECK_FALSE(FileExists(PathJoin(f.root.AsView(), u8".Game.download").AsView()));
}

TEST_CASE("sample downloads: a wrong digest, a missing sample and a cancel leave nothing behind")
{
    Fixture f;
    const String target = PathJoin(f.root.AsView(), u8"Game");
    {
        LoopbackFiles server;
        server.Serve(u8"/x.zip", f.zip);
        app::SampleInfo wrong = f.sample;
        wrong.sha256 = String(u8"0000000000000000000000000000000000000000000000000000000000000000");
        app::SampleDownload download(DefaultAllocator(), *f.client, wrong, server.Url(u8"/x.zip"), target);
        download.Start();
        f.Run(download, server);
        CHECK(download.GetState() == app::SampleDownload::State::Failed);
        CHECK(download.Error().ContainsIgnoreCase(u8"checksum"));
        CHECK(f.LeftNothing(target.AsView()));
    }
    {
        LoopbackFiles server; // serves nothing: a 404
        app::SampleDownload download(DefaultAllocator(), *f.client, f.sample, server.Url(u8"/x.zip"), target);
        download.Start();
        f.Run(download, server);
        CHECK(download.GetState() == app::SampleDownload::State::Failed);
        CHECK(download.Error().ContainsIgnoreCase(u8"does not have"));
        CHECK(f.LeftNothing(target.AsView()));
    }
    {
        LoopbackFiles server;
        server.Serve(u8"/x.zip", f.zip);
        app::SampleDownload download(DefaultAllocator(), *f.client, f.sample, server.Url(u8"/x.zip"), target);
        download.Start();
        download.Cancel();
        CHECK(download.GetState() == app::SampleDownload::State::Cancelled);
        for (u32 i = 0; i < 50; ++i) // nothing more arrives for it
        {
            server.Pump();
            f.client->Pump();
            SleepMilliseconds(1);
        }
        CHECK(download.GetState() == app::SampleDownload::State::Cancelled);
        CHECK(f.LeftNothing(target.AsView()));
    }
}

TEST_CASE("sample downloads: a release folder serves its files by plain name, and nothing outside it")
{
    const String folder = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_release_folder");
    (void)RemoveDirectoryRecursive(folder.AsView());
    REQUIRE(CreateDirectories(PathJoin(folder.AsView(), u8"inner").AsView()));
    const StringView text = u8"{\"samples\": []}";
    REQUIRE(WriteFile(PathJoin(folder.AsView(), u8"samples.json").AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    const auto get = [&folder](StringView target, StringView method = u8"GET")
    {
        foundation::http::HttpRequest request;
        request.method = String(method);
        request.target = String(target);
        return app::ReleaseFolder::Serve(folder.AsView(), request);
    };
    const foundation::http::HttpResponse found = get(u8"/samples.json");
    CHECK(found.status == 200);
    CHECK(found.BodyText() == text);
    CHECK(get(u8"/missing.zip").status == 404);
    CHECK(get(u8"/../secret").status == 404);
    CHECK(get(u8"/inner/x").status == 404);
    CHECK(get(u8"/").status == 404);
    CHECK(get(u8"/samples.json", u8"POST").status == 404);
    (void)RemoveDirectoryRecursive(folder.AsView());
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for VfsResourceProvider (foundation.ui.vfs) - Our addition, so covered per the additions
// rule. A mock in-memory IFileSystem (MemoryStream-backed) exercises the LoadText glue without disk IO.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs;
import foundation.ui.vfs;

using namespace foundation::core;
namespace vfs = foundation::vfs;
namespace ui = foundation::ui;

namespace
{
    class MemFS final : public vfs::IFileSystem
    {
    public:
        HashMap<String, String> files;

        UniquePtr<IStream> Open(StringView path, FileMode) override
        {
            String* content = files.Find(String(path));
            if (content == nullptr)
            {
                return {};
            }
            auto stream = MakeUnique<MemoryStream>(DefaultAllocator());
            (void)stream->Write(content->Data(), content->Size());
            (void)stream->Seek(0, SeekOrigin::Begin);
            return stream;
        }
        bool Exists(StringView path) override { return files.Find(String(path)) != nullptr; }
    };
}

TEST_CASE("vfs-provider: LoadText reads file bytes")
{
    MemFS fs;
    fs.files.InsertOrAssign(String(u8"a.sss"), String(u8"button { color: #fff; }"));
    ui::vfs::VfsResourceProvider provider(&fs);

    String out;
    CHECK(provider.LoadText(u8"a.sss", out));
    CHECK(out == u8"button { color: #fff; }");
}

TEST_CASE("vfs-provider: LoadText missing file fails")
{
    MemFS fs;
    ui::vfs::VfsResourceProvider provider(&fs);
    String out;
    CHECK(!provider.LoadText(u8"nope.sss", out));
}

TEST_CASE("vfs-provider: empty file succeeds with empty text")
{
    MemFS fs;
    fs.files.InsertOrAssign(String(u8"empty.sss"), String());
    ui::vfs::VfsResourceProvider provider(&fs);
    String out;
    CHECK(provider.LoadText(u8"empty.sss", out));
    CHECK(out.Size() == 0);
}

TEST_CASE("vfs-provider: null filesystem fails gracefully")
{
    ui::vfs::VfsResourceProvider provider(nullptr);
    String out;
    CHECK(!provider.LoadText(u8"x", out));
    CHECK(provider.LoadImage(u8"x") == nullptr);
}

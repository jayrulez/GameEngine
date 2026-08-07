// Tests for draconic.gui.vfs::VfsResourceProvider - loads + decodes background-image assets from
// a VFS. A mock in-memory IFileSystem exercises the plumbing; a real BMP round-trip (SaveImage ->
// NativeFileSystem -> LoadImage) exercises the decode path.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.image;
import draconic.image.io;
import draconic.vfs;
import draconic.gui.vfs;

using namespace draconic::foundation;
namespace vfs = draconic::vfs;
namespace image = draconic::image;
namespace gui = draconic::gui;

namespace
{
    // A minimal in-memory filesystem: serves stored byte blobs as MemoryStreams.
    class MemFS final : public vfs::IFileSystem
    {
    public:
        HashMap<String, Array<u8>> files;

        UniquePtr<IStream> Open(StringView path, FileMode) override
        {
            Array<u8>* content = files.Find(String(path));
            if (content == nullptr)
                return {};
            auto stream = MakeUnique<MemoryStream>(DefaultAllocator());
            if (content->Size() != 0)
                (void)stream->Write(content->Data(), content->Size());
            (void)stream->Seek(0, SeekOrigin::Begin);
            return stream;
        }
        bool Exists(StringView path) override { return files.Find(String(path)) != nullptr; }
    };
}

TEST_CASE("gui-vfs: null filesystem returns null")
{
    gui::vfs::VfsResourceProvider provider(nullptr);
    CHECK(provider.LoadImage(u8"anything.png") == nullptr);
}

TEST_CASE("gui-vfs: a missing file returns null")
{
    MemFS fs;
    gui::vfs::VfsResourceProvider provider(&fs);
    CHECK(provider.LoadImage(u8"nope.png") == nullptr);
}

TEST_CASE("gui-vfs: undecodable bytes return null")
{
    MemFS fs;
    Array<u8> junk;
    for (int i = 0; i < 16; ++i)
        junk.PushBack(static_cast<u8>(i));
    fs.files.InsertOrAssign(String(u8"bad.png"), Move(junk));
    gui::vfs::VfsResourceProvider provider(&fs);
    CHECK(provider.LoadImage(u8"bad.png") == nullptr);
}

TEST_CASE("gui-vfs: loads and decodes a real image (BMP round-trip)")
{
    // Write a 3x2 RGBA image to disk, then load it back through a NativeFileSystem + the provider.
    static const u8 pixels[3 * 2 * 4] = {
        255, 0,   0, 255, 0, 255, 0,   255, 0,   0, 255, 255,
        255, 255, 0, 255, 0, 255, 255, 255, 255, 0, 255, 255,
    };
    image::Image src(3, 2, image::PixelFormat::RGBA8, Span<const u8>(pixels, sizeof(pixels)));
    const StringView file(u8"gui_vfs_roundtrip.bmp");
    REQUIRE(image::io::SaveImage(src, file, image::io::ImageFileFormat::BMP).IsOk());

    vfs::NativeFileSystem fs(u8"."); // rooted at the test working directory
    gui::vfs::VfsResourceProvider provider(&fs);

    const image::ImageData* loaded = provider.LoadImage(file);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->Width() == 3);
    CHECK(loaded->Height() == 2);

    // The provider owns the image: a second load yields a distinct cached copy, both valid.
    const image::ImageData* again = provider.LoadImage(file);
    REQUIRE(again != nullptr);
    CHECK(again != loaded);
    CHECK(again->Width() == 3);
}

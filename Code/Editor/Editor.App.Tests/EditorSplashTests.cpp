// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the splash the editor shows while it starts (drawn on the CPU: its logo, its
// status line and its progress bar), the logo the Project panel and the project manager show, and
// the window icon, all from the repository's branding files.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.shell;
import foundation.ui;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace shell = foundation::shell;

namespace
{
    [[nodiscard]] String DataRoot()
    {
        return PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data");
    }

    [[nodiscard]] Array<u8> RobotoBytes()
    {
        Array<u8> bytes;
        Result<Array<byte>> read = ReadFile(PathJoin(DataRoot().AsView(), u8"Assets/fonts/roboto/Roboto-Regular.ttf").AsView());
        if (read.HasValue())
        {
            for (const byte b : read.Value())
            {
                bytes.PushBack(static_cast<u8>(b));
            }
        }
        return bytes;
    }

    struct Pixel
    {
        u8 r, g, b;
    };
    [[nodiscard]] Pixel At(const app::EditorSplash& splash, u32 x, u32 y)
    {
        const u8* p = &splash.Pixels()[(static_cast<usize>(y) * app::EditorSplash::kWidth + x) * 4];
        return Pixel{p[0], p[1], p[2]};
    }

    // A window that keeps the icon it is given.
    class IconWindow final : public shell::IWindow
    {
    public:
        Array<u32> sizes;
        [[nodiscard]] u32 Id() const noexcept override { return 1; }
        [[nodiscard]] u32 Width() const noexcept override { return 100; }
        [[nodiscard]] u32 Height() const noexcept override { return 100; }
        [[nodiscard]] i32 X() const noexcept override { return 0; }
        [[nodiscard]] i32 Y() const noexcept override { return 0; }
        void SetPosition(i32, i32) override {}
        void SetSize(u32, u32) override {}
        [[nodiscard]] f32 ContentScale() const noexcept override { return 1.0f; }
        [[nodiscard]] shell::NativeWindow Native() const noexcept override { return {}; }
        [[nodiscard]] bool IsOpen() const noexcept override { return true; }
        [[nodiscard]] bool IsMinimized() const noexcept override { return false; }
        void Close() override {}
        void StartTextInput() override {}
        void StopTextInput() override {}
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return false; }
        void SetIcon(Span<const shell::WindowIconImage> images) override
        {
            sizes.Clear();
            for (const shell::WindowIconImage& image : images)
            {
                CHECK(image.rgba != nullptr);
                CHECK(image.width == image.height);
                sizes.PushBack(image.width);
            }
        }
    };
}

TEST_CASE("splash: the logo, the status line and the progress bar, drawn on the CPU")
{
    // A small stand-in logo: an opaque red block.
    Array<u8> logo;
    for (u32 i = 0; i < 20 * 10; ++i)
    {
        logo.PushBack(255);
        logo.PushBack(0);
        logo.PushBack(0);
        logo.PushBack(255);
    }
    app::EditorSplash splash(nullptr, nullptr, Span<const u8>(logo.Data(), logo.Size()), 20, 10, RobotoBytes(),
                             u8"Version 0.1.0");
    REQUIRE(splash.Pixels().Size() == static_cast<usize>(app::EditorSplash::kWidth) * app::EditorSplash::kHeight * 4);
    const Pixel background = At(splash, app::EditorSplash::kWidth / 2, 20);

    // The logo, centred near the top.
    const Pixel logoPixel = At(splash, app::EditorSplash::kWidth / 2, 89);
    CHECK(logoPixel.r == 255);
    CHECK(logoPixel.g == 0);

    splash.Step(u8"Opening Level1", 0.5f);
    CHECK(splash.Status() == StringView(u8"Opening Level1"));
    CHECK(splash.Progress() == doctest::Approx(0.5f));
    // The bar: filled (the accent) up to half its width, the track after.
    const u32 barY = 255;
    const Pixel filled = At(splash, 40 + 120, barY);
    const Pixel track = At(splash, 40 + 360, barY);
    CHECK(filled.r == 0xF5);
    CHECK(filled.g == 0x8F);
    CHECK(track.r == 0x2E);
    // The status text drew something on its line (a lighter pixel than the background).
    bool inked = false;
    for (u32 x = 40; x < 200 && !inked; ++x)
    {
        for (u32 y = 222; y < 240 && !inked; ++y)
        {
            inked = At(splash, x, y).r > background.r + 40;
        }
    }
    CHECK(inked);

    splash.Step(u8"Ready", 2.0f); // clamped
    CHECK(splash.Progress() == doctest::Approx(1.0f));
    CHECK(At(splash, 40 + 470, barY).r == 0xF5);
}

TEST_CASE("splash: the editor's logo and window icon load from the branding files")
{
    CHECK(app::LoadEditorLogo(DataRoot().AsView(), DefaultAllocator()).Get() != nullptr);
    CHECK(app::LoadEditorLogo(u8"/nowhere", DefaultAllocator()).Get() == nullptr);

    IconWindow window;
    REQUIRE(app::ApplyEditorWindowIcon(window, DataRoot().AsView()));
    REQUIRE(window.sizes.Size() == 4u);
    CHECK(window.sizes[0] == 32u); // the normal scale's first
    CHECK(window.sizes[3] == 256u);
    IconWindow none;
    CHECK_FALSE(app::ApplyEditorWindowIcon(none, u8"/nowhere"));
    CHECK(none.sizes.IsEmpty());
}

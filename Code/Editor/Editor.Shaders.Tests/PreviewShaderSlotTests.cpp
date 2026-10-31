// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The shader page's preview shader slot: it registers both stages under the page's name, a
// release removes them, and once released it never reaches the shader system again. At editor
// exit the page outlives the renderer that owns the system (OnClose, then the runtime context
// shuts down, then the pages are destroyed), so a release from the page's destructor must not
// touch the freed system.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.rhi.null;
import foundation.shaders;
import foundation.shaders.system;
import editor.shaders;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

TEST_CASE("shader page: the preview slot registers, releases once and then lets the system go")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    const StringView name = u8"editor.preview.test";
    editor::PreviewShaderSlot slot;
    {
        shaders::ShaderSystem system(device);
        slot.Attach(&system, String(name));
        CHECK(slot.IsHeld());
        slot.Register(u8"float4 VSMain() : SV_Position { return 0; }\n", u8"VSMain",
                      shaders::ShaderFlags::None, u8"PSMain", shaders::ShaderFlags::None);
        CHECK(system.HasSource(name, shaders::ShaderStage::Vertex));
        CHECK(system.HasSource(name, shaders::ShaderStage::Fragment));

        slot.Release(); // the page's OnClose, while the renderer lives
        CHECK_FALSE(system.HasSource(name, shaders::ShaderStage::Vertex));
        CHECK_FALSE(system.HasSource(name, shaders::ShaderStage::Fragment));
        CHECK_FALSE(slot.IsHeld());
    } // the renderer (and its shader system) is destroyed

    // The page's destructor releases again and reaches nothing. Registering after a release does
    // nothing.
    slot.Release();
    slot.Register(u8"", u8"VSMain", shaders::ShaderFlags::None, u8"PSMain",
                  shaders::ShaderFlags::None);
    CHECK_FALSE(slot.IsHeld());
}

TEST_CASE("shader page: a preview slot with no shader system does nothing")
{
    editor::PreviewShaderSlot slot;
    slot.Attach(nullptr, String(u8"editor.preview.none"));
    CHECK_FALSE(slot.IsHeld());
    slot.Register(u8"", u8"VSMain", shaders::ShaderFlags::None, u8"PSMain",
                  shaders::ShaderFlags::None);
    slot.Release();
    CHECK(slot.Name() == StringView(u8"editor.preview.none"));
}

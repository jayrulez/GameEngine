// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Texture - the `foundation.texture` module.
//
// Logical texture types, a CPU-side upload descriptor (TextureData) and image-to-RHI
// format conversion. The descriptor layer between foundation.image (CPU) and
// foundation.rhi (GPU); consumers (the VG renderer, the texture resource factory)
// create the actual rhi::Texture from a TextureData. One named module composed of
// partitions.

export module foundation.texture;

export import :types;
export import :format_utils;
export import :data;

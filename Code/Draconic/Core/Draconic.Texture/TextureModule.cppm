// Draconic::Texture - the `draconic.texture` module.
//
// Logical texture types + a CPU-side upload descriptor (TextureData) and
// image->RHI format conversion. The descriptor layer between draconic.image (CPU)
// and draconic.rhi (GPU); consumers (VG renderer, the texture resource factory)
// create the actual rhi::Texture from a TextureData. Ported from
// Sedulous.Textures. One named module composed of partitions.

export module draconic.texture;

export import :types;
export import :format_utils;
export import :data;

# Draconic::VG.Renderer - `draconic.vg.renderer` (VGBatch -> RHI).
# Ported from Sedulous.VG.Renderer. Shader source: shaders/vg.{vert,frag}.hlsl
# (compile via draconic.shaders and pass the modules to VGRenderer::Initialize).

add_library(Draconic.VG.Renderer STATIC)
add_library(Draconic::VG.Renderer ALIAS Draconic.VG.Renderer)

target_link_libraries(Draconic.VG.Renderer PUBLIC
    Draconic::Foundation Draconic::RHI Draconic::VG Draconic::Image Draconic::Texture Draconic::Policy)

target_sources(Draconic.VG.Renderer
    PUBLIC
        FILE_SET vgrenderer_modules TYPE CXX_MODULES FILES
            RenderVertex.cppm
            Renderer.cppm
            VGRendererModule.cppm)


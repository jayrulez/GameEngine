# Foundation::VG.Renderer - `foundation.vg.renderer` (VGBatch to RHI).
# Shader source lives in Data/Engine/Shaders (vg.vs, vg.ps, vg_df.ps, vg_grad_radial.ps,
# vg_grad_conic.ps .hlsl), resolved by name through the shader file provider; compile them with
# foundation.shaders and pass the modules to VGRenderer::Initialize.

util_add_engine_library(VG.Renderer ALIAS Foundation::VG.Renderer)
target_link_libraries(VG.Renderer PUBLIC
    Foundation::Core Foundation::RHI Foundation::VG Foundation::Image Foundation::Texture Foundation::Policy)

target_sources(VG.Renderer
    PUBLIC
        FILE_SET vgrenderer_modules TYPE CXX_MODULES FILES
            RenderVertex.cppm
            Renderer.cppm
            PassTargets.cppm
            VGRendererModule.cppm)


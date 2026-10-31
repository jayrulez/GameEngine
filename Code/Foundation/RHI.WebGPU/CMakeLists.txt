# Foundation::RHI.WebGPU - `foundation.rhi.webgpu`: WebGPU backend over the standard
# webgpu.h. Desktop loads the wgpu-native sidecar at run time (the vendored
# ThirdParty::WgpuHeaders supply webgpu.h and wgpu.h as a headers-only build dependency);
# web gets both the header and the browser binding from Emscripten's emdawnwebgpu port.
util_add_engine_library(RHI.WebGPU ALIAS Foundation::RHI.WebGPU)
target_link_libraries(RHI.WebGPU
    PUBLIC Foundation::RHI Foundation::Core Foundation::Policy)

if(EMSCRIPTEN)
    # emdawnwebgpu supplies <webgpu/webgpu.h> (Dawn's spec header) at compile time and the
    # JS binding to navigator.gpu at link time. `embuilder build emdawnwebgpu` populates the
    # port cache. The flag is needed on both compile and link of every consumer, so it is
    # PUBLIC. (emcc 4 removed the older --use-port=emscripten_webgpu and -sUSE_WEBGPU=1.)
    target_compile_options(RHI.WebGPU PUBLIC "--use-port=emdawnwebgpu")
    target_link_options(RHI.WebGPU PUBLIC "--use-port=emdawnwebgpu")
else()
    target_link_libraries(RHI.WebGPU PRIVATE ThirdParty::WgpuHeaders)
endif()
target_sources(RHI.WebGPU PUBLIC FILE_SET rhi_webgpu_modules TYPE CXX_MODULES FILES
    WebGpuModule.cppm
    WebGpuApi.cppm
    WebGpuConversions.cppm
    WebGpuBlitHelper.cppm
    WebGpuBuffer.cppm
    WebGpuTexture.cppm
    WebGpuTextureView.cppm
    WebGpuSampler.cppm
    WebGpuShaderModule.cppm
    WebGpuBindGroupLayout.cppm
    WebGpuBindGroup.cppm
    WebGpuPipelineLayout.cppm
    WebGpuPushConstantEmulator.cppm
    WebGpuPipelineCache.cppm
    WebGpuRenderPipeline.cppm
    WebGpuComputePipeline.cppm
    WebGpuQuerySet.cppm
    WebGpuCommandBuffer.cppm
    WebGpuRenderPassEncoder.cppm
    WebGpuComputePassEncoder.cppm
    WebGpuRenderBundleEncoder.cppm
    WebGpuCommandEncoder.cppm
    WebGpuCommandPool.cppm
    WebGpuSurface.cppm
    WebGpuSwapChain.cppm
    WebGpuTransferBatch.cppm
    WebGpuFence.cppm
    WebGpuQueue.cppm
    WebGpuDevice.cppm
    WebGpuAdapter.cppm
    WebGpuBackend.cppm)


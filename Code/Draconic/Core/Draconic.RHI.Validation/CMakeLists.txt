# Draconic::RHI.Validation - `draconic.rhi.validation` (a wrapping layer that checks
# RHI API usage and forwards to an inner backend).

add_library(Draconic.RHI.Validation STATIC)
add_library(Draconic::RHI.Validation ALIAS Draconic.RHI.Validation)

target_link_libraries(Draconic.RHI.Validation PUBLIC Draconic::RHI Draconic::Foundation Draconic::Policy)

target_sources(Draconic.RHI.Validation
    PUBLIC
        FILE_SET rhi_validation_modules TYPE CXX_MODULES FILES
            ValidatedAdapter.cppm
            ValidatedFence.cppm
            ValidatedTransferBatch.cppm
            ValidatedRenderBundleEncoder.cppm
            ValidatedCommandEncoder.cppm
            ValidatedComputePassEncoder.cppm
            ValidatedRenderPassEncoder.cppm
            ValidatedCommandPool.cppm
            ValidatedQueue.cppm
            ValidatedSwapChain.cppm
            ValidatedDevice.cppm
            ValidatedBackend.cppm
            ValidatedWiring.cppm
            ValidationModule.cppm)


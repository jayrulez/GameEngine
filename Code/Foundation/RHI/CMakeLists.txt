# Foundation::RHI - the `foundation.rhi` module (the backend-agnostic RHI interface).
# Uses our types (foundation.core) under the minimal-STL, -fno-exceptions and
# -fno-rtti policy. Backends (Null, Vulkan, DX12, WebGPU, Validation) are separate targets.

util_add_engine_library(RHI ALIAS Foundation::RHI)
target_link_libraries(RHI PUBLIC Foundation::Core Foundation::Policy)

# Implementation units, so the headers they need stay out of the interface gcm (for GCC).
# Listed as regular sources, not in the CXX_MODULES file set.
target_sources(RHI PRIVATE
    ResourcesImpl.cpp
    LogImpl.cpp)                   # the process-wide log-sink slot (one per process)

target_sources(RHI
    PUBLIC
        FILE_SET rhi_modules TYPE CXX_MODULES FILES
            Log.cppm
            Enums.cppm
            Depth.cppm
            TextureFormat.cppm
            Types.cppm
            Forward.cppm
            Descriptors.cppm
            ExtDescriptors.cppm
            Resources.cppm
            Commands.cppm
            Extensions.cppm
            Queue.cppm
            SwapChain.cppm
            Device.cppm
            Counters.cppm
            RhiModule.cppm)

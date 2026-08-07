# Draconic::RHI - the `draconic.rhi` module (the backend-agnostic RHI interface).
# Ported from the Draconic RHI, adapted to Draconic types (Draconic.Foundation) and the
# minimal-STL / -fno-exceptions / -fno-rtti policy. Backends (Null, Vulkan, DX12,
# Validation) are separate targets.

add_library(Draconic.RHI STATIC)
add_library(Draconic::RHI ALIAS Draconic.RHI)

target_link_libraries(Draconic.RHI PUBLIC Draconic::Foundation Draconic::Policy)

# Implementation units (headers they need stay OUT of the interface gcm - GCC hygiene).
# Listed as regular sources, NOT in the CXX_MODULES file set.
target_sources(Draconic.RHI PRIVATE
    ResourcesImpl.cpp)

target_sources(Draconic.RHI
    PUBLIC
        FILE_SET rhi_modules TYPE CXX_MODULES FILES
            Log.cppm
            Enums.cppm
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
            RhiModule.cppm)

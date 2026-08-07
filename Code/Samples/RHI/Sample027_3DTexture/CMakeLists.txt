add_executable(DraconicSample027_3DTexture Main.cpp)
target_link_libraries(DraconicSample027_3DTexture PRIVATE Draconic::SamplesFramework Draconic::RHI.Vulkan Draconic::Foundation Draconic::Policy)
target_compile_options(DraconicSample027_3DTexture PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=unused-parameter;-Wno-error=unused-variable;-Wno-error=missing-field-initializers;-Wno-error=misleading-indentation>")
draconic_copy_sdl3_dll(DraconicSample027_3DTexture)

add_executable(DraconicSample010_MSAA Main.cpp)
target_link_libraries(DraconicSample010_MSAA PRIVATE Draconic::SamplesFramework Draconic::RHI.Vulkan Draconic::Foundation Draconic::Policy)
target_compile_options(DraconicSample010_MSAA PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=unused-parameter;-Wno-error=unused-variable;-Wno-error=missing-field-initializers;-Wno-error=misleading-indentation>")
draconic_copy_sdl3_dll(DraconicSample010_MSAA)

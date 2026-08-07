add_executable(DraconicSample022_StencilOutline Main.cpp)
target_link_libraries(DraconicSample022_StencilOutline PRIVATE Draconic::SamplesFramework Draconic::RHI.Vulkan Draconic::Foundation Draconic::Policy)
target_compile_options(DraconicSample022_StencilOutline PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=unused-parameter;-Wno-error=unused-variable;-Wno-error=missing-field-initializers;-Wno-error=misleading-indentation>")
draconic_copy_sdl3_dll(DraconicSample022_StencilOutline)

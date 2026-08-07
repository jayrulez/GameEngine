add_executable(DraconicSample028_ProceduralRT Main.cpp)
target_link_libraries(DraconicSample028_ProceduralRT PRIVATE Draconic::SamplesFramework Draconic::RHI.Vulkan Draconic::Foundation Draconic::Policy)
target_compile_options(DraconicSample028_ProceduralRT PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=unused-parameter;-Wno-error=unused-variable;-Wno-error=missing-field-initializers;-Wno-error=misleading-indentation>")
draconic_copy_sdl3_dll(DraconicSample028_ProceduralRT)

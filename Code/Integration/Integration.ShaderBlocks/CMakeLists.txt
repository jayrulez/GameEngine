# Integration.ShaderBlocks - every engine shader block against its C++ mirror, field by field
# (Specs/shader-blocks-cleanup.md, phase 0): the engine's shaders cooked to SPIR-V as the engine
# cook does, each block's members and offsets read from it, and compared with the structs the
# renderers fill. Cross-collection (the shader cook x the renderers' structs), so it lives here.
# Needs no GPU, unlike Integration.Shaders; it skips when the DXC runtime is unavailable.
if(NOT TARGET Foundation::Shaders)
    return() # no DXC (the web build): nothing to compile the shaders with
endif()
add_executable(Integration.ShaderBlocks TestsMain.cpp ShaderBlockLayoutTests.cpp)
# The renderers whose structs mirror the blocks: Render (and the built-in materials' builder),
# VG.Renderer, and the Engine's terrain and particle renderers.
target_link_libraries(Integration.ShaderBlocks PRIVATE
    Foundation::Render Foundation::Materials Foundation::Shaders Foundation::VFS Foundation::VG.Renderer
    Engine::Particles Engine::Terrain doctest::doctest)
target_compile_definitions(Integration.ShaderBlocks PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.ShaderBlocks PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.ShaderBlocks COMMAND Integration.ShaderBlocks)

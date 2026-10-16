add_executable(Engine.DefaultApp.Tests TestsMain.cpp StandardFactoriesTests.cpp FacadeSurfaceTests.cpp NetworkSpawnTests.cpp
    ProfilerOverlayHostTests.cpp)
target_link_libraries(Engine.DefaultApp.Tests PRIVATE Engine::DefaultApp Engine::Composition
    Foundation::Script.Facades
    Foundation::Fonts.Resource Foundation::Texture.Resource Foundation::Content
    Foundation::Heightfield Foundation::Terrain.Resource
    Foundation::VFS Foundation::Xml Foundation::Scene Foundation::Scene.Resource
    Foundation::Image Foundation::Image.IO Foundation::RHI.TestSupport doctest::doctest)
# The screenshot round trip runs on whichever real backends this build has (never assumed).
if(TARGET Foundation::RHI.Vulkan)
    target_link_libraries(Engine.DefaultApp.Tests PRIVATE Foundation::RHI.Vulkan)
    target_compile_definitions(Engine.DefaultApp.Tests PRIVATE OPTION_HAS_VULKAN)
endif()
if(TARGET Foundation::RHI.WebGPU)
    target_link_libraries(Engine.DefaultApp.Tests PRIVATE Foundation::RHI.WebGPU)
    target_compile_definitions(Engine.DefaultApp.Tests PRIVATE OPTION_HAS_WEBGPU)
endif()
target_compile_definitions(Engine.DefaultApp.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.DefaultApp.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Engine.DefaultApp.Tests COMMAND Engine.DefaultApp.Tests)

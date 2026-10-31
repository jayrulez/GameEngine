# Tools.ShaderPack - headless CLI that cooks the engine shader corpus into a shaders.dpak
# (the WGSL pack the web build ships). Needs DXC and the vendored naga/tint (baked into
# foundation.shaders), so it is a desktop authoring tool, never built for Emscripten.
if(EMSCRIPTEN)
    return()
endif()

add_executable(Tools.ShaderPack main.cpp)
target_link_libraries(Tools.ShaderPack PRIVATE
    Foundation::Core
    Foundation::Shaders
    Foundation::Policy)

set_target_properties(Tools.ShaderPack PROPERTIES OUTPUT_NAME ${BUILDSYSTEM_SHADERPACKCOOK_BASENAME})

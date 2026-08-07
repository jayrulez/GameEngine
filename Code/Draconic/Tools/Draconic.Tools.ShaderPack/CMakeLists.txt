# Draconic.Tools.ShaderPack - headless CLI that cooks the engine shader corpus into a shaders.dpak
# (the WGSL pack the web build ships). Needs DXC + the vendored naga/tint (baked into
# draconic.shaders), so it is a desktop authoring tool - never built for Emscripten.
if(EMSCRIPTEN)
    return()
endif()

add_executable(Draconic.Tools.ShaderPack main.cpp)
target_link_libraries(Draconic.Tools.ShaderPack PRIVATE
    Draconic::Foundation
    Draconic::Shaders
    Draconic::Policy)

set_target_properties(Draconic.Tools.ShaderPack PROPERTIES OUTPUT_NAME ${DRACONIC_SHADERPACKCOOK_BASENAME})

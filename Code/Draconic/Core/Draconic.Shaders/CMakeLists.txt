# Draconic::Shaders - `draconic.shaders` (HLSL compilation via DXC). The Compiler is
# a third-party edge (DXC's wide-char API), so it uses std strings/containers;
# the public Types use Draconic types.

if(NOT TARGET Draconic::DXCHeaders)
    message(STATUS "Skipping Draconic::Shaders - DXC headers not available")
    return()
endif()

add_library(Draconic.Shaders STATIC)
add_library(Draconic::Shaders ALIAS Draconic.Shaders)

target_link_libraries(Draconic.Shaders
    PUBLIC Draconic::Foundation Draconic::DXCHeaders Draconic::Policy Draconic::ShaderCookTools
    PRIVATE ${CMAKE_DL_LIBS})

target_sources(Draconic.Shaders
    PUBLIC
        FILE_SET shaders_modules TYPE CXX_MODULES FILES
            Types.cppm
            Flags.cppm
            Variants.cppm
            Pack.cppm
            Compiler.cppm
            Cook/WgslTranslator.cppm
            Cook/ShaderPackCooker.cppm
            ShadersModule.cppm)

# The shader system (variant compile-on-demand + cache) needs the RHI to create


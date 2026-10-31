# Foundation::Shaders - `foundation.shaders` (HLSL compilation via DXC). The Compiler is
# a third-party edge (DXC's wide-char API), so it uses std strings/containers;
# the public Types use engine types.

if(NOT TARGET ThirdParty::DXCHeaders)
    message(STATUS "Skipping Foundation::Shaders - DXC headers not available")
    return()
endif()

util_add_engine_library(Shaders ALIAS Foundation::Shaders)
target_link_libraries(Shaders
    PUBLIC Foundation::Core ThirdParty::DXCHeaders Foundation::Policy ThirdParty::ShaderCookTools
    PRIVATE ${CMAKE_DL_LIBS})

target_sources(Shaders
    PUBLIC
        FILE_SET shaders_modules TYPE CXX_MODULES FILES
            Types.cppm
            Flags.cppm
            Variants.cppm
            Properties.cppm
            Pack.cppm
            Compiler.cppm
            Cook/WgslTranslator.cppm
            Cook/ShaderPackCooker.cppm
            ShadersModule.cppm)



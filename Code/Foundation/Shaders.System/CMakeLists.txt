# Foundation::Shaders.System - `foundation.shaders.system` (variant compile on demand and cache,
# plus the dev file-backed source provider). Needs the RHI (to create GPU modules) on
# top of the RHI-free foundation.shaders; the VFS for the file provider.

util_add_engine_library(Shaders.System ALIAS Foundation::Shaders.System)
target_link_libraries(Shaders.System
    PUBLIC Foundation::Core Foundation::RHI Foundation::Shaders Foundation::VFS Foundation::Policy)

target_sources(Shaders.System
    PUBLIC
        FILE_SET shader_system_modules TYPE CXX_MODULES FILES
            ShadersSystemModule.cppm
            ShaderSystem.cppm
            FileShaderSourceProvider.cppm
            ShaderSystemHost.cppm)


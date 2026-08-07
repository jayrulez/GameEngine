# Draconic::Engine.Project - `draconic.project`, the runtime-side project definition (manifest +
# layout constants). Editor-free by design: shipping binaries link this, never the editor.
add_library(Draconic.Engine.Project STATIC)
add_library(Draconic::Engine.Project ALIAS Draconic.Engine.Project)

target_link_libraries(Draconic.Engine.Project
    PUBLIC Draconic::Foundation Draconic::VFS Draconic::Xml.Serialization Draconic::Policy)

target_sources(Draconic.Engine.Project
    PUBLIC
        FILE_SET project_modules TYPE CXX_MODULES FILES
            ProjectModule.cppm)

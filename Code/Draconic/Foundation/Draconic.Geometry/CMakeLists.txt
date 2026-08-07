# Draconic::Geometry - `draconic.geometry`, the engine's runtime mesh format
# (StaticMesh/SkinnedMesh + index buffer + procedural primitives). Pure data on
# top of Foundation; the renderer uploads the raw vertex/index streams.
add_library(Draconic.Geometry STATIC)
add_library(Draconic::Geometry ALIAS Draconic.Geometry)

target_link_libraries(Draconic.Geometry
    PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Geometry
    PUBLIC
        FILE_SET geometry_modules TYPE CXX_MODULES FILES
            Types.cppm
            IndexBuffer.cppm
            Mesh.cppm
            Primitives.cppm
            GeometryModule.cppm)


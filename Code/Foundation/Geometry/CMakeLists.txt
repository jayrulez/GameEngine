# Foundation::Geometry - `foundation.geometry`, the engine's runtime mesh format
# (StaticMesh/SkinnedMesh + index buffer + procedural primitives). Pure data on
# top of Core; the renderer uploads the raw vertex/index streams.
util_add_engine_library(Geometry ALIAS Foundation::Geometry)
target_link_libraries(Geometry
    PUBLIC Foundation::Core Foundation::Policy)

target_sources(Geometry
    PUBLIC
        FILE_SET geometry_modules TYPE CXX_MODULES FILES
            Types.cppm
            IndexBuffer.cppm
            Mesh.cppm
            Primitives.cppm
            MeshRaycast.cppm
            GeometryModule.cppm)


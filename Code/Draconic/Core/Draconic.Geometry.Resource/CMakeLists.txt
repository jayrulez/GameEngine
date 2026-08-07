# Draconic::Geometry.Resource - `draconic.geometry.resource` (meshes as resources:
# Static/SkinnedMeshSource -> Static/SkinnedMesh via factories).
add_library(Draconic.Geometry.Resource STATIC)
add_library(Draconic::Geometry.Resource ALIAS Draconic.Geometry.Resource)

target_link_libraries(Draconic.Geometry.Resource
    PUBLIC Draconic::Foundation Draconic::Resource Draconic::Content Draconic::Geometry Draconic::Policy)

target_sources(Draconic.Geometry.Resource
    PUBLIC
        FILE_SET mesh_resource_modules TYPE CXX_MODULES FILES
            MeshResource.cppm)


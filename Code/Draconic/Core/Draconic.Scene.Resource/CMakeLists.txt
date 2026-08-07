# Draconic::Scene.Resource - `draconic.scene.resource` (whole-scene serialization +
# runtime LoadScene from a content-DB instance).
add_library(Draconic.Scene.Resource STATIC)
add_library(Draconic::Scene.Resource ALIAS Draconic.Scene.Resource)

target_link_libraries(Draconic.Scene.Resource
    PUBLIC Draconic::Foundation Draconic::Content Draconic::Scene Draconic::Policy
           Draconic::Xml.Serialization)

target_sources(Draconic.Scene.Resource
    PRIVATE
        SceneResourceImpl.cpp   # out-of-line public-API defs (see SceneResource.cppm)
    PUBLIC
        FILE_SET scene_resource_modules TYPE CXX_MODULES FILES
            SceneResource.cppm)


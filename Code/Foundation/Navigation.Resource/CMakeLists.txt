# Foundation::Navigation.Resource - `foundation.navigation.resource` (cooked navmesh zones).
# Shaped like the collision assets: a Source record (serialized navmesh blob) produced by a bake
# action, and a runtime product (a loaded NavigationMesh) a NavMeshZoneComponent's Ref<> binds.
util_add_engine_library(Navigation.Resource ALIAS Foundation::Navigation.Resource)
target_link_libraries(Navigation.Resource PUBLIC
    Foundation::Core Foundation::Resource Foundation::Content Foundation::Policy
    Foundation::Navigation)
target_sources(Navigation.Resource PUBLIC
    FILE_SET navigation_resource_modules TYPE CXX_MODULES FILES NavigationResource.cppm)

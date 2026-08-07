# Draconic::UI.Resource - `draconic.ui.resource` (cooked UI documents + themes; UI-framework-free).
add_library(Draconic.UI.Resource STATIC)
add_library(Draconic::UI.Resource ALIAS Draconic.UI.Resource)
target_link_libraries(Draconic.UI.Resource PUBLIC
    Draconic::Foundation Draconic::Resource Draconic::Content Draconic::Policy)
target_sources(Draconic.UI.Resource PUBLIC FILE_SET ui_resource_modules TYPE CXX_MODULES FILES UIResource.cppm)

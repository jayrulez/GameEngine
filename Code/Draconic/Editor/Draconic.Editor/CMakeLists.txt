# Draconic::Editor - `draconic.editor` (asset-pipeline authoring base: Asset + builders).
# Tooling only - the runtime never links this.

add_library(Draconic.Editor STATIC)
add_library(Draconic::Editor ALIAS Draconic.Editor)

target_link_libraries(Draconic.Editor PUBLIC Draconic::Core Draconic::Content Draconic::VFS Draconic::Policy)

target_sources(Draconic.Editor
    PRIVATE
        AssetImpl.cpp
    PUBLIC
        FILE_SET editor_modules TYPE CXX_MODULES FILES
            Asset.cppm)


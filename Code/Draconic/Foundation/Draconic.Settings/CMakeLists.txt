# Draconic::Settings - `draconic.settings` (typed, versioned, backend-agnostic settings store).
# Sits just above Foundation so both the runtime and the editor can use it. Depends only on Foundation.
# See docs/design/settings.md.

add_library(Draconic.Settings STATIC)
add_library(Draconic::Settings ALIAS Draconic.Settings)

target_link_libraries(Draconic.Settings PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Settings
    PUBLIC
        FILE_SET settings_modules TYPE CXX_MODULES FILES
            Settings.cppm)


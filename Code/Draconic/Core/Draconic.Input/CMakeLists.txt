# Draconic::Input - `draconic.input`: the action-mapping layer (data model + ActionRuntime)
# over Foundation + Shell. Engine-global (per player, not per scene).
add_library(Draconic.Input STATIC)
add_library(Draconic::Input ALIAS Draconic.Input)

target_link_libraries(Draconic.Input
    PUBLIC Draconic::Foundation Draconic::Shell Draconic::Policy)

target_sources(Draconic.Input
    PRIVATE
        InputReflectionImpl.cpp   # input leaf-type + enum reflection (GCC hygiene: out of interfaces)
    PUBLIC
        FILE_SET input_modules TYPE CXX_MODULES FILES
            InputModule.cppm
            InputMap.cppm
            ActionRuntime.cppm)


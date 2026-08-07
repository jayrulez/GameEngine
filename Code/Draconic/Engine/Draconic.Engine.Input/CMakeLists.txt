# Draconic::Engine.Input - `draconic.engine.input`: runtime tick + device provider seam.
add_library(Draconic.Engine.Input STATIC)
add_library(Draconic::Engine.Input ALIAS Draconic.Engine.Input)

target_link_libraries(Draconic.Engine.Input
    PUBLIC Draconic::Foundation Draconic::Shell Draconic::Runtime Draconic::Script Draconic::Script.Facades Draconic::Input Draconic::Policy)

target_sources(Draconic.Engine.Input
    PRIVATE
        InputSubsystemImpl.cpp   # DRACONIC_REFLECT body (see InputSubsystem.cppm)
    PUBLIC
        FILE_SET input_subsystem_modules TYPE CXX_MODULES FILES
            InputSubsystem.cppm)

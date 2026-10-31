# Engine::Input - `engine.input`: the runtime input tick and the device provider.
util_add_engine_library(Engine.Input ALIAS Engine::Input)
target_link_libraries(Engine.Input
    PUBLIC Foundation::Core Foundation::Shell Foundation::Runtime Foundation::Script Foundation::Script.Facades Foundation::Input Engine::Domain Foundation::Input.Resource Foundation::Policy)

target_sources(Engine.Input
    PRIVATE
        InputSubsystemImpl.cpp   # REFLECT_MEMBERS body (see InputSubsystem.cppm)
        FittedInputSourceImpl.cpp # the fitted devices' members, so their vtables stay in this module
    PUBLIC
        FILE_SET input_subsystem_modules TYPE CXX_MODULES FILES
            InputSubsystem.cppm
            FittedInputSource.cppm)

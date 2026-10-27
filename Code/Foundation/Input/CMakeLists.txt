# Foundation::Input - `foundation.input`: the action-mapping layer (data model + ActionRuntime)
# over Core + Shell. Engine-global (per player, not per scene).
util_add_engine_library(Input ALIAS Foundation::Input)
target_link_libraries(Input
    PUBLIC Foundation::Core Foundation::Shell Foundation::Json Foundation::Policy)

target_sources(Input
    PRIVATE
        InputReflectionImpl.cpp   # input leaf-type + enum reflection (GCC hygiene: out of interfaces)
        ScriptedInputImpl.cpp     # ScriptedInputSource (a playtest's timeline)
        ScriptedInputJsonImpl.cpp # the timeline read from JSON (the playtest tools')
    PUBLIC
        FILE_SET input_modules TYPE CXX_MODULES FILES
            InputModule.cppm
            InputMap.cppm
            ActionRuntime.cppm
            BindingNames.cppm
            ScriptedInput.cppm
            ScriptedInputJson.cppm
            InputKind.cppm)


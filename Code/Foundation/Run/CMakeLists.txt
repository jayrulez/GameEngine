# Foundation::Run - `foundation.run`: a run (one running game) and its manager, as interfaces
# (IRun, IRunManager, IRunListener, IRunService) with one implementation each (RunBase,
# RunRegistry). Over Core and Messaging only: the runtime and the scenes depend on it, never the
# other way.

util_add_engine_library(Run ALIAS Foundation::Run)
target_link_libraries(Run PUBLIC Foundation::Core Foundation::Messaging Foundation::Policy)

target_sources(Run
    PRIVATE
        RunImpl.cpp
    PUBLIC
        FILE_SET run_modules TYPE CXX_MODULES FILES
            Run.cppm)

# Experimental::GUI.Pipeline - `experimental.gui.pipeline`: cooking and validating GUI documents and themes (tools only, never in a web or shipping build).
# foundation.ui's counterpart: UI.Pipeline. Specs/gui-lab-evolution-3.md.
util_add_engine_library(GUI.Pipeline ALIAS Experimental::GUI.Pipeline)
target_link_libraries(GUI.Pipeline
    PUBLIC Foundation::Core Experimental::GUI Foundation::Policy)

target_sources(GUI.Pipeline
    PUBLIC
        FILE_SET guipipeline_modules TYPE CXX_MODULES FILES
            PipelineModule.cppm)

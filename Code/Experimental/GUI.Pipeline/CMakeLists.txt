# Experimental::GUI.Pipeline - `experimental.gui.pipeline`: the lab GUI's documents (.gml) and style
# sheets (.css) as linked-source assets, their validating builders, importer and creators. Tools
# only, never in a web or shipping build. foundation.ui's counterpart: UI.Pipeline.
# Specs/gui-lab-evolution-3.md, E3.
util_add_engine_library(GUI.Pipeline ALIAS Experimental::GUI.Pipeline)
target_link_libraries(GUI.Pipeline
    PUBLIC Foundation::Core Foundation::Content Pipeline::Core Pipeline::Importer
           Experimental::GUI Experimental::GUI.Gamekit Experimental::GUI.Toolkit Experimental::GUI.Code Experimental::GUI.Resources
           Foundation::Policy)

target_sources(GUI.Pipeline
    PRIVATE
        GuiAssetImpl.cpp    # the assets' reflection
        GuiCreatorsImpl.cpp # File > New, asset_create
    PUBLIC
        FILE_SET guipipeline_modules TYPE CXX_MODULES FILES
            PipelineModule.cppm)

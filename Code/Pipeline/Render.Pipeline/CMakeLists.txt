# Pipeline::Render - `render.pipeline` (tooling: the render profile assets and their builders).
util_add_engine_library(Render.Pipeline ALIAS Pipeline::Render)
target_link_libraries(Render.Pipeline PUBLIC
    Foundation::Core Pipeline::Core Foundation::Content Engine::Render Foundation::Policy)
target_sources(Render.Pipeline
    PRIVATE
        RenderCreatorsImpl.cpp # File > New, asset_create
        RenderProfileAssetImpl.cpp # the profiles' reflection
    PUBLIC FILE_SET render_pipeline_modules TYPE CXX_MODULES FILES RenderProfileAsset.cppm)

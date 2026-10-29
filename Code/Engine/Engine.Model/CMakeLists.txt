# Engine::Model - `engine.model`: a cooked model put into a scene as entities (meshes with their
# materials, a skeletal animation on the root), for hosts that cook a model file at run time (the
# Sandbox sample, the model viewer).

util_add_engine_library(Engine.Model ALIAS Engine::Model)
target_link_libraries(Engine.Model
    PUBLIC Foundation::Core Foundation::Scene Foundation::Geometry Foundation::Materials
           Foundation::Animation Foundation::Model.Resource Engine::Render Engine::Animation
           Foundation::Policy)

target_sources(Engine.Model
    PUBLIC
        FILE_SET engine_model_modules TYPE CXX_MODULES FILES
            ModelInstance.cppm)

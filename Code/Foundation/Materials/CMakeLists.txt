# Foundation::Materials - `foundation.materials`, the data-driven material model
# (PipelineConfig, Material/Builder/Instance, and the bind-group-inferring
# MaterialSystem). Needs the RHI (GPU resources) and Shaders (ShaderFlags).
util_add_engine_library(Materials ALIAS Foundation::Materials)
target_link_libraries(Materials
    PUBLIC Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Policy)

target_sources(Materials
    PRIVATE
        TypesReflectionImpl.cpp   # render-state enum reflection (kept out of the interface for GCC)
    PUBLIC
        FILE_SET materials_modules TYPE CXX_MODULES FILES
            Types.cppm
            PipelineConfig.cppm
            Material.cppm
            MaterialBuilder.cppm
            MaterialInstance.cppm
            MaterialSystem.cppm
            MaterialsModule.cppm)


# Engine::Spline - `engine.spline`: the authorable 3D spline scene component (data-only; no
# subsystem, no tick). The curve math lives in Foundation::Spline; consumers and the editor
# tool evaluate and author through the component.

util_add_engine_library(Engine.Spline ALIAS Engine::Spline)
target_link_libraries(Engine.Spline PUBLIC
    Foundation::Core Foundation::Scene Foundation::Spline Foundation::Script.Facades Engine::Domain Foundation::Policy)

target_sources(Engine.Spline
    PRIVATE
        SplineComponentsImpl.cpp
    PUBLIC
        FILE_SET engine_spline_modules TYPE CXX_MODULES FILES
            SplineComponents.cppm)

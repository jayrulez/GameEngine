# Engine::UI - `engine.ui`: the game screen tier (canvases + overlay render + consumption).
util_add_engine_library(Engine.UI ALIAS Engine::UI)
target_link_libraries(Engine.UI
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Resource Foundation::Shell Foundation::RHI Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Resource
           Foundation::Input Engine::Input Foundation::UI Foundation::UI.Shell Foundation::UI.Gamekit
           Foundation::UI.Resource
           Foundation::Script Foundation::Script.Facades Engine::UI.Script
           Foundation::VG Foundation::VG.Renderer Foundation::Shaders Foundation::Shaders.System
           Foundation::Render Engine::Render Foundation::Image Foundation::Texture.Resource Foundation::Profiler Foundation::VFS Engine::Domain Foundation::Policy)
target_sources(Engine.UI
    PRIVATE
        UISubsystemImpl.cpp   # implementation unit: VG/shader contact + reflection bodies
    PUBLIC
        FILE_SET ui_subsystem_modules TYPE CXX_MODULES FILES
            UISubsystem.cppm)


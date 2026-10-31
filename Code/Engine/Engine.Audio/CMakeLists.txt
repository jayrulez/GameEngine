# Engine::Audio - `engine.audio`: scene integration
# (components, a per-scene voice group, transform and listener sync, the one-shot API).
util_add_engine_library(Engine.Audio ALIAS Engine::Audio)
target_link_libraries(Engine.Audio
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Audio Foundation::Resource Foundation::Content Foundation::Script
           Foundation::Script.Facades Foundation::Settings Engine::Render Foundation::Profiler Engine::Domain Foundation::Audio.Resource Foundation::Policy)

target_sources(Engine.Audio
    PRIVATE
        AudioSubsystemImpl.cpp   # implementation unit: listener push and reflection (the render dependency stays out of the interface)
    PUBLIC
        FILE_SET audio_subsystem_modules TYPE CXX_MODULES FILES
            AudioSubsystem.cppm
            AudioComponents.cppm)


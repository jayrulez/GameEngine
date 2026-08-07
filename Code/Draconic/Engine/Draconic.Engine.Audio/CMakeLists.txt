# Draconic::Engine.Audio - `draconic.engine.audio`: scene integration
# (components + per-scene voice group + transform/listener sync + one-shot API).
add_library(Draconic.Engine.Audio STATIC)
add_library(Draconic::Engine.Audio ALIAS Draconic.Engine.Audio)

target_link_libraries(Draconic.Engine.Audio
    PUBLIC Draconic::Foundation Draconic::Runtime Draconic::Scene Draconic::Engine.Scene
           Draconic::Audio Draconic::Resource Draconic::Content Draconic::Script
           Draconic::Script.Facades Draconic::Settings Draconic::Engine.Render Draconic::Policy)

target_sources(Draconic.Engine.Audio
    PRIVATE
        AudioSubsystemImpl.cpp   # implementation unit: listener push + reflection (render dep stays out of the interface)
    PUBLIC
        FILE_SET audio_subsystem_modules TYPE CXX_MODULES FILES
            AudioSubsystem.cppm
            AudioComponents.cppm)


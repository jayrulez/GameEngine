# Draconic::Editor.Audio - `draconic.editor.audio`: audio clip audition page (waveform + play).
add_library(Draconic.Editor.Audio STATIC)
add_library(Draconic::Editor.Audio ALIAS Draconic.Editor.Audio)
target_link_libraries(Draconic.Editor.Audio PUBLIC
    Draconic::Foundation Draconic::Content Draconic::Runtime.Client Draconic::UI Draconic::UI.Toolkit
    Draconic::Audio Draconic::Audio.Editor Draconic::Engine.Audio
    Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)
target_sources(Draconic.Editor.Audio PRIVATE SoundCuePageImpl.cpp AudioClipPageImpl.cpp BusLayoutPageImpl.cpp PUBLIC FILE_SET editor_audio_modules TYPE CXX_MODULES FILES AudioClipPage.cppm SoundCuePage.cppm BusLayoutPage.cppm)


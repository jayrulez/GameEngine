# Editor::Audio - `editor.audio`: the audio clip's audition page (its waveform, the kit's transport,
# its options) and the sound cue page, on the GUI, and the clip thumbnails. The bus layout page
# (BusLayoutPage*) comes back in its own part of X6's layer 5: out of the build until then.
util_add_engine_library(Editor.Audio ALIAS Editor::Audio)
target_link_libraries(Editor.Audio PUBLIC
    Foundation::Core Foundation::Content Foundation::Runtime.Client Foundation::GUI Foundation::GUI.Toolkit
    Foundation::Audio Pipeline::Audio Engine::Audio Foundation::Image Foundation::Image.IO Foundation::VFS
    Editor::Core Editor::App Foundation::Policy)
target_sources(Editor.Audio
    PRIVATE
        AudioClipPageImpl.cpp SoundCuePageImpl.cpp
    PUBLIC
        FILE_SET editor_audio_modules TYPE CXX_MODULES FILES
            AudioClipPage.cppm Audition.cppm WaveformView.cppm SoundCuePage.cppm AudioClipThumbnailGenerator.cppm)

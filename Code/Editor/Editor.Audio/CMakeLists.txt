# Editor::Audio - `editor.audio`: the audio clip's audition page (its waveform, the kit's transport,
# its options), on the GUI, and the clip thumbnails. The sound cue and the bus layout pages
# (SoundCuePage*, BusLayoutPage*) come back in their own parts of X6's layer 5: out of the build
# until then.
util_add_engine_library(Editor.Audio ALIAS Editor::Audio)
target_link_libraries(Editor.Audio PUBLIC
    Foundation::Core Foundation::Content Foundation::Runtime.Client Foundation::GUI Foundation::GUI.Toolkit
    Foundation::Audio Pipeline::Audio Engine::Audio Foundation::Image Foundation::Image.IO Foundation::VFS
    Editor::Core Editor::App Foundation::Policy)
target_sources(Editor.Audio
    PRIVATE
        AudioClipPageImpl.cpp
    PUBLIC
        FILE_SET editor_audio_modules TYPE CXX_MODULES FILES
            AudioClipPage.cppm Audition.cppm WaveformView.cppm AudioClipThumbnailGenerator.cppm)

# Editor::Audio - `editor.audio`: the audio pages on the GUI (the clip's audition page, the sound
# cue page, the bus layout page) and the clip thumbnails.
util_add_engine_library(Editor.Audio ALIAS Editor::Audio)
target_link_libraries(Editor.Audio PUBLIC
    Foundation::Core Foundation::Content Foundation::Runtime.Client Foundation::GUI Foundation::GUI.Toolkit
    Foundation::Audio Pipeline::Audio Engine::Audio Foundation::Image Foundation::Image.IO Foundation::VFS
    Editor::Core Editor::App Foundation::Policy)
target_sources(Editor.Audio
    PRIVATE
        AudioClipPageImpl.cpp SoundCuePageImpl.cpp BusLayoutPageImpl.cpp
    PUBLIC
        FILE_SET editor_audio_modules TYPE CXX_MODULES FILES
            AudioClipPage.cppm Audition.cppm WaveformView.cppm SoundCuePage.cppm BusLayoutModel.cppm BusLayoutPage.cppm
            AudioClipThumbnailGenerator.cppm)

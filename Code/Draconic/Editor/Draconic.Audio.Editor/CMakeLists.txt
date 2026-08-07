# Draconic::Audio.Editor - `draconic.audio.editor` (tooling: audio clip asset/builder/importer).
add_library(Draconic.Audio.Editor STATIC)
add_library(Draconic::Audio.Editor ALIAS Draconic.Audio.Editor)
target_link_libraries(Draconic.Audio.Editor PUBLIC
    Draconic::Foundation Draconic::Editor Draconic::Editor.Core Draconic::Content
    Draconic::Audio Draconic::Audio.Resource Draconic::Policy)
target_sources(Draconic.Audio.Editor
    PRIVATE
        AudioAssetImpl.cpp
    PUBLIC FILE_SET audio_editor_modules TYPE CXX_MODULES FILES AudioAsset.cppm)


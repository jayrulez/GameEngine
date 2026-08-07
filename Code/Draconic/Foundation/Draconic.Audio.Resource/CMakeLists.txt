# Draconic::Audio.Resource - `draconic.audio.resource` (cooked audio clips + factory).
add_library(Draconic.Audio.Resource STATIC)
add_library(Draconic::Audio.Resource ALIAS Draconic.Audio.Resource)
target_link_libraries(Draconic.Audio.Resource PUBLIC
    Draconic::Foundation Draconic::Resource Draconic::Content Draconic::Audio Draconic::Policy)
target_sources(Draconic.Audio.Resource PUBLIC FILE_SET audio_resource_modules TYPE CXX_MODULES FILES AudioResource.cppm)

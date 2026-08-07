# Draconic::Audio - `draconic.audio`: the miniaudio-backed audio engine core.
# miniaudio is PRIVATE: ma_* types never cross the module surface.
add_library(Draconic.Audio STATIC)
add_library(Draconic::Audio ALIAS Draconic.Audio)

target_link_libraries(Draconic.Audio
    PUBLIC Draconic::Foundation Draconic::VFS Draconic::Policy
    PRIVATE Draconic::Miniaudio)

target_sources(Draconic.Audio
    PRIVATE
        AudioEngineImpl.cpp      # module IMPLEMENTATION unit: all miniaudio contact (see AudioEngine.cppm)
        MiniaudioImpl.cpp   # plain TU: MINIAUDIO_IMPLEMENTATION + stb_vorbis (exactly once)
    PUBLIC
        FILE_SET audio_modules TYPE CXX_MODULES FILES
            AudioModule.cppm
            AudioClip.cppm
            SoundCue.cppm
            AudioEngine.cppm
            AudioReverb.cppm)

# stb_vorbis.c is included from ThirdParty/stb (no INTERFACE target carries that dir for
# sources; add it directly for the implementation TU).
target_include_directories(Draconic.Audio PRIVATE ${CMAKE_SOURCE_DIR}/ThirdParty/stb)

# Vendored implementation code builds without our -Werror strictness.
set_source_files_properties(MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "-Wno-error")


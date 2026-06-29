# Raptor::FontsDF — `raptor.fonts.df` (distance-field font atlas, runtime data type).
# No msdfgen dependency; the import-time baker is in Baker/.

add_library(RaptorFontsDF STATIC)
add_library(Raptor::FontsDF ALIAS RaptorFontsDF)

target_link_libraries(RaptorFontsDF PUBLIC Raptor::Core Raptor::Fonts Raptor::Policy)

target_sources(RaptorFontsDF
    PUBLIC
        FILE_SET fonts_df_modules TYPE CXX_MODULES FILES
            DFFontAtlas.cppm
            FontsDFModule.cppm)

if(TARGET Raptor::MsdfgenCore)
    add_subdirectory(Baker)
endif()

if(RAPTOR_BUILD_TESTS)
    add_subdirectory(Tests)
endif()

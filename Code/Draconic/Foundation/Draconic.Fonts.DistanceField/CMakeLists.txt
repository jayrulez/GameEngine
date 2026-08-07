# Draconic::Fonts.DistanceField - `draconic.fonts.distancefield` (distance-field font atlas, runtime data type).
# No msdfgen dependency; the import-time baker is in Baker/.

add_library(Draconic.Fonts.DistanceField STATIC)
add_library(Draconic::Fonts.DistanceField ALIAS Draconic.Fonts.DistanceField)

target_link_libraries(Draconic.Fonts.DistanceField PUBLIC Draconic::Foundation Draconic::Fonts Draconic::Policy)

target_sources(Draconic.Fonts.DistanceField
    PUBLIC
        FILE_SET fonts_df_modules TYPE CXX_MODULES FILES
            DFFontAtlas.cppm
            FontsDFModule.cppm)

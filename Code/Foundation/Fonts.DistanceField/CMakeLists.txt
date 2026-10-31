# Foundation::Fonts.DistanceField - `foundation.fonts.distancefield` (distance-field font atlas, runtime data type).
# No msdfgen dependency; the import-time baker is Fonts.DistanceField.Baker.

util_add_engine_library(Fonts.DistanceField ALIAS Foundation::Fonts.DistanceField)
target_link_libraries(Fonts.DistanceField PUBLIC Foundation::Core Foundation::Fonts Foundation::Policy)

target_sources(Fonts.DistanceField
    PUBLIC
        FILE_SET fonts_distancefield_modules TYPE CXX_MODULES FILES
            DistanceFieldFontAtlas.cppm
            FontsDistanceFieldModule.cppm)

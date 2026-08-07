# Draconic::Fonts.Importer - `draconic.fonts.importer` (editor-time TTF -> baked).
# Ported from Sedulous.Fonts.Importer (deps: Fonts, Fonts.Baked, Fonts.TTF).

add_library(Draconic.Fonts.Importer STATIC)
add_library(Draconic::Fonts.Importer ALIAS Draconic.Fonts.Importer)

target_link_libraries(Draconic.Fonts.Importer
    PUBLIC Draconic::Foundation Draconic::Fonts Draconic::Fonts.Baked Draconic::Fonts.TTF Draconic::Policy)

target_sources(Draconic.Fonts.Importer
    PUBLIC
        FILE_SET fonts_importer_modules TYPE CXX_MODULES FILES
            FontImporter.cppm)


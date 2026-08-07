# Draconic::Fonts.Resource - `draconic.fonts.resource` (the cooked font record + runtime
# Font product + FontFactory - the triad tier Sedulous.Fonts.Resources provided). Device-
# free: atlas images stay CPU-side; the VG layer uploads them like any font texture.

add_library(Draconic.Fonts.Resource STATIC)
add_library(Draconic::Fonts.Resource ALIAS Draconic.Fonts.Resource)

target_link_libraries(Draconic.Fonts.Resource
    PUBLIC Draconic::Foundation Draconic::Fonts Draconic::Fonts.Baked Draconic::Fonts.TTF Draconic::Fonts.DistanceField
           Draconic::Image Draconic::Content Draconic::Resource Draconic::Policy)

target_sources(Draconic.Fonts.Resource
    PUBLIC
        FILE_SET fonts_resource_modules TYPE CXX_MODULES FILES
            FontResource.cppm)

# Foundation::Fonts.Resource - `foundation.fonts.resource` (the cooked font record, the runtime
# Font product and FontFactory). Device-free: atlas images stay CPU-side; the VG layer uploads
# them like any font texture.

util_add_engine_library(Fonts.Resource ALIAS Foundation::Fonts.Resource)
target_link_libraries(Fonts.Resource
    PUBLIC Foundation::Core Foundation::Fonts Foundation::Fonts.Coverage Foundation::Fonts.TrueType Foundation::Fonts.DistanceField
           Foundation::Image Foundation::Content Foundation::Resource Foundation::Policy)

target_sources(Fonts.Resource
    PUBLIC
        FILE_SET fonts_resource_modules TYPE CXX_MODULES FILES
            FontResource.cppm)

# Foundation::VG.Resource - `foundation.vg.resource` (cooked vector images: validated SVG documents;
# free of the renderer and of any UI framework, so a headless tool or either GUI reads them).
util_add_engine_library(VG.Resource ALIAS Foundation::VG.Resource)
target_link_libraries(VG.Resource PUBLIC
    Foundation::Core Foundation::Resource Foundation::Content Foundation::Policy)
target_sources(VG.Resource PUBLIC FILE_SET vg_resource_modules TYPE CXX_MODULES FILES VGResource.cppm)

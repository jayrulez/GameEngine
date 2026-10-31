# Foundation::Heightfield - `foundation.heightfield`, the pure heightfield grid and sampling math
# (u16 square grid, bilinear height/normal, world<->grid, ray march, cell bounds). The shared source
# of truth for the terrain renderer, the physics collider, and the nav bake. Core only, no RHI.
util_add_engine_library(Heightfield ALIAS Foundation::Heightfield)
target_link_libraries(Heightfield
    PUBLIC Foundation::Core Foundation::Policy)

target_sources(Heightfield
    PUBLIC
        FILE_SET heightfield_modules TYPE CXX_MODULES FILES
            Heightfield.cppm)

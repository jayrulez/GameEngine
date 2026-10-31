# Foundation::Lod - `foundation.lod`: level-of-detail selection math (projected-sphere
# coverage, threshold walk, hysteresis) over Core. A leaf library of its own because LOD is
# a domain concept that core math should not know about. It is RHI-free, so foundation.render
# (mesh chains) and foundation.terrain (chunk geo-mip) share one formula from either side of
# the RHI.

util_add_engine_library(Lod ALIAS Foundation::Lod)
target_link_libraries(Lod PUBLIC Foundation::Core Foundation::Policy)

target_sources(Lod
    PUBLIC
        FILE_SET lod_modules TYPE CXX_MODULES FILES
            LodMath.cppm)

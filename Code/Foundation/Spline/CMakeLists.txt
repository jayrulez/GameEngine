# Foundation::Spline - `foundation.spline`: authorable 3D spline math (cubic-Bezier-native
# points with Auto/Smooth/Broken handle modes, arc length, closest point) over Core. A leaf
# under Core, like Foundation::Lod, because it is domain math core should not know about.
# RHI-free; the scene component, the editor tool and any other consumer share this one curve.

util_add_engine_library(Spline ALIAS Foundation::Spline)
target_link_libraries(Spline PUBLIC Foundation::Core Foundation::Policy)

target_sources(Spline
    PRIVATE
        SplineImpl.cpp
    PUBLIC
        FILE_SET spline_modules TYPE CXX_MODULES FILES
            Spline.cppm)

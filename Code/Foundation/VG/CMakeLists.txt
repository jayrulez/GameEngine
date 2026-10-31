# Foundation::VG - `foundation.vg` (2D vector-graphics core: paths, tessellation, context).
# Pure CPU: emits a VGBatch of vertices and commands that foundation.vg.renderer uploads
# to the GPU. Depends on Core (math, containers) and Image.

util_add_engine_library(VG ALIAS Foundation::VG)
target_link_libraries(VG PUBLIC Foundation::Core Foundation::Image Foundation::Fonts Foundation::Policy)

target_sources(VG
    PUBLIC
        FILE_SET vg_modules TYPE CXX_MODULES FILES
            Enums.cppm
            Vertex.cppm
            Fills.cppm
            Style.cppm
            Curves.cppm
            Path.cppm
            Shapes.cppm
            Tessellation.cppm
            Batch.cppm
            Cache.cppm
            Context.cppm
            VGModule.cppm)
target_sources(VG PRIVATE VGImpl.cpp)


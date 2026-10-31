# Foundation::RenderGraph - `foundation.rendergraph` (render graph over the RHI:
# pass dependency resolution, transient resource aliasing, automatic barriers).

util_add_engine_library(RenderGraph ALIAS Foundation::RenderGraph)
target_link_libraries(RenderGraph PUBLIC Foundation::Core Foundation::RHI Foundation::Profiler Foundation::Policy)

target_sources(RenderGraph
    PUBLIC
        FILE_SET render_graph_modules TYPE CXX_MODULES FILES
            Types.cppm
            Callbacks.cppm
            Descriptors.cppm
            PersistentResource.cppm
            RenderGraphResource.cppm
            RenderGraphPass.cppm
            SubresourceStateTracker.cppm
            BarrierSolver.cppm
            PassBuilder.cppm
            TransientTexturePool.cppm
            RenderGraph.cppm
            GraphProfiler.cppm
            GraphValidator.cppm
            GraphDebug.cppm
            RenderGraphModule.cppm)


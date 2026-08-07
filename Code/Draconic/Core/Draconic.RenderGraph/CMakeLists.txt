# Draconic::RenderGraph - `draconic.rendergraph` (render graph over the RHI:
# pass dependency resolution, transient resource aliasing, automatic barriers).
# Ported from Sedulous.RenderGraph.

add_library(Draconic.RenderGraph STATIC)
add_library(Draconic::RenderGraph ALIAS Draconic.RenderGraph)

target_link_libraries(Draconic.RenderGraph PUBLIC Draconic::Foundation Draconic::RHI Draconic::Policy)

target_sources(Draconic.RenderGraph
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


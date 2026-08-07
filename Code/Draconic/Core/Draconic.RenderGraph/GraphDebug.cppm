// Draconic::RenderGraph - :debug partition
//
// Debug visualization/reporting: Graphviz DOT export and a text summary. Ported
// from Sedulous.RenderGraph (GraphDebug.bf).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:debug;

import draconic.foundation;
import :types;
import :pass;
import :resource;
import :graph;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace detail
    {
        [[nodiscard]] inline StringView PassColor(RGPassType type)
        {
            switch (type)
            {
            case RGPassType::Render:
                return u8"#4488cc";
            case RGPassType::Compute:
                return u8"#cc8844";
            case RGPassType::Copy:
                return u8"#44aa44";
            }
            return u8"#888888";
        }
        [[nodiscard]] inline StringView LifetimeLabel(RGResourceLifetime lifetime)
        {
            switch (lifetime)
            {
            case RGResourceLifetime::Transient:
                return u8"transient";
            case RGResourceLifetime::Persistent:
                return u8"persistent";
            case RGResourceLifetime::Imported:
                return u8"imported";
            }
            return u8"?";
        }
        [[nodiscard]] inline StringView AccessLabel(RGAccessType type)
        {
            switch (type)
            {
            case RGAccessType::ReadTexture:
                return u8"read";
            case RGAccessType::ReadBuffer:
                return u8"read";
            case RGAccessType::ReadDepthStencil:
                return u8"depth-read";
            case RGAccessType::SampleDepthStencil:
                return u8"depth-sample";
            case RGAccessType::ReadCopySrc:
                return u8"copy-src";
            case RGAccessType::WriteColorTarget:
                return u8"color-out";
            case RGAccessType::WriteDepthTarget:
                return u8"depth-out";
            case RGAccessType::WriteStorage:
                return u8"storage-write";
            case RGAccessType::WriteCopyDst:
                return u8"copy-dst";
            case RGAccessType::ReadWriteStorage:
                return u8"rw-storage";
            case RGAccessType::ReadWriteDepthTarget:
                return u8"depth-rw";
            case RGAccessType::ReadWriteColorTarget:
                return u8"color-rw";
            }
            return u8"?";
        }
        [[nodiscard]] inline StringView PassTypeLabel(RGPassType type)
        {
            switch (type)
            {
            case RGPassType::Render:
                return u8"Render";
            case RGPassType::Compute:
                return u8"Compute";
            case RGPassType::Copy:
                return u8"Copy";
            }
            return u8"?";
        }
    }

    class GraphDebug
    {
    public:
        // Graphviz DOT: pass nodes (boxes) + resource nodes (ellipse/diamond) +
        // access edges. Culled passes/edges are dashed/gray.
        static void ExportDOT(RenderGraph& graph, String& out)
        {
            const Array<RenderGraphPass*>& passes = graph.Passes();
            const Array<RenderGraphResource*>& resources = graph.Resources();

            out.Append(u8"digraph RenderGraph {\n");
            out.Append(u8"  rankdir=LR;\n");
            out.Append(u8"  node [fontname=\"Helvetica\"];\n\n");

            for (usize i = 0; i < passes.Size(); ++i)
            {
                RenderGraphPass* pass = passes[i];
                const StringView style =
                    pass->isCulled ? StringView(u8"dashed") : StringView(u8"filled");
                const StringView fontColor =
                    pass->isCulled ? StringView(u8"gray") : StringView(u8"white");
                AppendFormat(
                    out,
                    u8"  pass{} [label=\"{}\" shape=box style={} fillcolor=\"{}\" fontcolor=\"{}\"",
                    i, pass->name.AsView(), style, detail::PassColor(pass->type), fontColor);
                if (pass->isCulled)
                {
                    out.Append(u8" color=gray");
                }
                out.Append(u8"];\n");
            }
            out.Append(u8"\n");

            for (usize i = 0; i < resources.Size(); ++i)
            {
                RenderGraphResource* res = resources[i];
                if (res == nullptr)
                {
                    continue;
                }
                const StringView shape = res->resourceType == RGResourceType::Texture
                                             ? StringView(u8"ellipse")
                                             : StringView(u8"diamond");
                AppendFormat(out, u8"  res{} [label=\"{}\\n({})\" shape={}];\n", i,
                             res->name.AsView(), detail::LifetimeLabel(res->lifetime), shape);
            }
            out.Append(u8"\n");

            for (usize passIdx = 0; passIdx < passes.Size(); ++passIdx)
            {
                RenderGraphPass* pass = passes[passIdx];
                for (const RGResourceAccess& access : pass->accesses)
                {
                    if (!access.handle.IsValid() || access.handle.index >= resources.Size())
                    {
                        continue;
                    }
                    if (resources[access.handle.index] == nullptr)
                    {
                        continue;
                    }

                    const StringView label = detail::AccessLabel(access.type);
                    if (access.IsRead())
                    {
                        AppendFormat(out, u8"  res{} -> pass{} [label=\"{}\"", access.handle.index,
                                     passIdx, label);
                        if (pass->isCulled)
                        {
                            out.Append(u8" style=dashed color=gray");
                        }
                        out.Append(u8"];\n");
                    }
                    if (access.IsWrite())
                    {
                        AppendFormat(out, u8"  pass{} -> res{} [label=\"{}\"", passIdx,
                                     access.handle.index, label);
                        if (pass->isCulled)
                        {
                            out.Append(u8" style=dashed color=gray");
                        }
                        out.Append(u8"];\n");
                    }
                }
            }

            out.Append(u8"}\n");
        }

        // Human-readable text summary (counts + execution order).
        static void ExportSummary(RenderGraph& graph, String& out)
        {
            const Array<RenderGraphPass*>& passes = graph.Passes();
            const Array<RenderGraphResource*>& resources = graph.Resources();
            const Array<i32>& executionOrder = graph.ExecutionOrder();

            usize activeCount = 0, culledCount = 0;
            for (RenderGraphPass* p : passes)
            {
                if (p->isCulled)
                {
                    ++culledCount;
                }
                else
                {
                    ++activeCount;
                }
            }

            usize resCount = 0, transientCount = 0, persistentCount = 0, importedCount = 0;
            for (RenderGraphResource* r : resources)
            {
                if (r == nullptr)
                {
                    continue;
                }
                ++resCount;
                switch (r->lifetime)
                {
                case RGResourceLifetime::Transient:
                    ++transientCount;
                    break;
                case RGResourceLifetime::Persistent:
                    ++persistentCount;
                    break;
                case RGResourceLifetime::Imported:
                    ++importedCount;
                    break;
                }
            }

            out.Append(u8"=== Render Graph Summary ===\n");
            AppendFormat(out, u8"Passes: {} active, {} culled, {} total\n", activeCount,
                         culledCount, passes.Size());
            AppendFormat(out, u8"Resources: {} total ({} transient, {} persistent, {} imported)\n",
                         resCount, transientCount, persistentCount, importedCount);
            AppendFormat(out, u8"Output: {}x{}\n\n", graph.OutputWidth(), graph.OutputHeight());

            if (!executionOrder.IsEmpty())
            {
                out.Append(u8"Execution order:\n");
                for (usize i = 0; i < executionOrder.Size(); ++i)
                {
                    RenderGraphPass* pass = passes[static_cast<usize>(executionOrder[i])];
                    AppendFormat(out, u8"  {}. [{}] {}\n", i + 1, detail::PassTypeLabel(pass->type),
                                 pass->name.AsView());
                }
            }
        }
    };
}

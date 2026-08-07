// Draconic::RenderGraph - :validator partition
//
// Validates a graph for common authoring errors: reads of never-written
// resources (error), passes with no execute callback (warning), and redundant
// writes with no read in between (warning). Ported from Sedulous.RenderGraph
// (GraphValidator.bf).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:validator;

import draconic.foundation;
import :types;
import :pass;
import :resource;
import :graph;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    enum class ValidationSeverity
    {
        Warning,
        Error
    };

    struct ValidationMessage
    {
        ValidationSeverity severity = ValidationSeverity::Warning;
        String message;
    };

    namespace detail
    {
        [[nodiscard]] inline StringView ResName(const Array<RenderGraphResource*>& resources,
                                                u32 index)
        {
            return (index < resources.Size() && resources[index] != nullptr)
                       ? resources[index]->name.AsView()
                       : StringView(u8"???");
        }
    }

    class GraphValidator
    {
    public:
        static void Validate(RenderGraph& graph, Array<ValidationMessage>& out)
        {
            CheckUninitializedReads(graph, out);
            CheckEmptyPasses(graph, out);
            CheckRedundantWrites(graph, out);
        }

        static void ValidateToString(RenderGraph& graph, String& out)
        {
            Array<ValidationMessage> messages;
            Validate(graph, messages);

            if (messages.IsEmpty())
            {
                out.Append(u8"Render graph validation: OK (no issues)\n");
                return;
            }

            AppendFormat(out, u8"Render graph validation: {} issue(s)\n", messages.Size());
            for (const ValidationMessage& msg : messages)
            {
                const StringView prefix = msg.severity == ValidationSeverity::Error
                                              ? StringView(u8"ERROR")
                                              : StringView(u8"WARNING");
                AppendFormat(out, u8"  [{}] {}\n", prefix, msg.message.AsView());
            }
        }

    private:
        // Reads of resources that no prior pass wrote (transient only - imported
        // and persistent are considered externally initialized).
        static void CheckUninitializedReads(RenderGraph& graph, Array<ValidationMessage>& out)
        {
            const Array<RenderGraphResource*>& resources = graph.Resources();
            HashSet<u32> written;

            for (u32 i = 0; i < resources.Size(); ++i)
            {
                RenderGraphResource* res = resources[i];
                if (res != nullptr && (res->lifetime == RGResourceLifetime::Imported ||
                                       res->lifetime == RGResourceLifetime::Persistent))
                {
                    written.Insert(i);
                }
            }

            for (RenderGraphPass* pass : graph.Passes())
            {
                for (const RGResourceAccess& access : pass->accesses)
                {
                    if (access.IsRead() && access.handle.IsValid() &&
                        !written.Contains(access.handle.index))
                    {
                        ValidationMessage msg;
                        msg.severity = ValidationSeverity::Error;
                        AppendFormat(msg.message,
                                     u8"Pass '{}' reads resource '{}' (index {}) which has not "
                                     u8"been written to",
                                     pass->name.AsView(),
                                     detail::ResName(resources, access.handle.index),
                                     access.handle.index);
                        out.PushBack(static_cast<ValidationMessage&&>(msg));
                    }
                }
                for (const RGResourceAccess& access : pass->accesses)
                {
                    if (access.IsWrite() && access.handle.IsValid())
                    {
                        written.Insert(access.handle.index);
                    }
                }
            }
        }

        static void CheckEmptyPasses(RenderGraph& graph, Array<ValidationMessage>& out)
        {
            for (RenderGraphPass* pass : graph.Passes())
            {
                bool hasCallback = false;
                switch (pass->type)
                {
                case RGPassType::Render:
                    hasCallback = static_cast<bool>(pass->executeCallback);
                    break;
                case RGPassType::Compute:
                    hasCallback = static_cast<bool>(pass->computeCallback);
                    break;
                case RGPassType::Copy:
                    hasCallback = static_cast<bool>(pass->copyCallback);
                    break;
                }
                if (!hasCallback)
                {
                    ValidationMessage msg;
                    msg.severity = ValidationSeverity::Warning;
                    AppendFormat(msg.message, u8"Pass '{}' has no execute callback",
                                 pass->name.AsView());
                    out.PushBack(static_cast<ValidationMessage&&>(msg));
                }
            }
        }

        // Resources written twice with no read in between.
        static void CheckRedundantWrites(RenderGraph& graph, Array<ValidationMessage>& out)
        {
            const Array<RenderGraphResource*>& resources = graph.Resources();
            HashMap<u32, String> lastWriter;

            for (RenderGraphPass* pass : graph.Passes())
            {
                for (const RGResourceAccess& access : pass->accesses)
                {
                    if (access.IsRead() && access.handle.IsValid())
                    {
                        lastWriter.Remove(access.handle.index);
                    }
                }
                for (const RGResourceAccess& access : pass->accesses)
                {
                    if (!access.IsWrite() || !access.handle.IsValid())
                    {
                        continue;
                    }
                    if (String* prev = lastWriter.Find(access.handle.index))
                    {
                        ValidationMessage msg;
                        msg.severity = ValidationSeverity::Warning;
                        AppendFormat(msg.message,
                                     u8"Resource '{}' written by pass '{}' was already written by "
                                     u8"'{}' without being read",
                                     detail::ResName(resources, access.handle.index),
                                     pass->name.AsView(), prev->AsView());
                        out.PushBack(static_cast<ValidationMessage&&>(msg));
                    }
                    lastWriter.InsertOrAssign(access.handle.index, String(pass->name));
                }
            }
        }
    };
}

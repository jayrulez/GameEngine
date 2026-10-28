// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :material_shader partition
//
// material_set_shader: puts a material on a shader, as the material page's Shader row does: on a
// custom shader asset, laid out by its cooked property table with every value whose name, type and
// size still fit kept; or back on a built-in (PBR or Unlit). Writing a material's shaderId alone
// (asset_data_write) leaves it in its old layout, which the cook re-lays every time with a warning
// for each property the shader lacks.

module;
#include "Core/Prelude.h"

export module editor.mcp:material_shader;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.content;
import foundation.materials;
import foundation.materials.resource;
import foundation.shaders.resource;
import materials.pipeline;
import shaders.pipeline;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

export namespace editor::mcp
{
    inline void RegisterMaterialShaderTool(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        server.RegisterTool(
            u8"material_set_shader",
            u8"Put a material on a shader, as the material page's Shader row does: `shader` is a "
            u8"custom shader asset's guid (the material laid out by its cooked properties, every "
            u8"value whose name, type and size fit kept; the shader must have cooked: asset_cook "
            u8"first), or `pbr`, `unlit` or `phong` for a built-in. Saves the material; asset_cook to see it. "
            u8"Returns the material's properties after. Use this, not asset_data_write of its "
            u8"shaderId, which leaves the material in its old layout.",
            SchemaBuilder()
                .Str(u8"material", u8"the material asset's guid", true)
                .Str(u8"shader", u8"a shader asset's guid, or `pbr`, `unlit` or `phong`", true)
                .Build(),
            foundation::mcp::ToolAnnotations::Overwrites(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (s->project == nullptr)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String materialText = args.Get(u8"material").AsString();
                const String shaderText = args.Get(u8"shader").AsString();
                Guid materialId;
                if (!Guid::TryParse(materialText.AsView(), materialId))
                {
                    return Err(Format(u8"'{}' is not a valid guid", materialText.AsView()));
                }
                foundation::content::Instance* instance =
                    s->project->SourceDb().GetInstance(materialId);
                RefPtr<ISerializable> object =
                    instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
                auto* material = Cast<pipeline::MaterialAsset>(object.Get());
                if (material == nullptr)
                {
                    return Err(Format(u8"no material asset with guid {} in the open project",
                                      materialText.AsView()));
                }

                if (const foundation::materials::BuiltinShadingInfo* builtin =
                        foundation::materials::FindBuiltinShadingByShortName(shaderText.AsView()))
                {
                    pipeline::SetMaterialBuiltInShader(material->source, builtin->shading);
                }
                else
                {
                    Guid shaderId;
                    if (!Guid::TryParse(shaderText.AsView(), shaderId))
                    {
                        return Err(Format(u8"'{}' is neither a shader asset's guid nor pbr, unlit or phong",
                                          shaderText.AsView()));
                    }
                    foundation::content::Instance* shaderAsset =
                        s->project->SourceDb().GetInstance(shaderId);
                    RefPtr<ISerializable> shaderObject =
                        shaderAsset != nullptr ? shaderAsset->ReadObject() : RefPtr<ISerializable>{};
                    if (Cast<pipeline::ShaderAsset>(shaderObject.Get()) == nullptr)
                    {
                        return Err(Format(u8"no shader asset with guid {} in the open project",
                                          shaderText.AsView()));
                    }
                    foundation::content::Instance* product =
                        s->project->CookedDb().GetInstance(shaderId);
                    RefPtr<ISerializable> cooked =
                        product != nullptr ? product->ReadObject() : RefPtr<ISerializable>{};
                    const auto* shader = Cast<foundation::shaders::ShaderSource>(cooked.Get());
                    if (shader == nullptr)
                    {
                        return Err(Format(u8"the shader '{}' has not cooked (or failed to): "
                                          u8"asset_cook, then try again",
                                          shaderAsset->Name()));
                    }
                    pipeline::SetMaterialShader(material->source, shaderId, *shader);
                }
                if (!instance->WriteObject(*material).IsOk())
                {
                    return Err(Format(u8"could not write '{}'", instance->Name()));
                }

                JsonValue properties = JsonValue::MakeArray();
                for (const String& name : material->source.propertyNames)
                {
                    properties.Add(JsonValue::MakeString(name));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"material", JsonValue::MakeString(String(instance->Name())));
                out.Set(u8"shaderName", JsonValue::MakeString(material->source.shaderName));
                out.Set(u8"properties", Move(properties));
                return out;
            });
    }
}

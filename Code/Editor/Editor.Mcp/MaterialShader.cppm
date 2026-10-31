// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :material_shader partition
//
// material_set_shader: puts a material on a shader, as the material page's Shader row does: on a
// custom shader asset, laid out by its cooked property table with every value whose name, type and
// size still fit kept; or back on a built-in (PBR, Unlit or Phong). Writing a material's shaderId
// alone (asset_data_write) leaves it in its old layout, which the cook lays out again every time
// with a warning for each property the shader lacks.
//
// A material's effect groups follow the shader's vertex variants. They are laid on the effects'
// tables, read from the engine's shader library (`effect` blocks in its .hlsli files, as the cook
// reads them), so a material moved onto a shader that declares WIND gains its Wind group (keeping
// the values it had), and one moved onto a shader that does not loses it. The result lists the
// groups with their values, addressed as `Effect.Property`.

module;
#include "Core/Prelude.h"

export module editor.mcp:material_shader;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.content;
import foundation.materials;
import foundation.materials.resource;
import foundation.shaders;
import foundation.shaders.resource;
import materials.pipeline;
import shaders.pipeline;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace editor::mcp::detail
{
    // A material's effect groups as a result lists them: each `{name, flag, properties: [{name,
    // type, value}]}`, with the flag by its #define name (WIND), a float's value as a number and a
    // wider value as an array. In a script or an override a parameter is `Name.Property`.
    inline JsonValue MaterialEffectsJson(const foundation::materials::MaterialSource& source)
    {
        using foundation::materials::MaterialPropertyType;
        JsonValue effects = JsonValue::MakeArray();
        for (const foundation::materials::MaterialEffectGroupSource& group : source.effects)
        {
            JsonValue entry = JsonValue::MakeObject();
            entry.Set(u8"name", JsonValue::MakeString(group.name));
            String flag;
            for (const foundation::shaders::ShaderFlagName& e : foundation::shaders::kShaderFlagNames)
            {
                if (static_cast<u32>(e.flag) == group.flag)
                {
                    flag = String(e.define);
                }
            }
            entry.Set(u8"flag", JsonValue::MakeString(flag));
            JsonValue properties = JsonValue::MakeArray();
            for (usize i = 0; i < group.propertyNames.Size(); ++i)
            {
                const auto type = i < group.propertyTypes.Size() ? static_cast<MaterialPropertyType>(group.propertyTypes[i])
                                                                 : MaterialPropertyType::Float;
                const u32 offset = i < group.propertyOffsets.Size() ? group.propertyOffsets[i] : 0u;
                const u32 size = i < group.propertySizes.Size() ? group.propertySizes[i] : 0u;
                JsonValue property = JsonValue::MakeObject();
                property.Set(u8"name", JsonValue::MakeString(group.propertyNames[i]));
                const usize count = Min<usize>(size / 4u, 4u);
                if (static_cast<usize>(offset) + count * 4u <= group.values.Size() && count > 0)
                {
                    if (type == MaterialPropertyType::Int)
                    {
                        i32 v = 0;
                        MemCopy(&v, group.values.Data() + offset, sizeof(v));
                        property.Set(u8"type", JsonValue::MakeString(String(u8"int")));
                        property.Set(u8"value", JsonValue::MakeNumber(static_cast<f64>(v)));
                    }
                    else
                    {
                        f32 v[4] = {0, 0, 0, 0};
                        MemCopy(v, group.values.Data() + offset, count * 4u);
                        property.Set(u8"type", JsonValue::MakeString(count == 1 ? String(u8"float") : String(u8"floats")));
                        if (count == 1)
                        {
                            property.Set(u8"value", JsonValue::MakeNumber(static_cast<f64>(v[0])));
                        }
                        else
                        {
                            JsonValue parts = JsonValue::MakeArray();
                            for (usize c = 0; c < count; ++c)
                            {
                                parts.Add(JsonValue::MakeNumber(static_cast<f64>(v[c])));
                            }
                            property.Set(u8"value", Move(parts));
                        }
                    }
                }
                properties.Add(Move(property));
            }
            entry.Set(u8"properties", Move(properties));
            effects.Add(Move(entry));
        }
        return effects;
    }
}

export namespace editor::mcp
{
    // `shaderLibrary`: the engine's Shaders/ folder, where the effects' tables are declared. When it
    // is empty none are read, so a material keeps the groups it carries.
    inline void RegisterMaterialShaderTool(foundation::mcp::McpServer& server, ProjectSession& session,
                                           StringView shaderLibrary)
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
            u8"Returns the material's properties after, and its effect groups (`effects`: each "
            u8"effect's name, its variant flag and its parameters with their values; Wind, with "
            u8"Strength, Speed and Height, on any shader whose vertex stage declares WIND, its "
            u8"values kept across the switch; Strength above zero sways). Use this, not "
            u8"asset_data_write of its shaderId, which leaves the material in its old layout.",
            SchemaBuilder()
                .Str(u8"material", u8"the material asset's guid", true)
                .Str(u8"shader", u8"a shader asset's guid, or `pbr`, `unlit` or `phong`", true)
                .Build(),
            foundation::mcp::ToolAnnotations::Overwrites(),
            [s, library = String(shaderLibrary)](const JsonValue& args) -> ToolResult
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

                // The shader's vertex variants: which effects' groups the material carries on it.
                foundation::shaders::ShaderFlags vertexVariants = foundation::shaders::ShaderFlags::None;
                if (const foundation::materials::BuiltinShadingInfo* builtin =
                        foundation::materials::FindBuiltinShadingByShortName(shaderText.AsView()))
                {
                    pipeline::SetMaterialBuiltInShader(material->source, builtin->shading);
                    vertexVariants = builtin->vertexVariants;
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
                    vertexVariants = static_cast<foundation::shaders::ShaderFlags>(shader->vertexVariantMask);
                }
                // Add a group the shader declares and the material lacks (as every new material
                // does, since the builders know no effect's table) at the effect's defaults. A
                // library that does not read leaves the groups the switch kept.
                if (!library.IsEmpty())
                {
                    foundation::shaders::ShaderEffectLibrary effects;
                    Array<String> effectErrors; // the shader cook reports a malformed block
                    (void)foundation::shaders::CollectShaderLibraryEffects(library.AsView(), effects, effectErrors);
                    foundation::materials::AdoptDeclaredEffects(editor::EditorRootAllocator(), material->source,
                                                                effects.All(), vertexVariants);
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
                out.Set(u8"effects", detail::MaterialEffectsJson(material->source));
                return out;
            });
    }
}

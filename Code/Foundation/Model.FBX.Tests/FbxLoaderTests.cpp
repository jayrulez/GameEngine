// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// FBX/OBJ loader: vertex welding and OBJ material sidecars. Both cases came from the Beef
// port, which found them by porting rather than reading: the welder keyed on the hash
// alone (a collision merged two different vertices into a pulled seam), and OBJ files
// never had their .mtl read (every material arrived named but white).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cstdio>
#include <cstring>

import foundation.core;
import foundation.model;
import foundation.model.fbx;

using namespace foundation::core;
using namespace foundation::model;

namespace
{
    void WriteText(const char* path, const char* text)
    {
        // The engine's own file write (std::fopen is a C4996 error under MSVC /WX).
        REQUIRE(WriteFile(StringView(reinterpret_cast<const utf8char*>(path)),
                          Span<const byte>(reinterpret_cast<const byte*>(text), std::strlen(text)))
                    .IsOk());
    }

    // A quad as two triangles sharing an edge: 4 distinct vertices, 6 indices.
    constexpr const char* kQuadObj = R"(mtllib welded.mtl
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vn 0 0 1
usemtl red
f 1//1 2//1 3//1
f 1//1 3//1 4//1
)";
    constexpr const char* kRedMtl = "newmtl red\nKd 1 0 0\n";
    // Linear 0.2140 is sRGB 0.5: a DCC's scene-linear colour, read as the authored sRGB colour.
    constexpr const char* kGreyMtl = "newmtl red\nKd 0.2140 0.2140 0.2140\n";
}

TEST_CASE("fbx.weld: colliding hashes never merge different vertices")
{
    fbx::VertexWelder welder(4);
    Array<u8> bytes;
    const u8 a[4] = {1, 2, 3, 4};
    const u8 b[4] = {9, 9, 9, 9};
    const u8 c[4] = {1, 2, 3, 4}; // same content as a
    const size_t sameHash = 0x1234u;
    CHECK(welder.Weld(sameHash, a, bytes) == 0);
    CHECK(welder.Weld(sameHash, b, bytes) == 1); // collided, but the bytes differ
    CHECK(welder.Weld(sameHash, c, bytes) == 0); // identical content welds
    CHECK(welder.Weld(0x9999u, c, bytes) == 2);  // a different hash is a different bucket
    CHECK(welder.Count() == 3);
    REQUIRE(bytes.Size() == 12u);
    CHECK(std::memcmp(bytes.Data() + 4, b, 4) == 0);
}

TEST_CASE("fbx.obj: a shared edge welds to distinct vertices only")
{
    WriteText("scratch_fbx_welded.obj", kQuadObj);
    WriteText("welded.mtl", kRedMtl);
    Model model;
    fbx::FbxLoader loader;
    REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_welded.obj")),
                        model) == ModelLoadResult::Ok);
    REQUIRE(model.meshes().Size() == 1u);
    const ModelMesh* mesh = model.meshes()[0];
    CHECK(mesh->indexCount() == 6);
    CHECK(mesh->vertexCount() == 4);
    std::remove("scratch_fbx_welded.obj");
    std::remove("welded.mtl");
}

TEST_CASE("fbx.obj: the .mtl sidecar is read, so usemtl carries its colour")
{
    WriteText("scratch_fbx_material.obj", kQuadObj);
    WriteText("welded.mtl", kRedMtl);
    Model model;
    fbx::FbxLoader loader;
    REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_material.obj")),
                        model) == ModelLoadResult::Ok);
    REQUIRE(model.materials().Size() >= 1u);
    const ModelMaterial* red = nullptr;
    for (const ModelMaterial* m : model.materials())
    {
        if (m->name() == StringView{u8"red"})
        {
            red = m;
        }
    }
    REQUIRE(red != nullptr);
    CHECK(red->baseColorFactor.x == doctest::Approx(1.0f));
    CHECK(red->baseColorFactor.y == doctest::Approx(0.0f));
    CHECK(red->baseColorFactor.z == doctest::Approx(0.0f));
    std::remove("scratch_fbx_material.obj");
    std::remove("welded.mtl");
}

TEST_CASE("fbx.obj: a material colour (scene-linear in the file) reads as authored sRGB")
{
    WriteText("scratch_fbx_grey.obj", kQuadObj);
    WriteText("welded.mtl", kGreyMtl);
    Model model;
    fbx::FbxLoader loader;
    REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_grey.obj")),
                        model) == ModelLoadResult::Ok);
    REQUIRE(model.materials().Size() >= 1u);
    const ModelMaterial* grey = model.materials()[0];
    CHECK(grey->baseColorFactor.x == doctest::Approx(0.5f).epsilon(0.002));
    CHECK(grey->baseColorFactor.y == doctest::Approx(0.5f).epsilon(0.002));
    std::remove("scratch_fbx_grey.obj");
    std::remove("welded.mtl");
}

TEST_CASE("fbx: a legacy material's colour ignores its diffuse factor, as the glTF has none")
{
    // Blender writes DiffuseFactor 0.8 beside the colour; the same model's glTF carries the colour
    // alone. Linear 0.2140 reads as sRGB 0.5 whatever the factor.
    constexpr const char* kFbx = R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
Objects:  {
	Material: 1000, "Material::Grey", "" {
		Version: 102
		ShadingModel: "phong"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.2140,0.2140,0.2140
			P: "DiffuseFactor", "Number", "", "A",0.8
		}
	}
}
Connections:  {
}
)";
    WriteText("scratch_fbx_factor.fbx", kFbx);
    Model model;
    fbx::FbxLoader loader;
    REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_factor.fbx")),
                        model) == ModelLoadResult::Ok);
    REQUIRE(model.materials().Size() >= 1u);
    const ModelMaterial* grey = model.materials()[0];
    CHECK(grey->baseColorFactor.x == doctest::Approx(0.5f).epsilon(0.002));
    std::remove("scratch_fbx_factor.fbx");
}

TEST_CASE("fbx.obj: a missing .mtl is not an error")
{
    WriteText("scratch_fbx_nomtl.obj", kQuadObj); // references welded.mtl, which is absent
    Model model;
    fbx::FbxLoader loader;
    CHECK(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_nomtl.obj")),
                      model) == ModelLoadResult::Ok);
    CHECK(model.meshes().Size() == 1u);
    std::remove("scratch_fbx_nomtl.obj");
}

// Imported as authored (Specs/phong-material.md): an FBX Phong material keeps its highlight (its
// specular colour times its factor, as sRGB, and its exponent), a Lambert one has none; both are
// told apart from PBR by the shading model the file names.
TEST_CASE("fbx: a Phong material keeps its highlight, a Lambert one is diffuse alone")
{
    constexpr const char* kFbx = R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
Objects:  {
	Material: 1000, "Material::Shiny", "" {
		Version: 102
		ShadingModel: "phong"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",1,1,1
			P: "SpecularColor", "Color", "", "A",0.2140,0.2140,0.2140
			P: "SpecularFactor", "Number", "", "A",1
			P: "ShininessExponent", "Number", "", "A",32
		}
	}
	Material: 1001, "Material::Matte", "" {
		Version: 102
		ShadingModel: "lambert"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.2140,0.2140,0.2140
		}
	}
}
Connections:  {
}
)";
    WriteText("scratch_fbx_phong.fbx", kFbx);
    Model model;
    fbx::FbxLoader loader;
    REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_phong.fbx")), model) == ModelLoadResult::Ok);
    const ModelMaterial* shiny = nullptr;
    const ModelMaterial* matte = nullptr;
    for (const ModelMaterial* m : model.materials())
    {
        if (m->name() == StringView{u8"Shiny"})
            shiny = m;
        if (m->name() == StringView{u8"Matte"})
            matte = m;
    }
    REQUIRE(shiny != nullptr);
    REQUIRE(matte != nullptr);
    CHECK(shiny->shadingModel == ShadingModel::Phong);
    CHECK(shiny->specularColor.x == doctest::Approx(0.5f).epsilon(0.002)); // linear 0.2140 is sRGB 0.5
    CHECK(shiny->shininess == doctest::Approx(32.0f));
    CHECK(matte->shadingModel == ShadingModel::Lambert);
    CHECK(matte->specularColor.x == 0.0f);
    CHECK(matte->baseColorFactor.x == doctest::Approx(0.5f).epsilon(0.002));
    std::remove("scratch_fbx_phong.fbx");
}

TEST_CASE("fbx.obj: a classic .mtl (Kd, Ks, Ns) is Phong; one with PBR keys stays PBR")
{
    WriteText("scratch_fbx_classic.obj", kQuadObj);
    WriteText("welded.mtl", "newmtl red\nKd 1 0 0\nKs 0.2140 0.2140 0.2140\nNs 50\n");
    {
        Model model;
        fbx::FbxLoader loader;
        REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_classic.obj")), model) == ModelLoadResult::Ok);
        REQUIRE(model.materials().Size() >= 1u);
        const ModelMaterial* red = model.materials()[0];
        CHECK(red->shadingModel == ShadingModel::Phong);
        CHECK(red->specularColor.x == doctest::Approx(0.5f).epsilon(0.002));
        CHECK(red->shininess == doctest::Approx(50.0f));
    }
    WriteText("welded.mtl", "newmtl red\nKd 1 0 0\nPr 0.4\nPm 0.0\n");
    {
        Model model;
        fbx::FbxLoader loader;
        REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_classic.obj")), model) == ModelLoadResult::Ok);
        REQUIRE(model.materials().Size() >= 1u);
        CHECK(model.materials()[0]->shadingModel == ShadingModel::Pbr);
    }
    std::remove("scratch_fbx_classic.obj");
    std::remove("welded.mtl");
}

TEST_CASE("fbx: Blender's export stays PBR, though it names every material Phong")
{
    constexpr const char* kFbx = R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
	Creator: "Blender (stable FBX IO) - 4.2.0 - 5.12.3"
}
Creator: "Blender (stable FBX IO) - 4.2.0 - 5.12.3"
Objects:  {
	Material: 1000, "Material::Painted", "" {
		Version: 102
		ShadingModel: "Phong"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.8,0.8,0.8
			P: "SpecularColor", "Color", "", "A",0.8,0.8,0.8
			P: "ShininessExponent", "Number", "", "A",25
		}
	}
}
Connections:  {
}
)";
    WriteText("scratch_fbx_blender.fbx", kFbx);
    Model model;
    fbx::FbxLoader loader;
    REQUIRE(loader.load(StringView(reinterpret_cast<const utf8char*>("scratch_fbx_blender.fbx")), model) == ModelLoadResult::Ok);
    REQUIRE(model.materials().Size() >= 1u);
    CHECK(model.materials()[0]->shadingModel == ShadingModel::Pbr);
    std::remove("scratch_fbx_blender.fbx");
}

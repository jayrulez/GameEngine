// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Importing files over a scratch project and importers of the test's own: a file no importer takes
// left out, said; each file described with what it would create, a previous import's choices laid
// over it; another importer chosen describing it again; the import waiting for a file kept, saying
// why; the commit creating what is kept under the names chosen; and a file that needs no review.
// A name another asset holds: one of another type waits for a rename, another file's of its type
// for Replace or Keep both.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import pipeline.importer;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::AssetImport;
namespace content = foundation::content;

namespace
{
    class ImportedThing : public ISerializable
    {
        RTTI_OBJECT(ImportedThing, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    // Takes ".crate": would make a mesh and a material, each kept or not and named as chosen.
    class CrateImporter final : public pipeline::IFileImporter
    {
    public:
        explicit CrateImporter(StringView label) : m_label(label) {}
        [[nodiscard]] StringView Label() const override { return m_label.AsView(); }
        [[nodiscard]] bool Accepts(StringView extension) const override { return extension == StringView(u8"crate"); }
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView, const pipeline::ImportOptions*, Object*) override
        {
            pipeline::ImportPlan plan;
            plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Mesh, String(u8"Body"), String(u8"Body"), true});
            plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Material, String(m_label), String(m_label), true});
            return plan;
        }
        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group, StringView) override
        {
            pipeline::ImportPlan stored;
            if (group.GetInstance(u8"Kept") != nullptr) // a previous import here renamed the body
                stored.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Mesh, String(u8"Body"), String(u8"Hull"), true});
            return stored;
        }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group& group, const pipeline::ImportOptions* options,
                                                        Object*, Array<pipeline::DeferredImportWrite>*) override
        {
            content::Instance* primary = nullptr;
            const auto make = [&](pipeline::ImportResourceKind kind, StringView source)
            {
                if (options != nullptr && !options->SelectionEnabled(kind, source))
                    return;
                content::Instance* made = group.CreateInstance(options != nullptr ? options->SelectionName(kind, source) : source, ImportedThing::StaticType());
                if (primary == nullptr)
                    primary = made;
            };
            make(pipeline::ImportResourceKind::Mesh, u8"Body");
            make(pipeline::ImportResourceKind::Material, m_label.AsView());
            return primary;
        }

    private:
        String m_label;
    };

    // Takes ".as": a script, no options and nothing to review.
    class ScriptImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Script"; }
        [[nodiscard]] bool Accepts(StringView extension) const override { return extension == StringView(u8"as"); }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group& group, const pipeline::ImportOptions*, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            return group.CreateInstance(u8"Player", ImportedThing::StaticType());
        }
    };

    // A single-asset importer of `extension` making a `typeName` asset (the clash check's input).
    class SingleImporter final : public pipeline::IFileImporter
    {
    public:
        SingleImporter(StringView extension, StringView typeName) : m_extension(extension), m_type(typeName) {}
        [[nodiscard]] StringView Label() const override { return m_extension.AsView(); }
        [[nodiscard]] bool Accepts(StringView extension) const override { return extension == m_extension.AsView(); }
        [[nodiscard]] StringView AssetTypeName(StringView) const override { return m_type.AsView(); }
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView path, const pipeline::ImportOptions*, Object*) override
        {
            return pipeline::SingleAssetPlan(path);
        }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group&, const pipeline::ImportOptions*, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            return Err(ErrorCode::NotSupported);
        }

    private:
        String m_extension;
        String m_type;
    };

    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        UniquePtr<AssetImport> import;
        content::Group* top = nullptr;

        explicit Bed(bool second = false)
        {
            std::error_code ec;
            std::filesystem::remove_all("asset_import_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"asset_import_project", u8"Import").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"asset_import_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(ImportedThing::StaticType());
            context.SetProject(project.Get());
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<CrateImporter>(u8"Paint"), DefaultAllocator()));
            if (second)
                context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<CrateImporter>(u8"Varnish"), DefaultAllocator()));
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<ScriptImporter>(), DefaultAllocator()));
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<SingleImporter>(u8"sheet", u8"SheetThing"), DefaultAllocator()));
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<SingleImporter>(u8"thing", u8"ImportedThing"), DefaultAllocator()));
            top = project->SourceDb().RootGroup();
            AssetImport::Services services;
            services.context = &context; // no cook, no jobs: at once
            import = MakeUnique<AssetImport>(DefaultAllocator(), Move(services));
        }
        ~Bed()
        {
            import.Reset();
            context.SetProject(nullptr);
            project.Reset();
            std::error_code ec;
            std::filesystem::remove_all("asset_import_project", ec);
        }
    };
}

RTTI_DEFINE_OBJECT(ImportedThing, "rtti::tests::assetimport")

TEST_CASE("asset import: a file nothing takes is left out; each file described; the commit makes what is kept, named as chosen")
{
    Bed bed;
    const String paths[] = {String(u8"/somewhere/box.crate"), String(u8"/somewhere/notes.txt")};
    RefPtr<AssetImport::Session> session = bed.import->Begin(Span<const String>(paths, 2u), *bed.top);
    REQUIRE(session);
    REQUIRE(session->files.Size() == 1u); // notes.txt: nothing imports it
    AssetImport::File& box = session->files[0];
    CHECK(box.Name() == StringView(u8"box.crate"));
    CHECK(box.state == AssetImport::File::State::Ready);
    REQUIRE(box.plan.entries.Size() == 2u);
    String why;
    CHECK(AssetImport::CanImport(*session, why));
    CHECK(why == String(u8"Into Content."));
    CHECK_FALSE(AssetImport::NeedsNoReview(*session));

    // The body renamed, the material left out.
    box.plan.entries[0].targetName = String(u8"Kept");
    box.plan.entries[1].enabled = false;
    REQUIRE(bed.import->Commit(*session));
    CHECK(bed.top->GetInstance(u8"Kept") != nullptr);
    CHECK(bed.top->GetInstance(u8"Paint") == nullptr);

    // Again into the same group: the previous choice laid over the plan.
    RefPtr<AssetImport::Session> again = bed.import->Begin(Span<const String>(paths, 1u), *bed.top);
    REQUIRE(again);
    CHECK(again->files[0].plan.entries[0].targetName == String(u8"Hull"));
}

TEST_CASE("asset import: another importer describes the file again; the import waits for a file kept; a script needs no review")
{
    Bed bed(true);
    const String box[] = {String(u8"/somewhere/box.crate")};
    RefPtr<AssetImport::Session> session = bed.import->Begin(Span<const String>(box, 1u), *bed.top);
    REQUIRE(session);
    AssetImport::File& file = session->files[0];
    REQUIRE(file.importers.Size() == 2u);
    CHECK(file.plan.entries[1].sourceName == String(u8"Paint"));
    bed.import->SetImporter(*session, 0, 1);
    CHECK(file.Importer()->Label() == StringView(u8"Varnish"));
    CHECK(file.plan.entries[1].sourceName == String(u8"Varnish"));

    file.kept = false;
    String why;
    CHECK_FALSE(AssetImport::CanImport(*session, why));
    CHECK(why == String(u8"No file is kept."));
    CHECK_FALSE(bed.import->Commit(*session));

    const String script[] = {String(u8"/somewhere/player.as")};
    RefPtr<AssetImport::Session> quick = bed.import->Begin(Span<const String>(script, 1u), *bed.top);
    REQUIRE(quick);
    CHECK(AssetImport::NeedsNoReview(*quick));
    REQUIRE(bed.import->Commit(*quick));
    CHECK(bed.top->GetInstance(u8"Player") != nullptr);
}

TEST_CASE("asset import: a name another asset holds waits: another type for a rename, another file's for Replace or Keep both")
{
    Bed bed;
    REQUIRE(bed.top->CreateInstance(u8"menu", ImportedThing::StaticType()) != nullptr);
    String why;

    // A "SheetThing" named menu over an ImportedThing: said, no import, and no import at once either.
    const String sheet[] = {String(u8"/somewhere/menu.sheet")};
    RefPtr<AssetImport::Session> other = bed.import->Begin(Span<const String>(sheet, 1u), *bed.top);
    REQUIRE(other);
    CHECK(AssetImport::ClashOf(*other, other->files[0]).kind == pipeline::ImportClashKind::OtherType);
    CHECK_FALSE(AssetImport::CanImport(*other, why));
    CHECK(why.AsView().ContainsIgnoreCase(u8"already a ImportedThing"));
    CHECK_FALSE(AssetImport::NeedsNoReview(*other));
    other->files[0].onConflict = pipeline::ImportConflictChoice::Replace; // no choice lets another type through
    CHECK_FALSE(AssetImport::CanImport(*other, why));
    other->files[0].plan.entries[0].targetName = String(u8"menu sheet"); // renamed: free
    CHECK(AssetImport::CanImport(*other, why));

    // An ImportedThing named menu that another file made: waits for the user's choice.
    const String thing[] = {String(u8"/elsewhere/menu.thing")};
    RefPtr<AssetImport::Session> same = bed.import->Begin(Span<const String>(thing, 1u), *bed.top);
    REQUIRE(same);
    CHECK(AssetImport::ClashOf(*same, same->files[0]).kind == pipeline::ImportClashKind::SameType);
    CHECK_FALSE(AssetImport::CanImport(*same, why));
    CHECK(why.AsView().ContainsIgnoreCase(u8"replace it, or keep both"));
    same->files[0].onConflict = pipeline::ImportConflictChoice::KeepBoth;
    CHECK(AssetImport::CanImport(*same, why));
}

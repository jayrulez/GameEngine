// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The import window over its markup (Data/Editor/Import.gml) and an import of the test's own: what a
// file would create shown by kind, each kept or not and named; Import open with what it will do,
// and waiting, saying why, while a file is being read or nothing is kept; the files listed when
// there are several; Import making what is kept and closing. Keeps the 2026-09-21 regression: Import
// opens once inline files are described (it once stayed shut, "click Import, nothing happens").

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import foundation.gui;
import pipeline.importer;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::AssetImport;
using editor::app::ImportWindow;
namespace gui = foundation::gui;
namespace content = foundation::content;

namespace
{
    class WindowThing : public ISerializable
    {
        RTTI_OBJECT(WindowThing, ISerializable)
    public:
        void Serialize(ISerializer& ar) override { (void)ar; }
    };

    // Takes ".box": would make a mesh and two textures.
    class BoxImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Box"; }
        [[nodiscard]] bool Accepts(StringView extension) const override { return extension == StringView(u8"box"); }
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView, const pipeline::ImportOptions*, Object*) override
        {
            pipeline::ImportPlan plan;
            plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Mesh, String(u8"Box"), String(u8"Box"), true});
            plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Texture, String(u8"Wood"), String(u8"Wood"), true});
            plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Texture, String(u8"Rust"), String(u8"Rust"), true});
            return plan;
        }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group& group, const pipeline::ImportOptions* options, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            content::Instance* primary = nullptr;
            for (const pipeline::ImportPlanEntry& entry : options->selection.entries)
                if (entry.enabled)
                {
                    content::Instance* made = group.CreateInstance(entry.targetName.AsView(), WindowThing::StaticType());
                    if (primary == nullptr)
                        primary = made;
                }
            return primary;
        }
    };

    RefPtr<gui::Node> LoadImport()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(ImportWindow::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    usize Count(gui::Node& node)
    {
        usize count = Cast<T>(&node) != nullptr ? 1u : 0u;
        for (usize i = 0; i < node.ChildCount(); ++i)
            count += Count<T>(*node.GetChildAt(i));
        return count;
    }

    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        UniquePtr<AssetImport> import;
        UniquePtr<ImportWindow> window;
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        content::Group* top = nullptr;

        Bed()
        {
            std::error_code ec;
            std::filesystem::remove_all("import_window_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"import_window_project", u8"Window").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"import_window_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(WindowThing::StaticType());
            context.SetProject(project.Get());
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<BoxImporter>(), DefaultAllocator()));
            top = project->SourceDb().RootGroup();
            AssetImport::Services services;
            services.context = &context;
            import = MakeUnique<AssetImport>(DefaultAllocator(), Move(services));
            ImportWindow::Services windows;
            windows.import = import.Get();
            window = MakeUnique<ImportWindow>(DefaultAllocator(), DefaultAllocator(), Move(windows), LoadImport());
            root->SetSize(Float2{1000.0f, 700.0f});
        }
        ~Bed()
        {
            window.Reset();
            import.Reset();
            context.SetProject(nullptr);
            project.Reset();
            std::error_code ec;
            std::filesystem::remove_all("import_window_project", ec);
        }
        RefPtr<AssetImport::Session> Open(Span<const String> paths)
        {
            RefPtr<AssetImport::Session> session = import->Begin(paths, *top);
            REQUIRE(session);
            REQUIRE(window->Open(*root, session));
            (void)root->UpdateLayout();
            return session;
        }
    };
}

RTTI_DEFINE_OBJECT(WindowThing, "rtti::tests::importwindow")

TEST_CASE("import window: a file's plan by kind, each kept and named; Import open saying what it does; Import makes what is kept and closes")
{
    Bed bed;
    const String box[] = {String(u8"/somewhere/crate.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(box, 1u));
    // Two kinds' boxes, three entries' boxes and names (no option toggles here).
    gui::Node& detail = *bed.window->Detail();
    CHECK(Count<gui::CheckBox>(detail) == 5u);
    CHECK(Count<gui::TextField>(detail) == 3u);
    CHECK(bed.window->ImportButton()->IsEnabled()); // the regression: open once described
    CHECK(bed.window->Why() == StringView(u8"Into Content."));

    session->files[0].plan.entries[2].enabled = false; // Rust left out
    CHECK(bed.window->Import());
    CHECK_FALSE(bed.window->IsOpen());
    CHECK(bed.top->GetInstance(u8"Box") != nullptr);
    CHECK(bed.top->GetInstance(u8"Wood") != nullptr);
    CHECK(bed.top->GetInstance(u8"Rust") == nullptr);
    CHECK(session->closed); // the window let go of it
}

TEST_CASE("import window: Import waits while a file is read, saying so, and opens when it is ready; several files are listed, none kept waits")
{
    Bed bed;
    const String two[] = {String(u8"/somewhere/a.box"), String(u8"/somewhere/b.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(two, 2u));
    CHECK(bed.window->ImportButton()->IsEnabled());
    CHECK(bed.window->Why() == StringView(u8"2 of 2 files into Content."));

    // As a heavy importer's file while the worker reads it, then told it is ready.
    session->files[1].state = AssetImport::File::State::Reading;
    session->onChanged(1);
    CHECK_FALSE(bed.window->ImportButton()->IsEnabled());
    CHECK(bed.window->Why() == StringView(u8"Reading b.box..."));
    CHECK_FALSE(bed.window->Import()); // it stays
    CHECK(bed.window->IsOpen());
    session->files[1].state = AssetImport::File::State::Ready;
    session->onChanged(1);
    CHECK(bed.window->ImportButton()->IsEnabled());

    session->files[0].kept = false;
    session->files[1].kept = false;
    session->onChanged(0);
    CHECK_FALSE(bed.window->ImportButton()->IsEnabled());
    CHECK(bed.window->Why() == StringView(u8"No file is kept."));
    bed.window->Close();
    CHECK(session->closed);
}

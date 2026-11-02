// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The import window over its markup (Data/Editor/Import.xml) and an import of the test's own. What a
// file would create is shown by kind, each entry kept or not and named. Import is enabled and says
// what it will do, or stays disabled with the reason while a file is being read or nothing is kept.
// The files are listed when there are several, and Import makes what is kept and closes. Import is
// enabled as soon as inline files are described.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.gui;
import foundation.gui.toolkit;
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
        [[nodiscard]] StringView ListedTypeName(StringView) const override { return StringView(reinterpret_cast<const utf8char*>(WindowThing::StaticType().name)); }
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

    // A preview naming its file, counting how many it has made.
    class NamePreview final : public editor::IImportPreview
    {
    public:
        explicit NamePreview(StringView path) : m_view(MakeRef<gui::Label>(DefaultAllocator()))
        {
            m_facts.PushBack(Format(u8"Made for {}", pipeline::FileNameOf(path)));
        }
        [[nodiscard]] gui::Node& View() override { return *m_view; }
        [[nodiscard]] Span<const String> Facts() const override { return m_facts.AsSpan(); }
        // As a read coming back does: new facts, and the window told.
        void Arrive(StringView fact)
        {
            m_facts.PushBack(String(fact));
            if (onChanged)
                onChanged();
        }

    private:
        RefPtr<gui::Label> m_view;
        Array<String> m_facts;
    };
    class NamePreviewFactory final : public editor::IImportPreviewFactory
    {
    public:
        explicit NamePreviewFactory(usize& made) : m_made(&made) {}
        [[nodiscard]] const TypeInfo* AssetType() const override { return &WindowThing::StaticType(); }
        [[nodiscard]] UniquePtr<editor::IImportPreview> Create(StringView path, editor::ImportPreviewReader&, IAllocator& allocator) override
        {
            ++*m_made;
            return UniquePtr<editor::IImportPreview>(allocator.New<NamePreview>(path), allocator);
        }

    private:
        usize* m_made;
    };

    // Options that declare one setting, as a real importer's do.
    class CrateOptions final : public pipeline::ImportOptions
    {
        RTTI_OBJECT(CrateOptions, pipeline::ImportOptions)
    public:
        bool hollow = false;
    };

    // Takes ".crate": one asset, with a setting.
    class CrateImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Crate"; }
        [[nodiscard]] bool Accepts(StringView extension) const override { return extension == StringView(u8"crate"); }
        [[nodiscard]] RefPtr<pipeline::ImportOptions> CreateOptions(IAllocator& allocator) const override { return MakeRef<CrateOptions>(allocator); }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group& group, const pipeline::ImportOptions*, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            return group.CreateInstance(u8"Crate", WindowThing::StaticType());
        }
    };

    RefPtr<gui::Node> LoadImport()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(editor::app::kImportWindowPath));
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

    // Every label's text under `node`, one line each, to look for one in.
    String Texts(gui::Node& node)
    {
        String out;
        if (auto* label = Cast<gui::Label>(&node))
        {
            out += label->GetText();
            out += u8"\n";
        }
        for (usize i = 0; i < node.ChildCount(); ++i)
            out += Texts(*node.GetChildAt(i));
        return out;
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
            (void)RemoveDirectoryRecursive(u8"import_window_project");
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"import_window_project", u8"Window").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"import_window_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(WindowThing::StaticType());
            context.SetProject(project.Get());
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<BoxImporter>(), DefaultAllocator()));
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<CrateImporter>(), DefaultAllocator()));
            top = project->SourceDb().RootGroup();
            AssetImport::Services services;
            services.context = &context;
            import = MakeUnique<AssetImport>(DefaultAllocator(), Move(services));
            ImportWindow::Services windows;
            windows.import = import.Get();
            windows.previews = &context.ImportPreviews();
            windows.previewReader = &context.ImportPreviewReads();
            window = MakeUnique<ImportWindow>(DefaultAllocator(), DefaultAllocator(), Move(windows), LoadImport());
            root->SetSize(Float2{1000.0f, 700.0f});
        }
        ~Bed()
        {
            window.Reset();
            import.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(u8"import_window_project");
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
REFLECT_MEMBERS(CrateOptions, "rtti::tests::importwindow")
{
    builder.Property<&CrateOptions::hollow>("hollow").PropAttribute("displayName", String(u8"Hollow"));
}

TEST_CASE("import window: a file's plan by kind, each kept and named; Import open saying what it does; Import makes what is kept and closes")
{
    Bed bed;
    const String box[] = {String(u8"/somewhere/crate.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(box, 1u));
    // The two kinds, closed: a box each, no names yet (there are no option toggles here).
    gui::Node& detail = *bed.window->Detail();
    CHECK(Count<gui::CheckBox>(detail) == 2u);
    CHECK(Count<gui::TextField>(detail) == 0u);
    CHECK_FALSE(bed.window->IsKindOpen(pipeline::ImportResourceKind::Texture));
    // Opened, each kind lists its items, each kept or dropped and named.
    bed.window->SetKindOpen(pipeline::ImportResourceKind::Mesh, true);
    bed.window->SetKindOpen(pipeline::ImportResourceKind::Texture, true);
    CHECK(Count<gui::CheckBox>(*bed.window->Detail()) == 5u);
    CHECK(Count<gui::TextField>(*bed.window->Detail()) == 3u);
    // One file is listed too, under its importer's label, and the preview's facts name it.
    CHECK(Count<gui::CheckBox>(*bed.window->Files()) == 1u);
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"Box · 1"));
    CHECK(Texts(*bed.window->Facts()).AsView().Contains(u8"Box"));
    CHECK(bed.window->ImportButton()->GetText() == StringView(u8"Import"));
    CHECK(bed.window->ImportButton()->IsEnabled()); // enabled once the files are described
    CHECK(bed.window->Why() == StringView(u8"Into Content."));

    session->files[0].plan.entries[2].enabled = false; // Rust left out
    CHECK(bed.window->Import());
    CHECK_FALSE(bed.window->IsOpen());
    CHECK(bed.top->GetInstance(u8"Box") != nullptr);
    CHECK(bed.top->GetInstance(u8"Wood") != nullptr);
    CHECK(bed.top->GetInstance(u8"Rust") == nullptr);
    CHECK(session->closed); // the window released the session
}

TEST_CASE("import window: Import waits while a file is read, saying so, and opens when it is ready; several files are listed, none kept waits")
{
    Bed bed;
    const String two[] = {String(u8"/somewhere/a.box"), String(u8"/somewhere/b.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(two, 2u));
    CHECK(bed.window->ImportButton()->IsEnabled());
    CHECK(bed.window->Why() == StringView(u8"2 of 2 files into Content."));
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"Box · 2")); // both under their importer's label
    CHECK(bed.window->ImportButton()->GetText() == StringView(u8"Import 2 Files"));

    // Mark a file as a heavy importer's while the worker reads it, then report it ready.
    session->files[1].state = AssetImport::File::State::Reading;
    session->onChanged(1);
    CHECK_FALSE(bed.window->ImportButton()->IsEnabled());
    CHECK(bed.window->Why() == StringView(u8"Reading b.box..."));
    CHECK_FALSE(bed.window->Import()); // the window stays open
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

TEST_CASE("import window: the preview registered for the type a file is listed by, its facts and the size under it, made again only for another file, its facts shown again when its read arrives")
{
    Bed bed;
    usize made = 0;
    bed.context.ImportPreviews().Register(UniquePtr<editor::IImportPreviewFactory>(DefaultAllocator().New<NamePreviewFactory>(made), DefaultAllocator()));
    const String two[] = {String(u8"/somewhere/a.box"), String(u8"/somewhere/b.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(two, 2u));
    REQUIRE(bed.window->Preview() != nullptr);
    CHECK(made == 1u);
    CHECK(Texts(*bed.window->Facts()).AsView().Contains(u8"Made for a.box"));
    CHECK_FALSE(Texts(*bed.window->Facts()).AsView().Contains(u8"Box\n")); // the preview's facts replace the label

    // The same file again keeps its preview; another file gets its own.
    bed.window->Select(0);
    CHECK(made == 1u);
    bed.window->Select(1);
    CHECK(made == 2u);
    CHECK(Texts(*bed.window->Facts()).AsView().Contains(u8"Made for b.box"));

    // A preview's read coming back shows its facts again.
    static_cast<NamePreview*>(bed.window->Preview())->Arrive(u8"Read at last");
    CHECK(Texts(*bed.window->Facts()).AsView().Contains(u8"Read at last"));
    bed.window->Close();
}

TEST_CASE("import window: a file's settings, as its importer's options declare them, in the inspector's property grid; a file with none shows none")
{
    Bed bed;
    const String two[] = {String(u8"/somewhere/a.crate"), String(u8"/somewhere/b.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(two, 2u));
    // The crate's options declare Hollow: one grid of its settings.
    CHECK(Count<gui::PropertyGrid>(*bed.window->Detail()) == 1u);
    REQUIRE(session->files[0].options);
    CHECK(session->files[0].options->Settings().Size() == 1u);
    // The box's importer declares none: no grid.
    bed.window->Select(1);
    CHECK(Count<gui::PropertyGrid>(*bed.window->Detail()) == 0u);
    bed.window->Select(0);
    CHECK(Count<gui::PropertyGrid>(*bed.window->Detail()) == 1u);
    bed.window->Close();
}

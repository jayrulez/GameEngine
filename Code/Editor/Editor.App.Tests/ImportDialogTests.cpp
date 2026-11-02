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
    // Previews stopped so far, and how many had been when a box last imported: Import must stop
    // the preview before the import runs.
    Atomic<usize> g_previewStops{0};
    Atomic<usize> g_stopsWhenImported{0};

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
        // A texture tells its size; the rest say nothing.
        [[nodiscard]] Array<pipeline::ImportPartFact> DescribePart(StringView, const pipeline::ImportOptions*, Object*, const pipeline::ImportPlanEntry& part) const override
        {
            Array<pipeline::ImportPartFact> facts;
            if (part.kind == pipeline::ImportResourceKind::Texture)
                facts.PushBack(pipeline::ImportPartFact{String(u8"Size"), Format(u8"{} is 64 × 64", part.sourceName)});
            return facts;
        }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group& group, const pipeline::ImportOptions* options, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            g_stopsWhenImported.store(g_previewStops.load());
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

    // A preview naming its file, counting how many it has made. It shows a mesh part alone and
    // nothing else, saying what it was last asked to show.
    class NamePreview final : public editor::IImportPreview, public editor::IImportPartPreview
    {
    public:
        explicit NamePreview(StringView path) : m_view(MakeRef<gui::Label>(DefaultAllocator()))
        {
            m_facts.PushBack(Format(u8"Made for {}", pipeline::FileNameOf(path)));
            Provide<editor::IImportPartPreview>(*this);
        }
        bool ShowPart(const pipeline::ImportPlanEntry& part) override
        {
            shown = Format(u8"part {}", part.sourceName);
            return part.kind == pipeline::ImportResourceKind::Mesh;
        }
        void ShowWhole() override { shown = String(u8"whole"); }
        String shown;
        [[nodiscard]] gui::Node& View() override { return *m_view; }
        [[nodiscard]] Span<const String> Facts() const override { return m_facts.AsSpan(); }
        void StopReading() override { g_previewStops.fetch_add(1); }
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
        [[nodiscard]] UniquePtr<editor::IImportPreview> Create(const editor::ImportPreviewSource& file, TaskPool&, IAllocator& allocator) override
        {
            ++*m_made;
            return UniquePtr<editor::IImportPreview>(allocator.New<NamePreview>(file.path), allocator);
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
        // A hollow crate is lined: its plan has the lining's texture as well.
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView, const pipeline::ImportOptions* options, Object*) override
        {
            pipeline::ImportPlan plan;
            plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Asset, String(u8"Crate"), String(u8"Crate"), true});
            const auto* crate = Cast<CrateOptions>(options);
            if (crate != nullptr && crate->hollow)
                plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Texture, String(u8"Lining"), String(u8"Lining"), true});
            return plan;
        }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group& group, const pipeline::ImportOptions*, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            return group.CreateInstance(u8"Crate", WindowThing::StaticType());
        }
    };

    // A shelf's options: whether its books (textures) come in, a setting that leaves a whole kind out.
    class ShelfOptions final : public pipeline::ImportOptions
    {
        RTTI_OBJECT(ShelfOptions, pipeline::ImportOptions)
    public:
        bool books = true;
    };

    // Takes ".shelf": a board (a mesh) and, while books is on, two books (textures).
    class ShelfImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Shelf"; }
        [[nodiscard]] bool Accepts(StringView extension) const override { return extension == StringView(u8"shelf"); }
        [[nodiscard]] RefPtr<pipeline::ImportOptions> CreateOptions(IAllocator& allocator) const override { return MakeRef<ShelfOptions>(allocator); }
        [[nodiscard]] StringView KindSetting(pipeline::ImportResourceKind kind) const override
        {
            return kind == pipeline::ImportResourceKind::Texture ? StringView(u8"books") : StringView{};
        }
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView, const pipeline::ImportOptions* options, Object*) override
        {
            pipeline::ImportPlan plan;
            plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Mesh, String(u8"Board"), String(u8"Board"), true});
            const auto* shelf = Cast<ShelfOptions>(options);
            if (shelf == nullptr || shelf->books)
            {
                plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Texture, String(u8"Atlas"), String(u8"Atlas"), true});
                plan.entries.PushBack(pipeline::ImportPlanEntry{pipeline::ImportResourceKind::Texture, String(u8"Index"), String(u8"Index"), true});
            }
            return plan;
        }
        [[nodiscard]] Result<content::Instance*> Import(StringView, const pipeline::ImportContext&, content::Group& group, const pipeline::ImportOptions*, Object*,
                                                        Array<pipeline::DeferredImportWrite>*) override
        {
            return group.CreateInstance(u8"Shelf", WindowThing::StaticType());
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
            context.Importers().Register(UniquePtr<pipeline::IFileImporter>(DefaultAllocator().New<ShelfImporter>(), DefaultAllocator()));
            top = project->SourceDb().RootGroup();
            AssetImport::Services services;
            services.context = &context;
            import = MakeUnique<AssetImport>(DefaultAllocator(), Move(services));
            ImportWindow::Services windows;
            windows.import = import.Get();
            windows.previews = &context.ImportPreviews();
            windows.tasks = &context.Tasks();
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
REFLECT_MEMBERS(ShelfOptions, "rtti::tests::importwindow")
{
    builder.Property<&ShelfOptions::books>("books").PropAttribute("displayName", String(u8"Books"));
}
REFLECT_MEMBERS(CrateOptions, "rtti::tests::importwindow")
{
    builder.Property<&CrateOptions::hollow>("hollow").PropAttribute("displayName", String(u8"Hollow"));
}

TEST_CASE("import window: a file's parts in a tree under it, by kind, each kept; a part selected shows its name to give it and what the file says of it; Import makes what is kept and closes")
{
    Bed bed;
    const String box[] = {String(u8"/somewhere/crate.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(box, 1u));
    // Under the file, its two kinds, closed: a box each beside the file's own.
    gui::Node& files = *bed.window->Files();
    CHECK(Count<gui::CheckBox>(files) == 3u);
    CHECK_FALSE(bed.window->IsKindOpen(pipeline::ImportResourceKind::Texture));
    CHECK(Count<gui::TextField>(*bed.window->Detail()) == 0u); // the file's settings: no names there
    // Opened, each kind lists its items, each with its box.
    bed.window->SetKindOpen(pipeline::ImportResourceKind::Mesh, true);
    bed.window->SetKindOpen(pipeline::ImportResourceKind::Texture, true);
    CHECK(Count<gui::CheckBox>(*bed.window->Files()) == 6u);
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"2 of 2"));
    // A part selected: what it is, its name to give it, its box.
    bed.window->SelectPart(0, 1); // Wood
    CHECK(bed.window->SelectedPart() == 1);
    CHECK(Count<gui::TextField>(*bed.window->Detail()) == 1u);
    CHECK(Texts(*bed.window->Detail()).AsView().Contains(u8"Texture"));
    CHECK(Texts(*bed.window->Detail()).AsView().Contains(u8"Wood is 64 × 64")); // its facts, as the importer tells them
    bed.window->SelectPart(0, 0); // Box: a mesh, nothing to tell
    CHECK_FALSE(Texts(*bed.window->Detail()).AsView().Contains(u8"As the file has it"));
    bed.window->Select(0);
    CHECK(bed.window->SelectedPart() == -1);
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

    // A part selected in the tree is asked of the preview; one it cannot show leaves the whole file,
    // and selecting the file shows the whole file again.
    auto* named = static_cast<NamePreview*>(bed.window->Preview());
    CHECK(named->shown == StringView(u8"whole"));
    bed.window->SelectPart(1, 0); // b.box's Box, a mesh
    named = static_cast<NamePreview*>(bed.window->Preview());
    CHECK(named->shown == StringView(u8"part Box"));
    bed.window->SelectPart(1, 1); // its Wood, a texture this preview cannot show
    CHECK(named->shown == StringView(u8"whole"));
    bed.window->SelectPart(1, 0);
    bed.window->Select(1);
    CHECK(named->shown == StringView(u8"whole"));

    // A preview's read coming back shows its facts again.
    static_cast<NamePreview*>(bed.window->Preview())->Arrive(u8"Read at last");
    CHECK(Texts(*bed.window->Facts()).AsView().Contains(u8"Read at last"));

    // Import stops the preview first: the import may change what the importer read, which a
    // preview may be reading.
    const usize before = g_previewStops.load();
    CHECK(bed.window->Import());
    CHECK(g_previewStops.load() == before + 1u);
    CHECK(g_stopsWhenImported.load() == before + 1u); // stopped before the box imported
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

TEST_CASE("import window: a setting that changes what the file makes changes the tree under it")
{
    Bed bed;
    const String one[] = {String(u8"/somewhere/a.crate")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(one, 1u));
    CHECK(Count<gui::CheckBox>(*bed.window->Files()) == 1u); // the file alone: it makes one asset
    CHECK(Texts(*bed.window->Detail()).AsView().Contains(u8"Hollow")); // a setting no kind row stands for is shown
    static_cast<CrateOptions*>(session->files[0].options.Get())->hollow = true;
    bed.window->SettingChanged(0, u8"hollow");
    bed.root->GetContext().Update(Duration::FromSeconds(1.0 / 60.0)); // a frame: the window's deferred work runs
    CHECK(session->files[0].plan.entries.Size() == 2u);
    CHECK(Count<gui::CheckBox>(*bed.window->Files()) == 3u); // and now a tree under it: its asset's kind and its textures
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"1 of 1"));
    static_cast<CrateOptions*>(session->files[0].options.Get())->hollow = false;
    bed.window->SettingChanged(0, u8"hollow");
    bed.root->GetContext().Update(Duration::FromSeconds(1.0 / 60.0)); // a frame: the window's deferred work runs
    CHECK(Count<gui::CheckBox>(*bed.window->Files()) == 1u);
    bed.window->Close();
}

TEST_CASE("import window: a kind row is its importer's setting for the kind: unticked it leaves them all out and stays, ticked they come back; that setting is not shown twice")
{
    Bed bed;
    const String one[] = {String(u8"/somewhere/a.shelf")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(one, 1u));
    auto* options = static_cast<ShelfOptions*>(session->files[0].options.Get());
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"2 of 2"));
    CHECK_FALSE(Texts(*bed.window->Detail()).AsView().Contains(u8"Books")); // the Textures row is that setting

    bed.window->SetKindKept(0, pipeline::ImportResourceKind::Texture, false);
    bed.root->GetContext().Update(Duration::FromSeconds(1.0 / 60.0));
    CHECK_FALSE(options->books);
    CHECK(session->files[0].plan.entries.Size() == 1u); // the importer leaves them out
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"Left out")); // the row stays, to tick them back
    CHECK(Count<gui::CheckBox>(*bed.window->Files()) == 3u); // the file, Meshes, Textures

    bed.window->SetKindKept(0, pipeline::ImportResourceKind::Texture, true);
    bed.root->GetContext().Update(Duration::FromSeconds(1.0 / 60.0));
    CHECK(options->books);
    CHECK(session->files[0].plan.entries.Size() == 3u);
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"2 of 2"));

    // A kind its importer has no setting for: its box keeps or drops the parts, nothing else.
    bed.window->SetKindKept(0, pipeline::ImportResourceKind::Mesh, false);
    bed.root->GetContext().Update(Duration::FromSeconds(1.0 / 60.0));
    CHECK(options->books);
    CHECK_FALSE(session->files[0].plan.entries[0].enabled);
    CHECK(Texts(*bed.window->Files()).AsView().Contains(u8"0 of 1"));
    bed.window->Close();
}

TEST_CASE("import window: Apply to all <label> in this import gives the selected file's settings to the drop's other files of its importer")
{
    Bed bed;
    const String three[] = {String(u8"/somewhere/a.crate"), String(u8"/somewhere/b.crate"), String(u8"/somewhere/c.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(three, 3u));
    // Another crate in the drop: the button offers the settings to it.
    usize buttons = 0;
    for (usize i = 0; i < bed.window->Detail()->ChildCount(); ++i)
        if (auto* button = Cast<gui::Button>(bed.window->Detail()->GetChildAt(i)))
            buttons += button->GetText() == StringView(u8"Apply to all Crate in this import") ? 1u : 0u;
    CHECK(buttons == 1u);
    static_cast<CrateOptions*>(session->files[0].options.Get())->hollow = true;
    CHECK(bed.window->ShareSettings(0) == 1u); // the other crate; the box has its own importer
    CHECK(static_cast<CrateOptions*>(session->files[1].options.Get())->hollow);
    bed.window->Close();
}

TEST_CASE("import window: while a file's importer reads it, its preview waits, and is made when the read arrives")
{
    Bed bed;
    usize made = 0;
    bed.context.ImportPreviews().Register(UniquePtr<editor::IImportPreviewFactory>(DefaultAllocator().New<NamePreviewFactory>(made), DefaultAllocator()));
    const String two[] = {String(u8"/somewhere/a.box"), String(u8"/somewhere/b.box")};
    RefPtr<AssetImport::Session> session = bed.Open(Span<const String>(two, 2u));
    CHECK(made == 1u);
    // b.box as a heavy importer's file, still being read: selected, it waits.
    session->files[1].state = AssetImport::File::State::Reading;
    bed.window->Select(1);
    CHECK(made == 1u);
    CHECK(bed.window->Preview() == nullptr);
    // The read arrives: the preview is made, with it.
    session->files[1].state = AssetImport::File::State::Ready;
    session->onChanged(1);
    CHECK(made == 2u);
    CHECK(bed.window->Preview() != nullptr);
    bed.window->Close();
}

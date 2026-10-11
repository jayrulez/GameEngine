// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_manager_view partition.
//
// The PROJECT MANAGER screen (built into the single editor exe): shown at startup when no
// project was given on the command line, and returned to by File > Close Project.
//
// Design: no selection state at all. The standalone entry points (New Project... /
// Open Folder...) sit at the top and are always available; below them, every recent
// project is a CARD carrying its own [Open] and [Remove] buttons - the action is always
// one click, and an empty list shows a friendly empty state instead of dangling
// selection-dependent buttons. Rows are live-probed on every Rebuild (missing manifests
// render dim with no Open; newer-engine stamps render amber) - the list never lies about
// what Open will find.
//
// Everything with project consequences goes through callbacks the application binds (and
// defers through the UI mutation queue - opening detaches THIS view mid-dispatch):
//   OnOpenProject(dir)         - open an existing project (the app runs the version gate)
//   OnCreateProject(dir, name) - scaffold + open a new project at dir
//   OnStoreChanged()           - the registry changed; persist the settings store
// Remove is view-internal (registry mutation + list rebuild) but still queues its rebuild:
// the button lives in the very row the rebuild destroys.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:project_manager_view;

import foundation.core;
import foundation.settings;
import foundation.shell;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.fonts; // TextAlignment (the version column)
import engine.project;
import editor.core;
import foundation.image;
import foundation.image.io;
import :editor_icons;
import :view_mode_toggles;
import :project_picture;

using namespace foundation::core;
using namespace foundation;
namespace shell = foundation::shell;

export namespace editor::app
{
    namespace ui = foundation::ui;
    namespace fonts = foundation::fonts;
    namespace project = engine::project;

    class ProjectManagerView final
    {
    public:
        // The allocator (required - the editor app passes its tagged root) backs the
        // manager screen's whole view tree.
        /// `logo` (LoadEditorLogo) heads the screen; null shows the product's name as text.
        explicit ProjectManagerView(IAllocator& allocator, ui::DrawablePtr logo = {}) noexcept
            : m_allocator(&allocator), m_logo(Move(logo))
        {
        }

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        Function<void(StringView)> OnOpenProject;
        Function<void(StringView, StringView)> OnCreateProject;
        Function<void()> OnStoreChanged;

        ProjectManagerView() = default;
        ProjectManagerView(const ProjectManagerView&) = delete;
        ProjectManagerView& operator=(const ProjectManagerView&) = delete;

        void Build(editor::ProjectManagerController& controller,
                   shell::IDialogService* dialogs, ui::UIContext* uiContext, u32 width,
                   u32 height)
        {
            m_controller = &controller;
            m_dialogs = dialogs;
            m_uiContext = uiContext;
            m_root = MakeRef<ui::RootView>(Allocator());
            m_root->ViewportSize = Float2{static_cast<f32>(width), static_cast<f32>(height)};
            m_root->DpiScale = 1.0f;

            // Center a fixed-width column: [spacer][column 720][spacer].
            auto outer = MakeRef<ui::FlexLayout>(Allocator());
            outer->Direction = ui::Orientation::Horizontal;
            auto leftSpacer = MakeRef<ui::FlexLayout>(Allocator());
            auto rightSpacer = MakeRef<ui::FlexLayout>(Allocator());
            auto column = MakeRef<ui::FlexLayout>(Allocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 12;
            column->Padding = ui::Thickness{0, 28};
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                outer->AddView(leftSpacer.Get(), grow);
            }
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(720.0f));
                lp.Height = ui::SizeSpec::Match();
                outer->AddView(column.Get(), lp);
            }
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                outer->AddView(rightSpacer.Get(), grow);
            }

            // Header: the logo (else the name) + engine version (the manager IS the version
            // disambiguator).
            {
                auto header = MakeRef<ui::FlexLayout>(Allocator());
                header->Direction = ui::Orientation::Horizontal;
                header->Spacing = 12;
                header->AlignItems = ui::Align::Center;
                if (m_logo.Get() != nullptr)
                {
                    constexpr f32 kLogoHeight = 44.0f;
                    auto logo = MakeRef<ui::DrawableView>(Allocator(), m_logo, kLogoHeight * 931.0f / 200.0f,
                                                          kLogoHeight);
                    header->AddView(logo.Get());
                }
                else
                {
                    auto title = MakeRef<ui::Label>(Allocator());
                    title->SetText(u8"AssiduousEngine");
                    title->FontSize.SetValue(24.0f);
                    header->AddView(title.Get());
                }
                auto version = MakeRef<ui::Label>(Allocator());
                String v(u8"engine ");
                v += project::kEngineVersionString;
                version->SetText(v.AsView());
                version->FontSize.SetValue(12.0f);
                version->TextColor.SetValue(Optional<Color>(Color{0.55f, 0.55f, 0.55f, 1.0f}));
                header->AddView(version.Get());
                column->AddView(header.Get());
            }

            // Standalone entry points - always available, independent of the list below.
            {
                auto actions = MakeRef<ui::FlexLayout>(Allocator());
                actions->Direction = ui::Orientation::Horizontal;
                actions->Spacing = 8;
                ProjectManagerView* self = this;
                auto create =
                    MakeRef<ui::Button>(Allocator(), StringView(u8"New Project..."));
                create->OnClick.Add([self](ui::ButtonBase*) { self->ShowCreateDialog(); });
                actions->AddView(create.Get());
                auto browse =
                    MakeRef<ui::Button>(Allocator(), StringView(u8"Open Folder..."));
                browse->OnClick.Add([self](ui::ButtonBase*) { self->BrowseAndOpen(); });
                actions->AddView(browse.Get());
                column->AddView(actions.Get());
            }

            // "Recent projects", and the List / Grid toggle (remembered with the projects).
            {
                auto bar = MakeRef<ui::FlexLayout>(Allocator());
                bar->Direction = ui::Orientation::Horizontal;
                bar->AlignItems = ui::Align::Center;
                auto caption = MakeRef<ui::Label>(Allocator());
                caption->SetText(u8"Recent projects");
                caption->FontSize.SetValue(13.0f);
                caption->TextColor.SetValue(Optional<Color>(Color{0.72f, 0.72f, 0.72f, 1.0f}));
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                bar->AddView(caption.Get(), grow);
                m_viewToggles = MakeRef<ViewModeToggles>(Allocator());
                m_viewToggles->SetGridMode(m_controller->GridView());
                ProjectManagerView* self = this;
                m_viewToggles->OnModeChanged = [self](bool grid) { self->SetGridView(grid); };
                bar->AddView(m_viewToggles.Get());
                ui::LayoutStyle match;
                match.Width = ui::SizeSpec::Match();
                column->AddView(bar.Get(), match);
            }

            // The projects: rows, or tiles that wrap; rebuilt whole.
            m_listColumn = MakeRef<ui::FlexLayout>(Allocator());
            m_listColumn->Direction = ui::Orientation::Vertical;
            m_listColumn->Spacing = 6;
            auto scroll = MakeRef<ui::ScrollView>(Allocator());
            scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
            scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
            // The bar takes its own room beside the projects rather than lying over them (user).
            scroll->ScrollBarMode.SetValue(ui::ScrollBarModeValue::Reserved);
            m_listColumn->Padding = ui::Thickness{0, 0, 8, 0}; // a gap before the bar
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                scroll->AddView(m_listColumn.Get(), lp);
            }
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                column->AddView(scroll.Get(), lp);
            }

            m_status = MakeRef<ui::Label>(Allocator());
            m_status->FontSize.SetValue(12.0f);
            m_status->TextColor.SetValue(Optional<Color>(Color{0.7f, 0.7f, 0.7f, 1.0f}));
            column->AddView(m_status.Get());

            m_root->AddView(outer.Get());
            Rebuild();
        }

        [[nodiscard]] ui::RootView* Root() const noexcept { return m_root.Get(); }

        void SetStatus(StringView text) { m_status->SetText(text); }

        // Re-read the registry + live-probe every entry, then rebuild the projects as rows or
        // tiles. Call on every return to the manager; registry mutations queue a rebuild themselves.
        void Rebuild()
        {
            m_listColumn->RemoveAllViews();
            m_thumbnails.Clear(); // the views showing them went with the rows
            const bool grid = m_controller->GridView();
            m_listColumn->Direction = grid ? ui::Orientation::Horizontal : ui::Orientation::Vertical;
            m_listColumn->Wrap = grid;
            m_listColumn->Spacing = grid ? 14.0f : 6.0f;
            m_listColumn->LineSpacing = grid ? 14.0f : 0.0f; // between the grid's rows
            m_shownCards.Clear();

            const RecentProjectsSettings& reg = m_controller->Entries();
            if (reg.entries.IsEmpty())
            {
                m_listColumn->Direction = ui::Orientation::Vertical;
                auto empty = MakeRef<ui::Label>(Allocator());
                empty->SetText(u8"No projects yet. Create a new project, or open an existing "
                               u8"project folder.");
                empty->FontSize.SetValue(13.0f);
                empty->TextColor.SetValue(Optional<Color>(Color{0.5f, 0.5f, 0.5f, 1.0f}));
                empty->WordWrap.SetValue(true);
                auto pad = MakeRef<ui::FlexLayout>(Allocator());
                pad->Padding = ui::Thickness{4, 16};
                pad->AddView(empty.Get());
                m_listColumn->AddView(pad.Get());
                return;
            }

            for (const RecentProjectEntry& entry : reg.entries)
            {
                Card card;
                card.path = entry.path;
                card.name = entry.name;
                card.engineVersion = entry.engineVersion;
                engine::project::ProjectSettings probed;
                if (ProbeProject(entry.path.AsView(), probed).IsOk())
                {
                    card.name = probed.name;
                    card.engineVersion = probed.engineVersion;
                    card.relation = CompareProjectEngineVersion(probed.engineVersion.AsView());
                }
                else
                {
                    card.missing = true;
                }
                if (grid)
                {
                    AddTile(card);
                }
                else
                {
                    AddRow(card);
                }
                m_shownCards.PushBack(Move(card));
            }
        }

        /// For tests: the layout shown, the projects shown, and whether one has its picture.
        [[nodiscard]] bool IsGridView() { return m_controller->GridView(); }
        void SetGridView(bool grid)
        {
            if (grid == m_controller->GridView())
            {
                return;
            }
            m_controller->SetGridView(grid);
            if (m_viewToggles.Get() != nullptr)
            {
                m_viewToggles->SetGridMode(grid);
            }
            if (OnStoreChanged)
            {
                OnStoreChanged();
            }
            QueueRebuild();
        }
        [[nodiscard]] usize CardCount() const noexcept { return m_shownCards.Size(); }
        [[nodiscard]] usize ThumbnailCount() const noexcept { return m_thumbnails.Size(); }
        /// The row menu's items for a project, as its "more" button shows them (tests).
        [[nodiscard]] Array<String> MenuItemsFor(StringView path) const
        {
            Array<String> items;
            const bool missing = !FileExists(PathJoin(path, u8"Project.xml").AsView());
            if (!missing)
            {
                items.PushBack(String(u8"Open"));
            }
            items.PushBack(String(u8"Show in Folder"));
            items.PushBack(String(u8"Copy Path"));
            if (!missing)
            {
                items.PushBack(String(u8"Choose Thumbnail Image..."));
                if (FileExists(ProjectThumbnailPath(path).AsView()))
                {
                    items.PushBack(String(u8"Clear Thumbnail"));
                }
            }
            items.PushBack(String(u8"Remove from List"));
            return items;
        }
        /// Use `image` (any picture the editor reads) as the project's thumbnail, saved as a PNG
        /// in its editor folder. False when it does not read.
        bool SetThumbnailFromImage(StringView projectPath, StringView image)
        {
            foundation::image::Image picture;
            if (!foundation::image::io::LoadImage(image, picture).IsOk())
            {
                SetStatus(u8"That image could not be read.");
                return false;
            }
            const String target = ProjectThumbnailPath(projectPath);
            (void)CreateDirectories(PathParent(target.AsView()));
            if (!foundation::image::io::SaveImage(picture, target.AsView(), foundation::image::io::ImageFileFormat::PNG)
                     .IsOk())
            {
                SetStatus(u8"The thumbnail could not be saved (an 8-bit image is needed).");
                return false;
            }
            QueueRebuild();
            return true;
        }

        /// What a grid tile shows on hover for the project at `path`: the whole name, the engine
        /// it was made with (or why it will not open) and the path the tile has no room for, with
        /// the actions worth a click from there. Interactive, so the pointer can reach its buttons.
        /// Null when no card shows the project.
        [[nodiscard]] RefPtr<ui::View> TileTooltipFor(StringView path)
        {
            const Card* card = nullptr;
            for (const Card& shown : m_shownCards)
            {
                if (shown.path.AsView() == path)
                {
                    card = &shown;
                }
            }
            if (card == nullptr)
            {
                return {};
            }
            auto box = MakeRef<ui::FlexLayout>(Allocator());
            box->Direction = ui::Orientation::Vertical;
            box->Spacing = 3;
            box->Padding = ui::Thickness{2, 2};
            auto name = MakeRef<ui::Label>(Allocator(), card->name.IsEmpty() ? StringView(u8"(unnamed)") : card->name.AsView());
            name->FontSize.SetValue(14.0f);
            name->TextColor.SetValue(Optional<Color>(TitleColor(*card)));
            box->AddView(name.Get());
            String detail;
            if (card->missing)
            {
                detail = String(u8"Missing: there is no project in this folder now");
            }
            else
            {
                detail = card->engineVersion.IsEmpty() ? String(u8"Engine version unknown")
                                                       : Format(u8"Engine {}", card->engineVersion.AsView());
                if (card->relation == EngineVersionRelation::ProjectNewer)
                {
                    detail += u8", newer than this editor";
                }
            }
            auto detailLabel = MakeRef<ui::Label>(Allocator(), detail.AsView());
            detailLabel->FontSize.SetValue(11.5f);
            detailLabel->TextColor.SetValue(Optional<Color>(Color{0.6f, 0.6f, 0.6f, 1.0f}));
            box->AddView(detailLabel.Get());
            auto pathLabel = MakeRef<ui::Label>(Allocator(), card->path.AsView());
            pathLabel->FontSize.SetValue(11.5f);
            pathLabel->TextColor.SetValue(Optional<Color>(Color{0.75f, 0.75f, 0.75f, 1.0f}));
            box->AddView(pathLabel.Get());

            auto actions = MakeRef<ui::FlexLayout>(Allocator());
            actions->Direction = ui::Orientation::Horizontal;
            actions->Spacing = 6;
            actions->Padding = ui::Thickness{0, 6, 0, 0};
            ProjectManagerView* self = this;
            const String projectPath = card->path;
            static constexpr StringView kActions[] = {u8"Open", u8"Show in Folder", u8"Copy Path"};
            for (const StringView item : kActions)
            {
                if (item == u8"Open" && card->missing)
                {
                    continue;
                }
                auto button = MakeRef<ui::Button>(Allocator(), item);
                button->OnClick.Add([self, projectPath, which = String(item)](ui::ButtonBase*)
                                    { self->RunCardMenuItem(projectPath, which.AsView()); });
                ui::LayoutStyle height;
                height.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(26.0f));
                actions->AddView(button.Get(), height);
            }
            box->AddView(actions.Get());
            return RefPtr<ui::View>(box.Get());
        }

    private:
        // A tile's picture: a click opens the project, a hover shows TileTooltipFor.
        class TileButton final : public ui::Button, public ui::ITooltipProvider
        {
        public:
            TileButton(ProjectManagerView& owner, StringView path)
                : ui::Button(StringView(u8"")), m_owner(owner), m_path(path)
            {
                IsTooltipInteractive = true;
            }
            [[nodiscard]] ui::ITooltipProvider* AsTooltipProvider() override { return this; }
            [[nodiscard]] RefPtr<ui::View> CreateTooltipContent() override
            {
                return m_owner.TileTooltipFor(m_path.AsView());
            }

        private:
            ProjectManagerView& m_owner;
            String m_path;
        };

        struct Card
        {
            String path;
            String name;
            String engineVersion;
            EngineVersionRelation relation = EngineVersionRelation::Same;
            bool missing = false;
        };

        // The card's title colour and suffix: missing dims, a newer engine warns.
        [[nodiscard]] static Color TitleColor(const Card& card)
        {
            return card.missing ? Color{0.5f, 0.5f, 0.5f, 1.0f}
                   : card.relation == EngineVersionRelation::ProjectNewer ? Color{0.95f, 0.75f, 0.3f, 1.0f}
                                                                          : Color{0.9f, 0.9f, 0.9f, 1.0f};
        }
        [[nodiscard]] static String Subtitle(const Card& card)
        {
            String text(card.engineVersion.IsEmpty() ? StringView(u8"") : card.engineVersion.AsView());
            if (card.missing)
            {
                text += text.IsEmpty() ? u8"missing" : u8" \u00b7 missing";
            }
            else if (card.relation == EngineVersionRelation::ProjectNewer)
            {
                text += u8" \u00b7 newer engine";
            }
            return text;
        }

        // The project's picture (its thumbnail, else a tile with its initial), `w` x `h`.
        RefPtr<ui::View> Picture(const Card& card, f32 w, f32 h, f32 radius)
        {
            return MakeProjectPicture(Allocator(), card.missing ? StringView() : card.path.AsView(),
                                      card.name.AsView(), w, h, radius, m_thumbnails);
        }

        // The "more" button: the project's menu.
        RefPtr<ui::IconButton> MoreButton(const Card& card)
        {
            auto more = MakeRef<ui::IconButton>(Allocator(), EditorIcons::Get().more.Get(), 16.0f);
            more->TooltipText = String(u8"More for this project");
            // A button's surface, so it reads as one beside Open: a quiet rounded fill.
            auto look = MakeRef<ui::StateListDrawable>(Allocator());
            const auto fill = [this](f32 alpha)
            {
                return ui::DrawablePtr(
                    MakeRef<ui::RoundedRectDrawable>(Allocator(), Color{1.0f, 1.0f, 1.0f, alpha}, 4.0f).Get());
            };
            look->Set(ui::ControlState::Normal, fill(0.045f));
            look->Set(ui::ControlState::Hover, fill(0.09f));
            look->Set(ui::ControlState::Pressed, fill(0.13f));
            more->SetStyle(ui::StyleProperty::Background, ui::DrawablePtr(look.Get()));
            ProjectManagerView* self = this;
            const String path = card.path;
            ui::IconButton* raw = more.Get();
            more->OnClick.Add([self, path, raw](ui::ButtonBase*) { self->ShowCardMenu(path, *raw); });
            return more;
        }

        void OpenCard(const String& path)
        {
            if (OnOpenProject)
            {
                OnOpenProject(path.AsView());
            }
        }

        void AddRow(const Card& card)
        {
            constexpr f32 kButtonHeight = 28.0f;
            auto row = MakeRef<ui::FlexLayout>(Allocator());
            row->Direction = ui::Orientation::Horizontal;
            row->AlignItems = ui::Align::Center;
            row->Spacing = 12;
            row->Padding = ui::Thickness{10, 8};
            row->AddView(Picture(card, 76.0f, 43.0f, 4.0f).Get());

            // The name (and what is wrong with the project, if anything) over the dim path; both
            // take only the room left and end in an ellipsis rather than run under the buttons.
            auto text = MakeRef<ui::FlexLayout>(Allocator());
            text->Direction = ui::Orientation::Vertical;
            text->Spacing = 2;
            {
                ui::LayoutStyle line;
                line.Width = ui::SizeSpec::Match();
                auto nameLabel = MakeRef<ui::Label>(Allocator());
                String title(card.name.IsEmpty() ? StringView(u8"(unnamed)") : card.name.AsView());
                if (card.missing)
                {
                    title += u8"   (missing)";
                }
                else if (card.relation == EngineVersionRelation::ProjectNewer)
                {
                    title += u8"   (newer engine)";
                }
                nameLabel->SetText(title.AsView());
                nameLabel->FontSize.SetValue(14.0f);
                nameLabel->Ellipsis.SetValue(true);
                nameLabel->TextColor.SetValue(Optional<Color>(TitleColor(card)));
                text->AddView(nameLabel.Get(), line);

                auto pathLabel = MakeRef<ui::Label>(Allocator());
                pathLabel->SetText(card.path.AsView());
                pathLabel->FontSize.SetValue(11.0f);
                pathLabel->Ellipsis.SetValue(true);
                pathLabel->TooltipText = card.path;
                pathLabel->TextColor.SetValue(Optional<Color>(Color{0.5f, 0.5f, 0.5f, 1.0f}));
                text->AddView(pathLabel.Get(), line);
            }
            {
                ui::LayoutStyle rest; // no width of its own (basis 0): the room the others leave
                rest.FlexBasis = ui::Unit::Dp(0.0f);
                rest.FlexGrow = 1.0f;
                row->AddView(text.Get(), rest);
            }

            // The engine version in its own column, so the rows' versions line up.
            {
                auto version = MakeRef<ui::Label>(Allocator(), card.engineVersion.AsView());
                version->FontSize.SetValue(11.5f);
                version->HAlign.SetValue(fonts::TextAlignment::Right);
                version->TextColor.SetValue(Optional<Color>(Color{0.55f, 0.55f, 0.55f, 1.0f}));
                ui::LayoutStyle fixed;
                fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(56.0f));
                row->AddView(version.Get(), fixed);
            }

            ui::LayoutStyle button;
            button.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(kButtonHeight));
            if (!card.missing)
            {
                auto open = MakeRef<ui::Button>(Allocator(), StringView(u8"Open"));
                ProjectManagerView* self = this;
                const String path = card.path;
                open->OnClick.Add([self, path](ui::ButtonBase*) { self->OpenCard(path); });
                ui::LayoutStyle wide = button;
                wide.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(64.0f));
                row->AddView(open.Get(), wide);
            }
            {
                ui::LayoutStyle square = button; // as tall as Open
                square.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(kButtonHeight));
                row->AddView(MoreButton(card).Get(), square);
            }

            // The card surface: a themed Panel (the stylesheet's "panel" class = the
            // palette's Surface color) so rows read as cards against the window background.
            auto surface = MakeRef<ui::Panel>(Allocator());
            surface->AddClass(u8"panel");
            {
                ui::LayoutStyle rowLp;
                rowLp.Width = ui::SizeSpec::Match();
                surface->AddView(row.Get(), rowLp);
            }
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            m_listColumn->AddView(surface.Get(), lp);
        }

        // A tile: the project's picture, then its name and engine with the "more" button; a click
        // on the picture opens it.
        void AddTile(const Card& card)
        {
            // Three to a row in the launcher's 720 px column, less the scrollbar and its gap.
            constexpr f32 kTileWidth = 220.0f;
            constexpr f32 kPictureHeight = 115.0f; // the picture 204 wide: about 16:9
            auto tile = MakeRef<ui::FlexLayout>(Allocator());
            tile->Direction = ui::Orientation::Vertical;
            tile->Spacing = 6;
            tile->Padding = ui::Thickness{8, 8};

            auto picture = MakeRef<TileButton>(Allocator(), *this, card.path.AsView());
            picture->SetStyle(ui::StyleProperty::Padding, ui::Thickness{0.0f, 0.0f});
            picture->SetStyle(ui::StyleProperty::Background,
                              ui::DrawablePtr(MakeRef<ui::RoundedRectDrawable>(Allocator(), Color{0, 0, 0, 0}, 6.0f).Get()));
            // The tile shows no path, so its tooltip does (two projects of one name tell apart).
            // A missing project's picture stays enabled for that tooltip (Show in Folder and Copy
            // Path still apply), and a click on it does nothing.
            if (!card.missing)
            {
                ProjectManagerView* self = this;
                const String path = card.path;
                picture->OnClick.Add([self, path](ui::ButtonBase*) { self->OpenCard(path); });
            }
            auto pictureStack = MakeRef<ui::Panel>(Allocator());
            pictureStack->AddView(Picture(card, kTileWidth - 16.0f, kPictureHeight, 6.0f).Get());
            pictureStack->AddView(picture.Get()); // the click target over the picture
            tile->AddView(pictureStack.Get());

            auto line = MakeRef<ui::FlexLayout>(Allocator());
            line->Direction = ui::Orientation::Horizontal;
            line->AlignItems = ui::Align::Center;
            line->Spacing = 6;
            auto text = MakeRef<ui::FlexLayout>(Allocator());
            text->Direction = ui::Orientation::Vertical;
            text->Spacing = 1;
            auto nameLabel = MakeRef<ui::Label>(Allocator());
            nameLabel->SetText(card.name.IsEmpty() ? StringView(u8"(unnamed)") : card.name.AsView());
            nameLabel->FontSize.SetValue(13.5f);
            nameLabel->Ellipsis.SetValue(true);
            nameLabel->TooltipText = card.path;
            nameLabel->TextColor.SetValue(Optional<Color>(TitleColor(card)));
            {
                ui::LayoutStyle match;
                match.Width = ui::SizeSpec::Match();
                text->AddView(nameLabel.Get(), match);
            }
            auto sub = MakeRef<ui::Label>(Allocator(), Subtitle(card).AsView());
            sub->FontSize.SetValue(11.0f);
            sub->TextColor.SetValue(Optional<Color>(Color{0.55f, 0.55f, 0.55f, 1.0f}));
            text->AddView(sub.Get());
            ui::LayoutStyle grow; // the room the "more" button leaves (basis 0), names cut short
            grow.FlexBasis = ui::Unit::Dp(0.0f);
            grow.FlexGrow = 1.0f;
            line->AddView(text.Get(), grow);
            line->AddView(MoreButton(card).Get());
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            tile->AddView(line.Get(), match);

            auto surface = MakeRef<ui::Panel>(Allocator());
            surface->AddClass(u8"panel");
            surface->AddView(tile.Get());
            ui::LayoutStyle fixed;
            fixed.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(kTileWidth));
            m_listColumn->AddView(surface.Get(), fixed);
        }

        void ShowCardMenu(const String& path, ui::View& anchor)
        {
            if (m_uiContext == nullptr)
            {
                return;
            }
            auto menu = MakeRef<ui::ContextMenu>(Allocator());
            ProjectManagerView* self = this;
            for (const String& item : MenuItemsFor(path.AsView()))
            {
                const StringView label = item.AsView();
                if (label == u8"Remove from List")
                {
                    menu->AddSeparator();
                }
                menu->AddItem(label,
                              [self, path, which = String(label)]()
                              { self->RunCardMenuItem(path, which.AsView()); });
            }
            const Float2 at = anchor.LocalToScreen(Float2{0.0f, anchor.Height()});
            menu->Show(m_uiContext, at.x, at.y);
        }

        void RunCardMenuItem(const String& path, StringView item)
        {
            if (item == u8"Open")
            {
                OpenCard(path);
            }
            else if (item == u8"Show in Folder")
            {
                if (m_dialogs != nullptr)
                {
                    m_dialogs->OpenPath(path.AsView());
                }
            }
            else if (item == u8"Copy Path")
            {
                if (m_uiContext != nullptr && m_uiContext->Clipboard() != nullptr)
                {
                    (void)m_uiContext->Clipboard()->SetText(path.AsView());
                    SetStatus(u8"Copied the project's path.");
                }
            }
            else if (item == u8"Choose Thumbnail Image...")
            {
                if (m_dialogs == nullptr)
                {
                    return;
                }
                static constexpr foundation::shell::FileFilter kImages[] = {{u8"Images", u8"png;jpg;jpeg;bmp;tga"}};
                ProjectManagerView* self = this;
                m_dialogs->ShowOpenFile(foundation::shell::DialogResultCallback{
                                            [self, path](Span<const String> paths)
                                            {
                                                if (paths.Size() > 0) // none: cancelled
                                                {
                                                    (void)self->SetThumbnailFromImage(path.AsView(), paths[0].AsView());
                                                }
                                            }},
                                        Span<const foundation::shell::FileFilter>(kImages, 1));
            }
            else if (item == u8"Clear Thumbnail")
            {
                (void)FileDelete(ProjectThumbnailPath(path.AsView()).AsView());
                QueueRebuild();
            }
            else if (item == u8"Remove from List")
            {
                RemoveEntry(path);
            }
        }

        // Rebuild after the click that asked for it is over (the clicked view is in what the
        // rebuild destroys); at once when not in a UI yet (tests).
        void QueueRebuild()
        {
            if (m_uiContext == nullptr)
            {
                Rebuild();
                return;
            }
            ProjectManagerView* self = this;
            m_uiContext->MutationQueueRef().QueueAction(Function<void()>{[self]() { self->Rebuild(); }});
        }

        void RemoveEntry(const String& path)
        {
            if (!m_controller->Remove(path.AsView()))
            {
                return;
            }
            if (OnStoreChanged)
            {
                OnStoreChanged();
            }
            // The menu that asked lives in the row the rebuild destroys - defer (the
            // mutation-queue rule: never mutate the tree mid-event-dispatch).
            QueueRebuild();
        }

        void BrowseAndOpen()
        {
            if (m_dialogs == nullptr)
            {
                return;
            }
            ProjectManagerView* self = this;
            m_dialogs->ShowOpenFolder(foundation::shell::DialogResultCallback{
                [self](Span<const String> paths)
                {
                    if (paths.Size() == 0)
                    {
                        return; // cancelled
                    }
                    engine::project::ProjectSettings probed;
                    if (!ProbeProject(paths[0].AsView(), probed).IsOk())
                    {
                        self->SetStatus(
                            u8"Not a project (no Project.xml there). Use New Project to start "
                            u8"one.");
                        return;
                    }
                    if (self->OnOpenProject)
                    {
                        self->OnOpenProject(paths[0].AsView());
                    }
                }});
        }

        void ShowCreateDialog()
        {
            RefPtr<ui::Dialog> dialog =
                MakeRef<ui::Dialog>(Allocator(), StringView(u8"New Project"));
            auto column = MakeRef<ui::FlexLayout>(Allocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6;

            auto nameEdit = MakeRef<ui::EditText>(Allocator());
            nameEdit->SetPlaceholder(u8"Project name");
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column->AddView(nameEdit.Get(), lp);
            }

            auto dirRow = MakeRef<ui::FlexLayout>(Allocator());
            dirRow->Direction = ui::Orientation::Horizontal;
            dirRow->Spacing = 6;
            auto dirEdit = MakeRef<ui::EditText>(Allocator());
            dirEdit->SetPlaceholder(u8"Parent directory (the project is created inside it)");
            {
                ui::LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                dirRow->AddView(dirEdit.Get(), lp);
            }
            auto browse = MakeRef<ui::Button>(Allocator(), StringView(u8"Browse..."));
            {
                ProjectManagerView* self = this;
                ui::EditText* dirRaw = dirEdit.Get();
                browse->OnClick.Add(
                    [self, dirRaw](ui::ButtonBase*)
                    {
                        if (self->m_dialogs == nullptr)
                        {
                            return;
                        }
                        self->m_dialogs->ShowOpenFolder(foundation::shell::DialogResultCallback{
                            [dirRaw](Span<const String> paths)
                            {
                                if (paths.Size() > 0)
                                {
                                    dirRaw->SetText(paths[0].AsView());
                                }
                            }});
                    });
            }
            dirRow->AddView(browse.Get());
            {
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column->AddView(dirRow.Get(), lp);
            }
            dialog->SetContent(column.Get());
            dialog->MinWidth.SetValue(460.0f);

            ui::Dialog* rawDialog = dialog.Get();
            ui::EditText* nameRaw = nameEdit.Get();
            ui::EditText* dirRaw = dirEdit.Get();
            ProjectManagerView* self = this;
            ui::Button* create = dialog->AddButton(u8"Create", ui::DialogResult::None);
            create->OnClick.Add(
                [self, rawDialog, nameRaw, dirRaw](ui::ButtonBase*)
                {
                    const StringView name = nameRaw->Text();
                    const StringView parent = dirRaw->Text();
                    if (name.IsEmpty() || parent.IsEmpty())
                    {
                        return; // both fields required; dialog stays up
                    }
                    rawDialog->Close(ui::DialogResult::OK);
                    if (self->OnCreateProject)
                    {
                        self->OnCreateProject(PathJoin(parent, name).AsView(), name);
                    }
                });
            dialog->AddButton(u8"Cancel", ui::DialogResult::Cancel);
            dialog->Show(m_uiContext);
        }

        editor::ProjectManagerController* m_controller = nullptr;
        shell::IDialogService* m_dialogs = nullptr;
        ui::UIContext* m_uiContext = nullptr;
        RefPtr<ui::RootView> m_root;
        RefPtr<ui::FlexLayout> m_listColumn;
        RefPtr<ui::Label> m_status;
        RefPtr<ViewModeToggles> m_viewToggles;
        Array<UniquePtr<foundation::image::Image>> m_thumbnails; // the pictures the cards borrow
        Array<Card> m_shownCards; // what the list shows, in order (a tile's tooltip reads its own)
    
        IAllocator* m_allocator;
        ui::DrawablePtr m_logo; // the screen's heading (null: the name as text)
    };
}

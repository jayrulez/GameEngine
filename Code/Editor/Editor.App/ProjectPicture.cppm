// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_picture partition.
//
// A project's picture, as the launcher's cards and the Project page show it: the thumbnail the
// editor keeps in the project's editor folder (ProjectThumbnailPath), else the project's initial
// on a quiet tile.

module;
#include "Core/Prelude.h"

export module editor.app:project_picture;

import foundation.core;
import foundation.ui;
import foundation.vg;    // CornerRadii (a thumbnail's rounding)
import foundation.fonts; // TextAlignment (the placeholder's initial)
import foundation.image;
import foundation.image.io;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;

    /// The picture of the project named `name` in `projectDirectory`, `w` x `h` with corners of
    /// `radius`: its thumbnail when one reads (an empty directory never does), else its initial.
    /// The image a thumbnail view borrows is moved into `keep`, which must outlive the view.
    [[nodiscard]] inline RefPtr<ui::View> MakeProjectPicture(IAllocator& allocator, StringView projectDirectory,
                                                             StringView name, f32 w, f32 h, f32 radius,
                                                             Array<UniquePtr<foundation::image::Image>>& keep)
    {
        ui::LayoutStyle size;
        size.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(w));
        size.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(h));
        auto frame = MakeRef<ui::FlexLayout>(allocator);
        auto image = MakeUnique<foundation::image::Image>(allocator);
        if (!projectDirectory.IsEmpty() &&
            foundation::image::io::LoadImage(ProjectThumbnailPath(projectDirectory).AsView(), *image).IsOk())
        {
            auto view = MakeRef<ui::ImageView>(allocator, image.Get());
            view->ScaleType.SetValue(ui::ScaleType::CenterCrop);
            view->CornerRadius.SetValue(foundation::vg::CornerRadii(radius));
            keep.PushBack(Move(image)); // the view borrows it
            frame->AddView(view.Get(), size);
            return RefPtr<ui::View>(frame.Get());
        }
        // No picture yet: the project's initial on a quiet tile.
        auto tile = MakeRef<ui::Panel>(allocator);
        tile->SetStyle(ui::StyleProperty::Background,
                       ui::DrawablePtr(MakeRef<ui::RoundedRectDrawable>(allocator, Color{1, 1, 1, 0.05f}, radius,
                                                                        Color{1, 1, 1, 0.08f}, 1.0f)
                                           .Get()));
        String initial;
        initial.Append(name.IsEmpty() ? utf8char('?') : name[0]);
        auto letter = MakeRef<ui::Label>(allocator, initial.AsView());
        letter->FontSize.SetValue(h * 0.42f);
        letter->TextColor.SetValue(Optional<Color>(Color{1, 1, 1, 0.35f}));
        letter->HAlign.SetValue(foundation::fonts::TextAlignment::Center);
        letter->VAlign.SetValue(foundation::fonts::VerticalAlignment::Middle);
        tile->AddView(letter.Get());
        frame->AddView(tile.Get(), size);
        return RefPtr<ui::View>(frame.Get());
    }
}

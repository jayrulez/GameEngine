// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :project_picture partition.
//
// A project's picture: the thumbnail the editor keeps in the project's editor folder
// (ProjectThumbnailPath), else the project's initial on a quiet tile. Its size and corners come
// from the sheet (`.project-picture`, plus a class the caller gives: the home's header, a
// manager's row).
module;
#include "Core/Prelude.h"

export module editor.app:project_picture;

import foundation.core;
import foundation.gui;
import foundation.image;
import foundation.image.io;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    // A class of one function rather than a function: gcc 15 writes a module it cannot read back
    // when an inline function at namespace scope here instantiates templates (MakeRef<Label>).
    struct ProjectPicture
    {
        /// The picture of the project named `name` in `projectDirectory`, with the class `size`
        /// (the sheet's size for where it shows): its thumbnail when one can be read (an empty
        /// directory never has one), else its initial. The image a thumbnail shows is moved into `keep`, which
        /// must outlive the picture.
        [[nodiscard]] static RefPtr<gui::Node> Make(IAllocator& allocator, StringView projectDirectory, StringView name, StringView size,
                                                    Array<UniquePtr<foundation::image::Image>>& keep)
        {
        auto image = MakeUnique<foundation::image::Image>(allocator);
        if (!projectDirectory.IsEmpty() && foundation::image::io::LoadImage(ProjectThumbnailPath(projectDirectory).AsView(), *image).IsOk())
        {
            RefPtr<gui::Image> picture = MakeRef<gui::Image>(allocator);
            picture->AddClass(u8"project-picture");
            picture->AddClass(size);
            picture->SetFit(gui::ObjectFit::Cover);
            picture->SetDrawable(MakeRef<gui::ImageDrawable>(allocator, image.Get()));
            keep.PushBack(Move(image)); // the picture borrows it
            return RefPtr<gui::Node>(picture);
        }
        // No picture yet: the project's initial on a quiet tile.
        RefPtr<gui::Panel> tile = MakeRef<gui::Panel>(allocator);
        tile->AddClass(u8"project-picture");
        tile->AddClass(u8"project-initial");
        tile->AddClass(size);
        RefPtr<gui::Label> letter = MakeRef<gui::Label>(allocator);
        String initial;
        initial.Append(name.IsEmpty() ? utf8char('?') : name[0]);
        letter->SetText(initial.AsView());
        tile->AddChild(letter.Get());
        return RefPtr<gui::Node>(tile);
        }
    };
}

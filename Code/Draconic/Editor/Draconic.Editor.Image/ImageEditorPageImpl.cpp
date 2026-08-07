// Draconic::EditorImage - the `draconic.editor.image` module (implementation).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.image;

import draconic.foundation;
import draconic.content;
import draconic.image;
import draconic.image.io;
import draconic.image.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace
    {
        constexpr StringView kColorSpaceItems[] = {u8"sRGB", u8"Linear"};

        [[nodiscard]] StringView PixelFormatLabel(image::PixelFormat format)
        {
            switch (format)
            {
            case image::PixelFormat::R8:
                return u8"R8";
            case image::PixelFormat::RG8:
                return u8"RG8";
            case image::PixelFormat::RGB8:
                return u8"RGB8";
            case image::PixelFormat::RGBA8:
                return u8"RGBA8";
            case image::PixelFormat::RGBA32F:
                return u8"RGBA32F (HDR)";
            default:
                return u8"?";
            }
        }
    } // namespace

    ImageEditorPage::ImageEditorPage(EditorContext& context, draconic::content::Instance& instance)
        : m_context(&context), m_title(instance.Name())
    {
        SetInstanceId(instance.Id());

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<image::ImageAsset>(Cast<image::ImageAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"Editor", u8"image '{}' failed to read - page opens empty",
                               m_title);
        }
        else
        {
            LoadPreview();
        }
        m_undoBaseline = Snapshot();

        // Left: source facts label + the preview image (fit-centered).
        m_image = MakeRef<ui::ImageView>(DefaultAllocator());
        m_image->ScaleType.SetValue(ui::ScaleType::FitCenter);
        m_image->SetImage(m_preview.Get());

        m_info = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8""));
        m_info->FontSize.SetValue(12.0f);

        auto previewColumn = MakeRef<ui::FlexLayout>(DefaultAllocator());
        previewColumn->Direction = ui::Orientation::Vertical;
        previewColumn->Spacing = 6.0f;
        previewColumn->Padding = ui::Thickness{8, 6};
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            previewColumn->AddView(m_info.Get(), lp);
        }
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            lp->Grow = 1.0f;
            previewColumn->AddView(m_image.Get(), lp);
        }

        m_grid = MakeRef<ui::toolkit::PropertyGrid>(DefaultAllocator());
        BuildGrid();

        auto gridColumn = MakeRef<ui::FlexLayout>(DefaultAllocator());
        gridColumn->Direction = ui::Orientation::Vertical;
        gridColumn->Padding = ui::Thickness{8, 6};
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Grow = 1.0f;
            gridColumn->AddView(m_grid.Get(), lp);
        }

        auto split = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
        split->SetSplitRatio(0.6f);
        split->SetPanes(previewColumn.Get(), gridColumn.Get());
        m_content = split;

        RefreshInfo();
    }

    void ImageEditorPage::LoadPreview()
    {
        if (m_asset.Get() == nullptr || m_asset->fileName.IsEmpty() ||
            m_context->Project() == nullptr)
        {
            return;
        }
        const String path =
            PathJoin(m_context->Project()->SourcesRoot().AsView(), m_asset->fileName.View());
        image::Image source;
        const Status loaded = image::io::LoadImage(path.AsView(), source);
        if (!loaded.IsOk())
        {
            DRACONIC_LOG_WARNING(u8"Editor", u8"image source missing or undecodable: {}", path);
            return;
        }
        m_sourceFormat = source.Format();
        m_sourceBytes = source.PixelData().Size();
        // Display path is 8-bit: convert through the image library (floats clamp).
        const image::Image display = (source.Format() == image::PixelFormat::RGBA8)
                                         ? Move(source)
                                         : source.ConvertFormat(image::PixelFormat::RGBA8);
        if (display.Width() == 0 || display.Height() == 0)
        {
            return;
        }
        m_preview = MakeUnique<image::OwnedImageData>(DefaultAllocator(), display.Width(),
                                                      display.Height(), image::PixelFormat::RGBA8,
                                                      display.PixelData(), m_asset->colorSpace);
    }

    void ImageEditorPage::RefreshInfo()
    {
        if (m_asset.Get() == nullptr)
        {
            m_info->SetText(u8"Image failed to load.");
            return;
        }
        if (m_preview.Get() == nullptr)
        {
            m_info->SetText(u8"No preview (source file missing or undecodable).");
            return;
        }
        String text(m_asset->fileName.View());
        text.Append(Format(u8"  |  {} x {}  |  {}  |  {} KiB", m_preview->Width(),
                           m_preview->Height(), PixelFormatLabel(m_sourceFormat),
                           m_sourceBytes / 1024)
                        .AsView());
        m_info->SetText(text.AsView());
    }

    void ImageEditorPage::BuildGrid()
    {
        m_grid->Clear();
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        ImageEditorPage* self = this;

        // The one authored field: how consumers interpret the pixels.
        auto colorSpace = MakeRef<ui::toolkit::EnumEditor>(
            DefaultAllocator(), u8"Color Space", static_cast<i32>(m_asset->colorSpace),
            Span<const StringView>{kColorSpaceItems, 2},
            Function<void(i32)>{[self](i32 v)
                                {
                                    self->m_asset->colorSpace =
                                        static_cast<image::ImageColorSpace>(v);
                                    Array<byte> after = self->Snapshot();
                                    (void)self->Commands().Execute(UniquePtr<IEditorCommand>(
                                        DefaultAllocator().New<EditImageCommand>(
                                            *self, u8"color-space", self->m_undoBaseline, after),
                                        DefaultAllocator()));
                                    self->m_undoBaseline = Move(after);
                                    self->MarkDirty();
                                }},
            u8"Import");
        m_colorSpaceRow = colorSpace.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(colorSpace.Get()));

        // Read-only source facts.
        auto stat = [&](StringView name, String value)
        {
            m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::StringEditor>(DefaultAllocator(), name, value.AsView(),
                                                   Function<void(StringView)>{}, u8"Source")
                    .Get()));
        };
        stat(u8"File", String(m_asset->fileName.View()));
        if (m_preview.Get() != nullptr)
        {
            stat(u8"Dimensions", Format(u8"{} x {}", m_preview->Width(), m_preview->Height()));
            stat(u8"Pixel Format", String(PixelFormatLabel(m_sourceFormat)));
            stat(u8"Data Size", Format(u8"{} KiB", m_sourceBytes / 1024));
        }
    }

    Array<byte> ImageEditorPage::Snapshot() const
    {
        Array<byte> blob;
        if (m_asset.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        m_asset->Serialize(ar);
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void ImageEditorPage::ApplyBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        Array<byte> current = Snapshot();
        if (current.Size() == blob.Size())
        {
            bool same = true;
            for (usize i = 0; i < blob.Size(); ++i)
            {
                if (current[i] != blob[i])
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return;
            }
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        m_asset->Serialize(ar);
        m_undoBaseline = blob;
        // One editable row: re-pull it (no grid rebuild needed).
        if (m_colorSpaceRow != nullptr)
        {
            m_colorSpaceRow->SetValue(static_cast<i32>(m_asset->colorSpace));
        }
        MarkDirty();
    }

    Status ImageEditorPage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        draconic::content::Instance* instance =
            m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status saved = instance->WriteObject(*m_asset);
        if (saved.IsOk())
        {
            ClearDirty();
            m_context->RequestCook(false);
            DRACONIC_LOG_INFO(u8"Editor", u8"saved image '{}'", m_title);
        }
        return saved;
    }

    const TypeInfo* ImageEditorPageFactory::PrimaryType() const
    {
        return &image::ImageAsset::StaticType();
    }

    UniquePtr<EditorPage> ImageEditorPageFactory::CreatePage(EditorContext& context,
                                                             draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<ImageEditorPage>(context, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
}

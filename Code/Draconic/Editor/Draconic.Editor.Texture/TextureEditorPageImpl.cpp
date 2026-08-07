// Draconic::EditorTexture - the `draconic.editor.texture` module (implementation).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.texture;

import draconic.foundation;
import draconic.content;
import draconic.image;
import draconic.image.io;
import draconic.texture;
import draconic.texture.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace
    {
        // Sampler/shape option labels (index == the enum's underlying value, both 0-based).
        constexpr StringView kColorSpaceItems[] = {u8"sRGB", u8"Linear"};
        constexpr StringView kShapeItems[] = {u8"2D", u8"2D Array", u8"3D", u8"Cubemap",
                                              u8"Cubemap Array"};
        constexpr StringView kFilterItems[] = {u8"Nearest", u8"Linear", u8"Mipmap Nearest",
                                               u8"Mipmap Linear"};
        constexpr StringView kWrapItems[] = {u8"Repeat", u8"Clamp To Edge", u8"Clamp To Border",
                                             u8"Mirrored Repeat"};

        // Presets mirror TextureAsset's Sedulous-derived setups; index 0 is a no-op sentinel.
        constexpr StringView kPresetItems[] = {u8"(apply preset)",  u8"UI",
                                               u8"Sprite",          u8"3D",
                                               u8"Equirect Skybox", u8"Cubemap Skybox"};

        // Convert a decoded source image to an RGBA8 CPU buffer for the ImageView (the VG image
        // path is 8-bit; HDR sources are tonemapped by a plain clamp - a faithful preview needs
        // no exposure control here).
        UniquePtr<image::OwnedImageData> ToPreview(const image::Image& src)
        {
            const u32 w = src.Width();
            const u32 h = src.Height();
            if (w == 0 || h == 0)
            {
                return {};
            }
            if (src.Format() == image::PixelFormat::RGBA8)
            {
                return MakeUnique<image::OwnedImageData>(DefaultAllocator(), w, h,
                                                         image::PixelFormat::RGBA8, src.PixelData(),
                                                         src.ColorSpace());
            }
            if (src.Format() == image::PixelFormat::RGBA32F)
            {
                const Span<const u8> raw = src.PixelData();
                const usize texels = static_cast<usize>(w) * h * 4;
                if (raw.Size() < texels * sizeof(f32))
                {
                    return {};
                }
                const f32* in = reinterpret_cast<const f32*>(raw.Data());
                Array<u8> out;
                out.Resize(texels);
                for (usize i = 0; i < texels; ++i)
                {
                    const f32 c = Clamp(in[i], 0.0f, 1.0f);
                    out[i] = static_cast<u8>(c * 255.0f + 0.5f);
                }
                return MakeUnique<image::OwnedImageData>(DefaultAllocator(), w, h,
                                                         image::PixelFormat::RGBA8, Move(out),
                                                         image::ImageColorSpace::Srgb);
            }
            return {};
        }
    } // namespace

    TextureEditorPage::TextureEditorPage(EditorContext& context,
                                         draconic::content::Instance& instance)
        : m_context(&context), m_title(instance.Name())
    {
        SetInstanceId(instance.Id());

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<texture::TextureAsset>(Cast<texture::TextureAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"Editor", u8"texture '{}' failed to read - page opens empty",
                               m_title);
        }
        else
        {
            LoadPreview(instance);
        }

        // Left: source facts label + the preview image (fit-centered on a dark panel).
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

        // Right: the property grid, inset off the pane edge like the material/scene inspectors.
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
        split->SetSplitRatio(0.55f);
        split->SetPanes(previewColumn.Get(), gridColumn.Get());
        m_content = split;

        RefreshInfo();
    }

    void TextureEditorPage::LoadPreview(draconic::content::Instance& instance)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }

        // Embedded mode (model imports): the "pixels" stream is raw RGBA8 at embeddedWidth/Height.
        if (m_asset->fileName.IsEmpty() && m_asset->embeddedWidth > 0 &&
            m_asset->embeddedHeight > 0)
        {
            UniquePtr<IStream> stream = instance.ReadData(u8"pixels");
            if (!stream)
            {
                return;
            }
            const i64 size = stream->Size();
            const usize expected =
                static_cast<usize>(m_asset->embeddedWidth) * m_asset->embeddedHeight * 4;
            if (size <= 0 || static_cast<usize>(size) < expected)
            {
                return;
            }
            Array<u8> pixels;
            pixels.Resize(expected);
            if (stream->Read(pixels.Data(), expected) != expected)
            {
                return;
            }
            m_sourceFormat = image::PixelFormat::RGBA8;
            m_preview = MakeUnique<image::OwnedImageData>(
                DefaultAllocator(), m_asset->embeddedWidth, m_asset->embeddedHeight,
                image::PixelFormat::RGBA8, Move(pixels), m_asset->colorSpace);
            return;
        }

        // External file: decode the source (the +X face stands in for a cubemap preview).
        if (m_asset->fileName.IsEmpty() || m_context->Project() == nullptr)
        {
            return;
        }
        const String path =
            PathJoin(m_context->Project()->SourcesRoot().AsView(), m_asset->fileName.View());
        image::Image image;
        const Status loaded = image::io::LoadImage(path.AsView(), image);
        if (!loaded.IsOk())
        {
            DRACONIC_LOG_WARNING(u8"Editor", u8"texture source missing or undecodable: {}", path);
            return;
        }
        m_sourceFormat = image.Format();
        m_preview = ToPreview(image);
    }

    void TextureEditorPage::RefreshInfo()
    {
        if (m_asset.Get() == nullptr)
        {
            m_info->SetText(u8"Texture failed to load.");
            return;
        }
        if (m_preview.Get() == nullptr)
        {
            m_info->SetText(u8"No preview (source file missing, embedded, or undecodable).");
            return;
        }
        // The GPU format the cook resolves from the source format + the chosen color space.
        const bool srgb = m_asset->colorSpace == image::ImageColorSpace::Srgb;
        StringView derived;
        if (m_sourceFormat == image::PixelFormat::RGBA32F)
        {
            derived = u8"RGBA32F";
        }
        else
        {
            derived = srgb ? StringView(u8"RGBA8 sRGB") : StringView(u8"RGBA8 Linear");
        }
        String text = Format(u8"{} x {}  |  source {}  |  cooks to {}", m_preview->Width(),
                             m_preview->Height(),
                             m_sourceFormat == image::PixelFormat::RGBA32F ? StringView(u8"HDR")
                                                                           : StringView(u8"LDR"),
                             derived);
        m_info->SetText(text.AsView());
    }

    void TextureEditorPage::BuildGrid()
    {
        m_grid->Clear();
        m_refreshers.Clear();
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        TextureEditorPage* self = this;

        // --- Presets (uncategorized, at the top): apply a Sedulous setup as one undo entry ---
        auto preset = MakeRef<ui::toolkit::EnumEditor>(
            DefaultAllocator(), StringView(u8"Preset"), 0, Span<const StringView>{kPresetItems, 6},
            Function<void(i32)>{[self](i32 index)
                                {
                                    if (index <= 0)
                                    {
                                        return;
                                    }
                                    self->ApplyEdit(u8"preset",
                                                    Function<void(texture::TextureAsset&)>{
                                                        [index](texture::TextureAsset& a)
                                                        {
                                                            switch (index)
                                                            {
                                                            case 1:
                                                                a.SetupForUI();
                                                                break;
                                                            case 2:
                                                                a.SetupForSprite();
                                                                break;
                                                            case 3:
                                                                a.SetupFor3D();
                                                                break;
                                                            case 4:
                                                                a.SetupForEquirectangularSkybox();
                                                                break;
                                                            case 5:
                                                                a.SetupForCubemapSkybox();
                                                                break;
                                                            default:
                                                                break;
                                                            }
                                                        }});
                                }});
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(preset.Get()));

        // --- Texture: color space + shape ---
        {
            auto editor = MakeRef<ui::toolkit::EnumEditor>(
                DefaultAllocator(), StringView(u8"Color Space"),
                static_cast<i32>(m_asset->colorSpace), Span<const StringView>{kColorSpaceItems, 2},
                Function<void(i32)>{
                    [self](i32 v)
                    {
                        self->ApplyEdit(
                            u8"colorSpace",
                            Function<void(texture::TextureAsset&)>{
                                [v](texture::TextureAsset& a)
                                { a.colorSpace = static_cast<image::ImageColorSpace>(v); }});
                    }},
                StringView(u8"Texture"));
            AddEditor(editor.Get(), [self, raw = editor.Get()]()
                      { raw->SetValue(static_cast<i32>(self->m_asset->colorSpace)); });
        }
        {
            auto editor = MakeRef<ui::toolkit::EnumEditor>(
                DefaultAllocator(), StringView(u8"Shape"), static_cast<i32>(m_asset->shape),
                Span<const StringView>{kShapeItems, 5},
                Function<void(i32)>{
                    [self](i32 v)
                    {
                        self->ApplyEdit(u8"shape",
                                        Function<void(texture::TextureAsset&)>{
                                            [v](texture::TextureAsset& a)
                                            { a.shape = static_cast<texture::TextureShape>(v); }});
                    }},
                StringView(u8"Texture"));
            AddEditor(editor.Get(), [self, raw = editor.Get()]()
                      { raw->SetValue(static_cast<i32>(self->m_asset->shape)); });
        }

        // --- Sampling: filters, wraps, mipmaps, anisotropy ---
        auto addFilterRow = [self](StringView label, StringView key,
                                   texture::TextureFilter texture::TextureAsset::* field)
        {
            auto editor = MakeRef<ui::toolkit::EnumEditor>(
                DefaultAllocator(), label, static_cast<i32>(self->m_asset.Get()->*field),
                Span<const StringView>{kFilterItems, 4},
                Function<void(i32)>{
                    [self, field, key = String(key)](i32 v)
                    {
                        self->ApplyEdit(
                            key.AsView(),
                            Function<void(texture::TextureAsset&)>{
                                [field, v](texture::TextureAsset& a)
                                { a.*field = static_cast<texture::TextureFilter>(v); }});
                    }},
                StringView(u8"Sampling"));
            self->AddEditor(editor.Get(), [self, raw = editor.Get(), field]()
                            { raw->SetValue(static_cast<i32>(self->m_asset.Get()->*field)); });
        };
        addFilterRow(u8"Min Filter", u8"minFilter", &texture::TextureAsset::minFilter);
        addFilterRow(u8"Mag Filter", u8"magFilter", &texture::TextureAsset::magFilter);

        auto addWrapRow = [self](StringView label, StringView key,
                                 texture::TextureWrap texture::TextureAsset::* field)
        {
            auto editor = MakeRef<ui::toolkit::EnumEditor>(
                DefaultAllocator(), label, static_cast<i32>(self->m_asset.Get()->*field),
                Span<const StringView>{kWrapItems, 4},
                Function<void(i32)>{
                    [self, field, key = String(key)](i32 v)
                    {
                        self->ApplyEdit(key.AsView(),
                                        Function<void(texture::TextureAsset&)>{
                                            [field, v](texture::TextureAsset& a)
                                            { a.*field = static_cast<texture::TextureWrap>(v); }});
                    }},
                StringView(u8"Sampling"));
            self->AddEditor(editor.Get(), [self, raw = editor.Get(), field]()
                            { raw->SetValue(static_cast<i32>(self->m_asset.Get()->*field)); });
        };
        addWrapRow(u8"Wrap U", u8"wrapU", &texture::TextureAsset::wrapU);
        addWrapRow(u8"Wrap V", u8"wrapV", &texture::TextureAsset::wrapV);
        addWrapRow(u8"Wrap W", u8"wrapW", &texture::TextureAsset::wrapW);

        {
            auto editor = MakeRef<ui::toolkit::BoolEditor>(
                DefaultAllocator(), StringView(u8"Generate Mipmaps"), m_asset->generateMipmaps,
                Function<void(bool)>{[self](bool v)
                                     {
                                         self->ApplyEdit(u8"generateMipmaps",
                                                         Function<void(texture::TextureAsset&)>{
                                                             [v](texture::TextureAsset& a)
                                                             { a.generateMipmaps = v; }});
                                     }},
                StringView(u8"Sampling"));
            AddEditor(editor.Get(), [self, raw = editor.Get()]()
                      { raw->SetValue(self->m_asset->generateMipmaps); });
        }
        {
            auto editor = MakeRef<ui::toolkit::RangeEditor>(
                DefaultAllocator(), StringView(u8"Anisotropy"), m_asset->anisotropy, 1.0f, 16.0f,
                1.0f,
                Function<void(f32)>{[self](f32 v)
                                    {
                                        self->ApplyEdit(u8"anisotropy",
                                                        Function<void(texture::TextureAsset&)>{
                                                            [v](texture::TextureAsset& a)
                                                            { a.anisotropy = v; }});
                                    }},
                StringView(u8"Sampling"));
            AddEditor(editor.Get(),
                      [self, raw = editor.Get()]() { raw->SetValue(self->m_asset->anisotropy); });
        }
    }

    void TextureEditorPage::AddEditor(ui::toolkit::PropertyEditor* editor,
                                      Function<void()> refresher)
    {
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(editor));
        ui::toolkit::PropertyEditor* raw = editor;
        m_refreshers.PushBack(Function<void()>{[raw, pull = Move(refresher)]()
                                               {
                                                   if (!raw->IsEditing())
                                                   {
                                                       pull();
                                                   }
                                               }});
    }

    Array<byte> TextureEditorPage::Snapshot() const
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

    void TextureEditorPage::ApplyBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        m_asset->Serialize(ar);
        // Refresh in place (no grid rebuild - that would destroy editor views mid-event).
        for (const Function<void()>& refresher : m_refreshers)
        {
            refresher();
        }
        RefreshInfo();
    }

    void TextureEditorPage::ApplyEdit(StringView mergeKey,
                                      Function<void(texture::TextureAsset&)> mutate)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        Array<byte> before = Snapshot();
        mutate(*m_asset);
        Array<byte> after = Snapshot();
        // The mutation already ran; Execute() re-applies `after` (idempotent).
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<EditTextureCommand>(*this, mergeKey, Move(before), Move(after)),
            DefaultAllocator()));
    }

    Status TextureEditorPage::Save()
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
            // Refresh the cooked product so every bound proxy hot-swaps to the new settings.
            m_context->RequestCook(false);
            DRACONIC_LOG_INFO(u8"Editor", u8"saved texture '{}'", m_title);
        }
        return saved;
    }

    const TypeInfo* TextureEditorPageFactory::PrimaryType() const
    {
        return &texture::TextureAsset::StaticType();
    }

    UniquePtr<EditorPage>
    TextureEditorPageFactory::CreatePage(EditorContext& context,
                                         draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<TextureEditorPage>(context, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
}

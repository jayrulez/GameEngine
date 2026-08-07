// Draconic::EditorFonts - the `draconic.editor.fonts` module (implementation).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.fonts;

import draconic.foundation;
import draconic.content;
import draconic.vfs;
import draconic.image;
import draconic.fonts;
import draconic.fonts.ttf;
import draconic.fonts.io;
import draconic.fonts.importer;
import draconic.fonts.distancefield.baker;
import draconic.fonts.editor;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace
    {
        constexpr StringView kModeItems[] = {u8"Raster Ramp", u8"Distance Field (MSDF)"};

        // "10, 12.5 24" -> floats; returns false (and leaves `out` untouched) on any junk.
        [[nodiscard]] bool ParseSizes(StringView text, Array<f32>& out)
        {
            Array<f32> parsed;
            f64 value = 0.0;
            f64 fraction = 0.0;
            bool inNumber = false;
            bool inFraction = false;
            for (usize i = 0; i <= text.Size(); ++i)
            {
                const utf8char c = i < text.Size() ? text[i] : utf8char(',');
                if (c >= u8'0' && c <= u8'9')
                {
                    if (inFraction)
                    {
                        fraction *= 0.1;
                        value += (c - u8'0') * fraction;
                    }
                    else
                    {
                        value = value * 10.0 + (c - u8'0');
                    }
                    inNumber = true;
                }
                else if (c == u8'.' && inNumber && !inFraction)
                {
                    inFraction = true;
                    fraction = 1.0;
                }
                else if (c == u8',' || c == u8' ' || c == u8'\t' || c == u8';')
                {
                    if (inNumber)
                    {
                        if (value <= 0.0 || value > 512.0)
                        {
                            return false;
                        }
                        parsed.PushBack(static_cast<f32>(value));
                        value = 0.0;
                        inNumber = false;
                        inFraction = false;
                    }
                }
                else
                {
                    return false; // junk character
                }
            }
            if (parsed.IsEmpty())
            {
                return false;
            }
            out = Move(parsed);
            return true;
        }

        [[nodiscard]] String FormatSizes(const Array<f32>& sizes)
        {
            String text;
            for (usize i = 0; i < sizes.Size(); ++i)
            {
                if (i != 0)
                {
                    text += u8", ";
                }
                const f32 s = sizes[i];
                const i32 whole = static_cast<i32>(s);
                const i32 tenth = static_cast<i32>((s - static_cast<f32>(whole)) * 10.0f + 0.5f);
                text += (tenth != 0) ? Format(u8"{}.{}", whole, tenth) : Format(u8"{}", whole);
            }
            return text;
        }
    } // namespace

    FontEditorPage::FontEditorPage(EditorContext& context, draconic::content::Instance& instance)
        : m_context(&context), m_title(instance.Name())
    {
        SetInstanceId(instance.Id());

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<fonts::FontAsset>(Cast<fonts::FontAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"Editor", u8"font '{}' failed to read - page opens empty",
                               m_title);
        }
        m_undoBaseline = Snapshot();

        m_image = MakeRef<ui::ImageView>(DefaultAllocator());
        m_image->ScaleType.SetValue(ui::ScaleType::FitCenter);

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

        RebakePreview();
    }

    FontEditorPage::~FontEditorPage()
    {
        if (m_activeSlot != nullptr)
        {
            m_activeSlot->pageAlive = false; // completion closure still owns + deletes it
            m_activeSlot = nullptr;
        }
    }

    FontEditorPage::BakeRequest FontEditorPage::CaptureBakeRequest()
    {
        BakeRequest request;
        if (m_asset.Get() == nullptr || m_asset->fileName.IsEmpty() ||
            m_context->Project() == nullptr)
        {
            return request;
        }
        request.valid = true;
        request.path =
            PathJoin(m_context->Project()->SourcesRoot().AsView(), m_asset->fileName.View());
        request.distanceField = m_asset->mode == fonts::FontBakeMode::DistanceField;
        // Coverage previews at the ramp's LARGEST size (the most informative atlas).
        request.size = m_asset->dfSize;
        if (!request.distanceField)
        {
            request.size = 14.0f;
            for (f32 s : m_asset->sizes)
            {
                request.size = Max(request.size, s);
            }
        }
        request.firstCodepoint = m_asset->firstCodepoint;
        request.lastCodepoint = m_asset->lastCodepoint;
        request.atlasWidth = m_asset->atlasWidth;
        request.atlasHeight = m_asset->atlasHeight;
        if (request.distanceField)
        {
            fonts::DFFonts::Initialize(); // idempotent; registration happens on the UI thread
        }
        return request;
    }

    // Worker-side (or synchronous fallback): pure CPU, touches ONLY the request + outcome.
    void FontEditorPage::RunBake(const BakeRequest& request, BakeOutcome& outcome)
    {
        outcome.generation = request.generation;
        outcome.size = request.size;
        if (!request.valid)
        {
            return;
        }
        Result<Array<byte>> bytes = ReadFile(request.path.AsView());
        if (!bytes.HasValue())
        {
            return;
        }
        const Span<const u8> fontBytes(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                       bytes.Value().Size());

        fonts::FontLoadOptions options = request.distanceField
                                             ? fonts::FontLoadOptions::DistanceField()
                                             : fonts::FontLoadOptions::Default();
        options.pixelHeight = request.size;
        options.firstCodepoint = request.firstCodepoint;
        options.lastCodepoint = request.lastCodepoint;
        options.atlasWidth = request.atlasWidth;
        options.atlasHeight = request.atlasHeight;

        if (request.distanceField)
        {
            Array<u8> copy;
            copy.Resize(fontBytes.Size());
            if (fontBytes.Size() != 0)
            {
                MemCopy(copy.Data(), fontBytes.Data(), fontBytes.Size());
            }
            fonts::TrueTypeFont font;
            if (font.Initialize(Move(copy), options.pixelHeight) != fonts::FontLoadResult::Success)
            {
                return;
            }
            Result<fonts::IFontAtlas*, fonts::FontLoadResult> baked =
                fonts::FontAtlasBakerFactory::Bake(font, options);
            if (!baked.HasValue())
            {
                return;
            }
            UniquePtr<fonts::IFontAtlas> atlas(baked.Value(), DefaultAllocator());
            for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
            {
                if (atlas->Contains(cp))
                {
                    ++outcome.glyphs;
                }
            }
            // Decode the field for the preview: median(r,g,b) is the signed distance
            // (0.5 = the glyph edge); a narrow ramp around it approximates the DF
            // shader's screen-space anti-aliasing. Showing the RAW channels here reads
            // as rainbow noise.
            const Span<const u8> field = atlas->PixelData();
            Array<u8> decoded(field.Size());
            for (usize px = 0; px + 3 < field.Size(); px += 4)
            {
                const u8 r = field[px + 0];
                const u8 g = field[px + 1];
                const u8 b = field[px + 2];
                const u8 med = Max(Min(r, g), Min(Max(r, g), b));
                const i32 alpha = Clamp((static_cast<i32>(med) - 112) * 8, 0, 255);
                decoded[px + 0] = 255;
                decoded[px + 1] = 255;
                decoded[px + 2] = 255;
                decoded[px + 3] = static_cast<u8>(alpha);
            }
            outcome.image = MakeUnique<image::OwnedImageData>(
                DefaultAllocator(), atlas->Width(), atlas->Height(), image::PixelFormat::RGBA8,
                Move(decoded), image::ImageColorSpace::Linear);
        }
        else
        {
            Result<fonts::BakedFontData*, fonts::FontLoadResult> baked =
                fonts::FontImporter::Bake(fontBytes, options);
            if (!baked.HasValue())
            {
                return;
            }
            UniquePtr<fonts::BakedFontData> data(baked.Value(), DefaultAllocator());
            outcome.glyphs = data->atlas->Regions().Size();
            outcome.image = UniquePtr<image::OwnedImageData>(
                fonts::FontAtlasTexture::ExpandR8ToRGBA8(data->atlas), DefaultAllocator());
        }
    }

    void FontEditorPage::RebakePreview()
    {
        BakeRequest request = CaptureBakeRequest();
        request.generation = ++m_bakeGeneration;

        EditorJobService* jobs = m_context->Jobs();
        if (jobs == nullptr)
        {
            // Headless/tests: no job pump, bake in place.
            BakeOutcome outcome;
            RunBake(request, outcome);
            ApplyBakeOutcome(Move(outcome));
            return;
        }
        if (m_bakeBusy)
        {
            m_pendingRequest = Move(request); // latest-wins; submitted when the flight lands
            m_pendingValid = true;
            return;
        }
        StartBake(Move(request));
    }

    void FontEditorPage::StartBake(BakeRequest request)
    {
        EditorJobService* jobs = m_context->Jobs();
        auto* slot = DefaultAllocator().New<BakeSlot>();
        m_activeSlot = slot;
        m_bakeBusy = true;
        if (m_info.Get() != nullptr)
        {
            m_info->SetText(u8"Baking preview...");
        }

        FontEditorPage* self = this;
        jobs->SubmitLight(
            Function<void()>{[slot, request = Move(request)]()
                             { RunBake(request, slot->outcome); }},
            Function<void()>{
                [self, slot]()
                {
                    // Main thread. The page may have closed mid-flight; the slot's flag is
                    // the guard (set/read on the main thread only).
                    if (slot->pageAlive)
                    {
                        self->m_activeSlot = nullptr;
                        self->m_bakeBusy = false;
                        self->ApplyBakeOutcome(Move(slot->outcome));
                        if (self->m_pendingValid)
                        {
                            self->m_pendingValid = false;
                            self->StartBake(Move(self->m_pendingRequest));
                        }
                    }
                    DefaultAllocator().Delete(slot);
                }});
    }

    void FontEditorPage::ApplyBakeOutcome(BakeOutcome outcome)
    {
        // A stale outcome (older than the newest request) never reaches the screen; the
        // pending resubmit is already on its way.
        if (outcome.generation != m_bakeGeneration)
        {
            return;
        }
        m_preview = Move(outcome.image);
        m_previewGlyphs = outcome.glyphs;
        m_previewSize = outcome.size;

        m_image->SetImage(m_preview.Get());
        if (m_asset.Get() == nullptr)
        {
            m_info->SetText(u8"Font failed to load.");
        }
        else if (m_preview.Get() == nullptr)
        {
            m_info->SetText(u8"No preview (source file missing, unparseable, or the atlas "
                            u8"is too small for the range).");
        }
        else
        {
            const i32 sizeWhole = static_cast<i32>(m_previewSize);
            m_info->SetText(
                Format(u8"{}  |  {} glyphs @ {}px  |  atlas {} x {}", m_asset->fileName.View(),
                       static_cast<u64>(m_previewGlyphs), sizeWhole, m_preview->Width(),
                       m_preview->Height())
                    .AsView());
        }
    }

    void FontEditorPage::CommitEdit(StringView mergeKey)
    {
        Array<byte> after = Snapshot();
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<EditFontCommand>(*this, mergeKey, m_undoBaseline, after),
            DefaultAllocator()));
        m_undoBaseline = Move(after);
        MarkDirty();
        RebakePreview();
        if (m_asset->mode != m_gridMode)
        {
            QueueGridRebuild();
        }
    }

    void FontEditorPage::QueueGridRebuild()
    {
        FontEditorPage* self = this;
        ui::UIContext* ctx = (m_grid.Get() != nullptr) ? m_grid->Context : nullptr;
        if (ctx != nullptr)
        {
            ctx->MutationQueueRef().QueueAction(Function<void()>{[self]() { self->BuildGrid(); }});
        }
        else
        {
            BuildGrid();
        }
    }

    void FontEditorPage::BuildGrid()
    {
        m_grid->Clear();
        m_familyRow = nullptr;
        m_modeRow = nullptr;
        m_sizesRow = nullptr;
        m_dfSizeRow = nullptr;
        m_firstRow = nullptr;
        m_lastRow = nullptr;
        m_atlasWidthRow = nullptr;
        m_atlasHeightRow = nullptr;
        m_fileRow = nullptr;
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        m_gridMode = m_asset->mode;
        FontEditorPage* self = this;

        auto family = MakeRef<ui::toolkit::StringEditor>(
            DefaultAllocator(), u8"Family", m_asset->family.AsView(),
            Function<void(StringView)>{[self](StringView v)
                                       {
                                           self->m_asset->family = String(v);
                                           self->CommitEdit(u8"family");
                                       }},
            u8"Font");
        m_familyRow = family.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(family.Get()));

        auto mode = MakeRef<ui::toolkit::EnumEditor>(
            DefaultAllocator(), u8"Bake Mode", static_cast<i32>(m_asset->mode),
            Span<const StringView>{kModeItems, 2},
            Function<void(i32)>{[self](i32 v)
                                {
                                    self->m_asset->mode = static_cast<fonts::FontBakeMode>(v);
                                    self->CommitEdit(u8"mode");
                                }},
            u8"Font");
        m_modeRow = mode.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(mode.Get()));

        if (m_asset->mode == fonts::FontBakeMode::RasterRamp)
        {
        auto sizes = MakeRef<ui::toolkit::StringEditor>(
            DefaultAllocator(), u8"Sizes (px)", FormatSizes(m_asset->sizes).AsView(),
            Function<void(StringView)>{[self](StringView v)
                                       {
                                           Array<f32> parsed;
                                           if (!ParseSizes(v, parsed))
                                           {
                                               // junk input: re-pull the last good value
                                               self->RefreshRows();
                                               return;
                                           }
                                           self->m_asset->sizes = Move(parsed);
                                           self->CommitEdit(u8"sizes");
                                       }},
            u8"Raster Ramp");
        m_sizesRow = sizes.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(sizes.Get()));
        }

        if (m_asset->mode == fonts::FontBakeMode::DistanceField)
        {
        auto dfSize = MakeRef<ui::toolkit::FloatEditor>(
            DefaultAllocator(), u8"MSDF Size (px)", static_cast<f64>(m_asset->dfSize), 8.0, 128.0,
            1.0, 0,
            Function<void(f64)>{[self](f64 v)
                                {
                                    self->m_asset->dfSize = static_cast<f32>(v);
                                    self->CommitEdit(u8"df-size");
                                }},
            u8"Distance Field");
        m_dfSizeRow = dfSize.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(dfSize.Get()));
        }

        auto first = MakeRef<ui::toolkit::IntEditor>(
            DefaultAllocator(), u8"First Codepoint", static_cast<i64>(m_asset->firstCodepoint), 0,
            0x10FFFF,
            Function<void(i64)>{[self](i64 v)
                                {
                                    self->m_asset->firstCodepoint = static_cast<i32>(v);
                                    self->CommitEdit(u8"first-cp");
                                }},
            u8"Glyph Range");
        m_firstRow = first.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(first.Get()));

        auto last = MakeRef<ui::toolkit::IntEditor>(
            DefaultAllocator(), u8"Last Codepoint", static_cast<i64>(m_asset->lastCodepoint), 0,
            0x10FFFF,
            Function<void(i64)>{[self](i64 v)
                                {
                                    self->m_asset->lastCodepoint = static_cast<i32>(v);
                                    self->CommitEdit(u8"last-cp");
                                }},
            u8"Glyph Range");
        m_lastRow = last.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(last.Get()));

        auto atlasWidth = MakeRef<ui::toolkit::IntEditor>(
            DefaultAllocator(), u8"Atlas Width", static_cast<i64>(m_asset->atlasWidth), 64, 8192,
            Function<void(i64)>{[self](i64 v)
                                {
                                    self->m_asset->atlasWidth = static_cast<u32>(v);
                                    self->CommitEdit(u8"atlas-w");
                                }},
            u8"Atlas");
        m_atlasWidthRow = atlasWidth.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(atlasWidth.Get()));

        auto atlasHeight = MakeRef<ui::toolkit::IntEditor>(
            DefaultAllocator(), u8"Atlas Height", static_cast<i64>(m_asset->atlasHeight), 64, 8192,
            Function<void(i64)>{[self](i64 v)
                                {
                                    self->m_asset->atlasHeight = static_cast<u32>(v);
                                    self->CommitEdit(u8"atlas-h");
                                }},
            u8"Atlas");
        m_atlasHeightRow = atlasHeight.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(atlasHeight.Get()));

        // Source file: read-only path display + a project-constrained picker. Free-typing
        // is deliberately not offered - a typo would cook a dangling reference.
        auto file = MakeRef<ui::toolkit::StringEditor>(DefaultAllocator(), u8"File",
                                                       m_asset->fileName.View(),
                                                       Function<void(StringView)>{}, u8"Source");
        m_fileRow = file.Get();
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(file.Get()));
        m_grid->AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
            MakeRef<ui::toolkit::ButtonEditor>(
                DefaultAllocator(), u8"Browse...",
                Function<void()>{[self]()
                                 {
                                     ui::UIContext* ctx = (self->m_grid.Get() != nullptr)
                                                              ? self->m_grid->Context
                                                              : nullptr;
                                     if (ctx == nullptr || self->m_context->Project() == nullptr)
                                     {
                                         return;
                                     }
                                     Array<String> extensions;
                                     extensions.PushBack(String(u8".ttf"));
                                     extensions.PushBack(String(u8".otf"));
                                     extensions.PushBack(String(u8".ttc"));
                                     auto dialog = MakeRef<app::PathPickerDialog>(
                                         DefaultAllocator(), u8"Select font file",
                                         self->m_context->Project()->SourcesRoot().AsView(),
                                         Move(extensions));
                                     dialog->OnPicked = [self](StringView picked)
                                     {
                                         self->m_asset->fileName = draconic::vfs::SourcePath(picked);
                                         self->CommitEdit(u8"file");
                                         self->RefreshRows();
                                     };
                                     dialog->Show(ctx);
                                 }},
                u8"Source")
                .Get()));
    }

    Array<byte> FontEditorPage::Snapshot() const
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

    void FontEditorPage::ApplyBlob(const Array<byte>& blob)
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
        m_asset->sizes.Clear(); // ISerializable array: restore clears in place
        m_asset->Serialize(ar);
        m_undoBaseline = blob;
        RefreshRows();
        MarkDirty();
        RebakePreview();
        if (m_asset->mode != m_gridMode)
        {
            QueueGridRebuild();
        }
    }

    void FontEditorPage::RefreshRows()
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        if (m_familyRow != nullptr)
        {
            m_familyRow->SetValue(m_asset->family.AsView());
        }
        if (m_modeRow != nullptr)
        {
            m_modeRow->SetValue(static_cast<i32>(m_asset->mode));
        }
        if (m_sizesRow != nullptr)
        {
            m_sizesRow->SetValue(FormatSizes(m_asset->sizes).AsView());
        }
        if (m_dfSizeRow != nullptr)
        {
            m_dfSizeRow->SetValue(static_cast<f64>(m_asset->dfSize));
        }
        if (m_firstRow != nullptr)
        {
            m_firstRow->SetValue(static_cast<i64>(m_asset->firstCodepoint));
        }
        if (m_lastRow != nullptr)
        {
            m_lastRow->SetValue(static_cast<i64>(m_asset->lastCodepoint));
        }
        if (m_atlasWidthRow != nullptr)
        {
            m_atlasWidthRow->SetValue(static_cast<i64>(m_asset->atlasWidth));
        }
        if (m_atlasHeightRow != nullptr)
        {
            m_atlasHeightRow->SetValue(static_cast<i64>(m_asset->atlasHeight));
        }
        if (m_fileRow != nullptr)
        {
            m_fileRow->SetValue(m_asset->fileName.View());
        }
    }

    Status FontEditorPage::Save()
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
            DRACONIC_LOG_INFO(u8"Editor", u8"saved font '{}'", m_title);
        }
        return saved;
    }

    const TypeInfo* FontEditorPageFactory::PrimaryType() const
    {
        return &fonts::FontAsset::StaticType();
    }

    UniquePtr<EditorPage> FontEditorPageFactory::CreatePage(EditorContext& context,
                                                            draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<FontEditorPage>(context, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
}

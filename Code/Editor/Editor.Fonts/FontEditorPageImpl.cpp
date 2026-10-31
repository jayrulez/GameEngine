// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Fonts - the `editor.fonts` module (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.fonts;

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.image;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.coverage.baker;
import foundation.fonts.distancefield.baker;
import fonts.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace fonts = foundation::fonts;
namespace image = foundation::image;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        constexpr StringView kSizesPath = u8"sizes";

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

    FontEditorPage::FontEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::FontAsset::StaticType(), kPath)
    {
        if (Content())
            m_preview = MakeUnique<app::ImagePreview>(Allocator(), *Content());
        m_ramp = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        if (const pipeline::FontAsset* asset = ObjectAs<pipeline::FontAsset>())
        {
            gui::PropertyInfo& sizes = m_ramp->Add(kSizesPath, gui::PropertyKind::String, gui::Variant(FormatSizes(asset->sizes)));
            sizes.DisplayName = String(u8"Sizes");
            sizes.Description = String(u8"The pixel sizes a raster ramp bakes, one atlas each: \"12, 16, 24\"");
            FontEditorPage* self = this;
            m_ramp->SetOnCommit([self](const gui::PropertyBag::Commit& commit)
                                {
                                    if (!commit.After.IsEmpty())
                                        (void)self->SetSizes(commit.After[0].AsString());
                                });
            AddSection(u8"ramp", u8"Raster Ramp", *m_ramp);
            ShowRamp();
        }
        RebakePreview();
    }

    FontEditorPage::~FontEditorPage()
    {
        if (m_activeSlot != nullptr)
        {
            m_activeSlot->pageAlive = false; // the completion closure still owns and deletes it
            m_activeSlot = nullptr;
        }
    }

    bool FontEditorPage::SetSizes(StringView text)
    {
        Array<f32> sizes;
        if (!ParseSizes(text, sizes))
        {
            SetNotice(u8"sizes", u8"Sizes are numbers from 1 to 512, apart by commas or spaces: the row keeps what it had.");
            ShowRamp();
            return false;
        }
        SetNotice(u8"sizes", StringView{});
        EditWhole(u8"font_sizes", [&sizes](ISerializable& object) { static_cast<pipeline::FontAsset&>(object).sizes = sizes; });
        return true;
    }

    void FontEditorPage::ShowRamp()
    {
        const pipeline::FontAsset* asset = ObjectAs<pipeline::FontAsset>();
        if (asset == nullptr)
            return;
        // A distance field bakes one size; only a raster font has a ramp.
        const bool raster = asset->mode == pipeline::FontBakeMode::RasterRamp;
        if (gui::PropertyInfo* info = m_ramp->FindInfo(kSizesPath); info != nullptr && info->Visible != raster)
        {
            info->Visible = raster;
            m_ramp->PropertiesChanged();
        }
        const String text = FormatSizes(asset->sizes);
        if (m_ramp->GetValue(kSizesPath, 0).AsString() != text.AsView())
            m_ramp->SetValue(kSizesPath, 0, gui::Variant(text.AsView()));
    }

    void FontEditorPage::OnObjectChanged()
    {
        ShowRamp();
        RebakePreview();
    }

    FontEditorPage::BakeRequest FontEditorPage::CaptureBakeRequest()
    {
        BakeRequest request;
        const pipeline::FontAsset* asset = ObjectAs<pipeline::FontAsset>();
        if (asset == nullptr || asset->fileName.IsEmpty() || Context().Project() == nullptr)
        {
            return request;
        }
        request.valid = true;
        request.path =
            PathJoin(Context().Project()->SourcesRoot().AsView(), asset->fileName.View());
        request.distanceField = asset->mode == pipeline::FontBakeMode::DistanceField;
        // Coverage previews at the ramp's largest size (the most informative atlas).
        request.size = asset->distanceFieldSize;
        request.distanceFieldRange = asset->distanceFieldRange;
        if (!request.distanceField)
        {
            request.size = 14.0f;
            for (f32 s : asset->sizes)
            {
                request.size = Max(request.size, s);
            }
        }
        request.firstCodepoint = asset->firstCodepoint;
        request.lastCodepoint = asset->lastCodepoint;
        request.atlasWidth = asset->atlasWidth;
        request.atlasHeight = asset->atlasHeight;
        if (request.distanceField)
        {
            fonts::DistanceFieldFonts::Initialize(); // idempotent; registration happens on the UI thread
        }
        return request;
    }

    // Worker side (or the synchronous fallback): pure CPU, touching only the request and the
    // outcome.
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
            options.distanceFieldRange = request.distanceFieldRange;

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
                fonts::FontAtlasBakerFactory::Bake(font, options, editor::EditorRootAllocator());
            if (!baked.HasValue())
            {
                return;
            }
            UniquePtr<fonts::IFontAtlas> atlas(baked.Value(), editor::EditorRootAllocator());
            for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
            {
                if (atlas->Contains(atlas->GlyphFor(cp)))
                {
                    ++outcome.glyphs;
                }
            }
            // Decode the field for the preview: median(r,g,b) is the signed distance
            // (0.5 = the glyph edge); a narrow ramp around it approximates the DF
            // shader's screen-space anti-aliasing. Showing the raw channels here reads
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
            outcome.image = image::Image(atlas->Width(), atlas->Height(), image::PixelFormat::RGBA8,
                                         Span<const u8>(decoded.Data(), decoded.Size()));
        }
        else
        {
            Result<fonts::BakedFontData*, fonts::FontLoadResult> baked =
                fonts::FontBaker::Bake(fontBytes, options, editor::EditorRootAllocator());
            if (!baked.HasValue())
            {
                return;
            }
            UniquePtr<fonts::BakedFontData> data(baked.Value(), editor::EditorRootAllocator());
            outcome.glyphs = data->atlas->Regions().Size();
            UniquePtr<image::OwnedImageData> expanded(fonts::FontAtlasTexture::ExpandR8ToRGBA8(data->atlas, editor::EditorRootAllocator()),
                                                      editor::EditorRootAllocator());
            if (expanded)
                outcome.image = image::Image(expanded->Width(), expanded->Height(), image::PixelFormat::RGBA8, expanded->PixelData());
        }
    }

    void FontEditorPage::RebakePreview()
    {
        BakeRequest request = CaptureBakeRequest();
        request.generation = ++m_bakeGeneration;

        EditorJobService* jobs = Context().Jobs();
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
            m_pendingRequest = Move(request); // the latest wins; submitted when the bake in flight completes
            m_pendingValid = true;
            return;
        }
        StartBake(Move(request));
    }

    void FontEditorPage::StartBake(BakeRequest request)
    {
        EditorJobService* jobs = Context().Jobs();
        auto* slot = Allocator().New<BakeSlot>();
        m_activeSlot = slot;
        m_bakeBusy = true;
        SetFact(u8"Atlas", u8"Baking...");

        FontEditorPage* self = this;
        jobs->SubmitLight(
            Function<void()>{[slot, bakeRequest = Move(request)]()
                             { RunBake(bakeRequest, slot->outcome); }},
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
                    self->Allocator().Delete(slot);
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
        m_previewGlyphs = outcome.glyphs;
        m_previewSize = outcome.size;
        if (outcome.image.Width() == 0)
        {
            SetFact(u8"Atlas", u8"Not baked");
            if (m_preview)
                m_preview->ShowNone(u8"No atlas: the source file is missing or unreadable, or the atlas is too small for the range.");
            return;
        }
        SetFact(u8"Glyphs", Format(u8"{} at {} px", static_cast<u64>(m_previewGlyphs), static_cast<i32>(m_previewSize)).AsView());
        SetFact(u8"Atlas", Format(u8"{} x {}", outcome.image.Width(), outcome.image.Height()).AsView());
        if (m_preview)
            m_preview->Show(Move(outcome.image));
    }

    const TypeInfo* FontEditorPageFactory::PrimaryType() const
    {
        return &pipeline::FontAsset::StaticType();
    }

    UniquePtr<EditorPage> FontEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<FontEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    Span<const String> FontEditorPage::SourceExtensions() const
    {
        static const String kFonts[] = {String(u8".ttf"), String(u8".otf"), String(u8".ttc")};
        return Span<const String>(kFonts, 3u);
    }
}

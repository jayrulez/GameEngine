// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::GUI - implementation unit: the resource provider's bindings, the subsystem's lifecycle,
// its overlays through GuiPassRenderer, and the domain.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h"

module engine.gui;

import foundation.core;
import engine.domain;
import foundation.profiler;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.resource;
import foundation.content;
import foundation.rhi;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.resource;  // the fonts' resource module (the domain)
import foundation.fonts.harfbuzz;  // the shaper the GUI's fonts get
import foundation.fonts.bidi;      // mixed-direction lines in visual order
import foundation.image;
import foundation.vg;
import foundation.vg.svg;
import foundation.vg.resource;      // VectorImage: an SVG by guid
import foundation.texture.resource; // Texture: an image by guid
import foundation.render.api;
import engine.render; // RenderSubsystem: the overlays' registration
import foundation.shell;
import foundation.input;
import engine.input;
import experimental.gui;
import experimental.gui.gamekit;
import experimental.gui.shell;
import experimental.gui.resources; // GuiDocument, GuiStyleSheet, kGuiResourceModule
import experimental.gui.hosting;   // GuiPassRenderer, VgShaderLibrary

using namespace foundation::core;

namespace engine::gui
{
    namespace
    {
        // Whether `instance` was cooked as `cooked`: a guid is bound only as what it is, since a
        // handle bound as another product caches its failure.
        [[nodiscard]] bool IsCookedAs(const foundation::content::Instance& instance, const TypeInfo& cooked) noexcept
        {
            return instance.TypeName() == StringView(reinterpret_cast<const utf8char*>(cooked.name)) &&
                   instance.TypeNamespace() == StringView(reinterpret_cast<const utf8char*>(cooked.namespaceName));
        }
    }

    // ===================================================================================
    // GuiResourceProvider
    // ===================================================================================

    struct GuiResourceProvider::State
    {
        explicit State(IAllocator& allocator) noexcept : Allocator(&allocator), Texts(allocator), Pictures(allocator), Images(allocator), Retired(allocator), RetiredPictures(allocator) {}

        // A document or sheet read: its handle, and the product it last answered with (held, so
        // a new product never takes its address).
        struct Text
        {
            Guid Id;
            RefPtr<resource::ResourceHandle> Handle;
            RefPtr<Object> Seen;
        };
        // A vector image's picture, parsed from the product it last saw.
        struct Picture
        {
            Guid Id;
            resource::Proxy<foundation::vg::VectorImage> Image;
            RefPtr<Object> Seen;
            UniquePtr<foundation::vg::svg::SVGDocument> Document;
        };
        // A texture's image key.
        struct Image
        {
            Guid Id;
            resource::Proxy<foundation::texture::Texture> Texture;
            UniquePtr<image::ImageDataRef> Key;
            u64 Uid = 0; // the texture product the key stands for; 0: none (yet)
        };

        IAllocator* Allocator;
        resource::ResourceManager* Resources = nullptr;
        Array<Text> Texts;
        Array<UniquePtr<Picture>> Pictures;
        Array<UniquePtr<Image>> Images;
        Array<UniquePtr<image::ImageDataRef>> Retired; // keys no texture backs now (views may hold them)
        Array<UniquePtr<foundation::vg::svg::SVGDocument>> RetiredPictures; // pictures replaced (widgets may hold them)
        TextureSink Sink;

        [[nodiscard]] const foundation::content::Instance* InstanceOf(const Guid& id) const
        {
            return Resources != nullptr ? Resources->Database().GetInstance(id) : nullptr;
        }

        void Tell(const image::ImageData* key, rhi::TextureView* view) const
        {
            if (Sink && key != nullptr)
                Sink(key, view);
        }

        // An image's key in line with its texture's current product.
        void Refresh(Image& entry)
        {
            foundation::texture::Texture* texture = entry.Texture.Get();
            const u64 uid = texture != nullptr && texture->View() != nullptr ? texture->Uid() : 0;
            if (uid == entry.Uid)
                return;
            if (uid != 0 && (!entry.Key || entry.Key->Width() != texture->Width() || entry.Key->Height() != texture->Height()))
            {
                // A key's size is the image's natural size: another size takes a new key, the old one
                // kept for the views that hold it.
                if (entry.Key)
                {
                    Tell(entry.Key.Get(), nullptr);
                    Retired.PushBack(Move(entry.Key));
                }
                entry.Key = MakeUnique<image::ImageDataRef>(*Allocator, texture->Width(), texture->Height());
            }
            entry.Uid = uid;
            Tell(entry.Key.Get(), uid != 0 ? texture->View() : nullptr);
        }

        // A picture parsed again when its vector image's product changed.
        void Refresh(Picture& entry)
        {
            foundation::vg::VectorImage* product = entry.Image.Get();
            if (product == entry.Seen.Get())
                return;
            entry.Seen = RefPtr<Object>(product);
            if (entry.Document)
                RetiredPictures.PushBack(Move(entry.Document));
            if (product == nullptr)
                return;
            Result<foundation::vg::svg::SVGDocument> parsed = foundation::vg::svg::SVGLoader::Load(product->svg.AsView());
            if (parsed.HasValue())
                entry.Document = MakeUnique<foundation::vg::svg::SVGDocument>(*Allocator, Move(parsed.Value()));
            else
                LOG_WARNING(u8"GUI", u8"vector image {} does not parse", entry.Id);
        }
    };

    GuiResourceProvider::GuiResourceProvider(IAllocator& allocator)
        : lab::IResourceProvider(allocator), m_state(MakeUnique<State>(allocator, allocator))
    {
    }
    GuiResourceProvider::~GuiResourceProvider() = default;

    bool GuiResourceProvider::ParseGuid(StringView path, Guid& out) noexcept
    {
        if (path.Size() == 38 && path[0] == utf8char('{') && path[37] == utf8char('}'))
            path = path.SubStr(1, 36);
        return Guid::TryParse(path, out) && !out.IsNil();
    }

    void GuiResourceProvider::SetResourceManager(resource::ResourceManager* resources)
    {
        State& state = *m_state;
        if (resources == state.Resources)
            return;
        state.Resources = resources;
        for (UniquePtr<State::Image>& entry : state.Images)
            if (entry->Key)
            {
                state.Tell(entry->Key.Get(), nullptr);
                state.Retired.PushBack(Move(entry->Key));
            }
        state.Images.Clear();
        for (UniquePtr<State::Picture>& entry : state.Pictures)
            if (entry->Document)
                state.RetiredPictures.PushBack(Move(entry->Document));
        state.Pictures.Clear();
        state.Texts.Clear();
    }

    resource::ResourceManager* GuiResourceProvider::GetResourceManager() const noexcept { return m_state->Resources; }

    void GuiResourceProvider::SetTextureSink(TextureSink sink)
    {
        State& state = *m_state;
        state.Sink = Move(sink);
        for (const UniquePtr<State::Image>& entry : state.Images)
            if (entry->Key && entry->Uid != 0)
                state.Tell(entry->Key.Get(), entry->Texture.Get() != nullptr ? entry->Texture->View() : nullptr);
    }

    const image::ImageData* GuiResourceProvider::LoadImage(StringView path)
    {
        State& state = *m_state;
        Guid id;
        if (!ParseGuid(path, id))
            return nullptr;
        State::Image* entry = nullptr;
        for (const UniquePtr<State::Image>& known : state.Images)
            if (known->Id == id)
                entry = known.Get();
        if (entry == nullptr)
        {
            const foundation::content::Instance* instance = state.InstanceOf(id);
            if (instance == nullptr || IsCookedAs(*instance, foundation::vg::VectorImageSource::StaticType()) ||
                IsCookedAs(*instance, lab::GuiDocumentSource::StaticType()) || IsCookedAs(*instance, lab::GuiStyleSheetSource::StaticType()))
                return nullptr; // not a texture (the texture records are several cooked types)
            UniquePtr<State::Image> made = MakeUnique<State::Image>(*state.Allocator);
            made->Id = id;
            made->Texture = state.Resources->Bind<foundation::texture::Texture>(id);
            entry = made.Get();
            state.Images.PushBack(Move(made));
        }
        state.Refresh(*entry);
        return entry->Uid != 0 ? entry->Key.Get() : nullptr;
    }

    bool GuiResourceProvider::LoadText(StringView path, String& out)
    {
        State& state = *m_state;
        Guid id;
        if (!ParseGuid(path, id))
            return false;
        State::Text* entry = nullptr;
        for (State::Text& known : state.Texts)
            if (known.Id == id)
                entry = &known;
        if (entry == nullptr)
        {
            const foundation::content::Instance* instance = state.InstanceOf(id);
            if (instance == nullptr)
                return false;
            RefPtr<resource::ResourceHandle> handle;
            if (IsCookedAs(*instance, lab::GuiDocumentSource::StaticType()))
                handle = state.Resources->Bind(lab::GuiDocument::StaticType(), id);
            else if (IsCookedAs(*instance, lab::GuiStyleSheetSource::StaticType()))
                handle = state.Resources->Bind(lab::GuiStyleSheet::StaticType(), id);
            else
                return false;
            state.Texts.PushBack(State::Text{id, Move(handle), {}});
            entry = &state.Texts.Back();
        }
        Object* product = entry->Handle->Get();
        entry->Seen = RefPtr<Object>(product);
        if (const lab::GuiDocument* document = Cast<lab::GuiDocument>(product))
        {
            out = document->markup;
            return true;
        }
        if (const lab::GuiStyleSheet* sheet = Cast<lab::GuiStyleSheet>(product))
        {
            out = sheet->css;
            return true;
        }
        return false;
    }

    const foundation::vg::svg::SVGDocument* GuiResourceProvider::LoadSvg(StringView path)
    {
        State& state = *m_state;
        Guid id;
        if (!ParseGuid(path, id))
            return nullptr;
        State::Picture* entry = nullptr;
        for (const UniquePtr<State::Picture>& known : state.Pictures)
            if (known->Id == id)
                entry = known.Get();
        if (entry == nullptr)
        {
            const foundation::content::Instance* instance = state.InstanceOf(id);
            if (instance == nullptr || !IsCookedAs(*instance, foundation::vg::VectorImageSource::StaticType()))
                return nullptr;
            UniquePtr<State::Picture> made = MakeUnique<State::Picture>(*state.Allocator);
            made->Id = id;
            made->Image = state.Resources->Bind<foundation::vg::VectorImage>(id);
            entry = made.Get();
            state.Pictures.PushBack(Move(made));
        }
        state.Refresh(*entry);
        return entry->Document.Get();
    }

    bool GuiResourceProvider::Poll()
    {
        State& state = *m_state;
        for (const UniquePtr<State::Image>& entry : state.Images)
            state.Refresh(*entry);
        for (const UniquePtr<State::Picture>& entry : state.Pictures)
            state.Refresh(*entry);
        bool changed = false;
        for (const State::Text& entry : state.Texts)
            changed = changed || entry.Handle->Get() != entry.Seen.Get();
        return changed;
    }

    // ===================================================================================
    // Components
    // ===================================================================================

    namespace
    {
        // A canvas's place in its scene's root, filling it (the `canvas` element to a sheet): its
        // document laid out at the root's size, or at the reference resolution and scaled
        // uniformly to fit, centred on a whole pixel. Paints nothing, so it never takes the
        // pointer from the game: only what its document paints does.
        class CanvasHost final : public lab::UIWidget
        {
        public:
            CanvasHost() { SetTag(StringView(u8"canvas")); }

            void Configure(i32 order, GuiScalerMode scaler, Float2 reference)
            {
                Order = order;
                if (scaler == m_scaler && reference == m_reference)
                    return;
                m_scaler = scaler;
                m_reference = reference;
                RequestLayout();
            }

            void OnLayout() override
            {
                const Float2 size = GetSize();
                Float2 inner = size;
                f32 scale = 1.0f;
                Float2 offset{0.0f, 0.0f};
                if (m_scaler == GuiScalerMode::ReferenceResolution && m_reference.x > 0.0f && m_reference.y > 0.0f && size.x > 0.0f &&
                    size.y > 0.0f)
                {
                    inner = m_reference;
                    scale = Min(size.x / inner.x, size.y / inner.y);
                    offset = Float2{Round((size.x - inner.x * scale) * 0.5f), Round((size.y - inner.y * scale) * 0.5f)};
                }
                for (usize i = 0; i < ChildCount(); ++i)
                {
                    lab::Node* child = GetChildAt(i);
                    child->SetScaleOrigin(Float2{0.0f, 0.0f});
                    child->SetScale(Float2{scale, scale});
                    child->Arrange(lab::Rect{offset.x, offset.y, inner.x, inner.y});
                }
            }

            i32 Order = 0;

        private:
            GuiScalerMode m_scaler = GuiScalerMode::ConstantPixel;
            Float2 m_reference{0.0f, 0.0f};
        };
    }

    REFLECT_ENUM(GuiScalerMode, "rtti::engine::gui")
    {
        builder.Value("ConstantPixel", GuiScalerMode::ConstantPixel);
        builder.Value("ReferenceResolution", GuiScalerMode::ReferenceResolution);
    }

    REFLECT_VALUE(GuiCanvasComponent, "rtti::engine::gui")
    {
        builder.Attribute("displayName", String(u8"GUI Canvas"))
            .Attribute("category", String(u8"GUI"))
            .Attribute("description", String(u8"Shows a GUI document in its scene, as a menu or a HUD."))
            .DataVersion(1);
        builder.Property<&GuiCanvasComponent::document>("document");
        builder.Property<&GuiCanvasComponent::sheet>("sheet");
        builder.Property<&GuiCanvasComponent::order>("order");
        builder.Property<&GuiCanvasComponent::visible>("visible");
        builder.Property<&GuiCanvasComponent::interactive>("interactive");
        builder.Property<&GuiCanvasComponent::scalerMode>("scalerMode");
        builder.Property<&GuiCanvasComponent::referenceResolution>("referenceResolution");
    }

    REFLECT_ENUM(GuiBillboardOrientation, "rtti::engine::gui")
    {
        builder.Value("Screen", GuiBillboardOrientation::Screen);
        builder.Value("Cylindrical", GuiBillboardOrientation::Cylindrical);
    }

    REFLECT_ENUM(GuiBillboardScale, "rtti::engine::gui")
    {
        builder.Value("Fixed", GuiBillboardScale::Fixed);
        builder.Value("Distance", GuiBillboardScale::Distance);
    }

    REFLECT_VALUE(GuiBillboardComponent, "rtti::engine::gui")
    {
        builder.Attribute("displayName", String(u8"GUI Billboard"))
            .Attribute("category", String(u8"GUI"))
            .Attribute("description", String(u8"Shows a GUI document over the entity, such as a nameplate or a health bar."))
            .DataVersion(1);
        builder.Property<&GuiBillboardComponent::document>("document");
        builder.Property<&GuiBillboardComponent::sheet>("sheet");
        builder.Property<&GuiBillboardComponent::offset>("offset");
        builder.Property<&GuiBillboardComponent::orientation>("orientation");
        builder.Property<&GuiBillboardComponent::scaleMode>("scaleMode");
        builder.Property<&GuiBillboardComponent::referenceDistance>("referenceDistance");
        builder.Property<&GuiBillboardComponent::minScale>("minScale");
        builder.Property<&GuiBillboardComponent::maxScale>("maxScale");
        builder.Property<&GuiBillboardComponent::visible>("visible");
    }

    GuiBillboardPlacement PlaceBillboard(const GuiBillboardComponent& billboard, const Float4x4& entityWorld, const Float4x4& viewProjection,
                                         Float3 cameraPosition, Float2 viewport) noexcept
    {
        GuiBillboardPlacement placement;
        Float3 point;
        if (billboard.orientation == GuiBillboardOrientation::Cylindrical)
        {
            const Float3 anchor = TransformPoint(Float3{0.0f, 0.0f, 0.0f}, entityWorld);
            point = Float3{anchor.x + billboard.offset.x, anchor.y + billboard.offset.y, anchor.z + billboard.offset.z};
        }
        else
            point = TransformPoint(billboard.offset, entityWorld);
        const Float4 clip = Float4{point.x, point.y, point.z, 1.0f} * viewProjection;
        if (clip.w <= 0.0f)
            return placement; // behind the camera
        placement.Shown = true;
        placement.Anchor = Float2{(clip.x / clip.w * 0.5f + 0.5f) * viewport.x, (0.5f - clip.y / clip.w * 0.5f) * viewport.y};
        if (billboard.scaleMode == GuiBillboardScale::Distance)
        {
            const f32 distance = Max(Length(Float3{point.x - cameraPosition.x, point.y - cameraPosition.y, point.z - cameraPosition.z}), 0.001f);
            placement.Scale = Clamp(billboard.referenceDistance / distance, billboard.minScale, billboard.maxScale);
        }
        return placement;
    }

    void RegisterGuiComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_GuiScalerMode();
            RttiRegisterValue_GuiCanvasComponent();
            RttiRegisterEnum_GuiBillboardOrientation();
            RttiRegisterEnum_GuiBillboardScale();
            RttiRegisterValue_GuiBillboardComponent();
            return true;
        }();
        (void)once;
    }

    // ===================================================================================
    // GuiSubsystem
    // ===================================================================================

    struct GuiSubsystem::RenderState
    {
        explicit RenderState(IAllocator& allocator) noexcept : Shaders(allocator) {}

        rhi::Device* Device = nullptr;
        lab::VgShaderLibrary Shaders;
        UniquePtr<lab::GuiPassRenderer> Renderer;
        // What PrepareOverlay recorded for the screen tier, and for which view.
        lab::GuiPassRenderer::Prepared Screen{};
        render::ScreenOverlayView ScreenView{};
        bool ScreenPrepared = false;
    };

    GuiSubsystem::GuiSubsystem(IAllocator& allocator, foundation::vfs::IFileSystem& dataFileSystem)
        : m_allocator(allocator, RegisterMemoryTag("GUI")), m_dataFileSystem(&dataFileSystem), m_sheets(m_allocator),
          m_sceneTiers(m_allocator)
    {
        m_context = MakeRef<lab::GUIContext>(m_allocator);
        m_resources = MakeUnique<GuiResourceProvider>(m_allocator, m_allocator);
        m_context->SetResourceProvider(m_resources.Get());
        m_styles.SetResourceProvider(m_resources.Get()); // the sheets' @import, url() and fonts
    }
    GuiSubsystem::~GuiSubsystem() = default;

    foundation::fonts::IFontService* GuiSubsystem::Fonts() const noexcept { return m_fonts.Get(); }

    bool GuiSubsystem::IsRenderReady() const noexcept { return m_render && m_render->Renderer && m_render->Renderer->IsUsable(); }

    bool GuiSubsystem::ReadDataFile(StringView path, Array<u8>& out) const
    {
        UniquePtr<IStream> stream = m_dataFileSystem->Open(path, FileMode::Read);
        if (!stream || !stream->IsValid())
            return false;
        const i64 size = stream->Size();
        if (size <= 0)
            return false;
        out.Resize(static_cast<usize>(size));
        return stream->Read(out.Data(), out.Size()) == static_cast<u64>(out.Size());
    }

    void GuiSubsystem::OnInit()
    {
        lab::RegisterGuiElements();     // the lab's widgets, by tag from markup
        lab::RegisterGamekitElements(); // and a game's screens and prompts
        // Before any font loads: a font gets its shaper and its atlas's glyphs when it loads.
        foundation::fonts::HarfBuzzShaping::Install();
        foundation::fonts::BidiAlgorithm::Install();
        UniquePtr<foundation::fonts::TrueTypeFontService> fonts = MakeUnique<foundation::fonts::TrueTypeFontService>(m_allocator, m_allocator);
        // The built-in font through the data mount (in a dev tree and any dist that stages it); the
        // project's own fonts come with the player's settings (E9).
        constexpr StringView kDefaultFont = u8"Assets/fonts/roboto/Roboto-Regular.ttf";
        Array<u8> bytes(m_allocator);
        if (ReadDataFile(kDefaultFont, bytes) &&
            fonts->LoadFontFromMemory(u8"Roboto", Span<const u8>(bytes.Data(), bytes.Size())) == foundation::fonts::FontLoadResult::Success)
            fonts->SetDefaultFamily(u8"Roboto");
        else
            LOG_INFO(u8"GUI", u8"built-in font '{}' not in the data root: GUI text needs the project's fonts", kDefaultFont);
        m_fonts = Move(fonts);
        m_context->SetFontService(m_fonts.Get());
        m_styles.SetFontService(m_fonts.Get());
        m_styles.SetUserAgentSheet(lab::CSSParser::Parse(lab::DefaultThemeCSS(), StringView(u8"theme")));
        ApplySheets();

        // The screen tier: the screen stack, and the overlay layer above it, each filling the root.
        // Neither paints, so neither takes the pointer from the game until something in it does.
        m_screenRoot = MakeRef<lab::SceneRoot>(m_allocator, m_context);
        {
            lab::Node::StyleWriteScope defaults(*m_screenRoot);
            m_screenRoot->EditLayoutStyle([](lab::LayoutStyle& s) { s.Kind = lab::Display::Stack; });
        }
        m_screens = MakeRef<lab::ScreenStack>(m_allocator);
        m_screenRoot->AddChild(m_screens.Get());
        m_overlay = MakeRef<lab::UIWidget>(m_allocator);
        m_overlay->SetTag(StringView(u8"overlays"));
        {
            lab::Node::StyleWriteScope defaults(*m_overlay);
            m_overlay->EditLayoutStyle([](lab::LayoutStyle& s) { s.Kind = lab::Display::Stack; });
        }
        m_screenRoot->AddChild(m_overlay.Get());
        m_screenBridge = MakeBridge(*m_screenRoot);
    }

    void GuiSubsystem::OnReady()
    {
        foundation::runtime::Context* context = GetContext();
        if (context == nullptr)
            return;
        m_input = context->GetSubsystem<engine::input::InputSubsystem>();
        if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
        {
            scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
            scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
        }
        // Headless contexts (tests, the cooker) have no render subsystem: nothing registers.
        if (auto* renderer = context->GetSubsystem<engine::render::RenderSubsystem>())
        {
            m_sceneRenderer = renderer;
            m_screenRenderer = renderer;
            m_sceneRenderer->RegisterOverlay(static_cast<render::ISceneOverlay*>(this));
            m_screenRenderer->RegisterOverlay(static_cast<render::IScreenOverlay*>(this));
        }
    }

    void GuiSubsystem::OnShutdown()
    {
        if (m_sceneRenderer != nullptr)
            m_sceneRenderer->UnregisterOverlay(static_cast<render::ISceneOverlay*>(this));
        if (m_screenRenderer != nullptr)
            m_screenRenderer->UnregisterOverlay(static_cast<render::IScreenOverlay*>(this));
        m_sceneRenderer = nullptr;
        m_screenRenderer = nullptr;
        if (foundation::runtime::Context* context = GetContext())
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
                scenes->UnregisterObserver(this);
        m_activeRoot = nullptr;
        m_pressRoot = nullptr;
        m_input = nullptr;
        m_sceneTiers.Clear();
        m_screenBridge = nullptr;
        if (m_screens)
            m_screens->Clear();
        m_overlay = nullptr;
        m_screens = nullptr;
        m_screenRoot = nullptr;
        m_resources->SetTextureSink(nullptr);
        m_context->SetPictureBaker(nullptr);
        m_render = nullptr; // waits for the GPU, then lets the renderers go
    }

    void GuiSubsystem::BeginFrame(f32 deltaTime)
    {
        PROFILE_SCOPE("GUI.BeginFrame");
        if (m_render && m_render->Renderer)
            m_render->Renderer->BeginFrame();
        if (m_resources->Poll())
            ApplySheets(); // a sheet (or a document) was cooked again: the GUI restyles
        for (SceneTier& tier : m_sceneTiers)
        {
            SyncBillboards(tier);
            SyncCanvases(tier);
        }
        PumpInput();
        // Unscaled: menus move while the game is paused.
        m_context->Update(Duration::FromSeconds(static_cast<f64>(deltaTime)));
        if (m_screenRoot)
            StyleAndLayOut(*m_screenRoot, m_screenRoot->GetViewportSize());
    }

    void GuiSubsystem::SetResourceManager(resource::ResourceManager* resources)
    {
        m_resources->SetResourceManager(resources);
        ApplySheets(); // the sheets resolve in the new project (or not at all)
    }

    void GuiSubsystem::SetStyleSheets(Span<const Guid> sheets)
    {
        m_sheets.Clear();
        for (const Guid& sheet : sheets)
            if (!sheet.IsNil())
                m_sheets.PushBack(sheet);
        ApplySheets();
    }

    // The game's sheets as one author sheet importing each, in order: the style manager expands
    // the imports through the provider, each over the ones before it.
    void GuiSubsystem::ApplySheets()
    {
        String css(m_allocator);
        for (const Guid& sheet : m_sheets)
        {
            css.Append(StringView(u8"@import \"{"));
            css.Append(Format(u8"{}", sheet).AsView());
            css.Append(StringView(u8"}\";\n"));
        }
        m_styles.SetStyleSheet(lab::CSSParser::Parse(css.AsView(), StringView(u8"game")));
        ++m_sheetGeneration;
    }

    void GuiSubsystem::StyleAndLayOut(lab::SceneRoot& root, Float2 size)
    {
        if (root.GetViewportSize() != size)
            root.SetViewportSize(size);
        m_styles.SetMediaContext(root.GetMediaContext());
        m_styles.ApplyTree(root);
        root.UpdateLayout();
    }

    lab::SceneRoot* GuiSubsystem::SceneRootFor(const scene::Scene& scene) noexcept
    {
        for (const SceneTier& tier : m_sceneTiers)
            if (tier.Scene == &scene)
                return tier.Root.Get();
        return nullptr;
    }

    void GuiSubsystem::OnSystemsReady(scene::Scene& scene)
    {
        if (SceneRootFor(scene) != nullptr)
            return;
        SceneTier tier;
        tier.Scene = &scene;
        tier.Root = MakeRef<lab::SceneRoot>(m_allocator, m_context);
        {
            lab::Node::StyleWriteScope defaults(*tier.Root);
            tier.Root->EditLayoutStyle([](lab::LayoutStyle& s) { s.Kind = lab::Display::Stack; }); // each canvas fills it
        }
        // The billboards' layer, under every canvas; it paints nothing, so it takes no pointer.
        tier.Billboards = MakeRef<lab::UIWidget>(m_allocator);
        tier.Billboards->SetTag(StringView(u8"billboards"));
        tier.Root->AddChild(tier.Billboards.Get());
        tier.Bridge = MakeBridge(*tier.Root);
        m_sceneTiers.PushBack(Move(tier));
    }

    void GuiSubsystem::OnDestroying(scene::Scene& scene)
    {
        for (usize i = 0; i < m_sceneTiers.Size(); ++i)
            if (m_sceneTiers[i].Scene == &scene)
            {
                if (m_activeRoot == m_sceneTiers[i].Root.Get())
                    m_activeRoot = nullptr;
                if (m_pressRoot == m_sceneTiers[i].Root.Get())
                    m_pressRoot = nullptr;
                m_sceneTiers.RemoveAt(i);
                return;
            }
    }

    // Each canvas's tree in line with its component: built from its document (again when the
    // document is cooked again), in its host in the scene's root, its sheet, shown while the canvas
    // and its entity are; the hosts of canvases gone swept; the hosts ordered.
    void GuiSubsystem::SyncCanvases(SceneTier& tier)
    {
        auto* canvases = tier.Scene->GetSystem<GuiCanvasComponentManager>();
        Array<const lab::Node*> seen(m_allocator);
        if (canvases != nullptr)
            canvases->ForEach(
                [&](GuiCanvasComponent& c, scene::EntityHandle entity)
                {
                    lab::GuiDocument* document = c.document.Get();
                    if (document != c.builtFrom.Get())
                    {
                        if (c.root && c.root->GetParent() != nullptr)
                            c.root->GetParent()->RemoveChild(c.root.Get());
                        c.root = nullptr;
                        c.sheetFrom = nullptr;
                        if (document != nullptr && !document->markup.IsEmpty())
                        {
                            lab::MarkupLoader loader(m_context->GetElementSet(), m_allocator, m_resources.Get(), m_fonts.Get());
                            loader.SetDataModel(m_context->GetDataRef());
                            c.root = loader.LoadFromString(document->markup.AsView());
                            if (!c.root)
                                LOG_WARNING(u8"GUI", u8"a canvas's document did not build");
                        }
                        c.builtFrom = RefPtr<Object>(document);
                    }
                    if (!c.host)
                    {
                        c.host = MakeRef<CanvasHost>(m_allocator);
                        tier.Root->AddChild(c.host.Get());
                        tier.Canvases.PushBack(c.host);
                    }
                    auto* host = static_cast<CanvasHost*>(c.host.Get());
                    host->Configure(c.order, c.scalerMode, c.referenceResolution);
                    seen.PushBack(host);
                    if (!c.root)
                        return;
                    if (c.root->GetParent() == nullptr)
                        host->AddChild(c.root.Get());
                    lab::GuiStyleSheet* sheet = c.sheet.Get();
                    if (sheet != c.sheetFrom.Get())
                    {
                        if (sheet != nullptr)
                            (void)m_styles.SetSubtreeSheet(*c.root, lab::CSSParser::Parse(sheet->css.AsView(), StringView(u8"canvas")));
                        else
                            m_styles.RemoveSubtreeSheet(*c.root);
                        c.sheetFrom = RefPtr<Object>(sheet);
                    }
                    c.root->SetDisplayed(c.visible && tier.Scene->IsEffectivelyActive(entity));
                    c.root->SetInert(!c.interactive);
                });
        // The hosts whose canvas went (despawned, the component removed), then the order: by
        // `order`, stably, after whatever else the root holds.
        Array<CanvasHost*> hosts(m_allocator);
        for (usize i = 0; i < tier.Canvases.Size();)
        {
            CanvasHost* host = static_cast<CanvasHost*>(tier.Canvases[i].Get());
            bool live = false;
            for (const lab::Node* known : seen)
                live = live || known == host;
            if (live)
            {
                hosts.PushBack(host);
                ++i;
                continue;
            }
            tier.Root->RemoveChild(host);
            tier.Canvases.RemoveAt(i);
        }
        // In the root's order, for the sort.
        for (usize i = 1; i < hosts.Size(); ++i)
            for (usize j = i; j > 0 && tier.Root->GetChildIndex(hosts[j]) < tier.Root->GetChildIndex(hosts[j - 1]); --j)
                Swap(hosts[j - 1], hosts[j]);
        bool sorted = true;
        for (usize i = 1; i < hosts.Size(); ++i)
            sorted = sorted && hosts[i - 1]->Order <= hosts[i]->Order;
        if (sorted)
            return;
        for (usize i = 1; i < hosts.Size(); ++i) // stable insertion sort
            for (usize j = i; j > 0 && hosts[j - 1]->Order > hosts[j]->Order; --j)
                Swap(hosts[j - 1], hosts[j]);
        for (CanvasHost* host : hosts)
            tier.Root->SetChildIndex(host, tier.Root->ChildCount() - 1);
    }

    // Each billboard's tree in line with its component, in the scene's billboard layer (rebuilt
    // when its document changes, its sheet, shown while it and its entity are); the trees of
    // billboards gone swept. Where each shows is a view's (PlaceBillboards).
    void GuiSubsystem::SyncBillboards(SceneTier& tier)
    {
        auto* billboards = tier.Scene->GetSystem<GuiBillboardComponentManager>();
        Array<const lab::Node*> seen(m_allocator);
        if (billboards != nullptr)
            billboards->ForEach(
                [&](GuiBillboardComponent& c, scene::EntityHandle entity)
                {
                    lab::GuiDocument* document = c.document.Get();
                    if (document != c.builtFrom.Get())
                    {
                        if (c.root && c.root->GetParent() != nullptr)
                            c.root->GetParent()->RemoveChild(c.root.Get());
                        c.root = nullptr;
                        c.sheetFrom = nullptr;
                        if (document != nullptr && !document->markup.IsEmpty())
                        {
                            lab::MarkupLoader loader(m_context->GetElementSet(), m_allocator, m_resources.Get(), m_fonts.Get());
                            loader.SetDataModel(m_context->GetDataRef());
                            c.root = loader.LoadFromString(document->markup.AsView());
                            if (!c.root)
                                LOG_WARNING(u8"GUI", u8"a billboard's document did not build");
                        }
                        c.builtFrom = RefPtr<Object>(document);
                    }
                    if (!c.root)
                        return;
                    if (c.root->GetParent() == nullptr)
                    {
                        c.root->EditLayoutStyle([](lab::LayoutStyle& s) { s.Placement = lab::Position::Absolute; });
                        tier.Billboards->AddChild(c.root.Get());
                    }
                    seen.PushBack(c.root.Get());
                    lab::GuiStyleSheet* sheet = c.sheet.Get();
                    if (sheet != c.sheetFrom.Get())
                    {
                        if (sheet != nullptr)
                            (void)m_styles.SetSubtreeSheet(*c.root, lab::CSSParser::Parse(sheet->css.AsView(), StringView(u8"billboard")));
                        else
                            m_styles.RemoveSubtreeSheet(*c.root);
                        c.sheetFrom = RefPtr<Object>(sheet);
                    }
                    c.root->SetDisplayed(c.visible && tier.Scene->IsEffectivelyActive(entity));
                });
        for (usize i = tier.Billboards->ChildCount(); i-- > 0;)
        {
            lab::Node* child = tier.Billboards->GetChildAt(i);
            bool live = false;
            for (const lab::Node* known : seen)
                live = live || known == child;
            if (!live)
                tier.Billboards->RemoveChild(child);
        }
    }

    // Each shown billboard placed for `view`: hung by its bottom centre on its projected point (its
    // size the last layout's: a new one settles the next frame), scaled toward that point, hidden
    // behind the camera, keeping its layout so nothing else moves.
    void GuiSubsystem::PlaceBillboards(SceneTier& tier, const render::SceneOverlayView& view)
    {
        auto* billboards = tier.Scene->GetSystem<GuiBillboardComponentManager>();
        if (billboards == nullptr)
            return;
        const Float2 viewport{static_cast<f32>(view.viewportWidth), static_cast<f32>(view.viewportHeight)};
        billboards->ForEach(
            [&](GuiBillboardComponent& c, scene::EntityHandle entity)
            {
                if (!c.root || !c.root->IsDisplayed())
                    return;
                const GuiBillboardPlacement placement =
                    PlaceBillboard(c, tier.Scene->GetWorldMatrix(entity), view.viewProjection, view.cameraPosition, viewport);
                c.root->SetVisible(placement.Shown);
                if (!placement.Shown)
                    return;
                const Float2 size = c.root->GetSize();
                const Float2 corner{placement.Anchor.x - size.x * 0.5f, placement.Anchor.y - size.y};
                c.root->EditLayoutStyle(
                    [corner](lab::LayoutStyle& s)
                    {
                        s.Left = lab::LayoutLength::Px(corner.x);
                        s.Top = lab::LayoutLength::Px(corner.y);
                    });
                c.root->SetScaleOrigin(Float2{size.x * 0.5f, size.y});
                c.root->SetScale(Float2{placement.Scale, placement.Scale});
            });
    }

    // Whether a scene's GUI shows anything: a canvas, or a billboard.
    bool GuiSubsystem::HasContent(const SceneTier& tier) noexcept
    {
        return !tier.Canvases.IsEmpty() || (tier.Billboards && tier.Billboards->ChildCount() > 0);
    }

    UniquePtr<lab::GuiInputBridge> GuiSubsystem::MakeBridge(lab::SceneRoot& root)
    {
        UniquePtr<lab::GuiInputBridge> bridge = MakeUnique<lab::GuiInputBridge>(m_allocator, root.GetEventDispatcher());
        bridge->SetInputKindTracker(&m_inputKind); // the pointer, the keys or a pad (K2b)
        bridge->SetTextInputTarget(m_textInputTarget);
        return bridge;
    }

    void GuiSubsystem::SetTextInputTarget(foundation::shell::IWindow* window)
    {
        m_textInputTarget = window;
        if (m_screenBridge)
            m_screenBridge->SetTextInputTarget(window);
        for (const SceneTier& tier : m_sceneTiers)
            tier.Bridge->SetTextInputTarget(window);
    }

    lab::GuiInputBridge* GuiSubsystem::BridgeOf(const lab::SceneRoot* root) noexcept
    {
        if (root != nullptr && root == m_screenRoot.Get())
            return m_screenBridge.Get();
        for (const SceneTier& tier : m_sceneTiers)
            if (tier.Root.Get() == root)
                return tier.Bridge.Get();
        return nullptr;
    }

    // Whether this frame's input may reach `tier`'s scene: the source's own scene when it is bound
    // to one (an editor Game tab), else every scene or none, as the input subsystem's policy says.
    bool GuiSubsystem::InputReaches(const SceneTier& tier) const noexcept
    {
        if (m_input == nullptr)
            return false;
        if (const void* bound = m_input->BoundSceneKey(); bound != nullptr)
            return static_cast<const void*>(tier.Scene) == bound;
        return m_input->UnboundScenePolicy() == engine::input::UnboundInputScenePolicy::AllScenes;
    }

    lab::SceneRoot* GuiSubsystem::PickInputRoot(bool pointer, Float2 point)
    {
        if (!m_screenRoot)
            return nullptr;
        // An occupied overlay layer (a system menu, a loading screen) is modal.
        for (usize i = 0; m_overlay && i < m_overlay->ChildCount(); ++i)
            if (m_overlay->GetChildAt(i)->IsDisplayed())
                return m_screenRoot.Get();
        // The GUI under the pointer, the screen tier's over a scene's.
        if (pointer)
        {
            if (m_screenRoot->GetEventDispatcher()->IsPointerOver(point))
                return m_screenRoot.Get();
            for (const SceneTier& tier : m_sceneTiers)
                if (InputReaches(tier) && tier.Root->GetEventDispatcher()->IsPointerOver(point))
                    return tier.Root.Get();
        }
        // The keys and a pad: the screens a game pushed, else a scene's GUI.
        if (m_screens && m_screens->Count() > 0)
            return m_screenRoot.Get();
        for (const SceneTier& tier : m_sceneTiers)
            if (InputReaches(tier) && HasContent(tier))
                return tier.Root.Get();
        return m_screenRoot.Get();
    }

    void GuiSubsystem::PumpInput()
    {
        m_pointerTaken = false;
        if (m_input == nullptr || !m_screenRoot)
            return;
        foundation::input::IInputSourceProvider& devices = m_input->ActiveSource();
        m_inputKind.Update(devices);
        foundation::shell::IMouse* mouse = devices.Mouse();
        foundation::shell::ITouch* touch = devices.Touch();
        const bool fingers = touch != nullptr && touch->TouchCount() > 0;
        Float2 point{0.0f, 0.0f};
        if (fingers)
        {
            foundation::shell::TouchPoint finger;
            if (touch->GetTouchPoint(0, finger))
                point = Float2{finger.pointerX, finger.pointerY};
        }
        else if (mouse != nullptr)
            point = Float2{mouse->X(), mouse->Y()};
        const bool pointer = fingers || mouse != nullptr;
        const bool held = fingers || (mouse != nullptr && (mouse->IsButtonDown(foundation::shell::MouseButton::Left) ||
                                                           mouse->IsButtonDown(foundation::shell::MouseButton::Right) ||
                                                           mouse->IsButtonDown(foundation::shell::MouseButton::Middle)));

        // One root a frame; a held press stays with the root it began in, so its release lands.
        lab::SceneRoot* root = held && m_pressRoot != nullptr ? m_pressRoot : PickInputRoot(pointer, point);
        if (root != m_activeRoot && m_activeRoot != nullptr && pointer)
        {
            // The root input leaves: nothing in it stays hovered.
            (void)m_activeRoot->GetEventDispatcher()->InjectMouseMove(Float2{-1.0e6f, -1.0e6f});
        }
        m_activeRoot = root;
        lab::GuiInputBridge* bridge = BridgeOf(root);
        if (bridge == nullptr)
            return;
        if (held && m_pressRoot == nullptr)
        {
            m_pressRoot = root; // the press begins: where it began says whether the GUI took it
            m_pressTaken = root->GetEventDispatcher()->IsPointerOver(point);
        }
        bridge->PumpDevices(mouse, touch, devices.Keyboard());
        // Keys, text and the pad through their events, in order; the pointer was polled above.
        for (const foundation::shell::InputEvent& event : devices.Events())
            switch (event.kind)
            {
            case foundation::shell::InputEventKind::KeyDown:
            case foundation::shell::InputEventKind::KeyUp:
            case foundation::shell::InputEventKind::TextInput:
            case foundation::shell::InputEventKind::TextEditing:
            case foundation::shell::InputEventKind::GamepadButtonDown:
            case foundation::shell::InputEventKind::GamepadButtonUp:
            case foundation::shell::InputEventKind::GamepadAxis:
                (void)bridge->Dispatch(event);
                break;
            default:
                break;
            }
        bridge->SyncInputKind(); // a pad or keys with no event this frame still count

        // What the action layer is told the GUI took: the pointer over GUI that takes it, or a press
        // that began there while it is held; the keys while text is edited. Added to Engine.UI's
        // claim (its pump runs first in this lane); EndFrame clears them after the frame read them.
        m_pointerTaken = pointer && (held ? m_pressTaken : root->GetEventDispatcher()->IsPointerOver(point));
        const bool keys = root->GetEventDispatcher()->WantsTextInput();
        if (!held)
        {
            m_pressRoot = nullptr;
            m_pressTaken = false;
        }
        const bool pointerTaken = m_pointerTaken;
        m_input->ForEachActionRuntime(
            [pointerTaken, keys](foundation::input::ActionRuntime& runtime)
            {
                foundation::input::ActionRuntime::ConsumptionMask mask = runtime.GetConsumptionMask();
                mask.pointer = mask.pointer || pointerTaken;
                mask.keyboard = mask.keyboard || keys;
                runtime.SetConsumptionMask(mask);
            });
    }

    void GuiSubsystem::EndFrame()
    {
        if (m_input == nullptr)
            return;
        m_input->ForEachActionRuntime([](foundation::input::ActionRuntime& runtime) { runtime.SetConsumptionMask({}); });
    }

    void GuiSubsystem::EnsureRenderReady(rhi::Device& device, i32 frameCount)
    {
        if (m_render && m_render->Device != nullptr)
            return;
        m_render = MakeUnique<RenderState>(m_allocator, m_allocator);
        m_render->Device = &device;
        if (!m_render->Shaders.Load(device, *m_dataFileSystem))
        {
            LOG_ERROR(u8"GUI", u8"no shader compiler and no shader pack: the GUI will not draw");
            return;
        }
        m_render->Renderer = MakeUnique<lab::GuiPassRenderer>(m_allocator, m_allocator, device, static_cast<u32>(frameCount < 1 ? 1 : frameCount),
                                                              m_fonts.Get(), m_render->Shaders.Set());
        m_context->SetPictureBaker(m_render->Renderer->Baker());
        lab::GuiPassRenderer* renderer = m_render->Renderer.Get();
        m_resources->SetTextureSink(
            [renderer](const image::ImageData* key, rhi::TextureView* view)
            {
                if (view != nullptr)
                    renderer->RegisterTexture(key, view);
                else
                    renderer->UnregisterTexture(key);
            });
    }

    void GuiSubsystem::Render(rhi::RenderPassEncoder& encoder, const render::SceneOverlayView& view)
    {
        if (!IsRenderReady() || view.viewportWidth == 0 || view.viewportHeight == 0)
            return;
        lab::SceneRoot* root = nullptr;
        for (const SceneTier& tier : m_sceneTiers)
            if (static_cast<const void*>(tier.Scene) == view.sceneKey)
                root = tier.Root.Get();
        if (root == nullptr)
            return;
        SceneTier* tier = nullptr;
        for (SceneTier& known : m_sceneTiers)
            if (known.Root.Get() == root)
                tier = &known;
        if (tier == nullptr || !HasContent(*tier))
            return;
        PlaceBillboards(*tier, view);
        // Laid out at the viewport's size (a split screen's half lays out its own) and drawn there.
        StyleAndLayOut(*root, Float2{static_cast<f32>(view.viewportWidth), static_cast<f32>(view.viewportHeight)});
        lab::GuiPassView pass;
        pass.X = view.viewportX;
        pass.Y = view.viewportY;
        pass.Width = view.viewportWidth;
        pass.Height = view.viewportHeight;
        pass.Format = view.targetFormat;
        pass.DepthStencilFormat = view.depthStencilFormat;
        pass.FrameIndex = view.frameIndex;
        m_render->Renderer->DrawInPass(*root, encoder, pass);
    }

    namespace
    {
        [[nodiscard]] lab::GuiPassView ScreenPass(const render::ScreenOverlayView& view) noexcept
        {
            lab::GuiPassView pass;
            pass.Width = view.width;
            pass.Height = view.height;
            pass.Format = view.targetFormat;
            pass.DepthStencilFormat = view.depthStencilFormat;
            pass.Encoded = view.encodedTarget;
            pass.FrameIndex = view.frameIndex;
            return pass;
        }
        [[nodiscard]] bool SameView(const render::ScreenOverlayView& a, const render::ScreenOverlayView& b) noexcept
        {
            return a.width == b.width && a.height == b.height && a.targetFormat == b.targetFormat &&
                   a.depthStencilFormat == b.depthStencilFormat && a.frameIndex == b.frameIndex && a.encodedTarget == b.encodedTarget;
        }
    }

    void GuiSubsystem::PrepareOverlay(rhi::CommandEncoder& encoder, const render::ScreenOverlayView& view)
    {
        if (!IsRenderReady())
            return;
        m_render->ScreenPrepared = false;
        // The bakes before any pass samples them, once a frame.
        (void)m_render->Renderer->RecordBakes(encoder, view.frameIndex);
        if (!m_screenRoot || view.width == 0 || view.height == 0)
            return;
        StyleAndLayOut(*m_screenRoot, Float2{static_cast<f32>(view.width), static_cast<f32>(view.height)});
        m_render->Screen = m_render->Renderer->Prepare(*m_screenRoot, ScreenPass(view), &encoder);
        m_render->ScreenView = view;
        m_render->ScreenPrepared = true;
    }

    void GuiSubsystem::Render(rhi::RenderPassEncoder& encoder, const render::ScreenOverlayView& view)
    {
        if (!IsRenderReady() || !m_screenRoot || view.width == 0 || view.height == 0)
            return;
        const bool prepared = m_render->ScreenPrepared && SameView(m_render->ScreenView, view);
        m_render->ScreenPrepared = false;
        if (prepared)
        {
            m_render->Renderer->Draw(encoder, m_render->Screen);
            return;
        }
        StyleAndLayOut(*m_screenRoot, Float2{static_cast<f32>(view.width), static_cast<f32>(view.height)});
        m_render->Renderer->DrawInPass(*m_screenRoot, encoder, ScreenPass(view));
    }

    const engine::DomainModule& GuiDomain() noexcept
    {
        static const resource::ResourceModule* const kResources[] = {
            &lab::kGuiResourceModule, &foundation::vg::kVGResourceModule, &foundation::fonts::kFontsResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"gui",
            .installScene = &AddGuiSceneManagers,
            .registerReflection = &RegisterGuiComponentReflection,
            .resources = Span<const resource::ResourceModule* const>{kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}

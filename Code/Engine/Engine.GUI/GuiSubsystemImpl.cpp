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
import foundation.gui;
import foundation.gui.gamekit;
import foundation.gui.shell;
import foundation.script;          // ComponentOf: a component's `.of(entity)`
import foundation.script.facades;  // the behaviour prelude's names
import foundation.gui.resources; // GuiDocument, GuiStyleSheet, kGuiResourceModule
import foundation.gui.hosting;   // GuiPassRenderer, VgShaderLibrary
import foundation.vg.renderer;     // VGPassTargets: a texture's multisampled pass

using namespace foundation::core;
using namespace foundation::gui;
namespace rhi = foundation::rhi; // repeated here because MSVC does not carry the interface's alias

namespace engine::gui
{
    namespace
    {
        // Whether `instance` was cooked as `cooked`. A guid is bound only as the product type it
        // really is, because a handle bound as another product type caches its failure.
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

        // A document or sheet that was read: its handle, and the product it last answered with.
        // The product is held so that a new product never reuses its address.
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
            u64 Uid = 0; // the texture product the key stands for; 0 when there is none yet
        };

        IAllocator* Allocator;
        resource::ResourceManager* Resources = nullptr;
        Array<Text> Texts;
        Array<UniquePtr<Picture>> Pictures;
        Array<UniquePtr<Image>> Images;
        Array<UniquePtr<image::ImageDataRef>> Retired; // keys no texture backs any more (views may still hold them)
        Array<UniquePtr<foundation::vg::svg::SVGDocument>> RetiredPictures; // pictures that were replaced (widgets may still hold them)
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

        // Brings an image's key in line with its texture's current product. Returns true when the
        // texture has just arrived, so an image that had no texture now has one.
        bool Refresh(Image& entry)
        {
            foundation::texture::Texture* texture = entry.Texture.Get();
            const u64 uid = texture != nullptr && texture->View() != nullptr ? texture->Uid() : 0;
            if (uid == entry.Uid)
                return false;
            const bool arrived = entry.Uid == 0 && uid != 0;
            if (uid != 0 && (!entry.Key || entry.Key->Width() != texture->Width() || entry.Key->Height() != texture->Height()))
            {
                // A key's size is the image's natural size, so a new size needs a new key. The old
                // key is kept for the views that still hold it.
                if (entry.Key)
                {
                    Tell(entry.Key.Get(), nullptr);
                    Retired.PushBack(Move(entry.Key));
                }
                entry.Key = MakeUnique<image::ImageDataRef>(*Allocator, texture->Width(), texture->Height());
            }
            entry.Uid = uid;
            Tell(entry.Key.Get(), uid != 0 ? texture->View() : nullptr);
            return arrived;
        }

        // Parses a picture again when its vector image's product has changed.
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
        : IResourceProvider(allocator), m_state(MakeUnique<State>(allocator, allocator))
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
                IsCookedAs(*instance, GuiDocumentSource::StaticType()) || IsCookedAs(*instance, GuiStyleSheetSource::StaticType()))
                return nullptr; // not a texture (texture records come in several cooked types)
            UniquePtr<State::Image> made = MakeUnique<State::Image>(*state.Allocator);
            made->Id = id;
            made->Texture = state.Resources->Bind<foundation::texture::Texture>(id);
            entry = made.Get();
            state.Images.PushBack(Move(made));
        }
        (void)state.Refresh(*entry);
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
            if (IsCookedAs(*instance, GuiDocumentSource::StaticType()))
                handle = state.Resources->Bind(GuiDocument::StaticType(), id);
            else if (IsCookedAs(*instance, GuiStyleSheetSource::StaticType()))
                handle = state.Resources->Bind(GuiStyleSheet::StaticType(), id);
            else
                return false;
            state.Texts.PushBack(State::Text{id, Move(handle), {}});
            entry = &state.Texts.Back();
        }
        Object* product = entry->Handle->Get();
        entry->Seen = RefPtr<Object>(product);
        if (const GuiDocument* document = Cast<GuiDocument>(product))
        {
            out = document->markup;
            return true;
        }
        if (const GuiStyleSheet* sheet = Cast<GuiStyleSheet>(product))
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
            m_imagesArrived = state.Refresh(*entry) || m_imagesArrived;
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
        // A canvas's place in its scene's root, which it fills; a sheet sees it as the `canvas`
        // element. The document is laid out at the root's size, or at the reference resolution and
        // then scaled uniformly to fit, centred on a whole pixel. The host paints nothing, so it
        // never takes the pointer from the game; only what its document paints does.
        class CanvasHost final : public UIWidget
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
                    Node* child = GetChildAt(i);
                    child->SetScaleOrigin(Float2{0.0f, 0.0f});
                    child->SetScale(Float2{scale, scale});
                    child->Arrange(Rect{offset.x, offset.y, inner.x, inner.y});
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
            .Attribute("description", String(u8"Shows a GUI document in its scene, as a menu or a HUD, or draws it into a texture."))
            .DataVersion(1);
        builder.Method<&foundation::script::ComponentOf<GuiCanvasComponent>, GuiCanvasComponent>("of");
        builder.Method<&GuiCanvasComponent::rootNode, Node>("root"); // typed as Node: AngelScript binds a Variant return only typed
        builder.Property<&GuiCanvasComponent::document>("document");
        builder.Property<&GuiCanvasComponent::sheet>("sheet");
        builder.Property<&GuiCanvasComponent::order>("order");
        builder.Property<&GuiCanvasComponent::visible>("visible");
        builder.Property<&GuiCanvasComponent::interactive>("interactive");
        builder.Property<&GuiCanvasComponent::scalerMode>("scalerMode");
        builder.Property<&GuiCanvasComponent::referenceResolution>("referenceResolution");
        builder.Property<&GuiCanvasComponent::renderMode>("renderMode");
        builder.Property<&GuiCanvasComponent::renderTextureWidth>("renderTextureWidth");
        builder.Property<&GuiCanvasComponent::renderTextureHeight>("renderTextureHeight");
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
        builder.Method<&foundation::script::ComponentOf<GuiBillboardComponent>, GuiBillboardComponent>("of");
        builder.Method<&GuiBillboardComponent::rootNode, Node>("root"); // typed as Node: AngelScript binds a Variant return only typed
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


    REFLECT_ENUM(GuiCanvasRenderMode, "rtti::engine::gui")
    {
        builder.Value("ScreenOverlay", GuiCanvasRenderMode::ScreenOverlay);
        builder.Value("RenderTexture", GuiCanvasRenderMode::RenderTexture);
    }

    REFLECT_VALUE(GuiWorldPanelComponent, "rtti::engine::gui")
    {
        builder.Attribute("displayName", String(u8"GUI World Panel"))
            .Attribute("category", String(u8"GUI"))
            .Attribute("description", String(u8"Shows a GUI document on a panel in the world, in the entity's plane, optionally taking the pointer."))
            .DataVersion(1);
        builder.Method<&foundation::script::ComponentOf<GuiWorldPanelComponent>, GuiWorldPanelComponent>("of");
        builder.Method<&GuiWorldPanelComponent::rootNode, Node>("root"); // typed as Node: AngelScript binds a Variant return only typed
        builder.Property<&GuiWorldPanelComponent::document>("document");
        builder.Property<&GuiWorldPanelComponent::sheet>("sheet");
        builder.Property<&GuiWorldPanelComponent::sizeMeters>("sizeMeters");
        builder.Property<&GuiWorldPanelComponent::pixelsPerMeter>("pixelsPerMeter");
        builder.Property<&GuiWorldPanelComponent::interactive>("interactive");
        builder.Property<&GuiWorldPanelComponent::visible>("visible");
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
            RttiRegisterEnum_GuiCanvasRenderMode();
            RttiRegisterValue_GuiWorldPanelComponent();
            return true;
        }();
        (void)once;
    }

    namespace
    {
        [[nodiscard]] foundation::core::Variant NodeHandle(const RefPtr<Node>& root)
        {
            return root ? root->HandleAs(Node::StaticType()) : foundation::core::Variant{};
        }
    }
    foundation::core::Variant GuiCanvasComponent::rootNode() const { return NodeHandle(root); }
    foundation::core::Variant GuiBillboardComponent::rootNode() const { return NodeHandle(root); }
    foundation::core::Variant GuiWorldPanelComponent::rootNode() const { return NodeHandle(root); }

    void RegisterGuiScriptFacades()
    {
        static const bool once = []()
        {
            RegisterGuiComponentReflection(); // first: the components' TypeData, which includes `of`
            RegisterGuiScriptSurface();  // `gui`, the widgets, the events, the tweens
            RegisterGamekitElements();   // the screen stack, screens, prompts
            foundation::script::RegisterExtraFacadeName(u8"gui"); // the prelude imports the alias
            const TypeInfo* const components[] = {&TypeOf<GuiCanvasComponent>(), &TypeOf<GuiBillboardComponent>(),
                                                  &TypeOf<GuiWorldPanelComponent>()};
            for (const TypeInfo* component : components)
            {
                GlobalTypeRegistry().Register(*component);
                foundation::script::RegisterExtraScriptRootType(component);
                foundation::script::RegisterExtraFacadeName(StringView(reinterpret_cast<const utf8char*>(component->name)));
            }
            return true;
        }();
        (void)once;
    }

    Float2 WorldPanelPixels(const GuiWorldPanelComponent& panel) noexcept
    {
        const f32 density = Max(panel.pixelsPerMeter, 1.0f);
        const u32 width = Clamp<u32>(static_cast<u32>(panel.sizeMeters.x * density + 0.5f), 16u, 2044u);
        const u32 height = Clamp<u32>(static_cast<u32>(panel.sizeMeters.y * density + 0.5f), 16u, 2044u);
        return Float2{static_cast<f32>(width), static_cast<f32>(height)};
    }

    void GuiPointerRay(const render::ViewCamera& camera, Float2 pointer, Float2 viewSize, Float3& origin, Float3& direction) noexcept
    {
        const Float4x4 inverse = Inverse(camera.ViewProjection());
        const f32 ndcX = (pointer.x / viewSize.x) * 2.0f - 1.0f;
        const f32 ndcY = 1.0f - (pointer.y / viewSize.y) * 2.0f;
        const auto unproject = [&](f32 z)
        {
            const Float4 clip = Float4{ndcX, ndcY, z, 1.0f} * inverse;
            const f32 w = clip.w != 0.0f ? clip.w : 1.0f;
            return Float3{clip.x / w, clip.y / w, clip.z / w};
        };
        origin = unproject(projection::kNdcDepthNear);
        const Float3 far = unproject(projection::kNdcDepthFar);
        direction = Normalized(Float3{far.x - origin.x, far.y - origin.y, far.z - origin.z});
    }

    GuiPanelHit RayHitGuiPanel(Float3 origin, Float3 direction, const Float4x4& panelWorld, Float2 sizeMeters) noexcept
    {
        GuiPanelHit result;
        const Float3 centre{panelWorld.m[3][0], panelWorld.m[3][1], panelWorld.m[3][2]};
        const Float3 right = Normalized(Float3{panelWorld.m[0][0], panelWorld.m[0][1], panelWorld.m[0][2]});
        const Float3 up = Normalized(Float3{panelWorld.m[1][0], panelWorld.m[1][1], panelWorld.m[1][2]});
        const Float3 normal = Cross(right, up);
        const f32 facing = Dot(direction, normal);
        if (Abs(facing) < 1.0e-6f)
            return result; // along the plane
        const f32 t = Dot(Float3{centre.x - origin.x, centre.y - origin.y, centre.z - origin.z}, normal) / facing;
        if (t <= 0.0f)
            return result; // behind the pointer
        const Float3 local{origin.x + direction.x * t - centre.x, origin.y + direction.y * t - centre.y, origin.z + direction.z * t - centre.z};
        const f32 x = Dot(local, right);
        const f32 y = Dot(local, up);
        if (Abs(x) > sizeMeters.x * 0.5f || Abs(y) > sizeMeters.y * 0.5f)
            return result;
        result.Hit = true;
        result.Distance = t;
        result.Uv = Float2{x / sizeMeters.x + 0.5f, 0.5f - y / sizeMeters.y};
        return result;
    }

    namespace
    {
        // The mouse as a world panel sees it. Its position is the point where the ray crosses the
        // panel, in the panel's pixels; its buttons, wheel and everything else come from the real
        // mouse.
        class PanelMouse final : public foundation::shell::IMouse
        {
        public:
            PanelMouse(foundation::shell::IMouse* mouse, Float2 at) noexcept : m_mouse(mouse), m_at(at) {}
            [[nodiscard]] f32 X() const override { return m_at.x; }
            [[nodiscard]] f32 Y() const override { return m_at.y; }
            [[nodiscard]] f32 GlobalX() const override { return m_mouse->GlobalX(); }
            [[nodiscard]] f32 GlobalY() const override { return m_mouse->GlobalY(); }
            [[nodiscard]] f32 DeltaX() const override { return m_mouse->DeltaX(); }
            [[nodiscard]] f32 DeltaY() const override { return m_mouse->DeltaY(); }
            [[nodiscard]] f32 ScrollX() const override { return m_mouse->ScrollX(); }
            [[nodiscard]] f32 ScrollY() const override { return m_mouse->ScrollY(); }
            [[nodiscard]] bool IsButtonDown(foundation::shell::MouseButton button) const override { return m_mouse->IsButtonDown(button); }
            [[nodiscard]] bool IsButtonPressed(foundation::shell::MouseButton button) const override { return m_mouse->IsButtonPressed(button); }
            [[nodiscard]] bool IsButtonReleased(foundation::shell::MouseButton button) const override { return m_mouse->IsButtonReleased(button); }
            [[nodiscard]] bool RelativeMode() const override { return m_mouse->RelativeMode(); }
            void SetRelativeMode(bool enabled) override { m_mouse->SetRelativeMode(enabled); }
            [[nodiscard]] bool CursorVisible() const override { return m_mouse->CursorVisible(); }
            void SetCursorVisible(bool visible) override { m_mouse->SetCursorVisible(visible); }
            void SetCursor(foundation::shell::CursorType cursor) override { m_mouse->SetCursor(cursor); }
            void SetGlobalCapture(bool capture) override { m_mouse->SetGlobalCapture(capture); }

        private:
            foundation::shell::IMouse* m_mouse;
            Float2 m_at;
        };
    }

    // ===================================================================================
    // GuiSubsystem
    // ===================================================================================

    struct GuiSubsystem::RenderState
    {

        rhi::Device* Device = nullptr;
        VgShaderLibrary Shaders;
        UniquePtr<GuiPassRenderer> Renderer;
        // What PrepareOverlay recorded for the screen tier, and for which view.
        GuiPassRenderer::Prepared Screen{};
        GuiPassRenderer::Prepared RunScreen{}; // the drawn run's tier, beneath the shared tier
        bool HasRunScreen = false;
        render::ScreenOverlayView ScreenView{};
        bool ScreenPrepared = false;

        // A texture a canvas or a panel draws into: the sampled texture, and the multisampled
        // colour and stencil its pass resolves from. Without them, a plain pass draws straight
        // into the texture.
        struct Target
        {
            explicit Target(IAllocator&) noexcept {}
            const SceneRoot* Root = nullptr; // the root that draws into it
            scene::Scene* Scene = nullptr;
            scene::EntityHandle Entity{};
            rhi::Texture* Texture = nullptr;
            rhi::TextureView* View = nullptr;
            foundation::vg::renderer::VGPassTargets Pass;
            bool Multisampled = false;
            u32 Width = 0;
            u32 Height = 0;
            rhi::ResourceState State = rhi::ResourceState::Undefined;
            bool Seen = false;
        };
        Array<UniquePtr<Target>> Targets;

        explicit RenderState(IAllocator& allocator, bool) noexcept : Shaders(allocator), Targets(allocator) {}
        ~RenderState()
        {
            if (Device == nullptr)
                return;
            Device->WaitIdle();
            for (const UniquePtr<Target>& target : Targets)
                Release(*target);
        }
        void Release(Target& target)
        {
            target.Pass.Destroy();
            if (target.View != nullptr)
                Device->DestroyTextureView(target.View);
            if (target.Texture != nullptr)
                Device->DestroyTexture(target.Texture);
            target.View = nullptr;
            target.Texture = nullptr;
        }
        // Returns `root`'s texture at `width` x `height`, making it, or making it again at a new
        // size. Remaking waits for the GPU, because the scene may still sample the old texture.
        // Returns null when the device makes no texture.
        Target* EnsureTarget(const SceneRoot& root, scene::Scene& scene, scene::EntityHandle entity, u32 width, u32 height,
                             IAllocator& allocator)
        {
            Target* found = nullptr;
            for (const UniquePtr<Target>& target : Targets)
                if (target->Root == &root)
                    found = target.Get();
            if (found == nullptr)
            {
                UniquePtr<Target> made = MakeUnique<Target>(allocator, allocator);
                made->Root = &root;
                made->Scene = &scene;
                made->Entity = entity;
                found = made.Get();
                Targets.PushBack(Move(made));
            }
            found->Seen = true;
            if (found->Texture != nullptr && found->Width == width && found->Height == height)
                return found;
            if (found->Texture != nullptr)
            {
                Device->WaitIdle();
                Release(*found);
            }
            if (!Device->CreateTexture(rhi::TextureDesc::RenderTarget(kTextureFormat, width, height, 1, u8"GUI texture"), found->Texture).IsOk())
            {
                found->Texture = nullptr;
                return nullptr;
            }
            rhi::TextureViewDesc viewDesc{};
            viewDesc.format = kTextureFormat;
            if (!Device->CreateTextureView(found->Texture, viewDesc, found->View).IsOk())
            {
                Device->DestroyTexture(found->Texture);
                found->Texture = nullptr;
                found->View = nullptr;
                return nullptr;
            }
            found->Pass.Configure(*Device);
            found->Multisampled = found->Pass.Create(kTextureFormat, width, height);
            found->Width = width;
            found->Height = height;
            found->State = rhi::ResourceState::Undefined;
            return found;
        }

        // sRGB, as the window is. The shaders write linear light, the texture encodes it, and the
        // scene samples linear light back.
        static constexpr rhi::TextureFormat kTextureFormat = rhi::TextureFormat::RGBA8UnormSrgb;
    };

    GuiSubsystem::GuiSubsystem(IAllocator& allocator, foundation::vfs::IFileSystem& dataFileSystem)
        : m_allocator(allocator, RegisterMemoryTag("GUI")), m_dataFileSystem(&dataFileSystem), m_otherFonts(m_allocator), m_sheets(m_allocator),
          m_sceneTiers(m_allocator), m_textureRoots(m_allocator)
    {
        m_context = MakeRef<GUIContext>(m_allocator);
        m_resources = MakeUnique<GuiResourceProvider>(m_allocator, m_allocator);
        m_context->SetResourceProvider(m_resources.Get());
        m_styles.SetResourceProvider(m_resources.Get()); // the sheets' @import, url() and fonts
    }
    GuiSubsystem::~GuiSubsystem() = default;

    const foundation::input::ActionRuntime* GuiSubsystem::PromptRuntime() const noexcept
    {
        return m_promptRuntime != nullptr ? m_promptRuntime : (m_input != nullptr ? &m_input->Runtime() : nullptr);
    }

    // Points the prompts at the runtime they should read, and tells them again when its bindings
    // may have changed.
    void GuiSubsystem::SyncPrompts()
    {
        const foundation::input::ActionRuntime* runtime = PromptRuntime();
        if (runtime != m_promptsRead)
        {
            m_promptsRead = runtime;
            m_promptsVersion = runtime != nullptr ? runtime->BindingsVersion() : 0;
            m_prompts.SetRuntime(runtime); // tells them
            return;
        }
        if (runtime != nullptr && runtime->BindingsVersion() != m_promptsVersion)
        {
            m_promptsVersion = runtime->BindingsVersion();
            m_prompts.NotifyBindingsChanged();
        }
    }

    foundation::fonts::IFontService* GuiSubsystem::Fonts() const noexcept { return m_projectFonts ? m_projectFonts.Get() : m_fonts.Get(); }

    void GuiSubsystem::SetFonts(const foundation::fonts::Font* defaultFont, Span<const foundation::fonts::Font* const> others)
    {
        // The editor sets the fonts again after every cook, mostly to the same ones. Unchanged
        // fonts keep the service they are in, and every label keeps what it was styled with.
        usize given = 0;
        bool same = m_projectFonts && m_defaultFont.Get() == defaultFont;
        for (const foundation::fonts::Font* font : others)
            if (font != nullptr)
            {
                same = same && given < m_otherFonts.Size() && m_otherFonts[given].Get() == font;
                ++given;
            }
        if (same && given == m_otherFonts.Size())
            return;
        m_defaultFont = RefPtr<foundation::fonts::Font>(const_cast<foundation::fonts::Font*>(defaultFont));
        m_otherFonts.Clear();
        for (const foundation::fonts::Font* font : others)
            if (font != nullptr)
                m_otherFonts.PushBack(RefPtr<foundation::fonts::Font>(const_cast<foundation::fonts::Font*>(font)));
        if (m_projectFonts)
        {
            m_retiredFonts.PushBack(Move(m_projectFonts)); // labels styled with it still hold its fonts
        }
        m_projectFonts = nullptr;
        if (m_defaultFont)
        {
            UniquePtr<foundation::fonts::ResourceFontService> service = MakeUnique<foundation::fonts::ResourceFontService>(m_allocator, m_allocator);
            service->AddFont(m_defaultFont.Get());
            service->SetDefaultFamily(m_defaultFont->Family());
            for (const RefPtr<foundation::fonts::Font>& font : m_otherFonts)
                service->AddFont(font.Get());
            m_projectFonts = Move(service);
        }
        else if (!m_otherFonts.IsEmpty())
            LOG_WARNING(u8"GUI", u8"{} GUI font(s) wait for a default font", static_cast<u64>(m_otherFonts.Size()));
        // From here on, everything that measures or draws text asks the new service, and every
        // root measures again.
        foundation::fonts::IFontService* fonts = Fonts();
        m_context->SetFontService(fonts);
        m_styles.SetFontService(fonts);
        ApplyUserAgentSheet(); // the default family now comes from the new service
        if (m_render && m_render->Renderer)
            m_render->Renderer->SetFontService(fonts);
        if (m_screen.Root)
            m_screen.Root->RequestLayoutTree();
        for (const UniquePtr<ScreenTier>& tier : m_runTiers)
            tier->Root->RequestLayoutTree();
        for (const SceneTier& tier : m_sceneTiers)
            tier.Root->RequestLayoutTree();
        for (const UniquePtr<TextureRoot>& root : m_textureRoots)
            root->Root->RequestLayoutTree();
    }

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
        RegisterGuiElements();     // the GUI's widgets, by tag from markup
        RegisterGamekitElements(); // a game's screens and prompts
        // Installed before any font loads, because a font gets its shaper and its atlas's glyphs as
        // it loads.
        foundation::fonts::HarfBuzzShaping::Install();
        foundation::fonts::BidiAlgorithm::Install();
        UniquePtr<foundation::fonts::TrueTypeFontService> fonts = MakeUnique<foundation::fonts::TrueTypeFontService>(m_allocator, m_allocator);
        // The built-in font comes through the data mount (present in a dev tree and in any dist
        // that stages it). The project's own fonts come with the player's settings.
        constexpr StringView kDefaultFont = u8"Engine/Fonts/roboto/Roboto-Regular.ttf";
        Array<u8> bytes(m_allocator);
        if (ReadDataFile(kDefaultFont, bytes) &&
            fonts->LoadFontFromMemory(u8"Roboto", Span<const u8>(bytes.Data(), bytes.Size())) == foundation::fonts::FontLoadResult::Success)
            fonts->SetDefaultFamily(u8"Roboto");
        else
            LOG_INFO(u8"GUI", u8"built-in font '{}' not in the data root: GUI text needs the project's fonts", kDefaultFont);
        m_fonts = Move(fonts);
        m_context->SetFontService(m_fonts.Get());
        m_styles.SetFontService(m_fonts.Get());
        ApplyUserAgentSheet();
        ApplySheets();

        // The shared screen tier: the screen stack, and the overlay layer above it, each filling the
        // root. Neither paints, so neither takes the pointer from the game until something in it
        // does.
        MakeScreenTier(m_screen);
        m_overlay = MakeRef<UIWidget>(m_allocator);
        m_overlay->SetTag(StringView(u8"overlays"));
        m_overlay->SetHitTestVisible(false); // over the screens; only its contents take the pointer
        {
            Node::StyleWriteScope defaults(*m_overlay);
            m_overlay->EditLayoutStyle([](LayoutStyle& s) { s.Kind = Display::Stack; });
        }
        m_screen.Root->AddChild(m_overlay.Get());
        // The touch controls' guide, beneath the screens pushed on this tier and the overlays.
        m_touchGuide = MakeRef<GuiTouchGuide>(m_allocator);
        m_screen.Root->AddChildAt(m_touchGuide.Get(), 0);
        // A script's root is its run's screen tier (the shared tier in the player).
        m_scriptBinding.resolve = [this]() { return ScreenRootFor(foundation::script::CurrentRun()); };
    }

    // Makes a screen tier's root, filled by its stack, and the tier's input. Scripts find the stack
    // by its id.
    void GuiSubsystem::MakeScreenTier(ScreenTier& tier)
    {
        tier.Root = MakeRef<SceneRoot>(m_allocator, m_context);
        {
            Node::StyleWriteScope defaults(*tier.Root);
            tier.Root->EditLayoutStyle([](LayoutStyle& s) { s.Kind = Display::Stack; });
        }
        tier.Screens = MakeRef<ScreenStack>(m_allocator);
        tier.Screens->SetId(StringView(u8"screens")); // a script's ScreenStack::of(gui::root().find("screens"))
        tier.Root->AddChild(tier.Screens.Get());
        tier.Bridge = MakeBridge(*tier.Root);
    }

    RefPtr<Node> GuiSubsystem::PushOverlay(const GuiDocument& document)
    {
        RefPtr<Node> tree = BuildDocument(document);
        if (tree)
            PushOverlay(*tree);
        return tree;
    }

    void GuiSubsystem::PushOverlay(Node& overlay)
    {
        if (m_overlay && overlay.GetParent() == nullptr)
            m_overlay->AddChild(&overlay);
    }

    void GuiSubsystem::RemoveOverlay(Node& overlay)
    {
        if (m_overlay && overlay.GetParent() == m_overlay.Get())
            m_overlay->RemoveChild(&overlay);
    }

    void GuiSubsystem::ExposeToScript(foundation::script::IScriptContext& scriptContext) { InstallGuiScriptService(scriptContext, m_scriptBinding); }

    void GuiSubsystem::OnReady()
    {
        foundation::runtime::SubsystemManager* context = Manager();
        if (context == nullptr)
            return;
        m_input = context->GetSubsystem<engine::input::InputSubsystem>();
        // Follows the runs' lifecycle, so a run's tier goes away with its session.
        if ((m_runManager = context->RunManager()) != nullptr)
            m_runManager->Subscribe(*this);
        m_prompts.Install(*m_context); // every prompt in the game's GUI names its bindings through this
        SyncPrompts();
        if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
        {
            scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
            scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
        }
        // Headless contexts (tests, the cooker) have no render subsystem, so nothing registers.
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
        if (m_runManager != nullptr)
        {
            m_runManager->Unsubscribe(*this);
            m_runManager = nullptr;
        }
        if (m_sceneRenderer != nullptr)
            m_sceneRenderer->UnregisterOverlay(static_cast<render::ISceneOverlay*>(this));
        if (m_screenRenderer != nullptr)
            m_screenRenderer->UnregisterOverlay(static_cast<render::IScreenOverlay*>(this));
        m_sceneRenderer = nullptr;
        m_screenRenderer = nullptr;
        if (foundation::runtime::SubsystemManager* context = Manager())
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
                scenes->UnregisterObserver(this);
        m_activeRoot = nullptr;
        m_pressRoot = nullptr;
        InputPrompts::Uninstall(*m_context);
        m_prompts.SetRuntime(nullptr);
        m_promptsRead = nullptr;
        m_input = nullptr;
        m_sceneTiers.Clear();
        m_textureRoots.Clear();
        for (const UniquePtr<ScreenTier>& tier : m_runTiers)
            tier->Screens->Clear();
        m_runTiers.Clear();
        if (m_screen.Screens)
            m_screen.Screens->Clear();
        m_overlay = nullptr;
        m_scriptBinding.resolve = nullptr; // a script still running then finds no root, rather than a freed one
        m_screen.Bridge = nullptr;
        m_screen.Screens = nullptr;
        m_screen.Root = nullptr;
        m_resources->SetTextureSink(nullptr);
        m_context->SetPictureBaker(nullptr);
        m_render = nullptr; // waits for the GPU, then releases the renderers
    }

    void GuiSubsystem::BeginFrame(f32 deltaTime)
    {
        PROFILE_SCOPE("GUI.BeginFrame");
        ++m_frameSerial;
        if (m_render && m_render->Renderer)
            m_render->Renderer->BeginFrame();
        if (m_resources->Poll())
            ApplySheets(); // a sheet or a document was cooked again, so the GUI restyles
        if (m_resources->TakeImagesArrived())
        {
            // A texture that an image asked for before it loaded has arrived. Every root lays out
            // again, and the images that got nothing ask again.
            if (m_screen.Root)
                m_screen.Root->RequestLayoutTree();
            for (const UniquePtr<ScreenTier>& tier : m_runTiers)
                tier->Root->RequestLayoutTree();
            for (const SceneTier& tier : m_sceneTiers)
                tier.Root->RequestLayoutTree();
            for (const UniquePtr<TextureRoot>& root : m_textureRoots)
                root->Root->RequestLayoutTree();
        }
        SyncPrompts();
        for (const UniquePtr<TextureRoot>& root : m_textureRoots)
            root->Seen = false;
        for (SceneTier& tier : m_sceneTiers)
        {
            SyncBillboards(tier);
            SyncCanvases(tier);
            SyncPanels(tier);
        }
        // Drops the roots of textures whose canvas or panel has gone. The textures themselves go
        // the next time RenderTextures runs.
        for (usize i = m_textureRoots.Size(); i-- > 0;)
            if (!m_textureRoots[i]->Seen)
            {
                if (m_activeRoot == m_textureRoots[i]->Root.Get())
                    m_activeRoot = nullptr;
                if (m_pressRoot == m_textureRoots[i]->Root.Get())
                    m_pressRoot = nullptr;
                m_textureRoots.RemoveAt(i);
            }
        PumpInput();
        // Unscaled time, so menus still move while the game is paused.
        m_context->Update(Duration::FromSeconds(static_cast<f64>(deltaTime)));
        if (m_screen.Root)
            StyleAndLayOut(*m_screen.Root, m_screen.Root->GetViewportSize());
        for (const UniquePtr<ScreenTier>& tier : m_runTiers)
            StyleAndLayOut(*tier->Root, tier->Root->GetViewportSize());
    }

    void GuiSubsystem::SetResourceManager(resource::ResourceManager* resources)
    {
        m_resources->SetResourceManager(resources);
        ApplySheets(); // the sheets now resolve in the new project, or not at all
    }

    void GuiSubsystem::SetStyleSheets(Span<const Guid> sheets)
    {
        m_sheets.Clear();
        for (const Guid& sheet : sheets)
            if (!sheet.IsNil())
                m_sheets.PushBack(sheet);
        ApplySheets();
    }

    // The user agent's sheet: the built-in theme, and the text of every tree in the default family
    // at the body size. The style applier gives text a font only where a family is declared, and a
    // root is not a text node that could hold one. So each tree's top elements (:root) declare the
    // family, the rest inherit it, and the game's sheets, which sit above the user agent, may choose
    // another.
    void GuiSubsystem::ApplyUserAgentSheet()
    {
        String css(DefaultThemeCSS(), m_allocator);
        if (foundation::fonts::IFontService* fonts = Fonts(); fonts != nullptr && !fonts->DefaultFontFamily().IsEmpty())
        {
            css.Append(StringView(u8"\n:root { font-family: \""));
            css.Append(fonts->DefaultFontFamily());
            css.Append(StringView(u8"\"; font-size: var(--font-body); }\n"));
        }
        m_styles.SetUserAgentSheet(CSSParser::Parse(css.AsView(), StringView(u8"theme")));
    }

    // Applies the game's sheets as one author sheet that imports each of them in order. The style
    // manager expands the imports through the provider, each over the ones before it.
    void GuiSubsystem::ApplySheets()
    {
        String css(m_allocator);
        for (const Guid& sheet : m_sheets)
        {
            css.Append(StringView(u8"@import \"{"));
            css.Append(Format(u8"{}", sheet).AsView());
            css.Append(StringView(u8"}\";\n"));
        }
        m_styles.SetStyleSheet(CSSParser::Parse(css.AsView(), StringView(u8"game")));
        ++m_sheetGeneration;
    }

    void GuiSubsystem::StyleAndLayOut(SceneRoot& root, Float2 size)
    {
        if (root.GetViewportSize() != size)
            root.SetViewportSize(size);
        m_styles.SetMediaContext(root.GetMediaContext());
        m_styles.ApplyTree(root);
        root.UpdateLayout();
    }

    SceneRoot* GuiSubsystem::SceneRootFor(const scene::Scene& scene) noexcept
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
        tier.Root = MakeRef<SceneRoot>(m_allocator, m_context);
        {
            Node::StyleWriteScope defaults(*tier.Root);
            tier.Root->EditLayoutStyle([](LayoutStyle& s) { s.Kind = Display::Stack; }); // each canvas fills it
        }
        // The billboards' layer, under every canvas. It paints nothing, so it takes no pointer.
        tier.Billboards = MakeRef<UIWidget>(m_allocator);
        tier.Billboards->SetTag(StringView(u8"billboards"));
        tier.Root->AddChild(tier.Billboards.Get());
        tier.Bridge = MakeBridge(*tier.Root);
        m_sceneTiers.PushBack(Move(tier));
    }

    void GuiSubsystem::OnDestroying(scene::Scene& scene)
    {
        for (usize i = m_textureRoots.Size(); i-- > 0;) // the roots that the scene's canvases and panels draw into
            if (m_textureRoots[i]->Scene == &scene)
            {
                if (m_activeRoot == m_textureRoots[i]->Root.Get())
                    m_activeRoot = nullptr;
                if (m_pressRoot == m_textureRoots[i]->Root.Get())
                    m_pressRoot = nullptr;
                m_textureRoots.RemoveAt(i);
            }
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

    RefPtr<Node> GuiSubsystem::BuildDocument(const GuiDocument& document)
    {
        if (document.markup.IsEmpty())
            return {};
        MarkupLoader loader(m_context->GetElementSet(), m_allocator, m_resources.Get(), Fonts());
        loader.SetDataModel(m_context->GetDataRef());
        RefPtr<Node> root = loader.LoadFromString(document.markup.AsView());
        if (!root)
            LOG_WARNING(u8"GUI", u8"a component's document did not build");
        return root;
    }

    // Builds a component's tree again when its document's product has changed (a re-cook, or
    // another document), removing the old tree from wherever it was. Returns true when it rebuilt.
    template <typename Component>
    bool GuiSubsystem::RebuildIfChanged(Component& c)
    {
        GuiDocument* document = c.document.Get();
        if (document == c.builtFrom.Get())
            return false;
        if (c.root && c.root->GetParent() != nullptr)
            c.root->GetParent()->RemoveChild(c.root.Get());
        c.root = document != nullptr ? BuildDocument(*document) : RefPtr<Node>{};
        c.sheetFrom = nullptr;
        c.builtFrom = RefPtr<Object>(document);
        return true;
    }

    // Applies a component's own sheet to its tree (which is in a GUI by now), and applies it again
    // when the sheet's product has changed.
    template <typename Component>
    void GuiSubsystem::ApplySheet(Component& c, StringView name)
    {
        GuiStyleSheet* sheet = c.sheet.Get();
        if (!c.root || sheet == c.sheetFrom.Get())
            return;
        if (sheet != nullptr)
            (void)m_styles.SetSubtreeSheet(*c.root, CSSParser::Parse(sheet->css.AsView(), name));
        else
            m_styles.RemoveSubtreeSheet(*c.root);
        c.sheetFrom = RefPtr<Object>(sheet);
    }

    // Returns the root that a texture's content (a render-texture canvas, a world panel) is drawn
    // in. The root is made for the entity the first time, holds `content` alone, and is kept as
    // long as it is seen each frame.
    RefPtr<SceneRoot> GuiSubsystem::TextureRootFor(scene::Scene& scene, scene::EntityHandle entity, Node& content)
    {
        TextureRoot* found = nullptr;
        for (const UniquePtr<TextureRoot>& known : m_textureRoots)
            if (known->Scene == &scene && known->Entity == entity && !known->Seen)
                found = known.Get();
        if (found == nullptr)
        {
            UniquePtr<TextureRoot> made = MakeUnique<TextureRoot>(m_allocator);
            made->Scene = &scene;
            made->Entity = entity;
            made->Root = MakeRef<SceneRoot>(m_allocator, m_context);
            {
                Node::StyleWriteScope defaults(*made->Root);
                made->Root->EditLayoutStyle([](LayoutStyle& s) { s.Kind = Display::Stack; }); // the content fills it
            }
            made->Bridge = MakeBridge(*made->Root);
            found = made.Get();
            m_textureRoots.PushBack(Move(made));
        }
        found->Seen = true;
        if (content.GetParent() != found->Root.Get())
        {
            if (content.GetParent() != nullptr)
                content.GetParent()->RemoveChild(&content);
            while (found->Root->ChildCount() > 0)
                found->Root->RemoveChild(found->Root->GetChildAt(0));
            found->Root->AddChild(&content);
        }
        return found->Root;
    }

    // Brings each canvas's tree in line with its component. The tree is built from the document
    // (and again when the document is re-cooked) and placed in the canvas's host in the scene's
    // root, or in a root of its own for a texture. Its sheet is applied, and it is shown while the
    // canvas and its entity are visible. Then the hosts of canvases that have gone are removed,
    // and the remaining hosts are ordered.
    void GuiSubsystem::SyncCanvases(SceneTier& tier)
    {
        auto* canvases = tier.Scene->GetSystem<GuiCanvasComponentManager>();
        Array<const Node*> seen(m_allocator);
        if (canvases != nullptr)
            canvases->ForEach(
                [&](GuiCanvasComponent& c, scene::EntityHandle entity)
                {
                    const bool wantsTexture = c.renderMode == GuiCanvasRenderMode::RenderTexture;
                    if (!RebuildIfChanged(c) && c.root && wantsTexture != (c.renderRoot.Get() != nullptr))
                    {
                        // Its mode changed: take the tree out of where it was. Its sheet is
                        // applied afresh where it goes.
                        if (c.root->GetParent() != nullptr)
                            c.root->GetParent()->RemoveChild(c.root.Get());
                        c.sheetFrom = nullptr;
                    }
                    if (wantsTexture)
                    {
                        c.host = nullptr; // not seen, so removed below
                        c.renderRoot = c.root ? TextureRootFor(*tier.Scene, entity, *c.root) : RefPtr<SceneRoot>{};
                    }
                    else
                    {
                        c.renderRoot = nullptr;
                        c.renderTexture = nullptr;
                        c.renderTextureView = nullptr;
                        if (!c.host)
                        {
                            c.host = MakeRef<CanvasHost>(m_allocator);
                            tier.Root->AddChild(c.host.Get());
                            tier.Canvases.PushBack(c.host);
                        }
                        auto* host = static_cast<CanvasHost*>(c.host.Get());
                        host->Configure(c.order, c.scalerMode, c.referenceResolution);
                        seen.PushBack(host);
                        if (c.root && c.root->GetParent() == nullptr)
                            host->AddChild(c.root.Get());
                    }
                    if (!c.root)
                        return;
                    ApplySheet(c, StringView(u8"canvas"));
                    c.root->SetDisplayed(c.visible && tier.Scene->IsEffectivelyActive(entity));
                    c.root->SetInert(!c.interactive);
                });
        // Removes the hosts whose canvas has gone (despawned, its component removed, or now drawn
        // into a texture), then orders the rest stably by `order`, after whatever else the root
        // holds.
        Array<CanvasHost*> hosts(m_allocator);
        for (usize i = 0; i < tier.Canvases.Size();)
        {
            CanvasHost* host = static_cast<CanvasHost*>(tier.Canvases[i].Get());
            bool live = false;
            for (const Node* known : seen)
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
        // Gather the hosts in the root's order, for the stable sort.
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

    // Brings each billboard's tree in line with its component, in the scene's billboard layer.
    // The tree is rebuilt when its document changes, its sheet is applied, and it is shown while
    // the billboard and its entity are visible. Trees of billboards that have gone are removed.
    // Each view decides where a billboard shows (PlaceBillboards).
    void GuiSubsystem::SyncBillboards(SceneTier& tier)
    {
        auto* billboards = tier.Scene->GetSystem<GuiBillboardComponentManager>();
        Array<const Node*> seen(m_allocator);
        if (billboards != nullptr)
            billboards->ForEach(
                [&](GuiBillboardComponent& c, scene::EntityHandle entity)
                {
                    (void)RebuildIfChanged(c);
                    if (!c.root)
                        return;
                    if (c.root->GetParent() == nullptr)
                    {
                        c.root->EditLayoutStyle([](LayoutStyle& s) { s.Placement = Position::Absolute; });
                        tier.Billboards->AddChild(c.root.Get());
                    }
                    seen.PushBack(c.root.Get());
                    ApplySheet(c, StringView(u8"billboard"));
                    c.root->SetDisplayed(c.visible && tier.Scene->IsEffectivelyActive(entity));
                });
        for (usize i = tier.Billboards->ChildCount(); i-- > 0;)
        {
            Node* child = tier.Billboards->GetChildAt(i);
            bool live = false;
            for (const Node* known : seen)
                live = live || known == child;
            if (!live)
                tier.Billboards->RemoveChild(child);
        }
    }

    // Brings each world panel's tree in line with its component, in a root of its own for its
    // texture.
    void GuiSubsystem::SyncPanels(SceneTier& tier)
    {
        auto* panels = tier.Scene->GetSystem<GuiWorldPanelComponentManager>();
        if (panels == nullptr)
            return;
        panels->ForEach(
            [&](GuiWorldPanelComponent& c, scene::EntityHandle entity)
            {
                (void)RebuildIfChanged(c);
                c.renderRoot = c.root ? TextureRootFor(*tier.Scene, entity, *c.root) : RefPtr<SceneRoot>{};
                if (!c.root)
                {
                    c.renderTexture = nullptr;
                    c.renderTextureView = nullptr;
                    return;
                }
                ApplySheet(c, StringView(u8"panel"));
                c.root->SetDisplayed(c.visible && tier.Scene->IsEffectivelyActive(entity));
                c.root->SetInert(!c.interactive);
            });
    }

    // Places each shown billboard for `view`. A billboard hangs by its bottom centre from its
    // projected point, using its size from the last layout (a new size settles the next frame). It
    // is scaled toward that point and hidden when the point is behind the camera. Its layout is
    // kept, so nothing else moves.
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
                    [corner](LayoutStyle& s)
                    {
                        s.Left = LayoutLength::Px(corner.x);
                        s.Top = LayoutLength::Px(corner.y);
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

    UniquePtr<GuiInputBridge> GuiSubsystem::MakeBridge(SceneRoot& root)
    {
        UniquePtr<GuiInputBridge> bridge = MakeUnique<GuiInputBridge>(m_allocator, root.GetEventDispatcher());
        bridge->SetInputKindTracker(&m_inputKind); // the pointer, the keys or a pad
        bridge->SetTextInputTarget(m_textInputTarget);
        return bridge;
    }

    void GuiSubsystem::SetTextInputTarget(foundation::shell::IWindow* window)
    {
        m_textInputTarget = window;
        if (m_screen.Bridge)
            m_screen.Bridge->SetTextInputTarget(window);
        for (const UniquePtr<ScreenTier>& tier : m_runTiers)
            tier->Bridge->SetTextInputTarget(window);
        for (const SceneTier& tier : m_sceneTiers)
            tier.Bridge->SetTextInputTarget(window);
    }

    GuiInputBridge* GuiSubsystem::BridgeOf(const SceneRoot* root) noexcept
    {
        if (ScreenTier* tier = TierOfRoot(root))
            return tier->Bridge.Get();
        for (const SceneTier& tier : m_sceneTiers)
            if (tier.Root.Get() == root)
                return tier.Bridge.Get();
        for (const UniquePtr<TextureRoot>& texture : m_textureRoots)
            if (texture->Root.Get() == root)
                return texture->Bridge.Get();
        return nullptr;
    }

    // Finds the nearest interactive world panel the pointer's ray crosses, and the point in the
    // panel's pixels. The ray is cast through each reachable scene's camera at the shape its GUI
    // last drew. When the mouse is captured, the ray goes through the view's centre, because the
    // crosshair is the pointer. With no panel hit, the target has no root.
    GuiSubsystem::InputTarget GuiSubsystem::PanelUnderPointer(Float2 point, bool centreAim)
    {
        InputTarget found;
        f32 nearest = 0.0f;
        for (const SceneTier& tier : m_sceneTiers)
        {
            auto* panels = tier.Scene->GetSystem<GuiWorldPanelComponentManager>();
            if (panels == nullptr || !InputReaches(tier))
                continue;
            const Float2 viewSize = tier.Root->GetViewportSize();
            if (viewSize.x <= 0.0f || viewSize.y <= 0.0f)
                continue; // not drawn yet, so there is no shape to cast through
            render::ViewCamera camera;
            if (!engine::render::ExtractPrimaryCamera(*tier.Scene, camera, nullptr, viewSize.x / viewSize.y))
                continue;
            Float3 origin;
            Float3 direction;
            GuiPointerRay(camera, centreAim ? Float2{viewSize.x * 0.5f, viewSize.y * 0.5f} : point, viewSize, origin, direction);
            panels->ForEach(
                [&](GuiWorldPanelComponent& c, scene::EntityHandle entity)
                {
                    if (!c.interactive || !c.visible || !c.renderRoot || !tier.Scene->IsEffectivelyActive(entity))
                        return;
                    const GuiPanelHit hit = RayHitGuiPanel(origin, direction, tier.Scene->GetWorldMatrix(entity), c.sizeMeters);
                    if (!hit.Hit || (found.Root != nullptr && hit.Distance >= nearest))
                        return;
                    const Float2 pixels = WorldPanelPixels(c);
                    found = InputTarget{c.renderRoot.Get(), Float2{hit.Uv.x * pixels.x, hit.Uv.y * pixels.y}, true, true};
                    nearest = hit.Distance;
                });
        }
        return found;
    }

    // Whether this frame's input may reach `tier`'s scene. When the source is bound to a scene (an
    // editor Game tab), only that scene; otherwise every scene or none, as the input subsystem's
    // policy says.
    bool GuiSubsystem::InputReaches(const SceneTier& tier) const noexcept
    {
        if (m_input == nullptr)
            return false;
        if (const void* bound = m_input->BoundSceneKey(); bound != nullptr)
            return static_cast<const void*>(tier.Scene) == bound;
        return m_input->UnboundScenePolicy() == engine::input::UnboundInputScenePolicy::AllScenes;
    }

    GuiSubsystem::InputTarget GuiSubsystem::PickInputRoot(bool pointer, Float2 point, bool centreAim)
    {
        if (!m_screen.Root)
            return {};
        // When the shared tier's overlay layer is occupied (a system menu, a loading screen), it is
        // modal. A passive overlay (a watermark, `pointer-events: none`) draws over everything and
        // takes no input.
        for (usize i = 0; m_overlay && i < m_overlay->ChildCount(); ++i)
            if (m_overlay->GetChildAt(i)->IsDisplayed() && m_overlay->GetChildAt(i)->IsHitTestVisible())
                return InputTarget{m_screen.Root.Get(), ScreenPointerPoint(m_screen, point), m_screen.HasResolution(), false};
        // The screen tier input reaches (the bound run's tier), with the pointer in its own pixels.
        ScreenTier& tier = InputTier();
        const InputTarget screen{tier.Root.Get(), ScreenPointerPoint(tier, point), tier.HasResolution(), false};
        // The GUI under the pointer: the screen tier first, then a scene's GUI, then a world panel.
        if (pointer)
        {
            if (tier.Root->GetEventDispatcher()->IsPointerOver(screen.Point))
                return screen;
            for (const SceneTier& scene : m_sceneTiers)
                if (InputReaches(scene) && scene.Root->GetEventDispatcher()->IsPointerOver(point))
                    return InputTarget{scene.Root.Get(), point, false, false};
            if (const InputTarget panel = PanelUnderPointer(point, centreAim); panel.Root != nullptr)
                return panel;
        }
        // The keys and a pad go to the screens a game pushed, or else to a scene's GUI.
        if (tier.Screens->Count() > 0)
            return screen;
        for (const SceneTier& scene : m_sceneTiers)
            if (InputReaches(scene) && HasContent(scene))
                return InputTarget{scene.Root.Get(), point, false, false};
        return screen;
    }

    void GuiSubsystem::PumpInput()
    {
        m_pointerTaken = false;
        if (m_input == nullptr || !m_screen.Root)
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
        // From the first touch on, the touch guide shows the controls of the sets in effect.
        m_touchSeen = m_touchSeen || fingers;
        if (m_touchGuide)
            m_touchGuide->Update(*m_input, m_touchSeen);
        const bool held = fingers || (mouse != nullptr && (mouse->IsButtonDown(foundation::shell::MouseButton::Left) ||
                                                           mouse->IsButtonDown(foundation::shell::MouseButton::Right) ||
                                                           mouse->IsButtonDown(foundation::shell::MouseButton::Middle)));

        // Input goes to one root a frame. A held press stays with the root it began in, so that its
        // release lands there. On a panel, the release lands where the ray still crosses it, or else
        // where it last did.
        const bool centreAim = mouse != nullptr && mouse->RelativeMode();
        InputTarget target = PickInputRoot(pointer, point, centreAim);
        if (held && m_pressRoot != nullptr && target.Root != m_pressRoot)
        {
            bool panel = false;
            for (const UniquePtr<TextureRoot>& texture : m_textureRoots)
                panel = panel || texture->Root.Get() == m_pressRoot;
            if (panel)
                target = InputTarget{m_pressRoot, m_panelPoint, true, true};
            else if (ScreenTier* tier = TierOfRoot(m_pressRoot))
                target = InputTarget{m_pressRoot, ScreenPointerPoint(*tier, point), tier->HasResolution(), false};
            else
                target = InputTarget{m_pressRoot, point, false, false};
        }
        if (target.Panel)
            m_panelPoint = target.Point;
        SceneRoot* root = target.Root;
        if (root != m_activeRoot && m_activeRoot != nullptr && pointer)
        {
            // Input is leaving this root, so move the pointer away and nothing in it stays hovered.
            (void)m_activeRoot->GetEventDispatcher()->InjectMouseMove(Float2{-1.0e6f, -1.0e6f});
        }
        m_activeRoot = root;
        GuiInputBridge* bridge = BridgeOf(root);
        if (bridge == nullptr)
            return;
        if (held && m_pressRoot == nullptr)
        {
            m_pressRoot = root; // a press begins; where it began decides whether the GUI took it
            m_pressTaken = target.Panel || root->GetEventDispatcher()->IsPointerOver(target.Point);
        }
        if (target.Mapped)
        {
            // A panel's pointer is where the ray crosses it. A fitted screen tier's pointer is where
            // the render-space pointer falls in its pixels. Fingers arrive as the mouse, since SDL
            // copies them to it.
            PanelMouse mapped(mouse, target.Point);
            bridge->PumpDevices(mouse != nullptr ? &mapped : nullptr, nullptr, devices.Keyboard());
        }
        else
            bridge->PumpDevices(mouse, touch, devices.Keyboard());
        // Keys, text and the pad go through their events, in order; the pointer was polled above.
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
        bridge->SyncInputKind(); // a pad or keys held with no event this frame still count

        // Tell the action layer what the GUI took: the pointer while it is over GUI that takes it,
        // or while a press that began there is held; the keys while text is being edited. These are
        // added to any claim made earlier in this lane, and EndFrame clears them after the frame has
        // read them.
        m_pointerTaken = pointer && (held ? m_pressTaken : target.Panel || root->GetEventDispatcher()->IsPointerOver(target.Point));
        const bool keys = root->GetEventDispatcher()->WantsTextInput();
        m_textWanted = keys;
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
        m_render = MakeUnique<RenderState>(m_allocator, m_allocator, true);
        m_render->Device = &device;
        if (!m_render->Shaders.Load(device, *m_dataFileSystem))
        {
            LOG_ERROR(u8"GUI", u8"no shader compiler and no shader pack: the GUI will not draw");
            return;
        }
        m_render->Renderer = MakeUnique<GuiPassRenderer>(m_allocator, m_allocator, device, static_cast<u32>(frameCount < 1 ? 1 : frameCount),
                                                              Fonts(), m_render->Shaders.Set());
        m_context->SetPictureBaker(m_render->Renderer->Baker());
        GuiPassRenderer* renderer = m_render->Renderer.Get();
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
        SceneRoot* root = nullptr;
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
        // Laid out at the viewport's size and drawn there; each half of a split screen lays out at
        // its own size.
        StyleAndLayOut(*root, Float2{static_cast<f32>(view.viewportWidth), static_cast<f32>(view.viewportHeight)});
        GuiPassView pass;
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
        [[nodiscard]] GuiPassView ScreenPass(const render::ScreenOverlayView& view) noexcept
        {
            GuiPassView pass;
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

    // Places a screen tier on `view`'s target and records it, with its layers on `encoder` when one
    // is given. The tier is laid out at the game's resolution and scale and drawn into the fitted
    // rectangle, rotated with the game's image, or else drawn at the target's own size over all of
    // it.
    static GuiPassRenderer::Prepared PrepareScreenTier(GuiPassRenderer& renderer, SceneRoot& root,
                                                            const GuiScreenPlacement& place, Float2 target,
                                                            const render::ScreenOverlayView& view, rhi::CommandEncoder* encoder,
                                                            const Function<void(SceneRoot&, Float2)>& layOut)
    {
        GuiPassView pass = ScreenPass(view);
        Float2 size = target;
        f32 scale = 1.0f;
        if (place.Fitted)
        {
            pass.X = static_cast<i32>(place.Rect.x);
            pass.Y = static_cast<i32>(place.Rect.y);
            pass.Width = static_cast<u32>(Max(place.Rect.width, 1.0f));
            pass.Height = static_cast<u32>(Max(place.Rect.height, 1.0f));
            pass.Transform = place.Transform;
            size = place.DrawSize;
            scale = place.DpiScale;
        }
        if (root.GetDisplayScale() != scale)
            root.SetDisplayScale(scale);
        layOut(root, size);
        return encoder != nullptr ? renderer.Prepare(root, pass, encoder) : renderer.Prepare(root, pass);
    }

    void GuiSubsystem::PrepareOverlay(rhi::CommandEncoder& encoder, const render::ScreenOverlayView& view)
    {
        if (!IsRenderReady())
            return;
        m_render->ScreenPrepared = false;
        // Record the bakes, once a frame, before any pass samples them.
        (void)m_render->Renderer->RecordBakes(encoder, view.frameIndex);
        if (!m_screen.Root || view.width == 0 || view.height == 0)
            return;
        const Float2 target{static_cast<f32>(view.width), static_cast<f32>(view.height)};
        const Function<void(SceneRoot&, Float2)> layOut = [this](SceneRoot& root, Float2 size) { StyleAndLayOut(root, size); };
        const auto prepare = [&](ScreenTier& tier)
        {
            tier.TargetSize = target;
            const GuiScreenPlacement place = PlaceScreen(tier.Resolution, tier.Fit, tier.Orientation, target);
            tier.Turn = place.Turn;
            return PrepareScreenTier(*m_render->Renderer, *tier.Root, place, target, view, &encoder, layOut);
        };
        // Draw the drawn run's own screens (its Game tab) first, then the shared tier above them.
        ScreenTier* run = m_runScreens ? FindRunTier(m_renderRun) : nullptr;
        m_render->HasRunScreen = run != nullptr;
        if (run != nullptr)
            m_render->RunScreen = prepare(*run);
        m_render->Screen = prepare(m_screen);
        m_render->ScreenView = view;
        m_render->ScreenPrepared = true;
    }

    void GuiSubsystem::Render(rhi::RenderPassEncoder& encoder, const render::ScreenOverlayView& view)
    {
        if (!IsRenderReady() || !m_screen.Root || view.width == 0 || view.height == 0)
            return;
        const bool prepared = m_render->ScreenPrepared && SameView(m_render->ScreenView, view);
        m_render->ScreenPrepared = false;
        if (prepared)
        {
            if (m_render->HasRunScreen)
                m_render->Renderer->Draw(encoder, m_render->RunScreen);
            m_render->Renderer->Draw(encoder, m_render->Screen);
            return;
        }
        // Nothing was prepared for this view, so record now and draw the layers in place.
        const Float2 target{static_cast<f32>(view.width), static_cast<f32>(view.height)};
        const Function<void(SceneRoot&, Float2)> layOut = [this](SceneRoot& root, Float2 size) { StyleAndLayOut(root, size); };
        const auto draw = [&](ScreenTier& tier)
        {
            tier.TargetSize = target;
            const GuiScreenPlacement place = PlaceScreen(tier.Resolution, tier.Fit, tier.Orientation, target);
            tier.Turn = place.Turn;
            m_render->Renderer->Draw(encoder, PrepareScreenTier(*m_render->Renderer, *tier.Root, place, target, view, nullptr, layOut));
        };
        if (ScreenTier* run = m_runScreens ? FindRunTier(m_renderRun) : nullptr)
            draw(*run);
        draw(m_screen);
    }

    // ---- the game's screen ----

    GuiScreenPlacement GuiSubsystem::PlaceScreen(Float2 resolution, FitMode fit, ScreenOrientation orientation, Float2 target) noexcept
    {
        GuiScreenPlacement place;
        if (target.x <= 0.0f || target.y <= 0.0f)
            return place;
        const ContentRotation turn = TurnFor(orientation, target.x, target.y);
        const bool hasResolution = resolution.x > 0.0f && resolution.y > 0.0f;
        if (!hasResolution && turn == ContentRotation::None)
            return place; // a plain draw
        const bool swaps = turn == ContentRotation::Quarter || turn == ContentRotation::ThreeQuarter;
        // The game's frame: its resolution, or else the target's own pixels in the game's shape.
        const Float2 content = hasResolution ? resolution : (swaps ? Float2{target.y, target.x} : target);
        const ContentFit placed{Rectangle{0.0f, 0.0f, target.x, target.y}, content, hasResolution ? fit : FitMode::Stretch, turn};
        place.Fitted = true;
        place.Turn = turn;
        place.Rect = placed.DstRect();
        place.DrawSize = swaps ? Float2{place.Rect.height, place.Rect.width} : Float2{place.Rect.width, place.Rect.height};
        const f32 scale = placed.Scale().y; // layout units per target pixel, down the game's height
        place.DpiScale = scale > 0.0f ? 1.0f / scale : 1.0f;
        if (turn != ContentRotation::None)
        {
            // Map the root's pixels onto the rectangle's, rotated as the game's image is. The affine
            // (row-vector form) is built from where the same fit takes the origin and each axis.
            const ContentFit turned{Rectangle{0.0f, 0.0f, place.Rect.width, place.Rect.height}, place.DrawSize, FitMode::Stretch, turn};
            const Float2 origin = turned.FromContent(Float2{0.0f, 0.0f});
            const Float2 xAxis = turned.FromContent(Float2{1.0f, 0.0f});
            const Float2 yAxis = turned.FromContent(Float2{0.0f, 1.0f});
            place.Transform = Float4x4{{{xAxis.x - origin.x, xAxis.y - origin.y, 0.0f, 0.0f},
                                        {yAxis.x - origin.x, yAxis.y - origin.y, 0.0f, 0.0f},
                                        {0.0f, 0.0f, 1.0f, 0.0f},
                                        {origin.x, origin.y, 0.0f, 1.0f}}};
        }
        return place;
    }

    void GuiSubsystem::ApplyResolution(ScreenTier& tier, u32 width, u32 height, FitMode fit)
    {
        tier.Resolution = width > 0 && height > 0 ? Float2{static_cast<f32>(width), static_cast<f32>(height)} : Float2{0.0f, 0.0f};
        tier.Fit = fit;
    }

    void GuiSubsystem::SetScreenResolution(u32 width, u32 height, FitMode fit) { ApplyResolution(m_screen, width, height, fit); }

    GuiSubsystem::ScreenTier* GuiSubsystem::FindRunTier(foundation::run::RunId run) noexcept
    {
        if (run == 0)
            return nullptr;
        for (const UniquePtr<ScreenTier>& tier : m_runTiers)
            if (tier->Run == run)
                return tier.Get();
        return nullptr;
    }

    GuiSubsystem::ScreenTier* GuiSubsystem::TierOfRoot(const SceneRoot* root) noexcept
    {
        if (root == nullptr)
            return nullptr;
        if (root == m_screen.Root.Get())
            return &m_screen;
        for (const UniquePtr<ScreenTier>& tier : m_runTiers)
            if (tier->Root.Get() == root)
                return tier.Get();
        return nullptr;
    }

    ScreenStack* GuiSubsystem::ScreensFor(foundation::run::IRun* run)
    {
        SceneRoot* root = ScreenRootFor(run);
        ScreenTier* tier = TierOfRoot(root);
        return tier != nullptr ? tier->Screens.Get() : nullptr;
    }

    SceneRoot* GuiSubsystem::ScreenRootFor(foundation::run::IRun* run)
    {
        if (!m_runScreens || run == nullptr || !m_screen.Root)
            return m_screen.Root.Get();
        if (ScreenTier* found = FindRunTier(run))
            return found->Root.Get();
        UniquePtr<ScreenTier> tier = MakeUnique<ScreenTier>(m_allocator);
        tier->Run = run->Id();
        MakeScreenTier(*tier);
        SceneRoot* root = tier->Root.Get();
        m_runTiers.PushBack(Move(tier));
        return root;
    }

    void GuiSubsystem::SetScreenResolution(foundation::run::IRun* run, u32 width, u32 height, FitMode fit)
    {
        // The tier is made on first use, so a tab can size it before anything is pushed.
        ScreenTier* tier = TierOfRoot(ScreenRootFor(run));
        if (tier != nullptr)
            ApplyResolution(*tier, width, height, fit);
    }

    void GuiSubsystem::SetScreenOrientation(foundation::run::IRun* run, ScreenOrientation orientation)
    {
        if (ScreenTier* tier = TierOfRoot(ScreenRootFor(run)))
            tier->Orientation = orientation;
    }

    void GuiSubsystem::EndRunScreens(foundation::run::RunId run)
    {
        for (usize i = 0; i < m_runTiers.Size(); ++i)
            if (m_runTiers[i]->Run == run)
            {
                SceneRoot* root = m_runTiers[i]->Root.Get();
                if (m_activeRoot == root)
                    m_activeRoot = nullptr;
                if (m_pressRoot == root)
                    m_pressRoot = nullptr;
                m_runTiers[i]->Screens->Clear();
                m_runTiers.RemoveAt(i);
                break;
            }
        if (m_renderRun == run)
            m_renderRun = 0;
    }

    GuiSubsystem::ScreenTier& GuiSubsystem::InputTier() noexcept
    {
        const void* bound = m_runScreens && m_input != nullptr ? m_input->BoundSceneKey() : nullptr;
        if (bound != nullptr)
            for (const SceneTier& scene : m_sceneTiers)
                if (static_cast<const void*>(scene.Scene) == bound)
                {
                    if (ScreenTier* tier = FindRunTier(scene.Scene->Run()))
                        return *tier;
                    break;
                }
        return m_screen;
    }

    Float2 GuiSubsystem::ScreenPointerPoint(const ScreenTier& tier, Float2 point) noexcept
    {
        if (!tier.HasResolution())
            return point; // already in the target's own pixels (a rotated game's pointer arrives in its frame)
        // Map the point as PlaceScreen fitted the tier on the last draw: offset it by the part of
        // the resolution a crop leaves out, then scale it into the root's pixels.
        const ContentFit fit{Rectangle{0.0f, 0.0f, tier.TargetSize.x, tier.TargetSize.y}, tier.Resolution, tier.Fit, tier.Turn};
        const Rectangle source = fit.SrcRect();
        const f32 scale = fit.Scale().y;
        const f32 dpi = scale > 0.0f ? 1.0f / scale : 1.0f;
        return Float2{(point.x - source.x) * dpi, (point.y - source.y) * dpi};
    }

    RefPtr<SceneRoot> GuiSubsystem::CreatePreview(StringView markup, StringView css, Array<GuiDiagnostic>* diagnostics)
    {
        if (markup.IsEmpty())
            return {};
        MarkupLoader loader(m_context->GetElementSet(), m_allocator, m_resources.Get(), Fonts());
        loader.SetDataModel(m_context->GetDataRef());
        loader.SetDiagnostics(diagnostics);
        RefPtr<Node> tree = loader.LoadFromString(markup);
        if (!tree)
            return {};
        RefPtr<SceneRoot> root = MakeRef<SceneRoot>(m_allocator, m_context);
        {
            Node::StyleWriteScope defaults(*root);
            root->EditLayoutStyle([](LayoutStyle& s) { s.Kind = Display::Stack; }); // the document fills it
        }
        root->AddChild(tree.Get());
        if (!css.IsEmpty())
            (void)m_styles.SetSubtreeSheet(*tree, CSSParser::Parse(css, StringView(u8"preview")));
        return root;
    }

    void GuiSubsystem::RenderPreview(SceneRoot& root, rhi::CommandEncoder& encoder, rhi::TextureView* target, rhi::TextureFormat format,
                                     u32 width, u32 height, u32 frameIndex)
    {
        if (!IsRenderReady() || target == nullptr || width == 0 || height == 0)
            return;
        GuiPassRenderer& renderer = *m_render->Renderer;
        (void)renderer.RecordBakes(encoder, frameIndex); // before the pass samples them, once a frame
        StyleAndLayOut(root, Float2{static_cast<f32>(width), static_cast<f32>(height)});
        GuiPassView view;
        view.Width = width;
        view.Height = height;
        view.Format = format;
        view.FrameIndex = frameIndex;
        const GuiPassRenderer::Prepared prepared = renderer.Prepare(root, view, &encoder);
        rhi::RenderPassDesc pass{};
        rhi::ColorAttachment color{};
        color.view = target;
        color.loadOp = rhi::LoadOp::Load;
        color.storeOp = rhi::StoreOp::Store;
        pass.colorAttachments.Add(color);
        if (rhi::RenderPassEncoder* passEncoder = encoder.BeginRenderPass(pass))
        {
            renderer.Draw(*passEncoder, prepared);
            passEncoder->End();
        }
    }

    usize GuiSubsystem::TextureCount() const noexcept { return m_render ? m_render->Targets.Size() : 0; }

    namespace
    {
        // Draws a texture's content into it. The content is laid out at the texture's size, inset by
        // `padding` on each side (left clear). The offscreen layers are drawn first, then the pass,
        // and the texture is left ready to sample.
        void DrawTexture(GuiPassRenderer& renderer, rhi::CommandEncoder& encoder, auto& target, SceneRoot& root, u32 padding,
                         u32 frameIndex)
        {
            GuiPassView view;
            view.X = static_cast<i32>(padding);
            view.Y = static_cast<i32>(padding);
            view.Width = target.Width - 2 * padding;
            view.Height = target.Height - 2 * padding;
            view.Format = rhi::TextureFormat::RGBA8UnormSrgb;
            if (target.Multisampled)
            {
                const foundation::vg::renderer::VGTargetConfig config = target.Pass.ConfigFor();
                view.SampleCount = config.sampleCount;
                view.DepthStencilFormat = config.depthStencilFormat;
            }
            view.FrameIndex = frameIndex;
            const GuiPassRenderer::Prepared prepared = renderer.Prepare(root, view, &encoder);
            encoder.TransitionTexture(target.Texture, target.State, rhi::ResourceState::RenderTarget);
            const rhi::ClearColor clear{0.0f, 0.0f, 0.0f, 0.0f};
            rhi::RenderPassDesc pass{};
            if (target.Multisampled)
                pass = target.Pass.BeginPass(encoder, target.View, clear, u8"GUI texture");
            else
            {
                rhi::ColorAttachment color{};
                color.view = target.View;
                color.loadOp = rhi::LoadOp::Clear;
                color.storeOp = rhi::StoreOp::Store;
                color.clearValue = clear;
                pass.colorAttachments.Add(color);
            }
            if (rhi::RenderPassEncoder* passEncoder = encoder.BeginRenderPass(pass))
            {
                renderer.Draw(*passEncoder, prepared);
                passEncoder->End();
            }
            encoder.TransitionTexture(target.Texture, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderRead);
            target.State = rhi::ResourceState::ShaderRead;
        }
    }

    void GuiSubsystem::RenderTextures(rhi::CommandEncoder& encoder, u32 frameIndex)
    {
        if (!IsRenderReady() || m_texturesSerial == m_frameSerial)
            return;
        m_texturesSerial = m_frameSerial; // once a frame, since several editor pages share one GUI
        GuiPassRenderer& renderer = *m_render->Renderer;
        (void)renderer.RecordBakes(encoder, frameIndex); // before any pass samples them
        for (const UniquePtr<RenderState::Target>& target : m_render->Targets)
            target->Seen = false;
        for (SceneTier& tier : m_sceneTiers)
        {
            auto* sprites = tier.Scene->GetSystem<engine::render::SpriteComponentManager>();
            auto* decals = tier.Scene->GetSystem<engine::render::DecalComponentManager>();
            // The entity of a render-texture canvas shows the canvas's texture on its sprite and
            // decal, through the same override a script would set. A sprite or decal that showed
            // the old texture lets it go.
            const auto show = [&](scene::EntityHandle entity, rhi::TextureView* was, rhi::TextureView* now)
            {
                if (auto* sprite = sprites != nullptr ? sprites->Get(entity) : nullptr; sprite != nullptr && (now != nullptr || sprite->texture == was))
                    sprite->texture = now;
                if (auto* decal = decals != nullptr ? decals->Get(entity) : nullptr; decal != nullptr && (now != nullptr || decal->texture == was))
                    decal->texture = now;
            };
            if (auto* canvases = tier.Scene->GetSystem<GuiCanvasComponentManager>())
                canvases->ForEach(
                    [&](GuiCanvasComponent& c, scene::EntityHandle entity)
                    {
                        if (c.renderMode != GuiCanvasRenderMode::RenderTexture || !c.renderRoot)
                            return;
                        rhi::TextureView* was = c.renderTextureView;
                        RenderState::Target* target = m_render->EnsureTarget(*c.renderRoot, *tier.Scene, entity, Max(c.renderTextureWidth, 1u),
                                                                             Max(c.renderTextureHeight, 1u), m_allocator);
                        c.renderTexture = target != nullptr ? target->Texture : nullptr;
                        c.renderTextureView = target != nullptr ? target->View : nullptr;
                        show(entity, was, c.renderTextureView);
                        if (target == nullptr || !c.visible || !tier.Scene->IsEffectivelyActive(entity))
                            return; // the texture is kept but not drawn
                        StyleAndLayOut(*c.renderRoot, Float2{static_cast<f32>(target->Width), static_cast<f32>(target->Height)});
                        DrawTexture(renderer, encoder, *target, *c.renderRoot, 0, frameIndex);
                    });
            // The subsystem drives a world panel's sprite outright. The sprite is the quad in its
            // entity's plane, a little larger than the panel to cover the transparent edge its texture
            // keeps, so the quad's own edge falls in clear texels and sampling smooths it. It is
            // drawn after tone mapping, so the panel keeps its authored colours, as the screen's GUI
            // does.
            constexpr u32 kEdge = 2;
            if (auto* panels = tier.Scene->GetSystem<GuiWorldPanelComponentManager>())
                panels->ForEach(
                    [&](GuiWorldPanelComponent& c, scene::EntityHandle entity)
                    {
                        if (!c.renderRoot)
                            return;
                        const Float2 pixels = WorldPanelPixels(c);
                        const u32 width = static_cast<u32>(pixels.x) + 2 * kEdge;
                        const u32 height = static_cast<u32>(pixels.y) + 2 * kEdge;
                        RenderState::Target* target = m_render->EnsureTarget(*c.renderRoot, *tier.Scene, entity, width, height, m_allocator);
                        c.renderTexture = target != nullptr ? target->Texture : nullptr;
                        c.renderTextureView = target != nullptr ? target->View : nullptr;
                        const bool shown = c.visible && tier.Scene->IsEffectivelyActive(entity);
                        if (sprites != nullptr)
                        {
                            engine::render::SpriteComponent* sprite = sprites->Get(entity);
                            if (sprite == nullptr)
                                sprite = &sprites->Add(entity);
                            sprite->orientation = engine::render::SpriteOrientation::EntityOriented;
                            sprite->size = Float2{c.sizeMeters.x * static_cast<f32>(width) / pixels.x, c.sizeMeters.y * static_cast<f32>(height) / pixels.y};
                            sprite->texture = c.renderTextureView;
                            sprite->visible = shown && target != nullptr;
                            sprite->postTonemap = true;
                        }
                        if (target == nullptr || !shown)
                            return;
                        StyleAndLayOut(*c.renderRoot, pixels);
                        DrawTexture(renderer, encoder, *target, *c.renderRoot, kEdge, frameIndex);
                    });
        }
        // Removes the textures whose canvas or panel has gone. A sprite or decal in a scene that
        // still exists lets go of such a texture first; then the texture is destroyed, after
        // waiting for the GPU.
        for (usize i = m_render->Targets.Size(); i-- > 0;)
        {
            RenderState::Target& target = *m_render->Targets[i];
            if (target.Seen)
                continue;
            bool sceneHere = false;
            for (const SceneTier& tier : m_sceneTiers)
                sceneHere = sceneHere || tier.Scene == target.Scene;
            if (sceneHere && target.View != nullptr)
            {
                if (auto* sprites = target.Scene->GetSystem<engine::render::SpriteComponentManager>())
                    if (auto* sprite = sprites->Get(target.Entity); sprite != nullptr && sprite->texture == target.View)
                        sprite->texture = nullptr;
                if (auto* decals = target.Scene->GetSystem<engine::render::DecalComponentManager>())
                    if (auto* decal = decals->Get(target.Entity); decal != nullptr && decal->texture == target.View)
                        decal->texture = nullptr;
            }
            m_render->Device->WaitIdle();
            m_render->Release(target);
            m_render->Targets.RemoveAt(i);
        }
    }

    GuiTouchGuide::GuiTouchGuide() : m_controls(MemoryAllocator()), m_labels(MemoryAllocator())
    {
        SetTag(StringView(u8"touchguide"));
        SetHitTestVisible(false);
        SetVisible(false);
    }

    void GuiTouchGuide::Update(engine::input::InputSubsystem& input, bool touched)
    {
        m_controls.Clear();
        if (touched)
            input.ForEachActionRuntime([this](foundation::input::ActionRuntime& runtime)
                                       {
                                           runtime.ForEachTouchControl([this](const foundation::input::ActionRuntime::TouchControl& control)
                                                                       {
                                                                           const foundation::input::Binding& b = *control.binding;
                                                                           Shown shown;
                                                                           shown.region = Rectangle{b.regionX, b.regionY, b.regionW, b.regionH};
                                                                           shown.stick = b.source == foundation::input::BindingSource::TouchStick;
                                                                           shown.down = control.down;
                                                                           shown.held = control.held;
                                                                           shown.anchor = control.anchor;
                                                                           shown.finger = control.finger;
                                                                           shown.radius = b.stickRadius > 0.0f ? b.stickRadius : 0.15f;
                                                                           m_controls.PushBack(shown);
                                                                           // The action's name, centred in its zone (for a stick, only while no finger holds it).
                                                                           if (m_labels.Size() < m_controls.Size())
                                                                           {
                                                                               RefPtr<Label> label = MakeRef<Label>(MemoryAllocator());
                                                                               label->SetHitTestVisible(false);
                                                                               label->SetInlineStyle(StringView(u8"text-align"), StringView(u8"center"));
                                                                               label->SetInlineStyle(StringView(u8"color"), StringView(u8"#ffffff"));
                                                                               m_labels.PushBack(label);
                                                                               AddChild(label.Get());
                                                                           }
                                                                           m_labels[m_controls.Size() - 1]->SetText(control.action);
                                                                       });
                                       });
        const f32 w = GetSize().x;
        const f32 h = GetSize().y;
        for (usize i = 0; i < m_labels.Size(); ++i)
        {
            Label& label = *m_labels[i];
            const bool show = i < m_controls.Size() && !m_controls[i].held;
            label.SetVisible(show);
            if (!show)
                continue;
            const Float2 centre = GuideCentre(m_controls[i], w, h);
            const f32 size = GuideRadius(m_controls[i], w, h) * 2.0f;
            // The name grows with its button, as the button grows with the screen. A fixed size
            // that is legible in a desktop window is tiny on a phone's dense pixels.
            const f32 fontSize = Max(kGuideMinFontSize, size * kGuideFontPerSize);
            label.SetInlineStyle(StringView(u8"font-size"), Format(u8"{}px", fontSize).AsView());
            label.SetInlineStyle(StringView(u8"opacity"), StringView(m_controls[i].down ? u8"1" : u8"0.85"));
            label.EditLayoutStyle([&](LayoutStyle& s)
            {
                s.Placement = Position::Absolute;
                s.Left = LayoutLength::Px(centre.x - size * 0.5f);
                s.Top = LayoutLength::Px(centre.y - fontSize * 0.65f); // puts the line's middle on the centre
                s.Width = LayoutLength::Px(size);
            });
        }
        // Ask for a redraw while a control shows (fingers move), and once more as the last one goes.
        // When idle with nothing shown, it asks for nothing.
        const bool wasShown = IsVisible();
        SetVisible(!m_controls.IsEmpty());
        if (!m_controls.IsEmpty() || wasShown)
            Invalidate();
    }

    void GuiTouchGuide::OnDraw(DrawContext& ctx, const Rect& localBounds)
    {
        const f32 w = localBounds.width;
        const f32 h = localBounds.height;
        foundation::vg::VGContext& g = ctx.VG();
        for (const Shown& shown : m_controls)
        {
            if (shown.stick && shown.held)
            {
                // A ring where the finger came down shows the stick's reach, and a knob where the
                // finger is, held inside the ring.
                const Float2 anchor{shown.anchor.x * w, shown.anchor.y * h};
                const f32 reach = shown.radius * w;
                Float2 knob{shown.finger.x * w, shown.finger.y * h};
                const Float2 offset{knob.x - anchor.x, knob.y - anchor.y};
                const f32 length = Sqrt(offset.x * offset.x + offset.y * offset.y);
                if (length > reach && length > 0.0f)
                    knob = Float2{anchor.x + offset.x / length * reach, anchor.y + offset.y / length * reach};
                g.FillCircle(anchor, reach, Color{0.0f, 0.0f, 0.0f, 0.22f});
                g.StrokeCircle(anchor, reach, Color{1.0f, 1.0f, 1.0f, 0.6f}, 3.0f);
                g.FillCircle(knob, reach * 0.4f, Color{1.0f, 1.0f, 1.0f, 0.7f});
                g.StrokeCircle(knob, reach * 0.4f, Color{0.0f, 0.0f, 0.0f, 0.35f}, 2.0f);
                continue;
            }
            // A button fills its zone, so what is seen is what is tapped. An idle stick is a small
            // ring low in its zone, where a thumb rests. A dark wash with a light edge reads on snow
            // and at night alike.
            const Float2 centre = GuideCentre(shown, w, h);
            const f32 radius = GuideRadius(shown, w, h);
            g.FillCircle(centre, radius, Color{0.0f, 0.0f, 0.0f, shown.down ? 0.34f : 0.2f});
            g.StrokeCircle(centre, radius, Color{1.0f, 1.0f, 1.0f, shown.down ? 0.85f : 0.5f}, 2.5f);
        }
    }

    DefaultLoadingScreen BuildDefaultLoadingScreen(IAllocator& allocator, StringView logoSvg)
    {
        DefaultLoadingScreen screen;
        // The screen is dark, with its column in the middle; the overlay layer stacks it over the
        // window.
        screen.Root = MakeRef<UIWidget>(allocator);
        {
            UIWidget& root = static_cast<UIWidget&>(*screen.Root);
            root.SetId(StringView(u8"loading-screen"));
            root.SetInlineStyle(StringView(u8"background-color"), StringView(u8"#0a0a0c"));
            root.EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Column;
                s.JustifyContent = Justify::Center;
                s.AlignItems = Align::Center;
                s.RowGap = 14.0f;
            });
        }
        if (!logoSvg.IsEmpty())
            if (RefPtr<SvgDrawable> logo = SvgDrawable::FromText(allocator, logoSvg))
            {
                constexpr f32 kLogoHeight = 72.0f;
                RefPtr<Image> image = MakeRef<Image>(allocator);
                image->SetId(StringView(u8"logo"));
                image->SetDrawable(logo);
                image->EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Width = LayoutLength::Px(kLogoHeight * kEngineLogoAspect);
                    s.Height = LayoutLength::Px(kLogoHeight);
                    s.Margin.Bottom = 22.0f; // the logo stands clear of the bar
                });
                screen.Root->AddChild(image.Get());
            }
        RefPtr<ProgressBar> bar = MakeRef<ProgressBar>(allocator);
        bar->SetId(StringView(u8"progress"));
        bar->EditLayoutStyle([](LayoutStyle& s)
        {
            s.Width = LayoutLength::Px(320.0f);
            s.Height = LayoutLength::Px(6.0f);
        });
        screen.Root->AddChild(bar.Get());
        screen.Progress = bar.Get();
        RefPtr<Label> status = MakeRef<Label>(allocator);
        status->SetId(StringView(u8"status"));
        status->SetText(StringView(u8"Loading"));
        status->SetInlineStyle(StringView(u8"font-size"), StringView(u8"13px"));
        status->SetInlineStyle(StringView(u8"color"), StringView(u8"#9aa3b2"));
        screen.Root->AddChild(status.Get());
        return screen;
    }

    const engine::DomainModule& GuiDomain() noexcept
    {
        static const resource::ResourceModule* const kResources[] = {
            &kGuiResourceModule, &foundation::vg::kVGResourceModule, &foundation::fonts::kFontsResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"gui",
            .installScene = &AddGuiSceneManagers,
            .registerReflection = &RegisterGuiComponentReflection,
            .registerScriptFacade = &RegisterGuiScriptFacades,
            .resources = Span<const resource::ResourceModule* const>{kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}

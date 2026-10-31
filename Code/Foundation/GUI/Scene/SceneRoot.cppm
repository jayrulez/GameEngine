// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :scene_root partition
//
// GUIContext: one GUI, made of any number of roots (windows, render targets, a panel in the
// world). It holds the state that belongs to the GUI rather than to a window: the animation clock
// (roots may pause or slow their own time on it), deferred tree edits, the bindings' scheduler
// (one flush a frame for every root, so a binding in one root that reads a value another root wrote
// runs once, in order), the named data, the commands and the keymap, the element table that weak
// handles resolve through (so a handle survives its node moving between roots), and the element
// set. It tracks the active root, the one that last received input; keystrokes, the gamepad and
// command availability follow that root's focus path. Every root is made in a context (a GUI with
// one root is a context with one root). The host advances the GUI once a frame through
// GUIContext::Update.
//
// SceneRoot: the root of one widget tree of a GUI, and its coordinator (the role of eepp's
// Scene::SceneNode). It is a Node subclass at the top of the tree. It owns the event dispatcher and
// its display scale, and reaches its GUIContext's ActionManager, MutationQueue, element table and
// element set.
// GUIContext::Update ticks running actions, then drains deferred tree edits. Drawing is
// inherited from Node::Draw, with the top layer drawn after the tree (DrawFrame).
//
// Each root has its own scale (one per window, or per render target such as a panel in the world
// or a texture). Its tree lays out in logical units, which its display scale (the window's content
// scale times a UI scale) maps to device pixels through the root's own transform. Drawing and hit
// testing therefore agree, and two windows on monitors of different density each keep their own
// scale.
//
// Nodes reach the coordinator through Node's virtual GetActionManager()/GetMutationQueue(),
// which walk up the parent chain; SceneRoot overrides them to return its own.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE

export module foundation.gui:scene_root;

import foundation.core; // Duration, Stopwatch
import foundation.profiler;
import foundation.vg; // VGBatch (what a frame drew, for the frame stats)
import :node;
import :frame_stats;
import :draw_context;
import :element_types;
import :action_manager;
import :mutation_queue;
import :event_dispatcher;
import :media_query; // MediaContext
import :reactive;    // EffectScheduler
import :data_model;
import :commands;
import :keymap;
import foundation.fonts;   // IFontService
import foundation.i18n;    // Localization
import :resource_provider; // IResourceProvider
import :picture_baker;     // IPictureBaker
import :text_editing_keys; // the editing keys every GUI starts with
import :accessibility;     // the accessibility tree's model and the host's bridge

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    class SceneRoot;

    class GUIContext : public RefCounted
    {
    public:
        // MemoryAllocator is valid in a constructor (MakeRef sets it first).
        GUIContext()
            : m_elements(core::MakeRef<ElementTable>(MemoryAllocator())), m_data(core::MakeRef<DataModel>(MemoryAllocator()))
        {
            BindTextEditingKeys(m_keymap); // bound first, so a GUI's own later bindings win a tie
            (void)m_keymap.Bind(core::StringView(u8"f10"), core::StringView(u8"menu.enter-bar"));
            (void)m_keymap.Bind(core::StringView(u8"shift-f10"), core::StringView(u8"menu.context"));
            // Menu movement keys, bound in the menu key context.
            (void)m_keymap.Bind(core::StringView(u8"up"), core::StringView(kMenuUpCommand), core::StringView(kMenuKeyContext));
            (void)m_keymap.Bind(core::StringView(u8"down"), core::StringView(kMenuDownCommand), core::StringView(kMenuKeyContext));
            (void)m_keymap.Bind(core::StringView(u8"left"), core::StringView(kMenuLeftCommand), core::StringView(kMenuKeyContext));
            (void)m_keymap.Bind(core::StringView(u8"right"), core::StringView(kMenuRightCommand), core::StringView(kMenuKeyContext));
            (void)m_keymap.Bind(core::StringView(u8"return"), core::StringView(kMenuActivateCommand), core::StringView(kMenuKeyContext));
            (void)m_keymap.Bind(core::StringView(u8"space"), core::StringView(kMenuActivateCommand), core::StringView(kMenuKeyContext));
            // A drag from the keys or the pad: picked up at the focus, and dropped with Return or
            // South, which the dispatcher handles during the drag.
            (void)m_keymap.Bind(core::StringView(u8"ctrl-shift-d"), core::StringView(u8"drag.pick"));
            (void)m_keymap.Bind(core::StringView(u8"pad-north"), core::StringView(u8"drag.pick"));
            // Back: one command for the keyboard and the pad, which a game can rebind like any
            // other.
            (void)m_keymap.Bind(core::StringView(u8"escape"), core::StringView(kBackCommand));
            (void)m_keymap.Bind(core::StringView(u8"pad-east"), core::StringView(kBackCommand));
        }
        GUIContext(const GUIContext&) = delete;
        GUIContext& operator=(const GUIContext&) = delete;
        ~GUIContext() override
        {
            SetLocalization(nullptr);
            m_elements->Close();
        }

        /// Advances every root by `elapsed`, once a frame. In order, it advances the clock (each
        /// root's actions at its own time scale), each root's input repeat and per-frame hook, the
        /// bindings whose values changed, the deferred tree edits, and then each root's layout.
        void Update(core::Duration elapsed);

        [[nodiscard]] ActionManager& GetActionManager() noexcept { return m_actionManager; }
        [[nodiscard]] MutationQueue& GetMutationQueue() noexcept { return m_mutationQueue; }
        [[nodiscard]] EffectScheduler& GetEffectScheduler() noexcept { return m_effects; }
        /// The GUI's named values, which markup and scripts bind to.
        [[nodiscard]] DataModel& GetData() noexcept { return *m_data; }
        /// Animates data value `name` to `to` over `seconds` on the GUI's clock with the easing
        /// `easing` names. Everything bound to it follows each frame (a score counting up with no
        /// widget to drive it), in the value's own type (a whole number counts in whole numbers). A
        /// new tween of the same name replaces the running one, starting from the value shown. A
        /// paused GUI root does not hold it. Returns the tween for further shaping. Defined in
        /// Styling/TweenImpl.cpp.
        RefPtr<Tween> AnimateData(core::StringView name, f64 to, f32 seconds, core::StringView easing = core::StringView(u8"ease"));
        /// Makes the same tween as AnimateData without starting it, as a step for a group.
        RefPtr<Tween> CreateDataTween(core::StringView name, f64 to, f32 seconds, core::StringView easing = core::StringView(u8"ease"));
        [[nodiscard]] const RefPtr<DataModel>& GetDataRef() const noexcept { return m_data; }
        [[nodiscard]] CommandSet& GetCommands() noexcept { return m_commands; }
        Command& DefineCommand(core::StringView id) { return m_commands.Define(id, MemoryAllocator()); }
        [[nodiscard]] Keymap& GetKeymap() noexcept { return m_keymap; }
        /// Sets where the roots are on the screen, supplied by a host that places its windows. With
        /// it a drag continues from root to root, and a drag released over no root ends outside.
        /// Not owned; with null, each root is on its own.
        void SetRootPlaces(IRootPlaces* places);
        [[nodiscard]] IRootPlaces* GetRootPlaces() const noexcept { return m_rootPlaces; }
        /// Sets the fonts and resources the GUI's text asks for (a rich text's own faces and its
        /// icons). Not owned.
        void SetFontService(fonts::IFontService* fonts) noexcept { m_fontService = fonts; }
        /// The user's preferences as the host knows them (the platform's dark mode, contrast and
        /// reduced-motion settings). Sheets read them as prefers-color-scheme, prefers-contrast and
        /// prefers-reduced-motion. A change restyles at the next frame.
        void SetColorScheme(bool dark) noexcept { m_darkScheme = dark; }
        [[nodiscard]] bool IsDarkScheme() const noexcept { return m_darkScheme; }
        void SetContrastPreference(ContrastPreference contrast) noexcept { m_contrast = contrast; }
        [[nodiscard]] ContrastPreference GetContrastPreference() const noexcept { return m_contrast; }
        void SetReducedMotion(bool reduce) noexcept { m_reducedMotion = reduce; }
        /// Sets the most the GUI's clock advances in one update, in seconds (0, the default, means
        /// no limit). A game sets it so a hitch (a level loading under a menu) does not skip a
        /// screen's entrance or exit; the motion runs slower for that frame instead of jumping to
        /// its end.
        void SetMaxClockStep(f32 seconds) noexcept { m_maxClockStep = core::Max(0.0f, seconds); }
        [[nodiscard]] f32 GetMaxClockStep() const noexcept { return m_maxClockStep; }
        [[nodiscard]] bool IsReducedMotion() const noexcept { return m_reducedMotion; }
        [[nodiscard]] fonts::IFontService* GetFontService() const noexcept { return m_fontService; }
        void SetResourceProvider(IResourceProvider* resources) noexcept { m_resources = resources; }
        /// Sets the host's picture baker (from its renderer), so an SVG skin draws from a texture
        /// baked at its displayed size instead of as paths every frame. Without one, skins draw as
        /// paths. Not owned.
        void SetPictureBaker(IPictureBaker* baker) noexcept { m_baker = baker; }
        [[nodiscard]] IPictureBaker* GetPictureBaker() const noexcept { return m_baker; }
        /// Sets the host's localization, through which translated properties resolve. Not owned. A
        /// change to it (a language, a table, pseudolocalization) re-resolves every translated
        /// property and restyles the roots (`:lang`). With null, a translated property shows its
        /// key.
        void SetLocalization(foundation::i18n::Localization* localization)
        {
            if (localization == m_localization)
                return;
            if (m_localization != nullptr)
                m_localization->RemoveOnChanged(m_localizationToken);
            m_localization = localization;
            m_localizationToken = 0;
            if (m_localization != nullptr)
                m_localizationToken = m_localization->OnChanged([this]() { LanguageChanged(); });
            LanguageChanged();
        }
        [[nodiscard]] foundation::i18n::Localization* GetLocalization() const noexcept { return m_localization; }
        /// Called by a binding that depends on the language (a translated property's binding), so
        /// it reruns when the language changes.
        void TrackLanguage() const { m_languageChanged.Track(); }

        /// Sets a service that a library finds through the GUI by its key (the gamekit's
        /// InputPrompts, which the host supplies). It is borrowed, and the host keeps it alive while
        /// it is set; null clears it. A binding that read the service runs again when it is set.
        void SetService(core::StringView key, void* service)
        {
            for (usize i = 0; i < m_services.Size(); ++i)
                if (m_services[i].Key.AsView() == key)
                {
                    if (service == nullptr)
                        m_services.RemoveAt(i);
                    else
                        m_services[i].Service = service;
                    m_servicesChanged.Notify();
                    return;
                }
            if (service != nullptr)
                m_services.PushBack(ServiceEntry{core::String(key), service});
            m_servicesChanged.Notify();
        }
        /// Returns the service under `key`, or null. A binding that reads it follows changes.
        [[nodiscard]] void* GetService(core::StringView key) const
        {
            m_servicesChanged.Track();
            for (const ServiceEntry& entry : m_services)
                if (entry.Key.AsView() == key)
                    return entry.Service;
            return nullptr;
        }
        [[nodiscard]] IResourceProvider* GetResourceProvider() const noexcept { return m_resources; }
        [[nodiscard]] ElementTable* GetElementTable() noexcept { return m_elements.Get(); }
        [[nodiscard]] const ElementSet& GetElementSet() const noexcept { return m_elementSet; }
        /// Sets which elements this GUI's markup and scripts may create (every registered element
        /// by default).
        void SetElementSet(ElementSet set) { m_elementSet = core::Move(set); }

        // === Roots ===
        [[nodiscard]] usize RootCount() const noexcept { return m_roots.Size(); }
        [[nodiscard]] SceneRoot* GetRootAt(usize index) const noexcept { return index < m_roots.Size() ? m_roots[index] : nullptr; }
        /// The root that last received input (keys, the gamepad, a press), or else the first root.
        /// Keystrokes, the gamepad and command availability go to it. A host or a script may set
        /// it.
        [[nodiscard]] SceneRoot* GetActiveRoot() const noexcept
        {
            return m_active != nullptr ? m_active : (m_roots.IsEmpty() ? nullptr : m_roots[0]);
        }
        /// Called when `root` took input from the user. It becomes the active root, and the other
        /// roots' light popups (auto popups and tooltips) close, as a press outside them would.
        void TookInput(SceneRoot& root);
        void SetActiveRoot(SceneRoot* root)
        {
            if (root == m_active)
                return;
            m_active = root;
            m_activeChanged.Notify();
        }

        // === Commands across roots ===
        /// Runs command `id` along the active root's focus path.
        bool RunCommand(core::StringView id);
        /// Whether command `id` is available on the active root's focus path. A binding that reads
        /// it follows the active root, its focus, the command and the handlers.
        [[nodiscard]] bool IsCommandAvailable(core::StringView id);

        /// How many bindings the last Update ran.
        [[nodiscard]] u32 LastBindingCount() const noexcept { return m_lastBindingCount; }

        // === Accessibility ===
        /// Sets the host's bridge to a screen reader (not owned; null for none). While it listens,
        /// each Update sends every root's changed nodes (all of them the first time) and the focus.
        void SetAccessibilityBridge(IAccessibilityBridge* bridge) noexcept
        {
            m_accessBridge = bridge;
            m_accessListening = false; // so a new listener receives the whole tree
        }
        [[nodiscard]] IAccessibilityBridge* GetAccessibilityBridge() const noexcept { return m_accessBridge; }
        /// Speaks `text` through the bridge, for news that no node holds (a score, a level loaded).
        void Announce(core::StringView text, AnnouncePriority priority = AnnouncePriority::Polite)
        {
            if (m_accessBridge != nullptr && m_accessBridge->IsListening())
                m_accessBridge->Announce(text, priority);
        }
        /// Queues an action that a platform requests (from any thread). It runs in the next Update,
        /// on the GUI's thread, through the same paths input takes.
        void RequestAccessAction(const AccessActionRequest& request)
        {
            m_accessLock.Lock();
            m_accessRequests.PushBack(request);
            m_accessLock.Unlock();
        }
        /// Runs an action now. Returns false when its node is gone or does not support it. Defined
        /// in AccessibilityImpl.cpp.
        bool RunAccessAction(const AccessActionRequest& request);
        /// Builds `root`'s whole tree as it is now, whether or not anything listens (for the
        /// inspector or a test).
        [[nodiscard]] AccessTreeUpdate BuildAccessTree(SceneRoot& root);
        /// Describes `node`'s accessibility node as it is now. Returns false when it has no node of
        /// its own (no role, hidden, or a part of a leaf), in which case a screen reader reads it
        /// as part of an ancestor.
        bool DescribeAccess(Node& node, AccessNode& out);
        /// Returns the node an accessibility id names (for an item id, its item view), or null when
        /// it is gone.
        [[nodiscard]] Node* ResolveAccessId(u64 id) const noexcept
        {
            if (IsAccessItemId(id))
            {
                const Node::ElementRef* view = m_accessViews.Find(static_cast<u32>((id & ~kAccessItemIdBit) >> 32));
                return view != nullptr && view->table != nullptr ? view->table->Resolve(view->slot, view->generation) : nullptr;
            }
            return m_elements->Resolve(static_cast<u32>(id & 0xFFFFFFFFu), static_cast<u32>(id >> 32));
        }
        /// Returns an item view's key among this GUI's item views, assigned the first time it is
        /// asked.
        [[nodiscard]] u32 AccessViewKey(Node& view); // AccessibilityImpl.cpp

    private:
        friend class SceneRoot;
        ActionManager m_actionManager;
        MutationQueue m_mutationQueue;
        EffectScheduler m_effects;
        RefPtr<ElementTable> m_elements; // weak references to this GUI's nodes resolve through it
        ElementSet m_elementSet = ElementSet::All();
        RefPtr<DataModel> m_data;        // shared with the bindings to it
        CommandSet m_commands;
        Keymap m_keymap;
        fonts::IFontService* m_fontService = nullptr;
        bool m_darkScheme = true; // the user's preferences, for media queries
        ContrastPreference m_contrast = ContrastPreference::NoPreference;
        bool m_reducedMotion = false;
        IResourceProvider* m_resources = nullptr;
        IPictureBaker* m_baker = nullptr;
        foundation::i18n::Localization* m_localization = nullptr; // the host's
        u32 m_localizationToken = 0;
        Observable m_languageChanged;
        struct ServiceEntry
        {
            core::String Key;
            void* Service = nullptr;
        };
        Array<ServiceEntry> m_services; // services libraries find through the GUI (borrowed)
        Observable m_servicesChanged;
        Array<Keystroke> m_chord; // a chord typed so far (in the active root)
        IAccessibilityBridge* m_accessBridge = nullptr; // the host's
        bool m_accessListening = false;                 // the bridge was listening at the last update
        core::Mutex m_accessLock;                       // guards the request queue, which any thread may add to
        core::HashMap<u32, Node::ElementRef> m_accessViews; // item views by key, for their items' ids
        u32 m_nextAccessView = 1;
        Array<AccessActionRequest> m_accessRequests;
        void RunAccessRequests();  // AccessibilityImpl.cpp: runs each queued request
        void FlushAccessibility(); // AccessibilityImpl.cpp: sends each root's changes to the bridge
        f32 m_chordAge = 0.0f;
        f32 m_maxClockStep = 0.0f; // the largest clock step (0: unlimited)
        static constexpr f32 kChordTimeout = 1.0f;
        Array<SceneRoot*> m_roots; // non-owning: each root registers itself and leaves on destruction
        SceneRoot* m_active = nullptr;
        Observable m_activeChanged;
        u32 m_lastBindingCount = 0;

        IRootPlaces* m_rootPlaces = nullptr; // the host's
        void AddRoot(SceneRoot& root); // defined after SceneRoot, since it gives the root's dispatcher the places
        void LanguageChanged(); // defined after SceneRoot, which it restyles
        void RemoveRoot(SceneRoot& root); // defined after SceneRoot, since the other roots' drags must forget it
    };

    class SceneRoot : public Node
    {
        RTTI_OBJECT(SceneRoot, Node)
    public:
        /// Makes a root of `context`'s GUI, beside its other roots (a GUI with one root is a context
        /// with one root).
        explicit SceneRoot(RefPtr<GUIContext> context) : m_context(core::Move(context))
        {
            m_context->AddRoot(*this);
            m_eventDispatcher.SetKeystrokeRouter([this](const Keystroke& stroke) { return RouteKeystroke(stroke); });
            m_eventDispatcher.SetOnInput([this]() { m_context->TookInput(*this); });
            // F10 (bound in every GUI): enters the first node in this tree that takes it, such as a
            // menu bar.
            (void)OnCommand(core::StringView(u8"menu.enter-bar"), [this](CommandEvent&) { (void)EnterFromKeyboardIn(*this); });
            // Shift+F10 (bound in every GUI): opens the focused node's context menu.
            (void)OnCommand(core::StringView(u8"menu.context"), [this](CommandEvent&) { (void)m_eventDispatcher.InjectContextMenu(); });
            // Ctrl+Shift+D or the pad's North (bound in every GUI): picks up a drag at the focus.
            (void)OnCommand(core::StringView(u8"drag.pick"), [this](CommandEvent&) { (void)m_eventDispatcher.PickDrag(); });
        }
        ~SceneRoot() override { m_context->RemoveRoot(*this); }

        [[nodiscard]] GUIContext& GetContext() noexcept { return *m_context; }
        [[nodiscard]] const RefPtr<GUIContext>& GetContextRef() const noexcept { return m_context; }
        [[nodiscard]] GUIContext* GetGUIContext() override { return m_context.Get(); }

        /// Runs this root's layout pass: every container that requested a layout, parents first,
        /// repeated while a layout requests more (a child's new size can move its parent), for at
        /// most a few rounds. A host runs it after styling and before drawing; Update runs it too.
        void UpdateLayout()
        {
            PROFILE_SCOPE("GUI.Layout");
            const core::Stopwatch watch = core::Stopwatch::StartNew();
            m_lastLayoutCount = Node::UpdateLayout();
            m_eventDispatcher.PlacePopups(); // popups follow anchors that moved
            m_frameStats.LayoutMs += watch.Elapsed().AsMilliseconds(); // accumulates, since the GUI's Update lays out too
            m_frameStats.LaidOut += m_lastLayoutCount;
        }
        /// How many containers the last layout pass laid out (none in a frame where nothing moved).
        [[nodiscard]] u32 LastLayoutCount() const noexcept { return m_lastLayoutCount; }

        /// Whether this root has anything to show that it has not drawn: something invalidated, an
        /// animation running on a node it shows, a layout or a binding waiting, or a direction
        /// held. An action on a node out of view still runs (the GUI's Update ticks it either way),
        /// but it asks for no frame until its effect becomes visible. A host skips a frame that
        /// needs none, so an idle GUI costs nothing.
        /// The reasons found go to the frame stats. The call also closes the frame's style and
        /// layout phases: they become the last frame's stats, which a draw then completes. A host
        /// calls it once a frame, after styling and layout.
        [[nodiscard]] bool NeedsFrame()
        {
            FrameReasons& why = m_frameStats.Reasons;
            // A bake finished since this root last drew, so what drew as paths while waiting now
            // draws from the baked texture.
            if (const IPictureBaker* baker = m_context->GetPictureBaker(); baker != nullptr && baker->Generation() != m_bakeGeneration)
                Invalidate();
            why.Redraw = NeedsRedraw();
            why.Layout = IsLayoutDirty() || HasLayoutDirtyDescendant();
            why.Pending = m_context->m_effects.HasPending();
            why.HeldDirection = m_eventDispatcher.IsHoldingDirection();
            const Node* animated = nullptr;
            if (!IsPaused())
                (void)m_context->m_actionManager.AnyAction([this, &animated](const Action& action, Node& target)
                {
                    if (target.GetSceneRoot() != this || !target.IsTreeVisible())
                        return false;
                    // When the target was out of view at the last frame (culled, or under something
                    // culled), an animation that cannot bring it into view changes nothing visible.
                    if (target.WasOutOfView() && !action.CanMoveTarget())
                        return false;
                    animated = &target;
                    return true;
                });
            why.Animation = animated != nullptr;
            m_frameStats.AnimatedNode = animated != nullptr ? animated->DebugName() : core::String{};
            why.Overlay = m_debugOverlay.RedrawFlash && !m_flashes.IsEmpty();
            const bool needed = why.Any();
            PublishFrame();
            return needed;
        }

        /// Draws the tree as Draw does, timing it and counting what it produced. A host draws its
        /// frame with this; a frame drawn through Draw alone is not counted.
        void DrawFrame(DrawContext& ctx)
        {
            PROFILE_SCOPE("GUI.Draw");
            if (!m_framePublished)
                PublishFrame(); // for a host that draws every frame without calling NeedsFrame
            const core::Stopwatch watch = core::Stopwatch::StartNew();
            const vg::VGBatch& before = ctx.VG().GetBatch();
            const usize vertices = before.vertices.Size() + before.boxVertices.Size();
            const usize commands = before.commands.Size();
            const u32 drawn = ctx.NodesDrawn();
            const u32 culled = ctx.NodesCulled();
            ctx.SetFrame(static_cast<u32>(m_framesDrawn + 1)); // drawn nodes record it (FramesDrawn is set afterwards)
            ctx.SetPictureBaker(m_context->GetPictureBaker());
            m_bakeGeneration = m_context->GetPictureBaker() != nullptr ? m_context->GetPictureBaker()->Generation() : 0;
            m_frameRedraws.Clear();
            if (m_debugOverlay.RedrawFlash)
                ctx.SetRedrawRecorder(&m_frameRedraws);
            // Damage: what changed since the last frame. The nodes' old areas were noted as they
            // changed, and their new areas are noted as they draw.
            m_lastDamage = core::Move(m_pendingDamage);
            m_pendingDamage = core::Array<Rect>{};
            ctx.SetDamageRecorder(&m_lastDamage);
            // The window bounds the drawing, so anything outside it is culled. This affects only
            // drawing; navigation and hit testing still reach a node past it.
            // The clip is pushed outside the root's own transform, so it is the root's size in the
            // device pixels it draws to (its layout size times its display scale), not its layout
            // size. On a scaled display the layout size would cut the drawing to a corner (1/1.75
            // of it at 175%).
            const bool sized = GetSize().x > 0.0f && GetSize().y > 0.0f;
            if (sized)
                ctx.PushClip(Rect{GetPosition().x, GetPosition().y, GetSize().x * m_displayScale, GetSize().y * m_displayScale});
            Draw(ctx);
            if (sized)
                ctx.PopClip();
            ctx.SetRedrawRecorder(nullptr);
            ctx.SetDamageRecorder(nullptr);
            m_lastDamageBounds = Rect{};
            for (usize i = 0; i < m_lastDamage.Size(); ++i)
                m_lastDamageBounds = i == 0 ? m_lastDamage[i] : Rect::Union(m_lastDamageBounds, m_lastDamage[i]);
            const vg::VGBatch& batch = ctx.VG().GetBatch(); // read after the flush, so its last command counts
            m_lastFrame.DrawMs = watch.Elapsed().AsMilliseconds();
            m_lastFrame.NodesDrawn = ctx.NodesDrawn() - drawn;
            m_lastFrame.NodesCulled = ctx.NodesCulled() - culled;
            m_lastFrame.Vertices = static_cast<u32>(batch.vertices.Size() + batch.boxVertices.Size() - vertices);
            m_lastFrame.Commands = static_cast<u32>(batch.commands.Size() - commands);
            m_lastFrame.FramesDrawn = ++m_framesDrawn;
            m_framePublished = false;
        }
        /// The areas the last frame changed, in device pixels: each node's area where it is new,
        /// changed, moved or gone, and the bounds of them all. A host that can present part of a
        /// frame (or redraw a HUD over its scene) redraws this much. It is empty when nothing
        /// changed.
        [[nodiscard]] const core::Array<Rect>& GetLastDamage() const noexcept { return m_lastDamage; }
        [[nodiscard]] const Rect& GetLastDamageBounds() const noexcept { return m_lastDamageBounds; }
        void AddDamage(const Rect& device) override { m_pendingDamage.PushBack(device); }

        /// The stats of the frame in progress, which each phase adds to.
        [[nodiscard]] FrameStats* GetFrameStats() override { return &m_frameStats; }
        /// The stats of the last frame: its style, layout and reasons (as NeedsFrame closed them),
        /// and its draw (none when the frame was skipped).
        [[nodiscard]] const FrameStats& GetLastFrameStats() const noexcept { return m_lastFrame; }
        [[nodiscard]] u32 GetLastDrawnFrame() const override { return static_cast<u32>(m_framesDrawn); }

        // === This root's time on the GUI's clock ===
        /// Sets the time scale of this root's animations (1 is real time, 0.5 half speed). A paused
        /// root's animations stand still.
        void SetTimeScale(f32 scale) noexcept { m_timeScale = core::Max(0.0f, scale); }
        [[nodiscard]] f32 GetTimeScale() const noexcept { return m_timeScale; }
        void SetPaused(bool paused) noexcept { m_paused = paused; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }
        [[nodiscard]] f32 EffectiveTimeScale() const noexcept { return m_paused ? 0.0f : m_timeScale; }

        [[nodiscard]] EffectScheduler* GetEffectScheduler() override { return &m_context->m_effects; }
        [[nodiscard]] ActionManager* GetActionManager() override { return &m_context->m_actionManager; }
        [[nodiscard]] MutationQueue* GetMutationQueue() override { return &m_context->m_mutationQueue; }
        [[nodiscard]] EventDispatcher* GetEventDispatcher() override { return &m_eventDispatcher; }
        [[nodiscard]] fonts::IFontService* GetFontService() override { return m_context->GetFontService(); }
        [[nodiscard]] IResourceProvider* GetResourceProvider() override { return m_context->GetResourceProvider(); }
        [[nodiscard]] ElementTable* GetElementTable() override { return m_context->m_elements.Get(); }
        [[nodiscard]] const ElementSet* GetElementSet() const override { return &m_context->m_elementSet; }

        // === Commands, along this root's focus path ===
        /// Runs command `id` along this root's focus path (just the root when nothing here is
        /// focused): capture handlers from the root down, then bubble handlers from the focused
        /// widget up. The first bubble handler stops it unless it sets Propagate. Returns false
        /// when the command is disabled or nothing handled it.
        bool RunCommand(core::StringView id)
        {
            if (const Command* command = m_context->GetCommands().Find(id); command != nullptr && !Untracked([&] { return command->IsEnabled(); }))
                return false;
            // Back closes the topmost auto popup before anything under it hears the command, since
            // the top layer is on top whether or not it holds the focus (a combo box's list).
            if (id == core::StringView(kBackCommand) && m_eventDispatcher.CloseTopAutoPopup(PopupCloseReason::Escape))
                return true;
            // The path from the focused node up to this root or a modal popup (below which all is
            // inert), focused node first. References are held for the walk, since a handler may
            // destroy nodes on it (a floating dock window emptied by its own move).
            Array<RefPtr<Node>> path(MemoryAllocator());
            for (Node* n = CommandStart(); n != nullptr; n = n->GetParent())
            {
                path.PushBack(RefPtr<Node>(n));
                if (n->IsRouteStop())
                    break;
            }
            bool ran = false;
            CommandEvent event{id, CommandPhase::Capture, false, false};
            for (usize i = path.Size(); i-- > 0;)
            {
                ran |= path[i]->RunCommandHandlers(event);
                if (event.Stop)
                    return true; // a capture handler took it (a modal wrapper, a mode that owns the key)
            }
            event.Phase = CommandPhase::Bubble;
            for (const RefPtr<Node>& n : path)
            {
                event.Propagate = false;
                if (n->RunCommandHandlers(event))
                {
                    ran = true;
                    if (!event.Propagate)
                        break;
                }
            }
            return ran;
        }

        /// Whether command `id` is enabled and a handler for it is on this root's focus path. A
        /// binding that reads it follows the focus, the command's state and the handlers.
        [[nodiscard]] bool IsCommandAvailable(core::StringView id)
        {
            m_context->GetCommands().TrackChanges();
            (void)m_eventDispatcher.ObserveFocus();
            if (const Command* command = m_context->GetCommands().Find(id); command != nullptr && !command->IsEnabled())
                return false;
            if (id == core::StringView(kBackCommand) && m_eventDispatcher.HasTopAutoPopup())
                return true;
            for (Node* n = CommandStart(); n != nullptr; n = n->GetParent())
            {
                if (n->HandlesCommand(id))
                    return true;
                if (n->IsRouteStop())
                    break;
            }
            return false;
        }

        /// Returns the key contexts in force at `from`, which the keymap matches against there:
        /// root first, one layer for each node from `from` up to this root or a modal popup that
        /// declares a context.
        [[nodiscard]] Array<KeyContextLayer> KeyContextsAt(Node* from) const
        {
            Array<KeyContextLayer> stack;
            Array<Node*> path;
            for (Node* n = from; n != nullptr; n = n->GetParent())
            {
                path.PushBack(n);
                if (n->IsRouteStop())
                    break;
            }
            KeyContextLayer layer;
            for (usize i = path.Size(); i-- > 0;)
                if (path[i]->GetKeyContext().Size() != 0)
                {
                    layer.Declare(path[i]->GetKeyContext());
                    stack.PushBack(layer);
                }
            return stack;
        }
        /// Ends a chord that was cut short. A binding for exactly the keys typed so far runs (such
        /// as a binding of the first key alone, which waited to see whether the chord went on).
        void FinishChord()
        {
            Array<Keystroke>& chord = m_context->m_chord;
            core::String command;
            const Array<KeyContextLayer> stack = KeyContextsAt(CommandStart());
            const bool whole = !chord.IsEmpty() && m_context->GetKeymap().MatchWhole(chord.AsSpan(), stack.AsSpan(), command);
            chord.Clear();
            if (whole)
                (void)RunCommand(command.AsView());
        }

        /// Matches a keystroke that no widget used against the GUI's keymap, in the contexts of this
        /// root's focus path; a chord waits for its next keystroke, and a match runs its command.
        /// Returns true when the keystroke was taken (a match, or the start of a chord).
        bool RouteKeystroke(const Keystroke& stroke)
        {
            const Array<KeyContextLayer> stack = KeyContextsAt(CommandStart()); // root first
            Array<Keystroke>& chord = m_context->m_chord;
            chord.PushBack(stroke);
            m_context->m_chordAge = 0.0f;
            core::String command;
            KeymapMatch match = m_context->GetKeymap().Match(chord.AsSpan(), stack.AsSpan(), command);
            if (match == KeymapMatch::None && chord.Size() > 1)
            {
                // The chord was broken off. The keys before this one run if they form a whole
                // binding (the first key's own binding), then this keystroke is matched alone.
                chord.PopBack();
                FinishChord();
                chord.PushBack(stroke);
                match = m_context->GetKeymap().Match(chord.AsSpan(), stack.AsSpan(), command);
            }
            if (match == KeymapMatch::Pending)
                return true;
            chord.Clear();
            if (match == KeymapMatch::None || command.Size() == 0)
                return false; // no binding, or unbound here: treated as unbound
            // When nothing on the focus path handles the command (a menu's keys on a bar button with
            // no menu open), treat the key as unbound so it goes on to navigation. A disabled
            // command that has a handler still takes the key.
            bool handled = command.AsView() == core::StringView(kBackCommand) && m_eventDispatcher.HasTopAutoPopup();
            for (Node* n = CommandStart(); n != nullptr && !handled; n = n->GetParent())
            {
                handled = n->HandlesCommand(command.AsView());
                if (n->IsRouteStop())
                    break;
            }
            if (!handled)
                return false;
            (void)RunCommand(command.AsView());
            return true;
        }

        /// Sets the device pixels per logical unit: the window's content scale times a UI scale.
        /// The tree lays out, styles and handles input in logical units, and the root's transform
        /// maps them to device pixels (input in device pixels hit-tests through it). The root's
        /// size follows: the viewport's device pixels divided by the scale.
        void SetDisplayScale(f32 scale)
        {
            const f32 was = m_displayScale;
            m_displayScale = core::Max(scale, 0.01f);
            Transformable::SetScale(core::Float2{m_displayScale, m_displayScale});
            if (m_displayScale != was)
            {
                RequestLayoutTree(); // every edge must snap to the new pixel grid
                Invalidate();        // only on a change; setting the same scale again makes no damage
            }
            ApplyViewport();
        }
        [[nodiscard]] f32 GetDisplayScale() const noexcept { return m_displayScale; }

        /// Sets what the debug overlay shows over this GUI's tree (nothing by default).
        void SetDebugOverlay(const DebugOverlay& overlay)
        {
            if (overlay == m_debugOverlay)
                return;
            m_debugOverlay = overlay;
            Invalidate();
        }
        [[nodiscard]] const DebugOverlay& GetDebugOverlay() const noexcept { return m_debugOverlay; }
        /// Sets the font the overlay uses to name nodes and number tab stops. Without one it draws
        /// no text.
        void SetDebugFont(fonts::CachedFont* font) noexcept { m_debugFont = font; }

    protected:
        [[nodiscard]] f32 TopPixelScale() const override { return m_displayScale; }
        // A node with no `lang` above it uses the GUI's language (from its Localization).
        [[nodiscard]] core::String ContextLanguage() const override
        {
            const foundation::i18n::Localization* loc = m_context->GetLocalization();
            return loc != nullptr ? core::String(loc->Language()) : core::String{};
        }
        [[nodiscard]] bool ContextRightToLeft() const override
        {
            const foundation::i18n::Localization* loc = m_context->GetLocalization();
            return loc != nullptr && loc->IsRightToLeft();
        }
        void OnDrawAbove(DrawContext& ctx) override
        {
            m_eventDispatcher.DrawPopups(ctx); // draws the top layer after the tree
            if (m_debugOverlay.AnyLayout())
                DrawLayoutOverlay(ctx, m_debugOverlay);
            if (m_debugOverlay.RedrawFlash)
                DrawRedrawFlashes(ctx);
            if (m_debugOverlay.AnyInput())
                DrawInputOverlay(ctx);
        }

    private:
        // Draws the input part of the debug overlay in this root's space (DebugOverlayImpl.cpp).
        void DrawInputOverlay(DrawContext& ctx) const;
        // Adds this frame's redraws to the flashes, which fade over half a second, and draws them
        // (DebugOverlayImpl.cpp).
        void DrawRedrawFlashes(DrawContext& ctx);

    public:
        /// How many of the debug overlay's redraw flashes are still fading.
        [[nodiscard]] usize RedrawFlashCount() const noexcept { return m_flashes.Size(); }

    private:
        struct RedrawFlash
        {
            Rect Box; // in this root's space
            core::Stopwatch Age;
        };
        // `node`'s border box in this root's space.
        [[nodiscard]] Rect InRootSpace(const Node& node) const
        {
            return (GetTransform().GetInverse() * node.GetWorldTransform()).TransformRect(node.GetLocalBounds());
        }

    public:

        /// Sets the device pixels this root covers (its window's or target's). The root's size is
        /// that viewport in logical units.
        void SetViewportSize(core::Float2 devicePixels)
        {
            m_viewport = devicePixels;
            ApplyViewport();
        }
        [[nodiscard]] core::Float2 GetViewportSize() const noexcept { return m_viewport; }

        /// Returns what this root's media queries see: its logical size and its density (96 dpi at
        /// 1x), among others.
        [[nodiscard]] MediaContext GetMediaContext() const noexcept
        {
            MediaContext context{GetSize().x, GetSize().y, 96.0f * m_displayScale};
            context.DarkScheme = m_context->IsDarkScheme();
            context.Contrast = m_context->GetContrastPreference();
            context.ReducedMotion = m_context->IsReducedMotion();
            context.Input = m_eventDispatcher.GetInputKind(); // `@media (input: pad)`
            return context;
        }

        // Per-frame hook for subclasses (layout, timers, ...).
        virtual void OnUpdate(core::Duration elapsed) { (void)elapsed; }

    private:
        friend class GUIContext;
        RefPtr<GUIContext> m_context;            // the GUI this root is part of (declared first, so it outlives the other members)
        bool m_accessSent = false;               // the whole tree has been sent to the bridge
        u64 m_accessFocus = 0;                   // the focus last sent to the bridge
        EventDispatcher m_eventDispatcher{this}; // this root's own pointer, hover and focus
        f32 m_displayScale = 1.0f;
        DebugOverlay m_debugOverlay;
        core::Array<Rect> m_frameRedraws;     // nodes drawn this frame that had invalidated themselves (world space)
        core::Array<Rect> m_pendingDamage;    // old areas noted since the last frame (device pixels)
        core::Array<Rect> m_lastDamage;       // the last frame's damage
        Rect m_lastDamageBounds;
        core::Array<RedrawFlash> m_flashes;   // flashes still fading
        fonts::CachedFont* m_debugFont = nullptr; // the overlay's text font (owned by the host)
        FrameStats m_frameStats; // the frame in progress
        FrameStats m_lastFrame;
        u64 m_framesDrawn = 0;
        u64 m_bakeGeneration = 0; // the picture baker's generation as of the last frame drawn
        bool m_framePublished = false;

        // Called when the frame's style and layout are done: their stats become the last frame's,
        // still without a draw.
        void PublishFrame()
        {
            m_lastFrame = core::Move(m_frameStats);
            m_lastFrame.FramesDrawn = m_framesDrawn;
            m_frameStats = FrameStats{};
            m_framePublished = true;
        }
        f32 m_timeScale = 1.0f;
        bool m_paused = false;
        u32 m_lastLayoutCount = 0;
        core::Float2 m_viewport{0.0f, 0.0f};

        // The focused node when it is in this tree, else this root.
        [[nodiscard]] Node* FocusInThisTree()
        {
            Node* focus = m_eventDispatcher.GetFocusNode();
            for (Node* n = focus; n != nullptr; n = n->GetParent())
                if (n == this)
                    return focus;
            return this;
        }
        // Enters the first visible, enabled node from `node` down (pre-order) that takes keyboard
        // entry. Returns false when none does.
        static bool EnterFromKeyboardIn(Node& node)
        {
            if (!node.IsVisible() || !node.IsTreeEnabled())
                return false;
            if (&node != node.GetRootNode() && node.EnterFromKeyboard())
                return true;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (EnterFromKeyboardIn(*node.GetChildAt(i)))
                    return true;
            return false;
        }
        // Where a command or a keystroke starts: the focus in this tree, else the root. When the
        // focus is below a modal popup, it starts at the modal popup, since everything under it is
        // inert, including its bindings.
        [[nodiscard]] Node* CommandStart()
        {
            Node* start = FocusInThisTree();
            return m_eventDispatcher.IsInputLive(start) ? start : m_eventDispatcher.GetFocusScopeRoot();
        }

        void ApplyViewport()
        {
            if (m_viewport.x > 0.0f || m_viewport.y > 0.0f)
                SetSize(core::Float2{m_viewport.x / m_displayScale, m_viewport.y / m_displayScale});
        }
    };

    inline void GUIContext::Update(core::Duration elapsed)
    {
        RunAccessRequests(); // run what a platform requested since the last update, so this frame shows it
        const f32 seconds = static_cast<f32>(elapsed.AsSecondsF());
        // Limit the clock's step, so a hitch slows the motion rather than skipping it.
        const core::Duration step = m_maxClockStep > 0.0f && seconds > m_maxClockStep
                                        ? core::Duration::FromSeconds(static_cast<f64>(m_maxClockStep))
                                        : elapsed;
        m_actionManager.Update(step,
                               [](Node& target)
                               {
                                   const SceneRoot* root = target.GetSceneRoot();
                                   return root != nullptr ? root->EffectiveTimeScale() : 1.0f;
                               });
        if (!m_chord.IsEmpty() && (m_chordAge += seconds) > kChordTimeout)
        {
            // A chord not finished in time: run the binding for the keys typed so far (such as the
            // first key alone).
            if (SceneRoot* root = GetActiveRoot())
                root->FinishChord();
            m_chord.Clear();
        }
        // A root's hook may add or remove roots, so walk a copy.
        const Array<SceneRoot*> roots = m_roots;
        for (SceneRoot* root : roots)
        {
            root->m_eventDispatcher.Tick(seconds);
            root->OnUpdate(elapsed);
        }
        m_lastBindingCount = m_effects.Flush();
        m_mutationQueue.Drain();
        for (SceneRoot* root : m_roots)
            root->UpdateLayout();
        FlushAccessibility();
    }

    inline void GUIContext::AddRoot(SceneRoot& root)
    {
        m_roots.PushBack(&root);
        root.GetEventDispatcher()->SetRootPlaces(m_rootPlaces);
    }
    inline void GUIContext::RemoveRoot(SceneRoot& root)
    {
        for (usize i = 0; i < m_roots.Size(); ++i)
            if (m_roots[i] == &root)
            {
                m_roots.RemoveAt(i);
                break;
            }
        for (SceneRoot* other : m_roots)
            other->GetEventDispatcher()->ForgetRoot(*root.GetEventDispatcher());
        if (m_active == &root)
            SetActiveRoot(nullptr);
    }
    inline void GUIContext::SetRootPlaces(IRootPlaces* places)
    {
        m_rootPlaces = places;
        for (SceneRoot* root : m_roots)
            root->GetEventDispatcher()->SetRootPlaces(places);
    }

    inline void GUIContext::LanguageChanged()
    {
        m_languageChanged.Notify(); // translated properties resolve again
        for (SceneRoot* root : m_roots)
            root->DirectionChanged(); // :lang may now match differently, and the new language's direction may differ
    }

    inline void GUIContext::TookInput(SceneRoot& root)
    {
        for (SceneRoot* other : m_roots)
            if (other != &root)
                other->GetEventDispatcher()->CloseLightPopups();
        SetActiveRoot(&root);
    }

    inline bool GUIContext::RunCommand(core::StringView id)
    {
        SceneRoot* root = GetActiveRoot();
        return root != nullptr && root->RunCommand(id);
    }

    inline bool GUIContext::IsCommandAvailable(core::StringView id)
    {
        m_activeChanged.Track();
        SceneRoot* root = GetActiveRoot();
        return root != nullptr && root->IsCommandAvailable(id);
    }
}

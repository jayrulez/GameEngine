// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.ModelViewer - `modelviewer.preferences`: the viewer's Preferences, a modal window built
// from Data/ModelViewer/Preferences.xml, as the editor's Preferences is built from its markup. It
// opens showing the current settings. Save gathers every control into one settings.set call (the
// same command an agent runs) and stays open with the reason when that is refused; Cancel or
// closing the window discards the changes. Clear cache runs cache.clear immediately.

module;
#include "Core/Prelude.h"

export module modelviewer.preferences;

import foundation.core;
import foundation.gui;
import foundation.json;
import modelviewer;

using namespace foundation::core;

export namespace modelviewer
{
    namespace gui = foundation::gui;
    using foundation::json::JsonValue;

    class PreferencesDialog
    {
    public:
        static constexpr const char8_t* kPath = u8"ModelViewer/Preferences.xml";

        struct Services
        {
            /// Runs settings.set with the controls' values; returns empty on success, else the reason.
            Function<String(const JsonValue&)> save;
            /// Runs cache.clear; returns the message to show (the bytes freed, or why it was refused).
            Function<String()> clearCache;
            /// The cache's current size, for display.
            Function<String()> cacheSize;
            /// Lets the user choose a folder for a field in a native dialog, and passes it to the callback.
            Function<void(StringView start, Function<void(StringView)>)> chooseFolder;
            /// Where screenshots go when the field is empty, and the default cache folder, for display.
            String defaultScreenshots;
            String defaultCache;
        };

        PreferencesDialog(IAllocator& allocator, Services services, RefPtr<gui::Node> body)
            : m_allocator(allocator), m_services(foundation::core::Move(services)), m_body(foundation::core::Move(body))
        {
            if (!m_body)
                return;
            m_window = MakeRef<gui::Window>(m_allocator);
            m_window->SetTitle(u8"Preferences");
            m_window->AddClass(u8"preferences-window");
            m_window->SetSize(Float2{kWidth, kHeight});
            m_window->GetContent()->AddChild(m_body.Get());
            Bind();
        }
        PreferencesDialog(const PreferencesDialog&) = delete;
        PreferencesDialog& operator=(const PreferencesDialog&) = delete;
        ~PreferencesDialog()
        {
            if (m_window && m_window->IsOpen())
                m_window->Close();
        }

        /// Opens the dialog over `root` with `settings` in its controls; false when the markup is
        /// missing or the dialog is already open.
        bool Open(gui::Node& root, const ViewerSettings& settings)
        {
            if (!m_window || m_window->IsOpen())
                return false;
            Show(settings);
            m_window->OpenModal(root);
            return true;
        }
        [[nodiscard]] bool IsOpen() const noexcept { return m_window && m_window->IsOpen(); }

        /// The controls' values as settings.set takes them.
        [[nodiscard]] JsonValue Gathered() const
        {
            JsonValue v = JsonValue::MakeObject();
            auto text = [&](StringView key, StringView id)
            {
                if (auto* field = Find<gui::TextField>(id))
                    v.Set(String(key), JsonValue::MakeString(String(field->GetText())));
            };
            auto check = [&](StringView key, StringView id)
            {
                if (auto* box = Find<gui::CheckBox>(id))
                    v.Set(String(key), JsonValue::MakeBool(box->IsChecked()));
            };
            auto number = [&](StringView key, StringView id)
            {
                if (auto* slider = Find<gui::Slider>(id))
                    v.Set(String(key), JsonValue::MakeNumber(slider->GetValue()));
            };
            text(u8"screenshotFolder", u8"pref-shot-folder");
            check(u8"askScreenshot", u8"pref-shot-ask");
            text(u8"screenshotPattern", u8"pref-shot-pattern");
            for (StringView density : kDensities)
                if (auto* segment = Find<gui::Button>(Format(u8"pref-density-{}", density).AsView()); segment != nullptr && segment->IsChecked())
                    v.Set(String(u8"density"), JsonValue::MakeString(String(density)));
            number(u8"recentCount", u8"pref-recent-count");
            check(u8"loop", u8"pref-loop");
            check(u8"looseTextures", u8"pref-loose");
            check(u8"grid", u8"pref-grid");
            number(u8"turntableSpeed", u8"pref-turntable");
            number(u8"exposure", u8"pref-exposure");
            number(u8"ambient", u8"pref-ambient");
            number(u8"sky", u8"pref-sky");
            for (const u32 size : kThumbnailSizes)
                if (auto* segment = Find<gui::Button>(Format(u8"pref-thumb-{}", size).AsView()); segment != nullptr && segment->IsChecked())
                    v.Set(String(u8"thumbnailSize"), JsonValue::MakeNumber(size));
            text(u8"cacheFolder", u8"pref-cache-folder");
            return v;
        }

        /// Saves the controls through settings.set, closing the dialog on success.
        bool Save()
        {
            const String refused = m_services.save ? m_services.save(Gathered()) : String{};
            if (!refused.IsEmpty())
            {
                if (auto* help = Find<gui::Label>(u8"pref-shot-help"))
                {
                    help->SetText(refused.AsView());
                    help->AddClass(u8"refused");
                }
                return false;
            }
            m_window->Close();
            return true;
        }

        [[nodiscard]] gui::Window* Window() const noexcept { return m_window.Get(); }

    private:
        static constexpr f32 kWidth = 580.0f;
        static constexpr f32 kHeight = 460.0f;
        static constexpr StringView kDensities[] = {u8"compact", u8"default", u8"comfortable"};

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_body ? Cast<T>(FindIn(*m_body, id)) : nullptr;
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        // Wires the controls once: the sliders' value labels, the folder buttons, and the dialog buttons.
        void Bind()
        {
            PreferencesDialog* self = this;
            const auto onChange = [](gui::Node* node, Function<void()> apply)
            {
                if (node != nullptr)
                    (void)node->AddEventListener(gui::EventType::Changed, [applyChange = foundation::core::Move(apply)](const gui::Event&) { applyChange(); });
            };
            for (StringView id : kSliders)
            {
                const String name(id);
                onChange(Find<gui::Slider>(id), [self, name]() { self->ShowValue(name.AsView()); });
            }
            auto browse = [self](StringView button, StringView field)
            {
                const String target(field);
                if (auto* b = self->Find<gui::Button>(button))
                    b->SetOnClick([self, target]()
                                  {
                                      auto* f = self->Find<gui::TextField>(target.AsView());
                                      if (f == nullptr || !self->m_services.chooseFolder)
                                          return;
                                      self->m_services.chooseFolder(f->GetText(), [self, target](StringView chosen)
                                                                    {
                                                                        if (auto* into = self->Find<gui::TextField>(target.AsView()))
                                                                            into->SetText(chosen);
                                                                    });
                                  });
            };
            browse(u8"pref-shot-browse", u8"pref-shot-folder");
            browse(u8"pref-cache-browse", u8"pref-cache-folder");
            if (auto* clear = Find<gui::Button>(u8"pref-cache-clear"))
                clear->SetOnClick([self]()
                                  {
                                      if (!self->m_services.clearCache)
                                          return;
                                      const String said = self->m_services.clearCache();
                                      if (auto* result = self->Find<gui::Label>(u8"pref-cache-result"))
                                          result->SetText(said.AsView());
                                      self->ShowCacheSize();
                                  });
            if (auto* save = Find<gui::Button>(u8"pref-save"))
                save->SetOnClick([self]() { (void)self->Save(); });
            if (auto* cancel = Find<gui::Button>(u8"pref-cancel"))
                cancel->SetOnClick([self]() { self->m_window->Close(); });
        }

        static constexpr StringView kSliders[] = {u8"pref-recent-count", u8"pref-turntable", u8"pref-exposure", u8"pref-ambient", u8"pref-sky"};

        void ShowValue(StringView slider)
        {
            const auto* s = Find<gui::Slider>(slider);
            auto* value = Find<gui::Label>(Format(u8"{}-value", slider).AsView());
            if (s == nullptr || value == nullptr)
                return;
            const f64 v = s->GetValue();
            if (slider == StringView(u8"pref-recent-count"))
                value->SetText(Format(u8"{}", static_cast<i32>(v + 0.5)).AsView());
            else if (slider == StringView(u8"pref-turntable"))
                value->SetText(Format(u8"{} degrees a second", static_cast<i32>(v + 0.5)).AsView());
            else
                value->SetText(FormatFixed(v, 2).AsView());
        }

        void ShowCacheSize()
        {
            if (auto* size = Find<gui::Label>(u8"pref-cache-size"); size != nullptr && m_services.cacheSize)
                size->SetText(m_services.cacheSize().AsView());
        }

        // Sets the controls from `s`.
        void Show(const ViewerSettings& s)
        {
            auto text = [&](StringView id, StringView value)
            {
                if (auto* field = Find<gui::TextField>(id))
                    field->SetText(value);
            };
            auto check = [&](StringView id, bool on)
            {
                if (auto* box = Find<gui::CheckBox>(id))
                    box->SetChecked(on);
            };
            auto number = [&](StringView id, f64 v)
            {
                if (auto* slider = Find<gui::Slider>(id))
                    slider->SetValue(v);
                ShowValue(id);
            };
            text(u8"pref-shot-folder", s.screenshotFolder.AsView());
            if (auto* field = Find<gui::TextField>(u8"pref-shot-folder"))
                field->SetPlaceholder(m_services.defaultScreenshots.AsView());
            if (auto* help = Find<gui::Label>(u8"pref-shot-help"))
            {
                help->RemoveClass(u8"refused");
                help->SetText(u8"Empty: the Pictures folder's ModelViewer, as shown. The Screenshot button saves here unless it asks; an agent's always do.");
            }
            check(u8"pref-shot-ask", s.askScreenshot);
            text(u8"pref-shot-pattern", s.screenshotPattern.AsView());
            for (StringView density : kDensities)
                if (auto* segment = Find<gui::Button>(Format(u8"pref-density-{}", density).AsView()))
                    segment->SetChecked(s.density.AsView() == density);
            number(u8"pref-recent-count", s.recentCount);
            check(u8"pref-loop", s.loop);
            check(u8"pref-loose", s.looseTextures);
            check(u8"pref-grid", s.grid);
            number(u8"pref-turntable", s.turntableSpeed);
            number(u8"pref-exposure", s.exposure);
            number(u8"pref-ambient", s.ambient);
            number(u8"pref-sky", s.sky);
            for (const u32 size : kThumbnailSizes)
                if (auto* segment = Find<gui::Button>(Format(u8"pref-thumb-{}", size).AsView()))
                    segment->SetChecked(s.thumbnailSize == size);
            text(u8"pref-cache-folder", s.cacheFolder.AsView());
            if (auto* field = Find<gui::TextField>(u8"pref-cache-folder"))
                field->SetPlaceholder(m_services.defaultCache.AsView());
            if (auto* help = Find<gui::Label>(u8"pref-cache-help"))
                help->SetText(u8"Empty: the viewer's folder in the user data folder, as shown. A new folder is used from the viewer's next start.");
            if (auto* result = Find<gui::Label>(u8"pref-cache-result"))
                result->SetText(u8"The cooked models and the thumbnails; each is made again when next needed.");
            ShowCacheSize();
        }

        IAllocator& m_allocator;
        Services m_services;
        RefPtr<gui::Node> m_body;
        RefPtr<gui::Window> m_window;
    };
}

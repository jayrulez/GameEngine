// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Core - :actions partition.
//
// Editor actions: one declaration per user command, and the registry every surface is built
// from: the menu bar (from menuPath), the shortcuts (from the effective chord), the toolbars (by
// id), the command palette, Preferences > Shortcuts, and the MCP action bridge. A surface never
// binds a command of its own; it asks the registry whether an action is enabled or checked and
// tells it to Execute. The declaration carries what every surface needs to show the action, and
// the three bindings that make up the command: execute, enabled (queried, never pushed) and
// checked (the state of a Toggle or Window). Actions take no arguments from any surface or from an
// agent. They run over a subject page the surface supplies: the active page for the menu bar, a
// chord, the palette and the MCP bridge (the registry asks its ActiveSubject), and the toolbar's
// own page for a page's toolbar, since a split layout shows two pages and only one is active.
// Anything that takes a real parameter is a tool or a list-driven menu, not an action.
//
// Registration is explicit, in the composition roots (the application for the editor-wide set,
// each domain's Register<Domain>Editor for its own), like pages, creators and the MCP tool
// contributions. An id registered twice is a programming error at boot, refused and logged, so
// the surfaces never see two actions with one name.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include <type_traits> // std::is_base_of_v (ServiceOf)

export module editor.core:actions;

import foundation.core;
import foundation.gui; // Keystroke (a chord), Observable (what changes), Menu (action items)
import :page;

using namespace foundation::core;

export namespace editor
{
    /// A keyboard chord, the GUI keymap's keystroke; unset (no key) means the action has no
    /// shortcut. Its text form is the one a binding uses ("ctrl-shift-z", through Of and Keys),
    /// which the keymap binds and the settings store.
    struct EditorShortcut
    {
        foundation::gui::Keystroke stroke{};

        /// The chord `keys` names ("ctrl-s", "f5", "shift-delete"); unset when it names none.
        [[nodiscard]] static EditorShortcut Of(StringView keys)
        {
            const Optional<foundation::gui::Keystroke> parsed = foundation::gui::ParseKeystroke(keys);
            return parsed.HasValue() ? EditorShortcut{parsed.Value()} : EditorShortcut{};
        }
        [[nodiscard]] bool IsSet() const noexcept { return stroke.Code != 0; }
        [[nodiscard]] bool operator==(const EditorShortcut&) const noexcept = default;
        /// The chord as a binding writes it ("ctrl-shift-z"); empty when unset.
        [[nodiscard]] String Keys() const { return IsSet() ? foundation::gui::KeystrokeName(stroke) : String{}; }
    };

    /// The chord as a shortcut column or a tool result shows it, as the GUI's menus do:
    /// "Ctrl+Shift+Z", "F5", "Del"; empty when unset.
    inline String FormatShortcut(EditorShortcut chord)
    {
        return chord.IsSet() ? foundation::gui::KeystrokeText(chord.stroke) : String{};
    }

    enum class EditorActionKind : u8
    {
        Command, // does something
        Toggle,  // flips a state the surfaces show as checked
        Window,  // shows or hides a panel; checked while shown
    };

    struct EditorActionDeclaration
    {
        /// Stable and dotted: "file.save", "edit.undo", "scene.simulate.start". The shortcut
        /// overrides, the palette and the MCP bridge identify the action by it; it is never shown
        /// as a label.
        String id;
        String label;       // "Save" (menus, toolbars)
        String description; // "Save the active page" (palette, tooltips, MCP)
        /// The menu bar is generated from these: "File/Save", "Edit/Undo", "Scene/Simulate/
        /// Start". Empty means not in the menu bar. Items are ordered by menuOrder within a menu,
        /// with a separator between order bands (hundreds).
        String menuPath;
        i32 menuOrder = 0;
        /// An icon by name, resolved by the surface that draws it; empty means text only.
        String icon;
        EditorShortcut shortcut; // the default; the user's override wins (Rebind)
        /// A fixed second chord the action also answers to (Ctrl+Y beside Ctrl+Shift+Z for redo).
        /// It is never rebound and counts as taken.
        EditorShortcut alternateShortcut;
        EditorActionKind kind = EditorActionKind::Command;
        /// The action changes nothing (the MCP readOnlyHint), so it runs under a read-only editor.
        bool readOnly = false;

        /// The bindings, called with the subject page (null when no page is the subject; an
        /// editor-wide action ignores it).
        Function<void(EditorPage*)> execute;
        Function<bool(EditorPage*)> enabled; // unset means always enabled
        Function<bool(EditorPage*)> checked; // Toggle and Window only; unset means never checked
        /// Why the action is disabled for the subject, shown where the action shows (for example,
        /// a tool with nothing to work on); unset or empty means no reason is given.
        Function<String(EditorPage*)> disabledReason;
    };

    /// The interface `page` publishes, or null when there is no page or it is not that kind of
    /// page. A domain's action binds through it ("enabled iff the subject is a scene page").
    template <typename T>
        requires std::is_base_of_v<IPageService, T>
    [[nodiscard]] T* ServiceOf(EditorPage* page) noexcept
    {
        return page != nullptr ? page->Service<T>() : nullptr;
    }

    class EditorActionRegistry
    {
    public:
        EditorActionRegistry() = default;
        EditorActionRegistry(const EditorActionRegistry&) = delete;
        EditorActionRegistry& operator=(const EditorActionRegistry&) = delete;

        /// Records a read of the actions for the GUI's reactive evaluation in progress (an
        /// Effect). Registrations and rebinds run it again, so a surface that caches (the menu
        /// bar, the keymap's bindings) rebuilds without subscribing by hand.
        void TrackChanges() const { m_changed.Track(); }

        /// The subject the argument-less calls run over; the context wires it to its active page.
        /// Unset (a bare registry) means no subject.
        Function<EditorPage*()> ActiveSubject;
        [[nodiscard]] EditorPage* Subject() const { return ActiveSubject ? ActiveSubject() : nullptr; }

        /// Register a declaration. Refused (false, logged) when the id or label is empty, when
        /// execute is unset, or when the id is already registered; each is a mistake in a
        /// composition root, never a runtime condition.
        bool Register(EditorActionDeclaration declaration)
        {
            if (declaration.id.IsEmpty() || declaration.label.IsEmpty() || !declaration.execute)
            {
                LOG_ERROR(u8"Editor", u8"action '{}' refused: an id, a label and execute are required",
                          declaration.id.AsView());
                return false;
            }
            if (Find(declaration.id.AsView()) != nullptr)
            {
                LOG_ERROR(u8"Editor", u8"action '{}' registered twice; the first stands",
                          declaration.id.AsView());
                return false;
            }
            const u64 key = Hash<StringView>{}(declaration.id.AsView());
            m_actions.PushBack(static_cast<EditorActionDeclaration&&>(declaration));
            m_index.InsertOrAssign(key, m_actions.Size() - 1);
            NotifyChanged();
            return true;
        }

        [[nodiscard]] const EditorActionDeclaration* Find(StringView id) const noexcept
        {
            const usize* slot = m_index.Find(Hash<StringView>{}(id));
            if (slot != nullptr && m_actions[*slot].id.AsView() == id)
            {
                return &m_actions[*slot];
            }
            // A hash collision or an unknown id: the linear walk gives the answer.
            for (const EditorActionDeclaration& action : m_actions)
            {
                if (action.id.AsView() == id)
                {
                    return &action;
                }
            }
            return nullptr;
        }

        /// Every action, in registration order.
        [[nodiscard]] Span<const EditorActionDeclaration> Actions() const noexcept
        {
            return Span<const EditorActionDeclaration>{m_actions.Data(), m_actions.Size()};
        }
        [[nodiscard]] usize Count() const noexcept { return m_actions.Size(); }

        /// Unknown ids are disabled and unchecked: a surface asking about a name nobody
        /// registered shows nothing runnable.
        [[nodiscard]] bool IsEnabled(StringView id) const { return IsEnabled(id, Subject()); }
        [[nodiscard]] bool IsEnabled(StringView id, EditorPage* subject) const
        {
            const EditorActionDeclaration* action = Find(id);
            return action != nullptr && IsEnabled(*action, subject);
        }
        [[nodiscard]] static bool IsEnabled(const EditorActionDeclaration& action, EditorPage* subject)
        {
            return !action.enabled || action.enabled(subject);
        }
        [[nodiscard]] String DisabledReason(StringView id) const { return DisabledReason(id, Subject()); }
        /// Why `id` is disabled for `subject`; empty when it is enabled or gives no reason.
        [[nodiscard]] String DisabledReason(StringView id, EditorPage* subject) const
        {
            const EditorActionDeclaration* action = Find(id);
            if (action == nullptr || IsEnabled(*action, subject) || !action->disabledReason)
                return String{};
            return action->disabledReason(subject);
        }
        [[nodiscard]] bool IsChecked(StringView id) const { return IsChecked(id, Subject()); }
        [[nodiscard]] bool IsChecked(StringView id, EditorPage* subject) const
        {
            const EditorActionDeclaration* action = Find(id);
            return action != nullptr && IsChecked(*action, subject);
        }
        [[nodiscard]] static bool IsChecked(const EditorActionDeclaration& action, EditorPage* subject)
        {
            return static_cast<bool>(action.checked) && action.checked(subject);
        }

        /// The single entry point: a menu click, a chord, a toolbar click, the palette and the MCP
        /// bridge all execute through here, over the active subject or the page a surface names.
        /// NotFound for an unknown id; NotSupported when the action is not enabled for that
        /// subject (the surfaces never offer a disabled action, so this is the bridge's refusal);
        /// Ok once execute ran.
        [[nodiscard]] Status Execute(StringView id) const { return Execute(id, Subject()); }
        [[nodiscard]] Status Execute(StringView id, EditorPage* subject) const
        {
            const EditorActionDeclaration* action = Find(id);
            if (action == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            if (!IsEnabled(*action, subject))
            {
                return Status{ErrorCode::NotSupported};
            }
            action->execute(subject);
            return Status{};
        }

        /// The effective chord: the user's override when one is set (an unset override means
        /// the user chose no shortcut), otherwise the declaration's default.
        [[nodiscard]] EditorShortcut Shortcut(StringView id) const
        {
            const EditorActionDeclaration* action = Find(id);
            if (action == nullptr)
            {
                return EditorShortcut{};
            }
            const EditorShortcut* override = m_overrides.Find(Hash<StringView>{}(id));
            return override != nullptr ? *override : action->shortcut;
        }
        /// The declaration's alternate chord (never overridden); unset for most actions.
        [[nodiscard]] EditorShortcut AlternateShortcut(StringView id) const
        {
            const EditorActionDeclaration* action = Find(id);
            return action != nullptr ? action->alternateShortcut : EditorShortcut{};
        }
        /// The action holding a chord, considering effective bindings and alternates; null when
        /// the chord is free.
        [[nodiscard]] const EditorActionDeclaration* HolderOf(EditorShortcut chord) const
        {
            if (!chord.IsSet())
            {
                return nullptr;
            }
            for (const EditorActionDeclaration& action : m_actions)
            {
                if (Shortcut(action.id.AsView()) == chord || action.alternateShortcut == chord)
                {
                    return &action;
                }
            }
            return nullptr;
        }
        /// Bind the user's chord to an action (an unset chord means no shortcut). NotFound for an
        /// unknown id; AlreadyExists when another action holds the chord, naming it in `holder`
        /// so the settings page can say so. The registry keeps the override for the session;
        /// EditorShortcutSettings persists it.
        [[nodiscard]] Status Rebind(StringView id, EditorShortcut chord,
                                    const EditorActionDeclaration** holder = nullptr)
        {
            const EditorActionDeclaration* action = Find(id);
            if (action == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            const EditorActionDeclaration* taken = HolderOf(chord);
            if (taken != nullptr && taken != action)
            {
                if (holder != nullptr)
                {
                    *holder = taken;
                }
                return Status{ErrorCode::AlreadyExists};
            }
            m_overrides.InsertOrAssign(Hash<StringView>{}(id), chord);
            NotifyChanged();
            return Status{};
        }
        /// Forget the user's override: the declaration's default chord applies again.
        void ResetShortcut(StringView id)
        {
            if (m_overrides.Remove(Hash<StringView>{}(id)))
            {
                NotifyChanged();
            }
        }
        [[nodiscard]] bool HasOverride(StringView id) const noexcept
        {
            return m_overrides.Find(Hash<StringView>{}(id)) != nullptr;
        }

    private:
        void NotifyChanged() const { m_changed.Notify(); }

        mutable foundation::gui::Observable m_changed; // registrations and rebinds
        Array<EditorActionDeclaration> m_actions;
        HashMap<u64, usize> m_index;              // id hash -> index (verified on lookup)
        HashMap<u64, EditorShortcut> m_overrides; // id hash -> the user's chord
    };

    /// Appends items for the actions `ids`, in that order, to a menu over `subject`. Each item is
    /// labelled from its declaration with its effective chord, enabled as the registry answers
    /// now, and executes through the registry over the subject. An id nobody registered adds
    /// nothing (the menu's author named an action its domain never declared). Returns how many
    /// were added.
    inline usize AppendActionItems(foundation::gui::Menu& menu,
                                   const EditorActionRegistry& actions, EditorPage* subject,
                                   Span<const StringView> ids)
    {
        usize added = 0;
        for (StringView id : ids)
        {
            const EditorActionDeclaration* action = actions.Find(id);
            if (action == nullptr)
            {
                continue;
            }
            const EditorActionRegistry* registry = &actions;
            const String actionId = action->id;
            foundation::gui::MenuItem* item =
                menu.AddItem(action->label.AsView(), [registry, actionId, subject]() { (void)registry->Execute(actionId.AsView(), subject); });
            item->SetShortcut(FormatShortcut(actions.Shortcut(actionId.AsView())).AsView());
            item->SetEnabled(EditorActionRegistry::IsEnabled(*action, subject));
            ++added;
        }
        return added;
    }
}

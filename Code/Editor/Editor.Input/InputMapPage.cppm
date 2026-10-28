// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - the `editor.input` module.
//
// InputMapEditorPage: an input map on the GUI from its markup (Data/Editor/InputMapPage.xml;
// Documentation/Specs/editor-gui.md, 5.4, layer 5, part 4; the user's choice among the designs
// mocked in the editor mock). The sets and their actions a tree beside the grid; the tree's bar
// adds a set and removes the chosen row, its search keeping the actions whose name or input
// matches. The grid shows the chosen set from reflection (the input types are reflected): its name
// and priority, then its actions as the grid's list (Add adds to this set), each opening in place
// to its kind, interaction, bindings and processing, named and summed up in a line by the page
// (:input_map_rows); an action chosen in the tree opens there. A binding's input is captured by
// pressing it (:input_capture_editor, the page reading the devices each frame). Every edit is one
// undo step. A map the game could not read says so over the rows, and Save refuses it.
//
// Where the old page laid the map out two ways to compare, each a bespoke card language, this is
// one layout of the grid's own rows.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.input;

export import :input_map_edits;
export import :input_map_rows;
export import :input_capture_editor;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.input;
import input.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class InputMapEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/InputMapPage.xml";
        /// The grid's section the chosen set's rows are in: its paths are "set/" and the set's own.
        static constexpr StringView kSetSection = u8"set";
        /// The set's own path of a grid row's ("set/actions.0" is "actions.0").
        [[nodiscard]] static StringView SetPath(StringView gridPath);

        InputMapEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~InputMapEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        /// Refused while the map is one the game could not read (it says why).
        [[nodiscard]] Status Save() override;

        /// The set the grid shows (-1: none, an empty map).
        [[nodiscard]] i32 ChosenSet() const noexcept { return m_set; }
        void ChooseSet(i32 set);
        /// The action's set chosen, the action opened in place and brought into view.
        void ChooseAction(i32 set, i32 action);
        /// A set added after the others, chosen.
        void AddSet();
        /// The tree's chosen row taken out: an action, or a set with its actions.
        bool RemoveChosen();
        /// Keeps the actions whose name or input holds `text` (empty: every one).
        void Search(StringView text);
        /// Listens for the input of the row `path` (a binding's, or one key of a four-key one), or stops.
        void ToggleListen(StringView path);
        /// Lands what the devices gave while listening (the page's frame does it from the shell's input).
        void LandCapture(const foundation::input::Binding& captured);

        // === For tests ===
        /// The tree's rows, indented ("Gameplay\n  Move\n  Jump").
        [[nodiscard]] String Tree() const;
        [[nodiscard]] foundation::gui::IPropertySource& SetRows() noexcept;
        [[nodiscard]] bool IsListening() const noexcept { return !m_listenPath.IsEmpty(); }
        [[nodiscard]] StringView ListenPath() const noexcept { return m_listenPath.AsView(); }
        [[nodiscard]] const InputCaptureServices& CaptureServices() const noexcept { return m_capture; }

    protected:
        void OnObjectChanged() override;

    private:
        struct TreeRow
        {
            i32 set = -1;
            i32 action = -1; // -1: the set's row
        };

        [[nodiscard]] foundation::input::InputMap* Map() const;
        void BindSet();
        void ShowTree();
        void ShowValidity();
        void StopListen();
        /// The binding (and the four-key slot: 0..3, -1 the whole) `path` names in the chosen set.
        bool BindingOf(StringView path, usize& action, usize& binding, i32& direction) const;

        UniquePtr<foundation::gui::ReflectedPropertySource> m_values; // the chosen set's rows
        UniquePtr<foundation::gui::TreeModel> m_model;
        Array<TreeRow> m_rows; // the tree's, by node id
        i32 m_set = -1;
        TreeRow m_chosen;
        bool m_showing = false; // choosing from code: not the user's
        String m_search;
        String m_listenPath; // the row listened for (empty: none)
        foundation::input::CaptureFilter m_listenFilter;
        InputCaptureServices m_capture;
        app::XmlPropertyTextFormat m_format;
        foundation::gui::TreeView* m_tree = nullptr;
    };

    class InputMapPageFactory final : public IEditorPageFactory
    {
    public:
        explicit InputMapPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// The input map page's registration (from the editor's assembly).
    inline void RegisterInputMapEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<InputMapPageFactory>(services), editor::EditorRootAllocator()));
    }
}

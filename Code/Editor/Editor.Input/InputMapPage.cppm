// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - the `editor.input` module.
//
// InputMapEditorPage: an input map, built from its markup (Data/Editor/InputMapPage.xml). The
// sets and their actions form a tree beside the grid; the tree's bar adds a set and removes the
// chosen row, and its search keeps the actions whose name or input matches. The grid shows the
// chosen set from reflection (the input types are reflected): its name and priority, then its
// actions as the grid's list (Add adds to this set). Each action opens in place to its kind,
// interaction, bindings and processing, which the page names and sums up in a line
// (:input_map_rows); an action chosen in the tree opens there. A binding's input is captured by
// pressing it (:input_capture_editor; the page reads the devices each frame). Every edit is one
// undo step. A map the game could not read says so over the rows, and Save refuses it.

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
        /// The grid's section the chosen set's rows are in: its paths are "set/" followed by the
        /// set's own path.
        static constexpr StringView kSetSection = u8"set";
        /// A grid row's path within the set ("set/actions.0" is "actions.0").
        [[nodiscard]] static StringView SetPath(StringView gridPath);

        InputMapEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~InputMapEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        /// Refused while the map is one the game could not read (it says why).
        [[nodiscard]] Status Save() override;

        /// The set the grid shows (-1: none, an empty map).
        [[nodiscard]] i32 ChosenSet() const noexcept { return m_set; }
        void ChooseSet(i32 set);
        /// Chooses the action's set, opens the action in place and brings it into view.
        void ChooseAction(i32 set, i32 action);
        /// Adds a set after the others and chooses it.
        void AddSet();
        /// Removes the tree's chosen row: an action, or a set with its actions.
        bool RemoveChosen();
        /// Keeps the actions whose name or input holds `text` (empty: every one).
        void Search(StringView text);
        /// Starts listening for the input of row `path` (a binding's, or one key of a four-key
        /// binding), or stops.
        void ToggleListen(StringView path);
        /// Applies the input the devices gave while listening (the page's frame calls it with the
        /// shell's input).
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
        /// Finds the binding `path` names in the chosen set, and its four-key slot (0..3, or -1
        /// for the whole binding).
        bool BindingOf(StringView path, usize& action, usize& binding, i32& direction) const;

        UniquePtr<foundation::gui::ReflectedPropertySource> m_values; // the chosen set's rows
        UniquePtr<foundation::gui::TreeModel> m_model;
        Array<TreeRow> m_rows; // the tree's rows, by node id
        i32 m_set = -1;
        TreeRow m_chosen;
        bool m_showing = false; // set while code makes a choice, so it is not taken as the user's
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

    /// Registers the input map page (called from the editor's assembly).
    inline void RegisterInputMapEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<InputMapPageFactory>(services), editor::EditorRootAllocator()));
    }
}

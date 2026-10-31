// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_page partition
//
// What every asset page shares, so each page library does not write its own:
//   - AssetPageServices: what a page gets from the editor when it is made (its markup, the asset
//     slots' table, the type icons, what a preview renders through), filled in by the application.
//   - FindById: finds an element of a page's markup by its id, which is how code binds to markup.
//   - XmlPropertyTextFormat: the engine's XmlSerializer as a reflected source's text format. An
//     edit's before and after states are written in it, and an undo reads them back.
//   - ReflectedEditCommand: one reflected source's change, undone and redone by its text on the
//     object it was applied to. The object is looked up again each time, since it may have been
//     read again in between.
//   - SnapshotObject and RestoreObject: an object's whole serialized state under its type's
//     current data versions, so an edit made by code that changes many values at once (a preset)
//     is still one undo step.

module;
#include "Core/Prelude.h"

export module editor.app:asset_page;

import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import foundation.runtime.client; // IApplicationHost
import foundation.gui;
import foundation.gui.toolkit;
import foundation.gui.hosting; // GuiHost
import editor.core;
import :resource_ref_editor;
import :code_languages;
import :picker; // PickerRequest

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    struct AssetPageServices
    {
        /// Loads a page's markup by its path under Data/ (null when it could not be read).
        Function<RefPtr<gui::Node>(StringView path)> loadMarkup;
        const ResourceRefServices* refs = nullptr; // the asset slots' table (null: none)
        editor::EditorIcons* icons = nullptr;
        /// What a page's preview renders through, and its window's GUI, which hosts the preview's
        /// viewport (null in a test; the preview then draws nothing).
        foundation::runtime::IApplicationHost* host = nullptr;
        gui::GuiHost* gui = nullptr;
        /// The code editors' languages by id (null in a test; code then shows uncoloured).
        CodeLanguages* languages = nullptr;
        /// Opens the editor's picker for `request` (a source file's Browse); unset: no picker.
        Function<void(PickerRequest request)> pick;
    };

    /// Where every page factory gets the services a page is made with. There is one, owned by the
    /// editor's assembly (or a test), and it outlives the factories. Each factory keeps a reference
    /// and asks it when it makes a page, by which time the window, the host and the GUI exist.
    /// Sharing one source by reference means no factory needs a copy of the making Function,
    /// which is move-only.
    class AssetPageServicesSource
    {
    public:
        AssetPageServicesSource() {} // makes pages with no services (a test's registration); user-provided, so a const instance needs no initializer
        explicit AssetPageServicesSource(Function<AssetPageServices()> make) : m_make(Move(make)) {}
        AssetPageServicesSource(const AssetPageServicesSource&) = delete;
        AssetPageServicesSource& operator=(const AssetPageServicesSource&) = delete;

        /// The services to make a page with, as they are now.
        [[nodiscard]] AssetPageServices Make() const { return m_make ? m_make() : AssetPageServices{}; }

    private:
        Function<AssetPageServices()> m_make;
    };

    /// The element under `root` (itself included) whose id is `id`, as a T; null when there is
    /// none or it is not one.
    template <typename T>
    [[nodiscard]] T* FindById(gui::Node& root, StringView id)
    {
        Array<gui::Node*> pending(EditorRootAllocator());
        pending.PushBack(&root);
        while (!pending.IsEmpty())
        {
            gui::Node* node = pending.Back();
            pending.PopBack();
            if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                return Cast<T>(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                pending.PushBack(node->GetChildAt(i));
        }
        return nullptr;
    }

    /// Pushes `type`'s current data-version chain (the type and its versioned bases) on `ar`
    /// without a header. It is for a scan, a snapshot or a replay of a live object, which is at the
    /// current version by definition, and matches what a saved envelope's version scope carries.
    /// Pop it afterwards.
    inline void PushCurrentDataVersions(ISerializer& ar, const TypeInfo& type)
    {
        SerializedDataVersion chain[16];
        u32 count = 0;
        chain[count++] = SerializedDataVersion{type.id, type.dataVersion};
        for (const TypeInfo* base = type.base; base != nullptr && count < 16; base = base->base)
            if (base->dataVersion > 0)
                chain[count++] = SerializedDataVersion{base->id, base->dataVersion};
        ar.PushVersionScope(chain, count);
    }

    /// Serializes `object`'s whole state to bytes.
    [[nodiscard]] inline Array<byte> SnapshotObject(ISerializable& object)
    {
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        PushCurrentDataVersions(ar, *object.GetType());
        object.Serialize(ar);
        ar.PopVersionScope();
        Array<byte> bytes;
        const Span<const byte> written = stream.Bytes();
        bytes.Reserve(written.Size());
        for (byte b : written)
            bytes.PushBack(b);
        return bytes;
    }

    /// Puts SnapshotObject's `bytes` back into `object`.
    inline bool RestoreObject(ISerializable& object, const Array<byte>& bytes)
    {
        MemoryStream stream;
        (void)stream.Write(bytes.Data(), bytes.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        PushCurrentDataVersions(ar, *object.GetType());
        object.Serialize(ar);
        ar.PopVersionScope();
        return ar.IsOk();
    }

    /// An edit made by code over a whole object: its state before and after, restored by undo and
    /// redo on the object, which is looked up again each time. `applied` runs after each restore.
    class ObjectSnapshotCommand final : public editor::IEditorCommand
    {
    public:
        ObjectSnapshotCommand(Function<ISerializable*()> target, Array<byte> before, Array<byte> after, StringView typeId, Function<void()> applied = {})
            : m_target(Move(target)), m_before(Move(before)), m_after(Move(after)), m_typeId(typeId), m_applied(Move(applied))
        {
        }

        [[nodiscard]] bool Execute() override { return Restore(m_after); }
        void Undo() override { (void)Restore(m_before); }
        [[nodiscard]] StringView TypeId() const override { return m_typeId.AsView(); }

    private:
        bool Restore(const Array<byte>& bytes)
        {
            ISerializable* object = m_target ? m_target() : nullptr;
            if (object == nullptr)
                return false;
            const bool ok = RestoreObject(*object, bytes);
            if (m_applied)
                m_applied();
            return ok;
        }

        Function<ISerializable*()> m_target;
        Array<byte> m_before;
        Array<byte> m_after;
        String m_typeId;
        Function<void()> m_applied;
    };

    class XmlPropertyTextFormat final : public gui::IPropertyTextFormat
    {
    public:
        explicit XmlPropertyTextFormat(IAllocator& allocator) : m_allocator(allocator) {}

        String Write(Function<void(ISerializer&)> write) override
        {
            foundation::xml::XmlSerializer ar(m_allocator);
            write(ar);
            String text;
            ar.GetOutput(text);
            return ar.IsOk() ? text : String{};
        }
        bool Read(StringView text, Function<void(ISerializer&)> read) override
        {
            foundation::xml::XmlDocument doc(m_allocator);
            if (doc.Parse(text) != foundation::xml::XmlResult::Ok)
                return false;
            foundation::xml::XmlSerializer ar(doc);
            read(ar);
            return ar.IsOk();
        }

    private:
        IAllocator& m_allocator;
    };

    class ReflectedEditCommand final : public editor::IEditorCommand
    {
    public:
        /// `target` finds the object of `type` the change was applied to (null when it is gone; the
        /// step then does nothing). `applied`, when given, runs after each restore, for example to
        /// bind a reference again or notify a preview. `format` outlives the command.
        ReflectedEditCommand(const TypeInfo& type, Function<void*()> target, gui::ReflectedChange change, gui::IPropertyTextFormat& format,
                             StringView typeId, Function<void()> applied = {})
            : m_type(&type), m_target(Move(target)), m_change(Move(change)), m_format(&format), m_typeId(typeId), m_applied(Move(applied))
        {
        }

        [[nodiscard]] bool Execute() override { return Restore(false); }
        void Undo() override { (void)Restore(true); }
        [[nodiscard]] StringView TypeId() const override { return m_typeId.AsView(); }

    private:
        bool Restore(bool before)
        {
            void* values = m_target ? m_target() : nullptr;
            if (values == nullptr)
                return false;
            gui::ReflectedPropertySource source(EditorRootAllocator());
            source.SetTextFormat(m_format);
            const Instance object(values, m_type);
            source.SetObjects(Span<const Instance>(&object, 1u));
            const bool ok = source.Restore(m_change, before);
            if (m_applied)
                m_applied();
            return ok;
        }

        const TypeInfo* m_type;
        Function<void*()> m_target;
        gui::ReflectedChange m_change;
        gui::IPropertyTextFormat* m_format;
        String m_typeId;
        Function<void()> m_applied;
    };
}

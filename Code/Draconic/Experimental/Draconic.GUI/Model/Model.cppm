// Draconic GUI - :model partition
//
// IModel: the abstract data source a model-backed view renders. Modeled on eepp's Models::Model
// (role only, common subset). A model exposes a grid of cells (RowCount x ColumnCount), returns
// a Variant per (index, role), and notifies attached clients (the views) when it changes so they
// refresh. Editing (IsEditable/SetData) is optional. Tree parenting / sorting proxies are
// follow-ups; this is the flat list/table foundation.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:model;

import draconic.foundation; // Array, String, usize
import :variant;
import :model_index;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    // Observer notified when a model changes (a view implements this to re-read + relayout).
    class IModelClient
    {
    public:
        virtual ~IModelClient() = default;
        virtual void OnModelUpdated() = 0;
    };

    class IModel
    {
    public:
        virtual ~IModel() = default;

        // Number of rows under `parent` (an invalid parent = the top level). Flat models
        // ignore the parent (they only have a top level).
        [[nodiscard]] virtual usize RowCount(const ModelIndex& parent = {}) const = 0;
        [[nodiscard]] virtual usize ColumnCount() const = 0;
        [[nodiscard]] virtual foundation::String ColumnName(usize /*column*/) const
        {
            return foundation::String{};
        }
        [[nodiscard]] virtual Variant Data(const ModelIndex& index,
                                           ModelRole role = ModelRole::Display) const = 0;

        [[nodiscard]] virtual bool IsEditable(const ModelIndex&) const { return false; }
        virtual void SetData(const ModelIndex&, const Variant&) {}

        // === Tree structure (flat defaults; a tree model overrides these) ===
        // The index of the cell at (row, column) under `parent`.
        [[nodiscard]] virtual ModelIndex Index(i32 row, i32 column = 0,
                                               const ModelIndex& /*parent*/ = {}) const
        {
            return MakeModelIndex(row, column);
        }
        // The parent of `child` (invalid = a top-level node; flat models have no parents).
        [[nodiscard]] virtual ModelIndex ParentIndex(const ModelIndex& /*child*/) const
        {
            return ModelIndex{};
        }
        // Whether `parent` has any child rows.
        [[nodiscard]] virtual bool HasChildren(const ModelIndex& parent = {}) const
        {
            return RowCount(parent) > 0;
        }

        [[nodiscard]] bool IsValidIndex(const ModelIndex& index) const
        {
            return index.Row >= 0 && static_cast<usize>(index.Row) < RowCount() &&
                   index.Column >= 0 && static_cast<usize>(index.Column) < ColumnCount();
        }

        // Client (view) registration. Clients are non-owning and must outlive the model or
        // unregister first.
        void AddClient(IModelClient* client)
        {
            if (client != nullptr)
                m_clients.PushBack(client);
        }
        void RemoveClient(IModelClient* client)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                if (m_clients[i] == client)
                {
                    m_clients.RemoveAt(i);
                    return;
                }
        }

        // Notify every attached client that the model changed (call after mutating it).
        void DidUpdate()
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnModelUpdated();
        }

    protected:
        Array<IModelClient*> m_clients; // non-owning
    };
}

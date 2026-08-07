// Draconic GUI - :model_index partition
//
// ModelIndex + ModelRole: the address of a cell in a Model, and the aspect a view asks for.
// Modeled on eepp's Models::ModelIndex / ModelRole (role only). This v1 is flat (row + column,
// for list and table models); a parent handle for tree models is a follow-up.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:model_index;

import draconic.foundation; // i32

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    // Which aspect of a cell a view is requesting (display text, a sort key, an icon, ...).
    enum class ModelRole
    {
        Display,
        Sort,
        Icon,
        Custom
    };

    struct ModelIndex
    {
        i32 Row = -1; // -1 = invalid
        i32 Column = 0;
        i64 InternalId = -1; // opaque node identity a tree model uses to locate the node

        [[nodiscard]] bool IsValid() const noexcept { return Row >= 0 && Column >= 0; }
        [[nodiscard]] bool operator==(const ModelIndex& other) const noexcept
        {
            return Row == other.Row && Column == other.Column && InternalId == other.InternalId;
        }
    };

    [[nodiscard]] inline ModelIndex MakeModelIndex(i32 row, i32 column = 0,
                                                   i64 internalId = -1) noexcept
    {
        return ModelIndex{row, column, internalId};
    }
}

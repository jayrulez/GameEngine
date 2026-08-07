// Draconic Foundation - :filesystem partition
//
// Whole-file convenience helpers over FileStream. Directory queries live in
// :system (DirectoryExists/CreateDirectory/RemoveDirectory) and are re-exported
// here for a single filesystem surface.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:filesystem;

import :base;
import :allocator;
import :array;
import :span;
import :string;
import :system;
import :io;

export namespace draconic::foundation
{
    // Reads an entire file into a byte buffer.
    [[nodiscard]] inline Result<Array<byte>> ReadFile(StringView path,
                                                      IAllocator& allocator = DefaultAllocator())
    {
        FileStream file(path, FileMode::Read);
        if (!file.IsValid())
        {
            return Err(ErrorCode::NotFound);
        }

        const i64 size = file.Size();
        if (size < 0)
        {
            return Err(ErrorCode::Internal);
        }

        Array<byte> data(allocator);
        data.Resize(static_cast<usize>(size));
        if (size > 0)
        {
            const u64 read = file.Read(data.Data(), static_cast<u64>(size));
            if (read != static_cast<u64>(size))
            {
                return Err(ErrorCode::Internal);
            }
        }
        return data;
    }

    // Writes a byte buffer to a file, replacing any existing contents.
    [[nodiscard]] inline Status WriteFile(StringView path, Span<const byte> data)
    {
        FileStream file(path, FileMode::Write);
        if (!file.IsValid())
        {
            return Status{ErrorCode::Internal};
        }
        if (!data.IsEmpty())
        {
            const u64 written = file.Write(data.Data(), data.Size());
            if (written != data.Size())
            {
                return Status{ErrorCode::Internal};
            }
        }
        return Status{};
    }
}

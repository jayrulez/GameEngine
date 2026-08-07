// Draconic Foundation - :iserializable partition
//
// ISerializable: the polymorphic serialization base. A type that wants to be
// serialized through the format-agnostic ISerializer (and stored in the content
// database) derives this and implements Serialize(). Being an Object, it carries
// reflected type identity (GetType()), which the polymorphic save/load path uses
// for a type tag + reflection-driven reconstruction.
//
// Value/POD/container types don't need this - they use the non-intrusive
// Serialize(ISerializer&, T&) free functions instead. ISerializable is for the
// objects that live in the content database.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.foundation:iserializable;

import :base;
import :type_info;
import :object;
import :iserializer;

export namespace draconic::foundation
{
    class ISerializable : public Object
    {
        DRACONIC_OBJECT(ISerializable, Object)
    public:
        // Describe this object's data once; runs in whichever direction `ar`
        // is configured for (read or write).
        virtual void Serialize(ISerializer& ar) = 0;
    };

    DRACONIC_DEFINE_OBJECT(ISerializable, "draconic::foundation")
}

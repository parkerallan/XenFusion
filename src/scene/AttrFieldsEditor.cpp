#include "scene/AttrFields.h"
#include "state/EngineState.h"

#include <stddef.h>
#include <string.h>

// The editor's instantiation of the shared field table, against ObjectAttribute.
// The runtime compiles runtime/src/AttrFieldsRt.cpp instead, from the same list.

namespace attrfields
{
    namespace
    {
#define XF_FIELD_ROW(name, kind, member) \
    { name, Kind##kind, (unsigned int)offsetof(ObjectAttribute, member) },

        // offsetof on a type with std::string members is conditionally
        // supported; MSVC and the XDK toolset both accept it, and the alternative
        // (a switch returning &a.member) would have to be written twice.
        const FieldDesc kTable[] = { XF_ATTR_FIELDS(XF_FIELD_ROW) };
#undef XF_FIELD_ROW

        const int kCount = (int)(sizeof(kTable) / sizeof(kTable[0]));
    }

    const FieldDesc* Table(int& outCount)
    {
        outCount = kCount;
        return kTable;
    }

    const FieldDesc* Find(const char* name)
    {
        if (!name) return 0;
        for (int i = 0; i < kCount; ++i)
            if (strcmp(kTable[i].name, name) == 0) return &kTable[i];
        return 0;
    }
}

#include "scene/AttrFields.h"
#include "SceneData.h"

#include <stddef.h>
#include <string.h>

// The runtime's instantiation of the shared field table, against RtAttribute.
// Same XF_ATTR_FIELDS list the editor expands in src/scene/AttrFieldsEditor.cpp;
// only the struct the offsets are taken from differs. If a field is added to the
// list but is missing from RtAttribute, this file fails to compile — which is
// the point: the two structs cannot drift apart silently.

namespace attrfields
{
    namespace
    {
#define XF_FIELD_ROW(name, kind, member) \
    { name, Kind##kind, (unsigned int)offsetof(RtAttribute, member) },

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

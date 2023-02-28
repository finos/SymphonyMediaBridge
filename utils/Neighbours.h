#pragma once
#include "memory/Map.h"

namespace
{
template <class V, class T>
bool isNeighbour(const V& groupList, const T& lookupTable)
{
    for (auto& entry : groupList)
    {
        if (lookupTable.contains(entry))
        {
            return true;
        }
    }
    return false;
}

template <class TMap>
bool areNeighbours(const TMap& table1, const TMap& table2)
{
    if (table1.size() < table2.size())
    {
        for (auto& entry : table1)
        {
            if (table2.contains(entry.first))
            {
                return true;
            }
        }
    }
    else
    {
        for (auto& entry : table2)
        {
            if (table1.contains(entry.first))
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace
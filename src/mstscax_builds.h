#pragma once

#include <windows.h>

namespace mstsfence::rdpedisp
{
    inline constexpr GUID kMstscaxPdb_6D429A36 = {
        0x6D429A36, 0x0C7A, 0xB9A0, { 0xA2, 0xBD, 0x12, 0x24, 0x07, 0xB8, 0xA7, 0x52 }
    };

    inline const wchar_t* KnownMstscaxBuildName(const GUID& guid)
    {
        if (IsEqualGUID(guid, kMstscaxPdb_6D429A36))
            return L"26100.8246/.8328/.8457";
        return nullptr;
    }
}

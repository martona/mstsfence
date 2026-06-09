#include "settings.h"

#include <windows.h>

namespace
{
    constexpr wchar_t kKey[] = L"Software\\mstsfence";

    bool ReadFlag(const wchar_t* name, bool fallback) noexcept
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        const LSTATUS s = ::RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD,
                                         nullptr, &value, &size);
        return (s == ERROR_SUCCESS) ? (value != 0) : fallback;  // missing -> default
    }

    void WriteFlag(const wchar_t* name, bool on) noexcept
    {
        HKEY key = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                              &key, nullptr) != ERROR_SUCCESS)
            return;
        const DWORD value = on ? 1u : 0u;
        ::RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        ::RegCloseKey(key);
    }
}

namespace mstsfence
{
    bool FenceEnabled()    { return ReadFlag(L"Fence", true); }
    bool DarkModeEnabled() { return ReadFlag(L"DarkMode", true); }
    bool DpiFixEnabled()   { return ReadFlag(L"DpiFix", false); }  // hidden feature -> default off

    void SetFenceEnabled(bool on)    { WriteFlag(L"Fence", on); }
    void SetDarkModeEnabled(bool on) { WriteFlag(L"DarkMode", on); }
    void SetDpiFixEnabled(bool on)   { WriteFlag(L"DpiFix", on); }
}

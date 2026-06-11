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

    DWORD ReadDword(const wchar_t* name, DWORD fallback) noexcept
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        const LSTATUS s = ::RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD,
                                         nullptr, &value, &size);
        return (s == ERROR_SUCCESS) ? value : fallback;  // missing -> default
    }

    void WriteDword(const wchar_t* name, DWORD value) noexcept
    {
        HKEY key = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                              &key, nullptr) != ERROR_SUCCESS)
            return;
        ::RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        ::RegCloseKey(key);
    }
}

namespace mstsfence
{
    bool FenceEnabled()    { return ReadFlag(L"Fence", true); }
    bool DarkModeEnabled() { return ReadFlag(L"DarkMode", true); }
    bool DpiFixEnabled()   { return ReadFlag(L"DpiFix", false); }  // hidden feature -> default off
    bool DpiOverrideEnabled() { return ReadFlag(L"DpiOverride", false); }
    unsigned DpiOverridePct()
    {
        DWORD v = ReadDword(L"DpiOverridePct", 100);
        return v < 100 ? 0u : (v > 500 ? 500u : static_cast<unsigned>(v));
    }
    unsigned DpiNudgePx()
    {
        DWORD v = ReadDword(L"DpiNudge", 0);   // 0 = off; cap at 64 px so a fat-fingered value can't break the connect
        return v > 64 ? 64u : static_cast<unsigned>(v);
    }
    unsigned DpiInjectScale()
    {
        DWORD v = ReadDword(L"DpiInjectScale", 0);   // 0 = off; else a DesktopScaleFactor clamped 100..500
        if (v == 0) return 0u;
        return v < 100 ? 100u : (v > 500 ? 500u : static_cast<unsigned>(v));
    }

    void SetFenceEnabled(bool on)    { WriteFlag(L"Fence", on); }
    void SetDarkModeEnabled(bool on) { WriteFlag(L"DarkMode", on); }
    void SetDpiFixEnabled(bool on)   { WriteFlag(L"DpiFix", on); }
    void SetDpiOverrideEnabled(bool on) { WriteFlag(L"DpiOverride", on); }
    void SetDpiOverridePct(unsigned pct)
    {
        if (pct < 100) pct = 100; else if (pct > 500) pct = 500;
        WriteDword(L"DpiOverridePct", pct);
    }
    void SetDpiNudgePx(unsigned px)
    {
        if (px > 64) px = 64;
        WriteDword(L"DpiNudge", px);
    }
    void SetDpiInjectScale(unsigned scale)
    {
        if (scale != 0 && scale < 100) scale = 100; else if (scale > 500) scale = 500;
        WriteDword(L"DpiInjectScale", scale);
    }
}

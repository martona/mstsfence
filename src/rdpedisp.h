#pragma once

// ---------------------------------------------------------------------------
// rdpedisp -- host DPI override for mstsc.
//
// When DpiOverride is on, mstsc's scale-bearing monitor data is rewritten in two
// places:
//   * connect-time GCC user data (CS_MONITOR / CS_MONITOR_EX), including the
//     single-monitor case where stock mstsc normally emits no monitor attributes;
//   * the later DISPLAYCONTROL_MONITOR_LAYOUT PDU (MS-RDPEDISP), so live resize
//     traffic keeps carrying the chosen host scale.
//
// Gated on HKCU\Software\mstsfence\DpiOverride and DpiOverridePct. On an
// unrecognized mstscax build it is a safe no-op. Reverse-engineering scaffolding
// (layout observe-hooks + DPI API probes) compiles in only under MSTSFENCE_DPI_DIAG.
// ---------------------------------------------------------------------------
namespace mstsfence
{
namespace rdpedisp
{
    void OnAttach();  // DllMain DLL_PROCESS_ATTACH (mstsc only): arm the mstscax-load hook + override.
    void OnDetach();  // DllMain DLL_PROCESS_DETACH: detach everything.
}
}

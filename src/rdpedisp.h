#pragma once

// ---------------------------------------------------------------------------
// rdpedisp -- connect-time DPI fix for mstsc.
//
// mstsc only emits the scale-bearing DISPLAYCONTROL_MONITOR_LAYOUT PDU (MS-RDPEDISP)
// in response to a resize, so a freshly connected session shows the host at 100% until
// you restore/maximize the window ("the dance"). This module replays that one send at
// connect: it hooks RdpDisplayControlChannel::OnDataReceived (the channel's first live
// moment) and calls SendMonitorLayoutPdu(channel, 0, 0) -- exactly what the dance does.
//
// The send's 'this' is the display-control *interface* sub-object, which is
// OnDataReceived's 'this' + 8 (a multiple-inheritance adjustment, pinned to the known
// mstscax build). The call is SEH-guarded; the latch is re-armed per connection.
//
// Gated on HKCU\Software\mstsfence\DpiFix (default OFF). On an unrecognized mstscax build
// it is a safe no-op. All the reverse-engineering scaffolding that found the fix (the
// layout observe-hooks + the DPI API probes) compiles in only under MSTSFENCE_DPI_DIAG.
// ---------------------------------------------------------------------------
namespace mstsfence
{
namespace rdpedisp
{
    void OnAttach();  // DllMain DLL_PROCESS_ATTACH (mstsc only): arm the mstscax-load hook + fix.
    void OnDetach();  // DllMain DLL_PROCESS_DETACH: detach everything.
}
}

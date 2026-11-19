/* SPDX-License-Identifier: GPL-3.0-only */
/* bcryptprimitives.dll for the wine in plugin/tests/docker/wine.Dockerfile (Debian 12's wine 8.0 has none):
 * clap-validator's Windows build (Rust's std) imports ProcessPrng from it. Test tooling only, never shipped:
 * tools/plugin_win.sh validate builds it next to a copy of clap-validator.exe. ProcessPrng fills the buffer from
 * RtlGenRandom (advapi32 SystemFunction036), which wine has. */
#include <windows.h>

BOOLEAN WINAPI SystemFunction036(PVOID buf, ULONG len);

__declspec(dllexport) BOOL WINAPI ProcessPrng(PBYTE data, SIZE_T len)
{
    while (len) {
        ULONG n = len > 0x10000000u ? 0x10000000u : (ULONG)len;
        if (!SystemFunction036(data, n)) return FALSE;
        data += n;
        len -= n;
    }
    return TRUE;
}

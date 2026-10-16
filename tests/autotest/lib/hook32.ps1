# Run by the autotester in 32-bit PowerShell: hooks a test game's main thread
# with CoopTestHook.dll's window hook (tests/autotest/hook). It writes "ready"
# to -Ready once the DLL is loaded (the runner then lets the suspended game
# run), tries the hook until the thread takes it (its first call into the
# window system, before its first window), writes the result to -Result and
# keeps the hook for -Seconds (a thread hook ends with the thread that made it).
param([string]$Dll, [int]$Thread, [string]$Ready, [string]$Result, [int]$Seconds = 60)
$ErrorActionPreference = "Stop"
# written whole or not at all (the runner reads it while this runs)
function Say([string]$File, [string]$Text) {
    Set-Content -LiteralPath "$File.tmp" -Value $Text -Encoding ascii
    Move-Item -LiteralPath "$File.tmp" -Destination $File -Force
}
try {
    if ([IntPtr]::Size -ne 4) { throw "not 32-bit" }
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class CoopHook32 {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)] static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr HookWait(uint threadId, uint timeoutMs, out uint ms);
    static HookWait s_wait;
    public static string Load(string dll) {
        IntPtr m = LoadLibraryW(dll);
        if (m == IntPtr.Zero) return "no dll (" + Marshal.GetLastWin32Error() + ")";
        IntPtr f = GetProcAddress(m, "CoopTestHookWait");
        if (f == IntPtr.Zero) return "no CoopTestHookWait";
        s_wait = (HookWait)Marshal.GetDelegateForFunctionPointer(f, typeof(HookWait));
        return "ready";
    }
    public static string Hook(uint thread) {
        uint ms;
        IntPtr hook = s_wait(thread, 20000, out ms);
        return hook == IntPtr.Zero ? "failed (" + Marshal.GetLastWin32Error() + ")" : "on after " + ms + " ms";
    }
}
"@
    $r = [CoopHook32]::Load($Dll)
    Say $Ready $r
    if ($r -ne "ready") { exit 1 }
    $h = [CoopHook32]::Hook([uint32]$Thread)
    Say $Result $h
    if ($h -like "on*") { Start-Sleep -Seconds $Seconds }
} catch {
    Say $Ready "error: $_"
}

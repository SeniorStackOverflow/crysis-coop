// The autotester's Windows side: its games run in a job (so the runner knows
// which Crysis is its own and none outlives it), hidden, left of all screens,
// and never keep the foreground the person at the computer had.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class CoopTestNative
{
    // ---- the job ----------------------------------------------------------
    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION Basic;
        public IO_COUNTERS Io;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateJobObjectW(IntPtr a, string name);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int cls, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int len);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool SetProcessAffinityMask(IntPtr h, UIntPtr mask);
    [DllImport("kernel32.dll")] static extern uint SetErrorMode(uint mode);

    const uint KILL_ON_JOB_CLOSE = 0x2000, DIE_ON_UNHANDLED_EXCEPTION = 0x400;
    static IntPtr s_job = IntPtr.Zero;

    // the job lives as long as this process: when the runner ends in any
    // way, Windows ends its games
    public static void EnsureJob()
    {
        if (s_job != IntPtr.Zero) return;
        // no "the program stopped working" window when a game crashes (the
        // games inherit the mode)
        SetErrorMode(0x0002 | 0x0001);   // SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS
        s_job = CreateJobObjectW(IntPtr.Zero, null);
        if (s_job == IntPtr.Zero) throw new Exception("CreateJobObject failed: " + Marshal.GetLastWin32Error());
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.Basic.LimitFlags = KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION;
        if (!SetInformationJobObject(s_job, 9, ref info, Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))))
            throw new Exception("SetInformationJobObject failed: " + Marshal.GetLastWin32Error());
    }

    public static bool InJob(int pid)
    {
        if (s_job == IntPtr.Zero) return false;
        IntPtr h = OpenProcess(0x1000, false, pid);   // PROCESS_QUERY_LIMITED_INFORMATION
        if (h == IntPtr.Zero) return false;
        bool r;
        bool ok = IsProcessInJob(h, s_job, out r);
        CloseHandle(h);
        return ok && r;
    }

    [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr job, int cls, IntPtr info, int len, IntPtr ret);
    static readonly HashSet<int> s_pids = new HashSet<int>();

    // the processes in the job now (the host's game and the companion's it started)
    public static int[] JobPids()
    {
        var list = new List<int>();
        if (s_job == IntPtr.Zero) return list.ToArray();
        int size = 8 + 64 * IntPtr.Size;
        IntPtr buf = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.WriteInt32(buf, 0, 64);
            if (QueryInformationJobObject(s_job, 3, buf, size, IntPtr.Zero))
            {
                int n = Marshal.ReadInt32(buf, 4);
                for (int i = 0; i < n && i < 64; ++i)
                    list.Add((int)Marshal.ReadIntPtr(buf, 8 + i * IntPtr.Size).ToInt64());
            }
        }
        finally { Marshal.FreeHGlobal(buf); }
        lock (s_pids) { s_pids.Clear(); foreach (int p in list) s_pids.Add(p); }
        return list.ToArray();
    }
    static bool Known(int pid) { lock (s_pids) return s_pids.Contains(pid); }

    public static void KillAll()
    {
        if (s_job != IntPtr.Zero) TerminateJobObject(s_job, 1);
    }

    // ---- starting a game --------------------------------------------------
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO
    {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    // the game's CPUs, as CrysisCoop.exe gives them (Crysis plays no sound on
    // a PC with many): 4, every other logical processor (one per core when
    // cores have two)
    public static ulong GameAffinity()
    {
        int n = Environment.ProcessorCount;
        if (n <= 4) return 0;
        int step = n >= 8 ? 2 : 1;
        ulong mask = 0;
        for (int i = 0, k = 0; i < n && k < 4; i += step, ++k) mask |= 1UL << i;
        return mask;
    }

    // hidden (Coop.dll shows it as a tool window), in the job before its
    // first instruction, with its own environment; returns the process id
    // the game as CrysisCoop.exe starts it: normal priority, its 4 CPUs
    // (COOPTEST_PRIORITY=below and COOPTEST_AFFINITY=0 change that, to find
    // what a test start does differently)
    public static int Start(string exe, string args, string dir, string[] envOverrides)
    {
        EnsureJob();
        var env = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
            env[(string)e.Key] = (string)e.Value;
        if (envOverrides != null)
            foreach (string kv in envOverrides)
            {
                int eq = kv.IndexOf('=');
                if (eq > 0) env[kv.Substring(0, eq)] = kv.Substring(eq + 1);
            }
        var block = new StringBuilder();
        foreach (var kv in env) block.Append(kv.Key).Append('=').Append(kv.Value).Append('\0');
        block.Append('\0');
        IntPtr pEnv = Marshal.StringToHGlobalUni(block.ToString());
        try
        {
            var si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            si.dwFlags = 1;        // STARTF_USESHOWWINDOW
            si.wShowWindow = 0;    // SW_HIDE
            PROCESS_INFORMATION pi;
            // CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT (| BELOW_NORMAL_PRIORITY_CLASS)
            uint flags = 0x4 | 0x400;
            if (Environment.GetEnvironmentVariable("COOPTEST_PRIORITY") == "below") flags |= 0x4000;
            if (!CreateProcessW(exe, new StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero, IntPtr.Zero, false, flags, pEnv, dir, ref si, out pi))
                throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
            if (!AssignProcessToJobObject(s_job, pi.hProcess))
            {
                int err = Marshal.GetLastWin32Error();
                TerminateProcessHandle(pi.hProcess);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                throw new Exception("AssignProcessToJobObject failed: " + err);
            }
            ulong aff = Environment.GetEnvironmentVariable("COOPTEST_AFFINITY") == "0" ? 0 : GameAffinity();
            if (aff != 0) SetProcessAffinityMask(pi.hProcess, new UIntPtr(aff));
            lock (s_pids) s_pids.Add(pi.dwProcessId);
            CloseHandle(pi.hProcess);
            // still suspended: the runner hooks its main thread, then Resume
            lock (s_suspended) s_suspended[pi.dwProcessId] = pi.hThread;
            LastThreadId = pi.dwThreadId;
            return pi.dwProcessId;
        }
        finally { Marshal.FreeHGlobal(pEnv); }
    }
    // a helper of the runner (the 32-bit hook helper): no window, no handles
    // of the runner (its output pipe stayed open while the helper waited), in
    // the job, so it ends with the test games
    public static int StartHelper(string exe, string args)
    {
        EnsureJob();
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.dwFlags = 1;
        si.wShowWindow = 0;
        PROCESS_INFORMATION pi;
        // CREATE_SUSPENDED | CREATE_NO_WINDOW
        if (!CreateProcessW(exe, new StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero, IntPtr.Zero, false, 0x4 | 0x08000000, IntPtr.Zero, null, ref si, out pi))
            throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        AssignProcessToJobObject(s_job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }

    static readonly Dictionary<int, IntPtr> s_suspended = new Dictionary<int, IntPtr>();
    public static int LastThreadId = 0;     // the main thread of the game Start made last

    // the game Start made runs from here
    public static bool Resume(int pid)
    {
        IntPtr h;
        lock (s_suspended)
        {
            if (!s_suspended.TryGetValue(pid, out h)) return false;
            s_suspended.Remove(pid);
        }
        bool ok = ResumeThread(h) != 0xFFFFFFFF;
        CloseHandle(h);
        return ok;
    }

    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint code);
    static void TerminateProcessHandle(IntPtr h) { TerminateProcess(h, 1); }

    // ---- the person's foreground -----------------------------------------
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool LockSetForegroundWindow(uint code);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

    static IntPtr s_userWnd = IntPtr.Zero;
    public static int FocusIncidents = 0;     // times a test game took the foreground
    public static int FocusPolls = 0;         // checks that found it there (about 200 ms each)
    static bool s_inIncident = false;
    static DateTime s_incidentStart;
    static readonly List<string> s_incidents = new List<string>();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);

    static int ForegroundPid(IntPtr fg)
    {
        uint pid; GetWindowThreadProcessId(fg, out pid);
        return (int)pid;
    }

    public static string Describe(IntPtr h)
    {
        if (h == IntPtr.Zero) return "nothing";
        var t = new StringBuilder(128); var c = new StringBuilder(64);
        GetWindowText(h, t, 128); GetClassName(h, c, 64);
        RECT r; GetWindowRect(h, out r);
        return "process " + ForegroundPid(h) + " " + c + " '" + t + "' " + (r.R - r.L) + "x" + (r.B - r.T) + " at " + r.L + "," + r.T;
    }

    // what happened to the foreground since the last call (for the run's log)
    public static string[] TakeIncidents()
    {
        lock (s_incidents) { var a = s_incidents.ToArray(); s_incidents.Clear(); return a; }
    }

    // no window may take the foreground while a game starts (its engine
    // makes and shows its window before Coop.dll is there to keep it a
    // window that is never active; with the person away from the keyboard
    // for a few minutes Windows lets a new window take the foreground: one
    // did for 1.1 s, 2026-10-15). Only the foreground process may lock it:
    // joined to the foreground thread's input, the runner counts as that
    public static bool LockForeground(bool on)
    {
        IntPtr fg = GetForegroundWindow();
        uint p; uint fgThread = fg != IntPtr.Zero ? GetWindowThreadProcessId(fg, out p) : 0;
        uint me = GetCurrentThreadId();
        bool attached = fgThread != 0 && fgThread != me && AttachThreadInput(me, fgThread, true);
        bool ok = LockSetForegroundWindow(on ? 1u : 2u);
        if (attached) AttachThreadInput(me, fgThread, false);
        return ok;
    }

    // called every ~200 ms by the runner: a test game that took the
    // foreground gives it back to the window the person used last
    static readonly object s_focusLock = new object();
    public static bool GuardFocus()
    {
        lock (s_focusLock) return GuardFocusLocked();
    }
    static bool GuardFocusLocked()
    {
        IntPtr fg = GetForegroundWindow();
        if (fg == IntPtr.Zero) return false;
        int pid = ForegroundPid(fg);
        if (!Known(pid))
        {
            if (s_inIncident)
            {
                s_inIncident = false;
                lock (s_incidents) s_incidents.Add("the foreground is back after " + (int)(DateTime.Now - s_incidentStart).TotalMilliseconds + " ms: " + Describe(fg));
            }
            s_userWnd = fg;
            return false;
        }
        FocusPolls++;
        if (!s_inIncident)
        {
            s_inIncident = true;
            s_incidentStart = DateTime.Now;
            FocusIncidents++;
            lock (s_incidents) s_incidents.Add("a test game took the foreground: " + Describe(fg) + "; the person's window: " + Describe(s_userWnd));
        }
        if (s_userWnd != IntPtr.Zero)
        {
            LockSetForegroundWindow(2);
            uint p; uint fgThread = GetWindowThreadProcessId(fg, out p);
            uint me = GetCurrentThreadId();
            bool attached = fgThread != 0 && fgThread != me && AttachThreadInput(me, fgThread, true);
            SetForegroundWindow(s_userWnd);
            if (attached) AttachThreadInput(me, fgThread, false);
        }
        return true;
    }

    // ---- at once, not at the next check: Windows tells this thread when any
    // window becomes the foreground or is shown (out of context, through
    // this thread's message loop); a test game's goes back / off screen in
    // the same moment
    delegate void WinEventProc(IntPtr hook, uint ev, IntPtr hwnd, int idObject, int idChild, uint thread, uint time);
    [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint min, uint max, IntPtr mod, WinEventProc proc, uint pid, uint thread, uint flags);
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; }
    [DllImport("user32.dll")] static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG msg);
    static WinEventProc s_winEvent;    // kept: the hook calls it
    static Thread s_eventThread;
    public static int EventGiveBacks = 0;

    public static void StartWatch()
    {
        if (s_eventThread != null) return;
        s_eventThread = new Thread(delegate () {
            s_winEvent = OnWinEvent;
            // EVENT_SYSTEM_FOREGROUND 3; EVENT_OBJECT_SHOW 0x8002; WINEVENT_OUTOFCONTEXT | SKIPOWNPROCESS
            SetWinEventHook(3, 3, IntPtr.Zero, s_winEvent, 0, 0, 0x0000 | 0x0002);
            SetWinEventHook(0x8002, 0x8002, IntPtr.Zero, s_winEvent, 0, 0, 0x0000 | 0x0002);
            MSG msg;
            while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) { TranslateMessage(ref msg); DispatchMessage(ref msg); }
        });
        s_eventThread.IsBackground = true;
        s_eventThread.Start();
    }

    static void OnWinEvent(IntPtr hook, uint ev, IntPtr hwnd, int idObject, int idChild, uint thread, uint time)
    {
        if (hwnd == IntPtr.Zero || idObject != 0) return;
        try
        {
            int pid = ForegroundPid(hwnd);
            if (!Known(pid) && !InJob(pid)) return;
            lock (s_pids) s_pids.Add(pid);
            RECT r; GetWindowRect(hwnd, out r);
            if (r.L > -3000 && IsWindowVisible(hwnd) && GetParentWindow(hwnd) == IntPtr.Zero)
                SetWindowPos(hwnd, new IntPtr(1), -4000, r.T, 0, 0, 0x0001 | 0x0010 | 0x0004 | 0x0200);
            if (ev == 3)
            {
                EventGiveBacks++;
                GuardFocus();
                Thread.Sleep(30);
                GuardFocus();    // (back: the incident ends here, not at the next poll)
            }
        }
        catch { }
    }
    [DllImport("user32.dll", EntryPoint = "GetParent")] static extern IntPtr GetParentWindow(IntPtr h);

    // ---- off screen: test windows left of all screens (never minimized:
    // a minimized game stops its loop) ---------------------------------------
    static Thread s_offThread;
    public static void StartOffScreen()
    {
        if (s_offThread != null) return;
        s_offThread = new Thread(OffScreenLoop);
        s_offThread.IsBackground = true;
        s_offThread.Start();
    }
    static void OffScreenLoop()
    {
        while (true)
        {
            try
            {
                EnumWindows(delegate (IntPtr h, IntPtr p) {
                    uint wp; GetWindowThreadProcessId(h, out wp);
                    if (Known((int)wp) && IsWindowVisible(h))
                    {
                        RECT r; GetWindowRect(h, out r);
                        if (r.L > -3000) SetWindowPos(h, new IntPtr(1), -4000, r.T, 0, 0, 0x0001 | 0x0010 | 0x0004 | 0x0200);
                    }
                    return true;
                }, IntPtr.Zero);
            }
            catch { }
            Thread.Sleep(50);
            if (++s_offTick % 20 == 0) JobPids();
        }
    }
    static int s_offTick = 0;

    // the size of a game's main window (the report proves it was a window)
    public static string WindowSize(int pid)
    {
        string best = "";
        int bestArea = 0;
        EnumWindows(delegate (IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp == pid && IsWindowVisible(h))
            {
                RECT r; GetWindowRect(h, out r);
                int a = (r.R - r.L) * (r.B - r.T);
                if (a > bestArea) { bestArea = a; best = (r.R - r.L) + "x" + (r.B - r.T) + " at " + r.L + "," + r.T; }
            }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}

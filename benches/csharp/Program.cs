// C# P/Invoke 基准：低层 port API（直接数据面，2 次拷贝） vs Rust frontend ABI
// （switch 路由数据面，4 次拷贝 + reader 线程/队列），外加 P/Invoke 边界开销。
//
// 用法：
//   dotnet build -c Release
//   DatarouterBench --lib <libblinkcms_datarouter.so> --switch <datarouter-switch>
//
// 依赖：switch 二进制与共享库均须为同一 layout（12）；库须为缺省构建（含
// DATAROUTER_UNSAFE，导出 datarouter_unsafe_send）。

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace DatarouterBench;

internal static class Native
{
    public const int OK = 0, INVALID_ARGUMENT = 1, NOT_FOUND = 2, BUSY = 3,
        FULL = 4, PAYLOAD_TOO_LARGE = 5, AGAIN = 6, TIMEOUT = 7,
        PERMISSION = 8, PLATFORM = 9, PROTOCOL = 10, INTERNAL = 11;
    public const uint FrontendAbiVersion = 2;
    public const int MaxPayload = 512 * 1024 - 8;

    [StructLayout(LayoutKind.Sequential)]
    public struct PortOptions // size=24 (x86-64)
    {
        public IntPtr ShmName;
        public uint TimeoutMs;
        public IntPtr AuthToken;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct MessageInfo // size=40 (x86-64)，fixed buffer 为 blittable（P/Invoke 零拷贝）
    {
        public fixed byte Source[32];
        public UIntPtr Len;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct FrontendOptions // size=80 (x86-64)
    {
        public IntPtr ShmName;
        public IntPtr ServiceName;
        public uint ConnectTimeoutMs;
        public uint RecvTimeoutMs;
        public uint AckTimeoutMs;
        public uint ReaderWaitMs;
        public UIntPtr AppQueueCapacity;
        public UIntPtr MaxMessageBytes;
        public UIntPtr MaxPendingAcks;
        public uint ChunkTtlMs;
        public UIntPtr MaxReassemblyGroups;
        public UIntPtr MaxReassemblyBytes;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct FrontendOptionsV3 // size=96 (x86-64): v2 + auth_token(8) + data_plane(1)+pad
    {
        public FrontendOptions Base;
        public IntPtr AuthToken;
        public byte DataPlane;
    }

    public const uint FrontendAbiVersionV3 = 3;
    public const byte DataPlaneDirect = 0;
    public const byte DataPlaneRouted = 1;

    [DllImport("__datarouter", EntryPoint = "datarouter_version", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr datarouter_version();

    [DllImport("__datarouter", EntryPoint = "datarouter_strerror", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr datarouter_strerror(int code);

    [DllImport("__datarouter", EntryPoint = "datarouter_open", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_open(ref PortOptions options, out IntPtr port);

    [DllImport("__datarouter", EntryPoint = "datarouter_logon", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_logon(IntPtr port,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string serviceName,
        byte[] errbuf, UIntPtr errbufLen);

    [DllImport("__datarouter", EntryPoint = "datarouter_unsafe_send", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_unsafe_send(IntPtr port,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string targetService,
        byte[] data, UIntPtr len);

    [DllImport("__datarouter", EntryPoint = "datarouter_recv", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_recv(IntPtr port, byte[] buffer,
        UIntPtr capacity, ref MessageInfo info);

    [DllImport("__datarouter", EntryPoint = "datarouter_frame_len", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_frame_len(IntPtr port, out UIntPtr payloadLen);

    [DllImport("__datarouter", EntryPoint = "datarouter_wait", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_wait(IntPtr port, uint timeoutMs);

    [DllImport("__datarouter", EntryPoint = "datarouter_close", CallingConvention = CallingConvention.Cdecl)]
    public static extern void datarouter_close(IntPtr port);

    [DllImport("__datarouter", EntryPoint = "datarouter_frontend_abi_version", CallingConvention = CallingConvention.Cdecl)]
    public static extern uint datarouter_frontend_abi_version();

    [DllImport("__datarouter", EntryPoint = "datarouter_frontend_open_v2", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_frontend_open_v2(ref FrontendOptions options,
        UIntPtr optionsSize, uint abiVersion, out IntPtr frontend);

    [DllImport("__datarouter", EntryPoint = "datarouter_frontend_open_v3", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_frontend_open_v3(ref FrontendOptionsV3 options,
        UIntPtr optionsSize, uint abiVersion, out IntPtr frontend);

    [DllImport("__datarouter", EntryPoint = "datarouter_frontend_send", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_frontend_send(IntPtr frontend,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string targetService,
        byte[] data, UIntPtr len, uint timeoutMs);

    [DllImport("__datarouter", EntryPoint = "datarouter_frontend_recv", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_frontend_recv(IntPtr frontend, byte[] buffer,
        UIntPtr capacity, ref MessageInfo info, uint timeoutMs);

    [DllImport("__datarouter", EntryPoint = "datarouter_frontend_close", CallingConvention = CallingConvention.Cdecl)]
    public static extern void datarouter_frontend_close(IntPtr frontend);
}

internal static class Program
{
    private static string _libPath = "";
    private static string _switchPath = "";

    private static void Main(string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--lib" && i + 1 < args.Length) _libPath = args[++i];
            else if (args[i] == "--switch" && i + 1 < args.Length) _switchPath = args[++i];
        }
        if (string.IsNullOrEmpty(_libPath) || string.IsNullOrEmpty(_switchPath))
        {
            Console.Error.WriteLine("usage: DatarouterBench --lib <lib.so> --switch <datarouter-switch>");
            Environment.Exit(2);
        }
        if (NativeResolver.Register(_libPath) is { } err)
        {
            Console.Error.WriteLine($"lib load failed: {err}");
            Environment.Exit(2);
        }
        if (!System.IO.File.Exists(_switchPath))
        {
            Console.Error.WriteLine($"switch not found: {_switchPath}");
            Environment.Exit(2);
        }

        Console.WriteLine($"lib: {_libPath}");
        Console.WriteLine($"switch: {_switchPath}");
        Console.WriteLine($"C version: {Ptr(Native.datarouter_version())}");
        Console.WriteLine($"frontend ABI: {Native.datarouter_frontend_abi_version()}");
        Console.WriteLine();

        int[] sizes = { 64, 1024, 65536, 307200 };
        int[] iters = { 2000, 2000, 400, 200 };
        int warmup = 100;

        MeasurePInvokeOverhead();

        string shm = $"/datarouter-cs-{Environment.ProcessId}-{Stopwatch.GetTimestamp()}";
        using var switchProc = SpawnSwitch(shm);

        Console.WriteLine("=== 低层 port API（直接数据面，2 次拷贝）===");
        using (var a = new PortApiBench(shm, sizes, iters, warmup))
        {
            a.Run();
        }
        Console.WriteLine();
        Console.WriteLine("=== Rust frontend ABI（直接数据面，缺省；open_v2/v3-direct）===");
        using (var b = new FrontendBench(shm, sizes, iters, warmup, Native.DataPlaneDirect))
        {
            b.Run();
        }
        Console.WriteLine();
        Console.WriteLine("=== Rust frontend ABI（路由数据面，open_v3 data_plane=ROUTED）===");
        using (var b2 = new FrontendBench(shm, sizes, iters, warmup, Native.DataPlaneRouted))
        {
            b2.Run();
        }

        switchProc.Kill();
        switchProc.WaitForExit();
    }

    private static System.Diagnostics.Process SpawnSwitch(string shm)
    {
        var p = new System.Diagnostics.Process
        {
            StartInfo = new System.Diagnostics.ProcessStartInfo
            {
                FileName = _switchPath,
                Arguments = $"--name {shm} --ttl-ms 60000 --allow bench_sender,bench_receiver",
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            }
        };
        p.Start();
        System.Threading.Thread.Sleep(200);
        return p;
    }

    private static void MeasurePInvokeOverhead()
    {
        const int n = 2_000_000;
        // warmup
        for (int i = 0; i < 100_000; i++) Native.datarouter_version();
        var sw = Stopwatch.StartNew();
        for (int i = 0; i < n; i++) Native.datarouter_version();
        sw.Stop();
        double nsPerCall = sw.Elapsed.TotalNanoseconds / n;
        Console.WriteLine($"P/Invoke 空调用开销（datarouter_version，{n} 次）: {nsPerCall:F1} ns/call");
        Console.WriteLine();
    }

    internal static string Ptr(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "";

    internal static void Check(int rc)
    {
        if (rc != Native.OK)
        {
            throw new InvalidOperationException($"rc={rc}: {Ptr(Native.datarouter_strerror(rc))}");
        }
    }

    internal static byte[] MakePayload(int size)
    {
        var data = new byte[size];
        Array.Fill(data, (byte)0xAB);
        return data;
    }

    // 8 字节头：type(1) flags(1) var_data_len(LE u16) body_len(LE u32) + body。
    internal static byte[] Pack(byte[] body)
    {
        var wire = new byte[8 + body.Length];
        wire[0] = 0x07; // MSG_BUSINESS_REQUEST
        wire[1] = 0;
        wire[2] = 0; wire[3] = 0;
        wire[4] = (byte)(body.Length & 0xFF);
        wire[5] = (byte)((body.Length >> 8) & 0xFF);
        wire[6] = (byte)((body.Length >> 16) & 0xFF);
        wire[7] = (byte)((body.Length >> 24) & 0xFF);
        Buffer.BlockCopy(body, 0, wire, 8, body.Length);
        return wire;
    }
}

// 路径 A：低层 port API —— datarouter_open/logon + unsafe_send + wait/frame_len/recv。
internal sealed class PortApiBench : IDisposable
{
    private readonly string _shm;
    private readonly int[] _sizes;
    private readonly int[] _iters;
    private readonly int _warmup;
    private readonly IntPtr _sender, _receiver;
    private readonly byte[] _recvBuf = new byte[Native.MaxPayload];

    public PortApiBench(string shm, int[] sizes, int[] iters, int warmup)
    {
        _shm = shm; _sizes = sizes; _iters = iters; _warmup = warmup;
        _sender = Open("bench_sender");
        _receiver = Open("bench_receiver");
    }

    private IntPtr Open(string service)
    {
        var opts = new Native.PortOptions();
        var shmPtr = Marshal.StringToCoTaskMemUTF8(_shm);
        try
        {
            opts.ShmName = shmPtr;
            opts.TimeoutMs = 5000;
            opts.AuthToken = IntPtr.Zero;
            Program.Check(Native.datarouter_open(ref opts, out var port));
            var errbuf = new byte[256];
            Program.Check(Native.datarouter_logon(port, service, errbuf, (UIntPtr)errbuf.Length));
            return port;
        }
        finally { Marshal.FreeCoTaskMem(shmPtr); }
    }

    public void Run()
    {
        Console.WriteLine("payload\titers\tp50(µs)\tp99(µs)\tmean(µs)\tRT/s");
        for (int i = 0; i < _sizes.Length; i++)
        {
            var payload = Program.MakePayload(_sizes[i]);
            for (int w = 0; w < _warmup; w++) RoundTrip(payload);
            var samples = new List<double>(_iters[i]);
            for (int k = 0; k < _iters[i]; k++)
            {
                var t0 = Stopwatch.GetTimestamp();
                RoundTrip(payload);
                samples.Add((Stopwatch.GetTimestamp() - t0) / (double)Stopwatch.Frequency * 1e6);
            }
            samples.Sort();
            double p50 = samples[_iters[i] / 2];
            double p99 = samples[(int)(_iters[i] * 0.99) - 1];
            double mean = 0; foreach (var s in samples) mean += s;
            mean /= _iters[i];
            Console.WriteLine($"{_sizes[i]}\t{_iters[i]}\t{p50:F2}\t{p99:F2}\t{mean:F2}\t{1e6 / mean:F0}");
        }
    }

    private void RoundTrip(byte[] payload)
    {
        Program.Check(Native.datarouter_unsafe_send(_sender, "bench_receiver", payload, (UIntPtr)payload.Length));
        Program.Check(Native.datarouter_wait(_receiver, 5000));
        var info = new Native.MessageInfo();
        Program.Check(Native.datarouter_frame_len(_receiver, out _));
        Program.Check(Native.datarouter_recv(_receiver, _recvBuf, (UIntPtr)_recvBuf.Length, ref info));

        Program.Check(Native.datarouter_unsafe_send(_receiver, "bench_sender", payload, (UIntPtr)payload.Length));
        Program.Check(Native.datarouter_wait(_sender, 5000));
        Program.Check(Native.datarouter_frame_len(_sender, out _));
        Program.Check(Native.datarouter_recv(_sender, _recvBuf, (UIntPtr)_recvBuf.Length, ref info));
    }

    public void Dispose()
    {
        Native.datarouter_close(_sender);
        Native.datarouter_close(_receiver);
    }
}

// 路径 B：Rust frontend ABI —— data_plane 直接（缺省）或路由（v3 opt-in）。
internal sealed class FrontendBench : IDisposable
{
    private readonly string _shm;
    private readonly int[] _sizes;
    private readonly int[] _iters;
    private readonly int _warmup;
    private readonly byte _dataPlane;
    private readonly IntPtr _sender, _receiver;
    private readonly byte[] _recvBuf = new byte[1024 * 1024];

    public FrontendBench(string shm, int[] sizes, int[] iters, int warmup, byte dataPlane)
    {
        _shm = shm; _sizes = sizes; _iters = iters; _warmup = warmup; _dataPlane = dataPlane;
        _sender = Open("bench_sender");
        _receiver = Open("bench_receiver");
    }

    private IntPtr Open(string service)
    {
        if (_dataPlane == Native.DataPlaneDirect)
        {
            var opts = new Native.FrontendOptions();
            var shmPtr = Marshal.StringToCoTaskMemUTF8(_shm);
            var svcPtr = Marshal.StringToCoTaskMemUTF8(service);
            try
            {
                opts.ShmName = shmPtr;
                opts.ServiceName = svcPtr;
                opts.ConnectTimeoutMs = 5000;
                opts.RecvTimeoutMs = 5000;
                Program.Check(Native.datarouter_frontend_open_v2(ref opts,
                    (UIntPtr)Marshal.SizeOf<Native.FrontendOptions>(),
                    Native.FrontendAbiVersion, out var frontend));
                return frontend;
            }
            finally
            {
                Marshal.FreeCoTaskMem(shmPtr);
                Marshal.FreeCoTaskMem(svcPtr);
            }
        }
        var v3 = new Native.FrontendOptionsV3();
        var shmPtr3 = Marshal.StringToCoTaskMemUTF8(_shm);
        var svcPtr3 = Marshal.StringToCoTaskMemUTF8(service);
        try
        {
            v3.Base.ShmName = shmPtr3;
            v3.Base.ServiceName = svcPtr3;
            v3.Base.ConnectTimeoutMs = 5000;
            v3.Base.RecvTimeoutMs = 5000;
            v3.AuthToken = IntPtr.Zero;
            v3.DataPlane = _dataPlane;
            Program.Check(Native.datarouter_frontend_open_v3(ref v3,
                (UIntPtr)Marshal.SizeOf<Native.FrontendOptionsV3>(),
                Native.FrontendAbiVersionV3, out var frontend));
            return frontend;
        }
        finally
        {
            Marshal.FreeCoTaskMem(shmPtr3);
            Marshal.FreeCoTaskMem(svcPtr3);
        }
    }

    public void Run()
    {
        Console.WriteLine("payload\titers\tp50(µs)\tp99(µs)\tmean(µs)\tRT/s");
        for (int i = 0; i < _sizes.Length; i++)
        {
            var wire = Program.Pack(Program.MakePayload(_sizes[i]));
            for (int w = 0; w < _warmup; w++) RoundTrip(wire);
            var samples = new List<double>(_iters[i]);
            for (int k = 0; k < _iters[i]; k++)
            {
                var t0 = Stopwatch.GetTimestamp();
                RoundTrip(wire);
                samples.Add((Stopwatch.GetTimestamp() - t0) / (double)Stopwatch.Frequency * 1e6);
            }
            samples.Sort();
            double p50 = samples[_iters[i] / 2];
            double p99 = samples[(int)(_iters[i] * 0.99) - 1];
            double mean = 0; foreach (var s in samples) mean += s;
            mean /= _iters[i];
            Console.WriteLine($"{_sizes[i]}\t{_iters[i]}\t{p50:F2}\t{p99:F2}\t{mean:F2}\t{1e6 / mean:F0}");
        }
    }

    private void RoundTrip(byte[] wire)
    {
        Program.Check(Native.datarouter_frontend_send(_sender, "bench_receiver", wire,
            (UIntPtr)wire.Length, 0));
        var info = new Native.MessageInfo();
        Program.Check(Native.datarouter_frontend_recv(_receiver, _recvBuf,
            (UIntPtr)_recvBuf.Length, ref info, 5000));

        Program.Check(Native.datarouter_frontend_send(_receiver, "bench_sender", wire,
            (UIntPtr)wire.Length, 0));
        Program.Check(Native.datarouter_frontend_recv(_sender, _recvBuf,
            (UIntPtr)_recvBuf.Length, ref info, 5000));
    }

    public void Dispose()
    {
        Native.datarouter_frontend_close(_sender);
        Native.datarouter_frontend_close(_receiver);
    }
}

// 运行时按实际路径解析 .so（DllImport 用占位名 "__datarouter"）。
internal static class NativeResolver
{
    public static string? Register(string libPath)
    {
        try
        {
            NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, asm, path) =>
            {
                if (name == "__datarouter")
                {
                    if (NativeLibrary.TryLoad(libPath, asm, path, out var handle))
                        return handle;
                    return IntPtr.Zero;
                }
                return IntPtr.Zero;
            });
            return null;
        }
        catch (Exception ex)
        {
            return ex.Message;
        }
    }
}

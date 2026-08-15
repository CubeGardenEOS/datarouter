# C# 使用 DataRouter（P/Invoke 指南）

## 0. 背景：unsafe 已是缺省模式

`unsafe` 直接数据面是 crate 的缺省 feature（`default = ["unsafe"]`，2026-08 翻转）。
对 C# 消费方来说关键点：**C ABI 与 shared-memory layout 不变**（layout 12、frontend
ABI v2），直接数据面只是**多导出** `datarouter_unsafe_send` 符号。因此：

- 直接数据面（2 次拷贝）：`datarouter_unsafe_send` + `datarouter_recv`（C 层没有
  独立的 unsafe_recv 符号——它就是直接读 RX ring 的 `datarouter_recv`）。
- 缺省构建（`-DDATAROUTER_UNSAFE` 或 Rust 缺省 feature）才导出 `datarouter_unsafe_send`；
  若构建时未定义该宏，`datarouter_unsafe_send` 不存在，只能走 switch 路由的
  `datarouter_send`。

两种 C# 接入路径：

| 路径 | 数据面 | 拷贝数 | 需要 Rust？ | 功能 |
|---|---|---|---|---|
| A. 低层 port API（`datarouter.h`） | 直接（unsafe） | 2 | **否**（纯 C） | 单帧 ≤ 512KiB，无分块/ACK；发送端自己 keepalive |
| B. frontend API（`datarouter_frontend.h`） | switch 路由 | 4 | 是（Rust staticlib） | 分块+逐片 ACK、重组、队列、自动 keepalive、NOT_FOUND 恢复 |

### 选型建议（重要）

**默认使用路径 B（Rust frontend ABI）**，与 Python/Java/Rust 绑定行为一致。
**不建议直接使用低层 port API（路径 A）作为首选**——它是维护中的可选项，
保留给明确需要直接数据面（2 拷贝）性能并接受其约束的场景。理由：

- 无分块/ACK、无重组、无应用队列：>512KiB 消息无法传输，ring 满直接 `ERR_FULL`；
- **无自动 keepalive**：必须自行周期调用 `datarouter_touch`，否则 TTL 后 slot 被回收；
- 无 NOT_FOUND/注册恢复逻辑（Rust frontend 内建自愈）；
- SPSC 单写者约束：同一目标 ring 只能有一个逻辑写者，混用 `datarouter_send` 与
  `datarouter_unsafe_send` 即双写者竞态；
- 无协议校验：`unsafe_send` 原样写帧，错误 wire 数据直接进对方 RX ring。

性能实测（同 harness 对比，`docs/benchmark-csharp-2026-08.md`）：frontend（路由面）
比 port API（直接面）慢 3–11×，但差异**来自数据面本身**（4 次拷贝 + reader 线程/队列），
不是语言边界——P/Invoke 开销仅 ~13ns/次，Rust 原生 safe vs unsafe 同样是 7× 量级。
若最终选择路径 A，用 blittable P/Invoke 结构体（见 §5）并自行实现 keepalive。

C++ 接入同一套 C ABI：默认同样用路径 B（Rust frontend），低层 port API 的警告同上。

## 1. 构建 C# 要链接的共享库

### 路径 A：纯 C 共享库（无需 Rust、无需 cargo）

```bash
cc -shared -fPIC -O3 -fno-strict-aliasing \
   -DDATAROUTER_CHANNEL_SIZE=524288U -DDATAROUTER_UNSAFE \
   -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L \
   src/common.c src/port.c src/registry.c src/ring.c src/scheduler.c \
   src/signal.c src/router.c src/config.c src/platform_linux.c \
   -o libdatarouter.so -lpthread
```

`-DDATAROUTER_UNSAFE` 导出 `datarouter_unsafe_send`（缺省模式）。验证：
`nm -D libdatarouter.so | grep datarouter_open`。

### 路径 B：含 Rust frontend 的共享库

```bash
cargo build --release          # 产出 target/release/libblinkcms_datarouter.a（缺省=unsafe）
cc -shared -o libdatarouter.so \
   -Wl,--whole-archive target/release/libblinkcms_datarouter.a \
   -Wl,--no-whole-archive -lrt -lpthread -ldl
```

路径 B 同时导出低层 API 与 frontend API（`datarouter_frontend_open_v2` 等）。

## 2. P/Invoke 声明（x86-64 布局已与 C 核对）

```csharp
using System;
using System.Runtime.InteropServices;

internal static class Datarouter
{
    public const int OK = 0, INVALID_ARGUMENT = 1, NOT_FOUND = 2, BUSY = 3,
        FULL = 4, PAYLOAD_TOO_LARGE = 5, AGAIN = 6, TIMEOUT = 7,
        PERMISSION = 8, PLATFORM = 9, PROTOCOL = 10, INTERNAL = 11;
    public const int MaxPayload = 512 * 1024 - 8;   // layout 12：2^19 - 8

    [StructLayout(LayoutKind.Sequential)]
    public struct Options   // size=24：IntPtr(8) + uint(4) + pad(4) + IntPtr(8)
    {
        public IntPtr ShmName;      // UTF-8，如 "/datarouter"
        public uint TimeoutMs;      // connect/recv 超时（毫秒）
        public IntPtr AuthToken;    // switch --token 时的共享密钥；无鉴权传 IntPtr.Zero
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MessageInfo // size=40：byte[32] + UIntPtr(8)
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] Source;       // 来源服务名（NUL 结尾）
        public UIntPtr Len;         // 实际帧负载长度
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TransportStats // size=48：6 × ulong
    {
        public ulong CorruptedRings;
        public ulong RegistrationRecoveries;
        public ulong BlockedWakeups;
        public ulong RegistrationRequestRecoveries;
        public ulong DeadProcessReaps;
        public ulong TransportGeneration;
    }

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr datarouter_version();

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr datarouter_strerror(int code);

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_open(ref Options options, out IntPtr port);

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_logon(IntPtr port, byte[] serviceName,
        byte[] errbuf, UIntPtr errbufLen);

    // 直接数据面（缺省模式）：直写目标 RX ring，绕过 switch 路由，2 次拷贝
    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_unsafe_send(IntPtr port, byte[] targetService,
        byte[] data, UIntPtr len);

    // switch 路由（safe 模式，4 次拷贝）
    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_send(IntPtr port, byte[] targetService,
        byte[] data, UIntPtr len);

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_recv(IntPtr port, byte[] buffer, UIntPtr capacity,
        ref MessageInfo info);

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_frame_len(IntPtr port, out UIntPtr payloadLen);

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_wait(IntPtr port, uint timeoutMs); // 阻塞等 rx_signal

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_poll(IntPtr port);

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern int datarouter_touch(IntPtr port); // keepalive，防 TTL 回收

    [DllImport("libdatarouter.so", CallingConvention = CallingConvention.Cdecl)]
    public static extern void datarouter_close(IntPtr port);
}
```

## 3. 最小示例（直接数据面）

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

static class Program
{
    static IntPtr Open(string service, string shm = "/datarouter")
    {
        var opts = new Datarouter.Options();
        opts.ShmName = Utf8(shm);
        opts.TimeoutMs = 3000;
        opts.AuthToken = IntPtr.Zero; // switch 无 --token
        if (Datarouter.datarouter_open(ref opts, out var port) != Datarouter.OK)
            throw new InvalidOperationException("open failed");
        var err = new byte[256];
        int rc = Datarouter.datarouter_logon(port, Utf8(service), err,
            (UIntPtr)err.Length);
        if (rc != Datarouter.OK)
            throw new InvalidOperationException(
                $"logon {service} failed: {Marshal.PtrToStringUTF8(Datarouter.datarouter_strerror(rc))}");
        return port;
    }

    static void Main()
    {
        // 前提：datarouter-switch 已启动（switch 仅承担控制面）
        var core = Open("core");
        var portal = Open("portal");

        // 发送端 keepalive 线程（switch 缺省 TTL 30s，建议每 2–3s touch 一次）
        var keepalive = new Thread(() => { while (true) { Thread.Sleep(2000);
            Datarouter.datarouter_touch(core); Datarouter.datarouter_touch(portal); } });
        keepalive.IsBackground = true; keepalive.Start();

        // 直接发送：portal -> core 的 RX ring（2 次拷贝，直写）
        var payload = Encoding.UTF8.GetBytes("hello from C#");
        int rc = Datarouter.datarouter_unsafe_send(portal, Utf8("core"), payload,
            (UIntPtr)payload.Length);
        if (rc == Datarouter.FULL) { /* ring 满：直接数据面无排队协议，退避重试 */ }
        else if (rc != Datarouter.OK) throw new InvalidOperationException(
            $"unsafe_send: {Marshal.PtrToStringUTF8(Datarouter.datarouter_strerror(rc))}");

        // 接收端：wait(rx_signal) -> frame_len -> recv（无 reader 线程，应用即 reader）
        var buf = new byte[Datarouter.MaxPayload];
        if (Datarouter.datarouter_wait(core, 3000) == Datarouter.OK)
        {
            Datarouter.datarouter_frame_len(core, out var len);
            var info = new Datarouter.MessageInfo();
            if (Datarouter.datarouter_recv(core, buf, (UIntPtr)buf.Length,
                ref info) == Datarouter.OK)
                Console.WriteLine($"got {info.Len} bytes from " +
                    Encoding.UTF8.GetString(info.Source).TrimEnd('\0'));
        }

        Datarouter.datarouter_close(core);
        Datarouter.datarouter_close(portal);
    }

    static IntPtr Utf8(string s) => Marshal.StringToCoTaskMemUTF8(s);
}
```

## 4. 关键注意点

1. **先启动 switch**：`datarouter-switch --name /datarouter [--token secret]`。缺 switch
   时 `datarouter_open` 返回 `ERR_NOT_FOUND`。
2. **鉴权**：switch 带 `--token` 启动时，`datarouter_options.auth_token` 必须匹配，
   否则 `ERR_PERMISSION`。
3. **keepalive**：低层 API 没有自动 keepalive，必须周期调用 `datarouter_touch`，
   否则超过 `--ttl-ms`（缺省 30000ms）slot 被回收（后续收发变 `NOT_FOUND`）。
4. **直接数据面约束**：同一目标 RX ring 只允许**一个逻辑写者**（SPSC）；ring 满返回
   `ERR_FULL`（无阻塞重排协议）；单帧 ≤ `MAX_APP_PAYLOAD`（512KiB-8），无分块/ACK。
5. **接收模型**：`datarouter_recv` 非阻塞（空返回 `ERR_AGAIN`）；先 `datarouter_wait`
   或 `datarouter_poll` 再 recv。保持单接收线程（SPSC）。
6. **超尺寸帧**：`datarouter_recv` 缓冲不足返回 `PAYLOAD_TOO_LARGE`，`info.Len` 给出
   所需长度，帧保留，可用 `datarouter_frame_len` 预取后重试。
7. **错误字符串**：`datarouter_strerror(rc)` 返回 UTF-8 C 字符串。
8. **Windows**：仓库有 `platform_windows.c`（Win32 file mapping 后端），把该文件替换
   `platform_linux.c` 编译出 DLL，P/Invoke 声明不变。
9. **布局升级**：layout/ABI 变更（layout 13+）时，C# 侧 `Options`/`MessageInfo` 等
   P/Invoke 结构体必须与 C 头文件同步；用 `tests/abi_compat.rs` /
   `python/tests/test_native_abi.py` 相同的思路在 C# 侧钉扎 struct size。

## 5. 性能：P/Invoke 边界与数据面选型

实测见 `docs/benchmark-csharp-2026-08.md`（同 harness 对比，可复现）。要点：

- **P/Invoke 边界 ≈ 13ns/次**，相对 µs 级消息处理可忽略。
- 直接面（路径 A）vs 路由面（路径 B）：64B p50 6.9µs vs 24.0µs；300KiB 32.8µs vs
  292.6µs（3–11×）。差距来自数据面（拷贝数 + reader 线程/队列），不是语言绑定——
  Rust 原生 safe vs unsafe 也是 7× 量级。
- C# vs 原生 Rust：带宽受限（大帧）≈ 1.1×，几乎无差；延迟受限（小帧）≈ 2×。
- **blittable 结构体**：`datarouter_message_info` 用 `fixed byte Source[32]` 代替
  `byte[]` 字段（`[MarshalAs(UnmanagedType.ByValArray)]` 会让结构体非 blittable，
  每次调用额外拷贝）。已核对布局一致（40B），可安全替换；`benches/csharp/Program.cs`
  即 blittable 版。

## 6. C++ 支持

C++ 与 C# 消费同一 C ABI：`#include "datarouter_frontend.h"` 直接链共享库即可
（`extern "C"` 已包装）。选型与警告同 C#：**默认用 Rust frontend ABI**（路径 B），
低层 port API（路径 A）是维护中的可选项，不建议作为首选（无 keepalive/分块/ACK/队列/
恢复，SPSC 单写者约束）。C++ 侧直接用 `datarouter_frontend_open_v2` 返回的不透明
句柄；struct 布局见 §2（`datarouter_frontend_options` 80B、`datarouter_message_info`
40B，x86-64，可直接 `reinterpret_cast` 对齐）。

> 已知缺口：Rust 侧 `Frontend::connect_unsafe`（直接面 + frontend 正确性兼得）目前
> 没有 C ABI 导出；C#/C++/Python 要直接面只能走低层 port API。后续可给 frontend
> 增加 `open_v2_unsafe`/`unsafe_send`/`unsafe_recv`/`frame_len` 的 C 导出（frontend
> ABI v3，向后兼容），让所有绑定都能拿到「直接面 + 自动 keepalive/恢复/校验」。

# C# P/Invoke 接入基准（2026-08）：低层 port API vs Rust frontend ABI

回答「C# 接入 Rust ABI 是否会降低性能」。结论先行：**P/Invoke 边界本身 ≈ 13ns/次，
可忽略；性能差异来自数据面选型，不是语言边界**。同 harness 同进程实测（Ryzen 5
3600，WSL2，后台有 Gradle daemon 噪声）：

| payload | 低层 port API（直接面，2 拷贝）p50 | Rust frontend（路由面，4 拷贝）p50 | 比值 |
|---|---|---|---|
| 64B | 6.86 µs | 23.98 µs | 3.5× |
| 1KiB | 6.85 µs | 21.10 µs | 3.1× |
| 64KiB | 11.68 µs | 134.83 µs | 11.5× |
| 300KiB | 32.78 µs | 292.58 µs | 8.9× |

- **P/Invoke 空调用开销：13.1 ns/call**（`datarouter_version` ×200 万次）。
- 与原生 Rust 同机数字对比（`docs/benchmark-modes-2026-08.md`：unsafe 3.5µs、
  safe 25.2µs @64B pingpong）：
  - C# 直接面 6.86µs vs Rust 直接面 3.5µs ≈ **1.96×**（延迟受限时 C# 层固定开销
    相对放大；300KiB 带宽受限时 C# 32.8µs vs Rust 29.8µs ≈ **1.1×**，几乎无差）。
  - C# 路由面 23.98µs vs Rust safe 25.2µs ≈ **0.95×**（完全相同量级——路由面的
    4 拷贝 + reader 线程/队列开销完全掩盖语言边界）。
- **frontend（路由面）比 port API（直接面）慢 3–11×，原因是数据面本身**：
  2 次额外 memcpy（switch 路由 + reader→队列）、reader 线程唤醒延迟、队列/重组
  校验——Rust 自身的 safe vs unsafe 也是 7×（25.2 vs 3.5µs）。语言绑定没有放大
  这个差距。

## 结论

1. 用 C# 调 datarouter 不会因为「Rust ABI」而损失性能；代价是每调用 ~13ns 的
   P/Invoke，相对 µs 级消息处理可忽略。大帧（带宽受限）C# 与原生几乎相同。
2. **性能选择的真正杠杆是数据面**：直接面（2 拷贝）≈ 前端路由面（4 拷贝 + 队列）
   的 1/3~1/11。Rust 侧已有 `connect_unsafe` 兼得「直接面 + frontend 正确性」，
   但该路径目前**没有 C ABI 导出**——C#/C++/Python 要直接面只能走低层 port API。
3. 工程建议：
   - 默认用 **Rust frontend ABI**（`datarouter_frontend.h`）：分块/ACK、重组、队列、
     自动 keepalive、NOT_FOUND 恢复，行为与 Rust/Python/Java 一致；对绝大多数
     业务（µs~ms 级）性能足够。
   - 只有测量证明 frontend 是瓶颈、且接受直接面约束（单写者/无分块/无队列/自管
     keepalive/`ERR_FULL`）时，才用低层 port API（`datarouter_unsafe_send` + recv）。
   - P/Invoke 结构体用 **blittable 声明**（`fixed byte[32]` 代替 `byte[]` 字段）：
     实测小帧 p50 再降 ~10%，frontend recv 路径降幅更大。

## 复现

```bash
# 构建 C# 基准（需要 dotnet SDK 8+）
cd benches/csharp && dotnet build -c Release
# 运行（--lib 用 Rust staticlib 链接出的共享库，含全部 C + Rust 符号）
cd ../.. && ./benches/csharp/bin/Release/net8.0/DatarouterBench \
    --lib build/libblinkcms_datarouter.so --switch build/datarouter-switch
```

`build/libblinkcms_datarouter.so` 的构建方式与 python wheel 相同：
`cargo build --release` 后用 `cc -shared -Wl,--whole-archive` 链接
`target/release/libblinkcms_datarouter.a`。基准自动 spawn 独立 switch
（唯一 shm 名 + allowlist），结束后 kill。

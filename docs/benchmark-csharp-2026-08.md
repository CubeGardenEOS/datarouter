# C# P/Invoke 接入基准（2026-08 更新：frontend 缺省即直接数据面）

回答「C# 接入 Rust frontend ABI 是否会降低性能」。结论：**不会——frontend ABI 是
C# 侧最快的选项，而且是缺省行为**。P/Invoke 边界 ~12ns/次可忽略；性能差异来自数据
面选型（直接面 2 拷贝 vs 路由面 4 拷贝 + reader/队列）。

自 0.2.x：`datarouter_frontend_open_v2` 缺省打开**直接（unsafe）数据面**；
`open_v3` 的 `data_plane=ROUTED` 才回到 switch 路由面。实测（Ryzen 5 3600，WSL2，
同 harness 同进程 pingpong p50，后台有 Gradle daemon 噪声）：

| payload | port API 直接面 | **frontend 直接面（缺省）** | frontend 路由面（v3 opt-in） |
|---|---|---|---|
| 64B | 6.97 µs | **4.17 µs** | 34.14 µs |
| 1KiB | 7.92 µs | **4.15 µs** | 40.26 µs |
| 64KiB | 11.86 µs | **10.68 µs** | 136.43 µs |
| 300KiB | 33.44 µs | 33.97 µs | 305.83 µs |

- **P/Invoke 空调用开销：~12 ns/次**（`datarouter_version` ×200 万次）。
- **frontend 直接面比裸 port API 更快（64B 4.17 vs 6.97µs）**：Rust 侧一个
  `frontend_send`/`frontend_recv` 调用内完成 wait+recv（每方向 2 次 P/Invoke），而
  port API 路径要 `unsafe_send + wait + frame_len + recv` 4 次调用。300KiB 带宽受限
  时两者持平（同样的 2 次 memcpy）。
- **frontend 路由面（v3 `data_plane=ROUTED`）是直接面的 ~8 倍**：4 次拷贝 + switch
  路由 + reader 线程/队列。Rust 原生 safe vs unsafe 同为 7× 量级——差距在数据面，
  语言绑定不放大。
- blittable 结构体（`fixed byte[32]`）仍建议：非 blittable 的 `byte[]` 字段让
  `MessageInfo` 每次调用额外拷贝（实测小帧再降 ~10%，frontend recv 路径降幅更大）。

## 结论

1. C#/Go/C++ 用 Rust frontend ABI 不会损失性能——直接面缺省即最优路径（还自带
   keepalive、NOT_FOUND 恢复、协议校验、`frame_len` 缓冲预取）。
2. 只有需要 >512KiB 分块消息或队列语义时才用 `open_v3 data_plane=ROUTED`（约 8×
   延迟代价，换来分块/ACK/重组）。
3. 低层 port API（`datarouter.h`）仍是维护中的可选项（无 keepalive/分块/队列/恢复），
   不建议作为首选；本机对比下它并不比 frontend 直接面快。

## 复现

```bash
cd benches/csharp && dotnet build -c Release
cd ../.. && ./benches/csharp/bin/Release/net8.0/DatarouterBench \
    --lib build/libblinkcms_datarouter.so --switch build/datarouter-switch
```

`build/libblinkcms_datarouter.so`：`cargo build --release` 后
`cc -shared -Wl,--whole-archive` 链接 `target/release/libblinkcms_datarouter.a`。
基准自动 spawn 独立 switch（唯一 shm 名 + allowlist），结束后 kill。

# DataRouter 安全审查与未完成门禁（2026-09-09）

本记录对应 `1990c84` 的开发候选。**整个原计划尚未完成，不可据此合入或发布。**
威胁模型为协作进程；不是抵御能任意修改整个共享映射的恶意进程的隔离边界。

## 已复现与修改

| 项目 | 修改及证据 | 状态 |
|---|---|---|
| C 空指针 | `validate_tlvs`、`frame_layout`、`chunk_metadata`、`keep_verify_ref` 增加入口检查；旧源码 sanitizer 复现四处崩溃 | 已验证 |
| KEEP 初始化 | 槽位乘法/容量约束、≤8 MiB、8 字节几何；旧源码极大 slot_capacity 触发 heap-buffer-overflow | 已验证 |
| KEEP 未对齐 | 条目按 8 字节对齐；不同长度重复覆盖测试在旧源码触发 UBSan | 已验证 |
| KEEP 位移、布局 | 位移前校验槽位；校验映射大小、内部几何、条目与槽位范围/计数 | 已验证基础边界 |
| 空键/空值/短缓冲 | 独立 pad 哨兵；空值允许空输出；FULL 返回所需长度 | 已验证 |
| TTL | 拒绝 `now + ttl` 溢出 | 已验证 |
| 前缀删除 | ≤8 MiB 完整扫描；6000 条记录及无匹配前缀不再停滞 | 已验证 |
| KEEP 覆盖 | 独占 C 读引用覆盖整个 memcpy/指针读取期间，verify 消费对应引用 | 已验证，读并发性能待测 |
| KEEP 恢复 | PID 原子发布所有权；确认死亡后独占整体失效；PID 复用保守延迟 | 暂停、SIGKILL 与整体恢复测试通过 |
| 注册代次 | KEEP 写入校验 route_id、PID、process_start_id、授权位 | 代次复用已验证；注销与授权变动竞态仍需扩大测试 |
| direct/routed 混合写 | 独占目标 RX 写入；竞争返回 AGAIN；独立生命周期 gate 不随 clear_entry 重置；router 在锁内检查 RX 几何与发布 | 4 发送线程、4000 帧逐来源顺序与内容已验证 |
| Ring 接收 | body 完整复制后一次提交 tail；recv/poll/frame_len/drain 共用读锁 | Rust/Ring 测试通过 |
| 等待 | wait 的硬错误立即返回；布局 16 改用通知代次及 Linux 单调 futex 等待 | 正在验证通知合并/多等待者及单因素性能 |
| Go | RWMutex 保护调用与 Close，KeepAlive 防止终结器提前释放 | 编译/绑定验收随默认矩阵记录 |
| Cargo/Python 打包 | 补入 keep.c/tlv.c；wheel 构建每次调用 Cargo，固定 target-dir | 离线 Cargo 包重建通过 |
| 原生产物选择 | 使用本次 Cargo build-script-executed 的 OUT_DIR，记录指纹；修复 --build-datarouter | 新 helper 已成功构建 |
| 运行版本/临时文件 | Git HEAD 注入 C 版本；临时可执行文件创建时即限制权限 | Rust ABI/启动测试通过 |
| 进程监控 | 仅保留存活 PID+starttime，按身份核对进程组，避免历史 PID 集合膨胀 | CLI 清理记录 remaining=0 |

安全数据保存在本机 `build/security-*`，产物不入库。`security-first`/`second` 的
sanitizer 链接失败也保留。运行库通过 dnf 下载并解包到
`~/.cache/gcc-sanitizers`，未修改系统运行库。`security-expanded` 对旧源码复现
11 个失败子检查，候选对应子检查通过。`security-final` 还通过 100 个 Rust harness
测试及两项独立多进程场景；后续最终结果与原始数据索引见 benchmark 对照文档。

## Benchmark 修改范围

八语言 JSON body 使用 `Public.Products.List`，以空白补齐约定字节数；不足时失败。
Python JSON 路径不再把 JSON 放入 context 而把无效填充作为 body。
C 路径现在实际使用 JSON 参数并报告 payload_mode；结果文件写入错误失败。
UDS 保存未完成收发帧，禁止在部分发送后换帧；零字节发送、断连与无法完成的帧失败。
C# Dispose 通过原子交换防止重复释放。统计验证允许真实零值。
这些修改改变了部分场景的契约，因此历史成绩只作历史记录。

## 额外审查发现

- Windows `platform_open` 的最大映射大小未包括 KEEP；已把上界改为最多端口加 8 MiB KEEP。
  通过 vswhere 找到 E 盘 MSVC 14.51；原生 C 编译和链接已通过，未执行 Windows 运行测试。
- `ring.c` 的 Windows 初始化改用 InitOnceExecuteOnce；开启 MSVC C11 原子选项，替换 Windows 路径中的 GCC 位扫描调用。
- JNI 的 jint 参数、message_type 和 flags 改为检查范围后转换；正常接口编译/生命周期测试随诊断配置复验。
- release.yml 原先只替换同名 wheel；现在确认本次 wheel 上传后删除被替代 wheel，并串行更新 latest。仅修改流程，未触发发布。
- 测量器原先每 50ms 计算随后丢弃的线程快照；普通轮已改为只计算首份，诊断保留旧模式。
  正式二进制的监控 A/B/B/A 均值变化 -0.17%，不足以解释原回归。

## 尚未完成（继续阻止合入）

- 完整扫描所有协议/绑定/CI/Windows 实现；Core、Portal、Cgeai 的隔离实际源码编译已通过。
- 普通 runner、PHP 专项与统一验证器彻底共用进程/构建/统计代码；完整源文件冻结契约。
- 八语言固定序号元数据、空 body ACK 回传序号，以及错误/重复/错序/多余 ACK 的故障验收。
- 所有语言参数截断、硬错误计数、分配失败、全部命中退出、输出失败等入口逐项收敛。
- direct 持锁进程死亡时已增加目标代次退休；更多进程并发发送与注销/复用/暂停故障仍需验收。
- Rust 映射引用、RAII guard 与重连后的旧引用释放已验证；更多异常断连/关闭交错场景仍需扩大。
- KEEP 多读者协议及性能验收；当前独占读、完整槽位验证是保守实现，不能宣称无回归。
- 布局 17 Ring 游标缓存行分离已通过安全检查；其单因素重复性失败，等待/通知及 Ring 仍需最终全矩阵验收。
- 持续覆盖引导协议模糊测试、所有 JNI/Go/Python/C# 生命周期故障用例、Windows 运行。
- C64 等待/Ring 各单因素 A/B/B/A；完整内部追踪及两轮重复性门禁。
  正式二进制 A/A 和监控 A/B/B/A 已完成，未解除原性能门禁。
- 在完全修复后的测量契约下完成每版本 360 场景+110 ACK RTT；现有矩阵仍无序号核验。
- PHP 8.2 NTS/JIT 关闭完整专项、latest 资产同步、消费者指针更新。

历史 `+44.34%` p99 回归保留，不用本次不同契约的成绩覆盖或解释它。
运行测试过程中不提交/更新源码指针；性能门禁未解除，不更新装配仓库的 DataRouter 指针。

## 扩展安全配置最终记录

`build/security-verified/results.json` 全部候选检查通过（27 项）：包含 switch/config 的真实
GCC -fanalyzer -c 静态检查、四类 ASan/UBSan 二进制与 13 个边界/故障子例、Rust 测试、
Core/Portal/Cgeai 实际源码隔离编译、Cargo 离线打包重建、MSVC 原生 C 编译和链接。
确认清理结果 c_cleanup.remaining=0、cleanup.remaining=0。

`security-complete`、`security-portable`、`security-windows-linked` 保留每次失败：分别发现
隔离消费者依赖路径、MSVC 原子支持开关、GCC 专有调用及静态检查格式截断诊断。
最后一项原有服务名校验已限制长度，改为经校验后的定长复制，未放宽名称约束。
详见 [源代码跑分对照](benchmark-security-2026-09.md)：正式性能门禁仍失败。

## 布局 15 生命周期增量

`build/security-reconnect-verified/results.json` 通过全部 28 项候选检查；Rust harness
现有 102 个测试另加两项多进程场景。新增测试覆盖注销关闭位阻止迟到引用、Rust guard
跨线程/跨 frontend 生命周期、旧映射与重连后新映射并存时的独立释放。
`io_users` 现为 route_id + 关闭位 + 引用数的单一原子值；路由代次耗尽拒绝复用，避免回绕 ABA。
C 检查、三个消费者编译、Cargo 包重建与 MSVC C 编译链接再次通过；两类 cleanup.remaining 均为 0。

`results/security-c64-diagnostic-20260909`（benchmark 仓库）保留 48/56 场景及 8 个首快照前
的 affinity 失败；已增加启动掩码就绪握手，后续复验不覆盖原失败记录。该轮 C64 A/A 为
20.549/20.310 µs；monitor repeated/once/once/repeated 为 20.220/20.850/20.889/20.979 µs。
这些数据未解释正式轮 29–32 µs 的长尾；且该诊断 C 可执行文件增加了可关闭追踪，不能当作
原始正式二进制的 A/A。原始布局 14 二进制及与源码 SHA-256 匹配的 C benchmark 源码已保存在
`build/diagnostic-layout14`，用于后续完全相同二进制的对照。

## Windows Rust 编译增量

`build/windows-rust-portable/results.json` 的 MSVC 原生 C 编译、switch 链接及 Windows
`cargo check --lib --offline --locked` 均通过。WSL UNC 文件系统不支持 Rust 增量锁，
因此检查显式设置 `CARGO_INCREMENTAL=0`。随后发现并修复 `Materialized.memfd` 引用
Unix 专用 `std::os::fd` 的编译错误，改用等价的 `libc::c_int`。
失败日志保存在 `windows-rust-logged.log`，成功日志在 `windows-rust-portable.log`。
未运行 Windows 服务或运行测试；编译通过不代表 Windows 运行门禁通过。

## UDS 故障复验

`datarouter-benchmark/results/security-uds-verified-20260909` 通过 56 个诊断场景及
22 次额外 ACK RTT；两轮 UDS business 均通过。保留先前 `security-c64-affinity-verified-20260909`
的两个 UDS business 失败。修复部分发送后的请求替换、全命中循环截止时间和末尾 FIN 握手。
本轮源码冻结检查通过；回收 236 个进程组、83 个所属共享内存，remaining=0。
这仍是加入统一序号 ACK 前的诊断契约，不是最终默认矩阵门禁。

## 布局 16 等待路径

`build/security-signal/results.json` 的 30 项候选检查全部通过，含 Windows C/Rust 编译、
三个消费者、Cargo 包重建和新增通知多进程故障测试。C/runtime 清理均为 remaining=0。
Linux 使用共享 futex 相对单调截止时间；调用者在读取队列/邮箱前保存通知代次，
发布后唤醒所有已登记等待者。通知不累计为帧计数，百万次 post 仅产生一次旧通知消费。
暂停或被杀等待者不会持有互斥锁；死亡留下的等待计数保守增加 wake 系统调用，禁止回绕。
Windows 保留轮询后端，使用相同代次避免单个消费者吞掉其他等待者的通知。

`security-wait-20260909` 的 C64 A/B/B/A 为 20.270/20.959/20.499/20.500 µs，
B/A 均值 +1.69%，两组重复性通过；清理 12 个进程组、4 个所属共享内存，remaining=0。
这项单因素结果不解除原默认矩阵的失败门禁。

## 布局 17 Ring 缓存行

头尾游标相距 64 字节，Ring 头为 128 字节；数据区偏移及通道跨度按 64 字节对齐。
C/Rust Ring 头大小常量同步，Frontend/PortFrontend 正常接口不变。
`build/security-ring/results.json` 全部 30 项候选检查通过，包含几何损坏拒绝、环回绕、
混合并发发送、三个真实消费者、Windows C/Rust 和 Cargo 包重建；清理 remaining=0。

`security-ring-20260909` 的 C64 A/B/B/A 为 22.680/20.610/21.940/20.580 µs。
组内变化 A=-9.26%、B=+6.45%，重复性失败；B/A 均值 -1.64% 不能作为改善结论。
A、B 源码及产物分别保存为 `build/diagnostic-ring-a`、`build/diagnostic-ring-b`，
前者也保存等待因素实验中的 B。四个场景正确性通过，清理 remaining=0。

## 时钟、令牌与 Python 源码包

Windows 原 QPC 换算先乘 10^9，长时间运行可溢出；现在先取整数商余数，极端频率用
无溢出整数乘除。100000 个随机输入与 128 位参考计算一致，Windows C/Rust 编译通过；
没有执行 Windows 运行测试。

SwitchHandle 改以 stdin 管道传令牌；命令参数和配置 Debug 输出不包含令牌。
C 的 `--token-stdin` 验证 1–128 字节及内嵌 NUL，旧 `--token` 保留；验证器记录旧参数时脱敏。
Rust 鉴权测试同时核对实际 `/proc/.../cmdline`，现有接口编译及正反鉴权检查通过。

`security-clock-token-package` 的 32 项 C/Rust/消费者/Windows/Cargo 检查通过；
唯一失败来自新 wheel 检查器只支持根目录布局。实际库在合法 `.data/purelib` 路径，
修正检查器后 `build/python-package-retest/results.json` 通过：从包含 Cargo 锁文件、C/Rust
源码、Git 身份与许可证的 sdist 单独重建 wheel，核对平台标签、原生 ABI、版本及许可证。
两次都保留原始记录；前次清理 remaining=0。构建工具缓存于 `~/.cache/datarouter-packaging`，
使用 build 1.3.0、setuptools 80.9.0、wheel 0.45.1。

## 构建记录最终增量

`build/security-build-recorded/results.json` 的 33 项候选检查全部通过；C/runtime 清理均为
remaining=0。原生构建记录增加实际 C 编译、归档、链接命令及 build.rs/Cargo 文件指纹，
共享库链接使用选定的 CC。旧基线没有编译命令记录文件时明确保留 null，而非伪造命令记录。
根 Cargo.lock 已纳入版本控制，避免干净 checkout 的 `--locked` 构建失败。
Python 完整 sdist→wheel 构建亦纳入 security 配置；可用 `DATAROUTER_PACKAGE_PYTHON`
指定安装了 build/setuptools/wheel 的解释器。

latest 流程串行移动同一个标签并覆盖上传当前 wheel；确认新资产上传后清理旧 wheel，
不删除历史 benchmark 报告。引用 API 行为依据 GitHub 的
[Git references](https://docs.github.com/en/rest/git/refs) 与
[release assets](https://docs.github.com/en/rest/releases/assets) 文档。未执行远程发布。

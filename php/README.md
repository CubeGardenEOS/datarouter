# PHP 8.2 NTS frontend

Linux x86_64，PHP 8.2 NTS CLI/FPM。原生扩展封装 `frontend_open_v3`，默认
DIRECT，显式 `DATAROUTER_ROUTED` 使用既有分块/ACK/重组。扩展不修改 C/Rust
校验、其他语言绑定或共享内存布局。协议编码、解码及帧上限来自既有 C ABI。

## 构建与 API

配置 `PHP`、`PHP_FPM`、`PHPIZE`、`PHP_CONFIG` 为同版工具的绝对路径，并将
对应 autoconf/m4 加入 PATH。缺失工具、版本/ABI/头文件不符均失败。
下载工具放 `~/.cache/<工具名>`，构建产物位于忽略的 `build/php/`。

统一执行：

```sh
cgeos-cli datarouter php verify --profile smoke --datarouter /path/to/datarouter \
  --benchmark /path/to/datarouter-benchmark
cgeos-cli datarouter php verify --profile full --datarouter /path/to/datarouter \
  --benchmark /path/to/datarouter-benchmark
```

驱动先重建本 checkout 的 native 库与 switch，再按
[PHP 官方 phpize 流程](https://www.php.net/manual/en/install.pecl.phpize.php)
执行 phpize、configure（显式 php-config）、make，不安装到系统 PHP。
独立构建 PHP 可运行 `python3 php/build.py`，要求 `build/` 已有本次 native 产物；
benchmark 的 `build.sh --languages=php` 调用同一入口，不改变默认语言集合。

```php
$p = datarouter_open('/example', 'portal'); // optional: plane, timeout_ms, token
try {
    $frame = datarouter_pack('{"action":"Public.Products.List"}');
    datarouter_send($p, 'core', $frame, 1000);
    $reply = datarouter_recv($p, 1000); // ['data' => PHP-owned bytes, 'source' => name]
    $message = datarouter_unpack($reply['data']); // type, flags, body, var_data
} finally {
    datarouter_close($p);
}
```

`datarouter_pack(body, type=7, var_data='')` 接受二进制 body 和原始 TLV 块；
TLV/wire 校验由现有 ABI 执行。`datarouter_max_payload()` 返回 native 帧上限，
包含 wire header，不是业务 body 上限。`DataRouterException::getCode()` 保留
原生错误码（1 参数/关闭或跨 fork、2 对端缺失、4 背压、5 超限、7 超时、
8 权限、10 协议错误）。PHP 类型错误使用 PHP 自身异常。

资源是非持久化、请求级连接；FPM fork 后在请求代码中 open，请求结束由资源
析构释放，也可显式 close。重复 close 幂等；关闭后访问报错。禁止把连接跨
fork 传递；PID 检查拒绝继承的资源，子进程析构不会访问继承的 native 线程锁。
接收结果复制到 PHP 自有字符串，关闭后仍有效。

## 专项验收

| 层 | smoke | full 增量 |
|---|---|---|
| 构建/加载 | 源码重建、CLI/FPM 同扩展、Git/库/扩展/源码 SHA-256、PHP ABI | 同左 |
| 通信 | PHP 双向、二进制/零字节、source、direct 临界/超限、routed 大消息重组 | 同左 |
| 错误 | 超时、缺失对端、鉴权拒绝、背压、重复关闭及关闭后访问、重连 | 对端 SIGKILL、switch 重建后显式重连；成功/失败/超时/SIGINT 进程退出 |
| 示例 | CLI echo 对端、真实 FastCGI 五次请求 | direct 单 worker 201 次、routed 四 worker 一万次、每 worker 100 次回收 |
| 底层回归 | — | 原样 cargo test（ABI/direct/多进程/KEEP）及四个 C 核心测试 |
| benchmark | 三个 PHP/C 组合各 100 次 raw 往返 | 三组合、raw/JSON、完整尺寸，两轮；C 基线/候选两轮 5% 门禁 |

`examples/cli.php` 是 CLI 请求/响应示例；`examples/fpm.php` 是 FPM 请求入口。
二者只使用 PascalCase Action。原生 direct 数据面是 SPSC：同一目标 ring 只允许
一个跨进程写者（见 `src/port.c` 的 `datarouter_unsafe_send` 约束）。四 worker
共用一个业务对端必须显式选择 routed（示例 `DR_PLANE=1`），不能并发向同一
目标做 direct send。首次 direct 四 worker 失败记录必须保留，不能把此限制
隐瞒为通用多写者支持。驱动检查进程退出码、结果存在、精确用例数、
检查数和通信计数。预期失败匹配具体错误码，不能把缺文件或少用例当成成功。

驱动统一创建进程组，处理 SIGINT/SIGTERM，超时或失败也终止子孙进程并 wait；
仅移除本次独占名称或本次子进程映射的共享内存、独占 socket。不能用全局
`pkill` 或通配删除 `/dev/shm`。结果写专用 benchmark 仓库 `results/php-*/`；
FPM 请求耗时独立记录，不能混入纯 IPC 延迟。性能方法与报告见该仓库 PHP 文档。

下游仅新增可选 PHP 接口，现有绑定/ABI 不变；PHP 接入尚不意味着数据库适配或
WordPress/WooCommerce 业务兼容。本次 latest 发布需在所有门禁通过后完成，
不得用 PHP 首次基线与 C 的绝对速度差判断 native 回归。

定向复测可加 `--only faults` 或 `--only benchmarks`。结果明确标注 coverage，
只有未使用 `--only` 的 full 全部通过才会输出 `full_gate=true`，定向结果不能
替代总验收。退出清理期间屏蔽重复 SIGINT/SIGTERM，以完成 kill/wait/unlink。

CI：独立 `.github/workflows/php.yml` 使用 `php82-benchmark` 专用自托管主机；
需配置 `CGEOS_ASSEMBLY` 与 `DATAROUTER_BENCHMARK_DIR` 仓库变量以及同版 PHP
工具路径。先从当前装配 checkout 构建 CLI，再执行本 checkout 的 PHP 验证。
默认 smoke，手动 full 在同一主机完成基线/候选，不使用异机历史成绩作门禁。

## 2026-09-08 验收状态

PHP 正确性、99 项 Rust 测试及多进程/C 核心回归、单 worker 201 请求、routed
四 worker 10000 请求和故障恢复已通过。固定 CPU 的两轮 native 对照中，64 B
pingpong p99 恶化 31.56%，超过 5% 门限，**full_gate=false，禁止合入/发布**。
即使 native 源码及库指纹未变，也保留门禁失败，不用推测的调度噪声替代通过
证据。详见专用 benchmark 仓库 `docs/benchmark-php82-cpu-refresh-2026-09-08.md`。

全语言复测还发现编译服务器被 kill 后到成为可 wait 子进程之间有短暂窗口。
清理现在最多等待 5 秒并持续回收子孙进程，增加“父进程已退出、子进程仍在”的
验证；定向故障复测含 5 项生命周期检查。该修复不修改 native 数据面或性能阈值。

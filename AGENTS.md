# AGENTS.md — CGEOS 开发工作区（开发约束）

本文件为 CGEOS 家族各子仓库统一的开发约束，供本仓库内 AI 代理与人类开发者参考。
本仓库是拆分式多仓库工作区（本地检出为 `~/CGEOS`，每个目录都是独立 git 仓库）的一员；本仓库在
CGEOS 平台中的角色见下方「仓库拓扑」。

## 许可

所有子仓库（及其 submodule）受 `LICENSE`（CubeGarden 通则软件许可（CGSGL）
Version 2）约束。新增代码与文档默认沿用该许可，不得引入与之冲突的第三方许可组件。

## 仓库拓扑

`~/CGEOS`（对 CGEOS 平台的拆分）：每个目录都是独立 git 仓库。

- **core（Rust，`CGEOS`）**：根仓库，进程编排/鉴权/资源释放/模式控制，单二进制
  `bizpilot-core`；内含内置 init 模块（caddy/frpc/rustfs 下载与校验）。
- **服务（Rust）**：`CGEOS-portal`（边缘节点/网关）、`CGEOS-cms`（业务/建站）、
  `CGEOS-cgeai`、`CGEOS-office`、`CGEOS-stronghold`。
- **服务（Python）**：`CGEOS-storage`、`CGEOS-content`、`CGEOS-ml`、
  `CGEOS-shared-python`（共享层）。
- **业务服务（Java/Spring Boot）**：`CGEOS-erp`（ERP 核心：单据引擎/基础资料/库存/资金，
  Spring Boot 4.1.0 LTS 多模块 Maven 工程，PostgreSQL via CGEOS database，datarouter JNI 接入）。
- **审计服务（C#/.NET 8）**：`CGEOS-audit`（独立非实时审计与版本管理，HDF5 存储，BLAKE3 哈希链）。
- **轻量服务/库**：`CGEOS-adaptor`、`CGEOS-insight`、`CGEOS-media`、`CGEOS-outpost`。
- **对外发布**：`CGEOS-client-service`（`@blinkcms/client-service`，含
  CGEOS-blinkimage 子模块）、`CGEOS-protect`（`@blinkcms/protect-wasm`）、
  `CGEOS-access-service`（`@blinkcms/access-service`，访客访问 SDK + Rust 核心）、
  `CGEOS-database-client`（Java/Rust 客户端库，支持 JDBC / R2DBC / sqlx driver）。
- **工具**：`CGEOS-cli`（`cgeos-cli`，AI 智能体 CLI）。
- **客户端/模板**：`BizPilot`（桌面/Android 管理端 hyper）、`BlinkCMS`（mdui
  站点/展示站模板；经典微信小程序已废弃并删除）。

core 以 submodule 引用各服务与共享库：`services/*`（上述服务仓库）、
`lib/datarouter`（共享传输层）、`lib/client-service`、`lib/protect`。

## 本地开发工具约定

- 所有下载型开发工具缓存到 `~/.cache/<工具名>` 固定路径，不要放到 `/tmp`，
  避免后续会话重复下载。
- npm 客户端/前端仓库统一使用 **bun**（`bun install` / `bun run <script>`）。
- 外部二进制（caddy/frpc/rustfs）下载逻辑**只写在 core 的 `src/init.rs`**；
  其他仓库不得自带重复下载器。

## 跨仓库依赖与发布

- `@blinkcms/client-service`、`@blinkcms/protect-wasm`、`@blinkcms/access-service`
  由对应仓库构建 wasm/rust 产物并打标签发布（git+ssh 拉取，标签 = 版本）；消费方
  以 `git+ssh://...#<tag>` 引用。
- 修改 wasm/原生产物后：先在源仓库构建并提交产物、打标签，再更新消费侧依赖引用。
- 站点模板 dist 由 BlinkCMS 以 GitHub Release artifact（site-template-*.zip）
  发布，CGEOS 打包消费；CGEOS 不引用 BlinkCMS/BizPilot 源码。
- 修改共享层（`shared_python`、依赖库、SDK）需评估下游（hyper / mdui / 各服务）
  并同步发布。

## DataRouter（共享传输层）

- submodule `lib/datarouter`：本地共享内存传输层（datarouter-switch 交换进程 +
  C Port API + Rust crate），payload 不透明字节，不做业务解析。
- 角色与测试方法见 `lib/datarouter/README.md`；更新 submodule 后必须重建产物并
  跑测试。

## 构建与运行

- 各仓库的构建方式见各自 README；core 侧开发构建 `cargo build`，运行
  `./target/debug/bizpilot-core --mode debug-run`。
- core 打包流程：`uv run python build/main.py bundle --release`（打包单二进制，
  嵌入 payload）；初始化 submodule 用 `git submodule update --init --recursive`。

## 变更纪律（开发约束）

- submodule（`services/*`、`lib/datarouter`、`lib/client-service`、`lib/protect`）
  的修改在对应子仓库内独立提交，再更新父仓库指针；不在父仓库内直接改 submodule
  内容而不提交。兄弟仓库（`repos/` 下平级检出）同理——各自独立提交。
- 产物不入库：构建产物（dist / apk / payload / resources 等）不提交。
- 文档变更与代码变更一起**原子**提交：一次变更一个 commit，勿混入无关改动。
- 文档保持与拓扑一致；过时文档及时删除。
- **提交人责任归属**：日常开发与代码提交必须使用开发人员本地 SSH 密钥，严禁使用 Bot 账号代理提交（CI 必要场景除外），以确保变更具备明确的人员责任归属与审计追责依据。

## 硬性工作约束（2026-08 起）

以下约束对 CGEOS 家族所有仓库生效，涉及代码/CI/发布变更前必须遵守：

1. **发布永远 latest（无版本区分）**：任何仓库的产物发布（npm/GitHub Release/artifact）都
   是**覆盖式 publish**——移动 latest 标签/指针、删除前次 artifact，manifest 只保留最新一条。
   版本号仅用于源码内元数据标识，不用于产物共存；发布流水线不得保留多版本或多历史版本。
2. **datarouter 变更后必跑基准（跑分）**：凡改动 datarouter（含 shared-memory 布局、数据面、
   FFI/协议、校验逻辑）必须先跑分并出对照（benches/* 与 docs/benchmark-*.md，同机两轮记录），
   无回归（±5%）方可合入。**datarouter 的校验功能一律下发到 C 部分**（`src/`），不得在
   Rust 前端重复实现第二份规则；Rust 只做委托 FFI 或调用唯一事实来源。
3. **桌面应用禁 CI 构建**：在本地构建流程（Windows 宿主已装 MSVC）完善之前，CI 不得构建
   桌面应用（BizPilot 等 hyper/客户端产物）；在线 pipeline 不得新增桌面 job，已有桌面 job 保持摘除。
4. **database 仓库（C++）与 KEEP 区域**：数据库逻辑集中于独立仓库 `CubeGardenEOS/database`
   （C++20 + CMake），core 不再内置数据库实现。缓存分层固定为：L1=datarouter **KEEP 区域**
   （由 Switch 维持，≤8MiB，仅 core 及 core 授权应用可写、其余只读，命中时由 port/前端直接
   返回、不经 Ring Buffer），L2=进程内 LRU，L3=Redis；主数据库仅接受 PostgreSQL。
   PG 访问层用 **libpqxx**（内部即 libpq）替掉裸 libpq 样板（连接生命周期/结果/事务/prepared/
   COPY/NOTIFY）；连接池、租户路由、prepared LRU、单飞与多层缓存协调仍自研（database 仓库）。
   datarouter 的 KEEP 区域实现位于 org 仓库 `CubeGardenEOS/datarouter`；协作检出为其
   submodule 位置（`lib/datarouter`）或平级仓库 `datarouter/`，**不得在 `lib/` 内直接编辑**。
5. **禁止使用 Bot 账号提交（必须使用本地 SSH 密钥）**：除非 CI/CD 自动化流水线必须（如自动化构建发布），
   否则所有日常开发、功能变更与代码维护**必须使用开发人员本地 SSH 密钥**进行 Git 提交与推送。严禁滥用 Bot
   账号代理日常提交，以防止提交记录责任不清、难以追责。
6. **Action 严格大驼峰命名（PascalCase）**：系统内所有协议消息、WebSocket `/terminal` 入口、
   DataRouter IPC 路由、服务注册与对外 SDK 中的 Action 标识，每一层级命名空间与动作谓词
   均使用 PascalCase（大驼峰）并以英文点号 `.` 分隔（例如 `Core.Auth.Login`、`Cms.Tenant.Products.List`、
   `Public.Products.Detail`、`Cgeai.Admin.Channels.List`、`Ml.Agent.Run`、`Erp.Document.Submit`、
   `Dbc.Query`、`Storage.Frontend.Package.List`）。严禁使用小写下划线（snake_case）或小驼峰（camelCase）
   作为 Action 字符串；新增或修改 Action 时文档与代码必须同步更新。

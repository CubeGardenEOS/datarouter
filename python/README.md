# DataRouter Python 绑定

通过 ctypes 使用随 wheel 分发的 DataRouter 原生库。当前 wheel 构建面向 Linux，
标记为平台相关的 `py3-none-linux_*`，不能在其他操作系统直接复用。

在仓库 `python/` 目录运行 `python -m build`，生成 sdist 后再从 sdist 构建 wheel。
需要 Rust/C 编译器；sdist 内含 Cargo 原始包、锁文件、C/Rust 源码和 Git 提交身份，
不依赖相邻 checkout 或已构建静态库。每次 wheel 构建都调用 Cargo 核对输入。

wheel 内的 `blinkcms_datarouter/_native/libblinkcms_datarouter.so` 通过
`datarouter_version()` 返回构建来源 Git 提交。共享内存布局升级时，switch 与绑定须一起替换。

遵循随包提供的 CubeGarden 通则软件许可（CGSGL）Version 2，见 `LICENSE`。

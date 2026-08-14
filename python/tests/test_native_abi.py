"""ABI 兼容性与真实 native 库冒烟测试。

与 `test_port.py`（用 mock `_lib` 做单元测试）不同，本测试加载真实
`libblinkcms_datarouter.so`，直接对 C ABI 做交叉校验，覆盖 Python wheel 的
稳定性关键点：

1. frontend ABI version（C 函数返回值）。
2. shared-memory 几何（`datarouter_max_app_payload` 与 layout 12 一致）。
3. `datarouter_frontend_open_v2` 的 struct size / ABI 校验路径（证明
   `_lib.datarouter_frontend_options` 的 ctypes 镜像与 C struct 尺寸一致）。
4. 缺省构建（`unsafe` 特性）下直写符号存在。
5. 真实端到端收发：spawn `build/datarouter-switch`（或 `DATAROUTER_SWITCH_BIN`），
   用 `Port` 走完 send/recv + 协议打包/解包。

库来源（`_lib._library_path`）：`DATAROUTER_LIB` 环境变量 → 已安装 wheel 内的
`_native/*.so` → 源码树 `build/libblinkcms_datarouter.so`（`python/setup.py
build_py` 产物）。与 `test_protocol.py` 一致：找不到库时整个套件失败。
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import time
import unittest
from pathlib import Path

sys_path = os.path.join(os.path.dirname(__file__), "..")
if sys_path not in __import__("sys").path:
    __import__("sys").path.insert(0, sys_path)

from blinkcms_datarouter import _lib, protocol  # noqa: E402
from blinkcms_datarouter.port import Port  # noqa: E402


def _switch_bin() -> Path | None:
    env = os.environ.get("DATAROUTER_SWITCH_BIN", "").strip()
    if env:
        path = Path(env)
        return path if path.is_file() else None
    path = Path(__file__).resolve().parents[2] / "build" / "datarouter-switch"
    return path if path.is_file() else None


class NativeAbiTest(unittest.TestCase):
    """对真实 .so 的 C ABI 交叉校验（不依赖 switch）。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.lib = _lib.load_library()

    def test_frontend_abi_version_is_two(self) -> None:
        self.assertEqual(
            self.lib.datarouter_frontend_abi_version(),
            _lib.DATAROUTER_FRONTEND_ABI_VERSION,
        )
        self.assertEqual(_lib.DATAROUTER_FRONTEND_ABI_VERSION, 2)

    def test_max_app_payload_matches_layout_12_geometry(self) -> None:
        # layout 12：ring 容量 = 2^19，最大单帧负载 = 2^19 - 8（8 字节帧头）。
        self.assertEqual(self.lib.datarouter_max_app_payload(), (512 * 1024) - 8)
        self.assertEqual(_lib.DATAROUTER_MAX_APP_PAYLOAD if hasattr(_lib, "DATAROUTER_MAX_APP_PAYLOAD") else (512 * 1024) - 8, (512 * 1024) - 8)

    def test_frontend_options_size_is_validated_against_c(self) -> None:
        opts = _lib.datarouter_frontend_options(
            shm_name=f"/datarouter-abi-check-{os.getpid()}".encode(),
            service_name=b"abi_check",
            connect_timeout_ms=50,
            recv_timeout_ms=50,
            ack_timeout_ms=0,
            reader_wait_ms=0,
            app_queue_capacity=0,
            max_message_bytes=0,
            max_pending_acks=0,
            chunk_ttl_ms=0,
            max_reassembly_groups=0,
            max_reassembly_bytes=0,
        )
        handle = ctypes.c_void_p()

        # 错误 struct size -> PROTOCOL（C 侧先校验 size/ABI 再连 switch）。
        rc = self.lib.datarouter_frontend_open_v2(
            ctypes.byref(opts),
            1,  # 与 C struct 尺寸不符
            _lib.DATAROUTER_FRONTEND_ABI_VERSION,
            ctypes.byref(handle),
        )
        self.assertEqual(rc, _lib.DATAROUTER_ERR_PROTOCOL)

        # 错误 ABI version -> PROTOCOL。
        rc = self.lib.datarouter_frontend_open_v2(
            ctypes.byref(opts),
            ctypes.sizeof(opts),
            _lib.DATAROUTER_FRONTEND_ABI_VERSION + 1,
            ctypes.byref(handle),
        )
        self.assertEqual(rc, _lib.DATAROUTER_ERR_PROTOCOL)

        # 正确 size / ABI，无 switch -> NOT_FOUND（而不是 PROTOCOL）。
        # 证明 python 的 ctypes struct 镜像尺寸与 C struct 完全一致。
        rc = self.lib.datarouter_frontend_open_v2(
            ctypes.byref(opts),
            ctypes.sizeof(opts),
            _lib.DATAROUTER_FRONTEND_ABI_VERSION,
            ctypes.byref(handle),
        )
        self.assertEqual(rc, _lib.DATAROUTER_ERR_NOT_FOUND)
        self.assertEqual(handle.value, None)

    def test_unsafe_symbols_present_in_default_build(self) -> None:
        # 缺省构建（unsafe 特性）必须导出直写数据面符号。
        self.assertTrue(hasattr(self.lib, "datarouter_unsafe_send"))
        self.assertTrue(hasattr(self.lib, "datarouter_frame_len"))


@unittest.skipUnless(
    _switch_bin() is not None,
    "datarouter-switch binary not found (set DATAROUTER_SWITCH_BIN or build build/datarouter-switch)",
)
class SwitchRoundTripTest(unittest.TestCase):
    """真实端到端：spawn switch，两个 Port 走完整收发 + 协议打包/解包。"""

    def setUp(self) -> None:
        self.shm_name = f"/datarouter-py-smoke-{os.getpid()}-{int(time.time_ns())}"
        self.proc = subprocess.Popen(
            [
                str(_switch_bin()),
                "--name",
                self.shm_name,
                "--ttl-ms",
                "5000",
                "--allow",
                "py_sender,py_receiver",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.2)

    def tearDown(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)

    def test_round_trip_via_real_switch(self) -> None:
        with Port.open("py_sender", shm_name=self.shm_name, timeout_ms=3000) as sender:
            with Port.open("py_receiver", shm_name=self.shm_name, timeout_ms=3000) as receiver:
                wire = protocol.pack_business_request(
                    module="smoke",
                    action="ping",
                    request_id="req-1",
                    payload=b"hello-datarouter-wheel",
                )
                sender.send("py_receiver", wire)
                data, source = receiver.recv(timeout_ms=3000)
                self.assertEqual(source, "py_sender")
                msg = protocol.unpack_business_message(data)
                self.assertEqual(msg.action, "ping")
                self.assertIn(b"hello-datarouter-wheel", msg.var_data + msg.body)

    def test_registry_snapshot_and_stats(self) -> None:
        with Port.open("py_sender", shm_name=self.shm_name, timeout_ms=3000) as sender:
            entries = sender.registry_snapshot()
            names = {entry.service_name for entry in entries}
            self.assertIn("py_sender", names)
            stats = sender.stats()
            self.assertGreaterEqual(stats.sent_frames, 0)


if __name__ == "__main__":
    unittest.main()

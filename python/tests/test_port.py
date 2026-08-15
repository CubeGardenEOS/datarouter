"""Tests for datarouter Port frontend behavior."""

from __future__ import annotations

import ctypes
import os
import sys
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from blinkcms_datarouter import _lib
from blinkcms_datarouter.port import Port


class _FakeLib:
    def __init__(self) -> None:
        self.close_count = 0
        self.recv_timeout_ms: int | None = None
        self.open_options: _lib.datarouter_frontend_options | None = None

    def datarouter_frontend_open_v3(
        self,
        _options: object,
        options_size: int,
        abi_version: int,
        _out: object,
    ) -> int:
        assert options_size == ctypes.sizeof(_lib.datarouter_frontend_options_v3)
        assert abi_version == _lib.DATAROUTER_FRONTEND_ABI_VERSION_V3
        self.open_options = _lib.datarouter_frontend_options_v3.from_buffer_copy(
            ctypes.string_at(
                _options,
                ctypes.sizeof(_lib.datarouter_frontend_options_v3),
            )
        )
        return _lib.DATAROUTER_ERR_BUSY

    def datarouter_frontend_open_v2(
        self,
        _options: object,
        options_size: int,
        abi_version: int,
        _out: object,
    ) -> int:
        assert options_size == ctypes.sizeof(_lib.datarouter_frontend_options)
        assert abi_version == _lib.DATAROUTER_FRONTEND_ABI_VERSION
        self.open_options = _lib.datarouter_frontend_options.from_buffer_copy(
            ctypes.string_at(
                _options,
                ctypes.sizeof(_lib.datarouter_frontend_options),
            )
        )
        return _lib.DATAROUTER_ERR_BUSY

    def datarouter_frontend_recv(
        self,
        _handle: object,
        _buffer: object,
        _capacity: int,
        _info: object,
        timeout_ms: int,
    ) -> int:
        self.recv_timeout_ms = timeout_ms
        return _lib.DATAROUTER_ERR_TIMEOUT

    def datarouter_frontend_close(self, _handle: object) -> None:
        self.close_count += 1


class _DynamicRecvLib(_FakeLib):
    def __init__(self, payload: bytes) -> None:
        super().__init__()
        self.payload = payload
        self.capacities: list[int] = []

    def datarouter_frontend_recv(
        self,
        _handle: object,
        buffer: object,
        capacity: int,
        info_pointer: object,
        timeout_ms: int,
    ) -> int:
        self.recv_timeout_ms = timeout_ms
        self.capacities.append(capacity)
        info = ctypes.cast(
            info_pointer,
            ctypes.POINTER(_lib.datarouter_message_info),
        ).contents
        info.len = len(self.payload)
        if capacity < len(self.payload):
            return _lib.DATAROUTER_ERR_PAYLOAD_TOO_LARGE
        ctypes.memmove(buffer, self.payload, len(self.payload))
        info.source = b"core"
        return _lib.DATAROUTER_OK


class _BlockingSendLib(_FakeLib):
    def __init__(self) -> None:
        super().__init__()
        self.send_started = threading.Event()
        self.release_send = threading.Event()

    def datarouter_frontend_send(self, *_args: object) -> int:
        self.send_started.set()
        self.release_send.wait(timeout=2)
        return _lib.DATAROUTER_OK


class _BlockingRecvLib(_FakeLib):
    def __init__(self) -> None:
        super().__init__()
        self.recv_started = threading.Event()
        self.release_recv = threading.Event()

    def datarouter_frontend_recv(self, *_args: object) -> int:
        self.recv_started.set()
        self.release_recv.wait(timeout=2)
        return _lib.DATAROUTER_ERR_TIMEOUT


class _StatsLib(_FakeLib):
    def datarouter_frontend_stats_snapshot_v2(
        self,
        _handle: object,
        stats_pointer: object,
        stats_size: int,
        abi_version: int,
    ) -> int:
        assert stats_size == ctypes.sizeof(_lib.datarouter_frontend_stats)
        assert abi_version == _lib.DATAROUTER_FRONTEND_ABI_VERSION
        stats = ctypes.cast(
            stats_pointer,
            ctypes.POINTER(_lib.datarouter_frontend_stats),
        ).contents
        stats.reader_alive = 1
        stats.reassembly_expired = 17
        stats.reassembly_rejected = 18
        stats.corrupted_rings = 19
        stats.registration_recoveries = 20
        stats.blocked_wakeups = 21
        return _lib.DATAROUTER_OK


class TestPortOpen(unittest.TestCase):
    def test_frontend_open_failure_does_not_close_null_handle(self) -> None:
        fake = _FakeLib()
        with patch("blinkcms_datarouter.port.load_library", return_value=fake):
            with self.assertRaisesRegex(_lib.DatarouterError, "BUSY"):
                Port.open("ml_manager")

        self.assertEqual(fake.close_count, 0)
        self.assertIsNotNone(fake.open_options)
        assert fake.open_options is not None
        self.assertEqual(fake.open_options.max_reassembly_groups, 0)
        self.assertEqual(fake.open_options.max_reassembly_bytes, 0)

    def test_recv_uses_explicit_timeout(self) -> None:
        fake = _FakeLib()
        port = Port(ctypes.c_void_p(1234), timeout_ms=3000)

        with patch("blinkcms_datarouter.port.load_library", return_value=fake):
            with self.assertRaises(_lib.DatarouterError):
                port.recv(timeout_ms=25)

        port._closed = True
        self.assertEqual(fake.recv_timeout_ms, 25)

    def test_recv_expands_to_reported_message_size_and_retries(self) -> None:
        payload = b"large-message" * (8 * 1024)
        fake = _DynamicRecvLib(payload)
        port = Port(ctypes.c_void_p(1234))

        with patch("blinkcms_datarouter.port.load_library", return_value=fake):
            data, source = port.recv(timeout_ms=25)
            port.close()

        self.assertEqual(data, payload)
        self.assertEqual(source, "core")
        self.assertEqual(fake.capacities, [64 * 1024, len(payload)])

    def test_close_waits_for_in_flight_send(self) -> None:
        fake = _BlockingSendLib()
        port = Port(ctypes.c_void_p(1234))
        send_errors: list[BaseException] = []
        close_started = threading.Event()

        def send() -> None:
            try:
                port.send("core", b"request")
            except BaseException as exc:  # pragma: no cover - diagnostic path
                send_errors.append(exc)

        def close() -> None:
            close_started.set()
            port.close()

        with patch("blinkcms_datarouter.port.load_library", return_value=fake):
            send_thread = threading.Thread(target=send)
            send_thread.start()
            self.assertTrue(fake.send_started.wait(timeout=1))

            close_thread = threading.Thread(target=close)
            close_thread.start()
            self.assertTrue(close_started.wait(timeout=1))
            time.sleep(0.02)
            self.assertTrue(close_thread.is_alive())
            self.assertEqual(fake.close_count, 0)

            fake.release_send.set()
            send_thread.join(timeout=1)
            close_thread.join(timeout=1)

        self.assertFalse(send_errors)
        self.assertFalse(send_thread.is_alive())
        self.assertFalse(close_thread.is_alive())
        self.assertEqual(fake.close_count, 1)

    def test_close_waits_for_in_flight_recv(self) -> None:
        fake = _BlockingRecvLib()
        port = Port(ctypes.c_void_p(1234))
        recv_errors: list[BaseException] = []

        def recv() -> None:
            try:
                port.recv(timeout_ms=100)
            except BaseException as exc:
                recv_errors.append(exc)

        with patch("blinkcms_datarouter.port.load_library", return_value=fake):
            recv_thread = threading.Thread(target=recv)
            recv_thread.start()
            self.assertTrue(fake.recv_started.wait(timeout=1))
            close_thread = threading.Thread(target=port.close)
            close_thread.start()
            time.sleep(0.02)
            self.assertTrue(close_thread.is_alive())
            self.assertEqual(fake.close_count, 0)
            fake.release_recv.set()
            recv_thread.join(timeout=1)
            close_thread.join(timeout=1)

        self.assertEqual(len(recv_errors), 1)
        self.assertIsInstance(recv_errors[0], _lib.DatarouterError)
        self.assertFalse(close_thread.is_alive())
        self.assertEqual(fake.close_count, 1)

    def test_stats_maps_hardening_counters(self) -> None:
        fake = _StatsLib()
        port = Port(ctypes.c_void_p(1234))

        with patch("blinkcms_datarouter.port.load_library", return_value=fake):
            stats = port.stats()
            port.close()

        self.assertTrue(stats.reader_alive)
        self.assertEqual(stats.reassembly_expired, 17)
        self.assertEqual(stats.reassembly_rejected, 18)
        self.assertEqual(stats.corrupted_rings, 19)
        self.assertEqual(stats.registration_recoveries, 20)
        self.assertEqual(stats.blocked_wakeups, 21)

    def test_raw_poll_wait_are_not_public_api(self) -> None:
        self.assertFalse(hasattr(Port, "poll"))
        self.assertFalse(hasattr(Port, "wait"))


if __name__ == "__main__":
    unittest.main()

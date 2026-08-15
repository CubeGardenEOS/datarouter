"""Port class wrapping the datarouter shared-memory transport."""

from __future__ import annotations

import ctypes
import threading
from dataclasses import dataclass

from . import _lib

from ._lib import (
    DATAROUTER_ERR_FULL,
    DATAROUTER_ERR_PAYLOAD_TOO_LARGE,
    DATAROUTER_ERR_TIMEOUT,
    DATAROUTER_FRONTEND_ABI_VERSION,
    DatarouterError,
    _check,
    datarouter_frontend_stats,
    datarouter_message_info,
    datarouter_registry_snapshot_entry,
    load_library,
)

_DEFAULT_SHM = b"/datarouter"
_DEFAULT_TIMEOUT_MS = 3000
_INITIAL_RECV_BUF_SIZE = 64 * 1024
_MAX_REGISTRY_ENTRIES = 64


@dataclass(frozen=True)
class RegistryEntry:
    service_name: str
    pid: int
    route_id: int
    channel_id: int
    slot: int
    last_seen_ns: int


@dataclass(frozen=True)
class FrontendStats:
    sent_frames: int
    sent_chunks: int
    received_frames: int
    received_complete: int
    received_chunk_acks: int
    received_chunk_errors: int
    ack_timeouts: int
    queue_full: int
    protocol_errors: int
    dropped_frames: int
    pending_acks: int
    queued_messages: int
    reader_alive: bool
    reader_errors: int
    keepalive_ticks: int
    last_keepalive_ns: int
    reassembly_expired: int
    reassembly_rejected: int
    corrupted_rings: int
    registration_recoveries: int
    blocked_wakeups: int
    dead_process_reaps: int
    transport_generation: int
    switch_recoveries: int


class Port:
    """A datarouter frontend handle for IPC.

    Usage::

        with Port.open("content_manager") as port:
            port.send("core", wire_bytes)
            data, source = port.recv()
    """

    def __init__(self, handle: ctypes.c_void_p, timeout_ms: int = _DEFAULT_TIMEOUT_MS) -> None:
        self._handle = handle
        self.timeout_ms = timeout_ms
        self._closed = False
        self._lock = threading.RLock()
        # Reusable receive buffer: grown on demand and retained across recv
        # calls so steady-state reception performs no per-message allocation
        # (a fresh ctypes array is zeroed on every call, which is expensive
        # for large payloads). Guarded by _lock, so one buffer per Port is
        # safe.
        self._recv_buf: ctypes.Array[ctypes.c_uint8] | None = None
        self._recv_capacity = 0

    @classmethod
    def open(
        cls,
        service_name: str,
        shm_name: str = _DEFAULT_SHM.decode(),
        timeout_ms: int = _DEFAULT_TIMEOUT_MS,
        data_plane: int = _lib.DATAROUTER_FRONTEND_DATA_PLANE_DIRECT,
        auth_token: str | None = None,
    ) -> Port:
        """Open a frontend and log on with the given service name.

        `data_plane` selects the direct (unsafe, default, 2 copies) or the
        routed (safe, switch-routed, 4 copies) plane; `auth_token` is
        required when the switch runs with `--token`.
        """
        lib = load_library()
        opts = _lib.datarouter_frontend_options_v3(
            shm_name=shm_name.encode("utf-8"),
            service_name=service_name.encode("utf-8"),
            connect_timeout_ms=timeout_ms,
            recv_timeout_ms=timeout_ms,
            ack_timeout_ms=0,
            reader_wait_ms=0,
            app_queue_capacity=0,
            max_message_bytes=0,
            max_pending_acks=0,
            chunk_ttl_ms=0,
            max_reassembly_groups=0,
            max_reassembly_bytes=0,
            auth_token=auth_token.encode("utf-8") if auth_token else None,
            data_plane=data_plane,
        )
        handle = ctypes.c_void_p()
        rc = lib.datarouter_frontend_open_v3(
            ctypes.byref(opts),
            ctypes.sizeof(opts),
            _lib.DATAROUTER_FRONTEND_ABI_VERSION_V3,
            ctypes.byref(handle),
        )
        _check(rc)
        return cls(handle, timeout_ms)

    def send(self, target: str, data: bytes) -> None:
        """Send wire-format bytes to a target service.

        Handles chunking and ACK flow control natively via
        datarouter_frontend_send.
        """
        with self._lock:
            self._require_open()
            lib = load_library()
            # Zero-copy: c_char_p references the bytes' internal buffer, so
            # no per-send ctypes copy is made (from_buffer_copy would copy
            # the whole payload on every call). `data` stays alive for the
            # duration of the call.
            buf = ctypes.cast(ctypes.c_char_p(data), ctypes.c_void_p)
            rc = lib.datarouter_frontend_send(
                self._handle,
                target.encode("utf-8"),
                buf,
                len(data),
                0,  # timeout_ms: 0 = use default (2000ms)
            )
            _check(rc)

    def recv(self, timeout_ms: int | None = None) -> tuple[bytes, str]:
        """Receive one complete message.

        Handles chunk ACK and reassembly natively via
        datarouter_frontend_recv. Returns (wire_bytes, source_service_name).
        """
        with self._lock:
            self._require_open()
            return self._recv_with_timeout(self.timeout_ms if timeout_ms is None else timeout_ms)

    def _recv_with_timeout(self, timeout_ms: int) -> tuple[bytes, str]:
        lib = load_library()
        capacity = self._recv_capacity
        while True:
            if capacity < _INITIAL_RECV_BUF_SIZE:
                capacity = _INITIAL_RECV_BUF_SIZE
            if self._recv_buf is None or capacity > self._recv_capacity:
                # Grow the cached buffer (zeroed allocation) only when a
                # larger message is seen; steady-state recv reuses it.
                self._recv_buf = (ctypes.c_uint8 * capacity)()
                self._recv_capacity = capacity
            out_buf = self._recv_buf
            info = datarouter_message_info()
            rc = lib.datarouter_frontend_recv(
                self._handle,
                ctypes.cast(out_buf, ctypes.c_void_p),
                capacity,
                ctypes.byref(info),
                timeout_ms,
            )
            if rc == DATAROUTER_ERR_PAYLOAD_TOO_LARGE:
                required = int(info.len)
                if required <= capacity:
                    _check(rc)
                capacity = required
                continue
            if rc == DATAROUTER_ERR_TIMEOUT:
                raise DatarouterError(DATAROUTER_ERR_TIMEOUT)
            _check(rc)
            length = int(info.len)
            if length > capacity:
                raise DatarouterError(
                    DATAROUTER_ERR_PAYLOAD_TOO_LARGE,
                    "received length exceeds buffer capacity",
                )
            source = info.source.decode("utf-8", "replace").rstrip("\x00")
            # C-speed copy: bytes(buf[:length]) would slice the ctypes array
            # element-by-element in Python (~24 ns/byte -> ~8 ms for a 300
            # KiB frame). string_at memcpy's at C speed.
            return ctypes.string_at(ctypes.addressof(out_buf), length), source

    def registry_snapshot(self) -> list[RegistryEntry]:
        """Return the frontend-managed datarouter registry snapshot."""
        with self._lock:
            self._require_open()
            lib = load_library()
            entries = (datarouter_registry_snapshot_entry * _MAX_REGISTRY_ENTRIES)()
            count = ctypes.c_size_t()
            rc = lib.datarouter_frontend_registry_snapshot(
                self._handle,
                entries,
                _MAX_REGISTRY_ENTRIES,
                ctypes.byref(count),
            )
            if rc != DATAROUTER_ERR_FULL:
                _check(rc)
            limit = min(count.value, _MAX_REGISTRY_ENTRIES)
            return [
                RegistryEntry(
                    service_name=entries[i].service_name.decode("utf-8", "replace").rstrip("\x00"),
                    pid=int(entries[i].pid),
                    route_id=int(entries[i].route_id),
                    channel_id=int(entries[i].channel_id),
                    slot=int(entries[i].slot),
                    last_seen_ns=int(entries[i].last_seen_ns),
                )
                for i in range(limit)
            ]

    def stats(self) -> FrontendStats:
        """Return a consistent snapshot of frontend and transport counters."""
        with self._lock:
            self._require_open()
            lib = load_library()
            raw = datarouter_frontend_stats()
            _check(
                lib.datarouter_frontend_stats_snapshot_v2(
                    self._handle,
                    ctypes.byref(raw),
                    ctypes.sizeof(raw),
                    DATAROUTER_FRONTEND_ABI_VERSION,
                )
            )
            return FrontendStats(
                sent_frames=int(raw.sent_frames),
                sent_chunks=int(raw.sent_chunks),
                received_frames=int(raw.received_frames),
                received_complete=int(raw.received_complete),
                received_chunk_acks=int(raw.received_chunk_acks),
                received_chunk_errors=int(raw.received_chunk_errors),
                ack_timeouts=int(raw.ack_timeouts),
                queue_full=int(raw.queue_full),
                protocol_errors=int(raw.protocol_errors),
                dropped_frames=int(raw.dropped_frames),
                pending_acks=int(raw.pending_acks),
                queued_messages=int(raw.queued_messages),
                reader_alive=bool(raw.reader_alive),
                reader_errors=int(raw.reader_errors),
                keepalive_ticks=int(raw.keepalive_ticks),
                last_keepalive_ns=int(raw.last_keepalive_ns),
                reassembly_expired=int(raw.reassembly_expired),
                reassembly_rejected=int(raw.reassembly_rejected),
                corrupted_rings=int(raw.corrupted_rings),
                registration_recoveries=int(raw.registration_recoveries),
                blocked_wakeups=int(raw.blocked_wakeups),
                dead_process_reaps=int(raw.dead_process_reaps),
                transport_generation=int(raw.transport_generation),
                switch_recoveries=int(raw.switch_recoveries),
            )

    def close(self) -> None:
        """Close the port and release resources."""
        with self._lock:
            if not self._closed:
                self._closed = True
                handle = self._handle
                self._handle = ctypes.c_void_p()
                lib = load_library()
                lib.datarouter_frontend_close(handle)

    def _require_open(self) -> None:
        if self._closed:
            raise ValueError("port is closed")

    def __enter__(self) -> Port:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if not self._closed:
            try:
                self.close()
            except Exception:
                pass

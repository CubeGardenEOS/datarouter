"""Low-level ctypes bindings for the datarouter C library."""

from __future__ import annotations

import ctypes
import os
from importlib import resources
from pathlib import Path
from typing import Any

DATAROUTER_LIB_ENV = "DATAROUTER_LIB"
LIBRARY_NAME = "libblinkcms_datarouter.so"

# Error codes
DATAROUTER_OK = 0
DATAROUTER_ERR_INVALID_ARGUMENT = 1
DATAROUTER_ERR_NOT_FOUND = 2
DATAROUTER_ERR_BUSY = 3
DATAROUTER_ERR_FULL = 4
DATAROUTER_ERR_PAYLOAD_TOO_LARGE = 5
DATAROUTER_ERR_AGAIN = 6
DATAROUTER_ERR_TIMEOUT = 7
DATAROUTER_ERR_PERMISSION = 8
DATAROUTER_ERR_PLATFORM = 9
DATAROUTER_ERR_PROTOCOL = 10
DATAROUTER_ERR_INTERNAL = 11
DATAROUTER_FRONTEND_ABI_VERSION = 2

_LIB: ctypes.CDLL | None = None
_RESOURCE_LIBRARY: Any | None = None
_RESOURCE_CONTEXT: Any | None = None


class datarouter_frontend_options(ctypes.Structure):
    _fields_ = [
        ("shm_name", ctypes.c_char_p),
        ("service_name", ctypes.c_char_p),
        ("connect_timeout_ms", ctypes.c_uint32),
        ("recv_timeout_ms", ctypes.c_uint32),
        ("ack_timeout_ms", ctypes.c_uint32),
        ("reader_wait_ms", ctypes.c_uint32),
        ("app_queue_capacity", ctypes.c_size_t),
        ("max_message_bytes", ctypes.c_size_t),
        ("max_pending_acks", ctypes.c_size_t),
        ("chunk_ttl_ms", ctypes.c_uint32),
        ("max_reassembly_groups", ctypes.c_size_t),
        ("max_reassembly_bytes", ctypes.c_size_t),
    ]


datarouter_options = datarouter_frontend_options


class datarouter_message_info(ctypes.Structure):
    _fields_ = [
        ("source", ctypes.c_char * 32),
        ("len", ctypes.c_size_t),
    ]


class datarouter_registry_snapshot_entry(ctypes.Structure):
    _fields_ = [
        ("service_name", ctypes.c_char * 32),
        ("pid", ctypes.c_int32),
        ("route_id", ctypes.c_uint32),
        ("channel_id", ctypes.c_uint32),
        ("slot", ctypes.c_uint32),
        ("last_seen_ns", ctypes.c_uint64),
    ]


class datarouter_frontend_stats(ctypes.Structure):
    _fields_ = [
        ("sent_frames", ctypes.c_uint64),
        ("sent_chunks", ctypes.c_uint64),
        ("received_frames", ctypes.c_uint64),
        ("received_complete", ctypes.c_uint64),
        ("received_chunk_acks", ctypes.c_uint64),
        ("received_chunk_errors", ctypes.c_uint64),
        ("ack_timeouts", ctypes.c_uint64),
        ("queue_full", ctypes.c_uint64),
        ("protocol_errors", ctypes.c_uint64),
        ("dropped_frames", ctypes.c_uint64),
        ("pending_acks", ctypes.c_uint64),
        ("queued_messages", ctypes.c_uint64),
        ("reader_alive", ctypes.c_uint64),
        ("reader_errors", ctypes.c_uint64),
        ("keepalive_ticks", ctypes.c_uint64),
        ("last_keepalive_ns", ctypes.c_uint64),
        ("reassembly_expired", ctypes.c_uint64),
        ("reassembly_rejected", ctypes.c_uint64),
        ("corrupted_rings", ctypes.c_uint64),
        ("registration_recoveries", ctypes.c_uint64),
        ("blocked_wakeups", ctypes.c_uint64),
        ("dead_process_reaps", ctypes.c_uint64),
        ("transport_generation", ctypes.c_uint64),
        ("switch_recoveries", ctypes.c_uint64),
    ]


def load_library() -> ctypes.CDLL:
    global _LIB
    if _LIB is None:
        _LIB = _configure(ctypes.CDLL(str(_library_path())))
    return _LIB


def reset_library_cache() -> None:
    global _LIB
    _LIB = None


def _configure(lib: ctypes.CDLL) -> ctypes.CDLL:
    # Frontend lifecycle
    lib.datarouter_frontend_abi_version.argtypes = []
    lib.datarouter_frontend_abi_version.restype = ctypes.c_uint32

    lib.datarouter_frontend_open_v2.argtypes = [
        ctypes.POINTER(datarouter_frontend_options),
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.datarouter_frontend_open_v2.restype = ctypes.c_int

    lib.datarouter_frontend_send.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_uint32,
    ]
    lib.datarouter_frontend_send.restype = ctypes.c_int

    lib.datarouter_frontend_recv.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(datarouter_message_info),
        ctypes.c_uint32,
    ]
    lib.datarouter_frontend_recv.restype = ctypes.c_int

    lib.datarouter_frontend_registry_snapshot.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(datarouter_registry_snapshot_entry),
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.datarouter_frontend_registry_snapshot.restype = ctypes.c_int

    lib.datarouter_frontend_stats_snapshot_v2.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(datarouter_frontend_stats),
        ctypes.c_size_t,
        ctypes.c_uint32,
    ]
    lib.datarouter_frontend_stats_snapshot_v2.restype = ctypes.c_int

    lib.datarouter_frontend_close.argtypes = [ctypes.c_void_p]
    lib.datarouter_frontend_close.restype = None

    # Message lifecycle
    lib.datarouter_msg_new.argtypes = [ctypes.c_uint8]
    lib.datarouter_msg_new.restype = ctypes.c_void_p

    lib.datarouter_msg_free.argtypes = [ctypes.c_void_p]
    lib.datarouter_msg_free.restype = None

    lib.datarouter_msg_set_body.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    lib.datarouter_msg_set_body.restype = None

    lib.datarouter_msg_set_var_data.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    lib.datarouter_msg_set_var_data.restype = None

    # TLV builders
    lib.datarouter_tlv_put_string.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.c_char_p,
    ]
    lib.datarouter_tlv_put_string.restype = None

    lib.datarouter_tlv_put_bytes.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    lib.datarouter_tlv_put_bytes.restype = None

    lib.datarouter_tlv_put_u16.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.c_uint16,
    ]
    lib.datarouter_tlv_put_u16.restype = None

    lib.datarouter_tlv_put_byte.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.c_uint8,
    ]
    lib.datarouter_tlv_put_byte.restype = None

    # TLV readers
    lib.datarouter_tlv_read_string.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    lib.datarouter_tlv_read_string.restype = ctypes.c_int

    lib.datarouter_tlv_read_bytes.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.datarouter_tlv_read_bytes.restype = ctypes.c_int

    lib.datarouter_tlv_read_u16.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint16),
    ]
    lib.datarouter_tlv_read_u16.restype = ctypes.c_int

    lib.datarouter_tlv_read_byte.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8),
    ]
    lib.datarouter_tlv_read_byte.restype = ctypes.c_int

    # Wire format
    lib.datarouter_msg_pack.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.datarouter_msg_pack.restype = ctypes.c_int

    lib.datarouter_msg_unpack.argtypes = [
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.datarouter_msg_unpack.restype = ctypes.c_int

    # Business builders
    lib.datarouter_build_business_request.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    lib.datarouter_build_business_request.restype = ctypes.c_void_p

    lib.datarouter_build_business_response.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_uint8,
    ]
    lib.datarouter_build_business_response.restype = ctypes.c_void_p

    # Field accessors
    lib.datarouter_msg_type.argtypes = [ctypes.c_void_p]
    lib.datarouter_msg_type.restype = ctypes.c_uint8

    lib.datarouter_msg_flags.argtypes = [ctypes.c_void_p]
    lib.datarouter_msg_flags.restype = ctypes.c_uint8

    lib.datarouter_msg_set_flags.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
    lib.datarouter_msg_set_flags.restype = None

    lib.datarouter_msg_var_data.argtypes = [ctypes.c_void_p]
    lib.datarouter_msg_var_data.restype = ctypes.POINTER(ctypes.c_uint8)

    lib.datarouter_msg_var_data_len.argtypes = [ctypes.c_void_p]
    lib.datarouter_msg_var_data_len.restype = ctypes.c_size_t

    lib.datarouter_msg_body.argtypes = [ctypes.c_void_p]
    lib.datarouter_msg_body.restype = ctypes.POINTER(ctypes.c_uint8)

    lib.datarouter_msg_body_len.argtypes = [ctypes.c_void_p]
    lib.datarouter_msg_body_len.restype = ctypes.c_size_t

    # Constant getters
    for name in [
        "datarouter_tag_module",
        "datarouter_tag_action",
        "datarouter_tag_streaming",
        "datarouter_tag_request_id",
        "datarouter_tag_connection_id",
        "datarouter_tag_context",
        "datarouter_tag_chunk_id",
        "datarouter_tag_chunk_index",
        "datarouter_tag_chunk_count",
        "datarouter_tag_status",
        "datarouter_tag_is_text",
        "datarouter_tag_json_data",
        "datarouter_tag_workflow_id",
        "datarouter_tag_task_uuid",
        "datarouter_tag_step_id",
        "datarouter_tag_actor_uuid",
        "datarouter_tag_trace_id",
        "datarouter_msg_business_request",
        "datarouter_msg_business_response",
        "datarouter_max_app_payload",
    ]:
        fn = getattr(lib, name)
        fn.argtypes = []
        fn.restype = ctypes.c_uint8 if "tag_" in name or "msg_" in name else ctypes.c_size_t

    return lib


def _library_path() -> Path:
    env_path = os.environ.get(DATAROUTER_LIB_ENV, "").strip()
    if env_path:
        path = Path(env_path)
        if not path.exists():
            raise FileNotFoundError(f"{DATAROUTER_LIB_ENV} does not exist: {path}")
        return path

    resource_path = _package_library_path()
    if resource_path is not None:
        return resource_path

    source_tree_path = _source_tree_library_path()
    if source_tree_path.exists():
        return source_tree_path

    raise FileNotFoundError(
        "datarouter library not found. Build lib/datarouter or set DATAROUTER_LIB."
    )


def _package_library_path() -> Path | None:
    global _RESOURCE_LIBRARY, _RESOURCE_CONTEXT
    if _RESOURCE_LIBRARY is None:
        candidate = resources.files("blinkcms_datarouter._native") / LIBRARY_NAME
        if not candidate.is_file():
            return None
        _RESOURCE_LIBRARY = candidate
    if isinstance(_RESOURCE_LIBRARY, Path):
        return _RESOURCE_LIBRARY
    _RESOURCE_CONTEXT = resources.as_file(_RESOURCE_LIBRARY)
    return _RESOURCE_CONTEXT.__enter__()


def _source_tree_library_path() -> Path:
    return Path(__file__).resolve().parents[2] / "build" / LIBRARY_NAME


_ERROR_NAMES: dict[int, str] = {
    DATAROUTER_OK: "OK",
    DATAROUTER_ERR_INVALID_ARGUMENT: "INVALID_ARGUMENT",
    DATAROUTER_ERR_NOT_FOUND: "NOT_FOUND",
    DATAROUTER_ERR_BUSY: "BUSY",
    DATAROUTER_ERR_FULL: "FULL",
    DATAROUTER_ERR_PAYLOAD_TOO_LARGE: "PAYLOAD_TOO_LARGE",
    DATAROUTER_ERR_AGAIN: "AGAIN",
    DATAROUTER_ERR_TIMEOUT: "TIMEOUT",
    DATAROUTER_ERR_PERMISSION: "PERMISSION",
    DATAROUTER_ERR_PLATFORM: "PLATFORM",
    DATAROUTER_ERR_PROTOCOL: "PROTOCOL",
    DATAROUTER_ERR_INTERNAL: "INTERNAL",
}


class DatarouterError(Exception):
    """Error from datarouter operations."""

    def __init__(self, code: int, message: str = ""):
        self.code = code
        name = _ERROR_NAMES.get(code, f"UNKNOWN({code})")
        if message:
            super().__init__(f"datarouter error {code} [{name}]: {message}")
        else:
            super().__init__(f"datarouter error {code} [{name}]")


def _check(code: int) -> None:
    if code != DATAROUTER_OK:
        raise DatarouterError(code)

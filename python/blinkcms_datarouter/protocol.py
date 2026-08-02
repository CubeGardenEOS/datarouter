"""Protocol codec for the datarouter IPC wire format."""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from typing import Optional

from ._lib import (
    DATAROUTER_OK,
    _check,
    load_library,
)

# TLV Tags
TAG_CONNECTION_ID: int = 0x04
TAG_REQUEST_ID: int = 0x05
TAG_STATUS: int = 0x06
TAG_IS_TEXT: int = 0x07
TAG_CONTEXT: int = 0x0D
TAG_JSON_DATA: int = 0x0E
TAG_MODULE: int = 0x0F
TAG_ACTION: int = 0x10
TAG_STREAMING: int = 0x11
TAG_WORKFLOW_ID: int = 0x12
TAG_TASK_UUID: int = 0x13
TAG_STEP_ID: int = 0x14
TAG_ACTOR_UUID: int = 0x15
TAG_TRACE_ID: int = 0x16

# The ring officially uses the whole 512 KiB channel. The 512 KiB ring minus
# the 8-byte ring header and 8-byte frame header gives the largest single-
# frame payload. The legacy 60 KiB cap is disabled.
MAX_APP_PAYLOAD: int = (512 * 1024) - 16

# Message types
MSG_BUSINESS_REQUEST: int = 0x07
MSG_BUSINESS_RESPONSE: int = 0x8C
MSG_LOG_NOTICE: int = 0x8D


@dataclass
class Message:
    """A decoded datarouter protocol message."""

    message_type: int
    flags: int = 0
    var_data: bytes = b""
    body: bytes = b""

    @property
    def module(self) -> str:
        return tlv_read_string(self.var_data, TAG_MODULE) or ""

    @property
    def action(self) -> str:
        return tlv_read_string(self.var_data, TAG_ACTION) or ""

    @property
    def request_id(self) -> str:
        return tlv_read_string(self.var_data, TAG_REQUEST_ID) or ""

    @property
    def connection_id(self) -> str:
        return tlv_read_string(self.var_data, TAG_CONNECTION_ID) or ""

    @property
    def streaming(self) -> bool:
        return tlv_read_byte(self.var_data, TAG_STREAMING) == 1

    @property
    def workflow_id(self) -> Optional[str]:
        return tlv_read_string(self.var_data, TAG_WORKFLOW_ID)

    @property
    def task_uuid(self) -> Optional[str]:
        return tlv_read_string(self.var_data, TAG_TASK_UUID)

    @property
    def step_id(self) -> Optional[str]:
        return tlv_read_string(self.var_data, TAG_STEP_ID)

    @property
    def actor_uuid(self) -> Optional[str]:
        return tlv_read_string(self.var_data, TAG_ACTOR_UUID)

    @property
    def trace_id(self) -> Optional[str]:
        return tlv_read_string(self.var_data, TAG_TRACE_ID)


def tlv_read_string(var_data: bytes, tag: int) -> Optional[str]:
    """Read a UTF-8 string field from var_data by tag."""
    raw = tlv_read_bytes(var_data, tag)
    if raw is None:
        return None
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return None


def tlv_read_bytes(var_data: bytes, tag: int) -> Optional[bytes]:
    """Read a raw byte slice field from var_data by tag."""
    if not var_data:
        return None
    lib = load_library()
    cmsg = lib.datarouter_msg_new(0)
    if not cmsg:
        return None
    try:
        vd_buf = (ctypes.c_uint8 * len(var_data)).from_buffer_copy(var_data)
        lib.datarouter_msg_set_var_data(cmsg, ctypes.cast(vd_buf, ctypes.c_void_p), len(var_data))
        out_len = ctypes.c_size_t(0)
        rc = lib.datarouter_tlv_read_bytes(cmsg, tag, None, 0, ctypes.byref(out_len))
        if rc != DATAROUTER_OK or out_len.value == 0:
            return None
        buf = (ctypes.c_uint8 * out_len.value)()
        rc = lib.datarouter_tlv_read_bytes(
            cmsg, tag, ctypes.cast(buf, ctypes.c_void_p), out_len, ctypes.byref(out_len)
        )
        if rc != DATAROUTER_OK:
            return None
        return bytes(buf[: out_len.value])
    finally:
        lib.datarouter_msg_free(cmsg)


def tlv_read_u16(var_data: bytes, tag: int) -> Optional[int]:
    """Read a u16 LE field from var_data by tag."""
    raw = tlv_read_bytes(var_data, tag)
    if raw is None or len(raw) < 2:
        return None
    return int.from_bytes(raw[:2], "little")


def tlv_read_byte(var_data: bytes, tag: int) -> Optional[int]:
    """Read a single byte field from var_data by tag."""
    raw = tlv_read_bytes(var_data, tag)
    if raw is None or len(raw) < 1:
        return None
    return raw[0]


def tlv_put_bytes(tag: int, value: bytes) -> bytes:
    """Build one TLV field."""
    if len(value) > 0xFFFF:
        raise ValueError("TLV value too large")
    return bytes([tag]) + len(value).to_bytes(2, "little") + value


def tlv_put_u16(tag: int, value: int) -> bytes:
    """Build one u16 TLV field."""
    return tlv_put_bytes(tag, int(value).to_bytes(2, "little"))


def _msg_to_python(cmsg: ctypes.c_void_p, lib: ctypes.CDLL) -> Message:
    """Convert a C message pointer to a Python Message."""
    msg_type = lib.datarouter_msg_type(cmsg)
    flags = lib.datarouter_msg_flags(cmsg)

    vd_len = lib.datarouter_msg_var_data_len(cmsg)
    var_data = b""
    if vd_len > 0:
        vd_ptr = lib.datarouter_msg_var_data(cmsg)
        if vd_ptr:
            var_data = bytes(vd_ptr[:vd_len])

    b_len = lib.datarouter_msg_body_len(cmsg)
    body = b""
    if b_len > 0:
        b_ptr = lib.datarouter_msg_body(cmsg)
        if b_ptr:
            body = bytes(b_ptr[:b_len])

    return Message(message_type=msg_type, flags=flags, var_data=var_data, body=body)


def pack(msg: Message) -> bytes:
    """Serialize a Message to wire format bytes."""
    lib = load_library()
    cmsg = lib.datarouter_msg_new(msg.message_type)
    if not cmsg:
        raise MemoryError("failed to allocate message")
    try:
        if msg.flags:
            lib.datarouter_msg_set_flags(cmsg, msg.flags)
        if msg.var_data:
            vd_buf = (ctypes.c_uint8 * len(msg.var_data)).from_buffer_copy(msg.var_data)
            lib.datarouter_msg_set_var_data(
                cmsg, ctypes.cast(vd_buf, ctypes.c_void_p), len(msg.var_data)
            )
        if msg.body:
            body_buf = (ctypes.c_uint8 * len(msg.body)).from_buffer_copy(msg.body)
            lib.datarouter_msg_set_body(cmsg, ctypes.cast(body_buf, ctypes.c_void_p), len(msg.body))

        capacity = 8 + len(msg.var_data) + len(msg.body)
        out_buf = (ctypes.c_uint8 * capacity)()
        out_len = ctypes.c_size_t(0)
        rc = lib.datarouter_msg_pack(
            cmsg,
            ctypes.cast(out_buf, ctypes.c_void_p),
            capacity,
            ctypes.byref(out_len),
        )
        _check(rc)
        return bytes(out_buf[: out_len.value])
    finally:
        lib.datarouter_msg_free(cmsg)


def unpack(data: bytes) -> Message:
    """Deserialize a Message from wire format bytes."""
    if not data:
        raise ValueError("empty data")
    lib = load_library()
    cmsg = ctypes.c_void_p()
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    rc = lib.datarouter_msg_unpack(ctypes.cast(buf, ctypes.c_void_p), len(data), ctypes.byref(cmsg))
    _check(rc)
    try:
        return _msg_to_python(cmsg, lib)
    finally:
        lib.datarouter_msg_free(cmsg)


def pack_business_request(
    module: str,
    action: str,
    request_id: str,
    connection_id: str = "",
    context_json: Optional[bytes] = None,
    payload: bytes = b"",
    *,
    workflow_id: Optional[str] = None,
    task_uuid: Optional[str] = None,
    step_id: Optional[str] = None,
    actor_uuid: Optional[str] = None,
    trace_id: Optional[str] = None,
) -> bytes:
    """Build and pack a business request message.

    Returns the wire-format bytes ready to send via Port.send().
    Workflow fields are optional metadata that extend var_data without
    changing body JSON compatibility.
    """
    lib = load_library()
    cmsg = lib.datarouter_build_business_request(
        module.encode("utf-8"),
        action.encode("utf-8"),
        request_id.encode("utf-8"),
        connection_id.encode("utf-8"),
        ctypes.cast(
            (ctypes.c_uint8 * len(context_json)).from_buffer_copy(context_json),
            ctypes.c_void_p,
        )
        if context_json
        else None,
        len(context_json) if context_json else 0,
        ctypes.cast(
            (ctypes.c_uint8 * len(payload)).from_buffer_copy(payload),
            ctypes.c_void_p,
        )
        if payload
        else None,
        len(payload) if payload else 0,
    )
    if not cmsg:
        raise MemoryError("failed to build business request")
    try:
        # Append workflow TLV fields if provided
        for tag, value in [
            (TAG_WORKFLOW_ID, workflow_id),
            (TAG_TASK_UUID, task_uuid),
            (TAG_STEP_ID, step_id),
            (TAG_ACTOR_UUID, actor_uuid),
            (TAG_TRACE_ID, trace_id),
        ]:
            if value:
                lib.datarouter_tlv_put_string(cmsg, tag, value.encode("utf-8"))

        return _pack_cmsg(cmsg, lib)
    finally:
        lib.datarouter_msg_free(cmsg)


def pack_business_response(
    module: str,
    action: str,
    request_id: str,
    connection_id: str = "",
    payload: bytes = b"",
    streaming: bool = False,
    *,
    workflow_id: Optional[str] = None,
    task_uuid: Optional[str] = None,
    step_id: Optional[str] = None,
    actor_uuid: Optional[str] = None,
    trace_id: Optional[str] = None,
) -> bytes:
    """Build and pack a business response message.

    Returns the wire-format bytes ready to send via Port.send().
    """
    lib = load_library()
    cmsg = lib.datarouter_build_business_response(
        module.encode("utf-8"),
        action.encode("utf-8"),
        request_id.encode("utf-8"),
        connection_id.encode("utf-8"),
        ctypes.cast(
            (ctypes.c_uint8 * len(payload)).from_buffer_copy(payload),
            ctypes.c_void_p,
        )
        if payload
        else None,
        len(payload) if payload else 0,
        1 if streaming else 0,
    )
    if not cmsg:
        raise MemoryError("failed to build business response")
    try:
        for tag, value in [
            (TAG_WORKFLOW_ID, workflow_id),
            (TAG_TASK_UUID, task_uuid),
            (TAG_STEP_ID, step_id),
            (TAG_ACTOR_UUID, actor_uuid),
            (TAG_TRACE_ID, trace_id),
        ]:
            if value:
                lib.datarouter_tlv_put_string(cmsg, tag, value.encode("utf-8"))

        return _pack_cmsg(cmsg, lib)
    finally:
        lib.datarouter_msg_free(cmsg)


def unpack_business_message(data: bytes) -> Message:
    """Unpack wire-format bytes into a Message.

    This is the same as unpack() but with a name that matches the
    Phase 1 spec. Prefer this for new code.
    """
    return unpack(data)


def _pack_cmsg(cmsg: ctypes.c_void_p, lib: ctypes.CDLL) -> bytes:
    """Pack a C message pointer to bytes."""
    vd_len = lib.datarouter_msg_var_data_len(cmsg)
    b_len = lib.datarouter_msg_body_len(cmsg)
    capacity = 8 + vd_len + b_len
    out_buf = (ctypes.c_uint8 * capacity)()
    out_len = ctypes.c_size_t(0)
    rc = lib.datarouter_msg_pack(
        cmsg,
        ctypes.cast(out_buf, ctypes.c_void_p),
        capacity,
        ctypes.byref(out_len),
    )
    _check(rc)
    return bytes(out_buf[: out_len.value])

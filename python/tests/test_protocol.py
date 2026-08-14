"""Tests for the datarouter protocol codec.

Tests are aligned with the Rust protocol tests in lib/datarouter/src/protocol.rs
to ensure cross-language compatibility of the wire format.
"""

from __future__ import annotations

import os
import sys
import unittest

# Ensure the source tree library is found
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from blinkcms_datarouter.protocol import (
    MAX_APP_PAYLOAD,
    MSG_BUSINESS_REQUEST,
    MSG_BUSINESS_RESPONSE,
    MSG_LOG_NOTICE,
    TAG_ACTION,
    TAG_ACTOR_UUID,
    TAG_CONNECTION_ID,
    TAG_CONTEXT,
    TAG_IS_TEXT,
    TAG_JSON_DATA,
    TAG_MODULE,
    TAG_REQUEST_ID,
    TAG_STATUS,
    TAG_STEP_ID,
    TAG_STREAMING,
    TAG_TASK_UUID,
    TAG_TRACE_ID,
    TAG_WORKFLOW_ID,
    Message,
    pack,
    pack_business_request,
    pack_business_response,
    tlv_read_byte,
    tlv_read_bytes,
    tlv_read_string,
    tlv_read_u16,
    unpack,
    unpack_business_message,
)


class TestTagConstantsMatchRust(unittest.TestCase):
    """Verify tag constant values match the Rust definitions exactly."""

    def test_tag_values(self) -> None:
        self.assertEqual(TAG_CONNECTION_ID, 0x04)
        self.assertEqual(TAG_REQUEST_ID, 0x05)
        self.assertEqual(TAG_STATUS, 0x06)
        self.assertEqual(TAG_IS_TEXT, 0x07)
        self.assertEqual(TAG_CONTEXT, 0x0D)
        self.assertEqual(TAG_JSON_DATA, 0x0E)
        self.assertEqual(TAG_MODULE, 0x0F)
        self.assertEqual(TAG_ACTION, 0x10)
        self.assertEqual(TAG_STREAMING, 0x11)
        self.assertEqual(TAG_WORKFLOW_ID, 0x12)
        self.assertEqual(TAG_TASK_UUID, 0x13)
        self.assertEqual(TAG_STEP_ID, 0x14)
        self.assertEqual(TAG_ACTOR_UUID, 0x15)
        self.assertEqual(TAG_TRACE_ID, 0x16)

    def test_message_type_values(self) -> None:
        self.assertEqual(MSG_BUSINESS_REQUEST, 0x07)
        self.assertEqual(MSG_BUSINESS_RESPONSE, 0x8C)
        self.assertEqual(MSG_LOG_NOTICE, 0x8D)

    def test_max_app_payload(self) -> None:
        # 512 KiB power-of-two ring capacity minus 8-byte frame header.
        self.assertEqual(MAX_APP_PAYLOAD, (512 * 1024) - 8)


class TestPackUnpack(unittest.TestCase):
    def test_round_trip(self) -> None:
        packed = pack_business_request(
            module="admin",
            action="auth.login",
            request_id="r1",
            connection_id="c1",
            payload=b'{"action":"auth.login"}',
        )
        unpacked = unpack(packed)
        self.assertEqual(unpacked.message_type, MSG_BUSINESS_REQUEST)
        self.assertEqual(unpacked.module, "admin")
        self.assertEqual(unpacked.action, "auth.login")
        self.assertEqual(unpacked.request_id, "r1")
        self.assertEqual(unpacked.connection_id, "c1")
        self.assertEqual(unpacked.body, b'{"action":"auth.login"}')

    def test_empty_message(self) -> None:
        msg = Message(message_type=0x01)
        packed = pack(msg)
        unpacked = unpack(packed)
        self.assertEqual(unpacked.message_type, 0x01)
        self.assertEqual(unpacked.body, b"")

    def test_empty_body_round_trip(self) -> None:
        """Aligns with Rust test: empty_body_message_round_trip."""
        packed = pack_business_request(
            module="admin",
            action="ping",
            request_id="p1",
            payload=b"",
        )
        unpacked = unpack(packed)
        self.assertEqual(unpacked.message_type, MSG_BUSINESS_REQUEST)
        self.assertEqual(unpacked.body, b"")
        self.assertEqual(unpacked.module, "admin")
        self.assertEqual(unpacked.action, "ping")

    def test_empty_var_data_round_trip(self) -> None:
        """Aligns with Rust test: empty_var_data_message_round_trip."""
        msg = Message(
            message_type=MSG_BUSINESS_RESPONSE,
            flags=0,
            var_data=b"",
            body=b"response body",
        )
        packed = pack(msg)
        unpacked = unpack(packed)
        self.assertEqual(unpacked.message_type, MSG_BUSINESS_RESPONSE)
        self.assertEqual(unpacked.body, b"response body")
        self.assertEqual(unpacked.var_data, b"")

    def test_pack_unpack_preserves_flags(self) -> None:
        """Aligns with Rust test: pack_unpack_preserves_flags."""
        msg = Message(
            message_type=MSG_BUSINESS_RESPONSE,
            flags=0x42,
            var_data=b"",
            body=b"flags-test",
        )
        packed = pack(msg)
        unpacked = unpack(packed)
        self.assertEqual(unpacked.flags, 0x42)


class TestTLVReadWrite(unittest.TestCase):
    def test_read_nonexistent_tag_returns_none(self) -> None:
        """Aligns with Rust test: tlv_read_nonexistent_tag_returns_none."""
        packed = pack_business_request(
            module="admin", action="test", request_id="r1", payload=b"body"
        )
        msg = unpack(packed)
        self.assertIsNone(tlv_read_string(msg.var_data, TAG_TRACE_ID))
        self.assertIsNone(tlv_read_u16(msg.var_data, TAG_STATUS))
        self.assertIsNone(tlv_read_byte(msg.var_data, TAG_STREAMING))


class TestBusinessRequest(unittest.TestCase):
    def test_basic(self) -> None:
        packed = pack_business_request(
            module="admin",
            action="auth.login",
            request_id="req-1",
            connection_id="ws-1",
            context_json=b'{"host":"example.com"}',
            payload=b'{"user":"admin"}',
        )
        msg = unpack_business_message(packed)
        self.assertEqual(msg.message_type, MSG_BUSINESS_REQUEST)
        self.assertEqual(msg.module, "admin")
        self.assertEqual(msg.action, "auth.login")
        self.assertEqual(msg.request_id, "req-1")
        self.assertEqual(msg.connection_id, "ws-1")
        self.assertEqual(msg.body, b'{"user":"admin"}')

    def test_with_context(self) -> None:
        """Aligns with Rust test: business_request_with_context."""
        ctx = b'{"host":"example.com","ip":"127.0.0.1"}'
        packed = pack_business_request(
            module="admin",
            action="test",
            request_id="r1",
            connection_id="c1",
            context_json=ctx,
            payload=b"body",
        )
        msg = unpack(packed)
        read_ctx = tlv_read_bytes(msg.var_data, TAG_CONTEXT)
        self.assertIsNotNone(read_ctx)
        self.assertEqual(read_ctx, ctx)

    def test_without_context(self) -> None:
        """Aligns with Rust test: business_request_with_empty_context."""
        packed = pack_business_request(
            module="admin",
            action="test",
            request_id="r1",
            payload=b"body",
        )
        msg = unpack(packed)
        self.assertIsNone(tlv_read_bytes(msg.var_data, TAG_CONTEXT))

    def test_with_workflow_fields(self) -> None:
        packed = pack_business_request(
            module="agent",
            action="ml.process",
            request_id="wf-1",
            payload=b'{"data":"test"}',
            workflow_id="wf-abc-123",
            task_uuid="task-uuid-456",
            step_id="step-7",
            actor_uuid="actor-uuid-890",
            trace_id="trace-id-xyz",
        )
        msg = unpack_business_message(packed)
        self.assertEqual(msg.module, "agent")
        self.assertEqual(msg.action, "ml.process")
        self.assertEqual(msg.workflow_id, "wf-abc-123")
        self.assertEqual(msg.task_uuid, "task-uuid-456")
        self.assertEqual(msg.step_id, "step-7")
        self.assertEqual(msg.actor_uuid, "actor-uuid-890")
        self.assertEqual(msg.trace_id, "trace-id-xyz")

    def test_without_workflow_fields(self) -> None:
        packed = pack_business_request(
            module="admin",
            action="test",
            request_id="r1",
            payload=b"payload",
        )
        msg = unpack_business_message(packed)
        self.assertIsNone(msg.workflow_id)
        self.assertIsNone(msg.task_uuid)
        self.assertIsNone(msg.step_id)
        self.assertIsNone(msg.actor_uuid)
        self.assertIsNone(msg.trace_id)

    def test_all_workflow_metadata_together(self) -> None:
        """Aligns with Rust test: extract_business_fields_with_all_workflow_metadata."""
        packed = pack_business_request(
            module="agent",
            action="content_import.run",
            request_id="req-full",
            connection_id="conn-1",
            payload=b"{}",
            workflow_id="wf-test",
            task_uuid="task-test",
            step_id="step-test",
            actor_uuid="actor-test",
            trace_id="trace-test",
        )
        msg = unpack(packed)
        self.assertEqual(msg.module, "agent")
        self.assertEqual(msg.action, "content_import.run")
        self.assertEqual(msg.request_id, "req-full")
        self.assertEqual(msg.connection_id, "conn-1")
        self.assertEqual(msg.workflow_id, "wf-test")
        self.assertEqual(msg.task_uuid, "task-test")
        self.assertEqual(msg.step_id, "step-test")
        self.assertEqual(msg.actor_uuid, "actor-test")
        self.assertEqual(msg.trace_id, "trace-test")


class TestProtocolControlHelpersAreNotPublic(unittest.TestCase):
    def test_chunk_helpers_are_not_application_api(self) -> None:
        import blinkcms_datarouter.protocol as protocol

        self.assertFalse(hasattr(protocol, "chunk_message"))
        self.assertFalse(hasattr(protocol, "chunk_ack_message"))
        self.assertFalse(hasattr(protocol, "ChunkAssembler"))


class TestBusinessResponse(unittest.TestCase):
    def test_basic(self) -> None:
        packed = pack_business_response(
            module="admin",
            action="auth.login",
            request_id="req-1",
            connection_id="ws-1",
            payload=b'{"success":true}',
        )
        msg = unpack_business_message(packed)
        self.assertEqual(msg.message_type, MSG_BUSINESS_RESPONSE)
        self.assertEqual(msg.module, "admin")
        self.assertEqual(msg.action, "auth.login")
        self.assertFalse(msg.streaming)

    def test_streaming(self) -> None:
        packed = pack_business_response(
            module="admin",
            action="visitors.live",
            request_id="req-2",
            payload=b'{"visitors":[]}',
            streaming=True,
        )
        msg = unpack_business_message(packed)
        self.assertTrue(msg.streaming)

    def test_with_workflow_fields(self) -> None:
        """Business responses can also carry workflow metadata."""
        packed = pack_business_response(
            module="agent",
            action="ml.process",
            request_id="wf-resp-1",
            payload=b'{"ok":true}',
            workflow_id="wf-resp",
            task_uuid="task-resp",
        )
        msg = unpack_business_message(packed)
        self.assertEqual(msg.message_type, MSG_BUSINESS_RESPONSE)
        self.assertEqual(msg.workflow_id, "wf-resp")
        self.assertEqual(msg.task_uuid, "task-resp")


class TestWireFormatCompatibility(unittest.TestCase):
    """Tests ensuring Python pack/unpack produces the same logical output as Rust."""

    def test_header_structure(self) -> None:
        """Verify the 8-byte header layout matches Rust wire format."""
        packed = pack_business_request(
            module="admin", action="test", request_id="r1", payload=b"hello"
        )
        # Header: [1B type][1B flags][2B var_data_len LE][4B body_len LE]
        self.assertEqual(packed[0], MSG_BUSINESS_REQUEST)  # message_type
        self.assertEqual(packed[1], 0)  # flags
        var_data_len = int.from_bytes(packed[2:4], "little")
        body_len = int.from_bytes(packed[4:8], "little")
        self.assertEqual(body_len, 5)  # len(b"hello")
        self.assertEqual(len(packed), 8 + var_data_len + body_len)

    def test_tlv_encoding_format(self) -> None:
        """Verify TLV entries use [1B tag][2B length LE][N bytes value]."""
        packed = pack_business_request(module="admin", action="test", request_id="r1", payload=b"")
        msg = unpack(packed)
        vd = msg.var_data
        # First TLV entry should start with TAG_MODULE (0x0F)
        self.assertEqual(vd[0], TAG_MODULE)
        # Next 2 bytes are the length in LE
        length = int.from_bytes(vd[1:3], "little")
        self.assertEqual(length, 5)  # len(b"admin")
        # Value follows
        self.assertEqual(vd[3 : 3 + length], b"admin")


if __name__ == "__main__":
    unittest.main()

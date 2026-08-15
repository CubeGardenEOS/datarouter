// Package datarouter wraps the BlinkCMS datarouter Rust frontend C ABI
// (datarouter_frontend.h) for Go via cgo.
//
// The frontend opens the switch-owned shared memory through the control
// plane and moves data on the **direct (unsafe) data plane by default**
// (2 payload copies, single-frame messages ≤ MAX_APP_PAYLOAD). Set
// Options.DataPlane = DataPlaneRouted for the switch-routed plane
// (reader thread + queue + chunking/ACK/reassembly).
//
// Prerequisites: the shared library (built from the Rust staticlib, e.g.
// `cargo build --release` + `cc -shared -Wl,--whole-archive`) must be at
// <repo>/build/libblinkcms_datarouter.so, and the switch binary at
// <repo>/build/datarouter-switch for the integration test.
package datarouter

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -Wl,-rpath,${SRCDIR}/../../build -lblinkcms_datarouter -lrt -lpthread -ldl
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "datarouter_frontend.h"
*/
import "C"

import (
	"runtime"
	"unsafe"
)

// Error codes (datarouter.h).
const (
	ErrOK               = 0
	ErrInvalidArgument  = 1
	ErrNotFound         = 2
	ErrBusy             = 3
	ErrFull             = 4
	ErrPayloadTooLarge  = 5
	ErrAgain            = 6
	ErrTimeout          = 7
	ErrPermission       = 8
	ErrPlatform         = 9
	ErrProtocol         = 10
	ErrInternal         = 11
	MaxPayload          = 512*1024 - 8 // MAX_APP_PAYLOAD (layout 12)
	FrontendABIVersion  = 2
	FrontendABIVersion3 = 3
)

// Data plane selection for Options.DataPlane.
const (
	DataPlaneDirect byte = 0 // default: 2 copies, single-frame messages
	DataPlaneRouted byte = 1 // switch-routed: queue + chunking/ACK/reassembly
)

// Error wraps a datarouter error code.
type Error struct{ Code int }

func (e *Error) Error() string {
	msg := C.datarouter_strerror(C.int(e.Code))
	if msg == nil {
		return "datarouter error"
	}
	return C.GoString(msg)
}

func codeError(code C.int) error {
	if code == C.int(ErrOK) {
		return nil
	}
	return &Error{Code: int(code)}
}

// Options configures Frontend.Open.
type Options struct {
	ShmName          string // default "/datarouter"
	ServiceName      string
	ConnectTimeoutMs uint32
	RecvTimeoutMs    uint32
	AuthToken        string // required when the switch runs with --token
	DataPlane        byte   // DataPlaneDirect (default) or DataPlaneRouted
}

// Frontend is an owned frontend handle. Close releases it.
type Frontend struct {
	handle *C.datarouter_frontend
	opts   Options
}

// Open connects through the switch control plane and registers ServiceName.
func Open(opts Options) (*Frontend, error) {
	if opts.ShmName == "" {
		opts.ShmName = "/datarouter"
	}
	c := C.datarouter_frontend_options_v3{}
	shm := C.CString(opts.ShmName)
	svc := C.CString(opts.ServiceName)
	defer C.free(unsafe.Pointer(shm))
	defer C.free(unsafe.Pointer(svc))
	c.shm_name = shm
	c.service_name = svc
	c.connect_timeout_ms = C.uint32_t(opts.ConnectTimeoutMs)
	c.recv_timeout_ms = C.uint32_t(opts.RecvTimeoutMs)
	if opts.ConnectTimeoutMs == 0 {
		c.connect_timeout_ms = 3000
	}
	if opts.RecvTimeoutMs == 0 {
		c.recv_timeout_ms = 3000
	}
	if opts.AuthToken != "" {
		tok := C.CString(opts.AuthToken)
		defer C.free(unsafe.Pointer(tok))
		c.auth_token = tok
	}
	c.data_plane = C.uint8_t(opts.DataPlane)
	var out *C.datarouter_frontend
	rc := C.datarouter_frontend_open_v3(
		&c,
		C.size_t(unsafe.Sizeof(c)),
		C.uint32_t(FrontendABIVersion3),
		&out,
	)
	if rc != C.int(ErrOK) {
		return nil, codeError(rc)
	}
	f := &Frontend{handle: out, opts: opts}
	runtime.SetFinalizer(f, (*Frontend).Close)
	return f, nil
}

// Send sends a packed wire-format message to targetService. On the direct
// plane the message must be a single frame ≤ MaxPayload; larger messages
// return ErrPayloadTooLarge (use the routed plane for chunking).
func (f *Frontend) Send(targetService string, data []byte, timeoutMs uint32) error {
	target := C.CString(targetService)
	defer C.free(unsafe.Pointer(target))
	var ptr unsafe.Pointer
	if len(data) > 0 {
		ptr = unsafe.Pointer(&data[0])
	}
	rc := C.datarouter_frontend_send(f.handle, target, ptr, C.size_t(len(data)), C.uint32_t(timeoutMs))
	return codeError(rc)
}

// Recv reads one complete message into buf. Returns (bytes written, source
// service, error). On a short buffer the message is not consumed and
// ErrPayloadTooLarge is returned; query FrameLen and retry with a larger
// buffer.
func (f *Frontend) Recv(buf []byte, timeoutMs uint32) (int, string, error) {
	var info C.datarouter_message_info
	var ptr unsafe.Pointer
	if len(buf) > 0 {
		ptr = unsafe.Pointer(&buf[0])
	}
	rc := C.datarouter_frontend_recv(f.handle, ptr, C.size_t(len(buf)), &info, C.uint32_t(timeoutMs))
	if rc != C.int(ErrOK) {
		return 0, "", codeError(rc)
	}
	n := int(info.len)
	src := make([]byte, 0, 32)
	for _, b := range info.source {
		if b == 0 {
			break
		}
		src = append(src, byte(b))
	}
	return n, string(src), nil
}

// FrameLen peeks the next RX frame's payload length without consuming it.
// Returns ErrAgain when the RX ring is empty.
func (f *Frontend) FrameLen() (int, error) {
	var n C.size_t
	rc := C.datarouter_frontend_frame_len(f.handle, &n)
	if rc != C.int(ErrOK) {
		return 0, codeError(rc)
	}
	return int(n), nil
}

// Close releases the frontend handle. Safe to call multiple times.
func (f *Frontend) Close() {
	if f == nil || f.handle == nil {
		return
	}
	C.datarouter_frontend_close(f.handle)
	f.handle = nil
	runtime.SetFinalizer(f, nil)
}

// Version returns the C library version string.
func Version() string {
	return C.GoString(C.datarouter_version())
}

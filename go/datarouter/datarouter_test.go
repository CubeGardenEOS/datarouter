package datarouter

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

// startSwitch spawns the repo's switch binary on a unique shm name.
func startSwitch(t *testing.T) (shm string, stop func()) {
	t.Helper()
	bin := os.Getenv("DATAROUTER_SWITCH_BIN")
	if bin == "" {
		cwd, err := os.Getwd()
		if err != nil {
			t.Fatal(err)
		}
		bin = filepath.Join(cwd, "..", "..", "build", "datarouter-switch")
	}
	shm = "/datarouter-go-test"
	cmd := exec.Command(bin, "--name", shm, "--ttl-ms", "30000", "--allow", "go_sender,go_receiver")
	if err := cmd.Start(); err != nil {
		t.Fatalf("start switch: %v", err)
	}
	time.Sleep(200 * time.Millisecond)
	return shm, func() {
		_ = cmd.Process.Kill()
		_, _ = cmd.Process.Wait()
	}
}

// packRequest builds a minimal business-request wire frame: 8-byte header
// (type, flags, var_data_len LE u16, body_len LE u32) + body.
func packRequest(body []byte) []byte {
	wire := make([]byte, 8+len(body))
	wire[0] = 0x07 // MSG_BUSINESS_REQUEST
	wire[1] = 0
	wire[2], wire[3] = 0, 0
	wire[4] = byte(len(body))
	wire[5] = byte(len(body) >> 8)
	wire[6] = byte(len(body) >> 16)
	wire[7] = byte(len(body) >> 24)
	copy(wire[8:], body)
	return wire
}

func openPair(t *testing.T, shm string, dataPlane byte) (*Frontend, *Frontend) {
	t.Helper()
	sender, err := Open(Options{ShmName: shm, ServiceName: "go_sender", RecvTimeoutMs: 3000, DataPlane: dataPlane})
	if err != nil {
		t.Fatalf("open sender: %v", err)
	}
	receiver, err := Open(Options{ShmName: shm, ServiceName: "go_receiver", RecvTimeoutMs: 3000, DataPlane: dataPlane})
	if err != nil {
		t.Fatalf("open receiver: %v", err)
	}
	t.Cleanup(func() { sender.Close(); receiver.Close() })
	return sender, receiver
}

func TestDirectPlaneRoundTrip(t *testing.T) {
	shm, stop := startSwitch(t)
	defer stop()
	sender, receiver := openPair(t, shm, DataPlaneDirect)

	payload := []byte("hello from go direct plane")
	wire := packRequest(payload)
	if err := sender.Send("go_receiver", wire, 2000); err != nil {
		t.Fatalf("send: %v", err)
	}
	buf := make([]byte, 1<<20)
	n, source, err := receiver.Recv(buf, 3000)
	if err != nil {
		t.Fatalf("recv: %v", err)
	}
	if source != "go_sender" {
		t.Errorf("source = %q, want go_sender", source)
	}
	if got, want := buf[:n], wire; string(got) != string(want) {
		t.Errorf("payload mismatch: got %d bytes", n)
	}
}

func TestRoutedPlaneRoundTrip(t *testing.T) {
	shm, stop := startSwitch(t)
	defer stop()
	sender, receiver := openPair(t, shm, DataPlaneRouted)

	wire := packRequest([]byte("hello from go routed plane"))
	if err := sender.Send("go_receiver", wire, 2000); err != nil {
		t.Fatalf("send: %v", err)
	}
	buf := make([]byte, 1<<20)
	n, source, err := receiver.Recv(buf, 3000)
	if err != nil {
		t.Fatalf("recv: %v", err)
	}
	if source != "go_sender" {
		t.Errorf("source = %q, want go_sender", source)
	}
	if got, want := buf[:n], wire; string(got) != string(want) {
		t.Errorf("payload mismatch: got %d bytes", n)
	}
}

func TestFrameLenPeek(t *testing.T) {
	shm, stop := startSwitch(t)
	defer stop()
	sender, receiver := openPair(t, shm, DataPlaneDirect)

	if _, err := receiver.FrameLen(); err == nil || err.(*Error).Code != ErrAgain {
		t.Fatalf("empty frame_len should be ErrAgain, got %v", err)
	}
	wire := packRequest(make([]byte, 4096))
	if err := sender.Send("go_receiver", wire, 2000); err != nil {
		t.Fatalf("send: %v", err)
	}
	n, err := receiver.FrameLen()
	if err != nil || n != len(wire) {
		t.Fatalf("frame_len = %d, %v; want %d", n, err, len(wire))
	}
	buf := make([]byte, 1<<20)
	if _, _, err := receiver.Recv(buf, 3000); err != nil {
		t.Fatalf("recv: %v", err)
	}
}

func TestShortBufferKeepsFrame(t *testing.T) {
	shm, stop := startSwitch(t)
	defer stop()
	sender, receiver := openPair(t, shm, DataPlaneDirect)

	wire := packRequest(make([]byte, 200*1024))
	if err := sender.Send("go_receiver", wire, 2000); err != nil {
		t.Fatalf("send: %v", err)
	}
	if _, _, err := receiver.Recv(make([]byte, 1024), 3000); err == nil || err.(*Error).Code != ErrPayloadTooLarge {
		t.Fatalf("short buffer should be ErrPayloadTooLarge, got %v", err)
	}
	buf := make([]byte, 1<<20)
	n, _, err := receiver.Recv(buf, 3000)
	if err != nil || n != len(wire) {
		t.Fatalf("retry with larger buffer: n=%d err=%v", n, err)
	}
}

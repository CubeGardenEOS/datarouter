package datarouter

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func randomShmName(t *testing.T) string {
	t.Helper()
	var token [16]byte
	if _, err := rand.Read(token[:]); err != nil {
		t.Fatalf("random shm name: %v", err)
	}
	return "/datarouter-go-test-" + hex.EncodeToString(token[:])
}

// switchLog supports os/exec's concurrent output copies and readiness reads.
type switchLog struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (l *switchLog) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.buf.Write(p)
}
func (l *switchLog) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.buf.String()
}

// startSwitch scopes a switch and its private shm object to one test.
func startSwitch(t *testing.T) string {
	t.Helper()
	bin := os.Getenv("DATAROUTER_SWITCH_BIN")
	if bin == "" {
		cwd, err := os.Getwd()
		if err != nil {
			t.Fatal(err)
		}
		bin = filepath.Join(cwd, "..", "..", "build", "datarouter-switch")
	}
	shm := randomShmName(t)
	cmd := exec.Command(bin, "--name", shm, "--ttl-ms", "30000", "--allow", "go_sender,go_receiver")
	logs := &switchLog{}
	cmd.Stdout, cmd.Stderr = logs, logs
	if err := cmd.Start(); err != nil {
		t.Fatalf("start switch: %v", err)
	}
	done := make(chan struct{})
	var waitErr error
	go func() { waitErr = cmd.Wait(); close(done) }()
	t.Cleanup(func() {
		_ = cmd.Process.Kill()
		<-done
		if t.Failed() {
			t.Logf("switch pid=%d shm=%s log:\n%s", cmd.Process.Pid, shm, logs.String())
		}
		// The test owns this generated name; remove an object left behind by
		// SIGKILL without ever touching a pre-existing fixed-name segment.
		_ = os.Remove(filepath.Join("/dev/shm", shm[1:]))
	})
	deadline := time.NewTimer(5 * time.Second)
	defer deadline.Stop()
	poll := time.NewTicker(5 * time.Millisecond)
	defer poll.Stop()
	for {
		select {
		case <-done:
			t.Fatalf("switch exited before ready: pid=%d shm=%s err=%v\n%s", cmd.Process.Pid, shm, waitErr, logs.String())
		case <-deadline.C:
			t.Fatalf("switch readiness timeout: pid=%d shm=%s\n%s", cmd.Process.Pid, shm, logs.String())
		case <-poll.C:
			for _, line := range strings.Split(logs.String(), "\n") {
				if strings.Contains(line, fmt.Sprintf("start pid=%d ", cmd.Process.Pid)) && strings.Contains(line, " shm="+shm+" ") {
					select {
					case <-done:
						t.Fatalf("switch exited at ready: %v\n%s", waitErr, logs.String())
					default:
						return shm
					}
				}
			}
		}
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
	t.Cleanup(func() { sender.Close() })
	receiver, err := Open(Options{ShmName: shm, ServiceName: "go_receiver", RecvTimeoutMs: 3000, DataPlane: dataPlane})
	if err != nil {
		t.Fatalf("open receiver: %v", err)
	}
	t.Cleanup(func() { receiver.Close() })
	return sender, receiver
}

func TestDirectPlaneRoundTrip(t *testing.T) {
	shm := startSwitch(t)
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
	shm := startSwitch(t)
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
	shm := startSwitch(t)
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
	shm := startSwitch(t)
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

// A subprocess lets expected fixture failures remain visible without turning
// the surrounding acceptance test into a false failure.
func TestFixtureFailureChild(t *testing.T) {
	mode := os.Getenv("DR_GO_FIXTURE_CASE")
	if mode == "" {
		t.Skip("fixture subprocess only")
	}
	if mode == "exit" || mode == "delay" {
		// An exec wrapper preserves PID when it finally starts the real switch.
		wrapper := filepath.Join(t.TempDir(), "switch")
		body := "#!/bin/sh\n"
		if mode == "exit" {
			body += "echo controlled-early-exit >&2\nexit 23\n"
		} else {
			body += "sleep 0.4\nexec \"$DR_GO_REAL_SWITCH\" \"$@\"\n"
		}
		if err := os.WriteFile(wrapper, []byte(body), 0700); err != nil {
			t.Fatal(err)
		}
		t.Setenv("DATAROUTER_SWITCH_BIN", wrapper)
	}
	shm := startSwitch(t)
	if mode == "receiver" {
		// Occupy the receiver name so the second Open must fail immediately.
		occupied, err := Open(Options{ShmName: shm, ServiceName: "go_receiver", RecvTimeoutMs: 100})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { occupied.Close() })
		openPair(t, shm, DataPlaneDirect)
	} else if mode == "layout" {
		path := filepath.Join("/dev/shm", shm[1:])
		f, err := os.OpenFile(path, os.O_RDWR, 0)
		if err != nil {
			t.Fatal(err)
		}
		// Version is the u32 immediately after magic in the stable control prefix.
		if _, err = f.WriteAt([]byte{255, 255, 255, 255}, 4); err != nil {
			f.Close()
			t.Fatal(err)
		}
		f.Close()
		_, err = Open(Options{ShmName: shm, ServiceName: "go_sender", RecvTimeoutMs: 100})
		if err == nil {
			t.Fatal("layout conflict unexpectedly opened")
		}
		if e, ok := err.(*Error); !ok || e.Code != ErrProtocol {
			t.Fatalf("wrong layout conflict error: %v", err)
		}
		t.Fatalf("controlled layout conflict: %v", err)
	}
}
func TestFixtureFailures(t *testing.T) {
	real := os.Getenv("DATAROUTER_SWITCH_BIN")
	if real == "" {
		cwd, _ := os.Getwd()
		real = filepath.Join(cwd, "../../build/datarouter-switch")
	}
	for _, mode := range []string{"delay", "exit", "receiver", "layout"} {
		t.Run(mode, func(t *testing.T) {
			cmd := exec.Command(os.Args[0], "-test.run=^TestFixtureFailureChild$", "-test.v", "-test.timeout=15s")
			cmd.Env = append(os.Environ(), "DR_GO_FIXTURE_CASE="+mode, "DR_GO_REAL_SWITCH="+real)
			output, err := cmd.CombinedOutput()
			t.Logf("%s", output)
			if mode == "delay" {
				if err != nil {
					t.Fatalf("delayed startup: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatal("expected failure was hidden")
			}
			want := map[string]string{"exit": "controlled-early-exit", "receiver": "open receiver:", "layout": "controlled layout conflict:"}[mode]
			if !strings.Contains(string(output), want) || !strings.Contains(string(output), "shm=") {
				t.Fatalf("missing failure evidence: %s", output)
			}
		})
	}
}
func TestParallelIsolation(t *testing.T) {
	for i := 0; i < 2; i++ {
		t.Run(fmt.Sprint(i), func(t *testing.T) { t.Parallel(); TestDirectPlaneRoundTrip(t) })
	}
}

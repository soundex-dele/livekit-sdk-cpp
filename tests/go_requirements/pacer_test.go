package pacer

// Injected through go test -overlay; the Go checkout is not edited.
import (
    "encoding/json"
    "fmt"
    "testing"
    "time"

    "github.com/livekit/protocol/logger"
    "github.com/pion/rtp"
)

type uavBlockedWriter struct { entered, release, returned chan struct{} }
func (w *uavBlockedWriter) WriteRTP(h *rtp.Header, p []byte) (int, error) {
    close(w.entered)
    <-w.release
    close(w.returned)
    return len(p), nil
}
func (w *uavBlockedWriter) Write(p []byte) (int, error) { return len(p), nil }

// Characterization, not an assertion that Q-06 is satisfied. A blocked writer
// deterministically reproduces growth beyond the deque's initial 512 slots.
func TestUAVNoQueueEvidence(t *testing.T) {
    w := &uavBlockedWriter{make(chan struct{}), make(chan struct{}), make(chan struct{})}
    p := NewNoQueue(logger.GetLogger(), nil)
    defer func() { p.Stop(); close(w.release); <-w.returned }()
    p.Enqueue(&Packet{Header: &rtp.Header{Version: 2}, Payload: []byte{1}, WriteStream: w})
    select { case <-w.entered: case <-time.After(time.Second): t.Fatal("writer did not enter") }
    for i := 0; i < 4096; i++ { p.Enqueue(&Packet{Payload: make([]byte, 1200)}) }
    p.lock.Lock()
    queued := p.packets.Len()
    // Release deliberately unsent probe packets without calling their writers.
    p.packets.Clear()
    p.lock.Unlock()
    data, _ := json.Marshal(map[string]any{"id":"noqueue_growth", "queued_packets":queued,
        "queued_payload_bytes":queued*1200, "initial_capacity":512})
    fmt.Println("UAV_EVIDENCE " + string(data))
    if queued != 4096 { t.Fatal("implementation changed: review Q-06 characterization before reusing conclusion") }
}

type uavCountingWriter struct { count int }
func (w *uavCountingWriter) WriteRTP(h *rtp.Header, p []byte) (int, error) { w.count++; return len(p), nil }
func (w *uavCountingWriter) Write(p []byte) (int, error) { return len(p), nil }
func TestUAVPassThroughEvidence(t *testing.T) {
    w := &uavCountingWriter{}
    p := NewPassThrough(logger.GetLogger(), nil)
    p.SetBitrate(8000)
    start := time.Now()
    for i := 0; i < 10; i++ { p.Enqueue(&Packet{Header:&rtp.Header{Version:2}, Payload:make([]byte, 1000), WriteStream:w}) }
    data, _ := json.Marshal(map[string]any{"id":"passthrough_burst", "packets_sent":w.count,
        "target_bps":8000, "payload_bytes":10000, "elapsed_ms":time.Since(start).Milliseconds()})
    fmt.Println("UAV_EVIDENCE " + string(data))
    if w.count != 10 { t.Fatal("synchronous sender changed: review characterization") }
}

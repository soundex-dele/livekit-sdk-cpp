package config

import (
    "encoding/json"
    "fmt"
    "strings"
    "testing"

    "github.com/livekit/protocol/codecs"
)

func TestUAVDefaultsEvidence(t *testing.T) {
    videoFEC := false
    videoCodecs := []string{}
    for _, c := range codecs.VideoCodecsParameters {
        name := strings.ToLower(c.MimeType)
        videoCodecs = append(videoCodecs, name)
        videoFEC = videoFEC || strings.Contains(name, "fec") || name == "video/red"
    }
    data, _ := json.Marshal(map[string]any{"id":"defaults", "video_fec_registered":videoFEC,
        "video_codecs":videoCodecs, "send_side_bwe":DefaultConfig.RTC.CongestionControl.UseSendSideBWE,
        "node_stats_interval_ms":DefaultConfig.NodeStats.StatsUpdateInterval.Milliseconds()})
    fmt.Println("UAV_EVIDENCE " + string(data))
}

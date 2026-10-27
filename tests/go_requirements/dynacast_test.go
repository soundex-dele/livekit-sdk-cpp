package dynacast

import (
    "sync"
    "testing"
    "time"

    "github.com/livekit/protocol/codecs/mime"
    "github.com/livekit/protocol/livekit"
    "github.com/stretchr/testify/require"
)

func TestUAVH265H264Regression(t *testing.T) {
    var mu sync.Mutex
    enabled := map[string]bool{}
    dm := NewDynacastManagerVideo(DynacastManagerVideoParams{
        Listener: &testDynacastManagerListener{
            onSubscribedMaxQualityChange: func(codecs []*livekit.SubscribedCodec) {
                mu.Lock(); defer mu.Unlock()
                enabled = map[string]bool{}
                for _, c := range codecs { for _, q := range c.Qualities { enabled[c.Codec] = enabled[c.Codec] || q.Enabled } }
            },
        },
    })
    defer dm.Close()
    state := func(h265, h264 bool) bool {
        mu.Lock(); defer mu.Unlock()
        return enabled[mime.MimeTypeH265.String()] == h265 && enabled[mime.MimeTypeH264.String()] == h264
    }
    dm.NotifySubscriberMaxQuality("existing-h265-viewer", mime.MimeTypeH265, livekit.VideoQuality_HIGH)
    require.Eventually(t, func() bool { return state(true, false) }, 5*time.Second, 10*time.Millisecond)
    dm.HandleCodecRegression(mime.MimeTypeH265, mime.MimeTypeH264)
    require.Eventually(t, func() bool { return state(false, true) }, 5*time.Second, 10*time.Millisecond)
    dm.NotifySubscriberMaxQuality("late-h264-viewer", mime.MimeTypeH264, livekit.VideoQuality_HIGH)
    dm.NotifySubscriberMaxQuality("existing-h265-viewer", mime.MimeTypeH265, livekit.VideoQuality_OFF)
    require.Eventually(t, func() bool { return state(false, true) }, 5*time.Second, 10*time.Millisecond)
    // Only the aggregate control state is tested, not publisher switching or decoding.
}

package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"github.com/livekit/protocol/livekit"
	lksdk "github.com/livekit/server-sdk-go/v2"
	"github.com/livekit/server-sdk-go/v2/datatrack"
	"github.com/livekit/server-sdk-go/v2/e2ee"
	"github.com/pion/rtp/codecs"
	"github.com/pion/webrtc/v4"
	pmedia "github.com/pion/webrtc/v4/pkg/media"
	"github.com/pion/webrtc/v4/pkg/media/samplebuilder"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type cryptoVector struct {
	Codec  string `json:"codec"`
	Plain  string `json:"plain"`
	Sealed string `json:"sealed"`
}

func cryptoVectors(dir string, verify bool) error {
	key := e2ee.NewExternalKeyProvider()
	if err := key.SetRawKey(make([]byte, 16), 0); err != nil {
		return err
	}
	codecs := map[string]lksdk.Codec{"opus": lksdk.CodecOpus, "h264": lksdk.CodecH264, "h265": lksdk.CodecH265}
	vectors := []cryptoVector{{Codec: "opus", Plain: "f8010203000001"}, {Codec: "h264", Plain: "0000000165880102000001"}, {Codec: "h265", Plain: "0000000126010102000001"}}
	if verify {
		data, err := os.ReadFile(filepath.Join(dir, "cpp-vectors.json"))
		if err != nil {
			return err
		}
		if err = json.Unmarshal(data, &vectors); err != nil {
			return err
		}
	}
	for i, v := range vectors {
		plain, err := hex.DecodeString(v.Plain)
		if err != nil {
			return err
		}
		if verify {
			sealed, err := hex.DecodeString(v.Sealed)
			if err != nil {
				return err
			}
			dec, err := lksdk.NewFrameDecryptor(key, codecs[v.Codec], nil)
			if err != nil {
				return err
			}
			actual, err := dec.DecryptFrame(sealed)
			if err != nil || !bytes.Equal(actual, plain) {
				return fmt.Errorf("C++ %s encryption incompatible: %v", v.Codec, err)
			}
		} else {
			enc, err := lksdk.NewFrameEncryptor(key, codecs[v.Codec])
			if err != nil {
				return err
			}
			sealed, err := enc.EncryptFrame(plain)
			if err != nil {
				return err
			}
			vectors[i].Sealed = hex.EncodeToString(sealed)
		}
	}
	if !verify {
		data, err := json.Marshal(vectors)
		if err != nil {
			return err
		}
		return os.WriteFile(filepath.Join(dir, "go-vectors.json"), data, 0600)
	}
	return nil
}
func extensionsRun(url, dir, mode string) error {
	if err := cryptoVectors(dir, false); err != nil {
		return err
	}

	mediaMode := mode == "encoded" || mode == "media-e2ee"
	key := e2ee.NewExternalKeyProvider()
	if err := key.SetRawKey(make([]byte, 16), 0); err != nil {
		return err
	}
	var videoEcho *lksdk.LocalTrack
	var room *lksdk.Room
	opts := []lksdk.ConnectOption{}
	if mode == "encrypted" || mode == "media-e2ee" {
		opts = append(opts, lksdk.WithDataEncryption(&lksdk.EncryptionOptions{KeyProvider: key}))
	}
	if mode == "backup" || mediaMode {
		opts = append(opts, lksdk.WithCodecs([]livekit.Codec{{Mime: "video/h264"}, {Mime: "audio/opus"}}))
	}
	if mode == "single" {
		opts = append(opts, lksdk.WithSinglePeerConnection())
	}
	failures := make(chan error, 4)
	var echoMu sync.Mutex
	var echo *datatrack.Frame
	cb := lksdk.NewRoomCallback()
	cb.OnTrackSubscribed = func(track *webrtc.TrackRemote, pub *lksdk.RemoteTrackPublication, p *lksdk.RemoteParticipant) {
		if p.Identity() != "cpp-peer" {
			return
		}
		go func() {
			if (mode == "backup" || mediaMode) && track.Codec().MimeType != "video/H264" {
				failures <- fmt.Errorf("backup negotiated %s", track.Codec().MimeType)
				return
			}
			if mode != "backup" && !mediaMode {
				for i := 0; i < 20; i++ {
					if _, _, err := track.ReadRTP(); err != nil {
						failures <- err
						return
					}
				}
				os.WriteFile(filepath.Join(dir, "go-video"), []byte("received"), 0600)
				return
			}
			p.WritePLI(track.SSRC())
			builder := samplebuilder.New(50, &codecs.H264Packet{}, 90000)
			file, err := os.Create(filepath.Join(dir, "go-video.h264"))
			if err != nil {
				failures <- err
				return
			}
			defer file.Close()
			count := 0
			started := false
			for {
				packet, _, err := track.ReadRTP()
				if err != nil {
					failures <- err
					return
				}
				builder.Push(packet)
				for sample := builder.Pop(); sample != nil; sample = builder.Pop() {
					if mode == "media-e2ee" {
						dec, err := lksdk.NewFrameDecryptor(key, lksdk.CodecH264, room.SifTrailer())
						if err != nil {
							failures <- err
							return
						}
						plain, err := dec.DecryptFrame(sample.Data)
						if err != nil {
							os.WriteFile(filepath.Join(dir, "failed-encrypted-frame.bin"), sample.Data, 0600)
							failures <- err
							return
						}
						if plain == nil {
							continue
						}
						sample.Data = plain
					}
					if !started {
						for i := 0; i+4 < len(sample.Data); i++ {
							if sample.Data[i] == 0 && sample.Data[i+1] == 0 && sample.Data[i+2] == 1 && sample.Data[i+3]&31 == 5 {
								started = true
								break
							}
						}
						if !started {
							p.WritePLI(track.SSRC())
							continue
						}
					}
					if mediaMode {
						if err := videoEcho.WriteSample(pmedia.Sample{Data: sample.Data, Duration: time.Second / 30}, nil); err != nil {
							failures <- err
							return
						}
					}
					if _, err := file.Write(sample.Data); err != nil {
						failures <- err
						return
					}
					count++
					if count == 5 {
						if err := file.Sync(); err != nil {
							failures <- err
							return
						}
						os.WriteFile(filepath.Join(dir, "go-video"), []byte("received"), 0600)
						if !mediaMode {
							return
						}
					}
				}
			}
		}()
	}

	cb.OnDataTrackPublished = func(track *datatrack.RemoteTrack, p *lksdk.RemoteParticipant) {
		if p.Identity() != "cpp-peer" {
			return
		}
		go func() {
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			stream, err := track.Subscribe(ctx)
			if err != nil {
				failures <- err
				return
			}
			defer stream.Close()
			frame, err := stream.Next(ctx)
			if err != nil {
				failures <- err
				return
			}
			if len(frame.Payload) != 40000 || frame.UserTimestamp == nil || *frame.UserTimestamp != 12345 {
				failures <- fmt.Errorf("C++ DataTrack metadata mismatch")
				return
			}
			for i, v := range frame.Payload {
				if v != byte(i*17) {
					failures <- fmt.Errorf("C++ DataTrack payload mismatch")
					return
				}
			}
			echoMu.Lock()
			echo = &frame
			echoMu.Unlock()
		}()
	}
	room, err := lksdk.ConnectToRoom(url, lksdk.ConnectInfo{APIKey: "reconnect-test", APISecret: "reconnect-integration-secret-32-characters", RoomName: "extensions", ParticipantIdentity: "go-peer"}, cb, opts...)
	if err != nil {
		return err
	}
	defer room.Disconnect()
	if mediaMode {
		trackOptions := []lksdk.LocalTrackOptions{}
		if mode == "media-e2ee" {
			enc, err := lksdk.NewFrameEncryptor(key, lksdk.CodecH264)
			if err != nil {
				return err
			}
			trackOptions = append(trackOptions, lksdk.WithFrameEncryptor(enc))
		}
		videoEcho, err = lksdk.NewLocalTrack(webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000}, trackOptions...)
		if err != nil {
			return err
		}
		publication := &lksdk.TrackPublicationOptions{Name: "go-encoded-echo"}
		if mode == "media-e2ee" {
			publication.Encryption = livekit.Encryption_GCM
		}
		if _, err = room.LocalParticipant.PublishTrack(videoEcho, publication); err != nil {
			return err
		}
	}
	localData, err := room.LocalParticipant.PublishDataTrack(context.Background(), "go-data")
	if err != nil {
		return err
	}
	if err = room.RegisterTextStreamHandler("request-text", func(reader *lksdk.TextStreamReader, identity string) {
		text := reader.ReadAll()
		if identity != "cpp-peer" || text != string(bytes.Repeat([]byte("中文🌍"), 10000)) {
			failures <- fmt.Errorf("C++ text mismatch")
			return
		}
		room.LocalParticipant.SendText(text, lksdk.StreamTextOptions{Topic: "reply-text", DestinationIdentities: []string{"cpp-peer"}})
	}); err != nil {
		return err
	}
	if err = room.RegisterByteStreamHandler("request-bytes", func(reader *lksdk.ByteStreamReader, identity string) {
		data := reader.ReadAll()
		if identity != "cpp-peer" || len(data) != 300000 {
			failures <- fmt.Errorf("C++ file length mismatch")
			return
		}
		for i, v := range data {
			if v != byte(i*31) {
				failures <- fmt.Errorf("C++ file content mismatch")
				return
			}
		}
		writer := room.LocalParticipant.StreamBytes(lksdk.StreamBytesOptions{Topic: "reply-bytes", DestinationIdentities: []string{"cpp-peer"}, TotalSize: uint64(len(data))})
		done := func() { writer.Close() }
		writer.Write(data, &done)
	}); err != nil {
		return err
	}
	if err = os.WriteFile(filepath.Join(dir, "go-ready"), []byte("ready"), 0600); err != nil {
		return err
	}
	timeout := time.After(50 * time.Second)
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case err := <-failures:
			return err
		case <-timeout:
			return fmt.Errorf("extension interop timeout")
		case <-ticker.C:
			echoMu.Lock()
			frame := echo
			echoMu.Unlock()
			if frame != nil {
				if err := localData.TryPush(*frame); err != nil {
					return err
				}
			}
			if _, err := os.Stat(filepath.Join(dir, "cpp-done")); err == nil {
				if err := cryptoVectors(dir, true); err != nil {
					return err
				}
				return os.WriteFile(filepath.Join(dir, "go-result.json"), []byte(`{"ok":true}`), 0600)
			}
		}
	}
}

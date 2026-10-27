// Test peer linked against the user's pinned Go SDK checkout, never a runtime SDK dependency.
package main

import (
 "errors"
 "encoding/json"
 "flag"
 "fmt"
 "math"
 "os"
 "path/filepath"
 "sync"
 "time"

 media "github.com/livekit/media-sdk"
 "github.com/livekit/protocol/logger"
 "github.com/livekit/protocol/livekit"
 lksdk "github.com/livekit/server-sdk-go/v2"
 lkmedia "github.com/livekit/server-sdk-go/v2/pkg/media"
 "github.com/pion/webrtc/v4"
)

type meter struct { mu sync.Mutex; started bool; previous int16; samples, crossings int; peak int; mime string; dtmf, partial, final bool }
type transcriptionPacket struct { value *livekit.Transcription }
func (p transcriptionPacket) ToProto() *livekit.DataPacket { return &livekit.DataPacket{Value:&livekit.DataPacket_Transcription{Transcription:p.value}} }
func (m *meter) Close() error { return nil }
func (m *meter) WriteSample(pcm media.PCM16Sample) error {
 m.mu.Lock(); defer m.mu.Unlock()
 if m.samples >= 96000 { return nil }
 if !m.started {
  for _, v := range pcm { if math.Abs(float64(v)) > 500 { m.started = true; break } }
  if !m.started { return nil }
 }
 for _, v := range pcm {
  if m.previous < 0 && v >= 0 { m.crossings++ }
  m.previous = v; m.samples++
  if a:=int(math.Abs(float64(v))); a>m.peak { m.peak=a }
 }
 return nil
}
func rpcChecks(room *lksdk.Room) error {
 reply,e:=room.LocalParticipant.PerformRpc(lksdk.PerformRpcParams{DestinationIdentity:"cpp-peer",Method:"cpp.echo",Payload:"hello 世界"})
 if e!=nil || reply==nil || *reply!="C++:hello 世界" {return fmt.Errorf("Go to C++ RPC echo failed: %v",e)}
 _,e=room.LocalParticipant.PerformRpc(lksdk.PerformRpcParams{DestinationIdentity:"cpp-peer",Method:"cpp.error"})
 var remote *lksdk.RpcError
 if !errors.As(e,&remote)||remote.Code!=2001||remote.Data==nil||*remote.Data!="cpp-details" {return fmt.Errorf("C++ RPC error: %v",e)}
 _,e=room.LocalParticipant.PerformRpc(lksdk.PerformRpcParams{DestinationIdentity:"cpp-peer",Method:"missing"})
 if !errors.As(e,&remote)||remote.Code!=1400 {return fmt.Errorf("C++ unknown RPC: %v",e)}
 return nil
}
func run() error {
 url:=flag.String("url", "", "local server URL"); dir:=flag.String("out", "", "result directory"); rpcOnly:=flag.Bool("rpc-only",false,"No media publication");flag.Parse()
 m:=&meter{}; failures:=make(chan error, 1)
 cb:=lksdk.NewRoomCallback()
 cb.OnDataPacket=func(packet lksdk.DataPacket, params lksdk.DataReceiveParams) {
  fmt.Fprintf(os.Stderr,"data packet %T sender=%s\n",packet,params.SenderIdentity)
  if digit,ok:=packet.(*livekit.SipDTMF);ok && params.SenderIdentity=="cpp-peer" && digit.Code==11 && digit.Digit=="#" {m.mu.Lock();m.dtmf=true;m.mu.Unlock()}
 }
 cb.OnTranscriptionReceived=func(segments []*lksdk.TranscriptionSegment,p lksdk.Participant,pub lksdk.TrackPublication) {
  fmt.Fprintf(os.Stderr,"transcription segments=%d participant=%v publication=%v\n",len(segments),p!=nil,pub!=nil)
  if p==nil || p.Identity()!="cpp-peer" || pub==nil {return}
  m.mu.Lock();defer m.mu.Unlock()
  for _,s:=range segments {if s.ID=="cpp-segment" && s.Language=="zh" && s.StartTime==123 && s.EndTime==456 {
   if s.Final && s.Text=="你好 🌍" {m.final=true} else if !s.Final && s.Text=="你好" {m.partial=true}
  }}
 }
 cb.OnTrackSubscribed=func(t *webrtc.TrackRemote, p *lksdk.RemoteTrackPublication, rp *lksdk.RemoteParticipant) {
  if rp.Identity()!="cpp-peer" || p.Name()!="cpp-440" { return }
  m.mu.Lock(); m.mime=t.Codec().MimeType; m.mu.Unlock()
  if _,err:=lkmedia.NewPCMRemoteTrack(t,m,lkmedia.WithTargetSampleRate(48000),lkmedia.WithTargetChannels(1)); err!=nil {
   select { case failures<-err: default: }
  }
 }
 room,err:=lksdk.ConnectToRoom(*url,lksdk.ConnectInfo{APIKey:"reconnect-test",APISecret:"reconnect-integration-secret-32-characters",RoomName:"go-interop",ParticipantIdentity:"go-peer",ParticipantKind:lksdk.ParticipantAgent},cb)
 if err!=nil {return err}; defer room.Disconnect()
 if err=room.RegisterRpcMethod("go.echo",func(data lksdk.RpcInvocationData)(string,error){
  if data.CallerIdentity!="cpp-peer" {return "",fmt.Errorf("unexpected caller")};return "Go:"+data.Payload,nil
 });err!=nil{return err}
 if err=room.RegisterRpcMethod("go.error",func(data lksdk.RpcInvocationData)(string,error){
  details:="go-details";return "",lksdk.NewRpcError(2002,"拒绝",&details)
 });err!=nil{return err}
 if *rpcOnly {
  if err=os.WriteFile(filepath.Join(*dir,"go-ready"),[]byte("ready"),0600);err!=nil{return err}
  deadline:=time.Now().Add(40*time.Second)
  for room.GetParticipantByIdentity("cpp-peer")==nil {if time.Now().After(deadline){return fmt.Errorf("RPC peer missing")};time.Sleep(10*time.Millisecond)}
  if err=rpcChecks(room);err!=nil{return err}
  if err=os.WriteFile(filepath.Join(*dir,"go-result.json"),[]byte(`{"ok":true,"no_media":true,"rpc_echo_error_unknown":true}`),0600);err!=nil{return err}
  for {if _,err=os.Stat(filepath.Join(*dir,"stop"));err==nil{return nil};if time.Now().After(deadline){return fmt.Errorf("RPC-only stop missing")};time.Sleep(10*time.Millisecond)}
 }
 track,err:=lkmedia.NewPCMLocalTrack(48000,1,logger.GetLogger()); if err!=nil{return err}
 defer func(){track.ClearQueue(); track.Close()}()
 publication,err:=room.LocalParticipant.PublishTrack(track,&lksdk.TrackPublicationOptions{Name:"go-880"});if err!=nil{return err}
 if err=os.WriteFile(filepath.Join(*dir,"go-ready"),[]byte("ready"),0600);err!=nil{return err}
 ticker:=time.NewTicker(20*time.Millisecond); defer ticker.Stop()
 deadline:=time.After(45*time.Second); index:=0; written,sent,rpcStarted,rpcOK:=false,false,false,false
 rpcDone:=make(chan error,1)
 for {
  select {
  case err:=<-failures: return err
  case err:=<-rpcDone: if err!=nil{return err};rpcOK=true
  case <-deadline: return fmt.Errorf("Go interop deadline exceeded")
  case <-ticker.C:
   pcm:=make(media.PCM16Sample,960)
   for i:=range pcm { pcm[i]=int16(6000*math.Sin(2*math.Pi*880*float64(index+i)/48000)) }; index+=960
   if err=track.WriteSample(pcm);err!=nil{return err}
   m.mu.Lock(); samples,crossings,peak,mime:=m.samples,m.crossings,m.peak,m.mime; dataOK:=m.dtmf&&m.partial&&m.final; m.mu.Unlock()
   if index%96000==0 {fmt.Fprintf(os.Stderr,"meter samples=%d crossings=%d peak=%d mime=%s data=%v\n",samples,crossings,peak,mime,dataOK)}
   if samples>=48000 && !rpcStarted {
    rpcStarted=true
    go func(){rpcDone<-rpcChecks(room)}()
   }
   if samples>=48000 && !sent {
    if err=room.LocalParticipant.PublishDTMF(10,"*");err!=nil{return err}
    for _,final:=range []bool{false,true} {
     text:="hello";if final{text="hello 世界"}
     tr:=&livekit.Transcription{TranscribedParticipantIdentity:"go-peer",TrackId:publication.SID(),Segments:[]*livekit.TranscriptionSegment{{Id:"go-segment",Text:text,StartTime:123,EndTime:456,Final:final,Language:"en"}}}
     if err=room.LocalParticipant.PublishDataPacket(transcriptionPacket{tr},lksdk.WithDataPublishReliable(true));err!=nil{return err}
    };sent=true
   }
   if samples>=96000 && dataOK && rpcOK && !written {
    hz:=float64(crossings)*48000/float64(samples)
    if math.Abs(hz-440)>22 || peak<500 || mime!=webrtc.MimeTypeOpus {return fmt.Errorf("C++ to Go invalid audio: %.2f Hz peak=%d mime=%s",hz,peak,mime)}
    data,_:=json.Marshal(map[string]any{"ok":true,"received_hz":hz,"samples":samples,"peak":peak,"mime":mime,"dtmf":true,"partial_final_transcription":true,"rpc_echo_error_unknown":true})
    if err=os.WriteFile(filepath.Join(*dir,"go-result.json"),data,0600);err!=nil{return err}; written=true
   }
   if written {if _,err=os.Stat(filepath.Join(*dir,"stop"));err==nil{return nil}}
  }
 }
}
func main(){if err:=run();err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}}

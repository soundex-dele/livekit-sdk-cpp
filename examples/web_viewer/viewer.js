'use strict';
const el = (id) => document.getElementById(id);
const log = (message) => {
  el('log').textContent = `${new Date().toLocaleTimeString()} ${message}\n${el('log').textContent}`.slice(0, 16000);
};
let session, room;
let initialized = false;
const tracks = new Map();
function clearTracks() {
  for (const { track } of tracks.values()) track.detach().forEach((element) => element.remove());
  tracks.clear(); el('tracks').replaceChildren(); el('empty').hidden = false;
  el('stats').textContent = '暂无接收数据';
}
function removeTrack(track) {
  track.detach().forEach((element) => element.remove());
  tracks.get(track.sid)?.card.remove(); tracks.delete(track.sid);
  el('empty').hidden = tracks.size > 0;
}
async function loadSession() {
  const response = await fetch('/session.json');
  session = await response.json();
  if (!response.ok) throw new Error(session.error);
  el('session').textContent = `房间：${session.room}`;
  if (!initialized) { el('server-url').value = session.url; initialized = true; }
  el('connect').disabled = false;
}
el('connect').onclick = async () => {
  el('connect').disabled = true; el('status').textContent = '正在连接…';
  try {
    await room?.disconnect(); clearTracks();
    await loadSession(); el('connect').disabled = true;
    const active = room = new LivekitClient.Room({ adaptiveStream: true, dynacast: false });
    active.on(LivekitClient.RoomEvent.ConnectionStateChanged, (state) => log(`连接状态：${state}`));
    active.on(LivekitClient.RoomEvent.TrackSubscriptionFailed, (sid, participant, reason) => log(`订阅失败：${sid} ${reason || ''}`));
    active.on(LivekitClient.RoomEvent.TrackSubscribed, (track, publication, participant) => {
      const card = document.createElement('div'); card.className = 'track';
      const title = document.createElement('div'); title.textContent = `${participant.identity} · ${track.kind} · ${publication.trackSid}`;
      const media = track.attach(); media.controls = true;
      if (track.kind === 'video') { media.muted = true; media.playsInline = true; }
      card.append(title, media); el('tracks').append(card);
      tracks.set(track.sid, { track, card }); el('empty').hidden = true;
      media.play().catch(() => log('浏览器暂停了自动播放，请点击“启用声音”或播放器播放按钮。'));
      log(`已订阅 ${participant.identity} 的 ${track.kind} 轨道`);
    });
    active.on(LivekitClient.RoomEvent.TrackUnsubscribed, removeTrack);
    active.on(LivekitClient.RoomEvent.Disconnected, (reason) => {
      if (room !== active) return;
      clearTracks(); el('status').textContent = '已断开'; el('connect').disabled = false;
      el('disconnect').disabled = true; el('audio').disabled = true; log(`已断开：${reason}`);
    });
    await active.connect(el('server-url').value.trim(), session.token, { autoSubscribe: true });
    el('status').textContent = '已连接'; el('disconnect').disabled = false; el('audio').disabled = false;
    await active.startAudio().catch((error) => log(`声音未启动：${error.message}`));
    log(`已加入房间 ${active.name}`);
  } catch (error) {
    el('status').textContent = '连接失败'; el('connect').disabled = false;
    log(`${error.name}: ${error.message}`);
    log('若出现 could not establish pc connection，请检查服务器公告的媒体 IP、UDP/TCP 端口和防火墙。');
  }
};
el('disconnect').onclick = () => room?.disconnect();
el('audio').onclick = async () => {
  try { await room?.startAudio(); for (const media of document.querySelectorAll('video,audio')) await media.play(); }
  catch (error) { log(error.message); }
};
setInterval(async () => {
  const lines = [];
  for (const { track } of tracks.values()) {
    const receiver = track.receiver;
    if (!receiver) continue;
    try {
      const report = await receiver.getStats();
      report.forEach((stat) => {
        if (stat.type === 'inbound-rtp') {
          lines.push(`${track.kind}: 接收 ${stat.bytesReceived ?? 0} 字节，丢包 ${stat.packetsLost ?? 0}${stat.kind === 'video' ? `，解码 ${stat.framesDecoded ?? 0} 帧` : ''}`);
        }
        if (stat.type === 'transport' && stat.selectedCandidatePairId) {
          const pair = report.get(stat.selectedCandidatePairId);
          const remote = pair && report.get(pair.remoteCandidateId);
          if (remote) lines.push(`媒体路径：${remote.protocol} ${remote.address || '(隐藏地址)'}:${remote.port} (${remote.candidateType})`);
        }
      });
    } catch (error) { lines.push(error.message); }
  }
  el('stats').textContent = lines.join('\n') || '暂无接收数据';
}, 1000);
loadSession().catch((error) => { el('session').textContent = '加载失败'; log(error.message); });

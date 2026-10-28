"""Evidence-based SFU simulation verdicts; device availability is not a gate."""

# These are still software/simulation obligations, not requests for real devices.
REMAINING = {
    'V-01': '模拟两个摄像头重连、重排后稳定业务标识与画面不串源。',
    'V-02': '', 'V-08': '',
    'V-11': '模拟多设备并发观看、独立授权和来源标识；C++ 单房间不能证明多机业务。',
    'V-12': '实现并模拟首位能力选择、全体 H.265→H.264 回退及回退状态恢复。',
    'V-13': '普通 UDP 连接、拒绝 TCP 与恢复已通过；回退编码时的组合验证依赖 V-12。',
    'A-01': '六条有向音频解码已通过；补软件混音和模拟回声输入的 AEC 验证。',
    'A-02': '正常多人加入/退出已通过；补模拟撤权、异常掉线及容量拒绝。',
    'B-01': '补本地生成的录像文件源经 TCP 发布和解码；合成 I420 还不是文件回放。',
    'B-03': '', 'B-04': '',
    'B-07': '本轮 TCP 正反向及恢复通过；历史间歇连接失败根因未关闭，慢端阻塞仍需模拟。',
    'N-02': '模拟上/下行限速，检查音频和主辅画面降级顺序。',
    'Q-01': '模拟带宽收缩，记录 BWE、软件编码器目标/实际码率和单订阅降级。',
    'Q-02': '组件参数/次数/寿命已测；补 C++ 媒体上下行丢包、RTX 恢复及总预算。',
    'Q-03': '组件与 IDR 已测；补模拟关键帧丢失、多人同源 PLI 合并限频及解码恢复。',
    'Q-05': '独立订阅操作已通过；补单观看者弱网下选层/暂停、其他观看者连续解码。',
    'Q-06': '实现并模拟实际 pacing、队列硬边界、完整帧依赖恢复及 TCP 写入截止。',
    'Q-07': '实现视频 FEC；模拟丢包，验证协商、生成和实际恢复，不能以 RTX 替代。',
    'Q-08': '补业务 ID、冻结/音频中断、TCP 积压及重连统计基线的模拟验证。',
    'N-01': '以模拟源时间码到解码输出时间测量各链路 P95；保留 500/300 ms 阈值。',
    'N-06': '重复模拟冷热加入，测首个解码帧/双向 PCM 的 P95；保留 2/3 s 阈值。',
    'P-02': '按 H.265 720p30/2 Mbps 和 Opus 32 kbps 模拟源阶梯加压。',
    'P-03': '固定模拟源数逐增订阅，分别测 UDP/TCP/混合及回退峰值。',
    'P-10': '候选最高模拟负载实际连续运行 8 h；不能把短测折算为 8 h。',
    'P-11': '实现并模拟动态源/订阅/连接/缓存配额、并发准入及释放。',
    'V-04': '停流后另一视频已通过；补同时有语音时故障隔离及明确流状态。',
    'A-05': '正常退出保留视频/其他语音已通过；补授权失效、异常清理和旧成员恢复拒绝。',
    'A-06': '旧 handle/权限版本已测；补重复加入及旧代次迟到撤权模拟。',
    'B-06': '撤轨/新 SID 重发已通过；补回放 EOS/异常状态及稳定逻辑流映射。',
    'N-04': '信令恢复/重建已通过；补媒体黑洞恢复和过期语音授权拒绝模拟。',
    'N-05': '',
    'N-08': '模拟客户端和服务端实际运行 8 h，检查崩溃与资源增长；温度/功耗不纳入本轮。',
    'N-09': '补设备/摄像头/回放会话/代次关联及软件质量查询证据。',
}

PARTIAL = {'V-01', 'V-13', 'A-01', 'A-02', 'B-01', 'Q-01', 'Q-02', 'Q-03',
           'Q-05', 'V-04', 'A-05', 'A-06', 'B-06', 'N-04'}


def simulation_rows(rows, scenarios, recovery, gaps, component_evidence=False):
    def case(protocol, kind, codec, *checks):
        matches = [r for r in scenarios if r.get('protocol') == protocol
                   and r.get('scenario') == kind and r.get('codec') == codec]
        # Do not erase a failed repeated round by finding one successful run.
        return bool(matches) and all(r.get('ok') and set(checks) <=
            {c.get('id') for c in r.get('checks', []) if c.get('status') == 'PASS'} for r in matches)

    both_udp = all(case('udp', 'video', codec, 'two_sources_two_viewers',
        'independent_unsubscribe_resubscribe', 'selected_ice_protocol') for codec in ('h264', 'h265'))
    both_tcp = all(case('tcp', 'video', codec, 'two_sources_two_viewers',
        'independent_unsubscribe_resubscribe', 'unpublish_isolation', 'republish_new_sid',
        'selected_ice_protocol') for codec in ('h264', 'h265'))
    mixed = case('udp', 'mixed', 'h265', 'three_way_audio_six_links',
                 'video_continues_after_concurrent_audio_and_voice_leave', 'selected_ice_protocol')
    restart = all(any(r.get('scenario') == 'restart' and r.get('transport') == transport
        and r.get('verified') is True and r.get('exit_code') == 0 for r in recovery) for transport in ('udp', 'tcp'))
    passed = {
        'V-02': (both_udp, 'H.264/H.265 两源两观看者实际解码，订阅独立且未按观看者重复发布。'),
        'V-08': (all(case('udp', 'dynacast', c, 'aggregate_demand_stops_source_and_rtp',
                         'selected_ice_protocol') for c in ('h264', 'h265')),
                 '0→1→2→1→0→1 模拟需求聚合，源消费和 RTP 停止/恢复。'),
        'B-03': (both_tcp and mixed and any(r.get('folder', '').replace('\\', '/').startswith('simultaneous/')
                     and r.get('protocol') == 'tcp' and r.get('ok') for r in scenarios),
                 '两条不同内容的 TCP 源模拟不同回放会话，独立撤轨/重发；与 UDP 视频/语音并行。外部 seek 实现不属于 SFU。'),
        'B-04': (both_tcp, '每条模拟 TCP 源由两观看者共享，发布者没有按观看者重复上传；慢端队列另由 Q-06 判定。'),
        'N-05': (restart, '终止/重启 Go 节点后，C++ 重新鉴权入房、恢复媒体；新 SID、稳定源标识、旧 handle 拒绝、撤下轨道不复活。'),
    }
    result = []
    for row in rows:
        item = dict(row)
        rid = row['id']
        item['remaining'] = REMAINING[rid]
        item['status'] = '未通过' if rid in gaps else '未测'
        item['coverage'] = '尚未模拟'
        if rid in gaps:
            item['coverage'] = '发现缺口或未关闭失败'
            item['reason'] = gaps[rid]
        elif passed.get(rid, (False,))[0]:
            item['status'] = '通过'
            item['coverage'] = '模拟通过'
            item['reason'] = passed[rid][1]
            if rid == 'N-05':
                item['evidence'] = [r['log'] for r in recovery if r.get('scenario') == 'restart' and r.get('verified')]
        elif rid in PARTIAL and (item.get('evidence') or component_evidence or recovery):
            item['coverage'] = '部分通过，待补模拟'
            item['reason'] = item['remaining']
        else:
            item['reason'] = item['remaining'] or '所需模拟证据不完整，不能判为通过。'
        result.append(item)
    return result

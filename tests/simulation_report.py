"""Reclassify preserved measurements without rerunning or modifying them."""
import argparse
from collections import Counter
import json
from pathlib import Path

from uav_requirements import digest, finish


def verified_recovery(summary, media):
    result = []
    for row in json.loads(summary.read_text(encoding='utf-8')):
        item = dict(row)
        item['verified'] = False
        if row['exit_code'] == 0:
            log = Path(row['log'])
            lines = log.read_text(encoding='utf-8').splitlines()
            evidence = None
            for line in lines:
                if line.startswith('{'):
                    evidence, _ = json.JSONDecoder().raw_decode(line)
            if evidence is None:
                raise ValueError(f'No recovery evidence: {log}')
            command = row['command']
            client = Path(command[command.index('--client') + 1])
            assert evidence['ok'] is True
            assert evidence['serverSHA256'] == media['sha256']['server']
            assert evidence['clientSHA256'] == digest(client)
            assert evidence['mediaTransport'] == row['transport']
            pairs = evidence['selectedPairs']
            assert pairs and all(p['localProtocol'] == p['remoteProtocol'] == row['transport']
                and p['localCandidateType'] in ('host', 'srflx', 'prflx')
                and p['remoteCandidateType'] in ('host', 'srflx', 'prflx') for p in pairs)
            if row['scenario'] == 'restart':
                assert evidence['rejoins'] == 2 and evidence['frames'] >= 45
                assert all(evidence[k] is True for k in ('stableSourceIds', 'staleHandlesRejected',
                                                        'removedTrackNotRepublished', 'terminalLeave'))
                assert len({p['sid'] for p in pairs if p['participant'] == 'publisher'}) >= 3
            item.update(verified=True, evidence=evidence, log_sha256=digest(log))
        result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--media-report', type=Path, required=True)
    parser.add_argument('--recovery-summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--component-results', type=Path, action='append', default=[])
    args = parser.parse_args()
    report = json.loads(args.media_report.read_text(encoding='utf-8'))
    report['recovery'] = verified_recovery(args.recovery_summary, report)
    report['evidence_reuse'] = {'media_report': str(args.media_report.resolve()),
        'sha256': digest(args.media_report), 'note': '仅改变模拟验收口径；不修改或伪装重新运行原始测量。'}
    report['component_evidence'] = []
    for path in args.component_results:
        events = [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines() if line.startswith('{')]
        passes = [e for e in events if e.get('Action') == 'pass' and e.get('Test')]
        assert passes and not any(e.get('Action') == 'fail' for e in events), path
        report['component_evidence'].append({'path': str(path.resolve()), 'sha256': digest(path), 'passed': len(passes)})
    report['open_issues'] = {'B-07': '本轮 TCP 模拟成功；历史间歇 transport failed 根因仍未关闭，未用成功重试删除失败记录。'}
    args.output.mkdir(parents=True, exist_ok=False)
    finish(report, args.output)
    print(dict(Counter(row['coverage'] for row in report['requirements'])))


if __name__ == '__main__':
    main()

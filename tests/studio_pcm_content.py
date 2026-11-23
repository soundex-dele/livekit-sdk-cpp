"""Per-slot PCM evidence for the single-remote, unity-gain Studio capacity fixture."""
from studio_pcm_timing import load_rows


def inspect_content(case, m):
    mix = load_rows(case/'pcm-mix-content.csv', 'slot,supplied,remote,program')
    encoded = load_rows(case/'pcm-encoder-content.csv', 'slot,encoder')
    writes = load_rows(case/'pcm-write.csv', 'slot,mixed,enter,leave,accepted')
    refills = load_rows(case/'pcm-refill.csv', 'attempt,acquired,before,after')
    slots = [r[0] for r in mix]
    if (not slots or slots != [r[0] for r in writes] or len(slots) != m['received']['studio.audio-output']
            or any(b-a != 10_000_000 for a, b in zip(slots, slots[1:]))):
        raise ValueError('source/mix/program slot counts or timestamps differ')
    if (not refills or any(a > b or not 0 <= lo <= hi == 9600 for a, b, lo, hi in refills) or
            any(a[1] > b[0] for a, b in zip(refills, refills[1:]))):
        raise ValueError('invalid bounded fixture refill history')
    supplied = sum(r[1] for r in mix)
    if 9600 + sum(hi-lo for _, _, lo, hi in refills) != supplied + m['fixtureFifoFinalFloats']:
        raise ValueError('fixture FIFO contents do not reconcile with real reads')
    # These probes request recording before either owner starts. There should be no
    # missing, duplicated, reordered or out-of-epoch calls to the real encoder.
    if slots[0] < m['recordEpochNs'] or slots != [r[0] for r in encoded]:
        raise ValueError('recording admission/encoder slot sequence differs from the input')
    gaps = []
    for (slot, size, remote, program), (_, encoder) in zip(mix, encoded):
        if size == 0:
            if remote != 0:
                raise ValueError(f'{slot}: empty fixture FIFO produced nonzero remote input')
            gaps.append(slot)
        elif size != 960 or remote != 1:
            raise ValueError(f'{slot}: supplied input is not a complete stereo fixture tone')
        if program != remote:
            raise ValueError(f'{slot}: mixer changed the remote fixture input')
        if encoder != program:
            raise ValueError(f'{slot}: encoder input differs from the program mix')
    return {'checkedSlots': len(slots), 'emptyInputSlotsNs': gaps, 'suppliedFloats': supplied,
            'maxRefillGapMs': max((b[1]-a[1] for a, b in zip(refills, refills[1:])), default=0)/1e6,
            'maxRefillLockWaitMs': max(b-a for a, b, _, _ in refills)/1e6}

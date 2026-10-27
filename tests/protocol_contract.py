"""Compare the compiled subset to pinned upstream descriptors (stdlib only).

Does not import Python protobuf or access the network. protoc validates syntax,
types, maps and imports; this script compares descriptor identity and presence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise ValueError('invalid descriptor varint')


def fields(data):
    result = {}
    offset = 0
    while offset < len(data):
        key, offset = varint(data, offset)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, offset = varint(data, offset)
        elif wire == 2:
            size, offset = varint(data, offset)
            value = data[offset:offset + size]
            if len(value) != size:
                raise ValueError('truncated descriptor')
            offset += size
        elif wire in (1, 5):
            size = 8 if wire == 1 else 4
            value = data[offset:offset + size]
            offset += size
        else:
            raise ValueError(f'unsupported descriptor wire type {wire}')
        result.setdefault(number, []).append(value)
    return result


def first(message, number, default=None):
    return message.get(number, [default])[0]


def definitions(blob):
    messages, enums = {}, {}

    def enum(raw, prefix):
        item = fields(raw)
        name = prefix + first(item, 1).decode()
        enums[name] = {first(v, 1).decode(): first(v, 2)
                       for v in map(fields, item.get(2, []))}

    def message(raw, prefix):
        item = fields(raw)
        name = prefix + first(item, 1).decode()
        oneofs = [first(fields(v), 1).decode() for v in item.get(8, [])]
        members = {}
        for f in map(fields, item.get(2, [])):
            members[first(f, 1).decode()] = {
                'number': first(f, 3), 'label': first(f, 4),
                'type': first(f, 5), 'type_name': first(f, 6, b'').decode(),
                'oneof': oneofs[first(f, 9)] if 9 in f else None,
                'optional': first(f, 17, 0),
            }
        messages[name] = members
        for nested in item.get(3, []):
            message(nested, name + '.')
        for nested in item.get(4, []):
            enum(nested, name + '.')

    for file in map(fields, fields(blob).get(1, [])):
        prefix = '.' + first(file, 2, b'').decode() + '.'
        for raw in file.get(4, []):
            message(raw, prefix)
        for raw in file.get(5, []):
            enum(raw, prefix)
    return messages, enums


# Legacy public API exposes these enums as int. Both have identical varint wire
# representation. Keep this list explicit: new mismatches must be reviewed.
LEGACY_ENUM_FIELDS = {
    '.livekit.ParticipantInfo.state', '.livekit.ParticipantInfo.kind',
    '.livekit.ParticipantInfo.disconnect_reason', '.livekit.VideoLayer.quality',
    '.livekit.LeaveRequest.reason', '.livekit.LeaveRequest.action',
    '.livekit.SubscriptionResponse.err', '.livekit.DataPacket.kind',
    '.livekit.UpdateTrackSettings.quality', '.livekit.ConnectionQualityInfo.quality',
}


def compare(subset, upstream):
    messages, enums = subset
    reference, reference_enums = upstream
    errors = []
    count = 0
    for name, members in messages.items():
        for field, actual in members.items():
            count += 1
            qualified = name + '.' + field
            expected = reference.get(name, {}).get(field)
            if expected is None:
                errors.append(f'{qualified}: absent upstream')
                continue
            expected = dict(expected)
            if qualified in LEGACY_ENUM_FIELDS and actual['type'] == 5 and expected['type'] == 14:
                expected.update(type=5, type_name='')
            if actual != expected:
                errors.append(f'{qualified}: {actual} != {expected}')
    for name, values in enums.items():
        for key, value in values.items():
            if reference_enums.get(name, {}).get(key) != value:
                errors.append(f'{name}.{key}: enum value changed')
    return errors, count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--protoc', required=True)
    parser.add_argument('--protobuf-includes', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / 'protocol'
    vendor = root / 'upstream'
    manifest = json.loads((vendor / 'manifest.json').read_text())
    for name, digest in manifest['files'].items():
        if hashlib.sha256((vendor / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'pinned upstream hash mismatch: {name}')
    with tempfile.TemporaryDirectory() as directory:
        descriptors = []
        for base, inputs in [(root, ['livekit.proto']), (vendor, ['livekit_rtc.proto'])]:
            output = Path(directory) / f'{len(descriptors)}.pb'
            subprocess.run([args.protoc, f'-I{base}', f'-I{args.protobuf_includes}',
                            '--include_imports', f'--descriptor_set_out={output}', *inputs], check=True)
            descriptors.append(definitions(output.read_bytes()))
        errors, count = compare(*descriptors)
        if errors:
            raise RuntimeError('\n'.join(errors))
        # Verify the checker detects changes instead of accepting any descriptor.
        mutated = json.loads(json.dumps(descriptors[0][0]))
        mutated['.livekit.SignalRequest']['offer']['number'] = 999
        if not compare((mutated, descriptors[0][1]), descriptors[1])[0]:
            raise AssertionError('descriptor drift check did not reject changed tag')
        print(json.dumps({'ok': True, 'fields': count, 'upstream': manifest['revision']}))


if __name__ == '__main__':
    main()

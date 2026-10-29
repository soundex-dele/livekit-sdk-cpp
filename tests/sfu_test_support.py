"""Shared loopback SFU fixture primitives."""
import base64
import hashlib
import hmac
import json
import socket
import time

def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wire_bytes(number, data):
    def varint(value):
        result = bytearray()
        while value >= 128:
            result.append((value & 127) | 128)
            value >>= 7
        result.append(value)
        return bytes(result)
    return varint(number * 8 + 2) + varint(len(data)) + data


def wire_uint(number, value):
    result = bytearray()
    for item in (number * 8, value):
        while item >= 128:
            result.append((item & 127) | 128)
            item >>= 7
        result.append(item)
    return bytes(result)


def readiness_token():
    def encode(value):
        return base64.urlsafe_b64encode(value).rstrip(b'=')
    header = encode(b'{"alg":"HS256","typ":"JWT"}')
    payload = encode(json.dumps({'iss': 'reconnect-test', 'sub': 'readiness', 'exp': int(time.time()) + 60,
        'video': {'roomJoin': True, 'room': 'reconnect-readiness'}}).encode())
    unsigned = header + b'.' + payload
    signature = encode(hmac.new(b'reconnect-integration-secret-32-characters', unsigned, hashlib.sha256).digest())
    return (unsigned + b'.' + signature).decode()

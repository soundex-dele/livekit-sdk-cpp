"""Verify the Go server-API HTTP contract without provisioning external services."""
import argparse, base64, hashlib, hmac, json, os, subprocess, threading, time, uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
p = argparse.ArgumentParser(); p.add_argument('--client', required=True); p.add_argument('--cli'); args = p.parse_args()
requests = []
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append((self.path, dict(self.headers), body))
        status, result = 200, {}
        if body.get('identity') == 'error': status, result = 404, {'code':'not_found','msg':'missing peer','meta':{'detail':'test'}}
        if body.get('identity') == 'slow': time.sleep(2)
        data = json.dumps(result).encode()
        try:
            self.send_response(status); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError): pass
server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
url = f'http://127.0.0.1:{server.server_port}'
env = dict(os.environ, LIVEKIT_URL=url, LIVEKIT_TOKEN='ambient-token', LIVEKIT_API_KEY='ambient-key', LIVEKIT_API_SECRET='ambient-secret')
def decode(token):
    head, payload, signature = token.split('.')
    expected = base64.urlsafe_b64encode(hmac.new(b'contract-secret',f'{head}.{payload}'.encode(),hashlib.sha256).digest()).decode().rstrip('=')
    assert hmac.compare_digest(signature, expected), 'JWT signature'
    return json.loads(base64.urlsafe_b64decode(payload+'='*(-len(payload)%4)))
try:
    proc = subprocess.run([str(Path(args.client).resolve()),url],env=env,capture_output=True,text=True,timeout=25)
    assert proc.returncode == 0, proc.stderr
    report = json.loads(proc.stdout); jwt = decode(report.pop('jwt'))
    assert jwt['iss']=='explicit-key' and jwt['sub']=='identity' and jwt['name']=='name' and jwt['metadata']=='meta' and jwt['attributes']=={'role':'test'} and jwt['kind']=='agent'
    assert jwt['exp']-jwt['iat']==120 and jwt['sip']=={'call':True}
    methods = json.loads(Path(__file__).with_name('api_methods.json').read_text())
    services = dict(room='RoomService',egress='Egress',ingress='Ingress',sip='SIP',agent_dispatch='AgentDispatchService',connector='Connector')
    expected = [(services[group],method) for group, values in methods.items() for method in values]
    ids = set()
    for (service,method),(path,headers,body) in zip(expected,requests):
        assert path==f'/twirp/livekit.{service}/{method}',path
        lower={k.lower():v for k,v in headers.items()}
        rid=lower['x-livekit-request-id']; assert uuid.UUID(rid).version==4 and rid not in ids; ids.add(rid)
        assert lower['content-type']=='application/json'
        claims=decode(lower['authorization'].removeprefix('Bearer ')); assert claims['iss']=='explicit-key'
        if service=='RoomService':
            grant={'roomCreate':True} if method in ('CreateRoom','DeleteRoom') else {'roomList':True} if method=='ListRooms' else {'roomAdmin':True,'room':'source'}
            if method in ('MoveParticipant','ForwardParticipant'): grant['destinationRoom']='target'
            assert claims['video']==grant,(method,claims)
            if method=='SendData': assert len(base64.b64decode(body['nonce']))==16
        elif service in ('Egress','Ingress'): assert claims['video']=={('roomRecord' if service=='Egress' else 'ingressAdmin'):True}
        elif service=='SIP':
            assert claims['sip']=={('call' if method in ('CreateSIPParticipant','TransferSIPParticipant') else 'admin'):True}
            if method=='TransferSIPParticipant': assert claims['video']=={'roomAdmin':True,'room':'source'}
            if method in ('CreateSIPParticipant','TransferSIPParticipant'): assert body['ringingTimeout']=='30s'
        elif service=='AgentDispatchService': assert claims['video']=={'roomAdmin':True,'room':'source'}
        else:
            grant={'roomCreate':True}
            if method not in ('ConnectWhatsAppCall','DisconnectWhatsAppCall'): grant['room']='source'
            assert claims['video']==grant
    n=len(expected); assert len(requests)==n+5,(len(requests),n)
    assert requests[n][2]=={'room':'source','dispatchId':'missing'}
    n+=1
    assert requests[n][1]['Authorization']=='Bearer ambient-token'
    assert requests[n+1][1]['Authorization']=='Bearer explicit-token'
    if args.cli:
        proc=subprocess.run([str(Path(args.cli).resolve()),'--api-key','explicit-key','--api-secret','contract-secret','room','list'],env=env,capture_output=True,text=True,timeout=10)
        assert proc.returncode==0,proc.stderr
        assert decode(requests[-1][1]['Authorization'].removeprefix('Bearer '))['video']=={'roomList':True}
    report.update(rpcMethods=len(expected), getDispatch=True, credentialPriority=True, structuredErrors=True, cancellation=True)
    root=Path('out/api-contract')/time.strftime('%Y%m%d-%H%M%S');root.mkdir(parents=True)
    (root/'result.json').write_text(json.dumps(report,indent=2));print(json.dumps(report),root)
finally:
    server.shutdown(); server.server_close()

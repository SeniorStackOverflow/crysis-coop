"""Protocol test of coop_relay.py (protocol 2 and 1). Usage: test_protocol.py ws://127.0.0.1:64190/  (or the wss:// URL behind Caddy)"""
import socket, ssl, struct, os, base64, sys, time

def ws_connect(url):
    secure = url.startswith('wss://'); rest = url.split('://', 1)[1]
    hostport, _, path = rest.partition('/'); path = '/' + path
    host, _, port = hostport.partition(':'); port = int(port or (443 if secure else 80))
    s = socket.create_connection((host, port), timeout=10)
    if secure: s = ssl.create_default_context().wrap_socket(s, server_hostname=host)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(('GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (path, host, key)).encode())
    resp = b''
    while b'\r\n\r\n' not in resp: resp += s.recv(1)
    assert b' 101 ' in resp.split(b'\r\n')[0], resp
    return s

def send(s, data):
    mask = os.urandom(4); n = len(data)
    head = bytes([0x82, 0x80 | n]) if n < 126 else bytes([0x82, 0x80 | 126]) + struct.pack('>H', n)
    m = (mask * (n // 4 + 1))[:n]
    s.sendall(head + mask + (int.from_bytes(data, 'big') ^ int.from_bytes(m, 'big')).to_bytes(n, 'big') if n else head + mask)

def recvx(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b

def recv(s, timeout=5):
    s.settimeout(timeout)
    b0, b1 = recvx(s, 2); n = b1 & 0x7f
    if n == 126: n = struct.unpack('>H', recvx(s, 2))[0]
    elif n == 127: n = struct.unpack('>Q', recvx(s, 8))[0]
    data = recvx(s, n)
    if b0 & 0x0f == 8: raise EOFError('closed')
    return data

def expect(s, op, what):
    r = recv(s)
    assert r[0] == op, '%s: got %r' % (what, r[:20])
    return r

url = sys.argv[1]
ok = lambda m: print('OK  ', m)

# ---- host v2: stable code
hkey = os.urandom(16)
host = ws_connect(url); send(host, bytes([1, 2]) + hkey)
r = expect(host, 2, 'registered'); code = struct.unpack_from('<I', r, 1)[0]; hid = r[5:13]
assert 100000 <= code <= 999999; ok('host v2 registered, code %d id %s' % (code, hid.hex()))
send(host, bytes([16, 2]))
host.close(); time.sleep(0.3)
host = ws_connect(url); send(host, bytes([1, 2]) + hkey)
r = expect(host, 2, 're-registered'); assert struct.unpack_from('<I', r, 1)[0] == code; ok('same key -> same code after reconnect')
send(host, bytes([16, 2]))

# ---- friend v2
fkey = os.urandom(16); token = os.urandom(8)
friend = ws_connect(url); send(friend, bytes([8, 2]) + struct.pack('<I', code) + token + fkey)
r = expect(friend, 9, 'joined'); assert r[1] == 2, r; ok('friend joined, host ready')
r = expect(host, 17, 'friend info'); cid = struct.unpack_from('<H', r, 1)[0]; fid = r[3:11]; ok('host told friend client %d id %s' % (cid, fid.hex()))
send(friend, bytes([10]) + b'hello'); r = expect(host, 4, 'data to host'); assert r[3:] == b'hello'
send(host, bytes([5]) + struct.pack('<H', cid) + b'olleh'); r = expect(friend, 11, 'data to friend'); assert r[1:] == b'olleh'; ok('data both ways')

# ---- host restart / connection loss
send(host, bytes([16, 1])); r = expect(friend, 16, 'restarting'); assert r[1] == 1; ok('friend told: host restarting')
send(host, bytes([16, 2])); r = expect(friend, 16, 'ready'); assert r[1] == 2; ok('friend told: host ready')
host.close(); r = expect(friend, 16, 'gone'); assert r[1] == 3; ok('friend told: host connection lost')
friend2 = ws_connect(url); send(friend2, bytes([8, 2]) + struct.pack('<I', code) + os.urandom(8) + os.urandom(16))
r = expect(friend2, 9, 'join while host away'); assert r[1] == 3; ok('a friend may join and wait while the host is away')
host = ws_connect(url); send(host, bytes([1, 2]) + hkey); expect(host, 2, 'host back')
seen = set()
for _ in range(2):
    r = expect(host, 17, 'friend infos again'); seen.add(struct.unpack_from('<H', r, 1)[0])
assert cid in seen; ok('host back: told who the friends are again')
send(host, bytes([16, 2])); r = expect(friend, 16, 'ready again'); assert r[1] == 2; ok('friend told: host ready again')
friend2.close()

# ---- cloud
def cloud(key):
    c = ws_connect(url); send(c, bytes([20, 2]) + key); r = expect(c, 21, 'welcome'); return c, r[1:9]
def blob(campaign_hex, stamp, players, checkpoint, size):
    progress = ('version=2\ncampaign=%s\nname=Test\nlevel=rescue\ncheckpoint=%s\nstamp=%d\nhostname=Host\n' % (campaign_hex, checkpoint, stamp)
                + ''.join('player=id:%s\tX\t\t\t\n' % p for p in players)).encode()
    save = os.urandom(size)
    return b'CCSV' + struct.pack('<II', 1, len(progress)) + progress + struct.pack('<I', len(save)) + save
def upload(c, campaign, data):
    send(c, bytes([22]) + campaign + struct.pack('<I', len(data)))
    r = recv(c)
    if r[0] != 23: return r
    for i in range(0, len(data), 48 * 1024): send(c, bytes([24]) + data[i:i + 48 * 1024])
    send(c, bytes([25])); return recv(c)
def download(c, campaign, which):
    send(c, bytes([29]) + campaign + bytes([which])); r = recv(c)
    if r[0] != 30: return r
    size = struct.unpack_from('<I', r, 1)[0]; data = b''
    while True:
        r = recv(c)
        if r[0] == 25: break
        assert r[0] == 24; data += r[1:]
    assert len(data) == size; return data

ch, _ = cloud(hkey)
campaign = os.urandom(8)
b1 = blob(campaign.hex(), 1000, [fid.hex()], 'rescue_start', 300000)
r = upload(ch, campaign, b1); assert r[0] == 26, r; ok('checkpoint stored (300 KB)')
r = upload(ch, campaign, b1); assert r == bytes([7, 11]), r; ok('second upload right away refused: too often')
send(ch, bytes([27])); r = expect(ch, 28, 'list'); lines = r[1:].decode().splitlines()
assert any(l.startswith(campaign.hex() + '\town') for l in lines), lines; ok('owner lists it: ' + lines[0].replace('\t', ' | '))
cf, _ = cloud(fkey)
send(cf, bytes([27])); r = expect(cf, 28, 'friend list'); assert (campaign.hex() + '\tmember') in r[1:].decode(); ok('the friend (listed as a player) lists it as member')
assert download(cf, campaign, 0) == b1; ok('the friend downloads it, identical')
cx, _ = cloud(os.urandom(16))
send(cx, bytes([27])); r = expect(cx, 28, 'stranger list'); assert campaign.hex() not in r[1:].decode()
r = download(cx, campaign, 0); assert r == bytes([7, 10]), r; ok('a stranger neither lists nor downloads it')
r = upload(cx, campaign, b1); assert r == bytes([7, 8]), r; ok('a stranger cannot overwrite it')
b2 = blob(campaign.hex(), 2000, [hid.hex()], 'rescue_02', 200000)
r = upload(cf, campaign, b2); assert r[0] == 26, r; ok('the friend continued the campaign and stored a checkpoint')
assert download(ch, campaign, 0) == b2 and download(ch, campaign, 1) == b1; ok('owner: last = the friend\'s, previous = his own')
bad = b1[:100]
r = upload(cx, os.urandom(8), bad[:50] + b'x'); assert r == bytes([7, 12]) or r == bytes([7, 11]), r; ok('garbage refused')
send(cf, bytes([33]) + campaign); expect(cf, 34, 'member leaves')
send(cf, bytes([27])); r = expect(cf, 28, 'list after leave'); assert campaign.hex() not in r[1:].decode(); ok('a member can drop it from his list')
send(ch, bytes([33]) + campaign); expect(ch, 34, 'owner deletes')
send(ch, bytes([27])); r = expect(ch, 28, 'list after delete'); assert campaign.hex() not in r[1:].decode(); ok('the owner deletes it')

# ---- protocol 1 (mod 0.2 / 0.3)
h1 = ws_connect(url); send(h1, bytes([1, 1]) + struct.pack('<H', 0) + bytes(8))
r = expect(h1, 2, 'v1 registered'); c1 = struct.unpack_from('<H', r, 1)[0]; assert 64101 <= c1 <= 64164
f1 = ws_connect(url); send(f1, bytes([8, 1]) + struct.pack('<H', c1)); r = recv(f1); assert r == b'\x09', r
send(f1, bytes([10]) + b'v1'); r = expect(h1, 4, 'v1 data'); assert r[3:] == b'v1'; ok('protocol 1 still works (code %d)' % c1)
print('ALL PASSED')

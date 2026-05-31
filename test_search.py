#!/usr/bin/env python3
import os
import socket
import struct
import subprocess
import threading
import time
import zlib


def u32(n):
    return struct.pack('<I', n)


def u64(n):
    return struct.pack('<Q', n)


def s(value):
    data = value.encode()
    return u32(len(data)) + data


def recv_exact(conn, n):
    data = b''
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def recv_server_msg(conn):
    length = struct.unpack('<I', recv_exact(conn, 4))[0]
    body = recv_exact(conn, length)
    code = struct.unpack('<I', body[:4])[0]
    return code, body[4:]


def send_server_msg(conn, code, payload=b''):
    conn.sendall(u32(4 + len(payload)) + u32(code) + payload)


def send_peer_init(conn):
    payload = bytes([1]) + s('fixturepeer') + s('P') + u32(0)
    conn.sendall(u32(len(payload)) + payload)


def send_search_response(conn):
    payload = b''.join([
        s('fixturepeer'),
        u32(1234),
        u32(1),
        bytes([1]),
        s('Music\\Linkin Park\\Numb.mp3'),
        u64(3456789),
        s('mp3'),
        u32(0),
        bytes([1]),
        u32(2048),
        u32(0),
        u32(0),
        u32(0),
    ])
    body = u32(9) + zlib.compress(payload)
    conn.sendall(u32(len(body)) + body)


def free_port():
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def server_thread(port, ready, peer_port=None):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port))
    srv.listen(1)
    ready.set()
    conn, _ = srv.accept()
    with conn:
        recv_server_msg(conn)  # Login
        send_server_msg(conn, 1, b'\x01')
        recv_server_msg(conn)  # SetWaitPort
        recv_server_msg(conn)  # Search
        if peer_port is not None:
            payload = b''.join([
                s('fixturepeer'),
                s('P'),
                socket.inet_aton('127.0.0.1'),
                u32(peer_port),
                u32(4321),
            ])
            send_server_msg(conn, 18, payload)
        time.sleep(2)
    srv.close()


def incoming_peer(port):
    deadline = time.time() + 5
    hosts = ('127.0.0.1', '::1')
    while time.time() < deadline:
        for host in hosts:
            try:
                conn = socket.create_connection((host, port), timeout=1)
                with conn:
                    send_peer_init(conn)
                    send_search_response(conn)
                    time.sleep(0.2)
                return
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError('client listen socket was not reachable')


def indirect_peer(port, ready):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port))
    srv.listen(1)
    ready.set()
    conn, _ = srv.accept()
    with conn:
        length = struct.unpack('<I', recv_exact(conn, 4))[0]
        body = recv_exact(conn, length)
        assert body[0] == 0
        assert struct.unpack('<I', body[1:5])[0] == 4321
        send_search_response(conn)
        time.sleep(2)
    srv.close()


def run_search(mode):
    server_port = free_port()
    listen_port = free_port()
    peer_port = free_port()
    ready = threading.Event()

    if mode == 'indirect':
        peer_ready = threading.Event()
        peer = threading.Thread(target=indirect_peer, args=(peer_port, peer_ready), daemon=True)
        peer.start()
        peer_ready.wait(5)
        server = threading.Thread(target=server_thread, args=(server_port, ready, peer_port), daemon=True)
    else:
        server = threading.Thread(target=server_thread, args=(server_port, ready), daemon=True)

    server.start()
    ready.wait(5)

    env = os.environ.copy()
    env.update({
        'SLSK_SERVER': '127.0.0.1',
        'SLSK_PORT': str(server_port),
        'SLSK_SEARCH_SECONDS': '1',
        'SLSK_USER': 'fixtureuser',
        'SLSK_PASS': 'fixturepass',
    })
    if mode == 'incoming':
        env['SLSK_LISTEN_PORT'] = str(listen_port)

    proc = subprocess.Popen(['./soulc', 'search', 'numb'], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if mode == 'incoming':
        incoming_peer(listen_port)

    out, err = proc.communicate(timeout=5)
    print(f'[{mode}]')
    print(out, end='')
    if err:
        print(err, end='', file=os.sys.stderr)
    assert proc.returncode == 0, proc.returncode
    assert 'fixturepeer\t3456789\tMusic\\Linkin Park\\Numb.mp3' in out


def main():
    run_search('incoming')
    run_search('indirect')


if __name__ == '__main__':
    main()

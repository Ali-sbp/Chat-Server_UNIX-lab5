#!/usr/bin/env python3
"""Exercise the real TCP server; use only Python's standard library."""

import contextlib
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else './chatserver').resolve()


def unused_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Peer:
    def __init__(self, port, receive_buffer=None):
        self.socket = socket.socket()
        self.socket.settimeout(3)
        if receive_buffer is not None:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.socket.connect(('127.0.0.1', port))
        self.buffer = b''
        welcome = self.line()
        assert welcome.startswith(b'INFO connected as guest'), welcome

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.socket.sendall(data)

    def line(self, timeout=3):
        deadline = time.monotonic() + timeout
        while b'\n' not in self.buffer:
            remaining = deadline - time.monotonic()
            assert remaining > 0, 'timed out waiting for a line'
            self.socket.settimeout(remaining)
            data = self.socket.recv(8192)
            if not data:
                assert not self.buffer, self.buffer
                return b''
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        return line

    def expect(self, expected):
        if isinstance(expected, str):
            expected = expected.encode()
        actual = self.line()
        assert actual == expected, (actual, expected)

    def quiet(self):
        assert not self.buffer, self.buffer
        assert not select.select([self.socket], [], [], 0.1)[0], 'unexpected data'

    def nick(self, name):
        self.send(f'/nick {name}\n')
        self.expect(f'INFO nickname is {name}')

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class Server:
    def __init__(self, directory, daemon=False):
        self.port = unused_port()
        self.log = Path(directory) / f'chat-{self.port}.log'
        self.daemon = daemon
        self.pid = None
        command = [str(BINARY), '--bind', '127.0.0.1', '--port', str(self.port),
                   '--log', str(self.log)]
        if not daemon:
            command.append('--foreground')
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 5
        try:
            while True:
                if daemon and self.log.exists():
                    match = re.search(r'pid=(\d+) INFO started ', self.log.read_text())
                    if match:
                        self.pid = int(match.group(1))
                        break
                if not daemon:
                    try:
                        with Peer(self.port):
                            break
                    except ConnectionRefusedError:
                        pass
                if self.process.poll() is not None and (not daemon or self.process.returncode != 0):
                    raise AssertionError(self.process.communicate())
                assert time.monotonic() < deadline, 'server did not start'
                time.sleep(0.02)
            if daemon:
                assert self.process.wait(timeout=3) == 0
        except BaseException:
            self.stop()
            raise

    def stop(self):
        if self.daemon and self.pid is not None:
            with contextlib.suppress(ProcessLookupError):
                os.kill(self.pid, signal.SIGTERM)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if 'INFO stopped;' in self.log.read_text():
                    break
                time.sleep(0.02)
            assert 'INFO stopped;' in self.log.read_text(), 'daemon did not shut down'
        elif self.process.poll() is None:
            self.process.terminate()
        try:
            output, errors = self.process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
            raise
        assert self.process.returncode == 0, (self.process.returncode, output, errors)
        assert not errors, errors

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.stop()


def routing_and_framing(directory):
    with Server(directory) as server, Peer(server.port) as alice, \
            Peer(server.port) as bob, Peer(server.port) as carol:
        alice.nick('Alice')
        bob.nick('Bob')
        carol.nick('Carol')

        alice.send('Hello everyone\n')
        for peer in (alice, bob, carol):
            peer.expect('[all] Alice: Hello everyone')

        alice.send('/msg Bob secret\n')
        alice.expect('INFO queued for Bob')
        bob.expect('[private] Alice: secret')
        carol.quiet()

        alice.send('/msg B')
        time.sleep(0.03)
        alice.send('ob split')
        time.sleep(0.03)
        alice.send(' message\r\n')
        alice.expect('INFO queued for Bob')
        bob.expect('[private] Alice: split message')

        bob.send('first\nsecond\n')
        for peer in (alice, bob, carol):
            peer.expect('[all] Bob: first')
            peer.expect('[all] Bob: second')

        alice.send('/msg Alice self\n')
        alice.expect('[private] Alice: self')
        alice.expect('INFO queued for Alice')
        bob.send('/nick Alice\n')
        bob.expect('ERROR nickname already in use')
        alice.send('/msg Nobody text\n')
        alice.expect('ERROR no such participant')
        alice.send('/msg Bob\n')
        alice.expect('ERROR usage: /msg NAME TEXT')
        alice.send('/nick bad/name\n')
        assert alice.line().startswith(b'ERROR usage: /nick')
        alice.send('/unknown\n')
        alice.expect('ERROR unknown command or arguments; use /help')
        alice.send(b'bad\x1btext\n')
        alice.expect('ERROR control characters are not allowed')
        alice.send('/who\n')
        alice.expect('INFO participants:')
        names = set()
        while True:
            line = alice.line()
            if line == b'INFO end of participants':
                break
            names.add(line)
        assert names == {b'USER Alice', b'USER Bob', b'USER Carol'}, names

        text = 'Привет, чат!'.encode()
        alice.send(text + b'\n')
        for peer in (alice, bob, carol):
            peer.expect(b'[all] Alice: ' + text)
        alice.send(b'x' * 1024 + b'\r\n')
        for peer in (alice, bob, carol):
            peer.expect(b'[all] Alice: ' + b'x' * 1024)

        for data, error in [(b'x' * 1026 + b'\n', b'ERROR line too long'),
                            (b'bad\x00data\n', b'ERROR null bytes are not allowed')]:
            with Peer(server.port) as bad:
                bad.send(data)
                bad.expect(error)
                bad.expect(b'')

        with Peer(server.port) as dave:
            dave.nick('Dave')
            dave.send('last one\nlast two\n')
            dave.socket.shutdown(socket.SHUT_WR)
            dave.expect('[all] Dave: last one')
            dave.expect('[all] Dave: last two')
            dave.expect(b'')
            for peer in (alice, bob, carol):
                peer.expect('[all] Dave: last one')
                peer.expect('[all] Dave: last two')

        with Peer(server.port) as incomplete:
            incomplete.send('unfinished')
            incomplete.socket.shutdown(socket.SHUT_WR)
            incomplete.expect(b'')
        alice.quiet()

        bob.send('/quit\nignored after quit\n')
        bob.expect('INFO goodbye')
        bob.expect(b'')
        with Peer(server.port) as replacement:
            replacement.nick('Bob')
        with Peer(server.port) as reset:
            reset.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            reset.send('/help\n')
        alice.send('still running\n')
        alice.expect('[all] Alice: still running')
        carol.expect('[all] Alice: still running')
    print('PASS routing, privacy, commands, framing, input limits, half-close and disconnects')


def capacity(directory):
    with Server(directory) as server:
        # Let the startup probe's EOF be processed before filling every slot.
        time.sleep(0.05)
        peers = []
        try:
            for _ in range(64):
                peers.append(Peer(server.port))
            with socket.create_connection(('127.0.0.1', server.port), timeout=3) as excess:
                assert excess.recv(1024) == b'', 'excess client was admitted'
            peers[0].send('/quit\n')
            peers[0].expect('INFO goodbye')
            peers[0].expect(b'')
            with Peer(server.port) as replacement:
                replacement.send('/help\n')
                replacement.expect('INFO commands:')
        finally:
            for peer in peers:
                peer.close()
    print('PASS client capacity and slot reuse')


def slow_reader(directory):
    with Server(directory) as server, Peer(server.port) as sender, \
            Peer(server.port, receive_buffer=1024) as slow, Peer(server.port) as observer:
        sender.nick('Sender')
        slow.nick('Slow')
        observer.nick('Observer')
        payload = 'x' * 900

        # Pending bytes must survive while the receiver temporarily does not read.
        for _ in range(32):
            sender.send(f'/msg Slow {payload}\n')
            sender.expect('INFO queued for Slow')
        for _ in range(32):
            slow.expect(f'[private] Sender: {payload}')

        # Keep only Slow from reading; Sender drains every acknowledgment.
        for _ in range(20000):
            sender.send(f'/msg Slow {payload}\n')
            result = sender.line()
            if result.startswith(b'ERROR'):
                assert result in (b'ERROR recipient cannot receive more messages',
                                  b'ERROR no such participant'), result
                break
            assert result == b'INFO queued for Slow', result
        else:
            raise AssertionError('slow reader was not bounded')

        sender.send('healthy clients continue\n')
        sender.expect('[all] Sender: healthy clients continue')
        observer.expect('[all] Sender: healthy clients continue')
        assert 'outgoing buffer limit' in server.log.read_text()
    print('PASS queued data survives delayed reads; slow reader is bounded and isolated')


def daemon_and_errors(directory):
    with Server(directory, daemon=True) as server:
        with Peer(server.port) as peer:
            peer.send('/quit\n')
            peer.expect('INFO goodbye')
            peer.expect(b'')
        # Binding must fail before the daemon launcher reports success.
        conflict = subprocess.run([str(BINARY), '--bind', '127.0.0.1',
                                   '--port', str(server.port),
                                   '--log', str(Path(directory) / 'conflict.log')],
                                  capture_output=True, timeout=3)
        assert conflict.returncode != 0
        assert 'ERROR server setup:' in (Path(directory) / 'conflict.log').read_text()

    # A clean shutdown releases the port for a subsequent listener.
    with socket.socket() as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('127.0.0.1', server.port))
    for arguments in (['--port', '0'], ['--port', 'abc'], ['--bind', 'invalid'],
                      ['--log', ''], ['unexpected']):
        result = subprocess.run([str(BINARY), *arguments], capture_output=True, timeout=3)
        assert result.returncode != 0, arguments
    help_result = subprocess.run([str(BINARY), '--help'], capture_output=True, timeout=3)
    assert help_result.returncode == 0 and b'--foreground' in help_result.stdout
    print('PASS daemon lifecycle, port reuse, logged bind failure and option validation')


def interrupt_idle_server(directory):
    with Server(directory) as server, Peer(server.port) as peer:
        # No chat activity is needed to wake select() during terminal-style shutdown.
        server.process.send_signal(signal.SIGINT)
        peer.expect(b'')
        assert server.process.wait(timeout=3) == 0
    print('PASS SIGINT wakes an idle event loop and closes connected clients')


with tempfile.TemporaryDirectory(prefix='unix-chat-tests-') as directory:
    routing_and_framing(directory)
    capacity(directory)
    slow_reader(directory)
    daemon_and_errors(directory)
    interrupt_idle_server(directory)
print('All integration checks passed.')

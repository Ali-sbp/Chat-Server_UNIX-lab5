# UNIX lab 5: network chat

A C11 TCP chat server using non-blocking sockets and `select()`. One event-loop
process handles every participant. It runs as a daemon by default and logs to a
file. Netcat is the test client allowed by the assignment.

## Build

From this directory, on Debian/Linux or macOS:

```sh
make
```

To select GCC explicitly:

```sh
make CC=gcc
```

No C++ compiler or third-party C libraries are needed. `make clean` removes the
executable and build products, leaving the sources and logs intact.

## Try the chat in foreground mode

Start the server in one terminal:

```sh
./chatserver --foreground
```

Open two other terminals and run this command in each:

```sh
nc 127.0.0.1 3425
```

Each connection receives a unique temporary name, such as `guest1`. In the first
client, enter `/nick Alice`; in the second, enter `/nick Bob`. Press Enter after
each command or message.

<!-- TODO 1/2: Changed command examples for receipt times and /b; previous table preserved:
| Input | Result |
| --- | --- |
| `Hello everyone` | Every participant, including the sender, receives `[all] Alice: Hello everyone` |
| `/msg Bob Hello Bob` | Only Bob receives `[private] Alice: Hello Bob`; Alice gets a queue confirmation |
| `/nick Alice` | Choose a unique, case-sensitive nickname |
| `/who` | List the current participants |
| `/help` | Show commands |
| `/quit` | Receive a goodbye and disconnect |
-->

| Input | Result |
| --- | --- |
| `Hello everyone` | Every participant, including the sender, receives `[12:34:56] [all] Alice: Hello everyone` |
| `/b Hello everyone` | Explicit broadcast using the same timestamped format as plain text |
| `/msg Bob Hello Bob` | Only Bob receives `[12:34:56] [private] Alice: Hello Bob`; Alice gets a queue confirmation |
| `/nick Alice` | Choose a unique, case-sensitive nickname |
| `/who` | List the current participants |
| `/help` | Show commands |
| `/quit` | Receive a goodbye and disconnect |

<!-- TODO 1: Document exactly what the added receipt timestamp means. -->
The `[HH:MM:SS]` prefix shows the server's local time when the complete incoming
message is handled. It is captured once and shared by all broadcast recipients.
It does not measure when netcat receives or displays the bytes. System replies,
such as errors and queue confirmations, keep their existing format.

<!-- TODO 2: Document the added explicit broadcast command and its validation. -->
`/b TEXT` broadcasts to everyone, including the sender; plain-text broadcasts
remain supported. Empty `/b` commands return `ERROR usage: /b TEXT`. To demonstrate:

```sh
printf '/nick Alice\n/b Hello everyone\n' | nc -w 2 127.0.0.1 3425
```

A private-message confirmation means the server queued the message; it is not an
acknowledgment from the recipient. Nicknames contain 1-24 ASCII letters, digits,
underscores, or hyphens. Names are case-sensitive: `Alice` and `alice` differ.
Blank lines are ignored. Text is passed through without encoding conversion, so
UTF-8 text works. A line beginning with `/` is interpreted as a command.

Ctrl+C closes a netcat client. Ctrl+C in the foreground server stops the server
and closes its clients. The same listener can then be started again.

## Daemon mode and logging

```sh
./chatserver --port 3425 --log ./server.log
tail -f server.log
```

The launcher exits after daemon setup. The daemon detaches using a double fork,
changes its working directory to `/`, and redirects standard input/output/error
to `/dev/null`. The log path is resolved before detaching. Startup, connections,
nickname changes, message routing, errors, and disconnections are logged. Routing
records include names and byte counts; they do not include private message text.

The `started` record contains the daemon PID, for example `pid=12345`. Stop that
specific process using its actual PID:

```sh
kill -TERM 12345
```

`SIGTERM` and `SIGINT` wake the event loop and release its resources. Shutdown
closes active connections; it does not promise to drain all queued messages.

## Settings and limits

```sh
./chatserver --help
./chatserver --foreground --bind 127.0.0.1 --port 5000 --log ./chat.log
```

Defaults: bind `0.0.0.0` (all local IPv4 interfaces), port `3425`, log
`./server.log`, daemon mode. To connect from another machine, use the server's
reachable IPv4 address. Inside the VM, `127.0.0.1` refers to the VM itself.

Limits are defined in `include/config.h`:

- 64 connected clients; additional connections are closed.
- 1024 input bytes per line, excluding LF or CRLF. This includes command text.
- 64 KiB of queued outgoing data per client. A client exceeding this limit is
  disconnected, allowing other clients to continue.
- Socket descriptors must be below `FD_SETSIZE`, as required by `select()`.

Messages are newline-delimited. Partial lines wait for more bytes, and several
lines received together are processed separately. An unfinished final line is
discarded when the peer finishes sending. Overlong lines and null bytes produce
an error and close that connection. Other terminal control characters are
rejected. The server supports ordinary netcat text input; it does not implement
the Telnet negotiation protocol.

## Module structure

| Module | Responsibility |
| --- | --- |
| `main.c` | Configuration, resource setup, daemon startup, event loop, cleanup |
| `config.c` / `config.h` | Defaults, command-line options, validation, shared limits |
| `daemon.c` / `daemon.h` | Terminal detachment and startup status |
| `logger.c` / `logger.h` | Timestamped file logging |
| `server.c` / `server.h` | Non-blocking sockets, readiness, input framing, reads/writes, signals |
| `clients.c` / `clients.h` | Client identities, socket ownership, input/output storage |
| `chat.c` / `chat.h` | Commands, nickname validation, unicast and broadcast routing |

The server extracts complete lines and passes them to the chat module. The chat
module queues messages using client storage. The server sends those bytes only
when `select()` reports write readiness, keeping any unsent remainder.

## Automated checks

Python 3 is needed only for the test script:

```sh
make test
```

The tests use temporary log files and local TCP ports, and stop their own server
processes afterward. They check routing, fragmented/combined lines, input errors,
disconnects, buffer limits, capacity, daemon startup/shutdown, and startup errors.

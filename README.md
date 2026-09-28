# tether

Reliable, connection-oriented transport over UDP, written in C. Built for Mini Project 1 of the Operating Systems and Networks course. The protocol is called **S.H.A.M.**

This branch is the code as it was submitted. The [`rework`](../../tree/rework) branch fixes a number of bugs, is much faster, and adds a test suite and benchmarks.

## What it does

Two programs, `server` and `client`, talk S.H.A.M. over UDP:

- **File transfer:** the client sends a file, the server saves it and prints its MD5.
- **Chat:** a two-way chat between the terminals, running over the same reliable transport.
- **Loss simulation:** both sides can drop a chosen fraction of outgoing packets, to show that retransmission works.

## Protocol

Every packet starts with a 12-byte header in network byte order:

| offset | size | field |
|---|---|---|
| 0 | 4 | sequence number |
| 4 | 4 | acknowledgement number |
| 8 | 2 | flags: `SYN` 0x1, `ACK` 0x2, `FIN` 0x4 |
| 10 | 2 | receive window size |

The payload that follows is up to 1024 bytes.

- **Connection setup:** a three-way handshake (SYN → SYN-ACK → ACK) with random initial sequence numbers.
- **Reliable data:** sequence numbers count bytes. The sender keeps up to 10 packets in flight, and each packet has its own retransmit timer. The first timeout is 500 ms, and it doubles on every retry up to 5 s. After 5 retries the sender gives up on that packet.
- **Receiver:** sends cumulative ACKs, and holds out-of-order packets until the gap before them is filled.
- **Teardown:** a four-way FIN exchange.

## Build

Needs gcc, make and OpenSSL (`libssl-dev` on Debian/Ubuntu, `brew install openssl` on macOS).

```sh
make          # builds ./client and ./server
make clean
```

## Usage

```sh
# file transfer
./server <port> [loss_rate]
./client <server_ip> <port> <input_file> <output_file> [loss_rate]

# chat (type "quit" to leave)
./server <port> --chat [loss_rate]
./client <server_ip> <port> --chat [loss_rate]
```

`loss_rate` is between 0.0 and 1.0. For example:

```sh
./server 8080 0.1
./client 127.0.0.1 8080 file.in received.txt 0.1
```

The server saves the file as `<output_file>` (any path is stripped off), prints its MD5 and exits.

Set `RUDP_LOG=1` to write a timestamped event log (SYN, DATA, ACK, TIMEOUT, RETX, FIN, …) to `clientlog.txt` / `serverlog.txt`.

## Layout

```
include/sham.h        header, flags, protocol constants
src/sham.c            initial sequence numbers, packet validation
src/network.c         socket setup, packing/unpacking, loss simulation
src/connection.c      handshake, sliding window, retransmission, teardown
src/logging.c         RUDP_LOG event log
src/client.c          client: file transfer and chat
src/server.c          server: file transfer and chat
file.in               sample input file
```

## Known issues

These are fixed on the [`rework`](../../tree/rework) branch:

- **The sliding window never slides.** Every 1 KB chunk waits for its ACK before the next one goes out, so in practice it is stop-and-wait. 1 MB takes about 11 s on loopback with no loss, and about 3 minutes at 10% loss.
- **Data can be skipped.** A packet that runs out of retries is dropped without any error, and the transfer still reports success.
- **Duplicated or reordered packets break the filename.** A filename that arrives late or out of order can end up with file data stuck onto it.
- **Ctrl+C doesn't stop a server** that is waiting for a client.
- **Other bugs:**
  - an empty file isn't saved
  - chat can overflow its receive buffer by one byte
  - sequence numbers can wrap around, and that isn't handled
  - packets from any address are accepted

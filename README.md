# TinyKV

TinyKV is a small, concurrent key/value database in C++20. It is designed as a systems programming portfolio project: the code includes a thread-safe storage core, a TCP server, a binary-safe RESP2 command protocol, an append-only write-ahead log, automated tests, and a repeatable benchmark.

## Features

- Concurrent reads and serialized writes using `std::shared_mutex`.
- Optional append-only WAL with sequence numbers and CRC-32 record checksums.
- WAL replay on startup; an incomplete final record is treated as a crash tail and truncated. A complete record with an invalid checksum or header stops startup with an error.
- Default durable mode flushes the WAL to the operating system before acknowledging each write (`fsync` on POSIX, `FlushFileBuffers` on Windows). `--no-sync` skips that flush for performance and weakens crash durability.
- RESP2 array-of-bulk-strings requests, binary-safe values, incremental parsing, pipelined requests, bounded request sizes, and a fixed-size connection worker pool.
- A command-line client, unit tests, and a 90/10 read/write benchmark.
- CMake build and CTest integration, with no third-party runtime dependencies.

## Build

Requires CMake 3.16 or newer and a C++20 compiler. On Windows, the build links the system Winsock library.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

The default server address is `127.0.0.1:6380`, so it is only reachable from the local machine unless you explicitly change the bind address.

## Run

```sh
# In-memory database
./build/tinykv-server

# Persistent database; sync every successful write
./build/tinykv-server --wal ./data/tinykv.wal

# In another terminal
./build/tinykv-cli PING
./build/tinykv-cli SET language C++
./build/tinykv-cli GET language
./build/tinykv-cli DEL language
./build/tinykv-cli STATS
```

On multi-configuration generators such as Visual Studio, executables are commonly placed in `build/Release/` instead of directly in `build/`.

Server options:

```text
--host ADDRESS   Bind address; use * to bind all interfaces (default 127.0.0.1)
--port PORT      TCP port (default 6380)
--workers N      Number of connection workers (default min(hardware threads, 32))
--wal PATH       Enable the WAL at PATH
--no-sync        Do not force WAL data to stable storage after each write
```

The CLI accepts the same host and port options before the command. It displays bulk values as text, so use a RESP client directly when you need to inspect arbitrary binary values.

## Commands

| Command | Response | Meaning |
|---|---|---|
| `PING` or `PING message` | `+PONG` or bulk string | Health check / echo |
| `SET key value` | `+OK` | Insert or replace a key |
| `GET key` | bulk string or null bulk | Read a value |
| `DEL key` | integer `0` or `1` | Delete a key |
| `STATS` | bulk string | Current key and operation counts |
| `QUIT` | `+BYE` | Close the client connection |

See [docs/protocol.md](docs/protocol.md) for framing, limits, and examples. Any RESP2 client that sends arrays of bulk strings can talk to TinyKV.

## Persistence and consistency

Each mutating operation is appended to the WAL while the database has its exclusive lock. In durable mode, the record is flushed before the in-memory map is changed and before the server returns success. This preserves ordering between acknowledged writes and recovered writes, subject to the operating system, filesystem, and storage device honoring their flush guarantees.

The WAL is append-only and currently has no snapshotting, compaction, replication, or multi-process file lock. It grows with every `SET` and `DEL`; plan to remove or rotate it only while the server is stopped. A WAL I/O failure makes later WAL writes fail rather than allowing the process to append after a possibly partial record.

## Concurrency and server model

Storage reads use a shared lock; mutations and WAL appends use an exclusive lock. The server has a fixed worker pool and a bounded queue of 256 pending connections. Each worker processes one connection at a time, including pipelined requests. A client connection is closed after one second without inbound data, which prevents idle clients from occupying workers indefinitely. Queue overflow receives a `server is busy` error and the connection is closed.

## Tests

The test executable covers CRUD behavior, binary keys and values, concurrent access, WAL recovery, crash-tail truncation, corruption detection, incremental protocol parsing, pipelining, and malformed requests.

```sh
ctest --test-dir build --output-on-failure
```

## Benchmark

`tinykv-bench` preloads a keyspace, then runs a deterministic 90% read / 10% write workload across the requested worker count. Preload time is excluded. The default is 200,000 operations per thread, 10,000 keys, and up to 32 hardware threads. The default benchmark is memory-only; optionally pass a WAL path to include persistence overhead.

```sh
./build/tinykv-bench --threads 8 --operations 200000 --keys 10000
./build/tinykv-bench --threads 4 --operations 50000 --keys 1000 --wal ./bench.wal --no-sync
```

For comparable results, record the compiler and version, build type, CPU, operating system, thread count, key space, and persistence mode. The benchmark is intended for local comparisons, not as a claim about production database throughput.

## Current scope

TinyKV is an educational single-node database. It does not provide authentication, TLS, network access control, transactions across multiple commands, expiration, replication, compaction, or production-grade observability. Keep it bound to loopback unless you add appropriate network protections.

## License

MIT; see [LICENSE](LICENSE).

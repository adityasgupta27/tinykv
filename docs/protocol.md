# TinyKV wire protocol

TinyKV accepts RESP2 command arrays whose elements are bulk strings. The parser is incremental, so array headers, bulk bodies, and terminators may arrive across separate TCP packets. Several complete requests may also be sent in one packet.

## Request format

A request has the following structure:

```text
*<argument-count>\r\n
$<byte-length>\r\n<argument bytes>\r\n
... one bulk string per argument ...
```

Lengths count bytes, not Unicode characters. Embedded NUL and CR/LF bytes are allowed inside bulk string bodies. The command name is case-insensitive; keys and values are byte strings and are case-sensitive.

Example `SET color blue` request:

```text
*3\r\n$3\r\nSET\r\n$5\r\ncolor\r\n$4\r\nblue\r\n
```

## Limits

- At most 16 arguments per request.
- At most 16 MiB per bulk string.
- At most 32 MiB for one encoded request.
- Requests must use an array of non-null bulk strings. Inline commands, nested arrays, and null bulk arguments are rejected.

Malformed requests receive an error response when possible, then the server closes that client connection.

## Responses

The server uses RESP simple strings (`+`), errors (`-`), integers (`:`), bulk strings (`$`), and null bulk strings (`$-1`). Values are binary-safe on the wire.

```text
PING              -> +PONG\r\n
SET k v           -> +OK\r\n
GET k, value=v    -> $1\r\nv\r\n
GET absent        -> $-1\r\n
DEL k, existed    -> :1\r\n
DEL k, missing    -> :0\r\n
```

The server executes pipelined requests in order and emits one response per request. It does not support RESP transactions, subscriptions, or nested response arrays.

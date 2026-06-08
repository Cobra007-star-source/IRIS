# IRIS — TechEmpower harness

This directory documents how the IRIS gateway plugs into the
[TechEmpower Framework Benchmarks](https://github.com/TechEmpower/FrameworkBenchmarks)
(TFB). The two files TFB consumes live at the repo root so the repo can be
dropped in as a framework directory unchanged:

- [`../iris.dockerfile`](../iris.dockerfile) — builds only the `iris-gw` target.
- [`../benchmark_config.json`](../benchmark_config.json) — declares the
  `/json` and `/plaintext` test URLs (Platform classification, no database).

## Routes implemented (network-bound tracks)

| Track        | URL          | Response                                   | Content-Type       |
|--------------|--------------|--------------------------------------------|--------------------|
| JSON         | `/json`      | `{"message":"Hello, World!"}` (27 bytes)   | `application/json` |
| Plaintext    | `/plaintext` | `Hello, World!` (13 bytes)                 | `text/plain`       |

DB / Fortunes tracks are a deferred second beachhead (see the plan).

## TFB correctness rules satisfied

- **JSON**: object is serialized per request by an actual JSON serializer
  (`gateway/main.cpp: serialize_message`), not a constant; `Content-Type:
  application/json`; `Content-Length` matches the serialized bytes.
- **Plaintext**: exact body `Hello, World!`; `Content-Type: text/plain`;
  HTTP pipelining supported (TFB sends 16 pipelined requests per write).
- Every response carries `Server`, `Date` (RFC 7231 IMF-fixdate, refreshed
  1 Hz), `Content-Type`, and `Content-Length`. Keep-alive is honored for both
  HTTP/1.1 (default) and HTTP/1.0 (`Connection: keep-alive` echoed).

## Run locally

```bash
# build + run
cmake -B build -DCMAKE_BUILD_TYPE=Release -DIRIS_BUILD_GATEWAY=ON
cmake --build build --target iris-gw -j
./build/gateway/iris-gw --port 8080

# load test (wrk preferred, ApacheBench fallback)
scripts/loadtest.sh 127.0.0.1 8080 15 256 4
```

## Run via Docker (as TFB does)

```bash
docker build -f iris.dockerfile -t iris-gw .
docker run --rm -p 8080:8080 iris-gw
curl localhost:8080/plaintext
curl localhost:8080/json
```

> Loopback numbers are functional sanity only. Real TFB scores require a
> bare-metal Linux run with a dedicated load-generator NIC; macOS Docker
> virtualizes the network and caps throughput.

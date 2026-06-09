# IRIS × Bowtie

[Bowtie](https://bowtie.report) is the independent, public harness that runs
every JSON Schema implementation against the official test suite and publishes
the results side by side. This directory contains everything needed to put
IRIS on bowtie.report.

IRIS reaches **1295/1295 (100%)** on the draft-2020-12 suite. The gateway never
calls the validator, so Bowtie is the *only* place that conformance becomes a
verifiable, third-party number rather than a self-claim.

## Pieces

| File | Role |
|------|------|
| `../bench/bowtie_iris.cpp` | The IHOP stdio harness (the binary Bowtie drives). |
| `../bench/bowtie_local_check.py` | Local driver: replays the official suite through the harness over the real protocol — no Docker, no bowtie CLI. |
| `Dockerfile` | Alpine image. Drop-in for `implementations/cpp-iris/Dockerfile` in the Bowtie repo. |

Key correctness properties of the harness (see the file header for the why):
- routes through `Validator::from_schema_json` (fast↔slow auto-fallback), **not**
  the fast-path-only entry — otherwise most cases would report unsupported;
- consumes the inline `case.registry` via `add_remote_document`, so `$ref` /
  remote-document tests resolve.

## 1. Local verification (no Docker)

```bash
cmake -B build-bowtie -DCMAKE_BUILD_TYPE=Release \
  -DIRIS_BUILD_SIMDJSON=OFF -DIRIS_BUILD_GATEWAY=OFF \
  -DIRIS_BUILD_TESTS=OFF -DIRIS_BUILD_EXAMPLES=OFF -DIRIS_ENABLE_JIT=OFF
cmake --build build-bowtie --target bowtie_iris -j

python3 bench/bowtie_local_check.py            # expect: 1295/1295 = 100.00%
python3 bench/bowtie_local_check.py --verbose   # per-failure detail if any
```

## 2. Build + run the image (final pre-PR gate)

Confirms the harness compiles under musl/Alpine and starts cleanly:

The image builds from a fresh clone of GitHub, so push your changes first (or
point `IRIS_REF` at your branch): `--build-arg IRIS_REF=<branch>`.

```bash
docker build -t cpp-iris -f bowtie/Dockerfile .   # builds from a fresh clone of IRIS@main
printf '%s\n%s\n%s\n' \
  '{"cmd":"start","version":1}' \
  '{"cmd":"dialect","dialect":"https://json-schema.org/draft/2020-12/schema"}' \
  '{"cmd":"stop"}' | docker run --rm -i cpp-iris
```

Or, with Bowtie installed (`pipx install bowtie-json-schema`), the real thing:

```bash
bowtie suite -i localhost/cpp-iris \
  https://github.com/json-schema-org/JSON-Schema-Test-Suite/tree/main/tests/draft2020-12 \
  | bowtie summary --show failures
```

## 3. Submit upstream to Bowtie

Per the [implementer guide](https://docs.bowtie.report/en/stable/implementers/):

1. Fork `bowtie-json-schema/bowtie`.
2. Create `implementations/cpp-iris/` and copy `bowtie/Dockerfile` there as
   `Dockerfile` (pin `IRIS_REF` to a tagged release before submitting).
3. Keep the harness small; it lives in this repo and the image clones it.
4. Open the PR against `implementations/`. CI builds the image and runs the
   suite; results then appear on bowtie.report.

License: IRIS is AGPL-3.0; the harness metadata reports it in the `start`
response `links`.

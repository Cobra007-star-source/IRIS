# =============================================================================
# iris.dockerfile  --  TechEmpower image for the IRIS gateway
#
# Builds only the `iris-gw` target (and its deps: iris_net + picohttpparser +
# libpq); the JSON Schema validator core is not compiled for the benchmark
# image. Runtime image is a slim Ubuntu carrying the binary + libpq.
# =============================================================================
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        g++ cmake make git ca-certificates libpq-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Gateway-only configuration: skip validator extras and their network fetches
# (simdjson/bowtie). picohttpparser is fetched at configure time.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DIRIS_BUILD_GATEWAY=ON -DIRIS_PROFILE=racing -DIRIS_BUILD_DB=ON \
        -DIRIS_BUILD_EXAMPLES=OFF -DIRIS_BUILD_BENCHMARKS=OFF \
        -DIRIS_BUILD_TESTS=OFF -DIRIS_BUILD_SIMDJSON=OFF -DIRIS_BUILD_BOWTIE=OFF \
 && cmake --build build --target iris-gw -j "$(nproc)"

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends libpq5 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/gateway/iris-gw /usr/local/bin/iris-gw

# TFB runs the database on a separate host reachable as `tfb-database`.
ENV IRIS_DB="host=tfb-database user=benchmarkdbuser password=benchmarkdbpass dbname=hello_world"

EXPOSE 8080
# workers=auto (one per core), SO_REUSEPORT + pinned (racing profile).
CMD ["iris-gw", "--port", "8080", "--db-pool", "16"]

# Build stage
FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt .
COPY include include
COPY src src
COPY tests tests
COPY benchmarks benchmarks

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel --target flashkv_server

# Runtime stage
FROM debian:bookworm-slim

RUN useradd --system --create-home flashkv \
    && mkdir /data && chown flashkv /data
COPY --from=build /src/build/flashkv_server /usr/local/bin/flashkv_server

USER flashkv
WORKDIR /data
VOLUME /data
EXPOSE 6379

ENTRYPOINT ["flashkv_server", "--bind", "0.0.0.0", "--aof", "/data/flashkv.aof"]

# UNTESTED: written without Docker available. Build and check before relying on it.
#   docker build -t phylka .
#   docker run -d --name phylka -p 7333:7333 -v phylka-data:/data phylka
# The RPC/explorer port (7334) listens on 127.0.0.1 inside the container only and
# is deliberately not published. Use `docker exec` to reach it.

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel "$(nproc)" \
 && ./build/phylka selftest > /dev/null

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /data --shell /usr/sbin/nologin phylka \
 && mkdir /data && chown phylka:phylka /data
COPY --from=build /src/build/phylka /usr/local/bin/phylka
USER phylka
ENV HOME=/data
VOLUME /data
EXPOSE 7333
STOPSIGNAL SIGINT
ENTRYPOINT ["phylka"]
CMD ["node", "--datadir", "/data", "--public"]

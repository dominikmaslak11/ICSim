# ICSim headless Docker image
# Build:  docker build -t icsim .
# Run:    docker run --rm -p 8080:8080 -p 23:23 icsim
#
# This image runs the ICSim CAN simulator in headless mode alongside
# the GVRET bridge (SavvyCAN, port 23) and the WebSocket bridge
# (browser dashboard, port 8080).  All three share the same virtual
# CAN bus (vcan0).

FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc meson ninja-build libsdl2-dev libsdl2-image-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN meson setup builddir \
    && meson compile -C builddir

# ---- runtime image ----
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsdl2-2.0-0 libsdl2-image-2.0-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/builddir/icsim /app/
COPY --from=builder /src/builddir/controls /app/
COPY --from=builder /src/builddir/savvycan_bridge /app/
COPY --from=builder /src/builddir/websocket_bridge /app/
COPY --from=builder /src/builddir/cansend /app/
COPY --from=builder /src/builddir/candump /app/
COPY --from=builder /src/data/ /app/data/
COPY --from=builder /src/models/ /app/models/
COPY --from=builder /src/scenarios/ /app/scenarios/

EXPOSE 23 8080

# Start script: launch bridges, then run icsim in headless mode
COPY docker-entrypoint.sh /app/
RUN chmod +x /app/docker-entrypoint.sh

ENTRYPOINT ["/app/docker-entrypoint.sh"]
CMD ["vcan0"]

#!/bin/sh
# ICSim Docker entrypoint — starts bridges then runs headless icsim
BUS="${1:-vcan0}"

echo "ICSim Docker — bus: $BUS"

# Start GVRET bridge (SavvyCAN) in background
/app/savvycan_bridge "$BUS" &
GVRET_PID=$!
echo "GVRET bridge PID: $GVRET_PID (port 23)"

# Start WebSocket bridge (browser dashboard) in background
/app/websocket_bridge "$BUS" &
WS_PID=$!
echo "WebSocket bridge PID: $WS_PID (port 8080)"

# Give bridges a moment to bind
sleep 1

# Run ICSim in headless mode (foreground — container stays alive)
echo "Starting ICSim headless..."
exec /app/icsim --headless "$BUS"

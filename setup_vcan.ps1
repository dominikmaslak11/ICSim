# Windows does not expose SocketCAN/vcan interfaces.
# This port uses a built-in virtual CAN bus over local UDP multicast instead.
# Use the same bus name in both programs; no kernel driver or admin setup is required.

param(
    [string]$BusName = "vcan0"
)

Write-Host "Virtual CAN bus '$BusName' is provided by the Windows ICSim binaries."
Write-Host "Run these in two terminals:"
Write-Host "  .\builddir\icsim.exe $BusName"
Write-Host "  .\builddir\controls.exe -X $BusName"
Write-Host ""
Write-Host "For SavvyCAN visibility, run:"
Write-Host "  .\builddir\savvycan_bridge.exe $BusName"
Write-Host "Then add a GVRET TCP/remote connection to 127.0.0.1 in SavvyCAN."

param(
    [string]$Bus = "vcan0",
    [string]$BuildDir = (Join-Path $PSScriptRoot "builddir"),
    [string]$SavvyCANPath = "C:\Users\dominik\Downloads\SavvyCAN-Windows_x64\SavvyCAN.exe",
    [switch]$NoNoise,
    [switch]$NoSavvyCAN,
    [switch]$NoBridgeStats,
    [switch]$KeepExisting
)

$ErrorActionPreference = "Stop"

function Resolve-Tool {
    param([string]$Name)
    $path = Join-Path $BuildDir $Name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing $Name in $BuildDir. Build first with: meson compile -C builddir"
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Stop-ExistingTools {
    $names = @("icsim_imgui", "savvycan_bridge")
    if (-not $NoSavvyCAN) {
        $names += "SavvyCAN"
    }

    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $names -contains $_.ProcessName } |
        ForEach-Object {
            try {
                Stop-Process -Id $_.Id -Force -ErrorAction Stop
                Write-Host "Stopped $($_.ProcessName) pid=$($_.Id)"
            } catch {
                Write-Warning "Could not stop $($_.ProcessName) pid=$($_.Id): $_"
            }
        }
}

function Start-App {
    param(
        [string]$Label,
        [string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = $BuildDir
    )

    $proc = Start-Process -FilePath $FilePath `
        -ArgumentList $ArgumentList `
        -WorkingDirectory $WorkingDirectory `
        -PassThru
    Write-Host ("Started {0,-18} pid={1}" -f $Label, $proc.Id)
}

$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$dashboard = Resolve-Tool "icsim_imgui.exe"
$bridge = Resolve-Tool "savvycan_bridge.exe"

if (-not $KeepExisting) {
    Stop-ExistingTools
}

$bridgeArgs = @()
if (-not $NoBridgeStats) {
    $bridgeArgs += "--stats"
}
$bridgeArgs += $Bus

$controlsArgs = @("--controls")
if ($NoNoise) {
    $controlsArgs += "-X"
}
$controlsArgs += $Bus

Start-App -Label "GVRET bridge" -FilePath $bridge -ArgumentList $bridgeArgs
Start-Sleep -Milliseconds 300
Start-App -Label "Dashboard" -FilePath $dashboard -ArgumentList @($Bus)
Start-Sleep -Milliseconds 300
Start-App -Label "Controls" -FilePath $dashboard -ArgumentList $controlsArgs

if (-not $NoSavvyCAN) {
    if (Test-Path -LiteralPath $SavvyCANPath) {
        Start-App -Label "SavvyCAN" `
            -FilePath (Resolve-Path -LiteralPath $SavvyCANPath).Path `
            -WorkingDirectory (Split-Path -Parent $SavvyCANPath)
    } else {
        Write-Warning "SavvyCAN not found: $SavvyCANPath"
    }
}

Write-Host ""
Write-Host "SavvyCAN GVRET: 127.0.0.1:23, bus 0, bitrate 500000"
Write-Host "Bus: $Bus"
Write-Host ("Background CAN noise: {0}" -f ($(if ($NoNoise) { "off" } else { "on" })))

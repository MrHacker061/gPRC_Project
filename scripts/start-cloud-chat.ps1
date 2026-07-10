param(
    [string]$ChatGrpcAddress = "127.0.0.1:50051",
    [string]$GameGrpcAddress = "127.0.0.1:50052",
    [int]$WebPort = 8080,
    [string]$Database = "chat_history.db"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$CMake = "cmake"
$CMakeFallback = "C:\Program Files\CMake\bin\cmake.exe"

if (-not (Get-Command $CMake -ErrorAction SilentlyContinue)) {
    if (Test-Path $CMakeFallback) {
        $CMake = $CMakeFallback
    } else {
        throw "CMake was not found. Install CMake or add it to PATH."
    }
}

if (-not (Get-Command "cloudflared" -ErrorAction SilentlyContinue)) {
    throw "cloudflared was not found. Install it with: winget install --id Cloudflare.cloudflared"
}

Push-Location $RepoRoot
try {
    Write-Host "Building Release..." -ForegroundColor Cyan
    & $CMake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
    & $CMake --build build --config Release

    Write-Host "Stopping old project processes..." -ForegroundColor Cyan
    Get-Process chat_server, game_server, web_gateway, cloudflared -ErrorAction SilentlyContinue | Stop-Process -Force

    $ChatServerExe = Join-Path $RepoRoot "build\Release\chat_server.exe"
    $GameServerExe = Join-Path $RepoRoot "build\Release\game_server.exe"
    $GatewayExe = Join-Path $RepoRoot "build\Release\web_gateway.exe"
    $GatewayUrl = "http://localhost:$WebPort"

    Write-Host "Starting chat gRPC server on $ChatGrpcAddress..." -ForegroundColor Cyan
    $chatServer = Start-Process -FilePath $ChatServerExe `
        -ArgumentList @($ChatGrpcAddress, $Database) `
        -WorkingDirectory $RepoRoot `
        -PassThru

    Write-Host "Starting game gRPC server on $GameGrpcAddress..." -ForegroundColor Cyan
    $gameServer = Start-Process -FilePath $GameServerExe `
        -ArgumentList @($GameGrpcAddress) `
        -WorkingDirectory $RepoRoot `
        -PassThru

    Start-Sleep -Seconds 1

    Write-Host "Starting web gateway on $GatewayUrl..." -ForegroundColor Cyan
    $gateway = Start-Process -FilePath $GatewayExe `
        -ArgumentList @($ChatGrpcAddress, $WebPort, $GameGrpcAddress) `
        -WorkingDirectory $RepoRoot `
        -PassThru

    Start-Sleep -Seconds 1

    $LogFile = Join-Path $RepoRoot "cloudflared.log"
    if (Test-Path $LogFile) {
        Remove-Item -LiteralPath $LogFile -Force
    }

    Write-Host "Starting Cloudflare Tunnel..." -ForegroundColor Cyan
    $tunnel = Start-Process -FilePath "cloudflared" `
        -ArgumentList @("tunnel", "--url", $GatewayUrl, "--logfile", $LogFile) `
        -WorkingDirectory $RepoRoot `
        -PassThru

    $publicUrl = $null
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $LogFile) {
            $content = Get-Content -LiteralPath $LogFile -Raw -ErrorAction SilentlyContinue
            $match = [regex]::Match($content, "https://[a-zA-Z0-9-]+\.trycloudflare\.com")
            if ($match.Success) {
                $publicUrl = $match.Value
                break
            }
        }
        Start-Sleep -Milliseconds 500
    }

    Write-Host ""
    Write-Host "Chat and Box Arena are running." -ForegroundColor Green
    Write-Host "Local chat URL: $GatewayUrl"
    Write-Host "Local game URL: $GatewayUrl/game"
    if ($publicUrl) {
        Write-Host "Cloudflare chat URL: $publicUrl" -ForegroundColor Green
        Write-Host "Cloudflare game URL: $publicUrl/game" -ForegroundColor Green
    } else {
        Write-Host "Cloudflare URL was not detected yet. Check: $LogFile" -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "Process IDs:"
    Write-Host "  chat_server: $($chatServer.Id)"
    Write-Host "  game_server: $($gameServer.Id)"
    Write-Host "  web_gateway: $($gateway.Id)"
    Write-Host "  cloudflared: $($tunnel.Id)"
    Write-Host ""
    Write-Host "Press Ctrl+C in this terminal to stop all four processes."

    try {
        while ($true) {
            Start-Sleep -Seconds 2
            if ($chatServer.HasExited -or $gameServer.HasExited -or $gateway.HasExited -or $tunnel.HasExited) {
                throw "One of the project processes stopped unexpectedly."
            }
        }
    } finally {
        Get-Process -Id @($chatServer.Id, $gameServer.Id, $gateway.Id, $tunnel.Id) -ErrorAction SilentlyContinue | Stop-Process -Force
    }
} finally {
    Pop-Location
}

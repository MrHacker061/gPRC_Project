param(
    [string]$OutputZip = "native_game_client.zip"
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

Push-Location $RepoRoot
try {
    & $CMake --build build --config Release --target native_game_client

    $PackageDir = Join-Path $RepoRoot "build\native_client_package"
    if (Test-Path $PackageDir) {
        Remove-Item -LiteralPath $PackageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $PackageDir | Out-Null

    Copy-Item -LiteralPath (Join-Path $RepoRoot "build\Release\native_game_client.exe") -Destination $PackageDir
    Copy-Item -Path (Join-Path $RepoRoot "build\Release\*.dll") -Destination $PackageDir

    @"
Box Arena Native gRPC Client

1. Run game_server somewhere reachable by this computer.
2. Start native_game_client.exe.
3. Enter the game_server gRPC address, for example:
   127.0.0.1:50052
4. Enter a name and click Connect.
5. Move with WASD or arrow keys.
6. Aim with the mouse and left-click nearby rocks to mine stone.

This native client connects directly to game_server with gRPC. It does not use web_gateway.
"@ | Set-Content -LiteralPath (Join-Path $PackageDir "README.txt")

    $ZipPath = Join-Path $RepoRoot $OutputZip
    if (Test-Path $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath $ZipPath
    Write-Host "Created $ZipPath" -ForegroundColor Green
} finally {
    Pop-Location
}

# gRPC Browser Chat

A C++ chat app where browser users join through a webpage, while the backend uses gRPC.

```text
Browser users
  -> Cloudflare public URL
  -> web_gateway on localhost:8080
  -> gRPC chat_server on localhost:50051
  -> SQLite chat_history.db
```

The browser does not talk to gRPC directly. It talks to `web_gateway`, and the gateway talks to `chat_server` using gRPC.

## 1. Install Tools

Install these on the machine that will run the server:

- Visual Studio 2022 Build Tools with C++ tools
- CMake
- Git
- vcpkg
- cloudflared

Useful install commands:

```powershell
winget install --id Kitware.CMake
winget install --id Cloudflare.cloudflared
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Set up vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Open a new PowerShell window after installing tools.

## 2. Build

From the repo folder:

```powershell
cd "C:\Users\bocaj\OneDrive\Desktop\gRPC Project\gPRC_Project"

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

If CMake was previously run incorrectly, delete `build` and try again:

```powershell
Remove-Item -Recurse -Force build
```

## Quick Start

To rebuild everything, start the gRPC server, start the web gateway, start Cloudflare Tunnel, and print the public Cloudflare URL:

```powershell
cd "C:\Users\bocaj\OneDrive\Desktop\gRPC Project\gPRC_Project"
powershell -ExecutionPolicy Bypass -File .\scripts\start-cloud-chat.ps1
```

## 3. Start The gRPC Server

Open a PowerShell window:

```powershell
cd "C:\Users\bocaj\OneDrive\Desktop\gRPC Project\gPRC_Project"
.\build\Release\chat_server.exe
```

The server listens on:

```text
localhost:50051
```

Chat history is saved to:

```text
chat_history.db
```

To choose a custom database file:

```powershell
.\build\Release\chat_server.exe 0.0.0.0:50051 my-chat.db
```

## 4. Start The Web Gateway

Open a second PowerShell window:

```powershell
cd "C:\Users\bocaj\OneDrive\Desktop\gRPC Project\gPRC_Project"
.\build\Release\web_gateway.exe localhost:50051 8080
```

The gateway listens on:

```text
http://localhost:8080
```

## 5. Start Cloudflare Tunnel

Open a third PowerShell window:

```powershell
cloudflared tunnel --url http://localhost:8080
```

Cloudflare will print a public URL like:

```text
https://example-random-name.trycloudflare.com
```

Send that URL to people. They can join from a browser without downloading anything.

## Important

Tunnel this:

```text
http://localhost:8080
```

Do not tunnel this:

```text
localhost:50051
```

`8080` is the webpage gateway. `50051` is the internal gRPC server.

## Features

- Browser-based chat
- C++ gRPC backend
- SQLite chat history
- Public rooms that update in the room list
- Private rooms that require a room name and password
- Typing indicators
- Cloudflare Tunnel support

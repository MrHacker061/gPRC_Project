# gRPC Browser Chat and Box Arena

A C++ project with a gRPC backend, a browser chat app, and a small real-time top-down multiplayer game.

```text
Browser users
  -> Cloudflare public URL
  -> web_gateway on localhost:8080
  -> chat_server on localhost:50051
  -> game_server on localhost:50052
```

Browsers do not talk to gRPC directly. They talk to `web_gateway`; the gateway talks to the C++ gRPC servers.

## Cloud Server Setup

Install the required tools on the machine that will host the project:

```powershell
winget install --id Kitware.CMake
winget install --id Cloudflare.cloudflared
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Install vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Open a new PowerShell window, then run:

```powershell
cd "C:\Users\bocaj\OneDrive\Desktop\gRPC Project\gPRC_Project"
powershell -ExecutionPolicy Bypass -File .\scripts\start-cloud-chat.ps1
```

That command rebuilds the project, starts:

- `chat_server.exe`
- `game_server.exe`
- `web_gateway.exe`
- `cloudflared`

It also prints public Cloudflare URLs:

```text
Cloudflare chat URL: https://example.trycloudflare.com
Cloudflare game URL: https://example.trycloudflare.com/game
```

Send the `/game` URL to people who should join the top-down box arena.

## Important Ports

Tunnel only the web gateway:

```text
http://localhost:8080
```

Do not tunnel the gRPC ports directly:

```text
localhost:50051
localhost:50052
```

## Features

- Browser chat
- SQLite chat history
- Public and private chat rooms
- Typing indicators
- Real-time top-down multiplayer game
- WASD player movement
- Cloudflare Tunnel support

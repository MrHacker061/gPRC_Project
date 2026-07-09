# gRPC Browser Chat

A small C++ chat project that uses gRPC for the core chat service and exposes a normal webpage so other people can join from a browser.

## Architecture

```text
Browser users
    |
    | HTTP form posts + Server-Sent Events
    v
web_gateway  (C++ / cpp-httplib)
    |
    | gRPC
    v
chat_server  (C++ / gRPC)
    ^
    |
chat_cli     (C++ / gRPC terminal client)
```

Browsers do not speak native gRPC directly, so `web_gateway` serves `web/index.html` and bridges browser traffic to the gRPC server.

## Project layout

- `proto/chat.proto` defines the gRPC service.
- `src/chat_server.cpp` runs the chat room server.
- `src/chat_cli.cpp` runs a terminal chat client.
- `src/web_gateway.cpp` serves the browser page and talks to gRPC.
- `web/index.html` is the browser chat UI.
- `vcpkg.json` lists the C++ dependencies.

## Prerequisites

Install these first:

- Visual Studio 2022 with the C++ desktop workload
- CMake 3.24 or newer
- Git
- vcpkg

Example vcpkg setup:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

## Build

From the repo root:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

The first build can take a while because vcpkg builds gRPC and protobuf.

## Run locally

Open three terminals from the repo root.

Start the gRPC server:

```powershell
.\build\Release\chat_server.exe
```

Start the browser gateway:

```powershell
.\build\Release\web_gateway.exe localhost:50051 8080
```

Open the webpage:

```text
http://localhost:8080
```

Optionally join from the terminal client:

```powershell
.\build\Release\chat_cli.exe localhost:50051 Jacob lobby
```

## Let others join

Keep `chat_server.exe` and `web_gateway.exe` running on your machine. Other people on the same network can open:

```text
http://YOUR_LOCAL_IP:8080
```

On Windows, you may need to allow the gateway through the firewall. You can find your local IP with:

```powershell
ipconfig
```

Look for the IPv4 address on your active Wi-Fi or Ethernet adapter.

## Next feature ideas

- Persist chat history to SQLite.
- Add multiple rooms to the UI.
- Add private messages.
- Add TLS for the gRPC server and HTTPS for the gateway.
- Add authentication or invite codes before exposing it outside your local network.

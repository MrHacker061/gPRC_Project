#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <grpcpp/grpcpp.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "game.grpc.pb.h"

namespace {

using game::GameService;
using game::JoinGameRequest;
using game::JoinGameResponse;
using game::LeaveGameRequest;
using game::LeaveGameResponse;
using game::PlayerInput;
using game::WorldSnapshot;
using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReaderWriter;
using grpc::Status;

constexpr int kTimerId = 1;
constexpr int kConnectButtonId = 1001;
constexpr int kServerEditId = 1002;
constexpr int kNameEditId = 1003;
constexpr float kPlayerSpeed = 230.0f;

HWND g_main_window = nullptr;
HWND g_server_edit = nullptr;
HWND g_name_edit = nullptr;
HWND g_connect_button = nullptr;
HWND g_status_label = nullptr;

struct PredictedPlayer {
  std::string id;
  std::string name;
  std::string color;
  float x = 0.0f;
  float y = 0.0f;
  float size = 28.0f;
};

class NativeGameClient {
 public:
  ~NativeGameClient() { Disconnect(); }

  bool IsConnected() const { return connected_; }

  bool Connect(const std::string& address,
               const std::string& display_name,
               std::string* error) {
    Disconnect();

    address_ = address.empty() ? "127.0.0.1:50052" : address;
    display_name_ = display_name.empty() ? "Native Player" : display_name;

    auto channel =
        grpc::CreateChannel(address_, grpc::InsecureChannelCredentials());
    stub_ = GameService::NewStub(channel);

    JoinGameRequest join_request;
    join_request.set_display_name(display_name_);
    JoinGameResponse join_response;
    ClientContext join_context;
    const Status join_status =
        stub_->JoinGame(&join_context, join_request, &join_response);
    if (!join_status.ok() || join_response.player_id().empty()) {
      if (error != nullptr) {
        *error = join_status.ok() ? "Game server rejected the join request."
                                  : join_status.error_message();
      }
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      player_id_ = join_response.player_id();
      arena_width_ = join_response.arena_width();
      arena_height_ = join_response.arena_height();
      latest_snapshot_.Clear();
      predicted_self_ = {};
      predicted_ready_ = false;
    }

    play_context_ = std::make_unique<ClientContext>();
    play_stream_ = stub_->Play(play_context_.get());
    running_ = true;
    connected_ = true;

    reader_thread_ = std::thread([this] { ReadSnapshots(); });
    SendInput();
    return true;
  }

  void Disconnect() {
    if (!connected_ && !running_) {
      return;
    }

    running_ = false;

    {
      std::lock_guard<std::mutex> lock(stream_mutex_);
      if (play_stream_) {
        play_stream_->WritesDone();
      }
    }

    if (play_context_) {
      play_context_->TryCancel();
    }

    if (reader_thread_.joinable()) {
      reader_thread_.join();
    }

    if (play_stream_) {
      play_stream_->Finish();
      play_stream_.reset();
    }

    std::string leaving_id;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      leaving_id = player_id_;
      player_id_.clear();
      latest_snapshot_.Clear();
      predicted_ready_ = false;
    }

    if (stub_ && !leaving_id.empty()) {
      LeaveGameRequest leave_request;
      leave_request.set_player_id(leaving_id);
      LeaveGameResponse leave_response;
      ClientContext leave_context;
      stub_->LeaveGame(&leave_context, leave_request, &leave_response);
    }

    play_context_.reset();
    stub_.reset();
    connected_ = false;
  }

  void SetKey(WPARAM key, bool down) {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(input_mutex_);
      switch (key) {
        case 'W':
        case VK_UP:
          changed = input_.up() != down;
          input_.set_up(down);
          break;
        case 'S':
        case VK_DOWN:
          changed = input_.down() != down;
          input_.set_down(down);
          break;
        case 'A':
        case VK_LEFT:
          changed = input_.left() != down;
          input_.set_left(down);
          break;
        case 'D':
        case VK_RIGHT:
          changed = input_.right() != down;
          input_.set_right(down);
          break;
        default:
          return;
      }
    }

    if (changed) {
      SendInput();
    }
  }

  void TickPrediction(float dt) {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (!predicted_ready_) {
      return;
    }

    PlayerInput input;
    {
      std::lock_guard<std::mutex> input_lock(input_mutex_);
      input = input_;
    }

    float dx = 0.0f;
    float dy = 0.0f;
    if (input.left()) dx -= 1.0f;
    if (input.right()) dx += 1.0f;
    if (input.up()) dy -= 1.0f;
    if (input.down()) dy += 1.0f;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length > 0.0f) {
      dx /= length;
      dy /= length;
    }

    const float half = predicted_self_.size / 2.0f;
    predicted_self_.x = std::clamp(predicted_self_.x + dx * kPlayerSpeed * dt,
                                   half, arena_width_ - half);
    predicted_self_.y = std::clamp(predicted_self_.y + dy * kPlayerSpeed * dt,
                                   half, arena_height_ - half);
  }

  WorldSnapshot Snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return latest_snapshot_;
  }

  PredictedPlayer Self() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return predicted_self_;
  }

  bool HasPredictedSelf() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return predicted_ready_;
  }

  std::string PlayerId() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return player_id_;
  }

  float ArenaWidth() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return arena_width_;
  }

  float ArenaHeight() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return arena_height_;
  }

 private:
  void SendInput() {
    if (!connected_) {
      return;
    }

    PlayerInput input;
    {
      std::lock_guard<std::mutex> lock(input_mutex_);
      input = input_;
      input.set_sequence(++sequence_);
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      input.set_player_id(player_id_);
    }

    std::lock_guard<std::mutex> lock(stream_mutex_);
    if (play_stream_) {
      play_stream_->Write(input);
    }
  }

  void ReadSnapshots() {
    WorldSnapshot snapshot;
    while (running_ && play_stream_ && play_stream_->Read(&snapshot)) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        latest_snapshot_ = snapshot;

        for (const auto& player : snapshot.players()) {
          if (player.player_id() != player_id_) {
            continue;
          }

          if (!predicted_ready_) {
            predicted_self_.id = player.player_id();
            predicted_self_.name = player.display_name();
            predicted_self_.color = player.color();
            predicted_self_.x = player.x();
            predicted_self_.y = player.y();
            predicted_self_.size = player.size();
            predicted_ready_ = true;
          } else {
            const float dx = player.x() - predicted_self_.x;
            const float dy = player.y() - predicted_self_.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > 400.0f) {
              predicted_self_.x = player.x();
              predicted_self_.y = player.y();
            } else if (!HasMovementInputLocked() && distance > 4.0f) {
              predicted_self_.x += dx * 0.04f;
              predicted_self_.y += dy * 0.04f;
            }
            predicted_self_.id = player.player_id();
            predicted_self_.name = player.display_name();
            predicted_self_.color = player.color();
            predicted_self_.size = player.size();
          }
        }
      }

      if (g_main_window != nullptr) {
        InvalidateRect(g_main_window, nullptr, FALSE);
      }
    }
  }

  bool HasMovementInputLocked() const {
    std::lock_guard<std::mutex> lock(input_mutex_);
    return input_.up() || input_.down() || input_.left() || input_.right();
  }

  std::string address_;
  std::string display_name_;
  std::unique_ptr<GameService::Stub> stub_;
  std::unique_ptr<ClientContext> play_context_;
  std::unique_ptr<ClientReaderWriter<PlayerInput, WorldSnapshot>> play_stream_;
  std::thread reader_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  mutable std::mutex state_mutex_;
  mutable std::mutex input_mutex_;
  std::mutex stream_mutex_;
  std::string player_id_;
  WorldSnapshot latest_snapshot_;
  PredictedPlayer predicted_self_;
  bool predicted_ready_ = false;
  float arena_width_ = 960.0f;
  float arena_height_ = 620.0f;
  PlayerInput input_;
  uint64_t sequence_ = 0;
};

NativeGameClient g_client;
std::chrono::steady_clock::time_point g_last_frame =
    std::chrono::steady_clock::now();

std::string WindowText(HWND window) {
  const int length = GetWindowTextLengthA(window);
  std::string text(static_cast<std::size_t>(length) + 1, '\0');
  if (length > 0) {
    GetWindowTextA(window, text.data(), length + 1);
  }
  text.resize(static_cast<std::size_t>(length));
  return text;
}

void SetStatus(const std::string& status) {
  SetWindowTextA(g_status_label, status.c_str());
}

COLORREF ParseColor(const std::string& color) {
  if (color.size() != 7 || color[0] != '#') {
    return RGB(45, 212, 191);
  }
  unsigned int rgb = 0;
  if (std::sscanf(color.c_str() + 1, "%x", &rgb) != 1) {
    return RGB(45, 212, 191);
  }
  return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

void DrawPlayer(HDC dc,
                const RECT& arena_rect,
                float arena_width,
                float arena_height,
                const PredictedPlayer& player,
                bool is_self) {
  const float scale_x =
      static_cast<float>(arena_rect.right - arena_rect.left) / arena_width;
  const float scale_y =
      static_cast<float>(arena_rect.bottom - arena_rect.top) / arena_height;
  const float scale = std::min(scale_x, scale_y);
  const int size = static_cast<int>(player.size * scale);
  const int x = arena_rect.left + static_cast<int>(player.x * scale_x) - size / 2;
  const int y = arena_rect.top + static_cast<int>(player.y * scale_y) - size / 2;

  HBRUSH brush = CreateSolidBrush(ParseColor(player.color));
  RECT box{x, y, x + size, y + size};
  FillRect(dc, &box, brush);
  DeleteObject(brush);

  if (is_self) {
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(245, 247, 251));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, box.left - 3, box.top - 3, box.right + 3, box.bottom + 3);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
  }

  SetTextColor(dc, RGB(245, 247, 251));
  SetBkMode(dc, TRANSPARENT);
  RECT label{x - 60, y - 20, x + size + 60, y - 4};
  DrawTextA(dc, player.name.c_str(), -1, &label,
            DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Paint(HWND window) {
  PAINTSTRUCT paint;
  HDC dc = BeginPaint(window, &paint);

  RECT client;
  GetClientRect(window, &client);
  HBRUSH background = CreateSolidBrush(RGB(17, 19, 24));
  FillRect(dc, &client, background);
  DeleteObject(background);

  RECT arena{20, 92, client.right - 20, client.bottom - 20};
  HBRUSH arena_brush = CreateSolidBrush(RGB(22, 26, 34));
  FillRect(dc, &arena, arena_brush);
  DeleteObject(arena_brush);

  HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(36, 43, 54));
  HGDIOBJ old_pen = SelectObject(dc, grid_pen);
  const float arena_width = g_client.ArenaWidth();
  const float arena_height = g_client.ArenaHeight();
  const float scale_x = static_cast<float>(arena.right - arena.left) / arena_width;
  const float scale_y =
      static_cast<float>(arena.bottom - arena.top) / arena_height;
  for (float x = 0.0f; x <= arena_width; x += 80.0f) {
    const int px = arena.left + static_cast<int>(x * scale_x);
    MoveToEx(dc, px, arena.top, nullptr);
    LineTo(dc, px, arena.bottom);
  }
  for (float y = 0.0f; y <= arena_height; y += 80.0f) {
    const int py = arena.top + static_cast<int>(y * scale_y);
    MoveToEx(dc, arena.left, py, nullptr);
    LineTo(dc, arena.right, py);
  }
  SelectObject(dc, old_pen);
  DeleteObject(grid_pen);

  HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(48, 56, 69));
  old_pen = SelectObject(dc, border_pen);
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
  Rectangle(dc, arena.left, arena.top, arena.right, arena.bottom);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(border_pen);

  const std::string self_id = g_client.PlayerId();
  const WorldSnapshot snapshot = g_client.Snapshot();
  for (const auto& player : snapshot.players()) {
    if (player.player_id() == self_id && g_client.HasPredictedSelf()) {
      continue;
    }
    PredictedPlayer draw_player;
    draw_player.id = player.player_id();
    draw_player.name = player.display_name();
    draw_player.color = player.color();
    draw_player.x = player.x();
    draw_player.y = player.y();
    draw_player.size = player.size();
    DrawPlayer(dc, arena, arena_width, arena_height, draw_player, false);
  }

  if (g_client.HasPredictedSelf()) {
    DrawPlayer(dc, arena, arena_width, arena_height, g_client.Self(), true);
  }

  EndPaint(window, &paint);
}

void ConnectOrDisconnect(HWND window) {
  if (g_client.IsConnected()) {
    g_client.Disconnect();
    SetWindowTextA(g_connect_button, "Connect");
    SetStatus("Disconnected");
    InvalidateRect(window, nullptr, FALSE);
    return;
  }

  SetStatus("Connecting...");
  std::string error;
  if (!g_client.Connect(WindowText(g_server_edit), WindowText(g_name_edit),
                        &error)) {
    SetStatus("Connection failed: " + error);
    return;
  }

  SetWindowTextA(g_connect_button, "Disconnect");
  SetStatus("Connected to " + WindowText(g_server_edit));
  SetFocus(window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      CreateWindowA("STATIC", "Server", WS_CHILD | WS_VISIBLE, 20, 18, 52, 20,
                    window, nullptr, nullptr, nullptr);
      g_server_edit = CreateWindowA(
          "EDIT", "127.0.0.1:50052",
          WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 76, 14, 180, 26,
          window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kServerEditId)),
          nullptr, nullptr);
      CreateWindowA("STATIC", "Name", WS_CHILD | WS_VISIBLE, 270, 18, 42, 20,
                    window, nullptr, nullptr, nullptr);
      g_name_edit = CreateWindowA(
          "EDIT", "Native Player",
          WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 316, 14, 150, 26,
          window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNameEditId)),
          nullptr, nullptr);
      g_connect_button = CreateWindowA(
          "BUTTON", "Connect", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 13,
          110, 28, window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConnectButtonId)),
          nullptr, nullptr);
      g_status_label = CreateWindowA("STATIC", "Disconnected",
                                     WS_CHILD | WS_VISIBLE, 20, 54, 560, 24,
                                     window, nullptr, nullptr, nullptr);
      SetTimer(window, kTimerId, 16, nullptr);
      return 0;

    case WM_COMMAND:
      if (LOWORD(wparam) == kConnectButtonId) {
        ConnectOrDisconnect(window);
        return 0;
      }
      break;

    case WM_KEYDOWN:
      g_client.SetKey(wparam, true);
      return 0;

    case WM_KEYUP:
      g_client.SetKey(wparam, false);
      return 0;

    case WM_TIMER: {
      const auto now = std::chrono::steady_clock::now();
      const float dt =
          std::chrono::duration<float>(now - g_last_frame).count();
      g_last_frame = now;
      g_client.TickPrediction(std::min(dt, 0.05f));
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }

    case WM_PAINT:
      Paint(window);
      return 0;

    case WM_DESTROY:
      KillTimer(window, kTimerId);
      g_client.Disconnect();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(window, message, wparam, lparam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
  const char* class_name = "GrpcBoxArenaNativeClient";

  WNDCLASSA window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

  if (!RegisterClassA(&window_class)) {
    MessageBoxA(nullptr, "Could not register window class.", "Box Arena",
                MB_ICONERROR);
    return 1;
  }

  g_main_window = CreateWindowExA(
      0, class_name, "Box Arena Native gRPC Client",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720,
      nullptr, nullptr, instance, nullptr);

  if (g_main_window == nullptr) {
    MessageBoxA(nullptr, "Could not create main window.", "Box Arena",
                MB_ICONERROR);
    return 1;
  }

  ShowWindow(g_main_window, show_command);

  MSG message{};
  while (GetMessageA(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }

  return static_cast<int>(message.wParam);
}

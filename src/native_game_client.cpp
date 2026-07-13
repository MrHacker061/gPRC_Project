#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <grpcpp/grpcpp.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
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
constexpr int kArenaMargin = 20;
constexpr int kArenaTop = 92;
constexpr int kMinimumWindowWidth = 640;
constexpr int kMinimumWindowHeight = 480;
constexpr float kPlayerSpeed = 320.0f;
constexpr float kCameraViewWidth = 960.0f;
constexpr float kCameraViewHeight = 620.0f;
constexpr float kInputStepSeconds = 0.016f;
constexpr float kReconciliationDeadZone = 1.0f;
constexpr float kActiveCorrection = 0.35f;
constexpr float kReconciliationSnapDistance = 120.0f;

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
  float aim_x = 1.0f;
  float aim_y = 0.0f;
};

void ApplyMovement(PredictedPlayer* player,
                   const PlayerInput& input,
                   float arena_width,
                   float arena_height) {
  if (player == nullptr || input.movement_seconds() <= 0.0f) {
    return;
  }
  float dx = 0.0f;
  float dy = 0.0f;
  if (input.left()) dx -= 1.0f;
  if (input.right()) dx += 1.0f;
  if (input.up()) dy -= 1.0f;
  if (input.down()) dy += 1.0f;
  const float length = std::hypot(dx, dy);
  if (length > 0.0f) {
    dx /= length;
    dy /= length;
  }
  const float seconds = std::clamp(input.movement_seconds(), 0.0f, 0.05f);
  const float padding = player->size * 0.78f;
  player->x = std::clamp(player->x + dx * kPlayerSpeed * seconds, padding,
                         arena_width - padding);
  player->y = std::clamp(player->y + dy * kPlayerSpeed * seconds, padding,
                         arena_height - padding);
}

struct NativeRenderState {
  std::string player_id;
  WorldSnapshot snapshot;
  PredictedPlayer predicted_self;
  bool predicted_ready = false;
  float arena_width = 960.0f;
  float arena_height = 620.0f;
};

struct CameraView {
  float x = 0.0f;
  float y = 0.0f;
  float width = kCameraViewWidth;
  float height = kCameraViewHeight;
};

CameraView CameraForState(const NativeRenderState& state) {
  CameraView camera;
  camera.width = std::min(kCameraViewWidth, std::max(state.arena_width, 1.0f));
  camera.height =
      std::min(kCameraViewHeight, std::max(state.arena_height, 1.0f));
  const float focus_x =
      state.predicted_ready ? state.predicted_self.x : camera.width / 2.0f;
  const float focus_y =
      state.predicted_ready ? state.predicted_self.y : camera.height / 2.0f;
  camera.x = std::clamp(focus_x - camera.width / 2.0f, 0.0f,
                        std::max(state.arena_width - camera.width, 0.0f));
  camera.y = std::clamp(focus_y - camera.height / 2.0f, 0.0f,
                        std::max(state.arena_height - camera.height, 0.0f));
  return camera;
}

class NativeGameClient {
 public:
  NativeGameClient() { input_.set_aim_x(1.0f); }

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
      pending_inputs_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(input_mutex_);
      sequence_ = 0;
      input_dirty_ = false;
    }

    play_context_ = std::make_unique<ClientContext>();
    play_stream_ = stub_->Play(play_context_.get());
    running_ = true;
    connected_ = true;

    reader_thread_ = std::thread([this] { ReadSnapshots(); });
    SendInput(0.0f);
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
      pending_inputs_.clear();
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
      SendInput(0.0f);
    }
  }

  void SetAimDirection(float aim_x, float aim_y) {
    const float length = std::hypot(aim_x, aim_y);
    if (!std::isfinite(length) || length < 0.001f) {
      return;
    }
    aim_x /= length;
    aim_y /= length;

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!predicted_ready_) {
        return;
      }
      predicted_self_.aim_x = aim_x;
      predicted_self_.aim_y = aim_y;
    }

    std::lock_guard<std::mutex> lock(input_mutex_);
    if (std::abs(input_.aim_x() - aim_x) < 0.001f &&
        std::abs(input_.aim_y() - aim_y) < 0.001f) {
      return;
    }
    input_.set_aim_x(aim_x);
    input_.set_aim_y(aim_y);
    input_dirty_ = true;
  }

  void FlushInput() {
    bool should_send = false;
    {
      std::lock_guard<std::mutex> lock(input_mutex_);
      should_send = input_dirty_;
    }
    if (should_send) {
      SendInput(0.0f);
    }
  }

  void AdvanceMovementStep() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!predicted_ready_) {
        return;
      }
    }
    bool has_movement = false;
    {
      std::lock_guard<std::mutex> lock(input_mutex_);
      has_movement = input_.up() || input_.down() || input_.left() ||
                     input_.right();
    }
    if (has_movement) {
      SendInput(kInputStepSeconds);
    }
  }

  NativeRenderState CaptureRenderState() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    NativeRenderState state;
    state.player_id = player_id_;
    state.snapshot = latest_snapshot_;
    state.predicted_self = predicted_self_;
    state.predicted_ready = predicted_ready_;
    state.arena_width = arena_width_;
    state.arena_height = arena_height_;
    return state;
  }

 private:
  void SendInput(float movement_seconds) {
    if (!connected_) {
      return;
    }

    PlayerInput input;
    {
      std::lock_guard<std::mutex> lock(input_mutex_);
      input = input_;
      input.set_sequence(++sequence_);
      input.set_movement_seconds(movement_seconds);
      input_dirty_ = false;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      input.set_player_id(player_id_);
      if (movement_seconds > 0.0f && predicted_ready_) {
        ApplyMovement(&predicted_self_, input, arena_width_, arena_height_);
        pending_inputs_.push_back(input);
      }
    }

    std::lock_guard<std::mutex> lock(stream_mutex_);
    if (play_stream_) {
      play_stream_->Write(input);
    }
  }

  void ReadSnapshots() {
    WorldSnapshot snapshot;
    while (running_ && play_stream_ && play_stream_->Read(&snapshot)) {
      const bool has_movement = HasMovementInput();
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
            predicted_self_.aim_x = player.aim_x();
            predicted_self_.aim_y = player.aim_y();
            pending_inputs_.clear();
            predicted_ready_ = true;
          } else {
            while (!pending_inputs_.empty() &&
                   pending_inputs_.front().sequence() <=
                       player.last_processed_input_sequence()) {
              pending_inputs_.pop_front();
            }

            PredictedPlayer reconciled = predicted_self_;
            reconciled.x = player.x();
            reconciled.y = player.y();
            reconciled.size = player.size();
            for (const auto& pending : pending_inputs_) {
              ApplyMovement(&reconciled, pending, arena_width_, arena_height_);
            }

            const float dx = reconciled.x - predicted_self_.x;
            const float dy = reconciled.y - predicted_self_.y;
            const float distance = std::hypot(dx, dy);
            const bool settled = !has_movement && pending_inputs_.empty();
            if (settled || distance <= kReconciliationDeadZone ||
                distance >= kReconciliationSnapDistance) {
              predicted_self_.x = reconciled.x;
              predicted_self_.y = reconciled.y;
            } else {
              predicted_self_.x += dx * kActiveCorrection;
              predicted_self_.y += dy * kActiveCorrection;
            }
            predicted_self_.id = player.player_id();
            predicted_self_.name = player.display_name();
            predicted_self_.color = player.color();
            predicted_self_.size = player.size();
          }
        }
      }

    }
  }

  bool HasMovementInput() const {
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
  std::deque<PlayerInput> pending_inputs_;
  bool predicted_ready_ = false;
  float arena_width_ = 960.0f;
  float arena_height_ = 620.0f;
  PlayerInput input_;
  bool input_dirty_ = false;
  uint64_t sequence_ = 0;
};

NativeGameClient g_client;

class PersistentBackBuffer {
 public:
  ~PersistentBackBuffer() { Reset(); }

  bool Ensure(HDC reference_dc, int width, int height) {
    if (memory_dc_ != nullptr && bitmap_ != nullptr && width_ == width &&
        height_ == height) {
      return true;
    }

    Reset();
    if (width <= 0 || height <= 0) {
      return false;
    }

    memory_dc_ = CreateCompatibleDC(reference_dc);
    if (memory_dc_ == nullptr) {
      return false;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    bitmap_ = CreateDIBSection(reference_dc, &bitmap_info, DIB_RGB_COLORS,
                               &pixels, nullptr, 0);
    if (bitmap_ == nullptr || pixels == nullptr) {
      Reset();
      return false;
    }

    previous_bitmap_ = SelectObject(memory_dc_, bitmap_);
    if (previous_bitmap_ == nullptr || previous_bitmap_ == HGDI_ERROR) {
      previous_bitmap_ = nullptr;
      Reset();
      return false;
    }

    width_ = width;
    height_ = height;
    return true;
  }

  void Reset() {
    if (memory_dc_ != nullptr && previous_bitmap_ != nullptr) {
      SelectObject(memory_dc_, previous_bitmap_);
    }
    if (bitmap_ != nullptr) {
      DeleteObject(bitmap_);
    }
    if (memory_dc_ != nullptr) {
      DeleteDC(memory_dc_);
    }
    memory_dc_ = nullptr;
    bitmap_ = nullptr;
    previous_bitmap_ = nullptr;
    width_ = 0;
    height_ = 0;
  }

  HDC dc() const { return memory_dc_; }

 private:
  HDC memory_dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ previous_bitmap_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

PersistentBackBuffer g_back_buffer;
POINT g_mouse_target{};
bool g_has_mouse_target = false;

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

RECT ArenaRectForClient(const RECT& client) {
  RECT arena{kArenaMargin, kArenaTop, client.right - kArenaMargin,
             client.bottom - kArenaMargin};
  if (arena.right <= arena.left || arena.bottom <= arena.top) {
    SetRectEmpty(&arena);
  }
  return arena;
}

void InvalidateArena(HWND window) {
  RECT client;
  GetClientRect(window, &client);
  const RECT arena = ArenaRectForClient(client);
  if (!IsRectEmpty(&arena)) {
    InvalidateRect(window, &arena, FALSE);
  }
}

void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) {
  SetDCBrushColor(dc, color);
  FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void FillCircle(HDC dc, int center_x, int center_y, int radius,
                COLORREF color) {
  if (radius <= 0) {
    return;
  }
  HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
  HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
  SetDCBrushColor(dc, color);
  Ellipse(dc, center_x - radius, center_y - radius, center_x + radius + 1,
          center_y + radius + 1);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
}

void DrawPlayer(HDC dc,
                const RECT& arena_rect,
                const CameraView& camera,
                const PredictedPlayer& player,
                bool is_self) {
  const float scale_x =
      static_cast<float>(arena_rect.right - arena_rect.left) / camera.width;
  const float scale_y =
      static_cast<float>(arena_rect.bottom - arena_rect.top) / camera.height;
  const float scale = std::min(scale_x, scale_y);
  const int radius =
      std::max(1, static_cast<int>(std::lround(player.size * scale / 2.0f)));
  const int center_x = arena_rect.left +
                       static_cast<int>(std::lround(
                           (player.x - camera.x) * scale_x));
  const int center_y = arena_rect.top +
                       static_cast<int>(std::lround(
                           (player.y - camera.y) * scale_y));

  float aim_x = player.aim_x * scale_x;
  float aim_y = player.aim_y * scale_y;
  const float aim_length = std::hypot(aim_x, aim_y);
  if (!std::isfinite(aim_length) || aim_length < 0.001f) {
    aim_x = 1.0f;
    aim_y = 0.0f;
  } else {
    aim_x /= aim_length;
    aim_y /= aim_length;
  }
  const float perpendicular_x = -aim_y;
  const float perpendicular_y = aim_x;
  const float forward = radius * 0.78f;
  const float sideways = radius * 0.84f;
  const int hand_radius =
      std::max(2, static_cast<int>(std::lround(radius * 0.36f)));
  const int outline_width =
      std::max(2, static_cast<int>(std::lround(radius * 0.18f)));
  const COLORREF outline = RGB(37, 40, 46);
  const COLORREF fill = ParseColor(player.color);

  for (const float side : {-1.0f, 1.0f}) {
    const int hand_x = center_x + static_cast<int>(std::lround(
                                      aim_x * forward +
                                      perpendicular_x * sideways * side));
    const int hand_y = center_y + static_cast<int>(std::lround(
                                      aim_y * forward +
                                      perpendicular_y * sideways * side));
    FillCircle(dc, hand_x, hand_y, hand_radius + outline_width, outline);
    FillCircle(dc, hand_x, hand_y, hand_radius, fill);
  }

  if (is_self) {
    FillCircle(dc, center_x, center_y, radius + outline_width + 3,
               RGB(245, 247, 251));
  }
  FillCircle(dc, center_x, center_y, radius + outline_width, outline);
  FillCircle(dc, center_x, center_y, radius, fill);

  SetTextColor(dc, RGB(245, 247, 251));
  SetBkMode(dc, TRANSPARENT);
  RECT label{center_x - 80, center_y - radius - outline_width - 22,
             center_x + 80, center_y - radius - outline_width - 4};
  DrawTextA(dc, player.name.c_str(), -1, &label,
            DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawMinimap(HDC dc,
                 const RECT& arena,
                 const NativeRenderState& state,
                 const CameraView& camera) {
  constexpr int kMapWidth = 184;
  constexpr int kMapMargin = 16;
  const float world_width = std::max(state.arena_width, 1.0f);
  const float world_height = std::max(state.arena_height, 1.0f);
  const int map_height = std::max(
      1, static_cast<int>(std::lround(kMapWidth * world_height / world_width)));
  const RECT map{arena.right - kMapWidth - kMapMargin,
                 arena.top + kMapMargin,
                 arena.right - kMapMargin,
                 arena.top + kMapMargin + map_height};

  FillSolidRect(dc, map, RGB(20, 27, 22));
  SetDCBrushColor(dc, RGB(205, 214, 209));
  FrameRect(dc, &map, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

  RECT view{
      map.left + static_cast<int>(std::lround(camera.x / world_width * kMapWidth)),
      map.top + static_cast<int>(std::lround(camera.y / world_height * map_height)),
      map.left + static_cast<int>(std::lround(
                     (camera.x + camera.width) / world_width * kMapWidth)),
      map.top + static_cast<int>(std::lround(
                    (camera.y + camera.height) / world_height * map_height))};
  SetDCBrushColor(dc, RGB(122, 136, 128));
  FrameRect(dc, &view, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

  for (const auto& player : state.snapshot.players()) {
    const bool is_self = player.player_id() == state.player_id;
    const float world_x = is_self && state.predicted_ready
                              ? state.predicted_self.x
                              : player.x();
    const float world_y = is_self && state.predicted_ready
                              ? state.predicted_self.y
                              : player.y();
    const int marker_x = map.left + static_cast<int>(
                                      std::lround(world_x / world_width * kMapWidth));
    const int marker_y = map.top + static_cast<int>(
                                     std::lround(world_y / world_height * map_height));
    if (is_self) {
      FillCircle(dc, marker_x, marker_y, 5, RGB(245, 247, 251));
    }
    FillCircle(dc, marker_x, marker_y, is_self ? 3 : 2,
               ParseColor(player.color()));
  }
}

void DrawScene(HDC dc, const RECT& client) {
  FillSolidRect(dc, client, RGB(17, 19, 24));

  const RECT arena = ArenaRectForClient(client);
  if (IsRectEmpty(&arena)) {
    return;
  }
  FillSolidRect(dc, arena, RGB(22, 26, 34));

  const NativeRenderState state = g_client.CaptureRenderState();
  const CameraView camera = CameraForState(state);
  const float scale_x =
      static_cast<float>(arena.right - arena.left) / camera.width;
  const float scale_y =
      static_cast<float>(arena.bottom - arena.top) / camera.height;

  HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
  SetDCPenColor(dc, RGB(36, 43, 54));
  const float first_x = std::floor(camera.x / 80.0f) * 80.0f;
  const float first_y = std::floor(camera.y / 80.0f) * 80.0f;
  for (float x = first_x; x <= camera.x + camera.width; x += 80.0f) {
    const int px = arena.left +
                   static_cast<int>(std::lround((x - camera.x) * scale_x));
    MoveToEx(dc, px, arena.top, nullptr);
    LineTo(dc, px, arena.bottom);
  }
  for (float y = first_y; y <= camera.y + camera.height; y += 80.0f) {
    const int py = arena.top +
                   static_cast<int>(std::lround((y - camera.y) * scale_y));
    MoveToEx(dc, arena.left, py, nullptr);
    LineTo(dc, arena.right, py);
  }
  SelectObject(dc, old_pen);

  SetDCBrushColor(dc, RGB(48, 56, 69));
  FrameRect(dc, &arena, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

  const int saved_dc = SaveDC(dc);
  if (saved_dc != 0) {
    IntersectClipRect(dc, arena.left + 1, arena.top + 1, arena.right - 1,
                      arena.bottom - 1);
  }
  for (const auto& player : state.snapshot.players()) {
    if (player.player_id() == state.player_id && state.predicted_ready) {
      continue;
    }
    PredictedPlayer draw_player;
    draw_player.id = player.player_id();
    draw_player.name = player.display_name();
    draw_player.color = player.color();
    draw_player.x = player.x();
    draw_player.y = player.y();
    draw_player.size = player.size();
    draw_player.aim_x = player.aim_x();
    draw_player.aim_y = player.aim_y();
    const float margin = draw_player.size;
    if (draw_player.x + margin < camera.x ||
        draw_player.x - margin > camera.x + camera.width ||
        draw_player.y + margin < camera.y ||
        draw_player.y - margin > camera.y + camera.height) {
      continue;
    }
    DrawPlayer(dc, arena, camera, draw_player, false);
  }

  if (state.predicted_ready) {
    DrawPlayer(dc, arena, camera, state.predicted_self, true);
  }
  if (saved_dc != 0) {
    RestoreDC(dc, saved_dc);
  }
  DrawMinimap(dc, arena, state, camera);
}

void Paint(HWND window) {
  PAINTSTRUCT paint;
  HDC dc = BeginPaint(window, &paint);

  RECT client;
  GetClientRect(window, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 0 || height <= 0) {
    EndPaint(window, &paint);
    return;
  }

  if (g_back_buffer.Ensure(dc, width, height)) {
    DrawScene(g_back_buffer.dc(), client);
    const int dirty_width = paint.rcPaint.right - paint.rcPaint.left;
    const int dirty_height = paint.rcPaint.bottom - paint.rcPaint.top;
    if (dirty_width > 0 && dirty_height > 0) {
      BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top, dirty_width,
             dirty_height, g_back_buffer.dc(), paint.rcPaint.left,
             paint.rcPaint.top, SRCCOPY);
    }
  } else {
    DrawScene(dc, client);
  }
  EndPaint(window, &paint);
}

void UpdateMouseAim(HWND window) {
  if (!g_has_mouse_target) {
    return;
  }

  RECT client;
  GetClientRect(window, &client);
  const RECT arena = ArenaRectForClient(client);
  if (IsRectEmpty(&arena) || !PtInRect(&arena, g_mouse_target)) {
    return;
  }

  const NativeRenderState state = g_client.CaptureRenderState();
  if (!state.predicted_ready) {
    return;
  }
  const CameraView camera = CameraForState(state);
  const float scale_x =
      static_cast<float>(arena.right - arena.left) / camera.width;
  const float scale_y =
      static_cast<float>(arena.bottom - arena.top) / camera.height;
  const float target_x =
      camera.x + (g_mouse_target.x - arena.left) / scale_x;
  const float target_y =
      camera.y + (g_mouse_target.y - arena.top) / scale_y;
  g_client.SetAimDirection(target_x - state.predicted_self.x,
                           target_y - state.predicted_self.y);
}

void ConnectOrDisconnect(HWND window) {
  if (g_client.IsConnected()) {
    g_client.Disconnect();
    SetWindowTextA(g_connect_button, "Connect");
    SetStatus("Disconnected");
    InvalidateArena(window);
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
  InvalidateArena(window);
  SetFocus(window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      CreateWindowA("STATIC", "Server",
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 20, 18, 52, 20,
                    window, nullptr, nullptr, nullptr);
      g_server_edit = CreateWindowA(
          "EDIT", "127.0.0.1:50052",
          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_BORDER | ES_AUTOHSCROLL,
          76, 14, 180, 26, window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kServerEditId)), nullptr,
          nullptr);
      CreateWindowA("STATIC", "Name",
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 270, 18, 42, 20,
                    window, nullptr, nullptr, nullptr);
      g_name_edit = CreateWindowA(
          "EDIT", "Native Player",
          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_BORDER | ES_AUTOHSCROLL,
          316, 14, 150, 26, window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNameEditId)), nullptr,
          nullptr);
      g_connect_button = CreateWindowA(
          "BUTTON", "Connect",
          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_PUSHBUTTON, 480, 13, 110,
          28, window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConnectButtonId)),
          nullptr, nullptr);
      g_status_label = CreateWindowA(
          "STATIC", "Disconnected",
          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 20, 54, 560, 24, window,
          nullptr, nullptr, nullptr);
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

    case WM_MOUSEMOVE: {
      g_mouse_target.x = static_cast<short>(LOWORD(lparam));
      g_mouse_target.y = static_cast<short>(HIWORD(lparam));
      RECT client;
      GetClientRect(window, &client);
      const RECT arena = ArenaRectForClient(client);
      g_has_mouse_target = !IsRectEmpty(&arena) &&
                           PtInRect(&arena, g_mouse_target);
      UpdateMouseAim(window);
      return 0;
    }

    case WM_TIMER: {
      if (wparam != kTimerId) {
        break;
      }
      if (g_client.IsConnected()) {
        UpdateMouseAim(window);
        g_client.FlushInput();
        g_client.AdvanceMovementStep();
        InvalidateArena(window);
      }
      return 0;
    }

    case WM_GETMINMAXINFO: {
      auto* min_max = reinterpret_cast<MINMAXINFO*>(lparam);
      min_max->ptMinTrackSize.x = kMinimumWindowWidth;
      min_max->ptMinTrackSize.y = kMinimumWindowHeight;
      return 0;
    }

    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        g_back_buffer.Reset();
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;

    case WM_DISPLAYCHANGE:
      g_back_buffer.Reset();
      InvalidateRect(window, nullptr, FALSE);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
      Paint(window);
      return 0;

    case WM_DESTROY:
      KillTimer(window, kTimerId);
      g_client.Disconnect();
      g_back_buffer.Reset();
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
  window_class.hbrBackground = nullptr;

  if (!RegisterClassA(&window_class)) {
    MessageBoxA(nullptr, "Could not register window class.", "Box Arena",
                MB_ICONERROR);
    return 1;
  }

  HWND main_window = CreateWindowExA(
      0, class_name, "Box Arena Native gRPC Client",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1000,
      720, nullptr, nullptr, instance, nullptr);

  if (main_window == nullptr) {
    MessageBoxA(nullptr, "Could not create main window.", "Box Arena",
                MB_ICONERROR);
    return 1;
  }

  ShowWindow(main_window, show_command);
  UpdateWindow(main_window);

  MSG message{};
  while (GetMessageA(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }

  return static_cast<int>(message.wParam);
}

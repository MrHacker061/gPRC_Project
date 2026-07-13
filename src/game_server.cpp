#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "game.grpc.pb.h"

namespace {

using game::GameService;
using game::InputAck;
using game::JoinGameRequest;
using game::JoinGameResponse;
using game::LeaveGameRequest;
using game::LeaveGameResponse;
using game::PlayerInput;
using game::PlayerState;
using game::WorldRequest;
using game::WorldSnapshot;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;

constexpr float kArenaWidth = 3200.0f;
constexpr float kArenaHeight = 2000.0f;
constexpr float kSpawnCameraHalfWidth = 480.0f;
constexpr float kSpawnCameraHalfHeight = 310.0f;
constexpr float kPlayerSize = 56.0f;
constexpr float kPlayerBoundaryPadding = kPlayerSize * 0.78f;
constexpr float kPlayerSpeed = 320.0f;
constexpr auto kTickDuration = std::chrono::milliseconds(16);

std::string NormalizeDisplayName(std::string name) {
  name.erase(name.begin(),
             std::find_if(name.begin(), name.end(), [](unsigned char ch) {
               return !std::isspace(ch);
             }));
  name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char ch) {
               return !std::isspace(ch);
             }).base(),
             name.end());
  if (name.empty()) {
    name = "Player";
  }
  if (name.size() > 24) {
    name.resize(24);
  }
  return name;
}

std::string MakePlayerId() {
  static std::mutex mutex;
  static std::mt19937_64 rng{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  return std::to_string(rng());
}

std::string PickColor(std::size_t index) {
  static const std::vector<std::string> colors = {
      "#2dd4bf", "#f97316", "#f43f5e", "#84cc16",
      "#38bdf8", "#facc15", "#c084fc", "#fb7185"};
  return colors[index % colors.size()];
}

struct InputState {
  float aim_x = 1.0f;
  float aim_y = 0.0f;
  uint64_t sequence = 0;
};

struct Player {
  std::string id;
  std::string display_name;
  std::string color;
  float x = 0.0f;
  float y = 0.0f;
  InputState input;
};

class GameWorld {
 public:
  GameWorld() : running_(true), tick_thread_([this] { TickLoop(); }) {}

  ~GameWorld() {
    running_ = false;
    changed_.notify_all();
    if (tick_thread_.joinable()) {
      tick_thread_.join();
    }
  }

  JoinGameResponse Join(const JoinGameRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    Player player;
    player.id = MakePlayerId();
    player.display_name = NormalizeDisplayName(request.display_name());
    player.color = PickColor(next_color_++);

    std::uniform_real_distribution<float> spawn_x(
        kSpawnCameraHalfWidth, kArenaWidth - kSpawnCameraHalfWidth);
    std::uniform_real_distribution<float> spawn_y(
        kSpawnCameraHalfHeight, kArenaHeight - kSpawnCameraHalfHeight);
    player.x = spawn_x(rng_);
    player.y = spawn_y(rng_);

    const std::string id = player.id;
    players_[id] = std::move(player);

    JoinGameResponse response;
    response.set_player_id(id);
    response.set_arena_width(kArenaWidth);
    response.set_arena_height(kArenaHeight);
    changed_.notify_all();
    return response;
  }

  bool Leave(const std::string& player_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool erased = players_.erase(player_id) > 0;
    if (erased) {
      changed_.notify_all();
    }
    return erased;
  }

  uint64_t UpdateInput(const PlayerInput& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto player = players_.find(input.player_id());
    if (player == players_.end()) {
      return input.sequence();
    }
    if (input.sequence() <= player->second.input.sequence) {
      return player->second.input.sequence;
    }

    const float aim_length =
        std::hypot(input.aim_x(), input.aim_y());
    if (std::isfinite(aim_length) && aim_length > 0.001f) {
      player->second.input.aim_x = input.aim_x() / aim_length;
      player->second.input.aim_y = input.aim_y() / aim_length;
    }

    const float movement_seconds = input.movement_seconds();
    if (std::isfinite(movement_seconds) && movement_seconds > 0.0f) {
      float dx = 0.0f;
      float dy = 0.0f;
      if (input.left()) dx -= 1.0f;
      if (input.right()) dx += 1.0f;
      if (input.up()) dy -= 1.0f;
      if (input.down()) dy += 1.0f;
      const float direction_length = std::hypot(dx, dy);
      if (direction_length > 0.0f) {
        dx /= direction_length;
        dy /= direction_length;
      }
      const float safe_seconds = std::min(movement_seconds, 0.05f);
      player->second.x = std::clamp(
          player->second.x + dx * kPlayerSpeed * safe_seconds,
          kPlayerBoundaryPadding, kArenaWidth - kPlayerBoundaryPadding);
      player->second.y = std::clamp(
          player->second.y + dy * kPlayerSpeed * safe_seconds,
          kPlayerBoundaryPadding, kArenaHeight - kPlayerBoundaryPadding);
    }
    player->second.input.sequence = input.sequence();
    return player->second.input.sequence;
  }

  void Stream(ServerContext* context, ServerWriter<WorldSnapshot>* writer) {
    uint64_t last_tick = 0;
    while (!context->IsCancelled()) {
      WorldSnapshot snapshot = SnapshotSince(context, &last_tick);
      if (snapshot.tick() == 0) {
        continue;
      }
      if (!writer->Write(snapshot)) {
        break;
      }
    }
  }

  WorldSnapshot SnapshotSince(ServerContext* context, uint64_t* last_tick) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(250), [&] {
      return tick_ != *last_tick || !running_ || context->IsCancelled();
    });
    if (!running_ || context->IsCancelled() || tick_ == *last_tick) {
      return {};
    }
    WorldSnapshot snapshot = SnapshotLocked();
    *last_tick = tick_;
    return snapshot;
  }

 private:
  void TickLoop() {
    while (running_) {
      std::this_thread::sleep_for(kTickDuration);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++tick_;
      }
      changed_.notify_all();
    }
  }

  WorldSnapshot SnapshotLocked() const {
    WorldSnapshot snapshot;
    snapshot.set_tick(tick_);
    snapshot.set_arena_width(kArenaWidth);
    snapshot.set_arena_height(kArenaHeight);
    for (const auto& [_, player] : players_) {
      PlayerState* state = snapshot.add_players();
      state->set_player_id(player.id);
      state->set_display_name(player.display_name);
      state->set_x(player.x);
      state->set_y(player.y);
      state->set_size(kPlayerSize);
      state->set_color(player.color);
      state->set_aim_x(player.input.aim_x);
      state->set_aim_y(player.input.aim_y);
      state->set_last_processed_input_sequence(player.input.sequence);
    }
    return snapshot;
  }

  std::atomic<bool> running_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::map<std::string, Player> players_;
  std::mt19937 rng_{std::random_device{}()};
  std::size_t next_color_ = 0;
  uint64_t tick_ = 0;
  std::thread tick_thread_;
};

class GameServiceImpl final : public GameService::Service {
 public:
  Status JoinGame(ServerContext*,
                  const JoinGameRequest* request,
                  JoinGameResponse* response) override {
    *response = world_.Join(*request);
    return Status::OK;
  }

  Status LeaveGame(ServerContext*,
                   const LeaveGameRequest* request,
                   LeaveGameResponse* response) override {
    response->set_ok(world_.Leave(request->player_id()));
    return Status::OK;
  }

  Status SendInput(ServerContext*,
                   const PlayerInput* request,
                   InputAck* response) override {
    response->set_sequence(world_.UpdateInput(*request));
    return Status::OK;
  }

  Status StreamWorld(ServerContext* context,
                     const WorldRequest*,
                     ServerWriter<WorldSnapshot>* writer) override {
    world_.Stream(context, writer);
    return Status::OK;
  }

  Status Play(ServerContext* context,
              ServerReaderWriter<WorldSnapshot, PlayerInput>* stream) override {
    std::atomic<bool> reading{true};
    std::thread reader_thread([&] {
      PlayerInput input;
      while (stream->Read(&input)) {
        world_.UpdateInput(input);
      }
      reading = false;
    });

    uint64_t last_tick = 0;
    while (!context->IsCancelled() && reading) {
      WorldSnapshot snapshot = world_.SnapshotSince(context, &last_tick);
      if (snapshot.tick() == 0) {
        continue;
      }
      if (!stream->Write(snapshot)) {
        break;
      }
    }

    if (reader_thread.joinable()) {
      reader_thread.join();
    }
    return Status::OK;
  }

 private:
  GameWorld world_;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string address = argc > 1 ? argv[1] : "0.0.0.0:50052";

  GameServiceImpl service;
  ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "Failed to start game server on " << address << "\n";
    return 1;
  }

  std::cout << "gRPC game server listening on " << address << "\n";
  server->Wait();
  return 0;
}

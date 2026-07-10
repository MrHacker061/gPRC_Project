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
using grpc::ServerWriter;
using grpc::Status;

constexpr float kArenaWidth = 960.0f;
constexpr float kArenaHeight = 620.0f;
constexpr float kPlayerSize = 28.0f;
constexpr float kPlayerSpeed = 230.0f;
constexpr auto kTickDuration = std::chrono::milliseconds(33);

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
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
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

    const float padding = 64.0f;
    std::uniform_real_distribution<float> spawn_x(padding, kArenaWidth - padding);
    std::uniform_real_distribution<float> spawn_y(padding, kArenaHeight - padding);
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
    player->second.input.up = input.up();
    player->second.input.down = input.down();
    player->second.input.left = input.left();
    player->second.input.right = input.right();
    player->second.input.sequence = input.sequence();
    return input.sequence();
  }

  void Stream(ServerContext* context, ServerWriter<WorldSnapshot>* writer) {
    uint64_t last_tick = 0;
    while (!context->IsCancelled()) {
      WorldSnapshot snapshot;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(250), [&] {
          return tick_ != last_tick || !running_ || context->IsCancelled();
        });
        if (!running_ || context->IsCancelled()) {
          break;
        }
        snapshot = SnapshotLocked();
        last_tick = tick_;
      }

      if (!writer->Write(snapshot)) {
        break;
      }
    }
  }

 private:
  void TickLoop() {
    auto previous = std::chrono::steady_clock::now();
    while (running_) {
      std::this_thread::sleep_for(kTickDuration);
      const auto now = std::chrono::steady_clock::now();
      const float dt =
          std::chrono::duration<float>(now - previous).count();
      previous = now;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, player] : players_) {
          float dx = 0.0f;
          float dy = 0.0f;
          if (player.input.left) {
            dx -= 1.0f;
          }
          if (player.input.right) {
            dx += 1.0f;
          }
          if (player.input.up) {
            dy -= 1.0f;
          }
          if (player.input.down) {
            dy += 1.0f;
          }

          const float length = std::sqrt(dx * dx + dy * dy);
          if (length > 0.0f) {
            dx /= length;
            dy /= length;
          }

          const float half = kPlayerSize / 2.0f;
          player.x = std::clamp(player.x + dx * kPlayerSpeed * dt, half,
                                kArenaWidth - half);
          player.y = std::clamp(player.y + dy * kPlayerSpeed * dt, half,
                                kArenaHeight - half);
        }
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

#include <grpcpp/grpcpp.h>
#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "chat.grpc.pb.h"
#include "game.grpc.pb.h"

namespace {

using chat::ChatEvent;
using chat::ChatMessage;
using chat::ChatService;
using chat::CreateRoomRequest;
using chat::CreateRoomResponse;
using chat::GetHistoryRequest;
using chat::GetHistoryResponse;
using chat::JoinRequest;
using chat::JoinResponse;
using chat::ListEventsRequest;
using chat::ListEventsResponse;
using chat::ListRoomsRequest;
using chat::ListRoomsResponse;
using chat::SendMessageResponse;
using chat::SubscribeRequest;
using chat::TypingRequest;
using chat::TypingResponse;
using game::GameService;
using game::InputAck;
using game::JoinGameRequest;
using game::JoinGameResponse;
using game::LeaveGameRequest;
using game::LeaveGameResponse;
using game::PlayerInput;
using game::WorldRequest;
using game::WorldSnapshot;
using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::Status;

int64_t NowUnixMs() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now.time_since_epoch())
      .count();
}

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string JsonEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

std::string EventTypeName(ChatEvent::EventType type) {
  switch (type) {
    case ChatEvent::MESSAGE:
      return "message";
    case ChatEvent::USER_JOINED:
      return "user_joined";
    case ChatEvent::USER_LEFT:
      return "user_left";
    case ChatEvent::SYSTEM:
      return "system";
    case ChatEvent::TYPING:
      return "typing";
    default:
      return "unknown";
  }
}

std::string EventToJson(const ChatEvent& event) {
  std::ostringstream json;
  json << "{\"type\":\"" << EventTypeName(event.type()) << "\"";
  json << ",\"eventId\":" << event.event_id();
  json << ",\"systemText\":\"" << JsonEscape(event.system_text()) << "\"";
  json << ",\"typingUser\":\"" << JsonEscape(event.typing_user()) << "\"";
  json << ",\"isTyping\":" << (event.is_typing() ? "true" : "false");

  if (event.has_message()) {
    const auto& message = event.message();
    json << ",\"message\":{";
    json << "\"displayName\":\"" << JsonEscape(message.display_name()) << "\"";
    json << ",\"text\":\"" << JsonEscape(message.text()) << "\"";
    json << ",\"sentAt\":" << message.sent_at_unix_ms();
    json << "}";
  } else {
    json << ",\"message\":null";
  }

  json << ",\"activeUsers\":[";
  for (int i = 0; i < event.active_users_size(); ++i) {
    if (i > 0) {
      json << ",";
    }
    json << "\"" << JsonEscape(event.active_users(i)) << "\"";
  }
  json << "]}";
  return json.str();
}

std::string JsonResponse(std::initializer_list<std::pair<std::string, std::string>> values) {
  std::ostringstream json;
  json << "{";
  bool first = true;
  for (const auto& [key, value] : values) {
    if (!first) {
      json << ",";
    }
    first = false;
    json << "\"" << JsonEscape(key) << "\":\"" << JsonEscape(value) << "\"";
  }
  json << "}";
  return json.str();
}

std::string JsonStringField(const std::string& json,
                            const std::string& key,
                            const std::string& fallback) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) {
    return fallback;
  }
  const std::size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) {
    return fallback;
  }
  const std::size_t begin = json.find('"', colon + 1);
  if (begin == std::string::npos) {
    return fallback;
  }

  std::string value;
  bool escaping = false;
  for (std::size_t i = begin + 1; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaping) {
      switch (ch) {
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        default:
          value += ch;
          break;
      }
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '"') {
      return value;
    }
    value += ch;
  }
  return fallback;
}

bool JsonBoolField(const std::string& json,
                   const std::string& key,
                   bool fallback) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) {
    return fallback;
  }
  const std::size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) {
    return fallback;
  }
  const std::size_t value_pos = json.find_first_not_of(" \t\r\n", colon + 1);
  if (value_pos == std::string::npos) {
    return fallback;
  }
  if (json.compare(value_pos, 4, "true") == 0) {
    return true;
  }
  if (json.compare(value_pos, 5, "false") == 0) {
    return false;
  }
  return fallback;
}

uint64_t JsonUInt64Field(const std::string& json,
                         const std::string& key,
                         uint64_t fallback) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) {
    return fallback;
  }
  const std::size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) {
    return fallback;
  }
  const std::size_t value_pos = json.find_first_of("0123456789", colon + 1);
  if (value_pos == std::string::npos) {
    return fallback;
  }
  try {
    return static_cast<uint64_t>(std::stoull(json.substr(value_pos)));
  } catch (...) {
    return fallback;
  }
}

std::string SnapshotToJson(const WorldSnapshot& snapshot,
                           const std::string& current_player_id) {
  std::ostringstream json;
  json << "{\"type\":\"snapshot\"";
  json << ",\"tick\":" << snapshot.tick();
  json << ",\"arenaWidth\":" << snapshot.arena_width();
  json << ",\"arenaHeight\":" << snapshot.arena_height();
  json << ",\"you\":\"" << JsonEscape(current_player_id) << "\"";
  json << ",\"players\":[";
  for (int i = 0; i < snapshot.players_size(); ++i) {
    if (i > 0) {
      json << ",";
    }
    const auto& player = snapshot.players(i);
    json << "{\"id\":\"" << JsonEscape(player.player_id()) << "\"";
    json << ",\"name\":\"" << JsonEscape(player.display_name()) << "\"";
    json << ",\"x\":" << player.x();
    json << ",\"y\":" << player.y();
    json << ",\"size\":" << player.size();
    json << ",\"color\":\"" << JsonEscape(player.color()) << "\"}";
  }
  json << "]}";
  return json.str();
}

std::string ParamOr(const httplib::Request& request,
                    const std::string& key,
                    const std::string& fallback) {
  return request.has_param(key) ? request.get_param_value(key) : fallback;
}

int64_t IntParamOr(const httplib::Request& request,
                   const std::string& key,
                   int64_t fallback) {
  try {
    return std::stoll(ParamOr(request, key, std::to_string(fallback)));
  } catch (...) {
    return fallback;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string grpc_address = argc > 1 ? argv[1] : "localhost:50051";
  const int http_port = argc > 2 ? std::stoi(argv[2]) : 8080;
  const std::string game_grpc_address =
      argc > 3 ? argv[3] : "localhost:50052";

  auto channel =
      grpc::CreateChannel(grpc_address, grpc::InsecureChannelCredentials());
  auto make_chat_stub = [&] { return ChatService::NewStub(channel); };
  auto game_channel = grpc::CreateChannel(game_grpc_address,
                                          grpc::InsecureChannelCredentials());
  auto make_game_stub = [&] { return GameService::NewStub(game_channel); };

  httplib::Server server;

  server.Get("/", [](const httplib::Request&, httplib::Response& response) {
    const std::string index = ReadFile(std::string(WEB_ROOT) + "/index.html");
    response.set_content(index, "text/html; charset=utf-8");
  });

  server.Get("/game", [](const httplib::Request&, httplib::Response& response) {
    const std::string index = ReadFile(std::string(WEB_ROOT) + "/game.html");
    response.set_header("Cache-Control", "no-store");
    response.set_content(index, "text/html; charset=utf-8");
  });

  server.Post("/join", [&](const httplib::Request& request,
                           httplib::Response& response) {
    JoinRequest join_request;
    join_request.set_display_name(ParamOr(request, "name", "Browser Guest"));
    join_request.set_room(ParamOr(request, "room", "lobby"));
    join_request.set_password(ParamOr(request, "password", ""));

    JoinResponse join_response;
    ClientContext context;
    const Status status = make_chat_stub()->Join(&context, join_request, &join_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
                           "application/json");
      return;
    }
    if (join_response.user_id().empty()) {
      response.status = 403;
      response.set_content(JsonResponse({{"error", join_response.message()}}),
                           "application/json");
      return;
    }

    response.set_content(
        JsonResponse({{"userId", join_response.user_id()},
                      {"displayName", join_response.display_name()},
                      {"room", join_response.room()},
                      {"message", join_response.message()}}),
        "application/json");
  });

  server.Get("/rooms", [&](const httplib::Request&, httplib::Response& response) {
    ListRoomsRequest rooms_request;
    ListRoomsResponse rooms_response;
    ClientContext context;
    const Status status = make_chat_stub()->ListRooms(&context, rooms_request, &rooms_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
                           "application/json");
      return;
    }

    std::ostringstream json;
    json << "{\"rooms\":[";
    for (int i = 0; i < rooms_response.rooms_size(); ++i) {
      if (i > 0) {
        json << ",";
      }
      json << "{\"name\":\"" << JsonEscape(rooms_response.rooms(i).name())
           << "\",\"createdAt\":" << rooms_response.rooms(i).created_at_unix_ms()
           << ",\"isPrivate\":"
           << (rooms_response.rooms(i).is_private() ? "true" : "false")
           << "}";
    }
    json << "]}";
    response.set_content(json.str(), "application/json");
  });

  server.Post("/rooms", [&](const httplib::Request& request,
                            httplib::Response& response) {
    CreateRoomRequest room_request;
    room_request.set_room(ParamOr(request, "room", "lobby"));
    room_request.set_is_private(ParamOr(request, "isPrivate", "false") == "true");
    room_request.set_password(ParamOr(request, "password", ""));

    CreateRoomResponse room_response;
    ClientContext context;
    const Status status = make_chat_stub()->CreateRoom(&context, room_request, &room_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
                           "application/json");
      return;
    }
    if (!room_response.error().empty() && !room_response.created()) {
      response.status = 409;
    }
    response.set_content(
        JsonResponse({{"room", room_response.room()},
                      {"created", room_response.created() ? "true" : "false"},
                      {"error", room_response.error()}}),
        "application/json");
  });

  server.Post("/send", [&](const httplib::Request& request,
                           httplib::Response& response) {
    ChatMessage message;
    message.set_user_id(ParamOr(request, "userId", ""));
    message.set_display_name(ParamOr(request, "displayName", "Browser Guest"));
    message.set_room(ParamOr(request, "room", "lobby"));
    message.set_text(ParamOr(request, "text", ""));
    message.set_sent_at_unix_ms(NowUnixMs());

    SendMessageResponse send_response;
    ClientContext context;
    const Status status = make_chat_stub()->SendMessage(&context, message, &send_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
                           "application/json");
      return;
    }

    if (!send_response.accepted()) {
      response.status = 400;
      response.set_content(JsonResponse({{"error", send_response.error()}}),
                           "application/json");
      return;
    }

    response.set_content("{\"ok\":true}", "application/json");
  });

  server.Post("/typing", [&](const httplib::Request& request,
                             httplib::Response& response) {
    TypingRequest typing_request;
    typing_request.set_user_id(ParamOr(request, "userId", ""));
    typing_request.set_display_name(ParamOr(request, "displayName", "Browser Guest"));
    typing_request.set_room(ParamOr(request, "room", "lobby"));
    typing_request.set_is_typing(ParamOr(request, "isTyping", "false") == "true");

    TypingResponse typing_response;
    ClientContext context;
    const Status status =
        make_chat_stub()->SendTyping(&context, typing_request, &typing_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
                           "application/json");
      return;
    }
    response.set_content("{\"ok\":true}", "application/json");
  });

  server.Get("/messages", [&](const httplib::Request& request,
                              httplib::Response& response) {
    ListEventsRequest events_request;
    events_request.set_room(ParamOr(request, "room", "lobby"));
    events_request.set_after_event_id(IntParamOr(request, "afterEventId", 0));

    ListEventsResponse events_response;
    ClientContext context;
    const Status status =
        make_chat_stub()->ListEvents(&context, events_request, &events_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
                           "application/json");
      return;
    }

    std::ostringstream json;
    json << "{\"events\":[";
    for (int i = 0; i < events_response.events_size(); ++i) {
      if (i > 0) {
        json << ",";
      }
      json << EventToJson(events_response.events(i));
    }
    json << "]}";
    response.set_content(json.str(), "application/json");
  });

  server.Get("/history", [&](const httplib::Request& request,
                             httplib::Response& response) {
    GetHistoryRequest history_request;
    history_request.set_room(ParamOr(request, "room", "lobby"));
    history_request.set_limit(static_cast<int32_t>(IntParamOr(request, "limit", 50)));

    GetHistoryResponse history_response;
    ClientContext context;
    const Status status =
        make_chat_stub()->GetHistory(&context, history_request, &history_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
                           "application/json");
      return;
    }

    std::ostringstream json;
    json << "{\"lastEventId\":" << history_response.latest_event_id()
         << ",\"messages\":[";
    for (int i = 0; i < history_response.messages_size(); ++i) {
      if (i > 0) {
        json << ",";
      }
      ChatEvent event;
      event.set_type(ChatEvent::MESSAGE);
      *event.mutable_message() = history_response.messages(i);
      json << EventToJson(event);
    }
    json << "]}";
    response.set_content(json.str(), "application/json");
  });

  server.Get("/events", [&](const httplib::Request& request,
                            httplib::Response& response) {
    const std::string user_id = ParamOr(request, "userId", "");
    const std::string room = ParamOr(request, "room", "lobby");

    response.set_header("Cache-Control", "no-cache, no-transform");
    response.set_header("Connection", "keep-alive");
    response.set_header("X-Accel-Buffering", "no");
    response.set_chunked_content_provider(
        "text/event-stream",
        [&, user_id, room](size_t, httplib::DataSink& sink) {
          SubscribeRequest subscribe_request;
          subscribe_request.set_user_id(user_id);
          subscribe_request.set_room(room);

          ClientContext context;
          std::mutex mutex;
          std::condition_variable changed;
          std::deque<ChatEvent> pending;
          bool reader_done = false;

          std::thread reader_thread([&] {
            std::unique_ptr<ClientReader<ChatEvent>> reader =
                make_chat_stub()->Subscribe(&context, subscribe_request);

            ChatEvent event;
            while (reader->Read(&event)) {
              {
                std::lock_guard<std::mutex> lock(mutex);
                pending.push_back(event);
              }
              changed.notify_one();
            }

            reader->Finish();
            {
              std::lock_guard<std::mutex> lock(mutex);
              reader_done = true;
            }
            changed.notify_one();
          });

          const std::string connected = ": connected\n\n";
          if (!sink.write(connected.data(), connected.size())) {
            context.TryCancel();
            if (reader_thread.joinable()) {
              reader_thread.join();
            }
            sink.done();
            return false;
          }

          while (true) {
            ChatEvent event;
            bool has_event = false;
            bool done = false;

            {
              std::unique_lock<std::mutex> lock(mutex);
              changed.wait_for(lock, std::chrono::seconds(15), [&] {
                return !pending.empty() || reader_done;
              });

              if (!pending.empty()) {
                event = pending.front();
                pending.pop_front();
                has_event = true;
              }
              done = reader_done && pending.empty();
            }

            std::string payload;
            if (has_event) {
              payload = "data: " + EventToJson(event) + "\n\n";
            } else if (done) {
              break;
            } else {
              payload = ": heartbeat\n\n";
            }

            if (!sink.write(payload.data(), payload.size())) {
              context.TryCancel();
              break;
            }
          }

          context.TryCancel();
          if (reader_thread.joinable()) {
            reader_thread.join();
          }
          sink.done();
          return false;
        });
  });

  server.WebSocket("/game/ws", [&](const httplib::Request&,
                                   httplib::ws::WebSocket& ws) {
    std::string message;
    if (ws.read(message) != httplib::ws::Text) {
      ws.close(httplib::ws::CloseStatus::ProtocolError,
               "Expected join message");
      return;
    }

    JoinGameRequest join_request;
    join_request.set_display_name(JsonStringField(message, "name", "Player"));

    JoinGameResponse join_response;
    ClientContext join_context;
    const Status join_status =
        make_game_stub()->JoinGame(&join_context, join_request, &join_response);
    if (!join_status.ok() || join_response.player_id().empty()) {
      ws.send(JsonResponse({{"type", "error"},
                            {"message", join_status.error_message()}}));
      ws.close(httplib::ws::CloseStatus::InternalError, "Game join failed");
      return;
    }

    const std::string player_id = join_response.player_id();
    ws.send(JsonResponse({{"type", "joined"},
                          {"playerId", player_id},
                          {"arenaWidth",
                           std::to_string(join_response.arena_width())},
                          {"arenaHeight",
                           std::to_string(join_response.arena_height())}}));

    ClientContext stream_context;
    std::atomic<bool> streaming{true};
    std::thread stream_thread([&, player_id] {
      WorldRequest world_request;
      world_request.set_player_id(player_id);
      std::unique_ptr<ClientReader<WorldSnapshot>> reader =
          make_game_stub()->StreamWorld(&stream_context, world_request);

      WorldSnapshot snapshot;
      while (streaming && reader->Read(&snapshot)) {
        if (!ws.send(SnapshotToJson(snapshot, player_id))) {
          break;
        }
      }
      reader->Finish();
    });

    while (ws.read(message) == httplib::ws::Text) {
      const std::string type = JsonStringField(message, "type", "");
      if (type != "input") {
        continue;
      }

      PlayerInput input;
      input.set_player_id(player_id);
      input.set_up(JsonBoolField(message, "up", false));
      input.set_down(JsonBoolField(message, "down", false));
      input.set_left(JsonBoolField(message, "left", false));
      input.set_right(JsonBoolField(message, "right", false));
      input.set_sequence(JsonUInt64Field(message, "seq", 0));

      InputAck ack;
      ClientContext input_context;
      make_game_stub()->SendInput(&input_context, input, &ack);
    }

    streaming = false;
    stream_context.TryCancel();
    if (stream_thread.joinable()) {
      stream_thread.join();
    }

    LeaveGameRequest leave_request;
    leave_request.set_player_id(player_id);
    LeaveGameResponse leave_response;
    ClientContext leave_context;
    make_game_stub()->LeaveGame(&leave_context, leave_request, &leave_response);
  });

  std::cout << "Web gateway listening on http://localhost:" << http_port << "\n";
  std::cout << "Forwarding browser chat traffic to gRPC server " << grpc_address
            << "\n";
  std::cout << "Forwarding browser game traffic to gRPC server "
            << game_grpc_address << "\n";

  if (!server.listen("0.0.0.0", http_port)) {
    std::cerr << "Failed to start web gateway on port " << http_port << "\n";
    return 1;
  }

  return 0;
}


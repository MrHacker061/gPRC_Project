#include <grpcpp/grpcpp.h>
#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "chat.grpc.pb.h"

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

  auto channel =
      grpc::CreateChannel(grpc_address, grpc::InsecureChannelCredentials());
  auto make_stub = [&] { return ChatService::NewStub(channel); };

  httplib::Server server;

  server.Get("/", [](const httplib::Request&, httplib::Response& response) {
    const std::string index = ReadFile(std::string(WEB_ROOT) + "/index.html");
    response.set_content(index, "text/html; charset=utf-8");
  });

  server.Post("/join", [&](const httplib::Request& request,
                           httplib::Response& response) {
    JoinRequest join_request;
    join_request.set_display_name(ParamOr(request, "name", "Browser Guest"));
    join_request.set_room(ParamOr(request, "room", "lobby"));

    JoinResponse join_response;
    ClientContext context;
    const Status status = make_stub()->Join(&context, join_request, &join_response);
    if (!status.ok()) {
      response.status = 502;
      response.set_content(JsonResponse({{"error", status.error_message()}}),
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
    const Status status = make_stub()->ListRooms(&context, rooms_request, &rooms_response);
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
           << "}";
    }
    json << "]}";
    response.set_content(json.str(), "application/json");
  });

  server.Post("/rooms", [&](const httplib::Request& request,
                            httplib::Response& response) {
    CreateRoomRequest room_request;
    room_request.set_room(ParamOr(request, "room", "lobby"));

    CreateRoomResponse room_response;
    ClientContext context;
    const Status status = make_stub()->CreateRoom(&context, room_request, &room_response);
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
    const Status status = make_stub()->SendMessage(&context, message, &send_response);
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
        make_stub()->SendTyping(&context, typing_request, &typing_response);
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
        make_stub()->ListEvents(&context, events_request, &events_response);
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
        make_stub()->GetHistory(&context, history_request, &history_response);
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
                make_stub()->Subscribe(&context, subscribe_request);

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

  std::cout << "Web gateway listening on http://localhost:" << http_port << "\n";
  std::cout << "Forwarding browser chat traffic to gRPC server " << grpc_address
            << "\n";

  if (!server.listen("0.0.0.0", http_port)) {
    std::cerr << "Failed to start web gateway on port " << http_port << "\n";
    return 1;
  }

  return 0;
}

#include <grpcpp/grpcpp.h>
#include <httplib.h>

#include <chrono>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "chat.grpc.pb.h"

namespace {

using chat::ChatEvent;
using chat::ChatMessage;
using chat::ChatService;
using chat::JoinRequest;
using chat::JoinResponse;
using chat::SendMessageResponse;
using chat::SubscribeRequest;
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
    default:
      return "unknown";
  }
}

std::string EventToJson(const ChatEvent& event) {
  std::ostringstream json;
  json << "{\"type\":\"" << EventTypeName(event.type()) << "\"";
  json << ",\"systemText\":\"" << JsonEscape(event.system_text()) << "\"";

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

  server.Get("/events", [&](const httplib::Request& request,
                            httplib::Response& response) {
    const std::string user_id = ParamOr(request, "userId", "");
    const std::string room = ParamOr(request, "room", "lobby");

    response.set_header("Cache-Control", "no-cache");
    response.set_header("Connection", "keep-alive");
    response.set_chunked_content_provider(
        "text/event-stream",
        [&, user_id, room](size_t, httplib::DataSink& sink) {
          SubscribeRequest subscribe_request;
          subscribe_request.set_user_id(user_id);
          subscribe_request.set_room(room);

          ClientContext context;
          std::unique_ptr<ClientReader<ChatEvent>> reader =
              make_stub()->Subscribe(&context, subscribe_request);

          ChatEvent event;
          while (reader->Read(&event)) {
            const std::string payload = "data: " + EventToJson(event) + "\n\n";
            if (!sink.write(payload.data(), payload.size())) {
              context.TryCancel();
              break;
            }
          }

          reader->Finish();
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

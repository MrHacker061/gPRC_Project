#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

void PrintEvent(const ChatEvent& event) {
  if (event.type() == ChatEvent::MESSAGE) {
    const auto& message = event.message();
    std::cout << "\n[" << message.display_name() << "] " << message.text()
              << "\n> " << std::flush;
    return;
  }

  if (!event.system_text().empty()) {
    std::cout << "\n* " << event.system_text() << "\n> " << std::flush;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string server_address = argc > 1 ? argv[1] : "localhost:50051";
  std::string display_name = argc > 2 ? argv[2] : "";
  const std::string room = argc > 3 ? argv[3] : "lobby";

  if (display_name.empty()) {
    std::cout << "Display name: ";
    std::getline(std::cin, display_name);
  }

  auto channel =
      grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
  auto stub = ChatService::NewStub(channel);

  JoinRequest join_request;
  join_request.set_display_name(display_name);
  join_request.set_room(room);

  JoinResponse join_response;
  ClientContext join_context;
  const Status join_status =
      stub->Join(&join_context, join_request, &join_response);
  if (!join_status.ok()) {
    std::cerr << "Join failed: " << join_status.error_message() << "\n";
    return 1;
  }

  std::cout << join_response.message() << " as "
            << join_response.display_name() << "\n";
  std::cout << "Type /quit to leave.\n";

  std::atomic<bool> running{true};
  auto subscribe_context = std::make_shared<ClientContext>();
  std::thread receiver([&] {
    SubscribeRequest subscribe_request;
    subscribe_request.set_user_id(join_response.user_id());
    subscribe_request.set_room(join_response.room());

    std::unique_ptr<ClientReader<ChatEvent>> reader =
        stub->Subscribe(subscribe_context.get(), subscribe_request);

    ChatEvent event;
    while (running && reader->Read(&event)) {
      PrintEvent(event);
    }

    const Status status = reader->Finish();
    if (running && !status.ok()) {
      std::cerr << "\nSubscription ended: " << status.error_message() << "\n";
    }
  });

  std::string line;
  std::cout << "> " << std::flush;
  while (std::getline(std::cin, line)) {
    if (line == "/quit") {
      break;
    }
    if (line.empty()) {
      std::cout << "> " << std::flush;
      continue;
    }

    ChatMessage message;
    message.set_user_id(join_response.user_id());
    message.set_display_name(join_response.display_name());
    message.set_room(join_response.room());
    message.set_text(line);
    message.set_sent_at_unix_ms(NowUnixMs());

    SendMessageResponse send_response;
    ClientContext send_context;
    const Status send_status =
        stub->SendMessage(&send_context, message, &send_response);
    if (!send_status.ok()) {
      std::cerr << "Send failed: " << send_status.error_message() << "\n";
    } else if (!send_response.accepted()) {
      std::cerr << "Send rejected: " << send_response.error() << "\n";
    }

    std::cout << "> " << std::flush;
  }

  running = false;
  subscribe_context->TryCancel();
  if (receiver.joinable()) {
    receiver.join();
  }

  std::cout << "\nGoodbye.\n";
  return 0;
}

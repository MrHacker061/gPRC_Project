#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "chat.grpc.pb.h"

namespace {

using chat::ChatEvent;
using chat::ChatMessage;
using chat::ChatService;
using chat::JoinRequest;
using chat::JoinResponse;
using chat::ListUsersRequest;
using chat::ListUsersResponse;
using chat::SendMessageResponse;
using chat::SubscribeRequest;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;

constexpr const char* kDefaultRoom = "lobby";

int64_t NowUnixMs() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now.time_since_epoch())
      .count();
}

std::string NormalizeRoom(const std::string& room) {
  return room.empty() ? kDefaultRoom : room;
}

std::string MakeUserId() {
  static std::mutex mutex;
  static std::mt19937_64 rng{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  return std::to_string(rng());
}

struct User {
  std::string id;
  std::string display_name;
  std::string room;
};

struct Subscriber {
  explicit Subscriber(std::string subscribed_room)
      : room(std::move(subscribed_room)) {}

  std::string room;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<ChatEvent> events;
};

class ChatHub {
 public:
  JoinResponse Join(const JoinRequest& request) {
    User user;
    user.id = MakeUserId();
    user.display_name =
        request.display_name().empty() ? "Anonymous" : request.display_name();
    user.room = NormalizeRoom(request.room());

    std::vector<std::string> active_users;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      users_[user.id] = user;
      active_users = ActiveUsersLocked(user.room);
    }

    ChatEvent event;
    event.set_type(ChatEvent::USER_JOINED);
    event.set_system_text(user.display_name + " joined " + user.room);
    for (const auto& name : active_users) {
      event.add_active_users(name);
    }
    Publish(user.room, event);

    JoinResponse response;
    response.set_user_id(user.id);
    response.set_display_name(user.display_name);
    response.set_room(user.room);
    response.set_message("Joined " + user.room);
    return response;
  }

  SendMessageResponse Send(const ChatMessage& incoming) {
    SendMessageResponse response;
    if (incoming.text().empty()) {
      response.set_accepted(false);
      response.set_error("Message text cannot be empty.");
      return response;
    }

    ChatMessage message = incoming;
    const std::string room = NormalizeRoom(message.room());
    message.set_room(room);
    if (message.sent_at_unix_ms() == 0) {
      message.set_sent_at_unix_ms(NowUnixMs());
    }

    std::vector<std::string> active_users;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto user = users_.find(message.user_id());
      if (user != users_.end()) {
        if (message.display_name().empty()) {
          message.set_display_name(user->second.display_name);
        }
        message.set_room(user->second.room);
      } else if (message.display_name().empty()) {
        message.set_display_name("Anonymous");
      }
      active_users = ActiveUsersLocked(message.room());
    }

    ChatEvent event;
    event.set_type(ChatEvent::MESSAGE);
    *event.mutable_message() = message;
    for (const auto& name : active_users) {
      event.add_active_users(name);
    }
    Publish(message.room(), event);

    response.set_accepted(true);
    return response;
  }

  std::vector<std::string> ListUsers(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ActiveUsersLocked(NormalizeRoom(room));
  }

  void Subscribe(ServerContext* context,
                 const SubscribeRequest& request,
                 ServerWriter<ChatEvent>* writer) {
    const std::string room = NormalizeRoom(request.room());
    const auto subscriber = std::make_shared<Subscriber>(room);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      subscribers_.push_back(subscriber);
    }

    ChatEvent welcome;
    welcome.set_type(ChatEvent::SYSTEM);
    welcome.set_system_text("Connected to " + room);
    for (const auto& name : ListUsers(room)) {
      welcome.add_active_users(name);
    }
    writer->Write(welcome);

    while (!context->IsCancelled()) {
      ChatEvent next;
      {
        std::unique_lock<std::mutex> lock(subscriber->mutex);
        subscriber->changed.wait_for(lock, std::chrono::milliseconds(250), [&] {
          return !subscriber->events.empty() || context->IsCancelled();
        });

        if (subscriber->events.empty()) {
          continue;
        }

        next = subscriber->events.front();
        subscriber->events.pop_front();
      }

      if (!writer->Write(next)) {
        break;
      }
    }

    RemoveSubscriber(subscriber);
    if (!request.user_id().empty()) {
      Leave(request.user_id());
    }
  }

 private:
  std::vector<std::string> ActiveUsersLocked(const std::string& room) const {
    std::vector<std::string> names;
    for (const auto& [_, user] : users_) {
      if (user.room == room) {
        names.push_back(user.display_name);
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  void Leave(const std::string& user_id) {
    User leaving;
    bool found = false;
    std::vector<std::string> active_users;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto user = users_.find(user_id);
      if (user == users_.end()) {
        return;
      }
      leaving = user->second;
      users_.erase(user);
      active_users = ActiveUsersLocked(leaving.room);
      found = true;
    }

    if (!found) {
      return;
    }

    ChatEvent event;
    event.set_type(ChatEvent::USER_LEFT);
    event.set_system_text(leaving.display_name + " left " + leaving.room);
    for (const auto& name : active_users) {
      event.add_active_users(name);
    }
    Publish(leaving.room, event);
  }

  void Publish(const std::string& room, const ChatEvent& event) {
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      subscribers_.erase(
          std::remove_if(subscribers_.begin(), subscribers_.end(),
                         [](const std::weak_ptr<Subscriber>& weak) {
                           return weak.expired();
                         }),
          subscribers_.end());

      for (const auto& weak : subscribers_) {
        if (const auto subscriber = weak.lock();
            subscriber && subscriber->room == room) {
          subscribers.push_back(subscriber);
        }
      }
    }

    for (const auto& subscriber : subscribers) {
      {
        std::lock_guard<std::mutex> lock(subscriber->mutex);
        subscriber->events.push_back(event);
      }
      subscriber->changed.notify_one();
    }
  }

  void RemoveSubscriber(const std::shared_ptr<Subscriber>& subscriber) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
                       [&](const std::weak_ptr<Subscriber>& weak) {
                         return weak.expired() || weak.lock() == subscriber;
                       }),
        subscribers_.end());
  }

  std::mutex mutex_;
  std::map<std::string, User> users_;
  std::vector<std::weak_ptr<Subscriber>> subscribers_;
};

class ChatServiceImpl final : public ChatService::Service {
 public:
  Status Join(ServerContext*,
              const JoinRequest* request,
              JoinResponse* response) override {
    *response = hub_.Join(*request);
    return Status::OK;
  }

  Status SendMessage(ServerContext*,
                     const ChatMessage* request,
                     SendMessageResponse* response) override {
    *response = hub_.Send(*request);
    return Status::OK;
  }

  Status Subscribe(ServerContext* context,
                   const SubscribeRequest* request,
                   ServerWriter<ChatEvent>* writer) override {
    hub_.Subscribe(context, *request, writer);
    return Status::OK;
  }

  Status ListUsers(ServerContext*,
                   const ListUsersRequest* request,
                   ListUsersResponse* response) override {
    for (const auto& name : hub_.ListUsers(request->room())) {
      response->add_display_names(name);
    }
    return Status::OK;
  }

 private:
  ChatHub hub_;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string address = argc > 1 ? argv[1] : "0.0.0.0:50051";

  ChatServiceImpl service;
  ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  if (!server) {
    std::cerr << "Failed to start chat server on " << address << "\n";
    return 1;
  }

  std::cout << "gRPC chat server listening on " << address << "\n";
  server->Wait();
  return 0;
}

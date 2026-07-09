#include <grpcpp/grpcpp.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
using chat::ListUsersRequest;
using chat::ListUsersResponse;
using chat::Room;
using chat::SendMessageResponse;
using chat::SubscribeRequest;
using chat::TypingRequest;
using chat::TypingResponse;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;

constexpr const char* kDefaultRoom = "lobby";
constexpr std::size_t kMaxLiveQueuePerSubscriber = 200;

int64_t NowUnixMs() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now.time_since_epoch())
      .count();
}

std::string Trim(std::string value) {
  const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string NormalizeRoom(const std::string& room) {
  std::string normalized = Trim(room);
  if (normalized.empty()) {
    normalized = kDefaultRoom;
  }
  if (normalized.size() > 40) {
    normalized.resize(40);
  }
  return normalized;
}

std::string NormalizeDisplayName(const std::string& display_name) {
  std::string normalized = Trim(display_name);
  if (normalized.empty()) {
    normalized = "Anonymous";
  }
  if (normalized.size() > 40) {
    normalized.resize(40);
  }
  return normalized;
}

std::string MakeUserId() {
  static std::mutex mutex;
  static std::mt19937_64 rng{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  return std::to_string(rng());
}

std::string ColumnText(sqlite3_stmt* stmt, int column) {
  const unsigned char* text = sqlite3_column_text(stmt, column);
  return text == nullptr ? "" : reinterpret_cast<const char*>(text);
}

void BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(sqlite3_errmsg(db_));
    }
  }

  ~Statement() {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }

  sqlite3_stmt* get() { return stmt_; }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

class ChatStore {
 public:
  explicit ChatStore(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
      throw std::runtime_error("Could not open SQLite database.");
    }
    Exec("PRAGMA journal_mode=WAL;");
    Exec("PRAGMA foreign_keys=ON;");
    Exec("CREATE TABLE IF NOT EXISTS rooms ("
         "name TEXT PRIMARY KEY,"
         "created_at_unix_ms INTEGER NOT NULL"
         ");");
    Exec("CREATE TABLE IF NOT EXISTS events ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "room TEXT NOT NULL,"
         "type INTEGER NOT NULL,"
         "user_id TEXT,"
         "display_name TEXT,"
         "text TEXT,"
         "system_text TEXT,"
         "sent_at_unix_ms INTEGER,"
         "typing_user TEXT,"
         "is_typing INTEGER NOT NULL DEFAULT 0"
         ");");
    Exec("CREATE TABLE IF NOT EXISTS messages ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "event_id INTEGER NOT NULL,"
         "room TEXT NOT NULL,"
         "user_id TEXT,"
         "display_name TEXT NOT NULL,"
         "text TEXT NOT NULL,"
         "sent_at_unix_ms INTEGER NOT NULL"
         ");");
    EnsureRoom(kDefaultRoom);
  }

  ~ChatStore() {
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
  }

  bool CreateRoom(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "INSERT OR IGNORE INTO rooms(name, created_at_unix_ms) "
                   "VALUES(?, ?);");
    BindText(stmt.get(), 1, room);
    sqlite3_bind_int64(stmt.get(), 2, NowUnixMs());
    StepDone(stmt.get());
    return sqlite3_changes(db_) > 0;
  }

  void EnsureRoom(const std::string& room) { CreateRoom(room); }

  std::vector<Room> ListRooms() {
    std::vector<Room> rooms;
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(
        db_,
        "SELECT name, created_at_unix_ms FROM rooms ORDER BY lower(name);");
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      Room room;
      room.set_name(ColumnText(stmt.get(), 0));
      room.set_created_at_unix_ms(sqlite3_column_int64(stmt.get(), 1));
      rooms.push_back(room);
    }
    return rooms;
  }

  ChatEvent SaveEvent(const std::string& room, const ChatEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "INSERT INTO events(room, type, user_id, display_name, text, "
                   "system_text, sent_at_unix_ms, typing_user, is_typing) "
                   "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);");
    BindText(stmt.get(), 1, room);
    sqlite3_bind_int(stmt.get(), 2, event.type());
    if (event.has_message()) {
      const auto& message = event.message();
      BindText(stmt.get(), 3, message.user_id());
      BindText(stmt.get(), 4, message.display_name());
      BindText(stmt.get(), 5, message.text());
      sqlite3_bind_int64(stmt.get(), 7, message.sent_at_unix_ms());
    } else {
      sqlite3_bind_null(stmt.get(), 3);
      sqlite3_bind_null(stmt.get(), 4);
      sqlite3_bind_null(stmt.get(), 5);
      sqlite3_bind_null(stmt.get(), 7);
    }
    BindText(stmt.get(), 6, event.system_text());
    BindText(stmt.get(), 8, event.typing_user());
    sqlite3_bind_int(stmt.get(), 9, event.is_typing() ? 1 : 0);
    StepDone(stmt.get());

    ChatEvent saved = event;
    saved.set_event_id(sqlite3_last_insert_rowid(db_));

    if (saved.type() == ChatEvent::MESSAGE && saved.has_message()) {
      SaveMessageLocked(saved.event_id(), saved.message());
    }

    return saved;
  }

  std::vector<ChatEvent> ListEvents(const std::string& room,
                                    int64_t after_event_id,
                                    int limit) {
    std::vector<ChatEvent> events;
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "SELECT id, type, user_id, display_name, text, system_text, "
                   "sent_at_unix_ms, typing_user, is_typing "
                   "FROM events WHERE room = ? AND id > ? "
                   "ORDER BY id ASC LIMIT ?;");
    BindText(stmt.get(), 1, room);
    sqlite3_bind_int64(stmt.get(), 2, after_event_id);
    sqlite3_bind_int(stmt.get(), 3, limit);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      events.push_back(EventFromRow(stmt.get(), room));
    }
    return events;
  }

  std::vector<ChatMessage> GetHistory(const std::string& room, int limit) {
    std::deque<ChatMessage> newest_first;
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "SELECT user_id, display_name, text, sent_at_unix_ms "
                   "FROM messages WHERE room = ? "
                   "ORDER BY id DESC LIMIT ?;");
    BindText(stmt.get(), 1, room);
    sqlite3_bind_int(stmt.get(), 2, limit);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      ChatMessage message;
      message.set_user_id(ColumnText(stmt.get(), 0));
      message.set_display_name(ColumnText(stmt.get(), 1));
      message.set_room(room);
      message.set_text(ColumnText(stmt.get(), 2));
      message.set_sent_at_unix_ms(sqlite3_column_int64(stmt.get(), 3));
      newest_first.push_front(message);
    }

    return {newest_first.begin(), newest_first.end()};
  }

  int64_t LatestEventId(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "SELECT COALESCE(MAX(id), 0) FROM events WHERE room = ?;");
    BindText(stmt.get(), 1, room);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      return sqlite3_column_int64(stmt.get(), 0);
    }
    return 0;
  }

 private:
  void Exec(const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      std::string message = error == nullptr ? sqlite3_errmsg(db_) : error;
      sqlite3_free(error);
      throw std::runtime_error(message);
    }
  }

  void StepDone(sqlite3_stmt* stmt) {
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw std::runtime_error(sqlite3_errmsg(db_));
    }
  }

  void SaveMessageLocked(int64_t event_id, const ChatMessage& message) {
    Statement stmt(db_,
                   "INSERT INTO messages(event_id, room, user_id, display_name, "
                   "text, sent_at_unix_ms) VALUES(?, ?, ?, ?, ?, ?);");
    sqlite3_bind_int64(stmt.get(), 1, event_id);
    BindText(stmt.get(), 2, message.room());
    BindText(stmt.get(), 3, message.user_id());
    BindText(stmt.get(), 4, message.display_name());
    BindText(stmt.get(), 5, message.text());
    sqlite3_bind_int64(stmt.get(), 6, message.sent_at_unix_ms());
    StepDone(stmt.get());
  }

  ChatEvent EventFromRow(sqlite3_stmt* stmt, const std::string& room) {
    ChatEvent event;
    event.set_event_id(sqlite3_column_int64(stmt, 0));
    event.set_type(static_cast<ChatEvent::EventType>(sqlite3_column_int(stmt, 1)));
    event.set_system_text(ColumnText(stmt, 5));
    event.set_typing_user(ColumnText(stmt, 7));
    event.set_is_typing(sqlite3_column_int(stmt, 8) != 0);

    if (event.type() == ChatEvent::MESSAGE) {
      ChatMessage* message = event.mutable_message();
      message->set_user_id(ColumnText(stmt, 2));
      message->set_display_name(ColumnText(stmt, 3));
      message->set_room(room);
      message->set_text(ColumnText(stmt, 4));
      message->set_sent_at_unix_ms(sqlite3_column_int64(stmt, 6));
    }

    return event;
  }

  sqlite3* db_ = nullptr;
  std::mutex mutex_;
};

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
  explicit ChatHub(std::string db_path) : store_(std::move(db_path)) {}

  JoinResponse Join(const JoinRequest& request) {
    User user;
    user.id = MakeUserId();
    user.display_name = NormalizeDisplayName(request.display_name());
    user.room = NormalizeRoom(request.room());
    store_.EnsureRoom(user.room);

    std::vector<std::string> active_users;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      users_[user.id] = user;
      active_users = ActiveUsersLocked(user.room);
    }

    ChatEvent event;
    event.set_type(ChatEvent::USER_JOINED);
    event.set_system_text(user.display_name + " joined " + user.room);
    AddActiveUsers(event, active_users);
    Publish(user.room, event);

    JoinResponse response;
    response.set_user_id(user.id);
    response.set_display_name(user.display_name);
    response.set_room(user.room);
    response.set_message("Joined " + user.room);
    return response;
  }

  CreateRoomResponse CreateRoom(const CreateRoomRequest& request) {
    CreateRoomResponse response;
    const std::string room = NormalizeRoom(request.room());
    if (room.empty()) {
      response.set_created(false);
      response.set_error("Room name cannot be empty.");
      return response;
    }

    const bool created = store_.CreateRoom(room);
    response.set_created(created);
    response.set_room(room);
    if (!created) {
      response.set_error("Room already exists.");
    }
    return response;
  }

  std::vector<Room> ListRooms() { return store_.ListRooms(); }

  SendMessageResponse Send(const ChatMessage& incoming) {
    SendMessageResponse response;
    if (Trim(incoming.text()).empty()) {
      response.set_accepted(false);
      response.set_error("Message text cannot be empty.");
      return response;
    }

    ChatMessage message = incoming;
    message.set_text(Trim(message.text()));
    message.set_room(NormalizeRoom(message.room()));
    store_.EnsureRoom(message.room());
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
      } else {
        message.set_display_name(NormalizeDisplayName(message.display_name()));
      }
      active_users = ActiveUsersLocked(message.room());
    }

    ChatEvent event;
    event.set_type(ChatEvent::MESSAGE);
    *event.mutable_message() = message;
    AddActiveUsers(event, active_users);
    Publish(message.room(), event);

    response.set_accepted(true);
    return response;
  }

  TypingResponse SendTyping(const TypingRequest& request) {
    const std::string room = NormalizeRoom(request.room());
    std::string display_name = NormalizeDisplayName(request.display_name());
    std::vector<std::string> active_users;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto user = users_.find(request.user_id());
      if (user != users_.end()) {
        display_name = user->second.display_name;
      }
      active_users = ActiveUsersLocked(room);
    }

    ChatEvent event;
    event.set_type(ChatEvent::TYPING);
    event.set_typing_user(display_name);
    event.set_is_typing(request.is_typing());
    AddActiveUsers(event, active_users);
    Publish(room, event);

    TypingResponse response;
    response.set_accepted(true);
    return response;
  }

  std::vector<std::string> ListUsers(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ActiveUsersLocked(NormalizeRoom(room));
  }

  std::vector<ChatEvent> ListEvents(const std::string& room,
                                    int64_t after_event_id) {
    const std::string normalized_room = NormalizeRoom(room);
    auto events = store_.ListEvents(normalized_room, after_event_id, 100);
    const auto active_users = ListUsers(normalized_room);
    for (auto& event : events) {
      event.clear_active_users();
      AddActiveUsers(event, active_users);
    }
    return events;
  }

  std::vector<ChatMessage> GetHistory(const std::string& room, int limit) {
    limit = std::clamp(limit <= 0 ? 50 : limit, 1, 200);
    return store_.GetHistory(NormalizeRoom(room), limit);
  }

  int64_t LatestEventId(const std::string& room) {
    return store_.LatestEventId(NormalizeRoom(room));
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
    AddActiveUsers(welcome, ListUsers(room));
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
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
  }

  void AddActiveUsers(ChatEvent& event,
                      const std::vector<std::string>& active_users) {
    for (const auto& name : active_users) {
      event.add_active_users(name);
    }
  }

  void Leave(const std::string& user_id) {
    User leaving;
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
    }

    ChatEvent event;
    event.set_type(ChatEvent::USER_LEFT);
    event.set_system_text(leaving.display_name + " left " + leaving.room);
    AddActiveUsers(event, active_users);
    Publish(leaving.room, event);
  }

  void Publish(const std::string& room, const ChatEvent& event) {
    const ChatEvent saved_event = store_.SaveEvent(room, event);
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
        subscriber->events.push_back(saved_event);
        while (subscriber->events.size() > kMaxLiveQueuePerSubscriber) {
          subscriber->events.pop_front();
        }
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

  ChatStore store_;
  std::mutex mutex_;
  std::map<std::string, User> users_;
  std::vector<std::weak_ptr<Subscriber>> subscribers_;
};

class ChatServiceImpl final : public ChatService::Service {
 public:
  explicit ChatServiceImpl(std::string db_path) : hub_(std::move(db_path)) {}

  Status Join(ServerContext*,
              const JoinRequest* request,
              JoinResponse* response) override {
    *response = hub_.Join(*request);
    return Status::OK;
  }

  Status CreateRoom(ServerContext*,
                    const CreateRoomRequest* request,
                    CreateRoomResponse* response) override {
    *response = hub_.CreateRoom(*request);
    return Status::OK;
  }

  Status ListRooms(ServerContext*,
                   const ListRoomsRequest*,
                   ListRoomsResponse* response) override {
    for (const auto& room : hub_.ListRooms()) {
      *response->add_rooms() = room;
    }
    return Status::OK;
  }

  Status SendMessage(ServerContext*,
                     const ChatMessage* request,
                     SendMessageResponse* response) override {
    *response = hub_.Send(*request);
    return Status::OK;
  }

  Status SendTyping(ServerContext*,
                    const TypingRequest* request,
                    TypingResponse* response) override {
    *response = hub_.SendTyping(*request);
    return Status::OK;
  }

  Status Subscribe(ServerContext* context,
                   const SubscribeRequest* request,
                   ServerWriter<ChatEvent>* writer) override {
    hub_.Subscribe(context, *request, writer);
    return Status::OK;
  }

  Status ListEvents(ServerContext*,
                    const ListEventsRequest* request,
                    ListEventsResponse* response) override {
    for (const auto& event :
         hub_.ListEvents(request->room(), request->after_event_id())) {
      *response->add_events() = event;
    }
    return Status::OK;
  }

  Status GetHistory(ServerContext*,
                    const GetHistoryRequest* request,
                    GetHistoryResponse* response) override {
    for (const auto& message :
         hub_.GetHistory(request->room(), request->limit())) {
      *response->add_messages() = message;
    }
    response->set_latest_event_id(hub_.LatestEventId(request->room()));
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
  const std::string db_path = argc > 2 ? argv[2] : "chat_history.db";

  try {
    ChatServiceImpl service(db_path);
    ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (!server) {
      std::cerr << "Failed to start chat server on " << address << "\n";
      return 1;
    }

    std::cout << "gRPC chat server listening on " << address << "\n";
    std::cout << "SQLite history stored at " << db_path << "\n";
    server->Wait();
  } catch (const std::exception& error) {
    std::cerr << "Server failed: " << error.what() << "\n";
    return 1;
  }

  return 0;
}

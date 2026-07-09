cmake_minimum_required(VERSION 3.24)

project(grpc_browser_chat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/chat.proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

string(REPLACE "\\" "/" WEB_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/web")

set(GENERATED_PROTO_SRCS
    "${GENERATED_DIR}/chat.pb.cc"
    "${GENERATED_DIR}/chat.pb.h"
    "${GENERATED_DIR}/chat.grpc.pb.cc"
    "${GENERATED_DIR}/chat.grpc.pb.h")

set(GRPC_CPP_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")

add_custom_command(
    OUTPUT ${GENERATED_PROTO_SRCS}
    COMMAND protobuf::protoc
    ARGS --grpc_out "${GENERATED_DIR}"
         --cpp_out "${GENERATED_DIR}"
         -I "${CMAKE_CURRENT_SOURCE_DIR}/proto"
         --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}"
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating C++ gRPC sources from proto/chat.proto"
    VERBATIM)

add_library(chat_proto STATIC ${GENERATED_PROTO_SRCS})
target_include_directories(chat_proto PUBLIC "${GENERATED_DIR}")
target_link_libraries(chat_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_executable(chat_server src/chat_server.cpp)
target_link_libraries(chat_server PRIVATE chat_proto)

add_executable(chat_cli src/chat_cli.cpp)
target_link_libraries(chat_cli PRIVATE chat_proto)

add_executable(web_gateway src/web_gateway.cpp)
target_compile_definitions(web_gateway PRIVATE WEB_ROOT="${WEB_ROOT_PATH}")
target_link_libraries(web_gateway PRIVATE chat_proto httplib::httplib)

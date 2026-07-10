cmake_minimum_required(VERSION 3.24)

project(grpc_browser_chat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(GRPC_CPP_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")

string(REPLACE "\\" "/" WEB_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/web")

function(add_grpc_proto target proto_file)
    get_filename_component(proto_name "${proto_file}" NAME_WE)
    set(generated_proto_srcs
        "${GENERATED_DIR}/${proto_name}.pb.cc"
        "${GENERATED_DIR}/${proto_name}.pb.h"
        "${GENERATED_DIR}/${proto_name}.grpc.pb.cc"
        "${GENERATED_DIR}/${proto_name}.grpc.pb.h")

    add_custom_command(
        OUTPUT ${generated_proto_srcs}
        COMMAND protobuf::protoc
        ARGS --grpc_out "${GENERATED_DIR}"
             --cpp_out "${GENERATED_DIR}"
             -I "${CMAKE_CURRENT_SOURCE_DIR}/proto"
             --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
             "${proto_file}"
        DEPENDS "${proto_file}"
        COMMENT "Generating C++ gRPC sources from ${proto_file}"
        VERBATIM)

    add_library(${target} STATIC ${generated_proto_srcs})
    target_include_directories(${target} PUBLIC "${GENERATED_DIR}")
    target_link_libraries(${target} PUBLIC gRPC::grpc++ protobuf::libprotobuf)
endfunction()

add_grpc_proto(chat_proto "${CMAKE_CURRENT_SOURCE_DIR}/proto/chat.proto")
add_grpc_proto(game_proto "${CMAKE_CURRENT_SOURCE_DIR}/proto/game.proto")

add_executable(chat_server src/chat_server.cpp)
target_link_libraries(chat_server PRIVATE chat_proto unofficial::sqlite3::sqlite3)

add_executable(chat_cli src/chat_cli.cpp)
target_link_libraries(chat_cli PRIVATE chat_proto)

add_executable(game_server src/game_server.cpp)
target_link_libraries(game_server PRIVATE game_proto)

add_executable(web_gateway src/web_gateway.cpp)
target_compile_definitions(web_gateway PRIVATE WEB_ROOT="${WEB_ROOT_PATH}")
target_link_libraries(web_gateway PRIVATE chat_proto game_proto httplib::httplib)

cmake_minimum_required(VERSION 3.10)
project(Binance-WebSocket-news)

set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)
find_package(Boost 1.71 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

add_executable(Binance-WebSocket-news main.cpp)

target_link_libraries(Binance-WebSocket-news PRIVATE 
    Boost::boost
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

target_include_directories(Binance-WebSocket-news PRIVATE 
    /usr/include/rapidjson
)

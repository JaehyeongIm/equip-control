cmake_minimum_required(VERSION 3.16)
project(equip-control-ec CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(ec
    src/main.cpp
    src/DeviceComm.cpp
    src/StateMachine.cpp
    src/AlarmManager.cpp
    src/HsmsServer.cpp
)

target_compile_options(ec PRIVATE -Wall -Wextra)

if(WIN32)
    target_link_libraries(ec ws2_32)
endif()

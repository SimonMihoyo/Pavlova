cmake_minimum_required(VERSION 3.14)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "macOS deployment target")
endif()

project(LegacyAgent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_executable(agentapp
    LegacyAgent/main.cpp
    LegacyAgent/ShellTool.cpp
    LegacyAgent/FileTool.cpp
    LegacyAgent/WebFetchTool.cpp
    LegacyAgent/JSONValue.cpp
    LegacyAgent/LLMClient.cpp
    LegacyAgent/AgentEngine.cpp
    LegacyAgent/SessionStore.cpp
    LegacyAgent/TuiApp.cpp
)

target_include_directories(agentapp PRIVATE LegacyAgent/headers)

if(APPLE)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    find_library(CFNETWORK_LIB CFNetwork)
    target_link_libraries(agentapp PRIVATE ${COREFOUNDATION_LIB} ${CFNETWORK_LIB})
endif()

# 系统自带 ncurses（macOS 内置 5.4，无需捆绑）
target_link_libraries(agentapp PRIVATE "-lncurses")

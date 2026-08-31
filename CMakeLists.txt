cmake_minimum_required(VERSION 3.14)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "macOS deployment target")
endif()

project(pavlova CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_executable(agentapp
    pavlova/main.cpp
    pavlova/ShellTool.cpp
    pavlova/FileTool.cpp
    pavlova/WebFetchTool.cpp
    pavlova/JSONValue.cpp
    pavlova/LLMClient.cpp
    pavlova/AgentEngine.cpp
    pavlova/SessionStore.cpp
    pavlova/TuiApp.cpp
)

target_include_directories(agentapp PRIVATE pavlova/headers)

if(APPLE)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    find_library(CFNETWORK_LIB CFNetwork)
    target_link_libraries(agentapp PRIVATE ${COREFOUNDATION_LIB} ${CFNETWORK_LIB})
endif()

# 系统自带 ncurses（macOS 内置 5.4，无需捆绑）
target_link_libraries(agentapp PRIVATE "-lncurses")

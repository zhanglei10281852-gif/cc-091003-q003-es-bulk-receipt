cmake_minimum_required(VERSION 3.16)
project(es_demo VERSION 1.0.0 LANGUAGES CXX)

# C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 查找 libcurl
find_package(CURL REQUIRED)

# ==================== nlohmann/json 依赖 ====================
# 自动下载 nlohmann/json 头文件（如果不存在）
set(JSON_HPP_PATH "${CMAKE_SOURCE_DIR}/include/json.hpp")
if(NOT EXISTS ${JSON_HPP_PATH})
    message(STATUS "正在下载 nlohmann/json...")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        ${JSON_HPP_PATH}
        SHOW_PROGRESS
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    if(NOT STATUS_CODE EQUAL 0)
        message(FATAL_ERROR "无法下载 nlohmann/json，请从 https://github.com/nlohmann/json/releases 手动下载")
    endif()
    message(STATUS "nlohmann/json 下载完成")
endif()

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CURL_INCLUDE_DIRS})

# 源文件
set(SOURCES
    src/main.cpp
    src/http_client.cpp
    src/es_client.cpp
)

# 可执行文件
add_executable(es_demo ${SOURCES})

# 链接库
target_link_libraries(es_demo PRIVATE ${CURL_LIBRARIES})

# 安装规则
install(TARGETS es_demo DESTINATION bin)

# 复制数据文件到构建目录
file(COPY ${CMAKE_SOURCE_DIR}/data DESTINATION ${CMAKE_BINARY_DIR})

# 输出配置信息
message(STATUS "C++ 标准: ${CMAKE_CXX_STANDARD}")
message(STATUS "CURL 头文件目录: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL 库: ${CURL_LIBRARIES}")

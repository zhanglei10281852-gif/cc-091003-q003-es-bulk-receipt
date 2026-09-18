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

# libcurl 仅主程序 es_demo 需要；离线回执演示与单元测试不依赖它。
find_package(CURL QUIET)

# ==================== nlohmann/json 依赖 ====================
# 优先使用仓库内置的多文件版本（include/nlohmann/json.hpp）；
# 否则回退为单头文件 include/json.hpp，缺失时自动下载。
set(NLOHMANN_DIR "${CMAKE_SOURCE_DIR}/include/nlohmann")
set(JSON_SINGLE_HEADER "${CMAKE_SOURCE_DIR}/include/json.hpp")
if(NOT EXISTS "${NLOHMANN_DIR}/json.hpp" AND NOT EXISTS ${JSON_SINGLE_HEADER})
    message(STATUS "正在下载 nlohmann/json...")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        ${JSON_SINGLE_HEADER}
        SHOW_PROGRESS
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    if(NOT STATUS_CODE EQUAL 0)
        message(FATAL_ERROR "无法下载 nlohmann/json，请从 https://github.com/nlohmann/json/releases 手动下载")
    endif()
    message(STATUS "nlohmann/json 下载完成")
endif()

# ==================== 主程序（需要 libcurl） ====================
if(CURL_FOUND)
    include_directories(${CURL_INCLUDE_DIRS})

    set(SOURCES
        src/main.cpp
        src/http_client.cpp
        src/es_client.cpp
        src/bulk_indexer.cpp
        src/bulk_transport_http.cpp
    )

    add_executable(es_demo ${SOURCES})
    target_include_directories(es_demo PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(es_demo PRIVATE ${CURL_LIBRARIES})

    install(TARGETS es_demo DESTINATION bin)

    message(STATUS "已配置主程序 es_demo")
    message(STATUS "CURL 头文件目录: ${CURL_INCLUDE_DIRS}")
    message(STATUS "CURL 库: ${CURL_LIBRARIES}")
else()
    message(WARNING "未找到 libcurl，跳过主程序 es_demo；离线演示与单元测试仍可构建")
endif()

# ==================== 离线演示与单元测试（不依赖 libcurl/真实 ES） ====================
# 只链接批量写入的纯逻辑部分；传输层在演示/测试中由脚本化内存版 ES 代替。
add_executable(bulk_receipt_demo
    demo/bulk_receipt_demo.cpp
    src/bulk_indexer.cpp
)
target_include_directories(bulk_receipt_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/testutil
)

add_executable(test_bulk_indexer
    tests/test_bulk_indexer.cpp
    src/bulk_indexer.cpp
)
target_include_directories(test_bulk_indexer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/testutil
)

enable_testing()
add_test(NAME bulk_indexer COMMAND test_bulk_indexer)

# 复制数据文件到构建目录
file(COPY ${CMAKE_SOURCE_DIR}/data DESTINATION ${CMAKE_BINARY_DIR})

message(STATUS "C++ 标准: ${CMAKE_CXX_STANDARD}")

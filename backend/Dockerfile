# ===========================================
# Elasticsearch C++ 示例程序多阶段构建
# 支持 ARM64 和 AMD64 架构
# ===========================================

# 第一阶段：构建
FROM --platform=$BUILDPLATFORM debian:bookworm-slim AS builder

# 安装构建依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libcurl4-openssl-dev \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /app

# 下载 nlohmann/json 头文件
RUN mkdir -p include && \
    curl -sL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp -o include/json.hpp

# 复制源代码
COPY CMakeLists.txt .
COPY include/http_client.hpp include/
COPY include/es_client.hpp include/
COPY src/ src/
COPY data/ data/

# 构建项目
RUN mkdir build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=Release .. \
    && make -j$(nproc)

# 第二阶段：运行
FROM --platform=$TARGETPLATFORM debian:bookworm-slim AS runtime

# 安装运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 创建非 root 用户
RUN useradd -m -s /bin/bash appuser

# 设置工作目录
WORKDIR /app

# 从构建阶段复制可执行文件和数据
COPY --from=builder /app/build/es_demo .
COPY --from=builder /app/build/data ./data

# 设置文件权限
RUN chown -R appuser:appuser /app

# 切换到非 root 用户
USER appuser

# 设置环境变量
ENV ES_HOST=localhost
ENV ES_PORT=9200

# 运行程序
CMD ["./es_demo"]

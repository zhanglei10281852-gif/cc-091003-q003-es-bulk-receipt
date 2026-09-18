# Elasticsearch 全文检索 C++ 示例

基于 C++17 实现的 Elasticsearch 全文检索演示项目，展示如何使用 C++ 与 Elasticsearch 进行交互，实现索引管理、文档 CRUD 和全文检索功能。

## 运行方式

### 方式一：Docker Compose（推荐）

```bash
# 1. 启动所有服务
docker-compose up --build -d

# 2. 查看 C++ 演示程序输出
docker logs es-demo-cpp

# 3. 停止服务
docker-compose down

# 4. （可选）启用 Kibana 可视化界面
docker-compose --profile kibana up -d
```

### 方式二：本地编译运行

需要先安装依赖：libcurl-dev

```bash
# Ubuntu/Debian
sudo apt-get install libcurl4-openssl-dev

# macOS
brew install curl

# 1. 启动 Elasticsearch
docker-compose up -d elasticsearch

# 2. 编译 C++ 项目（CMake 会自动下载 nlohmann/json）
cd backend
mkdir build && cd build
cmake ..
make

# 3. 运行程序
./es_demo
```

## 服务说明

| 服务          | 端口 | 说明                       |
| ------------- | ---- | -------------------------- |
| Elasticsearch | 9200 | 搜索引擎服务               |
| Kibana        | 5601 | ES 可视化管理（可选）      |
| cpp-demo      | -    | C++ 演示程序（一次性运行） |

### 访问地址

- Elasticsearch: http://localhost:9200
- Kibana: http://localhost:5601 （需使用 `--profile kibana` 启动）

## 认证说明

本项目为开发演示环境，已禁用安全认证（`xpack.security.enabled=false`），无需用户名密码即可访问。

> ⚠️ 生产环境请务必启用安全认证。

## 功能特性

### 索引管理

- ✅ 创建索引（支持自定义 mapping）
- ✅ 删除索引
- ✅ 查看索引信息

### 文档操作

- ✅ 添加文档
- ✅ 批量添加文档
- ✅ 可靠批量导入：可控分块、逐项回执、有界退避重试（见下）
- ✅ 获取文档
- ✅ 更新文档
- ✅ 删除文档

### 可靠批量导入（`bulkIndexWithReceipt`）

针对限流导致的「同批部分成功 / 部分 429、还夹着坏记录」场景：

- **稳定业务 ID 幂等**：调用方为每条文章提供非空、唯一的业务 ID；
  整批重放或「响应前断开后重发」只覆盖同 `_id` 文档，**不会产生第二份文章**。
- **可控分块**：同时受每块条目数（`chunkSize`）与请求体字节数（`maxChunkBytes`）约束。
- **逐项回执并保持原序**：每条给出最终状态、尝试次数、HTTP 状态与精简错误原因。
  - 🟢 **已写入**（首次成功）／🟡 **重试后成功**（429、502/503/504、断连后成功）
  - 🔴 **不可重试**（mapping 等 4xx 永久错误，立即留在失败清单，不浪费重试）
  - 另有 **重试耗尽**（可重试错误达到 `maxAttempts`，稍后再投即可）
- **发送前拒绝**：空批次、ID 数量不匹配、空/重复 ID、单条超过分块字节限制，
  抛 `BulkValidationError`，保证尚未发出任何请求。
- **可替换时钟与抖动**：`ISleeper`/`IJitter` 可注入，自动化测试零真实等待即可验证重试边界。

详见 [docs/bulk_import.md](docs/bulk_import.md)。

#### 离线回执演示与自动化测试（无需 Elasticsearch）

```bash
cd backend

# 运营回执演示：脚本化假 ES 复现「成功 + 429 重试成功 + mapping 坏数据 + 断连重放」，
# 并再次提交同一业务批次，展示三类记录与文档总数不变。
g++ -std=c++17 -Iinclude -I<nlohmann json 头文件目录> \
    src/bulk_demo.cpp src/bulk_writer.cpp src/bulk_test_support.cpp -o bulk_demo && ./bulk_demo

# 自动化测试（132+ 断言，零真实等待）
g++ -std=c++17 -Iinclude -I<nlohmann json 头文件目录> \
    tests/bulk_writer_test.cpp src/bulk_writer.cpp src/bulk_test_support.cpp \
    -o bulk_test && ./bulk_test

# 或用 CMake（完整工程；Docker 构建阶段会自动执行 ctest）
cmake -S . -B build && cmake --build build && (cd build && ctest --output-on-failure)
```

回执演示输出形如：

```
[第一次提交] 逐项回执（保持原输入顺序）
  序号  业务ID        状态          尝试  HTTP  精简错误原因
  1     partner-001   重试后成功     2     201   -
  ...
  3     partner-003   不可重试       2     400   mapper_parsing_exception: failed to parse ...
  5     partner-005   重试后成功     2     201   -
  汇总：已写入 2，重试后成功 3，不可重试 1
  搜索库文档总数（第一次提交后）：5
[第二次提交] ... 文档总数仍为 5  ✓ 重复提交安全
```

### 全文检索

- ✅ Match 查询（分词匹配）
- ✅ Multi-Match 查询（多字段搜索）
- ✅ Term 查询（精确匹配）
- ✅ Bool 组合查询
- ✅ 高亮显示
- ✅ 分页查询

### 分词说明

本 Demo 使用 Elasticsearch 内置的 `standard` 分词器。`standard` 分词器对中文采用单字切分（Unigram），例如"人工智能"会被切分为"人"、"工"、"智"、"能"四个 token。

如需真正的中文词语切分（如将"人工智能"作为一个完整词语），需要：

1. 安装 [IK 分词器插件](https://github.com/medcl/elasticsearch-analysis-ik)
2. 修改索引 mapping 中的 `analyzer` 为 `ik_max_word`（最细粒度）或 `ik_smart`（智能切分）

示例配置见下方"扩展开发"章节。

## 技术栈

- **语言**: C++17
- **HTTP 客户端**: libcurl
- **JSON 处理**: nlohmann/json（CMake 自动下载）
- **搜索引擎**: Elasticsearch 8.11.0
- **构建工具**: CMake 3.16+
- **容器化**: Docker & Docker Compose

## 项目结构

```
.
├── backend/                 # C++ 后端代码
│   ├── CMakeLists.txt      # CMake 构建配置
│   ├── Dockerfile          # Docker 镜像构建
│   ├── include/            # 头文件
│   │   ├── es_client.hpp   # ES 客户端类
│   │   ├── bulk_writer.hpp # 可靠批量写入（分块/回执/退避，传输可注入）
│   │   ├── bulk_test_support.hpp # 内存假 ES / 假时钟（演示与测试用）
│   │   ├── http_client.hpp # HTTP 客户端类
│   │   └── json.hpp        # nlohmann/json 库
│   ├── src/                # 源代码
│   │   ├── main.cpp        # 主程序入口
│   │   ├── bulk_demo.cpp   # 离线运营回执演示
│   │   ├── bulk_writer.cpp # 可靠批量写入实现
│   │   ├── bulk_test_support.cpp # 假 ES 实现
│   │   ├── es_client.cpp   # ES 客户端实现
│   │   └── http_client.cpp # HTTP 客户端实现
│   ├── tests/              # 自动化测试
│   │   └── bulk_writer_test.cpp # 批量写入测试（零真实等待）
│   └── data/               # 示例数据
│       └── sample_data.json
├── docs/                   # 文档
│   └── project_design.md   # 项目设计文档
├── docker-compose.yml      # Docker Compose 配置
├── .gitignore             # Git 忽略文件
└── README.md              # 项目说明
```

## 使用示例

程序运行后会自动执行以下演示：

1. **创建索引** - 创建名为 `articles` 的索引，配置中文分词
2. **批量导入** - 导入示例文章数据
3. **全文检索** - 演示各种搜索方式
4. **高亮显示** - 展示搜索结果高亮
5. **清理资源** - 删除测试索引

### 输出示例

```
========================================
  Elasticsearch C++ 全文检索 DEMO
========================================

[1] 创建索引 'articles'...
✓ 索引创建成功

[2] 批量导入文档...
✓ 成功导入 5 篇文章

[3] 全文检索演示...

--- Match 查询: "人工智能" ---
命中 2 条结果:
  [1] 人工智能的发展历程 (score: 8.234)
  [2] 机器学习入门指南 (score: 5.123)

--- 高亮搜索: "深度学习" ---
  标题: 深度学习实战
  高亮: ...<em>深度学习</em>是机器学习的一个分支...

[4] 清理资源...
✓ 索引删除成功

========================================
  演示完成！
========================================
```

## 扩展开发

### 启用中文分词（IK 分词器）

如需真正的中文分词能力，可以使用带 IK 分词器的 Elasticsearch 镜像：

```yaml
# docker-compose.yml 中替换 elasticsearch 镜像
elasticsearch:
  image: elasticsearch-ik:8.11.0 # 需自行构建或使用社区镜像
```

然后修改索引 mapping：

```cpp
json mapping = {
    {"properties", {
        {"title", {{"type", "text"}, {"analyzer", "ik_max_word"}}},
        {"content", {{"type", "text"}, {"analyzer", "ik_smart"}}},
        {"tags", {{"type", "keyword"}}},
        {"created_at", {{"type", "date"}}}
    }}
};
client.createIndex("my_index", mapping);
```

### 添加新的搜索功能

```cpp
// 在 es_client.hpp 中添加新方法
SearchResult fuzzySearch(const std::string& index,
                         const std::string& field,
                         const std::string& value,
                         int fuzziness = 2);
```

## 许可证

MIT License

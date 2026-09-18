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

# 2. 编译 C++ 项目（nlohmann/json 已内置；未装 libcurl 时只构建离线演示与测试）
cd backend
mkdir build && cd build
cmake ..
make
ctest --output-on-failure   # 可选：运行批量导入重试边界单元测试

# 3. 运行程序
./es_demo
# 或先看无需 ES 的逐项回执演示：
./bulk_receipt_demo
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
- ✅ **可靠批量导入：可控分块 + 逐项回执 + 有上限退避重试**（见下节）
- ✅ 获取文档
- ✅ 更新文档
- ✅ 删除文档

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

## 可靠批量导入（逐项回执）

针对资料运营成批导入合作方文章时“部分成功、部分 429、坏记录混入”的场景，
`ESClient::bulkIndexWithReceipts` 在普通批量写入之上提供：

- **可控分块**：`BulkOptions{chunkSize, maxChunkBytes}` 同时按条数与 NDJSON
  字节数切块；空批次、ID 数量不匹配、ID 空/重复、单条超过分块上限都在
  **发送前**抛出 `BulkValidationError`，不产生任何 HTTP 请求。
- **逐项回执**：每条文章必须带稳定业务 ID；返回的 `BulkReport::items`
  **严格保持原输入顺序**，每项给出最终状态、尝试次数、最终 HTTP 状态、
  ES 结果（created/updated + version）及精简单行错误原因。
- **三类终态一目了然**：`已写入`（首次成功）、`重试后写入`、`不可重试失败`
  （另有退避用尽的 `重试耗尽`、响应前断连的 `结果未确认` 两种值守状态）。
- **有上限退避，只重试未确认项**：仅对 **429 / 502 / 503 / 504** 及响应到达前的
  短暂传输失败重试，指数退避 + full jitter，受 `RetryPolicy{maxAttempts,
  initialBackoff, maxBackoff, multiplier}` 封顶；mapping 校验失败（400）等
  永久错误立即留在失败清单。分块整体被限流时重发整块，200 响应中个别条目
  失败时**只重发这些条目**。
- **断连重放不产生重复**：动作行固定携带业务 `_id`
  （`{"index":{"_index":...,"_id":...}}`），响应到达前断连后重发同一分块，
  ES 端按 `_id` 幂等覆盖，不会生成第二份文章。
- **时钟与抖动可替换**：`RetryPolicy::sleeper` 与 `jitter` 可注入替身，
  自动化测试无需真实等待即可断言退避次数、倍数与封顶。

### 离线回执演示（无需 ES / libcurl，可直接编译运行）

```bash
cd backend
g++ -std=c++17 -Iinclude -Itestutil \
    src/bulk_indexer.cpp demo/bulk_receipt_demo.cpp -o /tmp/bulk_receipt_demo
/tmp/bulk_receipt_demo
```

演示复现一个事故批次（6 篇文章，混发条目 429、mapping 400、响应前断连），
打印逐项回执，并重放同一业务批次确认文档总数不变。同目录单元测试：

```bash
g++ -std=c++17 -Iinclude -Itestutil \
    src/bulk_indexer.cpp tests/test_bulk_indexer.cpp -o /tmp/test_bulk_indexer
/tmp/test_bulk_indexer   # 125 项断言，覆盖重试边界与重放幂等
```

### 调用示例

```cpp
es::BulkOptions options{/*chunkSize=*/500, /*maxChunkBytes=*/5_MiB};
es::RetryPolicy retry;
retry.maxAttempts = 3;                 // 最多发送 3 次
retry.initialBackoff = 100ms;
retry.maxBackoff = 5s;                 // 退避封顶
// 自动化场景：retry.sleeper = [](auto){};  retry.jitter = []{ return 1.0; };

es::BulkReport report = client.bulkIndexWithReceipts(
    "articles", docs, businessIds, options, retry);

for (const auto& r : report.items) {   // 顺序与输入完全一致
    // r.status / r.attempts / r.httpStatus / r.errorReason
}
```

## 技术栈
- **语言**: C++17
- **HTTP 客户端**: libcurl
- **JSON 处理**: nlohmann/json（已内置在 backend/include/nlohmann/）
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
│   │   ├── es_client.hpp   # ES 客户端类（含 bulkIndexWithReceipts）
│   │   ├── bulk_indexer.hpp# 分块/逐项回执/退避重试核心（传输抽象可替换）
│   │   ├── http_client.hpp # HTTP 客户端类
│   │   └── nlohmann/       # 内置 nlohmann/json
│   ├── src/                # 源代码
│   │   ├── main.cpp        # 主程序入口
│   │   ├── es_client.cpp   # ES 客户端实现
│   │   ├── bulk_indexer.cpp
│   │   ├── bulk_transport_http.cpp # libcurl 传输适配器
│   │   └── http_client.cpp # HTTP 客户端实现
│   ├── demo/               # 离线逐项回执演示
│   │   └── bulk_receipt_demo.cpp
│   ├── tests/              # 离线单元测试
│   │   └── test_bulk_indexer.cpp
│   ├── testutil/           # 测试工具
│   │   └── fake_es.hpp     # 脚本化内存版 ES（429/400/断连注入）
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
2. **批量导入（逐项回执）** - 导入示例文章并逐条打印最终状态/尝试次数/HTTP
3. **全文检索** - 演示各种搜索方式
4. **高亮显示** - 展示搜索结果高亮
5. **清理资源** - 删除测试索引

容器启动时还会先运行 `bulk_receipt_demo`：在脚本化的内存版 ES 上复现
429 限流、mapping 坏记录与响应前断连，展示三类回执和重放幂等，无需真实 ES。

### 输出示例

```
========================================
  Elasticsearch C++ 全文检索 DEMO
========================================

[1] 创建索引 'articles'...
✓ 索引创建成功

[2] 批量导入文档（逐项回执）...

  序号  业务ID  最终状态          尝试  HTTP  ES结果
  1     1       已写入            1     201   created
  2     2       已写入            1     201   created
  ...

  已写入 5，重试后写入 0，不可重试失败 0，重试耗尽 0，结果未确认 0
  计划分块 3 个，实际发送 3 次
✓ 全部 5 篇文章导入成功

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

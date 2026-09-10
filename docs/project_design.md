# Elasticsearch 全文检索 C++ 示例项目设计

## 1. 系统架构

```mermaid
flowchart TD
    subgraph Client["C++ 客户端"]
        A[main.cpp] --> B[ESClient 类]
        B --> C[HttpClient 模块]
        B --> D[JSON 处理模块]
    end

    subgraph ES["Elasticsearch 服务"]
        E[REST API :9200]
        F[索引管理]
        G[文档 CRUD]
        H[全文检索引擎]
    end

    C -->|HTTP/REST| E
    E --> F
    E --> G
    E --> H
```

## 2. 模块设计

```mermaid
classDiagram
    class ESClient {
        -string baseUrl
        -HttpClient httpClient
        +createIndex(indexName, mappings)
        +deleteIndex(indexName)
        +indexDocument(index, id, doc)
        +getDocument(index, id)
        +deleteDocument(index, id)
        +search(index, query)
        +bulkIndex(index, docs)
    }

    class HttpClient {
        +get(url, headers)
        +post(url, body, headers)
        +put(url, body, headers)
        +delete(url, headers)
    }

    class Document {
        +string id
        +string title
        +string content
        +string author
        +string created_at
    }

    ESClient --> HttpClient
    ESClient --> Document
```

## 3. 功能清单

| 功能模块 | 功能点     | 说明                           |
| -------- | ---------- | ------------------------------ |
| 索引管理 | 创建索引   | 支持自定义 mapping 和 settings |
| 索引管理 | 删除索引   | 删除指定索引                   |
| 索引管理 | 查看索引   | 获取索引信息                   |
| 文档操作 | 添加文档   | 单条/批量添加                  |
| 文档操作 | 获取文档   | 根据 ID 获取                   |
| 文档操作 | 更新文档   | 更新指定文档                   |
| 文档操作 | 删除文档   | 删除指定文档                   |
| 全文检索 | Match 查询 | 分词匹配查询                   |
| 全文检索 | Term 查询  | 精确匹配查询                   |
| 全文检索 | Bool 查询  | 组合条件查询                   |
| 全文检索 | 高亮显示   | 搜索结果高亮                   |
| 全文检索 | 分页查询   | 支持 from/size                 |

## 4. API 接口设计

### 4.1 索引管理

- `PUT /{index}` - 创建索引
- `DELETE /{index}` - 删除索引
- `GET /{index}` - 获取索引信息

### 4.2 文档操作

- `POST /{index}/_doc/{id}` - 添加/更新文档
- `GET /{index}/_doc/{id}` - 获取文档
- `DELETE /{index}/_doc/{id}` - 删除文档
- `POST /{index}/_bulk` - 批量操作

### 4.3 搜索接口

- `POST /{index}/_search` - 搜索文档

## 5. 技术选型

| 组件        | 技术          | 版本  |
| ----------- | ------------- | ----- |
| 编程语言    | C++           | 17    |
| HTTP 客户端 | libcurl       | 7.x   |
| JSON 库     | nlohmann/json | 3.x   |
| 搜索引擎    | Elasticsearch | 8.x   |
| 构建工具    | CMake         | 3.16+ |
| 容器化      | Docker        | 20.x  |

## 6. 目录结构

```
es-cpp-demo/
├── backend/
│   ├── CMakeLists.txt
│   ├── Dockerfile
│   ├── include/
│   │   ├── es_client.hpp
│   │   ├── http_client.hpp
│   │   └── json.hpp
│   ├── src/
│   │   ├── main.cpp
│   │   ├── es_client.cpp
│   │   └── http_client.cpp
│   └── data/
│       └── sample_data.json
├── docker-compose.yml
├── .gitignore
├── README.md
└── docs/
    └── project_design.md
```

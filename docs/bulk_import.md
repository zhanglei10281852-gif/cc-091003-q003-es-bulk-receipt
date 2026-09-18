# 可靠批量导入设计（可控分块 + 逐项回执 + 退避重试）

## 1. 背景与目标

资料运营每天把合作方文章成批导入搜索库。Elasticsearch 限流（429）时同一批请求
部分成功、部分失败，还夹着格式错误记录；旧 `bulkIndex` 只回成功/失败数量，
值班人员无法知道重试谁，也担心整批重放产生重复文档。

目标：

- 调用方为每条文章提供**稳定业务 ID**，整批重放 / 响应前断开后重发都不产生第二份文档；
- **可控分块**：按条目数与请求体字节数拆分，超限单条在发送前拒绝；
- **逐项回执**：严格保持原输入顺序，含最终状态、尝试次数、HTTP 状态、精简错误原因；
- **分类重试**：429/502/503/504 与响应前传输失败只重试**尚未确认**的条目，退避有上限；
  mapping 校验（400 等）等永久错误立即定案，不消耗重试额度；
- **时钟与抖动可替换**：自动化场景用假时钟零真实等待即可证明重试边界。

## 2. 核心类型（`include/bulk_writer.hpp`）

| 类型 | 说明 |
| ---- | ---- |
| `IBulkTransport` | 分块传输抽象。生产实现 `CurlBulkTransport` 走 libcurl；测试用 `FakeBulkTransport`。约定：拿到任何 HTTP 响应都返回；仅响应到达前断链抛 `TransientTransportError`。 |
| `ISleeper` | 退避时钟。默认真实睡眠；测试用 `ScriptedSleeper` 记录每次毫秒数。 |
| `IJitter` | `[0,1)` 抖动源。默认随机；测试用 `ScriptedJitter` 确定性返回。 |
| `BulkOptions` | `chunkSize`、`maxChunkBytes`、`maxAttempts`、`initialDelay`、`maxDelay`、`backoffMultiplier`。 |
| `ItemReceipt` | 单条：`index`（原序）、`id`、`status`、`attempts`、`httpStatus`、`esResult`、`error`。 |
| `BulkReceipt` | 整批：`items`（原序）、`chunkCount`、`requestCount`、`backoffCount` 与统计方法。 |

### 最终状态（运营只需区分三类）

| 状态 | 含义 | 处置 |
| ---- | ---- | ---- |
| `Written`（已写入） | 首次尝试即 2xx | 无需处理 |
| `RetriedSuccess`（重试后成功） | 经历过 429/5xx/断连后确认写入 | 无需处理 |
| `FailedPermanent`（不可重试） | mapping 等 4xx 永久错误，立即留在失败清单 | 修正数据后以**同一 ID** 重投 |
| `RetriesExhausted`（重试耗尽） | 错误可重试但达到 `maxAttempts` | 稍后重新提交该业务批次 |

## 3. 幂等保证

- bulk action 固定为 `{"index": {"_index", "_id"}}`，且 `_id` 由调用方提供。
  相同 `_id` 的 `index` 是覆盖语义（先 created，再次 updated），**不新增文档**。
- 响应到达前断开时写入结果未知，安全做法就是原样重发同一分块；
  已落库的条目被覆盖、未落库的被创建，文档总数与「只执行一次」一致。
- 因此值班人员可以放心**整批再次提交**，文档总数不变（演示与自动化测试均断言这一点）。

## 4. 重试与退避

- 可重试状态：HTTP **429 / 502 / 503 / 504**，以及传输层失败（curl error、响应体无法对齐）。
- 整块 429/5xx：整组未确认条目一起退避后重发。
- 200 但 `items[]` 内逐条失败：成功的立即确认；逐条 429/5xx 只把对应条目带入下一次请求；
  逐条 4xx 立即永久定案。
- 退避：指数 + 全抖动
  `delay = random(0, min(maxDelay, initialDelay * multiplier^round))`，
  受 `maxDelay` 封顶，由可替换的 `ISleeper`/`IJitter` 驱动。
- 单条 `attempts` 只在它实际被发送时递增，达到 `maxAttempts` 即定案为 `RetriesExhausted`。

## 5. 发送前校验（抛 `BulkValidationError`，保证尚未发任何请求）

- 空批次；
- ID 数量与文档数量不匹配；
- 存在空 ID 或批内重复 ID；
- 文档不是 JSON 对象、无法序列化（如非法 UTF-8）；
- 单条序列化后（action 行 + source 行）字节数超过 `maxChunkBytes`。

## 6. 错误原因精简

`error` 取 ES 错误体的 `error.type` + `error.reason`（兼容 `root_cause` 与字符串形态），
压成单行并截断到约 240 字符；传输失败时为 curl 错误描述；成功时为空。

## 7. 如何验证

```bash
cd backend

# 离线自动化测试（假 ES + 假时钟，零真实等待、无需 Elasticsearch/libcurl 链接）
g++ -std=c++17 -Iinclude -I<nlohmann json 头文件目录> \
    src/bulk_writer.cpp src/bulk_test_support.cpp tests/bulk_writer_test.cpp \
    -o /tmp/bulk_writer_test && /tmp/bulk_writer_test

# 运营回执演示（离线，直接看到三类记录与「再投总数不变」结论）
g++ -std=c++17 -Iinclude ... src/bulk_writer.cpp src/bulk_test_support.cpp \
    src/bulk_demo.cpp -o /tmp/bulk_receipt_demo && /tmp/bulk_receipt_demo

# 完整工程（需要 libcurl 与真实/容器化 Elasticsearch）
cmake -S . -B build && cmake --build build && (cd build && ctest --output-on-failure)
```

真机链路另经 libcurl 对接脚本化 HTTP 假服务验证：响应前断连重放、逐条 429 重试成功、
400 永久失败、整批再投文档总数不变。

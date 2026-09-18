// BulkIndexer 单元测试：不依赖 libcurl 与真实 Elasticsearch。
// 退避时钟与抖动全部替换为确定性替身，用断言证明重试边界。
#include "bulk_indexer.hpp"
#include "fake_es.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace es;
using json = nlohmann::json;
using fake::FakeBulkServer;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::cerr << "断言失败 [" << __FILE__ << ":" << __LINE__           \
                      << "] " << #cond << "\n";                                \
        }                                                                      \
    } while (0)

#define CHECK_THROWS_VALIDATION(expr)                                          \
    do {                                                                       \
        ++g_checks;                                                            \
        bool caught = false;                                                   \
        try {                                                                  \
            expr;                                                              \
        } catch (const BulkValidationError&) {                                 \
            caught = true;                                                     \
        }                                                                      \
        if (!caught) {                                                         \
            ++g_failures;                                                      \
            std::cerr << "断言失败 [" << __FILE__ << ":" << __LINE__           \
                      << "] 期望 BulkValidationError: " #expr << "\n";         \
        }                                                                      \
    } while (0)

json doc(const std::string& title) {
    return json{{"title", title}, {"created_at", "2024-01-01"}};
}

std::vector<json> docs(std::initializer_list<std::string> titles) {
    std::vector<json> v;
    for (const auto& t : titles) v.push_back(doc(t));
    return v;
}

std::vector<std::string> ids(std::initializer_list<std::string> v) {
    return std::vector<std::string>(v);
}

/// 不真实等待、抖动固定为 1（退避时长取满档）的策略
RetryPolicy fastPolicy(int maxAttempts = 3) {
    RetryPolicy p;
    p.maxAttempts = maxAttempts;
    p.initialBackoff = std::chrono::milliseconds{100};
    p.maxBackoff = std::chrono::milliseconds{5000};
    p.multiplier = 2.0;
    p.sleeper = [](std::chrono::milliseconds) {};
    p.jitter = [] { return 1.0; };
    return p;
}

// ==================== 发送前校验 ====================

void testValidationRejectsBeforeSending() {
    FakeBulkServer server;
    BulkIndexer indexer(server, "articles", BulkOptions{}, fastPolicy());

    CHECK_THROWS_VALIDATION(indexer.run({}, {}));                       // 空批次
    CHECK_THROWS_VALIDATION(indexer.run(docs({"a", "b"}), ids({"x"}))); // 数量不匹配
    CHECK_THROWS_VALIDATION(
        indexer.run(docs({"a", "b"}), ids({"x", ""})));                 // 空 ID
    CHECK_THROWS_VALIDATION(
        indexer.run(docs({"a", "b"}), ids({"x", "x"})));                // ID 重复
    CHECK_THROWS_VALIDATION(indexer.run(
        std::vector<json>{json::array()}, ids({"x"})));                 // 非对象记录

    // 单条数据超过分块字节上限
    BulkOptions small{500, 128};
    BulkIndexer strictIndexer(server, "articles", small, fastPolicy());
    json big = json{{"content", std::string(200, 'x')}};
    CHECK_THROWS_VALIDATION(strictIndexer.run({big}, {"big"}));

    // 分块参数本身非法
    CHECK_THROWS_VALIDATION(
        BulkIndexer(server, "articles", BulkOptions{0, 1024}, fastPolicy()));

    // 全部在发送前拒绝：没有产生任何请求
    CHECK(server.requestCount == 0);
}

// ==================== 顺序与分块 ====================

void testOrderPreservedAndChunking() {
    FakeBulkServer server;
    BulkOptions opts{2, 5 * 1024 * 1024};
    BulkIndexer indexer(server, "articles", opts, fastPolicy());

    auto input = docs({"t0", "t1", "t2", "t3", "t4"});
    auto report = indexer.run(input, ids({"a", "b", "c", "d", "e"}));

    CHECK(report.chunksPlanned == 3);
    CHECK(report.requestsSent == 3);
    // 回执严格保持原输入顺序
    CHECK(report.items.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(report.items[i].index == i);
        CHECK(report.items[i].id == std::string(1, static_cast<char>('a' + i)));
        CHECK(report.items[i].status == ItemStatus::INDEXED);
        CHECK(report.items[i].attempts == 1);
        CHECK(report.items[i].httpStatus == 201);
        CHECK(report.items[i].chunkSeq == i / 2);
    }
    // 分块形状 2/2/1
    CHECK(server.requestIds.size() == 3);
    CHECK(server.requestIds[0] == ids({"a", "b"}));
    CHECK(server.requestIds[1] == ids({"c", "d"}));
    CHECK(server.requestIds[2] == ids({"e"}));
}

// ==================== 三类记录同批共存 ====================

void testThreeClassesInOneBatch() {
    FakeBulkServer server;
    server.setFault("c", FakeBulkServer::Fault::ITEM_RATE_LIMITED, 1);
    server.setFault("d", FakeBulkServer::Fault::ITEM_MAPPING_ERROR, 1);

    BulkIndexer indexer(server, "articles", BulkOptions{}, fastPolicy());
    auto report = indexer.run(docs({"a", "b", "c", "d"}),
                              ids({"a", "b", "c", "d"}));

    CHECK(report.items[0].status == ItemStatus::INDEXED);             // 已写入
    CHECK(report.items[1].status == ItemStatus::INDEXED);
    CHECK(report.items[2].status == ItemStatus::INDEXED_AFTER_RETRY); // 重试后写入
    CHECK(report.items[2].attempts == 2);
    CHECK(report.items[2].httpStatus == 201); // 首次 429 未落库，成功时 created
    CHECK(report.items[3].status == ItemStatus::PERMANENT_FAILURE);   // 不可重试
    CHECK(report.items[3].attempts == 1);
    CHECK(report.items[3].httpStatus == 400);
    CHECK(report.items[3].errorType == "mapper_parsing_exception");
    CHECK(!report.items[3].errorReason.empty());

    CHECK(report.indexedCount() == 2);
    CHECK(report.indexedAfterRetryCount() == 1);
    CHECK(report.permanentFailureCount() == 1);
    CHECK(report.writtenCount() == 3);
    CHECK(report.failedCount() == 1);

    // 第二次请求只携带尚未确认的 c；永久失败的 d 不再重发
    CHECK(server.requestIds.size() == 2);
    CHECK(server.requestIds[1] == ids({"c"}));
    CHECK(server.sendCountOf("d") == 1);
    // 退避只发生一次，固定抖动 1.0 => 100ms
    CHECK(report.backoffWaits.size() == 1);
    CHECK(report.backoffWaits[0] == std::chrono::milliseconds{100});
}

// ==================== 整批 429：退避序列与上限 ====================

void testWholeBatch429BackoffSequence() {
    FakeBulkServer server;
    server.setWholeRequestRejections(2);  // 前两次整批 429，第三次成功

    RetryPolicy p = fastPolicy(3);
    FakeBulkServer probe;                 // 仅用来直接量退避档位
    BulkIndexer probeIndexer(probe, "articles", BulkOptions{}, p);
    CHECK(probeIndexer.backoffDelay(1) == std::chrono::milliseconds{100});
    CHECK(probeIndexer.backoffDelay(2) == std::chrono::milliseconds{200});

    BulkIndexer indexer(server, "articles", BulkOptions{}, p);
    auto report = indexer.run(docs({"a", "b"}), ids({"a", "b"}));

    CHECK(server.requestCount == 3);
    for (const auto& r : report.items) {
        CHECK(r.status == ItemStatus::INDEXED_AFTER_RETRY);
        CHECK(r.attempts == 3);
        CHECK(r.httpStatus == 201);  // 最终状态取自成功响应
    }
    CHECK(report.backoffWaits ==
          (std::vector<std::chrono::milliseconds>{
              std::chrono::milliseconds{100}, std::chrono::milliseconds{200}}));
}

void testBackoffCappedAndJitterBoundary() {
    RetryPolicy p = fastPolicy(5);
    p.initialBackoff = std::chrono::milliseconds{100};
    p.maxBackoff = std::chrono::milliseconds{250};
    p.multiplier = 2.0;
    p.jitter = [] { return 1.0; };

    FakeBulkServer server;
    BulkIndexer indexer(server, "articles", BulkOptions{}, p);
    CHECK(indexer.backoffDelay(1) == std::chrono::milliseconds{100});
    CHECK(indexer.backoffDelay(2) == std::chrono::milliseconds{200});
    CHECK(indexer.backoffDelay(3) == std::chrono::milliseconds{250}); // 封顶
    CHECK(indexer.backoffDelay(4) == std::chrono::milliseconds{250}); // 不再增长

    // 抖动下边界 0：不等待，但仍重试
    p.jitter = [] { return 0.0; };
    BulkIndexer zeroJitter(server, "articles", BulkOptions{}, p);
    CHECK(zeroJitter.backoffDelay(3) == std::chrono::milliseconds{0});
}

void testRetryExhausted() {
    FakeBulkServer server;
    server.setWholeRequestRejections(5); // 始终 429

    BulkIndexer indexer(server, "articles", BulkOptions{}, fastPolicy(3));
    auto report = indexer.run(docs({"a"}), ids({"a"}));

    CHECK(report.requestsSent == 3);
    CHECK(report.items[0].status == ItemStatus::RETRY_EXHAUSTED);
    CHECK(report.items[0].attempts == 3);
    CHECK(report.items[0].httpStatus == 429);
    CHECK(!report.items[0].errorReason.empty());
    CHECK(server.docCount() == 0);
}

void testRetryableStatusClassification() {
    CHECK(isRetryableStatus(429));
    CHECK(isRetryableStatus(502));
    CHECK(isRetryableStatus(503));
    CHECK(isRetryableStatus(504));
    CHECK(!isRetryableStatus(200));
    CHECK(!isRetryableStatus(400)); // mapping 等永久错误
    CHECK(!isRetryableStatus(408)); // 请求超时不在重试白名单
    CHECK(!isRetryableStatus(500));
}

// ==================== 整批永久错误立即失败 ====================

void testWholeBatchPermanentError() {
    FakeBulkServer server;
    server.scriptedWholeResponses.push_back(HttpResponse{
        400,
        R"({"error":{"type":"illegal_argument_exception",)"
        R"("reason":"request body is malformed"},"status":400})",
        {}});

    BulkIndexer indexer(server, "articles", BulkOptions{}, fastPolicy());
    auto report = indexer.run(docs({"a", "b"}), ids({"a", "b"}));

    CHECK(report.requestsSent == 1);              // 不重试
    CHECK(report.backoffWaits.empty());
    CHECK(report.items[0].status == ItemStatus::PERMANENT_FAILURE);
    CHECK(report.items[1].status == ItemStatus::PERMANENT_FAILURE);
    CHECK(report.items[0].httpStatus == 400);
    CHECK(report.items[0].errorType == "illegal_argument_exception");
    CHECK(report.items[0].errorReason == "request body is malformed");
}

void testWholeBatch503IsRetried() {
    FakeBulkServer server;
    server.scriptedWholeResponses.push_back(HttpResponse{
        503, R"({"error":{"type":"unavailable_shards_exception",)"
             R"("reason":"primary shard not active"},"status":503})", {}});
    // 第二次请求走正常落库

    BulkIndexer indexer(server, "articles", BulkOptions{}, fastPolicy(3));
    auto report = indexer.run(docs({"a"}), ids({"a"}));

    CHECK(report.requestsSent == 2);
    CHECK(report.items[0].status == ItemStatus::INDEXED_AFTER_RETRY);
    CHECK(report.items[0].attempts == 2);
}

// ==================== 传输中断：重放不产生重复 ====================

void testDisconnectBeforeApplyRetried() {
    FakeBulkServer server;
    server.setFault("a", FakeBulkServer::Fault::DISCONNECT_BEFORE_APPLY, 1);

    BulkIndexer indexer(server, "articles", BulkOptions{}, fastPolicy(3));
    auto report = indexer.run(docs({"a"}), ids({"a"}));

    CHECK(report.requestsSent == 2);
    CHECK(report.items[0].status == ItemStatus::INDEXED_AFTER_RETRY);
    CHECK(report.items[0].httpStatus == 201);
    CHECK(server.docCount() == 1);
}

void testDisconnectAfterApplyReplayCreatesNoDuplicate() {
    FakeBulkServer server;
    // 第一次发送已落库，但响应在到达前断开；只允许尝试 1 次
    server.setFault("a", FakeBulkServer::Fault::DISCONNECT_AFTER_APPLY, 1);

    BulkIndexer first(server, "articles", BulkOptions{}, fastPolicy(1));
    auto r1 = first.run(docs({"a"}), ids({"a"}));

    CHECK(r1.items[0].status == ItemStatus::UNCONFIRMED);
    CHECK(r1.items[0].httpStatus == 0);
    CHECK(r1.items[0].attempts == 1);
    CHECK(server.docCount() == 1);  // 服务端实际已有一份

    // 值班人员用同一业务批次重发（相同分块、相同 _id）
    BulkIndexer second(server, "articles", BulkOptions{}, fastPolicy(3));
    auto r2 = second.run(docs({"a"}), ids({"a"}));

    CHECK(r2.items[0].written());
    CHECK(server.docCount() == 1);                 // 总数不变，没有第二份
    CHECK(server.versionOf("articles", "a") == 2); // 同 _id 幂等覆盖
}

void testReplaySameBatchKeepsDocCount() {
    FakeBulkServer server;
    BulkIndexer indexer(server, "articles", BulkOptions{}, fastPolicy());
    auto batch = docs({"a", "b", "c"});
    auto businessIds = ids({"a", "b", "c"});

    auto r1 = indexer.run(batch, businessIds);
    CHECK(r1.writtenCount() == 3);
    CHECK(server.docCount() == 3);

    // 再次提交完全相同的业务批次
    auto r2 = indexer.run(batch, businessIds);
    CHECK(r2.writtenCount() == 3);
    CHECK(server.docCount() == 3);                 // 总数不变
    for (const auto& id : businessIds) {
        CHECK(server.versionOf("articles", id) == 2); // 均为同 ID 更新
    }
}

// ==================== 注入的时钟被真实使用 ====================

void testInjectedClockAndSleeperUsed() {
    FakeBulkServer server;
    server.setWholeRequestRejections(1);

    std::vector<std::int64_t> sleptMs;
    RetryPolicy p = fastPolicy(2);
    p.sleeper = [&](std::chrono::milliseconds d) { sleptMs.push_back(d.count()); };
    p.jitter = [] { return 0.37; }; // 固定抖动 => 确定性时长 37ms

    BulkIndexer indexer(server, "articles", BulkOptions{}, p);
    auto report = indexer.run(docs({"a"}), ids({"a"}));

    CHECK(report.items[0].status == ItemStatus::INDEXED_AFTER_RETRY);
    CHECK(sleptMs.size() == 1);
    CHECK(sleptMs[0] == 37); // 自动化场景无需真实等待即可证明退避边界
}

} // namespace

int main() {
    testValidationRejectsBeforeSending();
    testOrderPreservedAndChunking();
    testThreeClassesInOneBatch();
    testWholeBatch429BackoffSequence();
    testBackoffCappedAndJitterBoundary();
    testRetryExhausted();
    testRetryableStatusClassification();
    testWholeBatchPermanentError();
    testWholeBatch503IsRetried();
    testDisconnectBeforeApplyRetried();
    testDisconnectAfterApplyReplayCreatesNoDuplicate();
    testReplaySameBatchKeepsDocCount();
    testInjectedClockAndSleeperUsed();

    std::cout << "完成 " << g_checks << " 项断言";
    if (g_failures == 0) {
        std::cout << "，全部通过 ✓\n";
        return 0;
    }
    std::cout << "，" << g_failures << " 项失败 ✗\n";
    return 1;
}

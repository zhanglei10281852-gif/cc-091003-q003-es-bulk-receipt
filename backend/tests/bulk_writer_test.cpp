// BulkWriter 自动化测试：全程使用内存假 ES 与可替换时钟/抖动，零真实等待。
// 退出码 0 表示全部通过。

#include "bulk_test_support.hpp"

#include <iostream>
#include <sstream>

using namespace es;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cout << "  [FAIL] " << message << "\n";
    }
}

json docOf(const std::string& title) {
    return json{{"title", title}, {"content", "c-" + title}};
}

std::shared_ptr<FakeBulkTransport> makeTransport() {
    return std::make_shared<FakeBulkTransport>();
}

BulkOptions fastOptions() {
    // 退避边界可预测：基数 100ms、倍数 2、上限 500ms
    BulkOptions o;
    o.chunkSize = 500;
    o.maxAttempts = 4;
    o.initialDelay = std::chrono::milliseconds(100);
    o.maxDelay = std::chrono::milliseconds(500);
    o.backoffMultiplier = 2.0;
    return o;
}

// 首次全部成功 ----------------------------------------------------------
void testAllWrittenFirstTry() {
    std::cout << "[test] all written on first try\n";
    auto t = makeTransport();
    auto sleeper = std::make_shared<ScriptedSleeper>();
    BulkWriter w(t, "articles", fastOptions(), sleeper,
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("a"), docOf("b"), docOf("c")},
                     {"art-1", "art-2", "art-3"});

    check(r.confirmedCount() == 3, "3 confirmed");
    check(r.failedCount() == 0, "0 failed");
    check(r.requestCount == 1, "exactly 1 request");
    check(r.backoffCount == 0, "no backoff");
    check(sleeper->sleepsMs.empty(), "sleeper never invoked");
    for (const auto& it : r.items) {
        check(it.status == ItemStatus::Written, "status Written");
        check(it.attempts == 1, "attempts == 1");
        check(it.httpStatus == 201, "http 201 created");
        check(it.error.empty(), "error empty");
    }
    // 回执严格保持输入顺序
    check(r.items[0].id == "art-1" && r.items[1].id == "art-2" &&
              r.items[2].id == "art-3",
          "receipt preserves input order");
    check(r.items[2].index == 2, "index field is original position");
    check(t->storedDocCount() == 3, "fake store has 3 docs");
}

// 单条 429 后成功：只重发未确认项 -------------------------------------
void testItemLevel429RetriesOnlyUnconfirmed() {
    std::cout << "[test] per-item 429 retries only the unconfirmed item\n";
    auto t = makeTransport();
    t->scriptItem("art-2", 429); // 仅 art-2 第一次 429
    auto sleeper = std::make_shared<ScriptedSleeper>();
    BulkWriter w(t, "articles", fastOptions(), sleeper,
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("a"), docOf("b"), docOf("c")},
                     {"art-1", "art-2", "art-3"});

    check(r.items[0].status == ItemStatus::Written, "art-1 written");
    check(r.items[1].status == ItemStatus::RetriedSuccess,
          "art-2 retried success");
    check(r.items[1].attempts == 2, "art-2 attempted twice");
    check(r.items[1].httpStatus == 201, "art-2 final http 201");
    check(r.items[2].status == ItemStatus::Written, "art-3 written");
    check(r.items[2].attempts == 1, "art-3 never resent");

    check(r.requestCount == 2, "2 requests total");
    check(t->history.size() == 2, "transport saw 2 requests");
    check(t->history[0].ids.size() == 3, "first request had 3 items");
    check(t->history[1].ids.size() == 1 && t->history[1].ids[0] == "art-2",
          "second request contained only art-2");
    check(r.backoffCount == 1, "one backoff between attempts");
    check(sleeper->sleepsMs.size() == 1 && sleeper->sleepsMs[0] == 100,
          "full-jitter at 1.0 => exactly initialDelay (100ms)");
    check(r.items[1].error.empty(), "success clears error");
}

// mapping 永久错误：立即定案、绝不重试 --------------------------------
void testMappingErrorIsPermanent() {
    std::cout << "[test] mapping error stays permanent, no retries\n";
    auto t = makeTransport();
    t->persistItemStatus("bad-1", 400); // mapping 校验失败
    auto sleeper = std::make_shared<ScriptedSleeper>();
    BulkWriter w(t, "articles", fastOptions(), sleeper,
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("ok"), docOf("bad")}, {"ok-1", "bad-1"});

    check(r.items[0].status == ItemStatus::Written, "good item written");
    check(r.items[0].attempts == 1, "good item not delayed by bad one");
    check(r.items[1].status == ItemStatus::FailedPermanent,
          "bad item failed_permanent");
    check(r.items[1].attempts == 1, "bad item attempted exactly once");
    check(r.items[1].httpStatus == 400, "http status 400 recorded");
    check(r.items[1].error.find("mapper_parsing_exception") !=
              std::string::npos,
          "error reason is concise and identifies mapper_parsing_exception");
    check(r.requestCount == 1, "single request, nothing unconfirmed to retry");
    check(r.backoffCount == 0, "no backoff for permanent failures");
    check(sleeper->sleepsMs.empty(), "sleeper never invoked");
    check(t->contains("ok-1") && !t->contains("bad-1"),
          "only good doc stored");
}

// 整请求 429 限流：整块退避后成功 --------------------------------------
void testWholeChunk429ThenSuccess() {
    std::cout << "[test] whole-chunk 429 then success\n";
    auto t = makeTransport();
    t->enqueueWholeStatus(429);
    auto sleeper = std::make_shared<ScriptedSleeper>();
    BulkWriter w(t, "articles", fastOptions(), sleeper,
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("a"), docOf("b")}, {"a", "b"});

    check(r.requestCount == 2, "request then retry");
    check(r.items[0].status == ItemStatus::RetriedSuccess &&
              r.items[1].status == ItemStatus::RetriedSuccess,
          "both retried_success");
    check(r.items[0].attempts == 2 && r.items[1].attempts == 2,
          "both attempted twice");
    check(r.items[0].httpStatus == 201, "final per-item http 201");
    check(r.items[0].error.empty(),
          "transient error reason cleared after success");
    check(sleeper->sleepsMs.size() == 1 && sleeper->sleepsMs[0] == 100,
          "100ms backoff on round 0");
    check(t->storedDocCount() == 2, "2 docs stored");
}

// 502/503/504 均可重试 --------------------------------------------------
void testGatewayStatusesRetriable() {
    std::cout << "[test] 502/503/504 are retriable, 401/404 are not\n";
    check(BulkWriter::isRetriableStatus(429), "429 retriable");
    check(BulkWriter::isRetriableStatus(502), "502 retriable");
    check(BulkWriter::isRetriableStatus(503), "503 retriable");
    check(BulkWriter::isRetriableStatus(504), "504 retriable");
    for (int s : {400, 401, 403, 404, 409, 500, 501}) {
        check(!BulkWriter::isRetriableStatus(s),
              "status " + std::to_string(s) + " not in retriable set");
    }
}

// 尝试次数上限：持续 429 最终定案、退避有界 ----------------------------
void testRetriesExhaustedWithBoundedBackoff() {
    std::cout << "[test] persistent 429 exhausts attempts with bounded backoff\n";
    auto t = makeTransport();
    t->persistItemStatus("hot", 429);
    auto sleeper = std::make_shared<ScriptedSleeper>();
    // jitter 恒为 1.0 => delay 取理论上限：100, 200, 400（< cap 500）
    BulkWriter w(t, "articles", fastOptions(), sleeper,
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("hot")}, {"hot"});

    check(r.items[0].status == ItemStatus::RetriesExhausted,
          "status retries_exhausted");
    check(r.items[0].attempts == 4, "attempts capped at maxAttempts (4)");
    check(r.items[0].httpStatus == 429, "http 429 retained");
    check(r.backoffCount == 3, "3 backoffs for 4 attempts");
    check(sleeper->sleepsMs.size() == 3, "sleeper invoked 3 times");
    check(sleeper->sleepsMs[0] == 100 && sleeper->sleepsMs[1] == 200 &&
              sleeper->sleepsMs[2] == 400,
          "exponential delays 100/200/400 under cap");
    for (long long ms : sleeper->sleepsMs)
        check(ms <= 500, "delay never exceeds maxDelay");
    check(!t->contains("hot"), "nothing stored for exhausted item");

    // jitter 恒为 0 => 退避时长为 0（自动化无需真实等待），但计数不变
    auto t2 = makeTransport();
    t2->persistItemStatus("hot", 429);
    auto s2 = std::make_shared<ScriptedSleeper>();
    BulkWriter w2(t2, "articles", fastOptions(), s2,
                  std::make_shared<ScriptedJitter>(0.0));
    auto r2 = w2.write({docOf("hot")}, {"hot"});
    check(r2.items[0].attempts == 4, "still 4 attempts with zero jitter");
    check(s2->sleepsMs == std::vector<long long>({0, 0, 0}),
          "zero-jitter sleeps are 0ms");
}

// 退避上限封顶：基数大时不超过 maxDelay --------------------------------
void testBackoffCap() {
    std::cout << "[test] backoff is capped at maxDelay\n";
    auto t = makeTransport();
    t->persistItemStatus("hot", 503);
    auto s = std::make_shared<ScriptedSleeper>();
    BulkOptions o = fastOptions();
    o.initialDelay = std::chrono::milliseconds(1000);
    o.maxDelay = std::chrono::milliseconds(500);
    o.maxAttempts = 5;
    BulkWriter w(t, "articles", o, s, std::make_shared<ScriptedJitter>(1.0));
    w.write({docOf("hot")}, {"hot"});
    check(s->sleepsMs.size() == 4, "4 backoffs");
    for (long long ms : s->sleepsMs) check(ms == 500, "capped at 500ms");
}

// 响应前断开：重放同块不产生重复文档 ----------------------------------
void testTransportDisconnectReplayNoDuplicates() {
    std::cout << "[test] disconnect before response: same chunk replayed safely\n";
    auto t = makeTransport();
    t->failNextRequestsTransport(1); // 首批在响应到达前断开
    auto s = std::make_shared<ScriptedSleeper>();
    BulkWriter w(t, "articles", fastOptions(), s,
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("a"), docOf("b"), docOf("c")},
                     {"a", "b", "c"});

    check(t->history.size() == 2, "2 physical requests");
    check(t->history[0].ids == t->history[1].ids,
          "identical chunk resent (same ids, same order)");
    for (const auto& it : r.items) {
        check(it.status == ItemStatus::RetriedSuccess,
              "id " + it.id + " retried_success");
        check(it.attempts == 2, "id " + it.id + " attempted twice");
        check(it.httpStatus == 201, "final 201");
    }
    check(t->storedDocCount() == 3, "exactly 3 docs despite replay");
    check(s->sleepsMs.size() == 1, "one backoff after disconnect");
}

// 再次提交同一业务批次：文档总数不变、版本递增 -------------------------
void testResubmitSameBatchKeepsDocCount() {
    std::cout << "[test] resubmitting same business batch changes no doc count\n";
    auto t = makeTransport();
    auto s = std::make_shared<ScriptedSleeper>();
    BulkWriter w(t, "articles", fastOptions(), s,
                 std::make_shared<ScriptedJitter>(1.0));
    std::vector<json> docs = {docOf("a"), docOf("b"), docOf("c")};
    std::vector<std::string> ids = {"a", "b", "c"};

    auto r1 = w.write(docs, ids);
    check(t->storedDocCount() == 3, "3 docs after first submit");
    long v1 = t->versionOf("a");

    // 模拟值班人员整批再提交
    auto r2 = w.write(docs, ids);
    check(t->storedDocCount() == 3, "still 3 docs after resubmit");
    check(t->versionOf("a") == v1 + 1, "same id overwritten (version bumped)");
    check(r2.confirmedCount() == 3, "all confirmed again");
    check(r2.failedCount() == 0, "no failures on resubmit");
    check(r2.requestCount == 1, "resubmit needed no retries");

    // 甚至第三次提交，文档总数依旧不变
    w.write(docs, ids);
    check(t->storedDocCount() == 3, "idempotent under repeated replays");
}

// 发送前校验：空批次 / ID 不匹配 / 空 ID / 重复 ID --------------------
void testValidationBeforeSend() {
    std::cout << "[test] invalid batches are rejected before any request\n";
    auto t = makeTransport();
    BulkWriter w(t, "articles", fastOptions(),
                 std::make_shared<ScriptedSleeper>(),
                 std::make_shared<ScriptedJitter>(1.0));

    auto expectReject = [&](const std::vector<json>& docs,
                            const std::vector<std::string>& ids,
                            const std::string& label) {
        std::size_t before = t->history.size();
        bool threw = false;
        try {
            w.write(docs, ids);
        } catch (const BulkValidationError& e) {
            threw = true;
            std::cout << "    rejected: " << e.what() << "\n";
        }
        check(threw, label + " throws BulkValidationError");
        check(t->history.size() == before, label + " sent zero requests");
    };

    expectReject({}, {}, "empty batch");
    expectReject({docOf("a"), docOf("b")}, {"a"}, "id count mismatch");
    expectReject({docOf("a"), docOf("b")}, {"a", "a"}, "duplicate id");
    expectReject({docOf("a")}, {""}, "empty id");
    expectReject({json::array({1, 2})}, {"arr"}, "non-object document");
}

// 单条超过分块字节限制：发送前拒绝 ------------------------------------
void testOversizedSingleDocumentRejected() {
    std::cout << "[test] single document exceeding chunk byte limit rejected\n";
    auto t = makeTransport();
    BulkOptions o = fastOptions();
    o.maxChunkBytes = 200; // 极小限制便于测试
    BulkWriter w(t, "articles", o, std::make_shared<ScriptedSleeper>(),
                 std::make_shared<ScriptedJitter>(1.0));

    json big = json{{"content", std::string(500, 'x')}};
    bool threw = false;
    try {
        w.write({big}, {"big-1"});
    } catch (const BulkValidationError& e) {
        threw = true;
        std::cout << "    rejected: " << e.what() << "\n";
    }
    check(threw, "oversized doc rejected");
    check(t->history.empty(), "no request sent");

    // 限制内的小文档不受影响
    auto r = w.write({docOf("small")}, {"small-1"});
    check(r.confirmedCount() == 1, "small doc accepted under same limit");
}

// 可控分块：按条目数与字节数拆分，跨块错误互不干扰 --------------------
void testChunkingByCountAndBytes() {
    std::cout << "[test] controllable chunking by count and by bytes\n";
    auto t = makeTransport();
    BulkOptions o = fastOptions();
    o.chunkSize = 2;
    BulkWriter w(t, "articles", o, std::make_shared<ScriptedSleeper>(),
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("a"), docOf("b"), docOf("c"), docOf("d"),
                      docOf("e")},
                     {"a", "b", "c", "d", "e"});
    check(r.chunkCount == 3, "5 docs with chunkSize 2 => 3 chunks");
    check(t->history.size() == 3, "3 physical requests");
    check(t->history[0].ids.size() == 2 && t->history[1].ids.size() == 2 &&
              t->history[2].ids.size() == 1,
          "chunk sizes 2/2/1");
    check(r.confirmedCount() == 5, "all confirmed");
    // 跨块回执仍是完整原序
    std::vector<std::string> got;
    for (const auto& it : r.items) got.push_back(it.id);
    check((got == std::vector<std::string>{"a", "b", "c", "d", "e"}),
          "receipt order intact across chunks");

    // 按字节拆块
    auto t2 = makeTransport();
    BulkOptions o2 = fastOptions();
    o2.chunkSize = 100;
    o2.maxChunkBytes = 90; // 每篇序列化后约 40~50 字节 => 每块至多 2 条
    BulkWriter w2(t2, "articles", o2, std::make_shared<ScriptedSleeper>(),
                  std::make_shared<ScriptedJitter>(1.0));
    auto r2 = w2.write({docOf("aa"), docOf("bb"), docOf("cc"), docOf("dd")},
                       {"a", "b", "c", "d"});
    check(r2.chunkCount >= 2, "byte limit forces multiple chunks");
    check(t2->storedDocCount() == 4, "all 4 stored");
}

// 综合事故现场：成功 + 限流重试成功 + 永久失败 + 断开重放 -------------
void testMixedIncidentScenario() {
    std::cout << "[test] mixed incident: ok / 429-then-ok / bad mapping / disconnect\n";
    auto t = makeTransport();
    // 第一个整块请求在响应前断开；之后：
    //   art-2 再有一次 429，art-3 始终 400（坏数据）
    t->failNextRequestsTransport(1);
    t->scriptItem("art-2", 429);
    t->persistItemStatus("art-3", 400);
    auto s = std::make_shared<ScriptedSleeper>();
    BulkWriter w(t, "articles", fastOptions(), s,
                 std::make_shared<ScriptedJitter>(1.0));

    auto r = w.write({docOf("one"), docOf("two"), docOf("three"), docOf("four")},
                     {"art-1", "art-2", "art-3", "art-4"});

    check(r.items[0].status == ItemStatus::RetriedSuccess,
          "art-1: written after disconnect replay");
    check(r.items[1].status == ItemStatus::RetriedSuccess,
          "art-2: written after disconnect + one 429");
    check(r.items[1].attempts == 3, "art-2 attempted 3 times");
    check(r.items[2].status == ItemStatus::FailedPermanent,
          "art-3: failed_permanent (mapping)");
    check(r.items[2].attempts == 2,
          "art-3 rode along until rejected (no extra retries of its own)");
    check(r.items[3].status == ItemStatus::RetriedSuccess,
          "art-4: written after replay");
    check(r.confirmedCount() == 3 && r.failedCount() == 1,
          "3 confirmed, 1 permanent failure");
    check(t->storedDocCount() == 3, "exactly 3 docs in store");

    // 只重放失败清单里的可处理项：art-3 再投仍是 400（永久），不会突然成功
    auto r2 = w.write({docOf("three")}, {"art-3"});
    check(r2.items[0].status == ItemStatus::FailedPermanent,
          "bad record stays permanently failed on resubmission");
    check(r2.items[0].attempts == 1, "no retries wasted on 400");
    check(t->storedDocCount() == 3, "doc count unchanged by bad-record replay");

    // 原批次完整再投：总数依旧
    auto r3 = w.write({docOf("one"), docOf("two"), docOf("three"), docOf("four")},
                      {"art-1", "art-2", "art-3", "art-4"});
    check(t->storedDocCount() == 3, "full resubmit keeps doc count at 3");
    check(r3.confirmedCount() == 3, "good records overwrite-confirmed again");
}

// 构造期参数校验 -------------------------------------------------------
void testOptionValidation() {
    std::cout << "[test] option validation\n";
    bool threw = false;
    try {
        BulkOptions o;
        o.maxAttempts = 0;
        BulkWriter w(makeTransport(), "i", o);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "maxAttempts=0 rejected");

    threw = false;
    try {
        BulkOptions o;
        o.chunkSize = 0;
        BulkWriter w(makeTransport(), "i", o);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "chunkSize=0 rejected");

    threw = false;
    try {
        BulkWriter w(nullptr, "i");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "null transport rejected");
}

} // namespace

int main() {
    std::cout << "=== BulkWriter test suite (fake ES, no real waiting) ===\n\n";
    testAllWrittenFirstTry();
    testItemLevel429RetriesOnlyUnconfirmed();
    testMappingErrorIsPermanent();
    testWholeChunk429ThenSuccess();
    testGatewayStatusesRetriable();
    testRetriesExhaustedWithBoundedBackoff();
    testBackoffCap();
    testTransportDisconnectReplayNoDuplicates();
    testResubmitSameBatchKeepsDocCount();
    testValidationBeforeSend();
    testOversizedSingleDocumentRejected();
    testChunkingByCountAndBytes();
    testMixedIncidentScenario();
    testOptionValidation();

    std::cout << "\n========================================\n";
    std::cout << "checks: " << g_checks
              << ", failures: " << g_failures << "\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "TESTS FAILED\n";
    return 1;
}

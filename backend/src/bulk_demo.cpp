// 运营回执演示：不依赖真实 Elasticsearch 与真实等待。
//
// 用脚本化的内存假 ES 复现最近的事故现场：
//   同一批中部分成功、部分 429、一条 mapping 坏数据、首个分块在响应前断开。
// 演示输出让值班人员：
//   1) 直接区分「已写入 / 重试后成功 / 不可重试」三类记录；
//   2) 看到每条的最终状态、尝试次数、HTTP 状态与精简错误原因；
//   3) 再次提交同一业务批次后，确认文档总数不变（稳定业务 ID 幂等覆盖）。

#include "bulk_test_support.hpp"

#include <iostream>
#include <iomanip>

using namespace es;

namespace Color {
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string CYAN = "\033[36m";
const std::string GRAY = "\033[90m";
const std::string BOLD = "\033[1m";
} // namespace Color

namespace {

json article(const std::string& title, const std::string& createdAt) {
    return json{{"title", title},
                {"content", "合作方供稿：" + title + "（正文略）"},
                {"author", "合作方"},
                {"category", "技术"},
                {"tags", {"合作", "导入"}},
                {"created_at", createdAt}};
}

std::string statusLabelZh(ItemStatus s) {
    switch (s) {
        case ItemStatus::Written: return "已写入";
        case ItemStatus::RetriedSuccess: return "重试后成功";
        case ItemStatus::FailedPermanent: return "不可重试";
        case ItemStatus::RetriesExhausted: return "重试耗尽(待再投)";
    }
    return "?";
}

std::string statusColor(ItemStatus s) {
    switch (s) {
        case ItemStatus::Written: return Color::GREEN;
        case ItemStatus::RetriedSuccess: return Color::YELLOW;
        case ItemStatus::FailedPermanent: return Color::RED;
        case ItemStatus::RetriesExhausted: return Color::RED;
    }
    return Color::RESET;
}

void printReceiptTable(const BulkReceipt& receipt) {
    std::cout << Color::BOLD
              << "  序号  业务ID         状态             尝试  HTTP  精简错误原因\n"
              << Color::RESET;
    std::cout << Color::GRAY
              << "  ----  -------------  ---------------  ----  ----  ----------------\n"
              << Color::RESET;
    for (const auto& it : receipt.items) {
        std::cout << "  " << std::left << std::setw(4) << (it.index + 1)
                  << "  " << std::setw(13) << it.id << "  "
                  << statusColor(it.status) << std::setw(15)
                  << statusLabelZh(it.status) << Color::RESET << "  "
                  << std::setw(4) << it.attempts << "  "
                  << std::setw(4) << (it.httpStatus == 0 ? std::string("断连")
                                                          : std::to_string(it.httpStatus))
                  << "  "
                  << (it.error.empty() ? std::string("-") : it.error)
                  << "\n";
    }
}

} // namespace

int main() {
    std::cout << Color::CYAN << Color::BOLD
              << "========================================\n"
              << "  合作方文章批量导入 · 逐项回执演示\n"
              << "  （退避时钟与抖动已替换为确定性实现，零真实等待）\n"
              << "========================================\n"
              << Color::RESET << "\n";

    // ---- 业务批次：6 篇文章，调用方提供稳定业务 ID ----
    std::vector<json> docs = {
        article("合作方文章-1", "2026-09-10"),
        article("合作方文章-2", "2026-09-11"),
        article("合作方文章-3-坏数据", "not-a-date"), // mapping 校验必失败
        article("合作方文章-4", "2026-09-12"),
        article("合作方文章-5", "2026-09-13"),
        article("合作方文章-6", "2026-09-14"),
    };
    std::vector<std::string> ids = {
        "partner-001", "partner-002", "partner-003",
        "partner-004", "partner-005", "partner-006"};

    std::cout << "业务批次共 " << docs.size()
              << " 篇文章，分块大小 3，退避上限 4 次/条。\n";
    std::cout << Color::GRAY
              << "事故注入：第 1 个分块响应前断开；partner-005 首次返回 429；"
                 "partner-003 持续 mapping 400。\n"
              << Color::RESET << "\n";

    auto transport = std::make_shared<FakeBulkTransport>();
    transport->failNextRequestsTransport(1); // 首批断连
    transport->scriptItem("partner-005", 429); // 该条首次被限流
    transport->persistItemStatus("partner-003", 400); // 坏数据恒 400

    BulkOptions options;
    options.chunkSize = 3;
    options.maxAttempts = 4;
    options.initialDelay = std::chrono::milliseconds(100);
    options.maxDelay = std::chrono::milliseconds(500);

    auto sleeper = std::make_shared<ScriptedSleeper>();
    BulkWriter writer(transport, "articles", options, sleeper,
                      std::make_shared<ScriptedJitter>(1.0));

    BulkReceipt receipt = writer.write(docs, ids);

    std::cout << Color::BOLD << "[第一次提交] 逐项回执（保持原输入顺序）\n"
              << Color::RESET;
    printReceiptTable(receipt);

    std::size_t written = receipt.count(ItemStatus::Written);
    std::size_t retried = receipt.count(ItemStatus::RetriedSuccess);
    std::size_t permanent = receipt.count(ItemStatus::FailedPermanent);
    std::size_t exhausted = receipt.count(ItemStatus::RetriesExhausted);
    std::cout << "\n  汇总："
              << Color::GREEN << "已写入 " << written << Color::RESET << "，"
              << Color::YELLOW << "重试后成功 " << retried << Color::RESET << "，"
              << Color::RED << "不可重试 " << permanent << Color::RESET;
    if (exhausted)
        std::cout << Color::RED << "，重试耗尽 " << exhausted << Color::RESET;
    std::cout << "\n  物理请求 " << receipt.requestCount << " 次 / "
              << receipt.chunkCount << " 个分块；退避 "
              << receipt.backoffCount << " 次（记录到的等待时长毫秒：";
    for (std::size_t i = 0; i < sleeper->sleepsMs.size(); ++i)
        std::cout << (i ? ", " : "") << sleeper->sleepsMs[i] << "ms";
    std::cout << "）\n";

    std::cout << Color::GRAY
              << "  值班处置：仅需关注失败清单 partner-003（mapping 永久错误），"
                 "修正 created_at 后以同一 ID 重投即可。\n"
              << Color::RESET;

    const std::size_t docsAfterFirst = transport->storedDocCount();
    std::cout << "\n  搜索库文档总数（第一次提交后）："
              << Color::BOLD << docsAfterFirst << Color::RESET << "\n";

    // ---- 值班人员（或定时任务）原样再提交同一业务批次 ----
    std::cout << Color::BOLD
              << "\n[第二次提交] 不改动任何 ID，整批原样再投（验证幂等）\n"
              << Color::RESET;
    BulkReceipt receipt2 = writer.write(docs, ids);
    printReceiptTable(receipt2);

    std::size_t docsAfterSecond = transport->storedDocCount();
    std::cout << "\n  搜索库文档总数（第二次提交后）："
              << Color::BOLD << docsAfterSecond << Color::RESET << "\n";

    bool countUnchanged = docsAfterFirst == docsAfterSecond;
    std::cout << "\n  " << (countUnchanged ? Color::GREEN : Color::RED)
              << (countUnchanged
                      ? "✓ 结论：再次提交同一业务批次，文档总数不变（稳定 _id 覆盖，无重复文章）。"
                      : "✗ 文档总数发生变化，幂等性被破坏！")
              << Color::RESET << "\n";

    std::cout << Color::BOLD << "\n三类记录判定：\n" << Color::RESET;
    std::cout << "  " << Color::GREEN << "[已写入]" << Color::RESET
              << "        attempts=1，HTTP 2xx\n";
    std::cout << "  " << Color::YELLOW << "[重试后成功]" << Color::RESET
              << "    attempts>1，最终 HTTP 2xx（本批的 429 与断连条目）\n";
    std::cout << "  " << Color::RED << "[不可重试]" << Color::RESET
              << "      mapping 等 4xx 永久错误，首次即定案、不消耗重试额度\n";

    return countUnchanged && receipt.failedCount() == 1 &&
                   receipt.confirmedCount() == 5
               ? 0
               : 1;
}

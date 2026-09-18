// 批量导入逐项回执演示（离线，无需真实 Elasticsearch）。
//
// 用脚本化的内存版 ES 复现资料运营的典型事故批次：同一批里既有一次写入的、
// 被 429 后重试成功的，也有 mapping 校验失败必须留下的坏记录，
// 还包含一次“响应到达前断连”。时钟与抖动全部注入，演示不发生真实等待。
#include "bulk_indexer.hpp"
#include "fake_es.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace es;
using json = nlohmann::json;
using fake::FakeBulkServer;

namespace {

const char* RESET = "\033[0m";
const char* RED = "\033[31m";
const char* GREEN = "\033[32m";
const char* YELLOW = "\033[33m";
const char* CYAN = "\033[36m";
const char* BOLD = "\033[1m";

void section(const std::string& title) {
    std::cout << "\n" << CYAN << BOLD << "── " << title << " ──" << RESET << "\n";
}

// 按终端显示宽度（CJK 及全角字符占 2 列）补齐
std::string padDisplay(const std::string& s, std::size_t width) {
    std::size_t w = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        if (i + len > s.size()) len = 1;
        unsigned int cp = c;
        if (len > 1) {
            cp = c & (0xFF >> (len + 1));
            for (std::size_t k = 1; k < len; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            }
        }
        bool wide = (cp >= 0x1100 && cp <= 0x115F) ||   // 韩文字母
                    (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
                    (cp >= 0xAC00 && cp <= 0xD7A3) ||   // 韩文音节
                    (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK 兼容表意
                    (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角符号
                    (cp >= 0xFFE0 && cp <= 0xFFE6);
        w += wide ? 2 : 1;
        i += len;
    }
    return w >= width ? s : s + std::string(width - w, ' ');
}

const char* statusColor(ItemStatus st) {
    switch (st) {
        case ItemStatus::INDEXED:             return GREEN;
        case ItemStatus::INDEXED_AFTER_RETRY: return YELLOW;
        case ItemStatus::PERMANENT_FAILURE:   return RED;
        case ItemStatus::RETRY_EXHAUSTED:     return RED;
        case ItemStatus::UNCONFIRMED:         return YELLOW;
    }
    return "";
}

void printReceipts(const BulkReport& report) {
    std::cout << "  " << BOLD
              << padDisplay("序号", 4) << " "
              << padDisplay("业务 ID", 10) << " "
              << padDisplay("最终状态", 14) << " "
              << padDisplay("尝试", 4) << " "
              << padDisplay("HTTP", 5) << " "
              << "ES 结果 / 精简错误原因" << RESET << "\n";

    for (const auto& r : report.items) {
        std::string attempts = std::to_string(r.attempts) + " 次";
        std::string http = r.httpStatus == 0 ? "断连" : std::to_string(r.httpStatus);

        std::cout << "  "
                  << padDisplay(std::to_string(r.index + 1), 4) << " "
                  << padDisplay(r.id, 10) << " "
                  << statusColor(r.status)
                  << padDisplay(toString(r.status), 14) << RESET << " "
                  << padDisplay(attempts, 4) << " "
                  << padDisplay(http, 5) << " ";

        if (r.written()) {
            std::cout << GREEN << r.esResult << "（version " << r.version << "）"
                      << RESET;
        } else {
            std::cout << RED;
            if (!r.errorType.empty()) std::cout << r.errorType << ": ";
            std::cout << (r.errorReason.empty() ? "(无错误详情)" : r.errorReason);
            std::cout << RESET;
        }
        std::cout << "\n";
    }
}

void printSummary(const BulkReport& report) {
    std::cout << "  " << GREEN << "已写入 " << report.indexedCount() << RESET
              << " ｜ " << YELLOW << "重试后写入 "
              << report.indexedAfterRetryCount() << RESET
              << " ｜ " << RED << "不可重试失败 "
              << report.permanentFailureCount() << RESET;
    if (report.retryExhaustedCount() || report.unconfirmedCount()) {
        std::cout << " ｜ " << RED << "重试耗尽 "
                  << report.retryExhaustedCount() << RESET
                  << " ｜ " << YELLOW << "结果未确认 "
                  << report.unconfirmedCount() << RESET;
    }
    std::cout << "\n  计划分块 " << report.chunksPlanned << " 个，实际发送 "
              << report.requestsSent << " 次；退避等待（模拟时钟）：";
    if (report.backoffWaits.empty()) {
        std::cout << "无";
    } else {
        for (std::size_t i = 0; i < report.backoffWaits.size(); ++i) {
            if (i) std::cout << "、";
            std::cout << report.backoffWaits[i].count() << "ms";
        }
    }
    std::cout << "\n";
}

json article(const std::string& title, const std::string& date) {
    return json{{"title", title},
                {"content", "合作方推送文章正文（示例）"},
                {"created_at", date}};
}

RetryPolicy deterministicClock() {
    RetryPolicy p;
    p.maxAttempts = 3;
    p.initialBackoff = std::chrono::milliseconds{200};
    p.maxBackoff = std::chrono::milliseconds{2000};
    p.multiplier = 2.0;
    p.sleeper = [](std::chrono::milliseconds d) {
        // 替身时钟：只记录、不睡眠，自动化场景无需真实等待
        std::cout << "    （模拟退避 " << d.count() << "ms，未真实等待）\n";
    };
    p.jitter = [] { return 1.0; }; // 抖动取满档，输出确定可复现
    return p;
}

} // namespace

int main() {
    std::cout << BOLD << CYAN
              << "========================================\n"
              << "  批量导入逐项回执演示（含 429/断连/坏记录）\n"
              << "========================================" << RESET << "\n";

    const std::string index = "partner_articles";
    FakeBulkServer server;

    // 事故脚本：
    //  art-1003 首次条目级 429（限流），第二次成功
    //  art-1004 恒为 400 mapping 校验失败（created_at 格式错误）
    //  art-1005 首次请求在响应到达前断连（请求未被处理），随分块重发成功
    server.setFault("art-1003", FakeBulkServer::Fault::ITEM_RATE_LIMITED, 1);
    server.setFault("art-1004", FakeBulkServer::Fault::ITEM_MAPPING_ERROR, 1);
    server.setFault("art-1005", FakeBulkServer::Fault::DISCONNECT_BEFORE_APPLY, 1);

    std::vector<json> batch = {
        article("城市早报：地铁新线开通", "2024-09-01"),
        article("科技周刊：国产芯片进展", "2024-09-02"),
        article("财经快讯：三季度数据发布", "2024-09-03"),
        article("格式损坏的合作方来稿", "2024/13/40"),   // 日期格式非法
        article("生活频道：秋日赏叶指南", "2024-09-05"),
        article("体育资讯：联赛赛程更新", "2024-09-06"),
    };
    std::vector<std::string> businessIds = {
        "art-1001", "art-1002", "art-1003",
        "art-1004", "art-1005", "art-1006"};

    section("场景一：一批 6 篇文章，分块大小 2，故障混发");
    BulkOptions options{2, 5 * 1024 * 1024};
    BulkIndexer indexer(server, index, options, deterministicClock());

    BulkReport report = indexer.run(batch, businessIds);

    std::cout << "\n逐项回执（顺序与原输入完全一致）：\n\n";
    printReceipts(report);
    std::cout << "\n汇总：\n";
    printSummary(report);

    std::cout << "\n值班处置建议：\n";
    std::cout << "  • " << GREEN << "已写入 / 重试后写入" << RESET
              << "：无需处理，回执给出最终 HTTP 与 ES 版本号。\n";
    std::cout << "  • " << RED << "不可重试失败（art-1004）" << RESET
              << "：只发送 1 次即留在失败清单，请合作方修正 mapping 数据后"
                 "以同一业务 ID 单独重提。\n";

    section("场景二：再次提交同一业务批次，验证不产生重复文档");
    std::cout << "  重放前索引文档总数：" << server.docCount() << "\n";
    BulkReport replay = indexer.run(batch, businessIds);
    std::cout << "  重放后索引文档总数：" << server.docCount()
              << "（art-1004 仍为永久失败，其余同 _id 幂等覆盖）\n";
    std::cout << "  重放回执：写入 " << replay.writtenCount() << "，不可重试失败 "
              << replay.permanentFailureCount()
              << "；5 篇已存在文档 result=updated、version 递增，无第二份文章。\n";

    section("场景三：响应到达前断连且不允许自动重试 => 结果未确认可安全重发");
    FakeBulkServer server2;
    // 请求已落库，但响应在途中丢失
    server2.setFault("art-2001", FakeBulkServer::Fault::DISCONNECT_AFTER_APPLY, 1);
    RetryPolicy once = deterministicClock();
    once.maxAttempts = 1;
    BulkIndexer onceIndexer(server2, index, BulkOptions{}, once);
    BulkReport r1 = onceIndexer.run({article("唯一一篇来稿", "2024-09-10")},
                                   {"art-2001"});
    std::cout << "  首次提交回执：" << statusColor(r1.items[0].status)
              << toString(r1.items[0].status) << RESET
              << "（HTTP 显示为“断连”，服务端是否落库未知）\n";
    std::cout << "  此时服务端文档数：" << server2.docCount() << "（实际已落库 1 份）\n";

    BulkIndexer replayIndexer(server2, index, BulkOptions{}, deterministicClock());
    BulkReport r2 = replayIndexer.run({article("唯一一篇来稿", "2024-09-10")},
                                      {"art-2001"});
    std::cout << "  凭相同业务 ID 重发后：" << statusColor(r2.items[0].status)
              << toString(r2.items[0].status) << RESET
              << "，服务端文档数仍为 " << server2.docCount()
              << "（同 _id 覆盖，version="
              << server2.versionOf(index, "art-2001") << "）。\n";

    section("场景四：发送前校验（任何 HTTP 请求都不会发出）");
    auto reject = [&](const char* label, const std::vector<json>& d,
                      const std::vector<std::string>& idsV,
                      const BulkOptions& opt = BulkOptions{}) {
        try {
            BulkIndexer v(server, index, opt, deterministicClock());
            v.run(d, idsV);
            std::cout << "  ✗ " << label << "：未被拒绝（异常！）\n";
        } catch (const BulkValidationError& e) {
            std::cout << "  ✓ " << label << "：" << e.what() << "\n";
        }
    };
    const int before = server.requestCount;
    reject("空批次", {}, {});
    reject("ID 数量不匹配",
           {article("a", "2024-09-01"), article("b", "2024-09-01")},
           {"art-1"});
    reject("单条超过分块字节上限",
           {json{{"content", std::string(2048, 'x')}}}, {"art-big"},
           BulkOptions{500, 1024});
    std::cout << "  校验期间新增 HTTP 请求数：" << server.requestCount - before
              << "（全部在发送前被明确拒绝）\n";

    std::cout << "\n" << BOLD << CYAN
              << "========================================\n"
              << "  演示完成\n"
              << "========================================" << RESET << "\n";
    return 0;
}

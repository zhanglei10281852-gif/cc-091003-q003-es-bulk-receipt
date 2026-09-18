#include "bulk_indexer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace es {

// ==================== 状态标签与汇总 ====================

std::string toString(ItemStatus status) {
    switch (status) {
        case ItemStatus::INDEXED:             return "已写入";
        case ItemStatus::INDEXED_AFTER_RETRY: return "重试后写入";
        case ItemStatus::PERMANENT_FAILURE:   return "不可重试失败";
        case ItemStatus::RETRY_EXHAUSTED:     return "重试耗尽";
        case ItemStatus::UNCONFIRMED:         return "结果未确认";
    }
    return "未知";
}

bool isRetryableStatus(int httpStatus) {
    return httpStatus == 429 || httpStatus == 502 ||
           httpStatus == 503 || httpStatus == 504;
}

std::size_t BulkReport::indexedCount() const {
    return std::count_if(items.begin(), items.end(), [](const ItemReceipt& r) {
        return r.status == ItemStatus::INDEXED;
    });
}

std::size_t BulkReport::indexedAfterRetryCount() const {
    return std::count_if(items.begin(), items.end(), [](const ItemReceipt& r) {
        return r.status == ItemStatus::INDEXED_AFTER_RETRY;
    });
}

std::size_t BulkReport::permanentFailureCount() const {
    return std::count_if(items.begin(), items.end(), [](const ItemReceipt& r) {
        return r.status == ItemStatus::PERMANENT_FAILURE;
    });
}

std::size_t BulkReport::retryExhaustedCount() const {
    return std::count_if(items.begin(), items.end(), [](const ItemReceipt& r) {
        return r.status == ItemStatus::RETRY_EXHAUSTED;
    });
}

std::size_t BulkReport::unconfirmedCount() const {
    return std::count_if(items.begin(), items.end(), [](const ItemReceipt& r) {
        return r.status == ItemStatus::UNCONFIRMED;
    });
}

std::size_t BulkReport::writtenCount() const {
    return std::count_if(items.begin(), items.end(), [](const ItemReceipt& r) {
        return r.written();
    });
}

std::size_t BulkReport::failedCount() const {
    return items.size() - writtenCount();
}

// ==================== 辅助函数 ====================

namespace {

constexpr std::size_t kMaxReasonLen = 200;

/// 将 ES 错误原因压缩为单行、定长，适合直接进入值班回执
std::string compactReason(std::string reason) {
    reason.erase(reason.begin(),
                 std::find_if_not(reason.begin(), reason.end(),
                                  [](unsigned char c) { return std::isspace(c); }));
    reason.erase(std::find_if_not(reason.rbegin(), reason.rend(),
                                  [](unsigned char c) { return std::isspace(c); }).base(),
                 reason.end());
    for (char& c : reason) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string collapsed;
    bool lastSpace = false;
    for (char c : reason) {
        if (c == ' ') {
            if (!lastSpace) collapsed.push_back(c);
            lastSpace = true;
        } else {
            collapsed.push_back(c);
            lastSpace = false;
        }
    }
    if (collapsed.size() > kMaxReasonLen) {
        collapsed.resize(kMaxReasonLen);
        collapsed += "...";
    }
    return collapsed;
}

/// 解析整条 _bulk 请求被拒时 ES 返回的 {"error": {...}} 结构
std::pair<std::string, std::string> parseTopLevelError(const std::string& body) {
    try {
        auto parsed = json::parse(body);
        const auto& err = parsed.value("error", json());
        if (err.is_string()) {
            return {"", compactReason(err.get<std::string>())};
        }
        if (err.is_object()) {
            std::string type = err.value("type", "");
            std::string reason = err.value("reason", "");
            if (reason.empty() && err.contains("caused_by")) {
                reason = err["caused_by"].value("reason", "");
            }
            return {type, compactReason(reason)};
        }
    } catch (const json::exception&) {
        // body 不是 JSON，退回 HTTP 层面的描述
    }
    return {};
}

double defaultJitter() {
    static thread_local std::mt19937 gen(
        static_cast<std::uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    return std::uniform_real_distribution<double>(0.0, 1.0)(gen);
}

} // namespace

// ==================== BulkIndexer ====================

BulkIndexer::BulkIndexer(BulkTransport& transport,
                         std::string indexName,
                         BulkOptions options,
                         RetryPolicy retryPolicy)
    : transport_(transport),
      indexName_(std::move(indexName)),
      options_(options),
      retry_(std::move(retryPolicy)) {
    if (options_.chunkSize == 0) {
        throw BulkValidationError("分块条数下限为 1");
    }
    if (options_.maxChunkBytes == 0) {
        throw BulkValidationError("分块字节上限必须大于 0");
    }
    if (retry_.maxAttempts < 1) {
        throw BulkValidationError("最大尝试次数必须 >= 1");
    }
}

std::chrono::milliseconds BulkIndexer::backoffDelay(int failedSends) const {
    if (failedSends <= 0) return std::chrono::milliseconds{0};

    const double initialMs =
        static_cast<double>(retry_.initialBackoff.count());
    double rawMs = initialMs * std::pow(retry_.multiplier, failedSends - 1);
    double cappedMs = std::min(rawMs,
                               static_cast<double>(retry_.maxBackoff.count()));

    const double factor = retry_.jitter ? retry_.jitter() : defaultJitter();
    return std::chrono::milliseconds{
        static_cast<std::int64_t>(cappedMs * std::min(1.0, std::max(0.0, factor)))};
}

std::vector<std::vector<std::size_t>> BulkIndexer::planChunks(
    const std::vector<PreparedItem>& prepared) const {
    std::vector<std::vector<std::size_t>> chunks;
    std::size_t currentBytes = 0;

    for (std::size_t i = 0; i < prepared.size(); ++i) {
        bool needNewChunk = chunks.empty() ||
                            chunks.back().size() >= options_.chunkSize ||
                            currentBytes + prepared[i].bytes > options_.maxChunkBytes;
        if (needNewChunk) {
            chunks.emplace_back();
            currentBytes = 0;
        }
        chunks.back().push_back(i);
        currentBytes += prepared[i].bytes;
    }
    return chunks;
}

std::string BulkIndexer::buildBody(const std::vector<PreparedItem>& prepared,
                                   const std::vector<std::size_t>& positions) const {
    std::string body;
    for (std::size_t pos : positions) {
        body += prepared[pos].actionLine;
        body.push_back('\n');
        body += prepared[pos].sourceLine;
        body.push_back('\n');
    }
    return body;
}

BulkReport BulkIndexer::run(const std::vector<json>& docs,
                            const std::vector<std::string>& ids) {
    // ---------- 发送前校验（任何 HTTP 请求发出之前） ----------
    if (docs.empty()) {
        throw BulkValidationError("空批次：至少需要一条文档");
    }
    if (docs.size() != ids.size()) {
        std::ostringstream oss;
        oss << "ID 数量不匹配：" << docs.size() << " 条文档对应 "
            << ids.size() << " 个业务 ID，要求一一对应";
        throw BulkValidationError(oss.str());
    }

    std::vector<PreparedItem> prepared;
    prepared.reserve(docs.size());
    std::unordered_set<std::string> seenIds;
    seenIds.reserve(ids.size());

    for (std::size_t i = 0; i < docs.size(); ++i) {
        if (!docs[i].is_object()) {
            throw BulkValidationError(
                "第 " + std::to_string(i) + " 条记录不是 JSON 对象");
        }
        if (ids[i].empty()) {
            throw BulkValidationError(
                "第 " + std::to_string(i) + " 条记录缺少稳定业务 ID");
        }
        if (!seenIds.insert(ids[i]).second) {
            throw BulkValidationError(
                "业务 ID 在批次内重复：" + ids[i]);
        }

        PreparedItem item;
        item.pos = i;
        item.actionLine =
            json{{"index", {{"_index", indexName_}, {"_id", ids[i]}}}}.dump();
        item.sourceLine = docs[i].dump();
        // 动作行 + '\n' + 源文档行 + '\n'
        item.bytes = item.actionLine.size() + 1 +
                     item.sourceLine.size() + 1;

        if (item.bytes > options_.maxChunkBytes) {
            std::ostringstream oss;
            oss << "业务 ID " << ids[i] << " 单条数据 " << item.bytes
                << " 字节，超过分块上限 " << options_.maxChunkBytes
                << " 字节，拒绝发送";
            throw BulkValidationError(oss.str());
        }
        prepared.push_back(std::move(item));
    }

    // ---------- 分块 ----------
    auto chunks = planChunks(prepared);

    BulkReport report;
    report.items.resize(docs.size());
    report.chunksPlanned = static_cast<int>(chunks.size());

    for (std::size_t c = 0; c < chunks.size(); ++c) {
        for (std::size_t pos : chunks[c]) {
            report.items[pos].index = pos;
            report.items[pos].id = ids[pos];
            report.items[pos].chunkSeq = c;
        }
        sendChunk(prepared, chunks[c], report);
    }
    return report;
}

void BulkIndexer::sendChunk(const std::vector<PreparedItem>& prepared,
                            const std::vector<std::size_t>& initialChunk,
                            BulkReport& report) {
    std::vector<std::size_t> pending = initialChunk;
    int sendsFailed = 0;  // 当前分块累计失败发送次数，用于指数退避档位

    while (!pending.empty()) {
        const std::string body = buildBody(prepared, pending);

        // 每条待确认记录的尝试次数 +1
        for (std::size_t pos : pending) {
            ++report.items[pos].attempts;
        }
        ++report.requestsSent;

        HttpResponse response;
        bool transportFailed = false;
        std::string transportMessage;
        try {
            response = transport_.post("/_bulk", body);
        } catch (const TransportError& e) {
            transportFailed = true;
            transportMessage = e.what();
        }

        // ---------- 情况 A：响应到达前断连/超时，整批状态未知 ----------
        if (transportFailed) {
            ++sendsFailed;
            std::vector<std::size_t> retryable;
            for (std::size_t pos : pending) {
                if (report.items[pos].attempts < retry_.maxAttempts) {
                    retryable.push_back(pos);
                } else {
                    auto& r = report.items[pos];
                    r.status = ItemStatus::UNCONFIRMED;
                    r.httpStatus = 0;
                    r.errorReason = compactReason(
                        "响应到达前连接断开（" + transportMessage +
                        "），是否落库未知，可凭相同业务 ID 重发");
                }
            }
            pending = std::move(retryable);
            if (pending.empty()) return;
            auto wait = backoffDelay(sendsFailed);
            report.backoffWaits.push_back(wait);
            if (retry_.sleeper) retry_.sleeper(wait);
            else std::this_thread::sleep_for(wait);
            continue;
        }

        // ---------- 情况 B：整批被限流/网关临时错误，整批可重试 ----------
        if (isRetryableStatus(response.statusCode)) {
            ++sendsFailed;
            auto [errType, errReason] = parseTopLevelError(response.body);
            if (errReason.empty()) {
                errReason = "整批返回 HTTP " +
                            std::to_string(response.statusCode) + "，服务暂不可用";
            }
            std::vector<std::size_t> retryable;
            for (std::size_t pos : pending) {
                auto& r = report.items[pos];
                r.httpStatus = response.statusCode;
                r.errorType = errType;
                r.errorReason = errReason;
                if (r.attempts < retry_.maxAttempts) {
                    retryable.push_back(pos);
                } else {
                    r.status = ItemStatus::RETRY_EXHAUSTED;
                }
            }
            pending = std::move(retryable);
            if (pending.empty()) return;
            auto wait = backoffDelay(sendsFailed);
            report.backoffWaits.push_back(wait);
            if (retry_.sleeper) retry_.sleeper(wait);
            else std::this_thread::sleep_for(wait);
            continue;
        }

        // ---------- 情况 C：其他非 2xx（如 400/401/404），整批永久失败 ----------
        if (!response.isSuccess()) {
            auto [errType, errReason] = parseTopLevelError(response.body);
            if (errReason.empty()) {
                errReason = "整批返回不可重试的 HTTP " +
                            std::to_string(response.statusCode);
            }
            for (std::size_t pos : pending) {
                auto& r = report.items[pos];
                r.status = ItemStatus::PERMANENT_FAILURE;
                r.httpStatus = response.statusCode;
                r.errorType = errType;
                r.errorReason = errReason;
            }
            return;
        }

        // ---------- 情况 D：HTTP 200，逐项核对 items ----------
        json parsed;
        json items;
        bool parseOk = true;
        try {
            parsed = json::parse(response.body);
            items = parsed.at("items");
            if (!items.is_array() || items.size() != pending.size()) {
                parseOk = false;
            }
        } catch (const json::exception&) {
            parseOk = false;
        }

        // 200 但回执无法与请求对应：落库状态未知，按可重试处理
        if (!parseOk) {
            ++sendsFailed;
            std::vector<std::size_t> retryable;
            for (std::size_t pos : pending) {
                auto& r = report.items[pos];
                r.httpStatus = response.statusCode;
                r.errorReason = compactReason(
                    "HTTP 200 响应无法解析或 items 数量不符，落库状态未知");
                if (r.attempts < retry_.maxAttempts) {
                    retryable.push_back(pos);
                } else {
                    r.status = ItemStatus::UNCONFIRMED;
                }
            }
            pending = std::move(retryable);
            if (pending.empty()) return;
            auto wait = backoffDelay(sendsFailed);
            report.backoffWaits.push_back(wait);
            if (retry_.sleeper) retry_.sleeper(wait);
            else std::this_thread::sleep_for(wait);
            continue;
        }

        std::vector<std::size_t> retryable;
        for (std::size_t k = 0; k < pending.size(); ++k) {
            auto& r = report.items[pending[k]];

            // 单条回执缺少 "index" 节属于协议异常：该条落库状态未知，
            // 按可重试处理而不是让异常逃出 run()
            if (!items[k].is_object() || !items[k].contains("index") ||
                !items[k]["index"].is_object()) {
                r.httpStatus = response.statusCode;
                r.errorReason = compactReason("回执缺少 index 节，落库状态未知");
                if (r.attempts < retry_.maxAttempts) {
                    retryable.push_back(pending[k]);
                } else {
                    r.status = ItemStatus::UNCONFIRMED;
                }
                continue;
            }

            const auto& idx = items[k]["index"];
            const int itemStatus = idx.value("status", 0);
            r.httpStatus = itemStatus;
            r.esResult = idx.value("result", "");
            r.version = static_cast<std::int64_t>(idx.value("_version",
                                                             static_cast<int64_t>(0)));

            if (itemStatus >= 200 && itemStatus < 300) {
                r.status = r.attempts == 1 ? ItemStatus::INDEXED
                                           : ItemStatus::INDEXED_AFTER_RETRY;
                r.errorType.clear();
                r.errorReason.clear();
                continue;
            }

            // 单条失败：提取 ES 的 error.type / error.reason
            if (idx.contains("error") && idx["error"].is_object()) {
                const auto& err = idx["error"];
                r.errorType = err.value("type", "");
                std::string reason = err.value("reason", "");
                if (reason.empty() && err.contains("caused_by")) {
                    reason = err["caused_by"].value("reason", "");
                }
                r.errorReason = compactReason(reason);
            } else {
                r.errorReason = "条目返回 HTTP " + std::to_string(itemStatus);
            }

            if (isRetryableStatus(itemStatus)) {
                if (r.attempts < retry_.maxAttempts) {
                    retryable.push_back(pending[k]);
                } else {
                    r.status = ItemStatus::RETRY_EXHAUSTED;
                }
            } else {
                // mapping 校验等永久错误：立即留在失败清单，不再重试
                r.status = ItemStatus::PERMANENT_FAILURE;
            }
        }

        if (retryable.empty()) return;

        // 仅重发尚未确认的条目；永久失败条目不再包含在请求中
        ++sendsFailed;
        pending = std::move(retryable);
        auto wait = backoffDelay(sendsFailed);
        report.backoffWaits.push_back(wait);
        if (retry_.sleeper) retry_.sleeper(wait);
        else std::this_thread::sleep_for(wait);
    }
}

} // namespace es

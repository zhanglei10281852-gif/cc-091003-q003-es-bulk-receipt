#include "bulk_writer.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace es {

namespace {

constexpr std::size_t kMaxErrorLen = 240;

std::string shorten(std::string text) {
    // 压成单行，避免错误原因冲垮回执展示
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    if (text.size() > kMaxErrorLen) {
        text.resize(kMaxErrorLen);
        text += "...";
    }
    return text;
}

/**
 * 从 ES 错误体中提炼「type: reason」级别的简短原因。
 * 兼容顶层 error 对象、root_cause 数组以及 error 为纯字符串三种形态。
 */
std::string extractError(const json& body, const std::string& fallback) {
    if (body.is_object()) {
        if (body.contains("error")) {
            const json& err = body["error"];
            if (err.is_string()) {
                return shorten(err.get<std::string>());
            }
            if (err.is_object()) {
                std::string type = err.value("type", "");
                std::string reason = err.value("reason", "");
                if (reason.empty() && err.contains("root_cause") &&
                    err["root_cause"].is_array() && !err["root_cause"].empty()) {
                    const json& rc = err["root_cause"][0];
                    if (rc.is_object()) {
                        if (type.empty()) type = rc.value("type", "");
                        reason = rc.value("reason", "");
                    }
                }
                std::ostringstream oss;
                if (!type.empty()) oss << type;
                if (!reason.empty()) {
                    if (!type.empty()) oss << ": ";
                    oss << reason;
                }
                std::string text = oss.str();
                if (!text.empty()) return shorten(text);
            }
        }
        if (body.contains("message") && body["message"].is_string()) {
            return shorten(body["message"].get<std::string>());
        }
    }
    return shorten(fallback);
}

std::string itemError(const json& actionResult) {
    if (actionResult.is_object() && actionResult.contains("error")) {
        std::ostringstream fallback;
        fallback << "HTTP " << actionResult.value("status", 0);
        return extractError(json{{"error", actionResult["error"]}}, fallback.str());
    }
    std::ostringstream oss;
    oss << "HTTP " << actionResult.value("status", 0);
    return oss.str();
}

std::string actionLine(const std::string& indexName,
                       const std::string& id) {
    json action = {{"index", {{"_index", indexName}, {"_id", id}}}};
    return action.dump();
}

class RealSleeper final : public ISleeper {
public:
    void sleepFor(std::chrono::milliseconds duration) override {
        std::this_thread::sleep_for(duration);
    }
};

class RandomJitter final : public IJitter {
public:
    RandomJitter() : gen_(std::random_device{}()), dist_(0.0, 1.0) {}
    double next() override { return dist_(gen_); }

private:
    std::mt19937 gen_;
    std::uniform_real_distribution<double> dist_;
};

} // namespace

// ==================== BulkReceipt 统计 ====================

std::size_t BulkReceipt::count(ItemStatus s) const {
    return static_cast<std::size_t>(
        std::count_if(items.begin(), items.end(),
                      [s](const ItemReceipt& r) { return r.status == s; }));
}

std::size_t BulkReceipt::confirmedCount() const {
    return count(ItemStatus::Written) + count(ItemStatus::RetriedSuccess);
}

std::size_t BulkReceipt::failedCount() const {
    return count(ItemStatus::FailedPermanent) + count(ItemStatus::RetriesExhausted);
}

// ==================== BulkWriter ====================

BulkWriter::BulkWriter(std::shared_ptr<IBulkTransport> transport,
                       std::string indexName,
                       BulkOptions options,
                       std::shared_ptr<ISleeper> sleeper,
                       std::shared_ptr<IJitter> jitter)
    : transport_(std::move(transport)),
      indexName_(std::move(indexName)),
      options_(options),
      sleeper_(std::move(sleeper)),
      jitter_(std::move(jitter)) {
    if (!transport_) {
        throw std::invalid_argument("BulkWriter requires a non-null transport");
    }
    if (options_.chunkSize == 0) {
        throw std::invalid_argument("chunkSize must be greater than 0");
    }
    if (options_.maxAttempts < 1) {
        throw std::invalid_argument("maxAttempts must be greater than 0");
    }
    if (!sleeper_) sleeper_ = std::make_shared<RealSleeper>();
    if (!jitter_) jitter_ = std::make_shared<RandomJitter>();
}

bool BulkWriter::isRetriableStatus(int statusCode) noexcept {
    return statusCode == 429 || statusCode == 502 ||
           statusCode == 503 || statusCode == 504;
}

const char* BulkWriter::statusName(ItemStatus status) noexcept {
    switch (status) {
        case ItemStatus::Written:          return "written";
        case ItemStatus::RetriedSuccess:   return "retried_success";
        case ItemStatus::FailedPermanent:  return "failed_permanent";
        case ItemStatus::RetriesExhausted: return "retries_exhausted";
    }
    return "unknown";
}

std::chrono::milliseconds BulkWriter::backoffDelay(int round) const {
    // 指数退避 + 全抖动：delay = random(0, min(maxDelay, base * 2^round))
    double baseMs = static_cast<double>(options_.initialDelay.count()) *
                    std::pow(options_.backoffMultiplier, round);
    double capMs = static_cast<double>(options_.maxDelay.count());
    double ceiling = std::min(baseMs, capMs);
    auto ms = static_cast<long long>(jitter_->next() * ceiling);
    return std::chrono::milliseconds(ms);
}

BulkReceipt BulkWriter::write(const std::vector<json>& docs,
                              const std::vector<std::string>& ids) {
    // ---------- 发送前校验：任何一条不满足都在发请求前明确拒绝 ----------
    if (docs.empty()) {
        throw BulkValidationError("bulk write rejected: batch is empty");
    }
    if (ids.size() != docs.size()) {
        std::ostringstream oss;
        oss << "bulk write rejected: id count (" << ids.size()
            << ") does not match document count (" << docs.size() << ")";
        throw BulkValidationError(oss.str());
    }
    std::set<std::string> uniqueIds;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i].empty()) {
            throw BulkValidationError(
                "bulk write rejected: empty business id at index " +
                std::to_string(i));
        }
        if (!uniqueIds.insert(ids[i]).second) {
            throw BulkValidationError(
                "bulk write rejected: duplicate business id '" + ids[i] +
                "' at index " + std::to_string(i));
        }
    }

    // 预算每条记录的 NDJSON 字节数，单条超限或无法序列化在发送前拒绝
    std::vector<std::string> docLines(docs.size());
    std::vector<std::size_t> fragmentBytes(docs.size());
    for (std::size_t i = 0; i < docs.size(); ++i) {
        if (!docs[i].is_object()) {
            throw BulkValidationError(
                "bulk write rejected: document at index " +
                std::to_string(i) + " is not a JSON object");
        }
        try {
            docLines[i] = docs[i].dump();
        } catch (const std::exception& e) {
            throw BulkValidationError(
                "bulk write rejected: document at index " +
                std::to_string(i) + " could not be serialized (" +
                e.what() + ")");
        }
        fragmentBytes[i] = actionLine(indexName_, ids[i]).size() + 1 +
                           docLines[i].size() + 1;
        if (fragmentBytes[i] > options_.maxChunkBytes) {
            std::ostringstream oss;
            oss << "bulk write rejected: document at index " << i
                << " (id=" << ids[i] << ") serializes to " << fragmentBytes[i]
                << " bytes, exceeding per-chunk limit "
                << options_.maxChunkBytes << " bytes";
            throw BulkValidationError(oss.str());
        }
    }

    // ---------- 分块：同时受 chunkSize 与 maxChunkBytes 约束 ----------
    struct Chunk { std::vector<std::size_t> order; };
    std::vector<Chunk> chunks;
    {
        Chunk current;
        std::size_t currentBytes = 0;
        for (std::size_t i = 0; i < docs.size(); ++i) {
            bool fullByCount = current.order.size() >= options_.chunkSize;
            bool fullByBytes = !current.order.empty() &&
                               currentBytes + fragmentBytes[i] > options_.maxChunkBytes;
            if (fullByCount || fullByBytes) {
                chunks.push_back(std::move(current));
                current = Chunk{};
                currentBytes = 0;
            }
            current.order.push_back(i);
            currentBytes += fragmentBytes[i];
        }
        if (!current.order.empty()) chunks.push_back(std::move(current));
    }

    BulkReceipt receipt;
    receipt.items.resize(docs.size());
    for (std::size_t i = 0; i < docs.size(); ++i) {
        receipt.items[i].index = i;
        receipt.items[i].id = ids[i];
    }

    const std::string path = "/" + indexName_ + "/_bulk";

    // ---------- 逐块处理：只重试尚未确认的条目 ----------
    for (const Chunk& chunk : chunks) {
        std::vector<std::size_t> pending = chunk.order;
        int round = 0;

        while (!pending.empty()) {
            // 组装当前未确认条目的 NDJSON（保持原始顺序）
            std::ostringstream ndjson;
            for (std::size_t idx : pending) {
                ndjson << actionLine(indexName_, ids[idx]) << "\n"
                       << docLines[idx] << "\n";
            }
            std::string body = ndjson.str();
            ++receipt.requestCount;
            for (std::size_t idx : pending) {
                ++receipt.items[idx].attempts;
            }

            // 发送：传输层失败（响应到达前断开）与收到响应分开处理
            BulkTransportResponse resp;
            bool transportFailed = false;
            std::string transportMessage;
            try {
                resp = transport_->sendBulk(path, body);
            } catch (const TransientTransportError& e) {
                transportFailed = true;
                transportMessage = e.what();
            }

            std::vector<std::size_t> nextPending;

            if (transportFailed) {
                // 结果未知：全部视为未确认，稍后凭稳定 _id 安全重放
                for (std::size_t idx : pending) {
                    ItemReceipt& r = receipt.items[idx];
                    r.httpStatus = 0;
                    r.error = shorten(transportMessage);
                }
                nextPending = pending;
            } else if (resp.statusCode == 200) {
                // bulk API 恒为 200，逐项成败看 items[].index.status
                json parsed;
                bool parsedOk = false;
                if (!resp.body.empty()) {
                    try {
                        parsed = json::parse(resp.body);
                        parsedOk = parsed.is_object() &&
                                   parsed.contains("items") &&
                                   parsed["items"].is_array() &&
                                   parsed["items"].size() == pending.size();
                    } catch (const json::parse_error&) {
                        parsedOk = false;
                    }
                }
                if (!parsedOk) {
                    // 响应体无法对齐到请求条目：整体结果未知，按瞬时失败重试
                    for (std::size_t idx : pending) {
                        ItemReceipt& r = receipt.items[idx];
                        r.httpStatus = 200;
                        r.error = "unparseable or misaligned bulk response";
                    }
                    nextPending = pending;
                } else {
                    const json& items = parsed["items"];
                    for (std::size_t k = 0; k < pending.size(); ++k) {
                        std::size_t idx = pending[k];
                        ItemReceipt& r = receipt.items[idx];
                        const json& entry = items[k];
                        const json& ir = entry.contains("index") ? entry["index"]
                                                                 : entry;
                        int itemStatus = ir.value("status", 0);
                        r.httpStatus = itemStatus;
                        if (itemStatus >= 200 && itemStatus < 300) {
                            r.status = (r.attempts == 1)
                                           ? ItemStatus::Written
                                           : ItemStatus::RetriedSuccess;
                            r.esResult = ir.value("result", "");
                            r.error.clear();
                        } else if (isRetriableStatus(itemStatus)) {
                            r.error = itemError(ir);
                            nextPending.push_back(idx);
                        } else {
                            // mapping 校验等永久错误：立即定案，不再重试
                            r.status = ItemStatus::FailedPermanent;
                            r.error = itemError(ir);
                        }
                    }
                }
            } else if (isRetriableStatus(resp.statusCode)) {
                // 整块被限流/网关抖动拒绝（通常没有 items）
                std::string reason = extractError(
                    json::parse(resp.body, nullptr, false), // 允许解析失败
                    resp.body.empty()
                        ? ("HTTP " + std::to_string(resp.statusCode))
                        : resp.body);
                for (std::size_t idx : pending) {
                    ItemReceipt& r = receipt.items[idx];
                    r.httpStatus = resp.statusCode;
                    r.error = reason;
                    nextPending.push_back(idx);
                }
            } else {
                // 整块永久性失败（如 400 请求体非法、401 未授权等）
                json parsed = json::parse(resp.body, nullptr, false);
                std::string reason = extractError(
                    parsed, "HTTP " + std::to_string(resp.statusCode));
                for (std::size_t idx : pending) {
                    ItemReceipt& r = receipt.items[idx];
                    r.status = ItemStatus::FailedPermanent;
                    r.httpStatus = resp.statusCode;
                    r.error = reason;
                }
            }

            // 达到尝试上限的条目定案为「可重试但已耗尽」
            for (std::size_t idx : nextPending) {
                if (receipt.items[idx].attempts >= options_.maxAttempts) {
                    receipt.items[idx].status = ItemStatus::RetriesExhausted;
                }
            }
            nextPending.erase(
                std::remove_if(nextPending.begin(), nextPending.end(),
                               [&](std::size_t idx) {
                                   return receipt.items[idx].attempts >=
                                          options_.maxAttempts;
                               }),
                nextPending.end());

            pending.swap(nextPending);
            if (pending.empty()) break;

            // 有界退避（时钟与抖动均可替换，自动化测试无需真实等待）
            sleeper_->sleepFor(backoffDelay(round));
            ++receipt.backoffCount;
            ++round;
        }
    }

    receipt.chunkCount = chunks.size();
    return receipt;
}

} // namespace es

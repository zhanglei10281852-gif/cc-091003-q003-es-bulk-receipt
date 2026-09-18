#ifndef FAKE_BULK_SERVER_HPP
#define FAKE_BULK_SERVER_HPP

#include "bulk_indexer.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace es {
namespace fake {

/**
 * 脚本化的内存版 Elasticsearch _bulk 端点，仅供演示与单元测试。
 *
 * - 按 (index, _id) 幂等落地：重复 index 同一 _id 只更新版本，不新增文档，
 *   因此可以直接断言“重放同一业务批次不会改变文档总数”；
 * - 可按业务 ID 注入：条目级 429（前 N 次）、条目级 400（永久）、
 *   响应到达前断连（落库 / 未落库两种语义）；
 *   也可让前 N 个整请求直接返回 429。
 */
class FakeBulkServer : public BulkTransport {
public:
    enum class Fault {
        OK,                          ///< 总是成功
        ITEM_RATE_LIMITED,           ///< 该 ID 前 times 次以条目 429 失败
        ITEM_MAPPING_ERROR,          ///< 该 ID 恒为 400 mapping 校验失败
        DISCONNECT_BEFORE_APPLY,     ///< 前 times 次：处理前断连（请求丢失）
        DISCONNECT_AFTER_APPLY       ///< 前 times 次：落库后断连（响应丢失）
    };

    struct StoredDoc {
        json source;
        std::int64_t version = 0;
    };

    struct FaultRule {
        Fault fault = Fault::OK;
        int times = 0;  // 剩余注入次数
    };

    /// 记录每个请求携带的业务 ID（按请求内顺序）
    std::vector<std::vector<std::string>> requestIds;
    int requestCount = 0;
    int wholeRejectionsLeft = 0;  // 剩余整请求 429 次数

    /// 排队的整请求脚本响应（FIFO，优先于条目规则），用于构造 400/503 等
    std::deque<HttpResponse> scriptedWholeResponses;

    void setFault(const std::string& id, Fault fault, int times = 0) {
        rules_[id] = FaultRule{fault, times};
    }

    void setWholeRequestRejections(int times) {
        wholeRejectionsLeft = times;
    }

    HttpResponse post(const std::string& /*url*/,
                      const std::string& ndjsonBody) override {
        ++requestCount;

        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= ndjsonBody.size()) {
            std::size_t nl = ndjsonBody.find('\n', start);
            std::string line = ndjsonBody.substr(
                start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty()) lines.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }

        struct Entry {
            std::string index;
            std::string id;
            json source;
        };
        std::vector<Entry> entries;
        for (std::size_t i = 0; i + 1 < lines.size(); i += 2) {
            json action = json::parse(lines[i]);
            const auto& meta = action.at("index");
            entries.push_back({meta.value("_index", ""),
                               meta.value("_id", ""),
                               json::parse(lines[i + 1])});
        }

        std::vector<std::string> ids;
        for (const auto& e : entries) ids.push_back(e.id);
        requestIds.push_back(ids);

        // 排队的整请求脚本响应（最优先）
        if (!scriptedWholeResponses.empty()) {
            HttpResponse r = std::move(scriptedWholeResponses.front());
            scriptedWholeResponses.pop_front();
            return r;
        }

        // 整请求被限流（ES 线程池拒绝时直接回 429，不落库任何条目）
        if (wholeRejectionsLeft > 0) {
            --wholeRejectionsLeft;
            return HttpResponse{429,
                R"({"error":{"type":"es_rejected_execution_exception",)"
                R"("reason":"rejected execution of primary, too many requests"},)"
                R"("status":429})", {}};
        }

        // 响应到达前断连：区分“未处理”与“已处理但响应丢失”
        for (const auto& e : entries) {
            auto it = rules_.find(e.id);
            if (it != rules_.end() && it->second.times > 0 &&
                (it->second.fault == Fault::DISCONNECT_BEFORE_APPLY ||
                 it->second.fault == Fault::DISCONNECT_AFTER_APPLY)) {
                const bool applied =
                    it->second.fault == Fault::DISCONNECT_AFTER_APPLY;
                --it->second.times;
                if (applied) {
                    for (const auto& en : entries) upsert(en.index, en.id, en.source);
                }
                throw TransportError(
                    applied ? "Failure when receiving data: Connection reset by peer"
                            : "Failed to connect: Connection refused");
            }
        }

        // 逐项处理，严格保持请求顺序
        json items = json::array();
        bool errors = false;
        for (const auto& e : entries) {
            auto it = rules_.find(e.id);
            if (it != rules_.end() && it->second.times > 0) {
                switch (it->second.fault) {
                    case Fault::ITEM_RATE_LIMITED: {
                        --it->second.times;
                        errors = true;
                        items.push_back({{"index", {
                            {"_index", e.index}, {"_id", e.id}, {"status", 429},
                            {"error", {
                                {"type", "es_rejected_execution_exception"},
                                {"reason", "rejected execution of primary for [" +
                                               e.id + "]"},
                                {"status", "429"}}}}}});
                        continue;
                    }
                    case Fault::ITEM_MAPPING_ERROR: {
                        // times 不递减：永久错误每次都复现
                        errors = true;
                        items.push_back({{"index", {
                            {"_index", e.index}, {"_id", e.id}, {"status", 400},
                            {"error", {
                                {"type", "mapper_parsing_exception"},
                                {"reason", "failed to parse field [created_at] of "
                                           "type [date] in document with id '" +
                                               e.id + "'"},
                                {"caused_by", {
                                    {"type", "illegal_argument_exception"},
                                    {"reason", "failed to parse date field "
                                               "[2024/13/40] with format [yyyy-MM-dd]"}}}}}}}});
                        continue;
                    }
                    default:
                        break;
                }
            }

            const std::string result = upsert(e.index, e.id, e.source);
            const auto& doc = store_[{e.index, e.id}];
            items.push_back({{"index", {
                {"_index", e.index},
                {"_id", e.id},
                {"_version", doc.version},
                {"result", result},
                {"status", result == "created" ? 201 : 200}}}});
        }

        json body = {
            {"took", 7},
            {"errors", errors},
            {"items", items}
        };
        return HttpResponse{200, body.dump(), {}};
    }

    std::size_t docCount() const { return store_.size(); }

    bool contains(const std::string& index, const std::string& id) const {
        return store_.find({index, id}) != store_.end();
    }

    std::int64_t versionOf(const std::string& index, const std::string& id) const {
        auto it = store_.find({index, id});
        return it == store_.end() ? 0 : it->second.version;
    }

    /// 某业务 ID 在全部请求中被发送的次数（验证永久失败条目不再被重发）
    int sendCountOf(const std::string& id) const {
        int n = 0;
        for (const auto& req : requestIds) {
            n += static_cast<int>(std::count(req.begin(), req.end(), id));
        }
        return n;
    }

private:
    std::map<std::string, FaultRule> rules_;
    std::map<std::pair<std::string, std::string>, StoredDoc> store_;

    std::string upsert(const std::string& index,
                       const std::string& id,
                       const json& source) {
        auto key = std::make_pair(index, id);
        auto it = store_.find(key);
        if (it == store_.end()) {
            store_[key] = StoredDoc{source, 1};
            return "created";
        }
        it->second.source = source;
        ++it->second.version;
        return "updated";
    }
};

} // namespace fake
} // namespace es

#endif // FAKE_BULK_SERVER_HPP

#include "bulk_test_support.hpp"

#include <sstream>

namespace es {

BulkTransportResponse FakeBulkTransport::sendBulk(const std::string& path,
                                                  const std::string& ndjsonBody) {
    // 解析 NDJSON：action 行与 source 行成对出现
    struct Line {
        std::string id;
        std::string index;
    };
    std::vector<Line> lines;
    {
        std::istringstream iss(ndjsonBody);
        std::string actionLine;
        std::string sourceLine;
        while (std::getline(iss, actionLine) &&
               std::getline(iss, sourceLine)) {
            if (actionLine.empty()) continue;
            json action = json::parse(actionLine);
            const json& meta = action["index"];
            lines.push_back({meta.value("_id", ""), meta.value("_index", "")});
        }
    }

    RecordedRequest rec;
    rec.requestIndex = static_cast<int>(history.size());
    rec.path = path;
    rec.body = ndjsonBody;
    for (const Line& l : lines) rec.ids.push_back(l.id);
    history.push_back(std::move(rec));

    for (const Line& l : lines) attemptsPerId_[l.id]++;

    // 场景一：响应到达前断开 —— 存储状态不变，调用方只能凭 _id 安全重放
    if (transportFailuresRemaining_ > 0) {
        --transportFailuresRemaining_;
        throw TransientTransportError(
            "connection reset by peer before response (simulated)");
    }

    // 场景二：整块 HTTP 级失败（429 限流 / 5xx 网关抖动 / 400 永久拒绝）
    if (!wholeStatusQueue_.empty()) {
        int status = wholeStatusQueue_.front();
        wholeStatusQueue_.pop_front();
        json body;
        if (status == 429) {
            body = {{"error", {{"type", "es_rejected_execution_exception"},
                               {"reason", "rejected execution of primary: too many requests"}}},
                    {"status", 429}};
        } else if (status >= 500) {
            body = {{"error", {{"type", "illegal_state_exception"},
                               {"reason", "simulated gateway failure"}}},
                    {"status", status}};
        } else {
            body = {{"error", {{"type", "illegal_argument_exception"},
                               {"reason", "simulated permanent bulk rejection"}}},
                    {"status", status}};
        }
        return {status, body.dump()};
    }

    // 场景三：逐项响应（部分成功 / 部分失败，与真实 ES bulk 语义一致）
    json items = json::array();
    bool errors = false;
    for (const Line& l : lines) {
        int itemStatus = 200;
        if (auto pit = persistentItemStatus_.find(l.id);
            pit != persistentItemStatus_.end()) {
            itemStatus = pit->second;
        } else if (auto it = itemScript_.find(l.id);
                   it != itemScript_.end() && !it->second.empty()) {
            itemStatus = it->second.front();
            it->second.pop_front();
        }

        json entry;
        if (itemStatus >= 200 && itemStatus < 300) {
            bool existed = storedIds_.count(l.id) != 0;
            long version = existed ? versions_[l.id] + 1 : 1;
            storedIds_.insert(l.id);
            versions_[l.id] = version;
            entry = {{"index", {
                {"_index", l.index},
                {"_id", l.id},
                {"_version", version},
                {"result", existed ? "updated" : "created"},
                {"status", existed ? 200 : 201}
            }}};
        } else {
            errors = true;
            std::string type;
            std::string reason;
            if (itemStatus == 429) {
                type = "es_rejected_execution_exception";
                reason = "rejected execution of primary for id " + l.id;
            } else {
                type = "mapper_parsing_exception";
                reason = "failed to parse field for id " + l.id +
                         ": mapping validation rejected the document";
            }
            entry = {{"index", {
                {"_index", l.index},
                {"_id", l.id},
                {"status", itemStatus},
                {"error", {{"type", type}, {"reason", reason}}}
            }}};
        }
        items.push_back(entry);
    }

    json response = {
        {"took", 7},
        {"errors", errors},
        {"items", items}
    };
    return {200, response.dump()};
}

} // namespace es

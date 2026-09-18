#ifndef BULK_TEST_SUPPORT_HPP
#define BULK_TEST_SUPPORT_HPP

#include "bulk_writer.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace es {

/**
 * 不睡真实时钟的 ISleeper：只记录每次退避的毫秒数，
 * 使自动化场景能在零等待下断言退避次数与时长边界。
 */
class ScriptedSleeper final : public ISleeper {
public:
    std::vector<long long> sleepsMs;
    void sleepFor(std::chrono::milliseconds duration) override {
        sleepsMs.push_back(duration.count());
    }
};

/**
 * 固定抖动源（亦可逐次脚本化），返回 [0,1] 内的确定值。
 */
class ScriptedJitter final : public IJitter {
public:
    explicit ScriptedJitter(double fixed = 0.5) : fixed_(fixed) {}

    void push(double value) { sequence_.push_back(value); }

    double next() override {
        if (!sequence_.empty()) {
            double v = sequence_.front();
            sequence_.pop_front();
            return v;
        }
        return fixed_;
    }

private:
    double fixed_;
    std::deque<double> sequence_;
};

/**
 * 内存中的 Elasticsearch /_bulk 模拟器。
 *
 *  - 以稳定 _id 去重存储：相同 _id 的 index 动作只覆盖、不新增文档，
 *    因此可以直接证明「断开重发 / 整批再提交」不会产生第二份文章；
 *  - 可脚本化：整请求级 429/5xx、响应前传输断开、
 *    以及逐条 429（瞬时）与 400（mapping 永久错误）。
 */
class FakeBulkTransport final : public IBulkTransport {
public:
    struct RecordedRequest {
        int requestIndex;
        std::string path;
        std::string body;
        std::vector<std::string> ids; // 该次请求实际包含的条目 ID（顺序）
    };

    std::vector<RecordedRequest> history;

    // ---- 脚本编排 ----

    /** 接下来的 n 次请求在响应到达前「断开连接」 */
    void failNextRequestsTransport(int n) { transportFailuresRemaining_ += n; }

    /** 接下来的请求依次直接返回指定的整块 HTTP 状态（如 429/503/400） */
    void enqueueWholeStatus(int status) { wholeStatusQueue_.push_back(status); }

    /** 某条目在接下来的请求中依次返回指定状态（耗尽后视为成功） */
    void scriptItem(const std::string& id, int status) {
        itemScript_[id].push_back(status);
    }

    /** 某条目始终返回指定状态（用于坏数据在整批重放时依然失败） */
    void persistItemStatus(const std::string& id, int status) {
        persistentItemStatus_[id] = status;
    }

    // ---- 可观测状态 ----

    std::size_t storedDocCount() const { return storedIds_.size(); }
    int attemptsFor(const std::string& id) const {
        auto it = attemptsPerId_.find(id);
        return it == attemptsPerId_.end() ? 0 : it->second;
    }
    bool contains(const std::string& id) const {
        return storedIds_.count(id) != 0;
    }
    long versionOf(const std::string& id) const {
        auto it = versions_.find(id);
        return it == versions_.end() ? 0 : it->second;
    }

    BulkTransportResponse sendBulk(const std::string& path,
                                   const std::string& ndjsonBody) override;

private:
    int transportFailuresRemaining_ = 0;
    std::deque<int> wholeStatusQueue_;
    std::map<std::string, std::deque<int>> itemScript_;
    std::map<std::string, int> persistentItemStatus_;
    std::set<std::string> storedIds_;
    std::map<std::string, long> versions_;
    std::map<std::string, int> attemptsPerId_;
};

} // namespace es

#endif // BULK_TEST_SUPPORT_HPP

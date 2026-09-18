#ifndef BULK_WRITER_HPP
#define BULK_WRITER_HPP

#include "json.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace es {

using json = nlohmann::json;

/**
 * 单条文章的最终状态。
 *
 * 运营视角只需要区分三类：
 *   Written          —— 已写入（首次即成功）
 *   RetriedSuccess   —— 重试后成功
 *   FailedPermanent  —— 不可重试（如 mapping 校验失败），立即留在失败清单
 * 另有 RetriesExhausted 表示错误本身可重试、但已达到尝试上限，
 * 回执中同样归入失败类，值班人员应稍后重新提交该业务批次。
 */
enum class ItemStatus {
    Written,           ///< 第一次尝试即确认写入
    RetriedSuccess,    ///< 至少经历一次重试后确认写入
    FailedPermanent,   ///< 永久性错误，不重试
    RetriesExhausted   ///< 可重试错误，但尝试次数达到上限
};

/**
 * 逐项回执。results 严格保持原始输入顺序。
 */
struct ItemReceipt {
    std::size_t index = 0;          ///< 原始输入下标（从 0 开始）
    std::string id;                 ///< 调用方提供的稳定业务 ID
    ItemStatus status = ItemStatus::FailedPermanent;
    int attempts = 0;               ///< 该条实际被发送的次数（含首次）
    int httpStatus = 0;             ///< 最后一次相关 HTTP 状态；传输层失败为 0
    std::string esResult;           ///< ES 返回的 result（created/updated），可能为空
    std::string error;              ///< 精简错误原因；成功时为空
};

/**
 * 整批回执。
 */
struct BulkReceipt {
    std::vector<ItemReceipt> items; ///< 与输入同序
    std::size_t chunkCount = 0;     ///< 划分出的分块数
    std::size_t requestCount = 0;   ///< 实际发出的 HTTP 请求次数（含重试）
    std::size_t backoffCount = 0;   ///< 实际发生的退避等待次数

    std::size_t count(ItemStatus s) const;
    std::size_t confirmedCount() const; ///< Written + RetriedSuccess
    std::size_t failedCount() const;    ///< FailedPermanent + RetriesExhausted
};

/**
 * 发送前校验失败（空批次、ID 数量不匹配、存在空 ID/重复 ID、
 * 单条数据超过分块字节限制等）。抛出时保证尚未发送任何请求。
 */
class BulkValidationError : public std::invalid_argument {
public:
    explicit BulkValidationError(const std::string& message)
        : std::invalid_argument(message) {}
};

/**
 * 请求已发出、但在响应到达前发生传输失败（连接断开、超时等）。
 * 此时写入结果未知，凭借调用方提供的稳定 _id 重放同一分块不会产生重复文档。
 */
class TransientTransportError : public std::runtime_error {
public:
    explicit TransientTransportError(const std::string& message)
        : std::runtime_error(message) {}
};

struct BulkTransportResponse {
    int statusCode = 0;
    std::string body;
};

/**
 * 分块传输层抽象。生产实现走 libcurl；测试中替换为脚本化 Fake。
 *
 * 约定：
 *  - 只要拿到了 HTTP 响应（包括 4xx/5xx）就必须返回，不允许抛异常；
 *  - 仅当响应到达前链路中断时抛 TransientTransportError。
 */
class IBulkTransport {
public:
    virtual ~IBulkTransport() = default;
    virtual BulkTransportResponse sendBulk(const std::string& path,
                                           const std::string& ndjsonBody) = 0;
};

/**
 * 退避等待时钟。生产实现睡真实时钟；自动化场景替换为记录型假实现，
 * 即可在不等待的情况下断言退避次数与等待时长边界。
 */
class ISleeper {
public:
    virtual ~ISleeper() = default;
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

/**
 * 随机抖动源，返回 [0, 1) 区间的 double。自动化场景可脚本化。
 */
class IJitter {
public:
    virtual ~IJitter() = default;
    virtual double next() = 0;
};

/**
 * 批量写入策略。
 */
struct BulkOptions {
    std::size_t chunkSize = 500;                 ///< 每个分块最大条目数
    std::size_t maxChunkBytes = 5u * 1024 * 1024;///< 单个分块请求体最大字节数（ES 默认 http.max_content_length）
    int maxAttempts = 4;                         ///< 单条最大尝试次数（含首次），重试总上限
    std::chrono::milliseconds initialDelay{200}; ///< 首次退避基数
    std::chrono::milliseconds maxDelay{5000};    ///< 退避上限
    double backoffMultiplier = 2.0;              ///< 指数退避倍数
};

/**
 * 可靠批量写入器。
 *
 * 设计要点：
 *  - 每条文章必须带调用方提供的稳定业务 ID，action 固定为 index（幂等覆盖），
 *    因此「响应前断开 → 重发同一分块」与「整批再次提交」都不会产生第二份文档；
 *  - 429/502/503/504 及传输层失败只重试尚未确认的条目，退避有上限、带抖动；
 *  - mapping 校验（400 等）等永久性错误当次即定案，不浪费重试额度；
 *  - 回执按原始输入顺序排列。
 *
 * 线程安全：单个 BulkWriter 实例不保证可并发调用 write()。
 */
class BulkWriter {
public:
    BulkWriter(std::shared_ptr<IBulkTransport> transport,
               std::string indexName,
               BulkOptions options = {},
               std::shared_ptr<ISleeper> sleeper = nullptr,
               std::shared_ptr<IJitter> jitter = nullptr);

    BulkReceipt write(const std::vector<json>& docs,
                      const std::vector<std::string>& ids);

    /** 状态码是否属于可重试的瞬时 HTTP 状态：429/502/503/504 */
    static bool isRetriableStatus(int statusCode) noexcept;

    /** 状态码的稳定英文标识，便于日志与报表 */
    static const char* statusName(ItemStatus status) noexcept;

private:
    std::shared_ptr<IBulkTransport> transport_;
    std::string indexName_;
    BulkOptions options_;
    std::shared_ptr<ISleeper> sleeper_;
    std::shared_ptr<IJitter> jitter_;

    std::chrono::milliseconds backoffDelay(int round) const;
};

} // namespace es

#endif // BULK_WRITER_HPP

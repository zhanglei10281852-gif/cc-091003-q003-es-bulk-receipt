#ifndef BULK_INDEXER_HPP
#define BULK_INDEXER_HPP

#include "http_client.hpp"  // HttpResponse（该头文件本身不依赖 libcurl）
#include "nlohmann/json.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace es {

using json = nlohmann::json;

// ==================== 异常 ====================

/**
 * 传输层故障：响应尚未到达连接即断开/超时。
 * 此时请求是否已被 Elasticsearch 处理是未知的，可凭相同业务 ID 安全重发。
 */
class TransportError : public std::runtime_error {
public:
    explicit TransportError(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * 发送前校验失败，任何 HTTP 请求都不会发出：
 * 空批次 / ID 缺失或数量不匹配 / 业务 ID 重复 / 单条记录超过分块字节上限 /
 * 分块参数非法 / 记录不是 JSON 对象。
 */
class BulkValidationError : public std::invalid_argument {
public:
    explicit BulkValidationError(const std::string& message)
        : std::invalid_argument(message) {}
};

// ==================== 回执类型 ====================

/**
 * 单条记录的最终状态。
 */
enum class ItemStatus {
    INDEXED,               ///< 首次尝试即写入（attempts == 1）
    INDEXED_AFTER_RETRY,   ///< 退避重试后写入（attempts > 1）
    PERMANENT_FAILURE,     ///< 收到不可重试响应（如 400 mapping 校验失败），不再重试
    RETRY_EXHAUSTED,       ///< 始终收到可重试状态（429/502/503/504），达到尝试上限
    UNCONFIRMED            ///< 最后一次发送遭遇传输中断，是否落库未知，可安全重发
};

/** 状态 -> 中文短标签，供演示回执直接打印 */
std::string toString(ItemStatus status);

/**
 * 逐项回执：与输入下标一一对应，BulkReport::items 严格保持原输入顺序。
 */
struct ItemReceipt {
    std::size_t index = 0;           ///< 原输入下标（从 0 开始）
    std::string id;                  ///< 调用方提供的稳定业务 ID
    ItemStatus status = ItemStatus::UNCONFIRMED;
    int attempts = 0;                ///< 包含该条的发送次数（1 表示一次成功）
    int httpStatus = 0;              ///< 最终确认性 HTTP 状态；传输中断为 0
    std::string esResult;            ///< ES 返回的 result：created / updated
    std::int64_t version = 0;        ///< ES 返回的文档版本（若有）
    std::string errorType;           ///< ES error.type，如 mapper_parsing_exception
    std::string errorReason;         ///< 精简后的单行错误原因
    std::size_t chunkSeq = 0;        ///< 首次发送所属分块序号（从 0 开始）

    bool written() const {
        return status == ItemStatus::INDEXED ||
               status == ItemStatus::INDEXED_AFTER_RETRY;
    }
};

// ==================== 配置 ====================

/**
 * 分块选项：每个 HTTP _bulk 请求同时受文档条数与字节数上限约束。
 */
struct BulkOptions {
    std::size_t chunkSize = 500;                 ///< 每分块最大文档数
    std::size_t maxChunkBytes = 5 * 1024 * 1024; ///< 每分块 NDJSON 最大字节数（默认 5 MiB）
};

/**
 * 有上限的指数退避策略（full jitter）。
 *
 * 第 f 次发送失败后的等待：sleep( jitter() * min(maxBackoff, initialBackoff * multiplier^(f-1)) )
 *
 * sleeper 与 jitter 均可替换：自动化场景注入“假睡眠”和固定抖动，
 * 无需真实等待即可断言退避次数、倍数与上限。
 */
struct RetryPolicy {
    int maxAttempts = 3;            ///< 单条最大发送次数（含首次，最小为 1）
    std::chrono::milliseconds initialBackoff{100};
    std::chrono::milliseconds maxBackoff{5000};
    double multiplier = 2.0;

    /// 用毫秒数阻塞；为空时退化为 std::this_thread::sleep_for
    std::function<void(std::chrono::milliseconds)> sleeper;
    /// 返回 [0,1) 的抖动值；为空时使用 std::mt19937 的随机值
    std::function<double()> jitter;
};

/**
 * 整批导入的汇总报告。
 */
struct BulkReport {
    std::vector<ItemReceipt> items;  ///< 与输入等长、同序
    int chunksPlanned = 0;           ///< 计划分块数
    int requestsSent = 0;            ///< 实际发出的 _bulk HTTP 请求数（含重试）
    std::vector<std::chrono::milliseconds> backoffWaits; ///< 各次退避时长（注入时钟时为模拟值）

    std::size_t indexedCount() const;             ///< 首次即写入
    std::size_t indexedAfterRetryCount() const;   ///< 重试后写入
    std::size_t permanentFailureCount() const;    ///< 不可重试失败
    std::size_t retryExhaustedCount() const;      ///< 重试耗尽
    std::size_t unconfirmedCount() const;         ///< 传输中断、结果未知
    std::size_t writtenCount() const;             ///< 已写入总数
    std::size_t failedCount() const;              ///< 未写入总数（耗尽 + 永久失败 + 未知）
};

// ==================== 传输抽象 ====================

/**
 * _bulk 请求的最小传输抽象。
 * 生产环境由 libcurl 适配器实现；测试与离线演示使用脚本化假实现。
 */
class BulkTransport {
public:
    virtual ~BulkTransport() = default;

    /**
     * 发送 POST。
     * @return 收到响应时返回其状态码与 body（即使是 4xx/5xx）；
     *         响应到达前发生断连/超时等短暂传输故障时抛 TransportError。
     */
    virtual HttpResponse post(const std::string& url,
                              const std::string& ndjsonBody) = 0;
};

/** 429 / 502 / 503 / 504 为可重试 HTTP 状态，其余非 2xx 一律视为永久错误 */
bool isRetryableStatus(int httpStatus);

/**
 * 生产环境传输适配器：把 BulkTransport 桥接到既有 libcurl HttpClient。
 * HttpClient 抛出的 HttpException 一律视为响应到达前的短暂传输故障，
 * 转换为 TransportError 参与退避重试。
 */
class HttpBulkTransport : public BulkTransport {
public:
    HttpBulkTransport(HttpClient& client, std::string baseUrl);

    HttpResponse post(const std::string& url,
                      const std::string& ndjsonBody) override;

private:
    HttpClient& client_;
    std::string baseUrl_;
};

// ==================== 批量写入器 ====================

/**
 * 具备可控分块、逐项回执、有上限退避重试的批量写入器。
 *
 * 关键约定：
 * - 每条记录必须携带调用方给定的稳定业务 ID，动作行固定为
 *   {"index": {"_index": ..., "_id": "<业务ID>"}}，因此响应前断连后
 *   重放同一分块只会覆盖同 _id 文档，不会产生第二份文章；
 * - 仅重试“尚未确认”的条目：分块整体被限流时重发整个分块，
 *   200 响应中个别条目可重试失败时只重发这些条目，永久错误立即落失败清单；
 * - run() 只在发送前校验失败时抛 BulkValidationError，单条失败全部体现在回执中。
 */
class BulkIndexer {
public:
    BulkIndexer(BulkTransport& transport,
                std::string indexName,
                BulkOptions options = BulkOptions{},
                RetryPolicy retryPolicy = RetryPolicy{});

    /**
     * 执行批量导入。
     * @param docs 与 ids 等长的文档对象数组
     * @param ids  与 docs 一一对应的稳定业务 ID，不允许空串
     * @throws BulkValidationError 发送前校验失败（此时不会发出任何请求）
     */
    BulkReport run(const std::vector<json>& docs,
                   const std::vector<std::string>& ids);

    /** 根据策略计算第 failedSends 次失败后的退避时长（full jitter，已封顶） */
    std::chrono::milliseconds backoffDelay(int failedSends) const;

private:
    struct PreparedItem {
        std::size_t pos = 0;
        std::string actionLine;
        std::string sourceLine;
        std::size_t bytes = 0;
    };

    BulkTransport& transport_;
    std::string indexName_;
    BulkOptions options_;
    RetryPolicy retry_;

    std::vector<std::vector<std::size_t>> planChunks(
        const std::vector<PreparedItem>& prepared) const;
    void sendChunk(const std::vector<PreparedItem>& prepared,
                   const std::vector<std::size_t>& chunk,
                   BulkReport& report);
    std::string buildBody(const std::vector<PreparedItem>& prepared,
                          const std::vector<std::size_t>& positions) const;
};

} // namespace es

#endif // BULK_INDEXER_HPP

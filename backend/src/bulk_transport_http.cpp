#include "bulk_indexer.hpp"

namespace es {

// libcurl 传输适配器独立成一个翻译单元，
// 使纯逻辑（分块/重试/回执）的演示与测试无需链接 libcurl。

HttpBulkTransport::HttpBulkTransport(HttpClient& client, std::string baseUrl)
    : client_(client), baseUrl_(std::move(baseUrl)) {}

HttpResponse HttpBulkTransport::post(const std::string& url,
                                    const std::string& ndjsonBody) {
    try {
        // _bulk 要求 NDJSON 编码；显式声明，避免被当作普通 JSON。
        return client_.post(baseUrl_ + url, ndjsonBody,
                            {{"Content-Type", "application/x-ndjson"}});
    } catch (const HttpException& e) {
        // 响应到达前的连接断开/超时等短暂传输故障，转换为可重试语义。
        throw TransportError(e.what());
    }
}

} // namespace es

#include "es_client.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>

namespace es {

// ==================== 构造与析构 ====================

ESClient::ESClient(const std::string& host, int port) {
    std::ostringstream oss;
    oss << "http://" << host << ":" << port;
    baseUrl_ = oss.str();
    httpClient_.setTimeout(30);
    httpClient_.setConnectTimeout(10);
}

ESClient::~ESClient() = default;

// ==================== 辅助方法 ====================

void ESClient::log(const std::string& message) {
    if (logCallback_) {
        logCallback_(message);
    }
}

std::string ESClient::buildUrl(const std::string& path) {
    return baseUrl_ + path;
}

void ESClient::setLogCallback(LogCallback callback) {
    logCallback_ = std::move(callback);
}

// ==================== 集群操作 ====================

bool ESClient::ping() {
    try {
        auto response = httpClient_.get(buildUrl("/"));
        return response.isSuccess();
    } catch (const HttpException&) {
        return false;
    }
}

json ESClient::clusterHealth() {
    auto response = httpClient_.get(buildUrl("/_cluster/health"));
    if (!response.isSuccess()) {
        throw ESException("Failed to get cluster health: " + response.body);
    }
    return json::parse(response.body);
}

json ESClient::clusterInfo() {
    auto response = httpClient_.get(buildUrl("/"));
    if (!response.isSuccess()) {
        throw ESException("Failed to get cluster info: " + response.body);
    }
    return json::parse(response.body);
}

// ==================== 索引操作 ====================

bool ESClient::createIndex(const std::string& indexName,
                           const json& mappings,
                           const json& settings) {
    json body;
    if (!mappings.empty()) {
        body["mappings"] = mappings;
    }
    if (!settings.empty()) {
        body["settings"] = settings;
    }
    
    log("Creating index: " + indexName);
    auto response = httpClient_.put(buildUrl("/" + indexName), body.dump());
    
    if (!response.isSuccess()) {
        auto error = json::parse(response.body);
        throw ESException("Failed to create index: " + 
                         error.value("error", json::object()).value("reason", response.body));
    }
    
    log("Index created successfully: " + indexName);
    return true;
}

bool ESClient::deleteIndex(const std::string& indexName) {
    log("Deleting index: " + indexName);
    auto response = httpClient_.del(buildUrl("/" + indexName));
    
    if (!response.isSuccess() && !response.isNotFound()) {
        throw ESException("Failed to delete index: " + response.body);
    }
    
    log("Index deleted: " + indexName);
    return true;
}

bool ESClient::indexExists(const std::string& indexName) {
    auto response = httpClient_.head(buildUrl("/" + indexName));
    return response.isSuccess();
}

json ESClient::getIndex(const std::string& indexName) {
    auto response = httpClient_.get(buildUrl("/" + indexName));
    if (!response.isSuccess()) {
        throw ESException("Failed to get index: " + response.body);
    }
    return json::parse(response.body);
}

bool ESClient::refreshIndex(const std::string& indexName) {
    auto response = httpClient_.post(buildUrl("/" + indexName + "/_refresh"), "");
    return response.isSuccess();
}

// ==================== 文档操作 ====================

DocResult ESClient::indexDocument(const std::string& indexName,
                                  const json& doc,
                                  const std::string& id) {
    std::string url = "/" + indexName + "/_doc";
    if (!id.empty()) {
        url += "/" + id;
    }
    
    auto response = httpClient_.post(buildUrl(url), doc.dump());
    
    DocResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.id = respJson.value("_id", "");
        result.index = respJson.value("_index", "");
        result.result = respJson.value("result", "");
        result.version = respJson.value("_version", 0);
        result.success = true;
        log("Document indexed: " + result.id);
    } else {
        result.success = false;
        throw ESException("Failed to index document: " + response.body);
    }
    
    return result;
}

std::optional<json> ESClient::getDocument(const std::string& indexName,
                                          const std::string& id) {
    auto response = httpClient_.get(buildUrl("/" + indexName + "/_doc/" + id));
    
    if (response.isNotFound()) {
        return std::nullopt;
    }
    
    if (!response.isSuccess()) {
        throw ESException("Failed to get document: " + response.body);
    }
    
    auto respJson = json::parse(response.body);
    if (respJson.value("found", false)) {
        return respJson["_source"];
    }
    return std::nullopt;
}

DocResult ESClient::updateDocument(const std::string& indexName,
                                   const std::string& id,
                                   const json& doc) {
    json body = {{"doc", doc}};
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_update/" + id), 
        body.dump()
    );
    
    DocResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.id = respJson.value("_id", "");
        result.index = respJson.value("_index", "");
        result.result = respJson.value("result", "");
        result.version = respJson.value("_version", 0);
        result.success = true;
        log("Document updated: " + result.id);
    } else {
        result.success = false;
        throw ESException("Failed to update document: " + response.body);
    }
    
    return result;
}

bool ESClient::deleteDocument(const std::string& indexName,
                              const std::string& id) {
    auto response = httpClient_.del(buildUrl("/" + indexName + "/_doc/" + id));
    
    if (response.isSuccess()) {
        log("Document deleted: " + id);
        return true;
    }
    
    if (response.isNotFound()) {
        return false;
    }
    
    throw ESException("Failed to delete document: " + response.body);
}

BulkResult ESClient::bulkIndex(const std::string& indexName,
                               const std::vector<json>& docs,
                               const std::vector<std::string>& ids) {
    std::ostringstream body;
    
    for (size_t i = 0; i < docs.size(); ++i) {
        json action = {{"index", {{"_index", indexName}}}};
        if (i < ids.size() && !ids[i].empty()) {
            action["index"]["_id"] = ids[i];
        }
        body << action.dump() << "\n";
        body << docs[i].dump() << "\n";
    }
    
    auto response = httpClient_.post(buildUrl("/_bulk"), body.str());
    
    BulkResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.took = respJson.value("took", 0);
        result.errors = respJson.value("errors", false);
        result.successCount = 0;
        result.failCount = 0;
        
        for (const auto& item : respJson["items"]) {
            DocResult docResult;
            const auto& indexResult = item["index"];
            docResult.id = indexResult.value("_id", "");
            docResult.index = indexResult.value("_index", "");
            docResult.result = indexResult.value("result", "");
            docResult.version = indexResult.value("_version", 0);
            docResult.success = indexResult.value("status", 500) < 300;
            
            if (docResult.success) {
                result.successCount++;
            } else {
                result.failCount++;
            }
            result.items.push_back(docResult);
        }
        
        log("Bulk indexed " + std::to_string(result.successCount) + " documents");
    } else {
        throw ESException("Bulk index failed: " + response.body);
    }
    
    return result;
}

// ==================== 搜索操作 ====================

SearchResult ESClient::parseSearchResponse(const json& response) {
    SearchResult result;
    result.took = response.value("took", 0);
    result.timedOut = response.value("timed_out", false);
    
    const auto& hits = response["hits"];
    const auto& total = hits["total"];
    result.total = total.is_object() ? total.value("value", 0) : total.get<int>();
    result.maxScore = hits.value("max_score", 0.0);
    
    for (const auto& hit : hits["hits"]) {
        SearchHit searchHit;
        searchHit.id = hit.value("_id", "");
        searchHit.index = hit.value("_index", "");
        searchHit.score = hit.value("_score", 0.0);
        searchHit.source = hit.value("_source", json::object());
        searchHit.highlight = hit.value("highlight", json::object());
        result.hits.push_back(searchHit);
    }
    
    return result;
}

SearchResult ESClient::matchSearch(const std::string& indexName,
                                   const std::string& field,
                                   const std::string& query,
                                   int from,
                                   int size) {
    json body = {
        {"query", {
            {"match", {{field, query}}}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::multiMatchSearch(const std::string& indexName,
                                        const std::vector<std::string>& fields,
                                        const std::string& query,
                                        int from,
                                        int size) {
    json body = {
        {"query", {
            {"multi_match", {
                {"query", query},
                {"fields", fields}
            }}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::termSearch(const std::string& indexName,
                                  const std::string& field,
                                  const std::string& value,
                                  int from,
                                  int size) {
    json body = {
        {"query", {
            {"term", {{field, value}}}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::boolSearch(const std::string& indexName,
                                  const json& must,
                                  const json& should,
                                  const json& mustNot,
                                  const json& filter,
                                  int from,
                                  int size) {
    json boolQuery;
    if (!must.empty()) boolQuery["must"] = must;
    if (!should.empty()) boolQuery["should"] = should;
    if (!mustNot.empty()) boolQuery["must_not"] = mustNot;
    if (!filter.empty()) boolQuery["filter"] = filter;
    
    json body = {
        {"query", {{"bool", boolQuery}}},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::searchWithHighlight(const std::string& indexName,
                                           const json& query,
                                           const std::vector<std::string>& highlightFields,
                                           int from,
                                           int size) {
    json fields;
    for (const auto& field : highlightFields) {
        fields[field] = json::object();
    }
    
    json body = {
        {"query", query},
        {"highlight", {
            {"pre_tags", {"<em>"}},
            {"post_tags", {"</em>"}},
            {"fields", fields}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::search(const std::string& indexName,
                              const json& queryBody) {
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_search"),
        queryBody.dump()
    );
    
    if (!response.isSuccess()) {
        throw ESException("Search failed: " + response.body);
    }
    
    return parseSearchResponse(json::parse(response.body));
}

} // namespace es

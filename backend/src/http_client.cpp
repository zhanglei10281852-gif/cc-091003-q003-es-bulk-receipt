#include "http_client.hpp"
#include <curl/curl.h>
#include <sstream>
#include <algorithm>

namespace es {

// ==================== 实现类 ====================

class HttpClient::Impl {
public:
    CURL* curl;
    long timeout;
    long connectTimeout;
    
    Impl() : curl(nullptr), timeout(30), connectTimeout(10) {
        curl = curl_easy_init();
        if (!curl) {
            throw HttpException("Failed to initialize CURL");
        }
    }
    
    ~Impl() {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }
    
    Impl(Impl&& other) noexcept 
        : curl(other.curl), timeout(other.timeout), connectTimeout(other.connectTimeout) {
        other.curl = nullptr;
    }
    
    Impl& operator=(Impl&& other) noexcept {
        if (this != &other) {
            if (curl) {
                curl_easy_cleanup(curl);
            }
            curl = other.curl;
            timeout = other.timeout;
            connectTimeout = other.connectTimeout;
            other.curl = nullptr;
        }
        return *this;
    }
};

// ==================== 回调函数 ====================

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t totalSize = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    
    std::string header(buffer, totalSize);
    size_t colonPos = header.find(':');
    if (colonPos != std::string::npos) {
        std::string key = header.substr(0, colonPos);
        std::string value = header.substr(colonPos + 1);
        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        (*headers)[key] = value;
    }
    return totalSize;
}

// ==================== HttpClient 实现 ====================

HttpClient::HttpClient() : pImpl(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() = default;

HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

void HttpClient::setTimeout(long seconds) {
    pImpl->timeout = seconds;
}

void HttpClient::setConnectTimeout(long seconds) {
    pImpl->connectTimeout = seconds;
}

HttpResponse HttpClient::get(const std::string& url, 
                             const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    std::string responseBody;
    
    curl_easy_reset(pImpl->curl);
    curl_easy_setopt(pImpl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pImpl->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(pImpl->curl, CURLOPT_TIMEOUT, pImpl->timeout);
    curl_easy_setopt(pImpl->curl, CURLOPT_CONNECTTIMEOUT, pImpl->connectTimeout);
    
    // 设置请求头
    struct curl_slist* headerList = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    if (headerList) {
        curl_easy_setopt(pImpl->curl, CURLOPT_HTTPHEADER, headerList);
    }
    
    CURLcode res = curl_easy_perform(pImpl->curl);
    
    if (headerList) {
        curl_slist_free_all(headerList);
    }
    
    if (res != CURLE_OK) {
        throw HttpException(std::string("GET request failed: ") + curl_easy_strerror(res));
    }
    
    curl_easy_getinfo(pImpl->curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    response.body = std::move(responseBody);
    
    return response;
}

HttpResponse HttpClient::post(const std::string& url, 
                              const std::string& body,
                              const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    std::string responseBody;
    
    curl_easy_reset(pImpl->curl);
    curl_easy_setopt(pImpl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pImpl->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(pImpl->curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(pImpl->curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(pImpl->curl, CURLOPT_TIMEOUT, pImpl->timeout);
    curl_easy_setopt(pImpl->curl, CURLOPT_CONNECTTIMEOUT, pImpl->connectTimeout);
    
    // 设置请求头
    struct curl_slist* headerList = nullptr;
    headerList = curl_slist_append(headerList, "Content-Type: application/json");
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    curl_easy_setopt(pImpl->curl, CURLOPT_HTTPHEADER, headerList);
    
    CURLcode res = curl_easy_perform(pImpl->curl);
    curl_slist_free_all(headerList);
    
    if (res != CURLE_OK) {
        throw HttpException(std::string("POST request failed: ") + curl_easy_strerror(res));
    }
    
    curl_easy_getinfo(pImpl->curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    response.body = std::move(responseBody);
    
    return response;
}

HttpResponse HttpClient::put(const std::string& url, 
                             const std::string& body,
                             const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    std::string responseBody;
    
    curl_easy_reset(pImpl->curl);
    curl_easy_setopt(pImpl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pImpl->curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(pImpl->curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(pImpl->curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(pImpl->curl, CURLOPT_TIMEOUT, pImpl->timeout);
    curl_easy_setopt(pImpl->curl, CURLOPT_CONNECTTIMEOUT, pImpl->connectTimeout);
    
    struct curl_slist* headerList = nullptr;
    headerList = curl_slist_append(headerList, "Content-Type: application/json");
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    curl_easy_setopt(pImpl->curl, CURLOPT_HTTPHEADER, headerList);
    
    CURLcode res = curl_easy_perform(pImpl->curl);
    curl_slist_free_all(headerList);
    
    if (res != CURLE_OK) {
        throw HttpException(std::string("PUT request failed: ") + curl_easy_strerror(res));
    }
    
    curl_easy_getinfo(pImpl->curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    response.body = std::move(responseBody);
    
    return response;
}

HttpResponse HttpClient::del(const std::string& url,
                             const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    std::string responseBody;
    
    curl_easy_reset(pImpl->curl);
    curl_easy_setopt(pImpl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pImpl->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(pImpl->curl, CURLOPT_TIMEOUT, pImpl->timeout);
    curl_easy_setopt(pImpl->curl, CURLOPT_CONNECTTIMEOUT, pImpl->connectTimeout);
    
    struct curl_slist* headerList = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    if (headerList) {
        curl_easy_setopt(pImpl->curl, CURLOPT_HTTPHEADER, headerList);
    }
    
    CURLcode res = curl_easy_perform(pImpl->curl);
    
    if (headerList) {
        curl_slist_free_all(headerList);
    }
    
    if (res != CURLE_OK) {
        throw HttpException(std::string("DELETE request failed: ") + curl_easy_strerror(res));
    }
    
    curl_easy_getinfo(pImpl->curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    response.body = std::move(responseBody);
    
    return response;
}

HttpResponse HttpClient::head(const std::string& url,
                              const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    
    curl_easy_reset(pImpl->curl);
    curl_easy_setopt(pImpl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pImpl->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(pImpl->curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(pImpl->curl, CURLOPT_TIMEOUT, pImpl->timeout);
    curl_easy_setopt(pImpl->curl, CURLOPT_CONNECTTIMEOUT, pImpl->connectTimeout);
    
    struct curl_slist* headerList = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    if (headerList) {
        curl_easy_setopt(pImpl->curl, CURLOPT_HTTPHEADER, headerList);
    }
    
    CURLcode res = curl_easy_perform(pImpl->curl);
    
    if (headerList) {
        curl_slist_free_all(headerList);
    }
    
    if (res != CURLE_OK) {
        throw HttpException(std::string("HEAD request failed: ") + curl_easy_strerror(res));
    }
    
    curl_easy_getinfo(pImpl->curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    
    return response;
}

} // namespace es

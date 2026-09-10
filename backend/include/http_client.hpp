#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

#include <string>
#include <map>
#include <memory>
#include <stdexcept>

namespace es {

/**
 * HTTP 响应结构
 */
struct HttpResponse {
    int statusCode;
    std::string body;
    std::map<std::string, std::string> headers;
    
    bool isSuccess() const {
        return statusCode >= 200 && statusCode < 300;
    }
    
    bool isNotFound() const {
        return statusCode == 404;
    }
};

/**
 * HTTP 客户端异常
 */
class HttpException : public std::runtime_error {
public:
    explicit HttpException(const std::string& message) 
        : std::runtime_error(message) {}
};

/**
 * HTTP 客户端类
 * 封装 libcurl 实现 HTTP 请求
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    
    // 禁止拷贝
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    
    // 允许移动
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;
    
    /**
     * 设置请求超时时间（秒）
     */
    void setTimeout(long seconds);
    
    /**
     * 设置连接超时时间（秒）
     */
    void setConnectTimeout(long seconds);
    
    /**
     * GET 请求
     */
    HttpResponse get(const std::string& url, 
                     const std::map<std::string, std::string>& headers = {});
    
    /**
     * POST 请求
     */
    HttpResponse post(const std::string& url, 
                      const std::string& body,
                      const std::map<std::string, std::string>& headers = {});
    
    /**
     * PUT 请求
     */
    HttpResponse put(const std::string& url, 
                     const std::string& body,
                     const std::map<std::string, std::string>& headers = {});
    
    /**
     * DELETE 请求
     */
    HttpResponse del(const std::string& url,
                     const std::map<std::string, std::string>& headers = {});
    
    /**
     * HEAD 请求
     */
    HttpResponse head(const std::string& url,
                      const std::map<std::string, std::string>& headers = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace es

#endif // HTTP_CLIENT_HPP

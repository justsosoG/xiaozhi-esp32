#ifndef _WEB_SERVER_H_
#define _WEB_SERVER_H_

#include <esp_http_server.h>

// 设备本地配置页：局域网内 http://<设备IP>/ 访问。
// 只在 STA 网络连接成功后启动，断网/配网时停止。
class WebServer {
public:
    static WebServer& GetInstance();

    void Start();
    void Stop();
    bool IsRunning() const { return server_ != nullptr; }

private:
    WebServer() = default;
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    static esp_err_t IndexHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t AlarmsGetHandler(httpd_req_t* req);
    static esp_err_t AlarmsPostHandler(httpd_req_t* req);
    static esp_err_t AlarmsDeleteHandler(httpd_req_t* req);
    static esp_err_t RingtonesHandler(httpd_req_t* req);
    static esp_err_t PreviewHandler(httpd_req_t* req);
    static esp_err_t SettingsHandler(httpd_req_t* req);
    static esp_err_t RebootHandler(httpd_req_t* req);

    httpd_handle_t server_ = nullptr;
};

#endif  // _WEB_SERVER_H_

#ifndef _WEB_SERVER_H_
#define _WEB_SERVER_H_

#include <esp_http_server.h>

#include <functional>

// 底盘遥控动作。WebServer 只负责协议与参数归一化，具体执行由板卡注册回调。
enum class ChassisAction {
    Forward,
    Backward,
    TurnLeft,
    TurnRight,
    Stop,
};

// 设备本地配置页：局域网内 http://<设备IP>/ 访问。
// 只在 STA 网络连接成功后启动，断网/配网时停止。
class WebServer {
public:
    // 底盘控制回调：执行成功返回 true（可在任意任务中调用，需自行保证线程安全）。
    using ChassisControlCallback =
        std::function<bool(ChassisAction action, int speed_percent, int duration_ms)>;

    static WebServer& GetInstance();

    void Start();
    void Stop();
    bool IsRunning() const { return server_ != nullptr; }

    // 可选能力：注册后 /api/chassis 可用，/api/status 会附带底盘状态。
    // 没有底盘的板卡不注册，接口返回 501。
    void SetChassisControlCallback(ChassisControlCallback callback);

private:
    WebServer() = default;
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    static esp_err_t IndexHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t ChassisHandler(httpd_req_t* req);
    static esp_err_t AlarmsGetHandler(httpd_req_t* req);
    static esp_err_t AlarmsPostHandler(httpd_req_t* req);
    static esp_err_t AlarmsDeleteHandler(httpd_req_t* req);
    static esp_err_t RingtonesHandler(httpd_req_t* req);
    static esp_err_t PreviewHandler(httpd_req_t* req);
    static esp_err_t SettingsHandler(httpd_req_t* req);
    static esp_err_t RebootHandler(httpd_req_t* req);

    httpd_handle_t server_ = nullptr;
    ChassisControlCallback chassis_control_;
};

#endif  // _WEB_SERVER_H_
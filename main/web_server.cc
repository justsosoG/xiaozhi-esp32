#include "web_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "alarm_manager.h"
#include "application.h"
#include "board.h"
#include "device_state_machine.h"
#include "system_info.h"
#include "wifi_manager.h"

#define TAG "WebServer"

// Board identity is injected by main/CMakeLists.txt.
#ifndef BOARD_TYPE
#define BOARD_TYPE "unknown"
#endif

// Embedded single-page UI (main/assets/web/index.html).
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

namespace {

constexpr int kWebServerPort = 80;
constexpr size_t kMaxRequestBodySize = 1024;
constexpr uint32_t kMainTaskTimeoutMs = 1500;

// 底盘遥控参数范围与板卡 MotorController 的约束保持一致。
constexpr int kChassisMinSpeedPercent = 20;
constexpr int kChassisMaxSpeedPercent = 80;
constexpr int kChassisDefaultSpeedPercent = 35;
constexpr int kChassisDefaultDurationMs = 800;
constexpr int kChassisMaxDurationMs = 30000;

// 最近一次底盘动作，供 /api/status 反馈（HTTP 任务写、主任务读）。
std::atomic<int> g_chassis_action{static_cast<int>(ChassisAction::Stop)};
std::atomic<int> g_chassis_speed_percent{0};
std::atomic<int64_t> g_chassis_until_us{0};

const char* ChassisActionName(ChassisAction action) {
    switch (action) {
        case ChassisAction::Forward:
            return "forward";
        case ChassisAction::Backward:
            return "backward";
        case ChassisAction::TurnLeft:
            return "turn_left";
        case ChassisAction::TurnRight:
            return "turn_right";
        case ChassisAction::Stop:
            return "stop";
    }
    return "stop";
}

struct OpResult {
    bool ok = false;
    std::string message;
};

// Runs fn in the main task and waits for its result. fn is captured by value so
// a late execution after a timeout cannot reference destroyed stack objects.
template <typename T>
bool RunInMainTask(std::function<T()> fn, T& result) {
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    Application::GetInstance().Schedule(
        [promise, fn = std::move(fn)]() mutable { promise->set_value(fn()); });
    if (future.wait_for(std::chrono::milliseconds(kMainTaskTimeoutMs)) !=
        std::future_status::ready) {
        return false;
    }
    result = future.get();
    return true;
}

std::string JsonToString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}

esp_err_t SendJsonBody(httpd_req_t* req, const std::string& body) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t SendFailure(httpd_req_t* req, const std::string& message,
                      const char* status = "400 Bad Request") {
    httpd_resp_set_status(req, status);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message.c_str());
    return SendJsonBody(req, JsonToString(root));
}

esp_err_t SendOpResult(httpd_req_t* req, const OpResult& result) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", result.ok);
    cJSON_AddStringToObject(root, result.ok ? "message" : "error", result.message.c_str());
    return SendJsonBody(req, JsonToString(root));
}

bool ReadBody(httpd_req_t* req, std::string& body) {
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxRequestBodySize)) {
        return false;
    }
    body.resize(static_cast<size_t>(req->content_len));
    int received = 0;
    while (received < req->content_len) {
        const int ret =
            httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += ret;
    }
    return true;
}

std::string BuildStatusJson() {
    auto& board = Board::GetInstance();
    auto& wifi = WifiManager::GetInstance();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "小智");
    cJSON_AddStringToObject(root, "board", BOARD_TYPE);
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "ip", wifi.GetIpAddress().c_str());
    cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
    cJSON_AddNumberToObject(root, "rssi", wifi.GetRssi());
    cJSON_AddNumberToObject(root, "free_heap",
                            static_cast<double>(SystemInfo::GetFreeHeapSize()));
    cJSON_AddNumberToObject(root, "uptime_s",
                            static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(
        root, "state",
        DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState()));

    auto codec = board.GetAudioCodec();
    cJSON_AddNumberToObject(root, "volume", codec != nullptr ? codec->output_volume() : 0);
    auto backlight = board.GetBacklight();
    cJSON_AddNumberToObject(root, "brightness",
                            backlight != nullptr ? backlight->brightness() : 0);
    cJSON_AddStringToObject(root, "ringtone",
                            AlarmManager::GetInstance().GetRingtoneId().c_str());
    cJSON_AddNumberToObject(root, "ring_seconds",
                            AlarmManager::GetInstance().GetRingDurationSeconds());

    // 底盘状态：供遥控页显示"当前在动/静止"。没有底盘的板卡回调未注册，
    // 这里会一直报告 stop，不影响页面其它功能。
    const auto chassis_action = static_cast<ChassisAction>(g_chassis_action.load());
    const bool chassis_moving = chassis_action != ChassisAction::Stop &&
                                esp_timer_get_time() < g_chassis_until_us.load();
    cJSON* chassis = cJSON_AddObjectToObject(root, "chassis");
    cJSON_AddBoolToObject(chassis, "moving", chassis_moving);
    cJSON_AddStringToObject(chassis, "action", ChassisActionName(chassis_action));
    cJSON_AddNumberToObject(chassis, "speed_percent", g_chassis_speed_percent.load());
    return JsonToString(root);
}

std::string BuildRingtonesJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(root, "ringtones");
    for (const auto& ringtone : AlarmManager::GetRingtones()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", ringtone.id);
        cJSON_AddStringToObject(item, "name", ringtone.name);
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddStringToObject(root, "selected",
                            AlarmManager::GetInstance().GetRingtoneId().c_str());
    return JsonToString(root);
}

bool IsKnownRingtone(const std::string& id) {
    for (const auto& ringtone : AlarmManager::GetRingtones()) {
        if (id == ringtone.id) {
            return true;
        }
    }
    return false;
}

}  // namespace

WebServer& WebServer::GetInstance() {
    static WebServer instance;
    return instance;
}

void WebServer::SetChassisControlCallback(ChassisControlCallback callback) {
    chassis_control_ = std::move(callback);
}

void WebServer::Start() {
    if (server_ != nullptr) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kWebServerPort;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start web server (port %d may be in use)", kWebServerPort);
        server_ = nullptr;
        return;
    }

    static const httpd_uri_t uris[] = {
        {"/", HTTP_GET, IndexHandler, nullptr},
        {"/api/status", HTTP_GET, StatusHandler, nullptr},
        {"/api/chassis", HTTP_POST, ChassisHandler, nullptr},
        {"/api/alarms", HTTP_GET, AlarmsGetHandler, nullptr},
        {"/api/alarms", HTTP_POST, AlarmsPostHandler, nullptr},
        {"/api/alarms", HTTP_DELETE, AlarmsDeleteHandler, nullptr},
        {"/api/ringtones", HTTP_GET, RingtonesHandler, nullptr},
        {"/api/preview", HTTP_POST, PreviewHandler, nullptr},
        {"/api/settings", HTTP_POST, SettingsHandler, nullptr},
        {"/api/reboot", HTTP_POST, RebootHandler, nullptr},
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(server_, &uri);
    }
    ESP_LOGI(TAG, "Web server started on port %d", kWebServerPort);
}

void WebServer::Stop() {
    if (server_ == nullptr) {
        return;
    }
    httpd_stop(server_);
    server_ = nullptr;
    ESP_LOGI(TAG, "Web server stopped");
}

esp_err_t WebServer::IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start,
                           static_cast<ssize_t>(index_html_end - index_html_start));
}

esp_err_t WebServer::StatusHandler(httpd_req_t* req) {
    std::string json;
    if (!RunInMainTask<std::string>([]() { return BuildStatusJson(); }, json)) {
        return SendFailure(req, "设备忙，请稍后重试", "503 Service Unavailable");
    }
    return SendJsonBody(req, json);
}

esp_err_t WebServer::ChassisHandler(httpd_req_t* req) {
    auto& self = WebServer::GetInstance();
    if (!self.chassis_control_) {
        return SendFailure(req, "该设备不支持底盘遥控", "501 Not Implemented");
    }
    std::string body;
    if (!ReadBody(req, body)) {
        return SendFailure(req, "请求体无效");
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return SendFailure(req, "JSON 格式错误");
    }

    const cJSON* action_item = cJSON_GetObjectItem(root, "action");
    const cJSON* speed_item = cJSON_GetObjectItem(root, "speed_percent");
    const cJSON* duration_item = cJSON_GetObjectItem(root, "duration_ms");
    const std::string action_name =
        cJSON_IsString(action_item) ? action_item->valuestring : std::string();
    int speed_percent =
        cJSON_IsNumber(speed_item) ? speed_item->valueint : kChassisDefaultSpeedPercent;
    int duration_ms = cJSON_IsNumber(duration_item) ? duration_item->valueint : 0;
    cJSON_Delete(root);

    ChassisAction action;
    if (action_name == "stop") {
        action = ChassisAction::Stop;
    } else if (action_name == "forward") {
        action = ChassisAction::Forward;
    } else if (action_name == "backward") {
        action = ChassisAction::Backward;
    } else if (action_name == "turn_left") {
        action = ChassisAction::TurnLeft;
    } else if (action_name == "turn_right") {
        action = ChassisAction::TurnRight;
    } else {
        return SendFailure(req, "未知动作，可用：forward/backward/turn_left/turn_right/stop");
    }

    if (action == ChassisAction::Stop) {
        speed_percent = g_chassis_speed_percent.load();
        duration_ms = 0;
    } else {
        speed_percent =
            std::clamp(speed_percent, kChassisMinSpeedPercent, kChassisMaxSpeedPercent);
        duration_ms = duration_ms > 0 ? std::clamp(duration_ms, 100, kChassisMaxDurationMs)
                                      : kChassisDefaultDurationMs;
    }

    if (!self.chassis_control_(action, speed_percent, duration_ms)) {
        return SendFailure(req, "动作执行失败", "503 Service Unavailable");
    }

    g_chassis_action.store(static_cast<int>(action));
    g_chassis_speed_percent.store(speed_percent);
    g_chassis_until_us.store(action == ChassisAction::Stop
                                 ? 0
                                 : esp_timer_get_time() +
                                       static_cast<int64_t>(duration_ms) * 1000);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "action", action_name.c_str());
    cJSON_AddNumberToObject(resp, "speed_percent", speed_percent);
    cJSON_AddNumberToObject(resp, "duration_ms", duration_ms);
    cJSON_AddBoolToObject(resp, "moving", action != ChassisAction::Stop);
    return SendJsonBody(req, JsonToString(resp));
}

esp_err_t WebServer::AlarmsGetHandler(httpd_req_t* req) {
    std::string json;
    if (!RunInMainTask<std::string>(
            []() { return AlarmManager::GetInstance().GetAlarmsJson(); }, json)) {
        return SendFailure(req, "设备忙，请稍后重试", "503 Service Unavailable");
    }
    return SendJsonBody(req, json);
}

esp_err_t WebServer::AlarmsPostHandler(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body)) {
        return SendFailure(req, "请求体无效");
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return SendFailure(req, "JSON 格式错误");
    }
    const cJSON* hour = cJSON_GetObjectItem(root, "hour");
    const cJSON* minute = cJSON_GetObjectItem(root, "minute");
    const cJSON* repeat = cJSON_GetObjectItem(root, "repeat");
    const cJSON* label = cJSON_GetObjectItem(root, "label");
    const int hour_value = cJSON_IsNumber(hour) ? hour->valueint : -1;
    const int minute_value = cJSON_IsNumber(minute) ? minute->valueint : -1;
    const std::string repeat_value =
        cJSON_IsString(repeat) ? repeat->valuestring : std::string("once");
    const std::string label_value =
        cJSON_IsString(label) ? label->valuestring : std::string("闹钟");
    cJSON_Delete(root);

    OpResult result{false, "设备忙，请稍后重试"};
    RunInMainTask<OpResult>(
        [hour_value, minute_value, repeat_value, label_value]() {
            OpResult local;
            local.ok = AlarmManager::GetInstance().AddAlarm(hour_value, minute_value,
                                                            repeat_value, label_value,
                                                            local.message);
            return local;
        },
        result);
    return SendOpResult(req, result);
}

esp_err_t WebServer::AlarmsDeleteHandler(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    int index = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "index", value, sizeof(value)) == ESP_OK) {
        index = std::atoi(value);
    }
    if (index < 1) {
        return SendFailure(req, "缺少有效的 index 参数");
    }

    OpResult result{false, "设备忙，请稍后重试"};
    RunInMainTask<OpResult>(
        [index]() {
            OpResult local;
            local.ok = AlarmManager::GetInstance().RemoveAlarm(index, local.message);
            return local;
        },
        result);
    return SendOpResult(req, result);
}

esp_err_t WebServer::RingtonesHandler(httpd_req_t* req) {
    std::string json;
    if (!RunInMainTask<std::string>([]() { return BuildRingtonesJson(); }, json)) {
        return SendFailure(req, "设备忙，请稍后重试", "503 Service Unavailable");
    }
    return SendJsonBody(req, json);
}

esp_err_t WebServer::PreviewHandler(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body)) {
        return SendFailure(req, "请求体无效");
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return SendFailure(req, "JSON 格式错误");
    }
    const cJSON* id = cJSON_GetObjectItem(root, "id");
    const std::string ringtone_id =
        cJSON_IsString(id) ? id->valuestring : std::string("star");
    cJSON_Delete(root);
    if (!IsKnownRingtone(ringtone_id)) {
        return SendFailure(req, "未知铃声");
    }
    Application::GetInstance().PlayPreviewRingtone(ringtone_id);
    return SendJsonBody(req, "{\"ok\":true}");
}

esp_err_t WebServer::SettingsHandler(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body)) {
        return SendFailure(req, "请求体无效");
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return SendFailure(req, "JSON 格式错误");
    }

    struct SettingsUpdate {
        bool has_volume = false;
        int volume = 0;
        bool has_brightness = false;
        int brightness = 0;
        bool has_ringtone = false;
        std::string ringtone;
        bool has_ring_seconds = false;
        int ring_seconds = 0;
    } update;

    const cJSON* item = cJSON_GetObjectItem(root, "volume");
    if (cJSON_IsNumber(item)) {
        update.volume = item->valueint;
        if (update.volume < 0 || update.volume > 100) {
            cJSON_Delete(root);
            return SendFailure(req, "volume 必须在 0-100");
        }
        update.has_volume = true;
    }
    item = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(item)) {
        update.brightness = item->valueint;
        if (update.brightness < 0 || update.brightness > 100) {
            cJSON_Delete(root);
            return SendFailure(req, "brightness 必须在 0-100");
        }
        update.has_brightness = true;
    }
    item = cJSON_GetObjectItem(root, "ringtone");
    if (cJSON_IsString(item)) {
        update.ringtone = item->valuestring;
        if (!IsKnownRingtone(update.ringtone)) {
            cJSON_Delete(root);
            return SendFailure(req, "未知铃声");
        }
        update.has_ringtone = true;
    }
    item = cJSON_GetObjectItem(root, "ring_seconds");
    if (cJSON_IsNumber(item)) {
        update.ring_seconds = item->valueint;
        if (update.ring_seconds != 30 && update.ring_seconds != 60 &&
            update.ring_seconds != 90) {
            cJSON_Delete(root);
            return SendFailure(req, "ring_seconds 只能是 30/60/90");
        }
        update.has_ring_seconds = true;
    }
    cJSON_Delete(root);

    Application::GetInstance().Schedule([update]() {
        auto& board = Board::GetInstance();
        if (update.has_volume) {
            auto codec = board.GetAudioCodec();
            if (codec != nullptr) {
                codec->SetOutputVolume(update.volume);
            }
        }
        if (update.has_brightness) {
            auto backlight = board.GetBacklight();
            if (backlight != nullptr) {
                backlight->SetBrightness(static_cast<uint8_t>(update.brightness), true);
            }
        }
        if (update.has_ringtone) {
            AlarmManager::GetInstance().SetRingtoneId(update.ringtone);
        }
        if (update.has_ring_seconds) {
            AlarmManager::GetInstance().SetRingDurationSeconds(update.ring_seconds);
        }
    });
    return SendJsonBody(req, "{\"ok\":true}");
}

esp_err_t WebServer::RebootHandler(httpd_req_t* req) {
    const esp_err_t err = SendJsonBody(req, "{\"ok\":true}");
    if (err == ESP_OK) {
        Application::GetInstance().Schedule([]() {
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP_LOGW(TAG, "Reboot requested from the web page");
            Application::GetInstance().Reboot();
        });
    }
    return err;
}

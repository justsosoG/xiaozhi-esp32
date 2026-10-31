#include "alarm_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "mcp_server.h"
#include "settings.h"

#define TAG "AlarmManager"

namespace {

constexpr const char* kDefaultLabel = "闹钟";
// 早于该时刻说明系统时间尚未与服务器同步（2020-09-13）。
constexpr time_t kMinValidEpoch = 1600000000;

std::string TrimAndLower(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    std::string result = text.substr(begin, end - begin);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

}  // namespace

AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

void AlarmManager::Initialize() {
    Load();
}

void AlarmManager::SetRingCallbacks(std::function<void(const Alarm&)> on_start,
                                    std::function<void()> on_replay,
                                    std::function<void()> on_stop) {
    on_start_ = std::move(on_start);
    on_replay_ = std::move(on_replay);
    on_stop_ = std::move(on_stop);
}

void AlarmManager::Load() {
    alarms_.clear();
    Settings settings("alarm");
    int count = settings.GetInt("count", 0);
    count = std::clamp(count, 0, static_cast<int>(kMaxAlarms));
    for (int i = 0; i < count; ++i) {
        const std::string prefix = "a" + std::to_string(i) + "_";
        Alarm alarm;
        alarm.hour = static_cast<uint8_t>(
            std::clamp(static_cast<int>(settings.GetInt(prefix + "hour", 7)), 0, 23));
        alarm.minute = static_cast<uint8_t>(
            std::clamp(static_cast<int>(settings.GetInt(prefix + "min", 0)), 0, 59));
        alarm.repeat_mask = static_cast<uint8_t>(
            std::clamp(static_cast<int>(settings.GetInt(prefix + "repeat", 0)), 0, 0x7F));
        alarm.enabled = settings.GetBool(prefix + "en", true);
        alarm.label = settings.GetString(prefix + "label", kDefaultLabel);
        alarm.next_epoch = static_cast<time_t>(settings.GetInt(prefix + "next", 0));
        alarms_.push_back(std::move(alarm));
    }
    ESP_LOGI(TAG, "Loaded %d alarm(s)", static_cast<int>(alarms_.size()));
}

void AlarmManager::Save() {
    Settings settings("alarm", true);
    settings.SetInt("count", static_cast<int>(alarms_.size()));
    for (size_t i = 0; i < alarms_.size(); ++i) {
        const std::string prefix = "a" + std::to_string(i) + "_";
        settings.SetInt(prefix + "hour", alarms_[i].hour);
        settings.SetInt(prefix + "min", alarms_[i].minute);
        settings.SetInt(prefix + "repeat", alarms_[i].repeat_mask);
        settings.SetBool(prefix + "en", alarms_[i].enabled);
        settings.SetString(prefix + "label", alarms_[i].label);
        // 一次性闹钟的绝对触发时刻；2038 年前 int32 足够表示秒级时间戳。
        settings.SetInt(prefix + "next", static_cast<int>(alarms_[i].next_epoch));
    }
}

void AlarmManager::OnClockTick(time_t now, bool time_synced) {
    time_synced_ = time_synced;
    const int64_t now_us = esp_timer_get_time();

    if (ringing_) {
        if (now_us - ring_started_us_ >= kRingTimeoutUs) {
            ESP_LOGI(TAG, "Alarm ring timeout after 60s");
            StopRinging();
        } else if (on_replay_ != nullptr && now_us >= next_replay_us_) {
            next_replay_us_ = now_us + kReplayIntervalUs;
            on_replay_();
        }
        return;
    }

    // 稍后提醒优先于普通闹钟。
    if (snooze_epoch_ != 0 && now >= snooze_epoch_) {
        Alarm snooze_alarm;
        snooze_alarm.label = snooze_label_.empty() ? kDefaultLabel : snooze_label_;
        snooze_epoch_ = 0;
        snooze_label_.clear();
        StartRinging(snooze_alarm, now_us);
        return;
    }

    // 时间未同步时暂停触发，避免在 1970 年乱响。
    if (!time_synced_ || now < kMinValidEpoch) {
        return;
    }

    // 一次性闹钟：到点触发并删除。
    for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
        if (!it->enabled || it->repeat_mask != 0 || it->next_epoch == 0) {
            continue;
        }
        if (now >= it->next_epoch) {
            Alarm alarm = *it;
            alarms_.erase(it);
            Save();
            StartRinging(alarm, now_us);
            return;
        }
    }

    // 重复闹钟：同一分钟内每个闹钟只触发一次。
    struct tm local_now;
    localtime_r(&now, &local_now);
    const int weekday = (local_now.tm_wday + 6) % 7;  // 周一 = 0
    const time_t current_minute = now / 60;
    for (auto& alarm : alarms_) {
        if (!alarm.enabled || alarm.repeat_mask == 0) {
            continue;
        }
        if (((alarm.repeat_mask >> weekday) & 0x01) == 0) {
            continue;
        }
        if (alarm.hour != local_now.tm_hour || alarm.minute != local_now.tm_min) {
            continue;
        }
        if (alarm.last_fired_minute == current_minute) {
            continue;
        }
        alarm.last_fired_minute = current_minute;
        StartRinging(alarm, now_us);
        return;
    }
}

void AlarmManager::StartRinging(const Alarm& alarm, int64_t now_us) {
    ringing_ = true;
    ring_started_us_ = now_us;
    next_replay_us_ = now_us + kReplayIntervalUs;
    ring_label_ = alarm.label.empty() ? kDefaultLabel : alarm.label;
    ESP_LOGI(TAG, "Alarm started: %s", ring_label_.c_str());
    if (on_start_ == nullptr) {
        ESP_LOGW(TAG, "No alarm ring callback registered");
        return;
    }
    Alarm display_alarm = alarm;
    display_alarm.label = ring_label_;
    on_start_(display_alarm);
}

void AlarmManager::StopRinging() {
    if (!ringing_) {
        return;
    }
    ringing_ = false;
    ESP_LOGI(TAG, "Alarm stopped: %s", ring_label_.c_str());
    ring_label_.clear();
    if (on_stop_ != nullptr) {
        on_stop_();
    }
}

bool AlarmManager::Snooze(int minutes) {
    if (!ringing_) {
        return false;
    }
    minutes = std::clamp(minutes, 1, 60);
    const time_t now = time(nullptr);
    snooze_epoch_ = now + static_cast<time_t>(minutes) * 60;
    snooze_label_ = ring_label_.empty() ? kDefaultLabel : ring_label_;
    ringing_ = false;
    ring_label_.clear();
    ESP_LOGI(TAG, "Snooze %d minute(s), next ring at %s", minutes,
             FormatTime(snooze_epoch_).c_str());
    if (on_stop_ != nullptr) {
        on_stop_();
    }
    return true;
}

time_t AlarmManager::NextDailyOccurrence(int hour, int minute, time_t now) {
    struct tm target;
    localtime_r(&now, &target);
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;
    target.tm_isdst = -1;
    time_t candidate = mktime(&target);
    if (candidate <= now) {
        target.tm_mday += 1;
        target.tm_isdst = -1;
        candidate = mktime(&target);
    }
    return candidate;
}

time_t AlarmManager::NextRepeatOccurrence(const Alarm& alarm, time_t now) {
    struct tm target;
    localtime_r(&now, &target);
    target.tm_hour = alarm.hour;
    target.tm_min = alarm.minute;
    target.tm_sec = 0;
    target.tm_isdst = -1;
    // 最多向后找 7 天，必然命中一个重复位。
    for (int i = 0; i < 8; ++i) {
        const time_t candidate = mktime(&target);
        const int weekday = (target.tm_wday + 6) % 7;
        if (candidate > now && ((alarm.repeat_mask >> weekday) & 0x01) != 0) {
            return candidate;
        }
        target.tm_mday += 1;
        target.tm_isdst = -1;
    }
    return 0;
}

std::string AlarmManager::FormatTime(time_t t) {
    if (t <= 0) {
        return "-";
    }
    struct tm value;
    localtime_r(&t, &value);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &value);
    return buffer;
}

std::string AlarmManager::FormatHourMinute(uint8_t hour, uint8_t minute) {
    char buffer[8];
    snprintf(buffer, sizeof(buffer), "%02u:%02u", static_cast<unsigned>(hour),
             static_cast<unsigned>(minute));
    return buffer;
}

std::string AlarmManager::RepeatName(uint8_t repeat_mask) {
    if (repeat_mask == 0) {
        return "once";
    }
    if (repeat_mask == 0x7F) {
        return "daily";
    }
    if (repeat_mask == 0x1F) {
        return "weekdays";
    }
    return "custom";
}

bool AlarmManager::ParseRepeat(const std::string& text, uint8_t& repeat_mask) {
    const std::string normalized = TrimAndLower(text);
    if (normalized == "once" || normalized == "一次" || normalized == "明天" ||
        normalized == "one-shot") {
        repeat_mask = 0;
        return true;
    }
    if (normalized == "daily" || normalized == "everyday" || normalized == "every-day" ||
        normalized == "每天" || normalized == "每日") {
        repeat_mask = 0x7F;
        return true;
    }
    if (normalized == "weekdays" || normalized == "weekday" || normalized == "工作日") {
        repeat_mask = 0x1F;
        return true;
    }
    return false;
}

void AlarmManager::RegisterTools(McpServer& mcp) {
    mcp.AddTool(
        "self.alarm.set",
        "Set a local alarm clock on the device. When it rings the device plays a short sound "
        "and shows a message; it stops automatically after 60 seconds, and the user can also "
        "wake the device to interrupt it.\n"
        "Args:\n"
        "  hour: local hour, 0-23.\n"
        "  minute: 0-59.\n"
        "  repeat: 'once' (one-shot), 'daily' (every day) or 'weekdays' (Monday-Friday).\n"
        "  label: short text displayed with the alarm, for example '起床'.\n"
        "Choose 'once' when the user says e.g. 'tomorrow at 7', 'daily' for 'every day at 7', "
        "'weekdays' for 'on weekdays'.",
        PropertyList({
            Property("hour", kPropertyTypeInteger, 7, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 0, 59),
            Property("repeat", kPropertyTypeString, std::string("once")),
            Property("label", kPropertyTypeString, std::string("闹钟")),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!time_synced_) {
                return std::string("错误：设备时间尚未同步（未联网），暂时无法设置闹钟。");
            }
            Alarm alarm;
            alarm.hour = static_cast<uint8_t>(properties["hour"].value<int>());
            alarm.minute = static_cast<uint8_t>(properties["minute"].value<int>());
            if (!ParseRepeat(properties["repeat"].value<std::string>(), alarm.repeat_mask)) {
                return std::string("错误：repeat 只能是 once、daily 或 weekdays。");
            }
            alarm.label = properties["label"].value<std::string>();
            if (alarm.label.empty()) {
                alarm.label = kDefaultLabel;
            }
            if (alarms_.size() >= kMaxAlarms) {
                return std::string("错误：闹钟数量已达上限（8 个），请先取消一个。");
            }
            const time_t now = time(nullptr);
            if (alarm.repeat_mask == 0) {
                alarm.next_epoch = NextDailyOccurrence(alarm.hour, alarm.minute, now);
            }
            alarms_.push_back(alarm);
            Save();
            const time_t next = alarm.repeat_mask == 0 ? alarm.next_epoch
                                                       : NextRepeatOccurrence(alarm, now);
            return std::string("已设置闹钟：") + RepeatName(alarm.repeat_mask) + " " +
                   FormatHourMinute(alarm.hour, alarm.minute) + "（" + alarm.label +
                   "），下次响铃 " + FormatTime(next) + "。";
        });

    mcp.AddTool("self.alarm.list",
                "List all alarms stored on the device, including their index, time, repeat rule, "
                "label and next ring time. Use the index with self.alarm.cancel.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue {
                    const time_t now = time(nullptr);
                    cJSON* root = cJSON_CreateArray();
                    for (size_t i = 0; i < alarms_.size(); ++i) {
                        const Alarm& alarm = alarms_[i];
                        const time_t next = alarm.repeat_mask == 0
                                                ? alarm.next_epoch
                                                : NextRepeatOccurrence(alarm, now);
                        cJSON* item = cJSON_CreateObject();
                        cJSON_AddNumberToObject(item, "index", static_cast<double>(i + 1));
                        cJSON_AddStringToObject(
                            item, "time", FormatHourMinute(alarm.hour, alarm.minute).c_str());
                        cJSON_AddStringToObject(item, "repeat",
                                                RepeatName(alarm.repeat_mask).c_str());
                        cJSON_AddStringToObject(item, "label", alarm.label.c_str());
                        cJSON_AddStringToObject(item, "next", FormatTime(next).c_str());
                        cJSON_AddItemToArray(root, item);
                    }
                    return root;
                });

    mcp.AddTool("self.alarm.cancel",
                "Cancel one alarm by its index (1-based, from self.alarm.list) or cancel all "
                "alarms.",
                PropertyList({
                    Property("index", kPropertyTypeInteger, 0, 0, static_cast<int>(kMaxAlarms)),
                    Property("all", kPropertyTypeBoolean, false),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    if (properties["all"].value<bool>()) {
                        alarms_.clear();
                        Save();
                        return std::string("已取消全部闹钟。");
                    }
                    const int index = properties["index"].value<int>();
                    if (index < 1 || static_cast<size_t>(index) > alarms_.size()) {
                        return std::string("错误：没有第 ") + std::to_string(index) + " 个闹钟。";
                    }
                    alarms_.erase(alarms_.begin() + (index - 1));
                    Save();
                    return std::string("已取消第 ") + std::to_string(index) + " 个闹钟。";
                });

    mcp.AddTool("self.alarm.stop", "Stop the alarm that is currently ringing.", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    if (!ringing_) {
                        return std::string("当前没有闹钟在响。");
                    }
                    StopRinging();
                    return std::string("已停止闹钟。");
                });

    mcp.AddTool("self.alarm.snooze",
                "Snooze the alarm that is currently ringing. It will ring again after the given "
                "number of minutes.",
                PropertyList({
                    Property("minutes", kPropertyTypeInteger, 5, 1, 60),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    const int minutes = properties["minutes"].value<int>();
                    if (!Snooze(minutes)) {
                        return std::string("当前没有闹钟在响，无法稍后提醒。");
                    }
                    return std::string("已稍后提醒：") + std::to_string(minutes) +
                           " 分钟后再次响铃。";
                });
}

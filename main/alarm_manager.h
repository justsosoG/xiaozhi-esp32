#ifndef _ALARM_MANAGER_H_
#define _ALARM_MANAGER_H_

#include <ctime>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class McpServer;

// 本地闹钟数据模型。
// repeat_mask 的 bit0..bit6 分别对应周一..周日；为 0 表示一次性闹钟。
struct Alarm {
    uint8_t hour = 0;          // 0-23，本地时间
    uint8_t minute = 0;        // 0-59
    uint8_t repeat_mask = 0;   // 0 = 一次性；0x7F = 每天；0x1F = 工作日
    bool enabled = true;
    std::string label = "闹钟";
    time_t next_epoch = 0;     // 仅一次性闹钟使用：绝对触发时刻（本地时间戳）
    time_t last_fired_minute = 0;  // 运行期去重，不持久化
};

// 可选的闹钟铃声（资源在 main/assets/common/alarm_*.ogg）。
struct RingtoneInfo {
    const char* id;
    const char* name;
};

// 闹钟管理器：负责闹钟的增删查、NVS 持久化、每秒钟的触发检查和响铃生命周期。
// 所有方法都在主任务中被调用（MCP 工具回调、本地配置页的请求也经 Application::Schedule
// 投递到主任务），因此无需额外加锁。
class AlarmManager {
public:
    static AlarmManager& GetInstance();

    // 从 NVS 加载闹钟与配置，在 Application::Initialize() 中调用一次。
    void Initialize();

    // 注册设备端 MCP 工具（self.alarm.set/list/cancel/stop/snooze）。
    void RegisterTools(McpServer& mcp);

    // 主任务每秒调用一次。time_synced 为 false 时暂停触发（时间不可信）。
    void OnClockTick(time_t now, bool time_synced);

    // 响铃动作由 Application 注入，回调都在主任务执行。
    // on_start: 进入响铃（切状态、显示、播放第一遍声音）
    // on_replay: 响铃期间每隔几秒重播一次声音
    // on_stop: 停止响铃（清理显示、回到 idle）
    void SetRingCallbacks(std::function<void(const Alarm&)> on_start,
                          std::function<void()> on_replay, std::function<void()> on_stop);

    bool IsRinging() const { return ringing_; }

    // 停止当前响铃（唤醒词打断、self.alarm.stop、超时都会走到这里）。
    void StopRinging();

    // ---- 本地配置页（main/web_server.cc）使用的接口，必须在主任务调用 ----
    static const std::vector<RingtoneInfo>& GetRingtones();
    std::string GetRingtoneId() const { return ringtone_id_; }
    bool SetRingtoneId(const std::string& id);
    int GetRingDurationSeconds() const { return ring_duration_s_; }
    bool SetRingDurationSeconds(int seconds);
    std::string GetAlarmsJson() const;
    bool AddAlarm(int hour, int minute, const std::string& repeat, const std::string& label,
                  std::string& message);
    bool RemoveAlarm(int index, std::string& message);

private:
    AlarmManager() = default;
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    static constexpr size_t kMaxAlarms = 8;
    static constexpr int64_t kReplayIntervalUs = 3LL * 1000 * 1000;  // 每 3 秒重播一次

    void Load();
    void Save();
    void StartRinging(const Alarm& alarm, int64_t now_us);
    bool Snooze(int minutes);

    static time_t NextDailyOccurrence(int hour, int minute, time_t now);
    static time_t NextRepeatOccurrence(const Alarm& alarm, time_t now);
    static std::string FormatTime(time_t t);
    static std::string FormatHourMinute(uint8_t hour, uint8_t minute);
    static std::string RepeatName(uint8_t repeat_mask);
    static bool ParseRepeat(const std::string& text, uint8_t& repeat_mask);
    static const RingtoneInfo* FindRingtone(const std::string& id);

    std::vector<Alarm> alarms_;
    bool time_synced_ = false;
    bool ringing_ = false;
    int64_t ring_started_us_ = 0;
    int64_t next_replay_us_ = 0;
    std::string ring_label_;
    std::string ringtone_id_ = "star";
    int ring_duration_s_ = 60;
    time_t snooze_epoch_ = 0;
    std::string snooze_label_;

    std::function<void(const Alarm&)> on_start_;
    std::function<void()> on_replay_;
    std::function<void()> on_stop_;
};

#endif  // _ALARM_MANAGER_H_

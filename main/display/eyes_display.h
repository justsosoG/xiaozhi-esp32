#ifndef _EYES_DISPLAY_H_
#define _EYES_DISPLAY_H_

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <cstdint>

// 128x64 单色 OLED 上的"两只眼睛"动画界面：
// - 空闲时随机眨眼、视线游走，像活着一样
// - SetEmotion 切换表情：每种情绪有独立的眼型、眨眼节奏和微动作
//   （开心眯眼跳动、大笑抖动、难过流泪、生气颤抖、惊讶睁大、困倦、
//     思考、眨眼单眼、爱心、亲亲、酷、做鬼脸、疑惑、害羞、好吃、放松、自信）
// - 聆听时注视前方，说话时轻微上下浮动，响铃时睁大
// - 底部保留一行文字，用于状态/对话字幕
class EyesDisplay : public LvglDisplay {
public:
    EyesDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~EyesDisplay();

    void SetupUI() override;
    bool SetEmotion(const char* emotion) override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetChatMessage(const char* role, const char* content) override;
    void RemoveChatMessage(const char* role, const char* content) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetTheme(Theme* theme) override;
    void SetPowerSaveMode(bool on) override;
    bool IsMonochrome() const override { return true; }
    bool SupportsGuiOperations() const override { return false; }

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    // 顺序必须与 eyes_display.cc 里的表情参数表一致。
    enum class Expression {
        Neutral,
        Happy,
        Laughing,
        Sad,
        Crying,
        Angry,
        Surprised,
        Shocked,
        Sleepy,
        Thinking,
        Winking,
        Loving,
        Kissy,
        Cool,
        Silly,
        Confused,
        Embarrassed,
        Delicious,
        Relaxed,
        Confident,
    };

    // 一种表情的静态参数：眼型、偏移、视线、浮动/抖动节奏、眨眼间隔等。
    struct ExpressionProfile {
        float left_h;        // 左眼基础高度（px）
        float right_h;       // 右眼基础高度（px）
        float left_y_off;    // 左眼垂直偏移（px）
        float right_y_off;   // 右眼垂直偏移（px）
        float inset;         // >0 时双眼向中间靠拢（px）
        float gaze_x;        // 视线水平偏置（px）
        float gaze_y;        // 视线垂直偏置（px）
        float bob_amp;       // 上下浮动幅度（px）
        float bob_freq;      // 上下浮动相位增量（rad/帧）
        float shake_amp;     // 左右抖动幅度（px）
        float shake_freq;    // 左右抖动相位增量（rad/帧）
        uint32_t blink_ms;   // 自动眨眼基础间隔（ms），0 表示不自动眨眼
        bool tear;           // 是否显示眼泪
        bool tear_continuous;// 眼泪是否连续流（否则滴一下停一会）
        bool right_closed;   // 右眼保持闭合（winking）
    };

    static const ExpressionProfile& ProfileFor(Expression expression);

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* tear_ = nullptr;
    lv_obj_t* text_label_ = nullptr;
    lv_timer_t* anim_timer_ = nullptr;

    Expression expression_ = Expression::Neutral;
    uint32_t text_hide_at_ms_ = 0;

    // 平滑后的眼型（向当前表情的目标值插值，实现自然过渡）。
    float left_h_ = 30.0f;
    float right_h_ = 30.0f;
    float left_y_off_ = 0.0f;
    float right_y_off_ = 0.0f;
    float left_open_ = 1.0f;
    float right_open_ = 1.0f;

    // 视线。
    float gaze_x_ = 0.0f;
    float gaze_y_ = 0.0f;
    float target_gaze_x_ = 0.0f;
    float target_gaze_y_ = 0.0f;

    // 呼吸/抖动/说话节奏。
    float bob_phase_ = 0.0f;
    float bob_value_ = 0.0f;
    float shake_phase_ = 0.0f;
    float shake_value_ = 0.0f;
    float speak_phase_ = 0.0f;
    float speak_bob_ = 0.0f;

    // 眼泪动画进度 0..1。
    float tear_progress_ = 0.0f;

    // 眨眼状态。
    bool blinking_ = false;
    uint32_t blink_started_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t next_gaze_ms_ = 0;

    // 眼睛几何。
    int eye_width_ = 28;
    int eye_center_y_ = 20;
    int left_center_x_ = 46;
    int right_center_x_ = 82;

    void SetupEyesUi();
    static void OnAnimTimer(lv_timer_t* timer);
    void Tick();
    void ApplyEyes();
    void UpdateTear(bool visible, bool continuous);
    void OnExpressionChanged();
    void ScheduleNextBlink(uint32_t now);
    void ScheduleNextGaze(uint32_t now);
    void ShowText(const char* text, int duration_ms);
};

#endif  // _EYES_DISPLAY_H_
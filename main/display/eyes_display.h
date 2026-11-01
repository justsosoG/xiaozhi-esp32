#ifndef _EYES_DISPLAY_H_
#define _EYES_DISPLAY_H_

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

// 128x64 单色 OLED 上的"两只眼睛"动画界面：
// - 空闲时随机眨眼、视线游走，像活着一样
// - SetEmotion 切换眼神（开心眯眼/难过/生气/惊讶/困倦/思考）
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
    enum class Mood {
        Neutral,
        Happy,
        Sad,
        Angry,
        Surprised,
        Sleepy,
        Thinking,
    };

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* text_label_ = nullptr;
    lv_timer_t* anim_timer_ = nullptr;

    Mood mood_ = Mood::Neutral;
    uint32_t text_hide_at_ms_ = 0;

    // Animation state.
    float gaze_x_ = 0.0f;
    float gaze_y_ = 0.0f;
    float target_gaze_x_ = 0.0f;
    float target_gaze_y_ = 0.0f;
    float open_ratio_ = 1.0f;
    float bob_offset_ = 0.0f;
    float bob_phase_ = 0.0f;
    bool blinking_ = false;
    uint32_t blink_started_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t next_gaze_ms_ = 0;

    int eye_width_ = 28;
    int eye_center_y_ = 20;
    int left_center_x_ = 46;
    int right_center_x_ = 82;
    int eye_base_height_ = 30;

    void SetupEyesUi();
    static void OnAnimTimer(lv_timer_t* timer);
    void Tick();
    void ApplyEyes();
    void ScheduleNextBlink(uint32_t now);
    void ScheduleNextGaze(uint32_t now);
    void ShowText(const char* text, int duration_ms);
    int CurrentEyeHeight() const;
};

#endif  // _EYES_DISPLAY_H_

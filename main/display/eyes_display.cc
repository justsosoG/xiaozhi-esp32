#include "eyes_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>

#define TAG "EyesDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

#ifdef HAS_OLED_CJK_FONT
// 内嵌的 14px/1bpp 常用中文点阵字体（由 main/CMakeLists.txt EMBED_FILES 打包）。
extern const char font_noto_sans_common_14_1_bin_start[]
    asm("_binary_font_noto_sans_common_14_1_bin_start");
#endif

namespace {
// 和 OledDisplay 保持一致：前景色用黑色，保证在现有面板/颜色映射下可见。
inline lv_color_t ForegroundColor() { return lv_color_black(); }

constexpr uint32_t kBlinkDurationMs = 160;
constexpr uint32_t kFrameIntervalMs = 33;
}  // namespace

EyesDisplay::EyesDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

#ifdef HAS_OLED_CJK_FONT
    // cbin 字体从 LVGL 堆分配，必须在 lvgl_port_init() 之后创建。
    auto cjk_font = std::make_shared<LvglCBinFont>(
        const_cast<char*>(font_noto_sans_common_14_1_bin_start));
    if (cjk_font->font() != nullptr) {
        dark_theme->set_text_font(cjk_font);
    } else {
        ESP_LOGW(TAG, "Embedded Chinese font is invalid, falling back to the built-in font");
    }
#endif

    ESP_LOGI(TAG, "Adding eyes display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
    }
}

EyesDisplay::~EyesDisplay() {
    if (anim_timer_ != nullptr) {
        lv_timer_del(anim_timer_);
        anim_timer_ = nullptr;
    }
    left_eye_ = nullptr;
    right_eye_ = nullptr;
    text_label_ = nullptr;
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool EyesDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void EyesDisplay::Unlock() { lvgl_port_unlock(); }

void EyesDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    Display::SetupUI();
    DisplayLockGuard lock(this);
    SetupEyesUi();
}

void EyesDisplay::SetupEyesUi() {
    auto screen = lv_screen_active();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (theme != nullptr && theme->text_font() != nullptr) {
        lv_obj_set_style_text_font(screen, theme->text_font()->font(), 0);
    }
    lv_obj_set_style_text_color(screen, ForegroundColor(), 0);

    left_eye_ = lv_obj_create(screen);
    right_eye_ = lv_obj_create(screen);
    for (auto* eye : {left_eye_, right_eye_}) {
        lv_obj_remove_style_all(eye);
        lv_obj_set_style_bg_color(eye, ForegroundColor(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, eye_width_ / 2, 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    }

    text_label_ = lv_label_create(screen);
    lv_obj_set_width(text_label_, width_);
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(text_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(text_label_, ForegroundColor(), 0);
    lv_obj_align(text_label_, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_label_set_text(text_label_, "");

    const uint32_t now = lv_tick_get();
    next_blink_ms_ = now + 1500 + (esp_random() % 2500);
    next_gaze_ms_ = now + 1000 + (esp_random() % 2000);
    ApplyEyes();
    anim_timer_ = lv_timer_create(OnAnimTimer, kFrameIntervalMs, this);
}

void EyesDisplay::OnAnimTimer(lv_timer_t* timer) {
    auto* self = static_cast<EyesDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->Tick();
    }
}

void EyesDisplay::ScheduleNextBlink(uint32_t now) {
    next_blink_ms_ = now + 2000 + (esp_random() % 4000);
}

void EyesDisplay::ScheduleNextGaze(uint32_t now) {
    next_gaze_ms_ = now + 1200 + (esp_random() % 2600);
}

int EyesDisplay::CurrentEyeHeight() const {
    const int height = static_cast<int>(eye_base_height_ * open_ratio_);
    return std::max(3, height);
}

void EyesDisplay::ApplyEyes() {
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }
    const int h = CurrentEyeHeight();
    const int w = eye_width_;
    const int radius = std::max(2, std::min(w, h) / 2);
    const int y = eye_center_y_ - h / 2 + static_cast<int>(gaze_y_ + bob_offset_);
    const int x_shift = static_cast<int>(gaze_x_);
    const int left_x = left_center_x_ - w / 2 + x_shift;
    const int right_x = right_center_x_ - w / 2 + x_shift;

    lv_obj_set_pos(left_eye_, left_x, y);
    lv_obj_set_size(left_eye_, w, h);
    lv_obj_set_style_radius(left_eye_, radius, 0);
    lv_obj_set_pos(right_eye_, right_x, y);
    lv_obj_set_size(right_eye_, w, h);
    lv_obj_set_style_radius(right_eye_, radius, 0);
}

void EyesDisplay::Tick() {
    const uint32_t now = lv_tick_get();
    const auto state = Application::GetInstance().GetDeviceState();

    // 说话时眼睛轻微上下浮动，像是在回应。
    if (state == kDeviceStateSpeaking) {
        bob_phase_ += 0.22f;
        bob_offset_ = std::sin(bob_phase_) * 1.5f;
    } else {
        bob_offset_ *= 0.85f;
    }

    // 眨眼：闭眼 -> 保持 -> 睁开，偶尔连眨两下。
    if (!blinking_ && now >= next_blink_ms_) {
        blinking_ = true;
        blink_started_ms_ = now;
    }
    if (blinking_) {
        const uint32_t elapsed = now - blink_started_ms_;
        const float progress = static_cast<float>(elapsed) / kBlinkDurationMs;
        if (progress < 0.35f) {
            open_ratio_ = 1.0f - (progress / 0.35f) * 0.95f;
        } else if (progress < 0.55f) {
            open_ratio_ = 0.05f;
        } else if (progress < 1.0f) {
            open_ratio_ = 0.05f + ((progress - 0.55f) / 0.45f) * 0.95f;
        } else {
            blinking_ = false;
            open_ratio_ = 1.0f;
            if ((esp_random() % 100) < 25) {
                next_blink_ms_ = now + 220;
            } else {
                ScheduleNextBlink(now);
            }
        }
    } else if (open_ratio_ < 1.0f) {
        open_ratio_ = 1.0f;
    }

    // 视线：聆听时注视前方，说话时左右轻摆，其余时间随机游走。
    if (state == kDeviceStateListening) {
        target_gaze_x_ = 0.0f;
        target_gaze_y_ = 0.0f;
    } else if (mood_ == Mood::Sad) {
        target_gaze_x_ = 0.0f;
        target_gaze_y_ = 3.0f;
    } else if (mood_ == Mood::Thinking) {
        target_gaze_x_ = 6.0f;
        target_gaze_y_ = -3.0f;
    } else if (state == kDeviceStateSpeaking) {
        target_gaze_x_ = std::sin(bob_phase_ * 0.7f) * 2.0f;
        target_gaze_y_ = 0.0f;
    } else if (now >= next_gaze_ms_) {
        constexpr int kRangeX = 6;
        constexpr int kRangeY = 3;
        target_gaze_x_ = static_cast<float>(
            static_cast<int>(esp_random() % (kRangeX * 2 + 1)) - kRangeX);
        target_gaze_y_ = static_cast<float>(
            static_cast<int>(esp_random() % (kRangeY * 2 + 1)) - kRangeY);
        ScheduleNextGaze(now);
    }
    gaze_x_ += (target_gaze_x_ - gaze_x_) * 0.18f;
    gaze_y_ += (target_gaze_y_ - gaze_y_) * 0.18f;

    // 字幕自动隐藏。
    if (text_hide_at_ms_ != 0 && now >= text_hide_at_ms_) {
        text_hide_at_ms_ = 0;
        if (text_label_ != nullptr) {
            lv_label_set_text(text_label_, "");
        }
    }

    ApplyEyes();
}

void EyesDisplay::ShowText(const char* text, int duration_ms) {
    if (text_label_ == nullptr) {
        return;
    }
    if (text == nullptr || text[0] == '\0') {
        lv_label_set_text(text_label_, "");
        text_hide_at_ms_ = 0;
        return;
    }
    std::string content = text;
    std::replace(content.begin(), content.end(), '\n', ' ');
    lv_label_set_text(text_label_, content.c_str());
    text_hide_at_ms_ = duration_ms > 0 ? lv_tick_get() + duration_ms : 0;
}

bool EyesDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);

    const std::string name = emotion;
    if (name == "happy" || name == "funny" || name == "laughing" || name == "silly" ||
        name == "loving" || name == "kissy" || name == "winking" || name == "delicious") {
        mood_ = Mood::Happy;
        eye_base_height_ = 14;
        eye_center_y_ = 18;
    } else if (name == "sad" || name == "crying" || name == "embarrassed") {
        mood_ = Mood::Sad;
        eye_base_height_ = 20;
        eye_center_y_ = 22;
    } else if (name == "angry") {
        mood_ = Mood::Angry;
        eye_base_height_ = 16;
        eye_center_y_ = 22;
    } else if (name == "surprised" || name == "shocked") {
        mood_ = Mood::Surprised;
        eye_base_height_ = 38;
        eye_center_y_ = 20;
    } else if (name == "sleepy" || name == "relaxed") {
        mood_ = Mood::Sleepy;
        eye_base_height_ = 5;
        eye_center_y_ = 20;
    } else if (name == "thinking" || name == "confused") {
        mood_ = Mood::Thinking;
        eye_base_height_ = 20;
        eye_center_y_ = 21;
    } else if (name == "neutral" || name == "robot_2" || name == "cool" ||
               name == "confident") {
        mood_ = Mood::Neutral;
        eye_base_height_ = 30;
        eye_center_y_ = 20;
    } else {
        // 未知情绪由上层回退到 neutral。
        return false;
    }
    return true;
}

void EyesDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    ShowText(status, 0);
}

void EyesDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    ShowText(notification, duration_ms);
}

void EyesDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void EyesDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    ShowText(content, 6000);
}

void EyesDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (text_label_ == nullptr || content == nullptr) {
        return;
    }
    if (std::string(content) == lv_label_get_text(text_label_)) {
        lv_label_set_text(text_label_, "");
        text_hide_at_ms_ = 0;
    }
}

void EyesDisplay::UpdateStatusBar(bool update_all) {
    // 单色 OLED 只保留眼睛和一行文字，不画状态栏图标。
    (void)update_all;
}

void EyesDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    Display::SetTheme(theme);
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    if (lvgl_theme != nullptr && lvgl_theme->text_font() != nullptr && text_label_ != nullptr) {
        lv_obj_set_style_text_font(text_label_, lvgl_theme->text_font()->font(), 0);
    }
}

void EyesDisplay::SetPowerSaveMode(bool on) {
    if (panel_ != nullptr) {
        Settings settings("wifi", false);
        if (settings.GetBool("power_save_display_off", false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}

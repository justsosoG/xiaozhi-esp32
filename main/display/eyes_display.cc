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
constexpr int kMinEyeHeight = 3;
// 眼睛最多占到 y=34，把下面留给嘴巴。
constexpr int kMaxEyeHeight = 32;
constexpr int kEyeBottomLimit = 34;
// 嘴巴不能压到底部文字行（文字约从 y=49 开始）。
constexpr int kMouthBottomLimit = 48;
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
    tear_ = nullptr;
    mouth_bar_ = nullptr;
    mouth_line_ = nullptr;
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

const EyesDisplay::ExpressionProfile& EyesDisplay::ProfileFor(Expression expression) {
    // 与 header 里的 Expression 顺序一一对应：
    // Neutral, Happy, Laughing, Sad, Crying, Angry, Surprised, Shocked, Sleepy,
    // Thinking, Winking, Loving, Kissy, Cool, Silly, Confused, Embarrassed,
    // Delicious, Relaxed, Confident
    //
    // 字段顺序：left_h, right_h, left_y, right_y, inset, gaze_x, gaze_y,
    //          bob_amp, bob_freq, shake_amp, shake_freq, blink_ms,
    //          tear, tear_cont, right_closed,
    //          mouth, mouth_w, mouth_h, mouth_dx, mouth_dy
    static const ExpressionProfile kProfiles[] = {
        /* Neutral     */ {28, 28, 0, 0, 0, 0, 0, 0.8f, 0.05f, 0.0f, 0.0f, 3000, false, false,
                           false, MouthShape::Bar, 10, 3, 0, 0},
        /* Happy       */ {14, 14, -2, -2, 0, 0, 0, 2.2f, 0.22f, 0.0f, 0.0f, 2600, false, false,
                           false, MouthShape::Smile, 18, 6, 0, 0},
        /* Laughing    */ {6, 6, 0, 0, 0, 0, -1, 1.2f, 0.35f, 1.0f, 0.8f, 0, false, false, false,
                           MouthShape::Oval, 16, 12, 0, 0},
        /* Sad         */ {18, 18, 3, 3, 0, 0, 3, 0.4f, 0.05f, 0.0f, 0.0f, 5000, true, false, false,
                           MouthShape::Frown, 14, 5, 0, 0},
        /* Crying      */ {16, 16, 3, 3, 0, 0, 3, 0.5f, 0.06f, 0.4f, 0.2f, 6000, true, true, false,
                           MouthShape::Frown, 14, 6, 0, 0},
        /* Angry       */ {14, 14, 2, 2, 0, 0, 1, 0.3f, 0.06f, 1.3f, 0.9f, 4000, false, false, false,
                           MouthShape::Bar, 12, 3, 0, 1},
        /* Surprised   */ {28, 28, 0, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 3600, false, false, false,
                           MouthShape::Oval, 8, 10, 0, 0},
        /* Shocked     */ {30, 30, -1, -1, 0, 0, 0, 0.0f, 0.0f, 0.8f, 1.2f, 0, false, false, false,
                           MouthShape::Oval, 10, 12, 0, 0},
        /* Sleepy      */ {8, 8, 0, 0, 0, 0, 1, 0.3f, 0.04f, 0.0f, 0.0f, 2400, false, false, false,
                           MouthShape::Dot, 6, 3, 0, 0},
        /* Thinking    */ {20, 20, -1, -1, 0, 6, -3, 0.5f, 0.05f, 0.0f, 0.0f, 3600, false, false,
                           false, MouthShape::Bar, 8, 3, 4, 0},
        /* Winking     */ {28, 28, 0, 0, 0, 0, 0, 0.8f, 0.05f, 0.0f, 0.0f, 3600, false, false, true,
                           MouthShape::Smile, 16, 5, 0, 0},
        /* Loving      */ {14, 14, -3, -3, 4, 0, 0, 1.5f, 0.15f, 0.0f, 0.0f, 4200, false, false,
                           false, MouthShape::Smile, 14, 4, 0, 0},
        /* Kissy       */ {12, 12, -2, -2, 0, 0, 0, 2.5f, 0.28f, 0.0f, 0.0f, 3000, false, false,
                           false, MouthShape::Oval, 6, 6, 0, 0},
        /* Cool        */ {16, 16, 1, 1, 0, 0, 0, 0.4f, 0.04f, 0.0f, 0.0f, 5200, false, false, false,
                           MouthShape::Bar, 14, 3, 2, 0},
        /* Silly       */ {26, 10, 0, 0, 0, 0, 0, 1.0f, 0.14f, 0.8f, 0.7f, 3000, false, false, false,
                           MouthShape::Wave, 16, 4, 0, 0},
        /* Confused    */ {22, 22, -3, 3, 0, 4, -2, 0.5f, 0.05f, 0.0f, 0.0f, 3600, false, false,
                           false, MouthShape::Wave, 12, 3, 0, 0},
        /* Embarrassed */ {16, 16, 2, 2, 2, 0, 3, 0.3f, 0.04f, 0.0f, 0.0f, 4500, false, false, false,
                           MouthShape::Dot, 6, 3, 0, 0},
        /* Delicious   */ {14, 14, -1, -1, 0, 0, 0, 2.0f, 0.30f, 0.0f, 0.0f, 3000, false, false,
                           false, MouthShape::Smile, 14, 5, 0, 0},
        /* Relaxed     */ {12, 12, 0, 0, 0, 0, 0, 0.5f, 0.05f, 0.0f, 0.0f, 4500, false, false, false,
                           MouthShape::Bar, 10, 3, 0, 0},
        /* Confident   */ {22, 22, -1, -1, 0, 0, 0, 0.5f, 0.05f, 0.0f, 0.0f, 4500, false, false,
                           false, MouthShape::Bar, 12, 3, 1, 0},
    };
    const int index = static_cast<int>(expression);
    const int count = static_cast<int>(sizeof(kProfiles) / sizeof(kProfiles[0]));
    if (index < 0 || index >= count) {
        return kProfiles[0];
    }
    return kProfiles[index];
}

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
    tear_ = lv_obj_create(screen);
    mouth_bar_ = lv_obj_create(screen);
    for (auto* shape : {left_eye_, right_eye_, tear_, mouth_bar_}) {
        lv_obj_remove_style_all(shape);
        lv_obj_set_style_bg_color(shape, ForegroundColor(), 0);
        lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(shape, eye_width_ / 2, 0);
        lv_obj_clear_flag(shape, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth_bar_, LV_OBJ_FLAG_HIDDEN);

    mouth_line_ = lv_line_create(screen);
    lv_obj_set_style_line_width(mouth_line_, 3, 0);
    lv_obj_set_style_line_color(mouth_line_, ForegroundColor(), 0);
    lv_obj_set_style_line_rounded(mouth_line_, true, 0);
    lv_obj_clear_flag(mouth_line_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mouth_line_, LV_OBJ_FLAG_HIDDEN);

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
    const auto& profile = ProfileFor(expression_);
    left_h_ = profile.left_h;
    right_h_ = profile.right_h;
    left_y_off_ = profile.left_y_off;
    right_y_off_ = profile.right_y_off;
    mouth_w_ = profile.mouth_w;
    mouth_h_ = profile.mouth_h;
    ApplyEyes();
    ApplyMouth();
    anim_timer_ = lv_timer_create(OnAnimTimer, kFrameIntervalMs, this);
}

void EyesDisplay::OnAnimTimer(lv_timer_t* timer) {
    auto* self = static_cast<EyesDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->Tick();
    }
}

void EyesDisplay::OnExpressionChanged() {
    const auto& profile = ProfileFor(expression_);
    const uint32_t now = lv_tick_get();
    blinking_ = false;
    left_open_ = 1.0f;
    right_open_ = 1.0f;
    next_blink_ms_ = now + 600 + (esp_random() % 1600);
    next_gaze_ms_ = now + 400 + (esp_random() % 1200);
    target_gaze_x_ = profile.gaze_x;
    target_gaze_y_ = profile.gaze_y;
    tear_progress_ = 0.0f;
}

void EyesDisplay::ScheduleNextBlink(uint32_t now) {
    const auto& profile = ProfileFor(expression_);
    if (profile.blink_ms == 0) {
        return;
    }
    next_blink_ms_ = now + profile.blink_ms / 2 + (esp_random() % (profile.blink_ms + 1));
}

void EyesDisplay::ScheduleNextGaze(uint32_t now) {
    next_gaze_ms_ = now + 1200 + (esp_random() % 2600);
}

void EyesDisplay::UpdateTear(bool visible, bool continuous) {
    if (tear_ == nullptr) {
        return;
    }
    if (!visible) {
        lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);
        tear_progress_ = 0.0f;
        return;
    }
    tear_progress_ += 0.018f;
    if (tear_progress_ >= 1.0f) {
        tear_progress_ = 0.0f;
    }
    // 难过时是"滴一下、停一会"，大哭时连续流。
    if (!continuous && tear_progress_ > 0.55f) {
        lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const int w = 3;
    const int h = 6;
    // 眼泪挂在右眼外侧的脸颊上往下流，避开嘴巴。
    const int x = right_center_x_ + eye_width_ / 2 + 2;
    const int y = eye_center_y_ + 6 + static_cast<int>(tear_progress_ * 20.0f);
    lv_obj_remove_flag(tear_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(tear_, std::clamp(x, 0, width_ - w), std::clamp(y, 0, kEyeBottomLimit - h));
    lv_obj_set_size(tear_, w, h);
    lv_obj_set_style_radius(tear_, 2, 0);
}

void EyesDisplay::ApplyEyes() {
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }
    const auto& profile = ProfileFor(expression_);
    const int w = eye_width_;
    const int left_h = std::clamp(static_cast<int>(left_h_ * left_open_), kMinEyeHeight,
                                  kMaxEyeHeight);
    const int right_h = std::clamp(static_cast<int>(right_h_ * right_open_), kMinEyeHeight,
                                   kMaxEyeHeight);
    const int gy = static_cast<int>(gaze_y_ + bob_value_ + speak_bob_);
    const int gx = static_cast<int>(gaze_x_ + shake_value_);
    const int inset = static_cast<int>(profile.inset);

    const int left_x = left_center_x_ + inset - w / 2 + gx;
    const int right_x = right_center_x_ - inset - w / 2 + gx;
    const int left_y = std::clamp(eye_center_y_ - left_h / 2 +
                                      static_cast<int>(left_y_off_ + gy),
                                  0, kEyeBottomLimit - left_h);
    const int right_y = std::clamp(eye_center_y_ - right_h / 2 +
                                       static_cast<int>(right_y_off_ + gy),
                                   0, kEyeBottomLimit - right_h);

    lv_obj_set_pos(left_eye_, std::clamp(left_x, 0, width_ - w), left_y);
    lv_obj_set_size(left_eye_, w, left_h);
    lv_obj_set_style_radius(left_eye_, std::max(2, std::min(w, left_h) / 2), 0);
    lv_obj_set_pos(right_eye_, std::clamp(right_x, 0, width_ - w), right_y);
    lv_obj_set_size(right_eye_, w, right_h);
    lv_obj_set_style_radius(right_eye_, std::max(2, std::min(w, right_h) / 2), 0);
}

void EyesDisplay::ApplyMouth() {
    if (mouth_bar_ == nullptr || mouth_line_ == nullptr) {
        return;
    }
    const auto& profile = ProfileFor(expression_);
    const bool speaking = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;

    MouthShape shape = profile.mouth;
    float target_w = profile.mouth_w;
    float target_h = profile.mouth_h;
    float dx = profile.mouth_dx;
    float dy = profile.mouth_dy;

    if (speaking) {
        // 说话时嘴巴一张一合，看起来像在讲话。
        shape = MouthShape::Oval;
        target_w = 11.0f;
        target_h = 4.0f + 7.0f * std::fabs(std::sin(mouth_phase_));
        dx = 0.0f;
        dy = 0.0f;
    }

    mouth_w_ += (target_w - mouth_w_) * 0.25f;
    mouth_h_ += (target_h - mouth_h_) * 0.25f;

    const int cx = mouth_center_x_ + static_cast<int>(dx) + static_cast<int>(gaze_x_ * 0.3f);
    const int cy = mouth_center_y_ + static_cast<int>(dy);
    const int mw = std::clamp(static_cast<int>(mouth_w_), 3, 26);
    const int mh = std::clamp(static_cast<int>(mouth_h_), 3, 12);

    lv_obj_add_flag(mouth_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth_line_, LV_OBJ_FLAG_HIDDEN);

    if (shape == MouthShape::Bar || shape == MouthShape::Oval || shape == MouthShape::Dot) {
        const int bar_h = (shape == MouthShape::Bar) ? std::max(3, mh) : mh;
        const int bar_w = (shape == MouthShape::Dot) ? mw : mw;
        lv_obj_remove_flag(mouth_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(mouth_bar_, std::clamp(cx - bar_w / 2, 0, width_ - bar_w),
                       std::clamp(cy - bar_h / 2, kEyeBottomLimit, kMouthBottomLimit - bar_h));
        lv_obj_set_size(mouth_bar_, bar_w, bar_h);
        lv_obj_set_style_radius(mouth_bar_, std::max(2, std::min(bar_w, bar_h) / 2), 0);
        return;
    }

    // 曲线嘴：微笑 / 撇嘴 / 波浪。用折线近似，坐标相对屏幕左上角。
    const int base_x = cx - mw / 2;
    const int base_y = cy - mh / 2;
    if (shape == MouthShape::Smile) {
        mouth_points_[0] = {static_cast<lv_value_precise_t>(base_x),
                            static_cast<lv_value_precise_t>(base_y)};
        mouth_points_[1] = {static_cast<lv_value_precise_t>(base_x + mw / 4),
                            static_cast<lv_value_precise_t>(base_y + mh / 2)};
        mouth_points_[2] = {static_cast<lv_value_precise_t>(base_x + mw / 2),
                            static_cast<lv_value_precise_t>(base_y + mh)};
        mouth_points_[3] = {static_cast<lv_value_precise_t>(base_x + mw * 3 / 4),
                            static_cast<lv_value_precise_t>(base_y + mh / 2)};
        mouth_points_[4] = {static_cast<lv_value_precise_t>(base_x + mw),
                            static_cast<lv_value_precise_t>(base_y)};
    } else if (shape == MouthShape::Frown) {
        mouth_points_[0] = {static_cast<lv_value_precise_t>(base_x),
                            static_cast<lv_value_precise_t>(base_y + mh)};
        mouth_points_[1] = {static_cast<lv_value_precise_t>(base_x + mw / 4),
                            static_cast<lv_value_precise_t>(base_y + mh / 2)};
        mouth_points_[2] = {static_cast<lv_value_precise_t>(base_x + mw / 2),
                            static_cast<lv_value_precise_t>(base_y)};
        mouth_points_[3] = {static_cast<lv_value_precise_t>(base_x + mw * 3 / 4),
                            static_cast<lv_value_precise_t>(base_y + mh / 2)};
        mouth_points_[4] = {static_cast<lv_value_precise_t>(base_x + mw),
                            static_cast<lv_value_precise_t>(base_y + mh)};
    } else {  // Wave
        mouth_points_[0] = {static_cast<lv_value_precise_t>(base_x),
                            static_cast<lv_value_precise_t>(base_y + mh / 2)};
        mouth_points_[1] = {static_cast<lv_value_precise_t>(base_x + mw / 4),
                            static_cast<lv_value_precise_t>(base_y)};
        mouth_points_[2] = {static_cast<lv_value_precise_t>(base_x + mw / 2),
                            static_cast<lv_value_precise_t>(base_y + mh / 2)};
        mouth_points_[3] = {static_cast<lv_value_precise_t>(base_x + mw * 3 / 4),
                            static_cast<lv_value_precise_t>(base_y + mh)};
        mouth_points_[4] = {static_cast<lv_value_precise_t>(base_x + mw),
                            static_cast<lv_value_precise_t>(base_y + mh / 2)};
    }
    lv_obj_set_pos(mouth_line_, 0, 0);
    lv_obj_set_size(mouth_line_, width_, height_);
    lv_line_set_points(mouth_line_, mouth_points_, 5);
    lv_obj_remove_flag(mouth_line_, LV_OBJ_FLAG_HIDDEN);
}

void EyesDisplay::Tick() {
    const uint32_t now = lv_tick_get();
    const auto state = Application::GetInstance().GetDeviceState();
    const auto& profile = ProfileFor(expression_);

    // 每个表情有自己的呼吸/抖动节奏。
    bob_phase_ += profile.bob_freq;
    bob_value_ = profile.bob_amp * std::sin(bob_phase_);
    shake_phase_ += profile.shake_freq;
    shake_value_ = profile.shake_amp * std::sin(shake_phase_ * 2.3f);

    // 说话时再叠加一层上下浮动，嘴巴同时开合，像是在回应。
    if (state == kDeviceStateSpeaking) {
        speak_phase_ += 0.22f;
        speak_bob_ = std::sin(speak_phase_) * 1.5f;
        mouth_phase_ += 0.9f;
    } else {
        speak_bob_ *= 0.85f;
        mouth_phase_ += 0.25f;
    }

    // 眨眼：闭眼 -> 保持 -> 睁开，偶尔连眨两下。
    float blink_open = 1.0f;
    if (profile.blink_ms != 0) {
        if (!blinking_ && now >= next_blink_ms_) {
            blinking_ = true;
            blink_started_ms_ = now;
        }
        if (blinking_) {
            const uint32_t elapsed = now - blink_started_ms_;
            const float progress = static_cast<float>(elapsed) / kBlinkDurationMs;
            if (progress < 0.35f) {
                blink_open = 1.0f - (progress / 0.35f) * 0.95f;
            } else if (progress < 0.55f) {
                blink_open = 0.05f;
            } else if (progress < 1.0f) {
                blink_open = 0.05f + ((progress - 0.55f) / 0.45f) * 0.95f;
            } else {
                blinking_ = false;
                blink_open = 1.0f;
                if ((esp_random() % 100) < 25) {
                    next_blink_ms_ = now + 220;
                } else {
                    ScheduleNextBlink(now);
                }
            }
        }
    } else {
        blinking_ = false;
    }
    left_open_ = blink_open;
    right_open_ = profile.right_closed ? 0.04f : blink_open;

    // 眼型平滑过渡到当前表情的目标值。
    left_h_ += (profile.left_h - left_h_) * 0.18f;
    right_h_ += (profile.right_h - right_h_) * 0.18f;
    left_y_off_ += (profile.left_y_off - left_y_off_) * 0.18f;
    right_y_off_ += (profile.right_y_off - right_y_off_) * 0.18f;

    // 视线：聆听时看前方，说话时轻摆，其余时间在表情偏置附近游走。
    if (state == kDeviceStateListening) {
        target_gaze_x_ = profile.gaze_x * 0.25f;
        target_gaze_y_ = profile.gaze_y * 0.25f;
    } else if (state == kDeviceStateSpeaking) {
        target_gaze_x_ = profile.gaze_x + std::sin(bob_phase_ * 0.7f) * 2.0f;
        target_gaze_y_ = profile.gaze_y;
    } else if (profile.tear || profile.gaze_y != 0.0f) {
        // 难过/思考等有表情偏置时不做大幅游走，避免破坏表情。
        target_gaze_x_ = profile.gaze_x + std::sin(now * 0.0003f) * 1.5f;
        target_gaze_y_ = profile.gaze_y;
    } else if (now >= next_gaze_ms_) {
        constexpr int kRangeX = 5;
        constexpr int kRangeY = 3;
        target_gaze_x_ = profile.gaze_x +
                         static_cast<float>(static_cast<int>(esp_random() % (kRangeX * 2 + 1)) -
                                            kRangeX);
        target_gaze_y_ = profile.gaze_y +
                         static_cast<float>(static_cast<int>(esp_random() % (kRangeY * 2 + 1)) -
                                            kRangeY);
        ScheduleNextGaze(now);
    }
    gaze_x_ += (target_gaze_x_ - gaze_x_) * 0.18f;
    gaze_y_ += (target_gaze_y_ - gaze_y_) * 0.18f;

    // 眼泪。
    UpdateTear(profile.tear, profile.tear_continuous);

    // 字幕自动隐藏。
    if (text_hide_at_ms_ != 0 && now >= text_hide_at_ms_) {
        text_hide_at_ms_ = 0;
        if (text_label_ != nullptr) {
            lv_label_set_text(text_label_, "");
        }
    }

    ApplyEyes();
    ApplyMouth();
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
    Expression expression;
    if (name == "neutral" || name == "robot_2") {
        expression = Expression::Neutral;
    } else if (name == "happy") {
        expression = Expression::Happy;
    } else if (name == "laughing") {
        expression = Expression::Laughing;
    } else if (name == "funny" || name == "silly") {
        expression = Expression::Silly;
    } else if (name == "sad") {
        expression = Expression::Sad;
    } else if (name == "crying") {
        expression = Expression::Crying;
    } else if (name == "embarrassed") {
        expression = Expression::Embarrassed;
    } else if (name == "angry") {
        expression = Expression::Angry;
    } else if (name == "surprised") {
        expression = Expression::Surprised;
    } else if (name == "shocked") {
        expression = Expression::Shocked;
    } else if (name == "sleepy") {
        expression = Expression::Sleepy;
    } else if (name == "relaxed") {
        expression = Expression::Relaxed;
    } else if (name == "thinking") {
        expression = Expression::Thinking;
    } else if (name == "confused") {
        expression = Expression::Confused;
    } else if (name == "winking") {
        expression = Expression::Winking;
    } else if (name == "loving") {
        expression = Expression::Loving;
    } else if (name == "kissy") {
        expression = Expression::Kissy;
    } else if (name == "cool") {
        expression = Expression::Cool;
    } else if (name == "confident") {
        expression = Expression::Confident;
    } else if (name == "delicious") {
        expression = Expression::Delicious;
    } else {
        // 未知情绪由上层回退到 neutral。
        return false;
    }

    if (expression != expression_) {
        expression_ = expression;
        OnExpressionChanged();
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
    // 单色 OLED 只保留眼睛、嘴巴和一行文字，不画状态栏图标。
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
#include "wifi_board.h"
#include "application.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "led/single_led.h"
#include "button.h"
#include "mcp_server.h"
#include "web_server.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#ifdef CONFIG_2WD_DISPLAY_SSD1306_128X64
#include <esp_lcd_io_i2c.h>
#include <esp_lcd_panel_ssd1306.h>
#include "display/eyes_display.h"
#endif
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace {
static const char* kTag = "DevKitC1_2WD";
constexpr uint32_t kPwmMaximumDuty = 1023;
constexpr int kDefaultSpeedPercent = 35;
constexpr int kMinimumSpeedPercent = 20;
constexpr int kMaximumSpeedPercent = 80;
constexpr int kDefaultDurationMs = 800;
// 单次动作上限 30s：仍是有限时长，到点由定时器自动停车，避免小车失控。
constexpr int kMaxDurationMs = 30000;
constexpr ledc_timer_t kMotorPwmTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kMotorLeftPwmChannel = LEDC_CHANNEL_2;
constexpr ledc_channel_t kMotorRightPwmChannel = LEDC_CHANNEL_3;
constexpr EventBits_t kMotorStopEvent = BIT0;
constexpr uint32_t kMotorStopTaskStackSize = 2048;
enum class Motion { Forward, Backward, Left, Right };

/** 封装 TB6612 两路方向/PWM 控制，并用定时器提供自动停车保护。 */
class MotorController {
public:
    MotorController() {
        // 上电先让驱动器待机，避免 GPIO 初始化瞬间电机误动作。
        gpio_config_t cfg = {.pin_bit_mask = (1ULL << MOTOR_AIN1_GPIO) | (1ULL << MOTOR_AIN2_GPIO) |
                              (1ULL << MOTOR_BIN1_GPIO) | (1ULL << MOTOR_BIN2_GPIO) |
                              (1ULL << MOTOR_STBY_GPIO), .mode = GPIO_MODE_OUTPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(MOTOR_STBY_GPIO, 0);
        const ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT, .timer_num = kMotorPwmTimer, .freq_hz = 20000,
            .clk_cfg = LEDC_AUTO_CLK, .deconfigure = false};
        ESP_ERROR_CHECK(ledc_timer_config(&timer));
        ConfigurePwm(kMotorLeftPwmChannel, MOTOR_PWMA_GPIO);
        ConfigurePwm(kMotorRightPwmChannel, MOTOR_PWMB_GPIO);
        stop_event_group_ = xEventGroupCreate();
        configASSERT(stop_event_group_ != nullptr);
        // 定时器回调只投递停车事件，实际 GPIO 操作交给任务执行，避免回调与 Move() 互锁。
        const esp_timer_create_args_t args = {.callback = [](void* p) {
                auto* self = static_cast<MotorController*>(p);
                xEventGroupSetBits(self->stop_event_group_, kMotorStopEvent);
            }, .arg = this,
            .dispatch_method = ESP_TIMER_TASK, .name = "motor_stop", .skip_unhandled_events = true};
        ESP_ERROR_CHECK(esp_timer_create(&args, &stop_timer_));
        // 任务栈必须由 FreeRTOS 动态分配：xTaskCreateStatic 要求 TCB 位于内部 RAM，
        // 而板上内部 RAM 不满足 xPortCheckValidTCBMem() 的字节可访问要求，会触发 assert。
        if (xTaskCreate(StopTask, "motor_stop_task", kMotorStopTaskStackSize, this,
                        tskIDLE_PRIORITY + 1, &stop_task_) != pdPASS) {
            ESP_LOGE(kTag, "Failed to create motor stop task");
        }
    }
    ~MotorController() {
        Stop();
        // 停止定时器并等回调退出的路径依赖任务仍存活，必须先删任务再删事件组。
        if (stop_task_ != nullptr) {
            vTaskDelete(stop_task_);
            stop_task_ = nullptr;
        }
        esp_timer_delete(stop_timer_);
        vEventGroupDelete(stop_event_group_);
    }

    void Move(Motion motion, int duration_ms, int speed_percent) {
        duration_ms = std::clamp(duration_ms, 100, kMaxDurationMs);
        // 以 10 位 LEDC 分辨率把语音给出的速度百分比换算为 PWM 占空比。
        speed_percent = std::clamp(speed_percent, kMinimumSpeedPercent, kMaximumSpeedPercent);
        const uint32_t duty = kPwmMaximumDuty * speed_percent / 100;
        std::lock_guard<std::mutex> lock(mutex_);
        esp_timer_stop(stop_timer_);
        bool left_forward = motion != Motion::Backward;
        bool right_forward = motion != Motion::Backward;
        if (motion == Motion::Left) left_forward = false;
        if (motion == Motion::Right) right_forward = false;
        SetDirection(MOTOR_AIN1_GPIO, MOTOR_AIN2_GPIO, left_forward);
        SetDirection(MOTOR_BIN1_GPIO, MOTOR_BIN2_GPIO, right_forward);
        SetPwm(kMotorLeftPwmChannel, duty);
        SetPwm(kMotorRightPwmChannel, duty);
        gpio_set_level(MOTOR_STBY_GPIO, 1);
        // esp_timer_stop() 不等待回调返回。若上一条动作的停车回调已排在 esp_timer 任务队列里，
        // 它仍会在线程被调度后投递停车事件。这里记下本次动作的绝对截止时刻，停车事件到达时
        // 用当前时间判断它是否属于本次动作；早于截止时刻的一律是过期事件，直接丢弃。
        deadline_us_.store(esp_timer_get_time() + duration_ms * 1000LL);
        ESP_ERROR_CHECK(esp_timer_start_once(stop_timer_, duration_ms * 1000ULL));
    }
    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_timer_) esp_timer_stop(stop_timer_);
        gpio_set_level(MOTOR_STBY_GPIO, 0);
        SetPwm(kMotorLeftPwmChannel, 0);
        SetPwm(kMotorRightPwmChannel, 0);
        gpio_set_level(MOTOR_AIN1_GPIO, 0); gpio_set_level(MOTOR_AIN2_GPIO, 0);
        gpio_set_level(MOTOR_BIN1_GPIO, 0); gpio_set_level(MOTOR_BIN2_GPIO, 0);
    }
private:
    std::mutex mutex_;
    // 本次动作的绝对截止时刻（微秒）。停车任务据此识别并丢弃上一条动作遗留的超时事件。
    std::atomic<int64_t> deadline_us_{0};
    esp_timer_handle_t stop_timer_ = nullptr;
    EventGroupHandle_t stop_event_group_ = nullptr;
    TaskHandle_t stop_task_ = nullptr;

    static void StopTask(void* context) {
        auto* self = static_cast<MotorController*>(context);
        while (true) {
            xEventGroupWaitBits(self->stop_event_group_, kMotorStopEvent, pdTRUE, pdFALSE,
                                portMAX_DELAY);
            // 早于本次动作截止时刻的停车事件属于上一条动作，丢弃，避免误停刚发起的新动作。
            if (esp_timer_get_time() < self->deadline_us_.load()) {
                continue;
            }
            self->Stop();
        }
    }

    static void ConfigurePwm(ledc_channel_t channel, gpio_num_t pin) {
        const ledc_channel_config_t cfg = {.gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = channel, .intr_type = LEDC_INTR_DISABLE, .timer_sel = kMotorPwmTimer,
            .duty = 0, .hpoint = 0, .flags = {.output_invert = 0}};
        ESP_ERROR_CHECK(ledc_channel_config(&cfg));
    }
    static void SetDirection(gpio_num_t in1, gpio_num_t in2, bool forward) {
        gpio_set_level(in1, forward ? 1 : 0); gpio_set_level(in2, forward ? 0 : 1);
    }
    static void SetPwm(ledc_channel_t channel, uint32_t duty) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    }
};
}

class Esp32S3DevKitC1TwoWheelBoard : public WifiBoard {
public:
    Esp32S3DevKitC1TwoWheelBoard() : motors_() {
#ifdef CONFIG_2WD_DISPLAY_SSD1306_128X64
        // OLED 与 codec 共用 I2C 总线，先把总线建好再初始化屏幕。
        InitializeCodecI2c();
        InitializeOledDisplay();
#else
        // 初始化顺序与原 bread-compact-wifi-lcd 板一致：先显示，再音频和电机工具。
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeCodecI2c();
        // 恢复已保存的屏幕亮度；首次启动时使用 Backlight 的默认亮度。
        GetBacklight()->RestoreBrightness();
#endif
        RegisterMotorTools();
    }

    Led* GetLed() override {
        static SingleLed led(GPIO_NUM_48);
        return &led;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
#ifdef CONFIG_2WD_DISPLAY_SSD1306_128X64
        // OLED 没有背光。
        return nullptr;
#else
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
#endif
    }
    AudioCodec* GetAudioCodec() override {
        // 复用项目已有的 ES7210 ADC + ES8311 DAC 全双工实现。
        static BoxAudioCodec codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
        return &codec;
    }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Display* display_ = nullptr;
    MotorController motors_;

#ifndef CONFIG_2WD_DISPLAY_SSD1306_128X64
    void InitializeSpi() {
        // LCD 使用 SPI3；MISO 不接线，发送缓冲区放入 DMA 以保证刷屏稳定。
        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = DISPLAY_MOSI_PIN;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.sclk_io_num = DISPLAY_CLK_PIN;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        // 配置 ST7789 的 SPI 命令/数据控制线。
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 创建并初始化 ST7789 面板，参数与 bread-compact-wifi-lcd 原实现一致。
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                      DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
#else
    void InitializeOledDisplay() {
        // 0.96 寸 SSD1306 128x64，与 ES8311/ES7210 共用 GPIO1/2 的 I2C 总线。
        // 模块常见地址 0x3C，少数 0x3D；两个都试一下，找不到就退化为 NoDisplay。
        for (uint8_t address : {0x3C, 0x3D}) {
            esp_lcd_panel_io_i2c_config_t io_config = {};
            io_config.dev_addr = address;
            io_config.control_phase_bytes = 1;
            io_config.dc_bit_offset = 6;
            io_config.lcd_cmd_bits = 8;
            io_config.lcd_param_bits = 8;
            io_config.scl_speed_hz = 400 * 1000;
            esp_lcd_panel_io_handle_t panel_io = nullptr;
            if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &panel_io) != ESP_OK) {
                continue;
            }
            esp_lcd_panel_dev_config_t panel_config = {};
            panel_config.reset_gpio_num = GPIO_NUM_NC;
            panel_config.bits_per_pixel = 1;
            esp_lcd_panel_ssd1306_config_t ssd1306_config = {
                .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
                .contrast = 128,
            };
            panel_config.vendor_config = &ssd1306_config;
            esp_lcd_panel_handle_t panel = nullptr;
            if (esp_lcd_new_panel_ssd1306(panel_io, &panel_config, &panel) != ESP_OK) {
                esp_lcd_panel_io_del(panel_io);
                continue;
            }
            esp_err_t err = esp_lcd_panel_reset(panel);
            if (err == ESP_OK) {
                err = esp_lcd_panel_init(panel);
            }
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "SSD1306 init failed at 0x%02X", address);
                esp_lcd_panel_del(panel);
                esp_lcd_panel_io_del(panel_io);
                continue;
            }
            ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
            ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
            display_ = new EyesDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
            ESP_LOGI(kTag, "SSD1306 128x64 OLED initialized at 0x%02X", address);
            return;
        }
        ESP_LOGE(kTag, "No SSD1306 OLED found, falling back to NoDisplay");
        display_ = new NoDisplay();
    }
#endif
    void InitializeCodecI2c() {
        const i2c_master_bus_config_t cfg = {.i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN, .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .intr_priority = 0,
            .trans_queue_depth = 0, .flags = {.enable_internal_pullup = 1}};
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }
    void RegisterMotorTools() {
        auto& mcp = McpServer::GetInstance();
        const PropertyList motion_properties({
            Property("duration_ms", kPropertyTypeInteger, kDefaultDurationMs, 100, kMaxDurationMs),
            Property("speed_percent", kPropertyTypeInteger, kDefaultSpeedPercent,
                     kMinimumSpeedPercent, kMaximumSpeedPercent),
        });
        RegisterMove(mcp, "self.chassis.forward", "小车低速前进；speed_percent 可调节速度",
                     Motion::Forward, motion_properties);
        RegisterMove(mcp, "self.chassis.backward", "小车低速后退；speed_percent 可调节速度",
                     Motion::Backward, motion_properties);
        RegisterMove(mcp, "self.chassis.turn_left", "小车低速左转；speed_percent 可调节速度",
                     Motion::Left, motion_properties);
        RegisterMove(mcp, "self.chassis.turn_right", "小车低速右转；speed_percent 可调节速度",
                     Motion::Right, motion_properties);
        mcp.AddTool("self.chassis.stop", "立即停止小车", PropertyList(), [this](const PropertyList&) -> ReturnValue {
            motors_.Stop(); return true;
        });

        // 网页遥控：/api/chassis -> MotorController。回调在 HTTP 任务中执行，
        // MotorController 内部有互斥锁，可安全跨任务调用。
        WebServer::GetInstance().SetChassisControlCallback(
            [this](ChassisAction action, int speed_percent, int duration_ms) {
                switch (action) {
                    case ChassisAction::Forward:
                        motors_.Move(Motion::Forward, duration_ms, speed_percent);
                        return true;
                    case ChassisAction::Backward:
                        motors_.Move(Motion::Backward, duration_ms, speed_percent);
                        return true;
                    case ChassisAction::TurnLeft:
                        motors_.Move(Motion::Left, duration_ms, speed_percent);
                        return true;
                    case ChassisAction::TurnRight:
                        motors_.Move(Motion::Right, duration_ms, speed_percent);
                        return true;
                    case ChassisAction::Stop:
                        motors_.Stop();
                        return true;
                }
                return false;
            });
    }
    void RegisterMove(McpServer& mcp, const char* name, const char* description, Motion motion,
                      const PropertyList& properties) {
        mcp.AddTool(name, description, properties, [this, motion](const PropertyList& args) -> ReturnValue {
            motors_.Move(motion, args["duration_ms"].value<int>(),
                         args["speed_percent"].value<int>());
            return true;
        });
    }
};
DECLARE_BOARD(Esp32S3DevKitC1TwoWheelBoard);

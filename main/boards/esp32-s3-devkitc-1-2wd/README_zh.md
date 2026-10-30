# ESP32-S3-DevKitC-1 双轮小车

LCD（复用 bread-compact-wifi-lcd）：`MOSI=47`、`CLK=21`、`DC=40`、`RST=45`、`CS=41`、背光 `42`。

Codec：`SDA=1`、`SCL=2`、`MCK=15`、`BCK=5`、`WS=4`、功放 `EN=38`。

数据线按模块丝印直连、不要交叉：模块 `DO`（模块的输出）接 `GPIO6`，模块 `DI`（模块的输入）接 `GPIO7`。丝印是站在模块一侧定义的，接反会导致喇叭无声且麦克风读到的样本恒为 ±1。I2C 地址：ES8311 为 `0x18`（7 位），ES7210 的 `ADDR` 引脚接高，7 位地址为 `0x41`，因此 `AUDIO_CODEC_ES7210_ADDR` 配成 `0x82`；沿用默认的 `0x80` 会一直报 `ES7210: Write register fail` 并触发断言重启。

实时打断（barge-in）：模块在板内已把 ES8311 的一路输出回接到 ES7210 的第 3 通道，所以 `AUDIO_INPUT_REFERENCE` 为 `true`、`config.json` 打开 `CONFIG_USE_DEVICE_AEC`。语音输入因此是双通道（MIC1 + 参考），启动日志中应出现 `|AEC(FD_LOW_COST, NLP_VERY_AGGRESSIVE)|` 节点。设备端 AEC 依赖喇叭信号干净回采且麦克风与喇叭有物理隔离，摆放不当或音量过大时打断会变迟钝。

TB6612：左路 `AIN1=8/AIN2=9/PWMA=10`，右路 `BIN1=11/BIN2=12/PWMB=13`，`STBY=14`。`VCC` 接 3.3V 逻辑电源，`VM` 接独立电机电池，所有 GND 共地。

MCP 工具：`self.chassis.forward`、`backward`、`turn_left`、`turn_right`、`stop`。移动默认 800ms、PWM 速度 35%，可选速度范围 20–80%，动作时长范围 100–3000ms，超时自动停车。PWM 百分比不等于精确 RPM，实际速度会随负载和电池电压变化。

构建：`python3 scripts/build.py esp32-s3-devkitc-1-2wd --name esp32-s3-devkitc-1-2wd`。

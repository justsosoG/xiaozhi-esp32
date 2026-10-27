# ESP32-S3-DevKitC-1 双轮小车

LCD（复用 bread-compact-wifi-lcd）：`MOSI=47`、`CLK=21`、`DC=40`、`RST=45`、`CS=41`、背光 `42`。

Codec：`SDA=1`、`SCL=2`、`MCK=15`、`BCK=5`、Codec `DO -> GPIO6`、Codec `DI <- GPIO7`、功放 `EN=38`。

TB6612：左路 `AIN1=8/AIN2=9/PWMA=10`，右路 `BIN1=11/BIN2=12/PWMB=13`，`STBY=14`。`VCC` 接 3.3V 逻辑电源，`VM` 接独立电机电池，所有 GND 共地。

MCP 工具：`self.chassis.forward`、`backward`、`turn_left`、`turn_right`、`stop`。移动默认 800ms、PWM 速度 35%，可选速度范围 20–80%，动作时长范围 100–3000ms，超时自动停车。PWM 百分比不等于精确 RPM，实际速度会随负载和电池电压变化。

构建：`python3 scripts/build.py esp32-s3-devkitc-1-2wd --name esp32-s3-devkitc-1-2wd`。

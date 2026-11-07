# ESP32 PDD Clock · 拼多多股价桌面时钟

基于 **Waveshare ESP32-S3-Touch-LCD-1.54** 的桌面迷你信息屏：
实时时钟 + 天气 + 拼多多(PDD)股价与美股交易状态，电池供电，
内置六轴陀螺仪，**无论怎么摆放（0°/90°/180°/270°），屏幕文字始终正向可读**。

## 功能特性

- 🕐 **实时时钟**：NTP 授时（东八区），30px 大字体，秒级刷新无闪烁
- 🌤️ **天气**：Open-Meteo 免费接口（无需 key），按城市自动地理编码，10 分钟刷新
- 📈 **PDD 股价**：腾讯行情接口（国内直连免 key），30 秒刷新，红涨绿跌配色
- 🏷️ **交易状态**：盘前/交易中/盘后/已收盘，美东时间本地计算（含 DST 规则）
- 🔄 **四向姿态旋转**：陀螺仪感知姿态，软件重排 + 画布按位旋转，文字永远正向
- 🔋 **电池管理**：iPhone 风格电量图标（eFuse 校准 ADC），充电显示绿色闪电
- 🔘 **按键**：PWR 双击熄屏 / 长按关机，PLUS/BOOT 调亮度
- 📶 **网页配网**：首次上电自动开热点 `PDD-Clock`，浏览器填 WiFi/城市即用
- 🛡️ **IMU 自愈**：I2C 总线毛刺防护，寄存器校验 + 四层健康监控自动恢复

## 硬件

| 项目 | 规格 |
|---|---|
| 开发板 | Waveshare ESP32-S3-Touch-LCD-1.54（16MB Flash） |
| 屏幕 | ST7789 240×240 IPS，4 线 SPI |
| 陀螺仪 | QMI8658 六轴（I2C 0x6B，SDA=42/SCL=41） |
| 电池 | 锂电池 USB 充电（PW4054），ADC 分压检测 |
| 按键 | PWR=GPIO5 / PLUS=GPIO4 / BOOT=GPIO0 |

屏幕布局（240×240，逻辑坐标）：

```
┌──────────────────────────────┐ 状态栏：日期 + WiFi圆点 + 电池
│ 09/03 周四            ●  ▮▮ │
├──────────────────────────────┤
│         01:23:45             │ 时钟（logisoso30）
├──────────────────────────────┤
│  ☀  28 °C 晴  东莞            │ 天气行
├──────────────────────────────┤
│ 拼多多 PDD             [盘前] │ 股票区
│           82.91               │   （红涨绿跌）
│        +0.41  +0.50%          │
└──────────────────────────────┘
```

## 快速开始

### 方式一：免编译直接刷机（推荐）

1. 下载 [Releases](../../releases) 里的 `firmware-merged.bin`
2. 用 Chrome/Edge 打开 [ESP Launchpad](https://espressif.github.io/esp-launchpad/)
3. 连接设备 → 添加文件，地址填 `0x0`（Flash Mode: DIO, Size: 16MB）→ Program
4. 若进不了下载模式：按住 BOOT 键再点 Connect

> `firmware.bin` 是纯 app 镜像（刷 `0x10000`），`firmware-merged.bin` 是完整镜像（刷 `0x0`）。

### 方式二：PlatformIO 编译

```bash
pio run -t upload
```

关键配置（`platformio.ini` 已含，勿改丢）：

```ini
board_upload.flash_size = 16MB      ; 必须 16MB(DIO)，否则无限重启
board_build.flash_mode = dio
board_build.partitions = default_16MB.csv
build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1
```

依赖库：`U8g2_for_Adafruit_GFX`、`GFX Library for Arduino @1.4.9`（注意：1.5+ 需要 Arduino core 3.x）、`ArduinoJson@^7`。

## 首次配网

1. 上电后无 WiFi 配置时自动开热点 **`PDD-Clock`**（无密码）
2. 手机/电脑连上后浏览器打开 `192.168.4.1`（任意网址自动跳转）
3. 填写 WiFi 名称/密码/城市 → 保存，设备自动重启进入主界面
4. 也可在 `include/config.h` 硬编码 `WIFI_SSID / WIFI_PASSWORD / CITY_NAME`

## 按键

| 操作 | 功能 |
|---|---|
| PWR 双击（≤400ms） | 熄屏 / 亮屏 |
| PWR 长按 2s | 关机（释放电源保持脚） |
| PLUS / BOOT | 亮度 + / −（档位 0/20/40/60/80/100） |

## 可配置项（`include/config.h`）

| 宏 | 含义 | 默认 |
|---|---|---|
| `WIFI_SSID / WIFI_PASSWORD` | 留空则进配网模式 | `""` |
| `CITY_NAME / CITY_LAT / CITY_LON` | 城市（经纬度为 0 则按名查询） | 深圳 / 0 / 0 |
| `STOCK_SYMBOL / STOCK_NAME` | 股票代码/名称 | PDD / 拼多多 |
| `BRIGHT_DEFAULT_PCT` | 开机默认亮度 % | 40 |
| `HEADING_WHEN_AX_POS / _AY_POS` | IMU 轴向→方向映射（批次不同可校准） | 1 / 0 |
| `CHG_ACTIVE_LOW` | 充电检测电平 | 1（低=充电） |

## 实现要点（踩坑记录）

- **姿态旋转**：不调用 `setRotation()`（会把文字一起转躺）。固定 rotation=0，
  文字画进 1bpp 画布后**按位旋转**再贴屏，所有元素走逻辑坐标变换
- **防闪烁**：背景+文字在内存离屏合成 RGB565 位图后一次性推送，
  无"先擦后画"中间帧
- **IMU 自愈**：本板 I2C 总线偶发毛刺会篡改 QMI8658 内部寄存器（表现为放久了
  画面不转）。固件四层监控：连续读失败 / 数据冻结 / 合加速度物理不变量 /
  寄存器回读校验，触发即软复位传感器并重建总线
- **充电检测**：本机 PW4054 的 CHRG 为开漏输出（低=充电，浮空=未充电），
  需 `INPUT_PULLUP`；与常见文档描述相反，以实测为准
- **电池电压**：必须用 `esp_adc_cal` eFuse 校准，线性公式会低估约 0.5V
- **美股交易状态**：不依赖行情接口字段，本地用 UTC±DST 偏移 + gmtime 计算，
  DST 规则：3 月第二个周日 ~ 11 月第一个周日

## 数据源与致谢

- 授时：`ntp.aliyun.com`
- 天气：[Open-Meteo](https://open-meteo.com/)（免费，无需 key）
- 行情：腾讯行情 `qt.gtimg.cn`（国内直连，无需 key）
- 字体/图形：[U8g2](https://github.com/olikraus/u8g2)（文泉驿字体）、
  [Arduino_GFX](https://github.com/moononournation/Arduino_GFX)、[ArduinoJson](https://arduinojson.org/)

## License

[MIT](LICENSE)

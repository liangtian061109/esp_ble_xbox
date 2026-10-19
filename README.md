# esp_ble_xbox

**ESP32 蓝牙 HID Host —— 直连 Xbox 无线手柄**

![target](https://img.shields.io/badge/target-ESP32-red)
![esp-idf](https://img.shields.io/badge/ESP--IDF-v6.0.2-blue)
![license](https://img.shields.io/badge/license-Unlicense%20OR%20CC0--1.0-green)

把 Xbox 无线手柄（以及大部分第三方兼容手柄）直接接到 ESP32 上：开机自动连接、断线自动重连、按键 / 摇杆 / 扳机 / 十字键实时解码成可读日志。

项目基于 ESP-IDF 官方示例 [`esp_hid_host`](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/esp_hid_host) 改写。官方示例适合演示，但直接拿来连手柄会连续踩坑——配对方式不对、空闲 10 秒掉线、复位后再也连不上。这些坑本项目都填了，并把结论写进文档：

- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) —— Xbox 蓝牙报告的真实字节布局与实测键位
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) —— 按日志关键词定位故障
- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) —— 构建环境、IDF 补丁原理、验证清单

---

## 目录

- [特性](#特性)
- [硬件与支持范围](#硬件与支持范围)
- [快速开始](#快速开始)
- [配置项](#配置项)
- [输出日志](#输出日志)
- [键位表](#键位表)
- [连接与重连策略](#连接与重连策略)
- [ESP-IDF 补丁（重要）](#esp-idf-补丁重要)
- [适配其他手柄](#适配其他手柄)
- [目录结构](#目录结构)
- [已知限制](#已知限制)
- [排障](#排障)
- [贡献](#贡献)
- [许可证](#许可证)

---

## 特性

| 特性 | 说明 |
|---|---|
| **直连指定地址** | 跳过扫描，上电直接向写死的地址发起连接，通常 1～2 秒连上 |
| **失败回退扫描** | 直连不通时自动扫描，只挑“像手柄”的设备（名称关键词 / COD / BLE Appearance），不会乱连隔壁手机 |
| **Just Works 配对** | IO 能力设为 `ESP_BT_IO_CAP_NONE`，配合 `BTA_DmBond()` 主动发起 SSP，避免掉进 legacy PIN 分支 |
| **断线原地重连** | 链路断开后 2 秒重新连接，**不重启芯片**（重启会命中 ESP-IDF #18335，见下文） |
| **空闲保活** | HID 主机连上后会滑进 sniff 省电模式，空闲时被 supervision timeout 踢掉；每 5 秒发一条全 0 输出报文把链路唤醒 |
| **输入解码** | 16 字节报告解成按键名、摇杆量化值、扳机百分比、十字键方向，**只打印变化的部分**，不刷屏 |
| **报文心跳** | 每 5 秒打印 `ALIVE: reports=N (+M in last 5s)`，一眼区分“手柄没发数据”和“发了但没解析对” |
| **诊断扫描模式** | `SCAN_ONLY_MODE=1` 时只扫描不连接，打印附近所有经典蓝牙设备的地址 / 名称 / COD / RSSI |
| **分层日志开关** | `BT_VERBOSE_LOG=1` 可临时打开 BTM / SDP / L2CAP / HIDH 的 DEBUG 日志 |
| **附带 IDF 补丁** | 修复 v6.0.2 复位后无法重连 HID 设备的 [#18335](https://github.com/espressif/esp-idf/issues/18335)，见 [`patches/`](patches/) |

## 硬件与支持范围

**需要 ESP32（原版），因为它依赖经典蓝牙（Bluetooth Classic）。**

| 芯片 | 是否可用 | 原因 |
|---|---|---|
| ESP32 / ESP32-PICO | ✅ | 支持经典蓝牙 + BLE 双模 |
| ESP32-S2 / S3 / C2 / C3 / C5 / C6 / C61 / H2 | ❌ | 只有 BLE，没有经典蓝牙，无法连接经典 HID 手柄 |

其他要求：

- **Bluedroid 协议栈**（本项目不走 NimBLE 的 Classic 路径）
- 任意 ESP32 开发板 + USB 数据线即可，无需额外电路
- 测试环境：ESP32-DevKitC（4 MB flash）、ESP-IDF **v6.0.2**、Windows 10 + VS Code ESP-IDF 插件
- 测试手柄：Xbox 无线手柄（蓝牙版），`VID 0x045E / PID 0x02FD`，Report Map 334 字节，主输入报告 ID 1 长 16 字节

> Xbox 360 手柄用的是微软私有 2.4 GHz 协议，不是蓝牙，本项目不支持。

## 快速开始

### 1. 准备 ESP-IDF

```bash
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32 && . ./export.sh
```

### 2. 获取本项目

```bash
git clone https://github.com/liangtian061109/esp_ble_xbox.git
cd esp_ble_xbox
```

### 3. 打 ESP-IDF 补丁（推荐，见[下文](#esp-idf-补丁重要)）

```bash
cd $IDF_PATH
git apply /path/to/esp_ble_xbox/patches/esp-idf-18335-hid-reconnect.patch
```

### 4. 找到手柄的蓝牙地址

打开 `main/esp_hid_host_main.c`，把诊断模式打开：

```c
#define SCAN_ONLY_MODE 1
```

编译烧录后，串口会逐条列出附近设备，找到手柄那行（地址和名称都来自手柄本身）：

```
I (10469) ESP_HIDH_DEMO: [1] a0:5a:5d:bb:a0:e6 | 外设/手柄 (COD 5/2) | RSSI -38 dBm | 名字:"Xbox Wireless Controller"
```

> 手柄必须先进入配对模式才会被扫到：**按住顶部 USB-C 口旁的配对键约 3 秒，直到 Xbox 键快闪**。只开机、不按配对键时它不做 inquiry scan，扫不到很正常。

### 5. 填入地址并关闭诊断模式

```c
#define TARGET_BD_ADDR_STR "a0:5a:5d:bb:a0:e6"   // 换成你的手柄地址
#define SCAN_ONLY_MODE 0
```

### 6. 编译烧录

```bash
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash monitor     # Windows 下端口按实际填，Linux/macOS 一般是 /dev/ttyUSB0
```

VS Code 用户：`Build` → `Flash` 即可。**不要点 `Erase Flash`**——那会清掉 NVS 里的配对密钥，得重新配一次。

> `sdkconfig` 被 `.gitignore` 忽略，首次构建会用 `sdkconfig.defaults` 自动生成（已包含 Bluedroid + 经典蓝牙 + HID Host 的必要配置）。若要改扫描传输方式：`idf.py menuconfig` → `HID Example Configuration` → `Transport to scan`，经典手柄选 `Classic Bluetooth only`（默认）。

### 7. 连接

手柄先进配对模式，再给 ESP32 上电（或按复位）。正常会看到：

```
I (1337) ESP_HIDH_DEMO: 直连指定手柄 a0:5a:5d:bb:a0:e6（可在配置区改地址换手柄）
I (1337) ESP_HIDH_DEMO: CONNECTING to a0:5a:5d:bb:a0:e6 ...
I (1347) ESP_HIDH_DEMO: BONDING a0:5a:5d:bb:a0:e6 ...
I (56237) ESP_HIDH_DEMO: a0:5a:5d:bb:a0:e6 OPEN:
BDA:a0:5a:5d:bb:a0:e6, Status: OK, Connected: YES, Handle: 0, Usage: GAMEPAD
PID: 0x02fd, VID: 0x045e, VERSION: 0x0903
Report Map Length: 334
   GAMEPAD   INPUT REPORT, ID:   1, Length:  16
I (56267) ESP_HIDH_DEMO: BASELINE: LX:0 LY:256 RX:0 RY:-257 LT:0 RT:0 DPAD:CENTER BTN:0x0000 START:0
I (114697) ESP_HIDH_DEMO: CONNECTED. Monitoring input (report count every 5s)
```

连上手柄会短震一下，这是正常的主机确认，不是断开。

## 配置项

所有日常要改的开关都集中在 `main/esp_hid_host_main.c` 顶部的「用户配置区」。

| 宏 | 默认 | 作用 |
|---|---|---|
| `TARGET_BD_ADDR_STR` | `"a0:5a:5d:bb:a0:e6"` | 目标手柄地址。留空 `""` 则跳过直连，直接扫描后自动选 |
| `SCAN_ONLY_MODE` | `0` | `1` = 只扫描不连接（找地址 / 判断手柄在不在时用） |
| `BT_VERBOSE_LOG` | `0` | `1` = 打开 BTM/SDP/L2CAP/APPL/HIDH 的 DEBUG 日志（会刷屏） |
| `KEEPALIVE_OUTPUT` | `1` | `1` = 每 5 秒发全 0 输出报文保活；手柄出现异常震动 / 灯效时改 `0` |
| `DECODE_INPUT` | `1` | `1` = 解码成按键名和摇杆值；`0` = 只打印发生变化的原始字节 |
| `DUMP_REPORT_MAP` | `0` | `1` = 逐字节转储 Report Map（换手柄时用来逆推布局） |
| `SCAN_DURATION_SECONDS` | `10` | 每轮扫描时长（秒） |
| `CONNECT_WAIT_SECONDS` | `20` | 单次连接最多等多久。手柄从寻呼到 HID 通道建立常要十几秒，调小反而更容易失败 |
| `MAX_CANDIDATES` | `8` | 一轮最多尝试几个候选设备 |
| `ALIVE_REPORT_SEC` | `5` | 心跳打印间隔（秒），同时决定保活报文发送间隔 |

改 `BT_VERBOSE_LOG` 无需动 `sdkconfig`——`CONFIG_LOG_MAXIMUM_LEVEL` 默认已设为 DEBUG，只是把各层默认级别留在 WARNING，避免串口被刷爆。

## 输出日志

按键、方向键、摇杆、扳机只打印**变化**，并在行尾附带原始按位值：

```
BASELINE: LX:0 LY:256 RX:0 RY:-257 LT:0 RT:0 DPAD:CENTER BTN:0x0000 START:0
A[DOWN] | BTN:0x0001
A[UP] | BTN:0x0000
X[DOWN] | BTN:0x0008
LB[DOWN] | BTN:0x0040
DPAD:UP(1) | BTN:0x0000
LX:-32768 LY:-7169 | BTN:0x0000
LT:100% | BTN:0x0000
ALIVE: reports=522 (+522 in last 5s)
LINK LOST, reconnecting in 2s (no reboot)...
```

- `BASELINE` 是连上后第一帧：什么都没按时各字段的静默值，用来判断摇杆中位是否漂移
- `BTN:0x....` 是 15 个按键位组成的原始值，改键位表时以它为准
- 摇杆值已减中位，范围约 `-32768..32767`；死区 3000，跨 2000 才打印一次
- 扳机 `0..1023`，按 `0..15` 档量化打印

其他报告（`ID 2` / `ID 4`）不参与手柄解码，会原样打印 `INPUT ID:x Len:y` + hex。

## 键位表

这套位序是逐键实测出来的，**不是标准 XInput 顺序**（第三方兼容手柄常见）：

| bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 字节15.0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 名称 | A | B | — | X | Y | — | LB | RB | — | — | — | SELECT | — | L3 | R3 | START |

几个容易踩的点：

- **A、B 之后空一位才是 X、Y**，`bit2` 是预留位。按 X 报成 Y、按 Y 报成 LB，就是忘了这一位。
- **LB / RB 在 bit6 / bit7**，不是紧跟 Y。
- **bit13 / bit14 同时是摇杆按下和背键 M1 / M2**——这类手柄把背键硬件并联在 L3 / R3 上，HID 报告只发同一个 bit，软件无法区分。
- **SELECT = bit11，START = 字节 15 的 bit0**。后者常被误当成 Xbox 引导键。
- `bit2 / 5 / 8 / 9 / 10 / 12` 在这台手柄上按遍所有键都没有输出，基本可判定为空位。

改键位只需动一个数组（索引即 bit 号，`NULL` 表示未登记、日志会显示 `BITn`）：

```c
static const char *s_btn_names[15] = {
    "A", "B", NULL, "X", "Y", NULL, "LB", "RB",
    NULL, NULL, NULL, "SELECT", NULL, "L3", "R3",
};
```

完整报告布局、十字键编码、保活输出报文格式见 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)。

## 连接与重连策略

```
上电
 └─ 配置了地址？ ── 是 ─→ 直连（BTA_DmBond 主动配对）─→ 成功 ─→ 监控 + 保活
          │                        │失败
          └── 否 ──────────────────┴─→ 扫描 10s ─→ 只挑"像手柄"的设备 ─→ 逐个尝试
                                                          │全失败
                                                          └─→ 1s 后重扫
 监控中链路断开 ─→ 2s 后原地重连（不重启）
```

两个设计决定，都是踩坑换来的：

1. **断开不重启。** 早期版本用 `esp_restart()` 清状态，结果每次重启都命中 #18335——带着 NVS 里的旧 bond 重连时被拒（`reject connect request from bonding device`）或 SDP 超时。原地重连没有这个问题。
2. **扫描只连“像手柄”的设备。** 判定顺序：名称含 `Xbox` / `Gamepad` / `Controller` 等关键词 → BLE Appearance `0x03C4` → 经典蓝牙 COD `major=5` 且 `minor=1或2`。早期版本把全部扫描结果当兜底，误连过隔壁的陌生设备，冒出一堆 `SDP no matches` 假故障。

## ESP-IDF 补丁（重要）

**症状**：烧录后第一次能连，**复位一次就再也连不上**，必须重新配对。

**原因**：ESP-IDF v6.0.2 的 Bluedroid 在初始化 HID Host 时没有从 NVS 恢复已绑定设备的 HID 信息（`btc_hh.c` 里那行 `btc_storage_load_bonded_hid_info()` 是注释状态）。重连时栈不知道对端的历史信息，被迫重新走 SDP；而链路此时已进入 sniff 省电模式，SDP 超时 → `OPEN ERROR: ERR_SDP`。另有一处 `bta_hh_act.c` 的成功路径漏置 `status`，导致 BTC 层把设备表项清掉（日志 `Device does not exist in DB`）。

这是上游 issue [#18335](https://github.com/espressif/esp-idf/issues/18335)。

**套用**（两处改动，各 1 行代码）：

```bash
cd $IDF_PATH
git apply /path/to/esp_ble_xbox/patches/esp-idf-18335-hid-reconnect.patch
```

**还原**：

```bash
cd $IDF_PATH
git apply -R /path/to/esp_ble_xbox/patches/esp-idf-18335-hid-reconnect.patch
```

**验证**：连上手柄 → 按 ESP32 复位键 → 日志应直接 `conn complete` → `AUTH_CMPL stat:0x00` → `OPEN`，**不再出现** `reject connect request from bonding device` 和 `ERR_SDP`。

补丁修改的是 IDF 组件，套用后第一次 `idf.py build` 会重编整个 `bt` 组件，慢几分钟属正常。补丁只对 v6.0.2 验证过；其他版本请先看 `git apply --check` 是否通过。不打补丁也能用，代价是每次复位后要重新配对手柄。

## 适配其他手柄

1. 把 `DUMP_REPORT_MAP` 设为 `1`，烧录后拿到完整 Report Map（HID 描述符），确认输入报告的 ID 和长度。
2. 把 `DECODE_INPUT` 设为 `0`，逐个按键，观察哪些字节发生变化（日志会打印 `[索引] 旧值->新值`）。
3. 按实测结果修改 `gamepad_decode()`（字节偏移）和 `s_btn_names`（位序），必要时调整 `STICK_DEADZONE` / `STICK_STEP` / `TRIGGER_STEP`。

BLE 手柄同样支持：`menuconfig` → `Transport to scan` 选 `BLE only`，解码部分照样按上面三步改。注意 `looks_like_gamepad()` 里 COD 判断只对经典蓝牙有效。

## 目录结构

```
esp_ble_xbox/
├── CMakeLists.txt
├── sdkconfig.defaults              # 首次构建的默认配置（Bluedroid + Classic + HID Host）
├── main/
│   ├── esp_hid_host_main.c         # 主逻辑：连接状态机、报告解码、保活、日志
│   ├── esp_hid_gap.c / .h          # GAP 初始化、扫描、配对（改自官方示例）
│   ├── Kconfig.projbuild           # Transport to scan 选项
│   └── CMakeLists.txt
├── patches/
│   └── esp-idf-18335-hid-reconnect.patch
├── docs/
│   ├── PROTOCOL.md                 # 报告格式与键位原理
│   ├── TROUBLESHOOTING.md          # 按日志定位故障
│   └── DEVELOPMENT.md              # 构建环境、补丁原理、验证清单
├── README.md
└── LICENSE
```

关键函数：

| 函数 | 职责 |
|---|---|
| `app_main()` | NVS / GAP / HIDH 初始化，拉起任务 |
| `hid_demo_task()` | 外层循环：直连 → 扫描 → 挑候选 → 监控 → 重连 |
| `connect_classic()` | `esp_hidh_dev_open()` + `BTA_DmBond()`，并拦截“假连接” |
| `monitor_until_disconnect()` | 心跳计数 + 保活输出，直到链路断开 |
| `hidh_callback()` | HID 事件分发（OPEN / INPUT / BATTERY / CLOSE） |
| `gamepad_decode()` / `gamepad_print_changes()` | 字节 → 语义；只打印变化 |

## 已知限制

- **只支持经典蓝牙手柄**（Xbox One S / Series X|S 蓝牙版、以及兼容手柄）。Xbox 360 手柄不是蓝牙，不支持。
- **手柄只记一个主机**：换主机（手机 / PC / ESP32）必须重新进入配对模式，这是手柄本身的行为。
- **非标准键位需自行适配**：第三方手柄位序各不相同，见[适配其他手柄](#适配其他手柄)。
- **背键无法单独识别**：若手柄把 M1/M2 硬件并联到 L3/R3，HID 层只有一个 bit。想区分只能在手柄配套 App 里改映射到别的键，但前提是该 App 能映射到手柄键位而不是键盘键（键盘映射在手柄模式下不生效）。
- **只解码 ID 1 的 16 字节报告**，其他报告原样打印。
- **保活报文可能影响个别手柄**（异常震动 / 灯效），可置 `KEEPALIVE_OUTPUT 0` 关闭。
- **经典蓝牙与 WiFi 共存带宽有限**，同时跑 WiFi 高吞吐时连接稳定性会下降。
- **不打 IDF 补丁时，复位后需要重新配对**。

## 排障

按日志关键词查表，见 [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)。常见几条：

| 日志 | 含义 | 处置 |
|---|---|---|
| `hcif conn complete: hdl 0xfff, st 0x4` | 寻呼超时，手柄没应答 | 手柄进配对模式、确认没被其他主机占用、靠近 |
| `hcif conn complete: hdl 0x8x, st 0x22` | LMP 响应超时 | 同上；手柄可能正在连别的设备 |
| `OPEN ERROR: ERR_SDP` | SDP 没拿到 HID 服务记录 | 打 IDF 补丁；确认是手柄而不是别的设备 |
| `ESP_HIDH: Already Connected` | 设备表里有上次的假连接 | 复位一次；或直接重连（本项目会自动跳过） |
| `hcif disc complete: ... rsn 0x8` | supervision timeout，空闲掉线 | 打开 `KEEPALIVE_OUTPUT` |
| `PIN_REQ_EVT ... status 0x0c Cmd Disallowed` | 掉进了 legacy PIN 路径 | IO 能力必须是 `ESP_BT_IO_CAP_NONE` |
| `ALIVE: (+0 in last 5s)` | 链路在但手柄不发数据 | 手柄休眠，按一下 Xbox 键唤醒 |

## 贡献

欢迎提 issue / PR。提交前建议：

1. 说明手柄型号 + `VID/PID`（`OPEN` 日志里有），不同型号位序可能不同；
2. 附上 `BT_VERBOSE_LOG=1` 的关键片段，而不是整段刷屏日志；
3. 改解码逻辑时同步更新 [`docs/PROTOCOL.md`](docs/PROTOCOL.md) 的布局表。

## 许可证

沿用 ESP-IDF 示例的双许可证：[Unlicense](https://unlicense.org/) **OR** [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/)，详见 [LICENSE](LICENSE)。

## 致谢

- 基础代码来自 [ESP-IDF `esp_hid_host` 示例](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/esp_hid_host)（Espressif Systems）
- 报告布局参考 [xpadneo](https://github.com/atar-axis/xpadneo) 社区对 Xbox 蓝牙报告的逆向成果
- 复位重连问题定位参考 ESP-IDF issue [#18335](https://github.com/espressif/esp-idf/issues/18335)

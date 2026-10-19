# 开发与验证

面向要改代码或验证回归的人。只想把手柄连起来请看 [README](../README.md)；遇到故障请看 [TROUBLESHOOTING](TROUBLESHOOTING.md)。

---

## 1. 环境

| 项 | 值 |
|---|---|
| 芯片 | ESP32（**必须**，经典蓝牙只有原版 ESP32 有） |
| ESP-IDF | **v6.0.2**（补丁只对该版本验证过） |
| 协议栈 | Bluedroid（本项目不需要 NimBLE） |
| 构建 | CMake + `idf.py`，`MINIMAL_BUILD ON` |
| 已验证平台 | Windows 10 + VS Code ESP-IDF 扩展 |

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash monitor      # Ctrl+] 退出 monitor
```

首次构建会从 `sdkconfig.defaults` 生成 `sdkconfig`（后者被 git 忽略）。

## 2. 代码结构

```
main/
├── esp_hid_host_main.c   697 行   应用层：配置、状态机、解码、保活
├── esp_hid_gap.c        1275 行   GAP 初始化、扫描、配对（改自官方示例）
├── esp_hid_gap.h         114 行   接口与寄存器宏
└── Kconfig.projbuild               menuconfig 里的 Transport to scan
```

职责边界：

| 文件 | 负责 | 不负责 |
|---|---|---|
| `esp_hid_host_main.c` | 连谁、怎么连、连上之后干什么、日志怎么打 | 协议栈初始化细节 |
| `esp_hid_gap.c` | `esp_bt_controller_init`、GAP 回调、配对应答、扫描实现 | 业务逻辑 |

### 2.1 主流程

```
app_main()
 ├─ esp_log_level_set(...)        # BT_VERBOSE_LOG 打开时才生效
 ├─ nvs_flash_init()              # 存 bond，别乱擦
 ├─ esp_hid_gap_init(HID_HOST_MODE)
 ├─ esp_hidh_init(&config)        # event_stack_size = 4096
 └─ xTaskCreate(hid_demo_task, 6 KB, prio 2)
```

### 2.2 连接状态机（`hid_demo_task`）

```
while (1)
 ├─ 配了地址？
 │   ├─ 是 → connect_classic(target)
 │   │        ├─ esp_hidh_dev_open() 返回 NULL → 有假连接，跳过本轮
 │   │        └─ BTA_DmBond() 主动配对
 │   │     → wait_for_connect(20s)
 │   │        ├─ 成功 → monitor_until_disconnect() → 断链后 2s 原地重连
 │   │        └─ 失败 → 落到扫描
 │   └─ 否 → 直接扫描
 ├─ scan_once(10s)
 ├─ SCAN_ONLY_MODE → report_scan_results() → 3s 后重扫（不连接）
 ├─ looks_like_gamepad() 过滤出候选（最多 8 个）
 ├─ 逐个 connect + wait_for_connect
 └─ 连上 → monitor_until_disconnect() → 2s 后重扫
```

### 2.3 关键函数

| 函数 | 作用 | 注意点 |
|---|---|---|
| `connect_classic()` | open + bond | `esp_hidh_dev_open()` 返回 NULL 时**不要**再调 `BTA_DmBond()`，会刷一堆认证失败 |
| `wait_for_connect()` | 等 `hid_connected` | 1 秒粒度轮询，最长 `CONNECT_WAIT_SECONDS` |
| `monitor_until_disconnect()` | 心跳 + 保活 | 每 `ALIVE_REPORT_SEC` 秒打印一次，并发一条全 0 输出报文 |
| `hidh_callback()` | HID 事件分发 | 在协议栈任务上下文执行，**不要在里面做长阻塞操作** |
| `gamepad_decode()` | 16 字节 → 语义 | 见 [PROTOCOL](PROTOCOL.md) |
| `gamepad_print_changes()` | 只打印变化 | 想加自己的控制逻辑就写在这里 |
| `msg_append()` | 安全拼接日志 | 替代裸 `snprintf` 累加 offset，防止写满后下溢 |

## 3. 关键实现说明

### 3.1 为什么要调 `BTA_DmBond()`

ESP-IDF 没有「主动发起经典蓝牙配对」的公开 API，只有 `esp_bt_gap_pin_reply()` / `esp_bt_gap_ssp_confirm_reply()` 这类**应答**接口。而 Xbox 手柄的 HID 服务要求链路已认证 + 加密，等对端来发起配对往往等不到。

`BTA_DmBond()` 是 Bluedroid 内部接口（`libbt.a` 里有符号），作用等同 Android 的 `createBond()`：

```c
extern void BTA_DmBond(uint8_t *bd_addr);   // 声明在 esp_hid_host_main.c 顶部
```

它不在公开头文件里，所以需要自己 `extern` 声明。这是本项目唯一的「非公开 API」依赖，升级 IDF 时要留意该符号是否还存在（链接失败会直接报 undefined reference）。

### 3.2 IO 能力必须是 `ESP_BT_IO_CAP_NONE`

```c
esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));
```

设成 `ESP_BT_IO_CAP_IO`（官方示例的写法）会协商数字比较/密码，手柄没有输入输出能力，最终回落到 legacy PIN，然后 `Cmd Disallowed` + 认证超时，`stat:0x09`。

### 3.3 假连接的识别与跳过

`esp_hidh_dev_open()` 返回 `NULL` 表示 `esp_hidh` 的设备表里还留着该地址的记录（上一次失败没收干净），日志会打 `ESP_HIDH: Already Connected`。此时再发配对只会产生一堆认证失败，因此直接跳过本轮：

```c
esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BT, 0);
if (dev == NULL) {
    ESP_LOGW(TAG, "设备表里已有残留记录（假连接），本轮跳过");
    return false;
}
```

早期版本在这里 `esp_restart()` 清状态，结果每次重启都命中 #18335，变成死循环。现在改为原地重连。

### 3.4 保活报文

```c
uint8_t idle_out[8] = {0};
esp_hidh_dev_output_set(s_dev, 0, 3, idle_out, sizeof(idle_out));
```

报告 ID 3，全 0 = 停止震动。目的是把链路从 sniff 状态反复唤回，避免 supervision timeout（`rsn 0x8`）。

### 3.5 日志缓冲安全

拼接多个字段时不要写 `off += snprintf(msg + off, sizeof(msg) - off, ...)`：一旦 `off > sizeof(msg)`，`sizeof(msg) - off` 作为 `size_t` 会下溢成巨大值，直接越界写。本项目统一用：

```c
static int msg_append(char *dst, size_t size, int off, const char *fmt, ...);
```

写满后停止追加，返回实际偏移供下次使用。极端场景（15 个键全按下 + 四轴 + 双扳机同时变化）能真实触发。

## 4. sdkconfig 关键项

| 配置 | 值 | 说明 |
|---|---|---|
| `CONFIG_BT_BLUEDROID_ENABLED` | y | 协议栈选型，必须 Bluedroid |
| `CONFIG_BT_CLASSIC_ENABLED` | y | 经典蓝牙总开关 |
| `CONFIG_BT_HID_HOST_ENABLED` | y | HID 主机角色 |
| `CONFIG_BT_BLE_ENABLED` | y | 同时开 BLE，`HID_HOST_MODE` 才是 3（双模） |
| `CONFIG_BTDM_CTRL_MODE_BTDM` | y | 控制器双模 |
| `CONFIG_LOG_MAXIMUM_LEVEL` | DEBUG(4) | 不开这个，`BT_VERBOSE_LOG` 的 DEBUG 语句根本编不进固件 |
| `CONFIG_BT_LOG_*_TRACE_LEVEL` | WARNING(2) | 各层默认级别。留在 WARNING 是为了不刷屏，需要时用运行时 `esp_log_level_set` 临时提 |
| `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` | y | 加了日志和解码后固件变大，普通单 app 分区容易放不下 |

扫描传输方式在 `main/Kconfig.projbuild`：`CONFIG_SCAN_TRANSPORT_CLASSIC / BLE / BOTH`，映射到 `HID_SCAN_MASK`。

## 5. ESP-IDF 补丁

### 5.1 原理

两个独立缺陷，都影响「复位后重连」：

| 文件 | 位置 | 改动 |
|---|---|---|
| `components/bt/host/bluedroid/btc/profile/std/hid/btc_hh.c` | `btc_hh_cb_handler()`，约 1267 行 | 取消注释 `btc_storage_load_bonded_hid_info()` |
| `components/bt/host/bluedroid/bta/hh/bta_hh_act.c` | `bta_hh_maint_dev_act()`，约 957 行 | `HID_HostAddDev()` 成功分支补 `dev_info.status = BTA_HH_OK;` |

第一个缺陷：HID Host 初始化时不从 NVS 恢复已绑定设备的描述符和 `app_id`。于是重连时 `bta_hh_open_act()` 认为这是新设备，必须重新走 SDP；而链路此时在 sniff 模式，SDP 超时 → `ERR_SDP`。

第二个缺陷：成功路径没置状态，BTC 层拿到 `BTA_HH_ERR` 就当失败，把 `added_devices` 表项清掉 → 下次重连日志出现 `Device does not exist in DB`。

恢复信息后 `p_cb->app_id != 0`，`bta_hh_open_act()` 直接走 `OPEN_CMPL`，跳过 SDP，问题消失。

### 5.2 套用与还原

```bash
cd $IDF_PATH
git apply /path/to/esp_ble_xbox/patches/esp-idf-18335-hid-reconnect.patch

# 验证补丁与当前树一致（应无输出）
git apply --check /path/to/esp_ble_xbox/patches/esp-idf-18335-hid-reconnect.patch

# 还原
git apply -R /path/to/esp_ble_xbox/patches/esp-idf-18335-hid-reconnect.patch
```

不想用 patch 也可以手工改那两处，实际改动只有 2 行代码（另有 4 行注释）。改完 `idf.py build` 会重编整个 `bt` 组件，慢几分钟。

> 补丁改的是 IDF 源码，`idf.py fullclean` 不会撤销它。换 IDF 版本或重新 clone 后需要重新套用。

## 6. 日志级别

`BT_VERBOSE_LOG 1` 会打开 BTM / SDP / L2CAP / APPL / HIDH 的 DEBUG。HCI 和 BTC 的 DEBUG 默认注释掉了——它们会打印每条命令和报文，输出量大一个数量级，需要时手动取消注释。

**调通之后一定关掉。** 手柄 10 ms 一帧，每帧至少两行 DEBUG ≈ 200 行/秒，115200 串口扛不住，日志写入会阻塞协议栈，现象就是「按键没反应」。

只想临时看某一层，可以在 `app_main` 里单独提：

```c
esp_log_level_set("BT_SDP", ESP_LOG_DEBUG);
```

## 7. 验证清单

改动连接、配对、保活相关代码后，按这五项回归。

### 7.1 首次配对

1. 手柄长按配对键 3 秒到 Xbox 键快闪；
2. 烧录（不 erase）或复位 ESP32；
3. 期望：`CONNECTING` → `BONDING` → `conn complete st 0x0` → `AUTH_CMPL stat:0x00 lk_type:4` → `OPEN: Usage: GAMEPAD`。
4. 手柄应短震一次。

### 7.2 复位重连（最关键）

1. 已连上的状态下按 ESP32 复位键（**不要** erase flash）；
2. 期望 5 秒内自动连回，日志里**不应出现**：
   - `reject connect request from bonding device`
   - `OPEN ERROR: ERR_SDP`
   - `Device does not exist in DB`
3. 失败说明补丁没生效（或那次成功连接前 erase 过，NVS 里没存下 HID 信息）：先连成功一次让栈写入 NVS，再复位重试。

### 7.3 空闲保活

1. 连上后什么都不按，静置 2 分钟；
2. 期望：`ALIVE` 持续输出，没有 `hcif disc complete ... rsn 0x8`；
3. 若仍掉线，确认 `KEEPALIVE_OUTPUT 1` 且 `monitor_until_disconnect()` 在跑（有 `ALIVE` 就说明在跑）。

### 7.4 输入完整性

逐个按键、推摇杆到四个极限、把两个扳机按到底，对照 [PROTOCOL 的键位表](PROTOCOL.md#25-按键位表逐键实测)：

- 每个键都应有独立的 `[DOWN]` / `[UP]`；
- 摇杆应能打到接近 `±32768`；
- 扳机应能到 `100%`；
- 十字键八个方向加回中，方向不应错位。

### 7.5 扫描模式

`SCAN_ONLY_MODE 1` 编译一次，确认：

- 手柄在配对模式时能被扫到，地址与 `TARGET_BD_ADDR_STR` 一致；
- 不在配对模式时扫不到（这是正常现象，不是 bug）；
- 只打印、不连接，日志里没有 `CONNECTING`。

### 7.6 记录表

| 项 | 结果 | 备注 |
|---|---|---|
| 7.1 首次配对 | ☐ 通过 ☐ 失败 | |
| 7.2 复位重连 | ☐ 通过 ☐ 失败 | |
| 7.3 空闲 2 分钟 | ☐ 通过 ☐ 失败 | |
| 7.4 输入完整性 | ☐ 通过 ☐ 失败 | |
| 7.5 扫描模式 | ☐ 通过 ☐ 失败 | |

## 8. 快速语法检查

完整 `idf.py build` 慢，只改一个文件时可以借上次构建的编译参数做单文件检查（`-Wall -Werror` 会真的报错，`-fsyntax-only` 不报未使用变量这类警告，所以建议直接 `-c` 出对象文件）：

```bash
cd esp_ble_xbox
python - <<'PY'
import json, re, subprocess, os, tempfile
d = json.load(open('build/compile_commands.json', encoding='utf-8'))
e = next(x for x in d if x['file'].replace('\\','/').endswith('main/esp_hid_host_main.c'))
out = os.path.join(tempfile.gettempdir(), 'check.o').replace('\\','/')
cmd = re.sub(r'\s-o\s+\S+', ' -o ' + out, e['command'])
p = subprocess.run(cmd, cwd=e['directory'], capture_output=True, text=True, errors='replace')
print('EXIT', p.returncode)
print(((p.stdout or '') + (p.stderr or '')).strip()[-2000:])
PY
```

前提是已经成功构建过一次（`build/compile_commands.json` 存在）。

## 9. 待办

- [ ] 震动反馈 API（封装输出报告 ID 3 的马达字段）
- [ ] 组合键 / 长按判定工具函数
- [ ] 电量读取（`ESP_HIDH_BATTERY_EVENT` 已有日志，可做成查询接口）
- [ ] 把 `TARGET_BD_ADDR_STR` 等配置从编译期宏改为 NVS 存储（换手柄免重编）
- [ ] 多手柄同时连接
- [ ] BLE 手柄的标准解码（目前解码逻辑是 Xbox 经典蓝牙专用）
- [ ] `docs/` 补一份真实失败日志的完整时间线案例

## 10. 编码约定

- 编译参数带 `-Wall -Werror`，任何警告都会导致构建失败；
- 高频日志（按键、摇杆、心跳）用纯 ASCII，避免串口 monitor 把中文显示成 `[?:e4]`；
- 拼接日志统一走 `msg_append()`；
- 新增对外符号尽量 `static`，避免污染链接空间；
- 改动报告解析时，同步更新 [PROTOCOL](PROTOCOL.md) 里的表格和实测证据。

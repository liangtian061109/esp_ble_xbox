# 排障手册

按日志定位故障。建议先看[速查表](#0-30-秒速查表)，再按[分阶段排障](#3-分阶段排障)深入。

---

## 0. 30 秒速查表

| 你看到的现象 | 日志关键词 | 最可能原因 | 处置 |
|---|---|---|---|
| 一直连不上，反复扫描 | `hdl 0xfff, st 0x4` | 寻呼超时：手柄没应答 | 手柄进配对模式；确认没被手机/PC 占着；靠近 |
| 连上了但立刻失败 | `st 0x22` | LMP 响应超时 | 同上；手柄可能正在连别的主机 |
| 连上后报错退出 | `OPEN ERROR: ERR_SDP` | SDP 没拿到 HID 服务记录 | 打 IDF 补丁；确认连的是手柄 |
| 第一次能连，复位后再也连不上 | `reject connect request from bonding device` | ESP-IDF #18335 | 打 [`patches/`](../patches/) 里的补丁 |
| 每次 open 都被拒 | `ESP_HIDH: Already Connected` | 设备表里有上次的假连接 | 复位一次；本项目会自动跳过 |
| 玩着玩着断 | `rsn 0x8` | sniff 省电 + supervision timeout | 打开 `KEEPALIVE_OUTPUT` |
| 按键没反应 | `ALIVE: (+0 in last 5s)` | 手柄休眠 / 链路已断 | 按 Xbox 键唤醒 |
| 按键有日志但名字不对 | `BTN:0x....` 与预期不符 | 键位表和这把手柄不匹配 | 按 [`PROTOCOL.md`](PROTOCOL.md) 实测后改 `s_btn_names` |
| 日志刷屏、按键变卡 | 大量 `BT_HIDH` / `BT_APPL` DEBUG | 串口带宽被日志吃满 | 关掉 `BT_VERBOSE_LOG` |
| 配对时报 PIN 被拒 | `PIN_REQ_EVT` + `0x0c Cmd Disallowed` | 掉进 legacy PIN 路径 | IO 能力必须是 `ESP_BT_IO_CAP_NONE` |

---

## 1. 读日志的基本方法

### 1.1 日志格式

```
I (56237) ESP_HIDH_DEMO: a0:5a:5d:bb:a0:e6 OPEN:
│    │           │
│    │           └─ TAG：哪个模块打的
│    └─ 开机后毫秒数
└─ 级别：E 错误 / W 警告 / I 信息 / D 调试
```

本项目相关的 TAG：

| TAG | 含义 | 什么时候关心 |
|---|---|---|
| `ESP_HIDH_DEMO` | 本项目的应用日志 | 全部，先看这个 |
| `ESP_HID_GAP` | GAP / 配对 / 扫描（来自 `esp_hid_gap.c`） | 配对失败、扫描异常 |
| `BT_HCI` | 控制器层：建链、断链、模式切换 | 连不上、掉线 |
| `BT_BTM` | 安全管理器 | 配对、加密失败 |
| `BT_SDP` | 服务发现 | `ERR_SDP` 类问题 |
| `BT_L2CAP` | 通道建立 | HID 通道建不起来 |
| `BT_APPL` / `BT_HIDH` | 上层 HID 状态机 | 连接状态异常 |
| `BT_BTC` | 协议栈与应用的桥接 | 认证结果 |

### 1.2 三个锚点

不管日志多长，先找这三行，就能把问题定位到阶段：

```
ESP_HIDH_DEMO: CONNECTING to xx:xx:xx:xx:xx:xx ...     ← 开始连
ESP_HID_GAP: BT GAP AUTH_CMPL: ... stat:0x00           ← 配对结果
ESP_HIDH_DEMO: xx:xx:xx:xx:xx:xx OPEN:                 ← 成功打开
```

- 连 `CONNECTING` 后连建链日志都没有 → 看[3.1](#31-阶段一寻呼acl-没建起来)
- 有 `AUTH_CMPL` 但 `stat != 0x00` → 看[3.2](#32-阶段二配对)
- `stat:0x00` 但迟迟没有 `OPEN` → 看[3.3](#33-阶段三sdp-与-hid-通道)

---

## 2. 正常时序

一次成功的连接应该长这样（时间戳为实测值）：

| 阶段 | 日志 | 说明 |
|---|---|---|
| 1. 准备 | `setting hid gap, mode:3, scan mask:0x01` | mode 3 = BT+BLE 双模，scan mask 0x01 = 只扫经典蓝牙 |
| 2. 发起 | `CONNECTING to a0:5a:5d:bb:a0:e6 ...` | `esp_hidh_dev_open()` |
| 3. 配对 | `BONDING a0:5a:5d:bb:a0:e6 ...` | `BTA_DmBond()` 主动发起 SSP |
| 4. 建链 | `hcif conn complete: hdl 0x81, st 0x0` | `hdl` 是有效句柄（不是 0xfff），`st 0x0` = 成功 |
| 5. 认证 | `AUTH_CMPL: ... stat:0x00 lk_type:4` | `stat 0` = 成功；`lk_type 4` = 未认证组合密钥（P-192），Just Works 的正常结果 |
| 6. 加密 | `Security Manager: Start encryption` → `access granted` | HID 通道要求链路加密 |
| 7. 控制通道 | `Got Control conn cnf ... CID 0x41, Result: 0` | L2CAP PSM 0x11 |
| 8. 中断通道 | `got Interrupt conn cnf ... CID 0x40, Result: 0` | L2CAP PSM 0x13 |
| 9. 打开 | `bta_hh_open_act: Device[0] connected` | HID 主机状态机进入 CONN |
| 10. 完成 | `OPEN: Status: OK, Connected: YES, Usage: GAMEPAD` | 可以收数据了 |
| 11. 监控 | `BASELINE: ...` → `ALIVE: reports=N` | 首帧基线 + 5 秒心跳 |

> `opcode=0x0419, status= 0x0c: Cmd Disallowed` 出现在第 4 步之前是**正常的**：那是 `Read Remote Extended Features` 在 ACL 还没建好时发出被拒，栈随后会自己重试。

---

## 3. 分阶段排障

### 3.1 阶段一：寻呼（ACL 没建起来）

**特征日志**

```
W (6477) BT_HCI: hcif conn complete: hdl 0xfff, st 0x4
W (6487) BT_BTC: btc_dm_auth_cmpl_evt() - Pairing timeout; retrying () ...
E (6442) BT_HIDH: OPEN ERROR: ERR_SDP
```

`hdl 0xfff` 是无效句柄，`st 0x4` = **Page Timeout**：ESP 向该地址寻呼约 5～20 秒，**对端完全没有应答**。后续所有错误（SDP、Pairing timeout、`Device does not exist`）都是连带反应，不用管。

也可能是 `st 0x22`（LMP Response Timeout）：对端收到了寻呼但没走完 LMP 握手。

**逐条排查**

1. **手柄真的在配对模式吗？** 只说「开机」不够。Xbox 手柄不按配对键时不做 inquiry scan，也不接受新主机寻呼。必须**按住顶部 USB-C 口旁的配对键约 3 秒，直到 Xbox 键快闪**。
2. **手柄是不是还被别的主机占着？** 手柄只记一个主机。上次连过手机/PC 且那边蓝牙还开着时，它会优先回连旧主机。先关掉那边的蓝牙，或把旧设备从手柄里「忘掉」。
3. **距离 / 干扰。** 先放到 30 cm 内试。经典蓝牙在 2.4 GHz 拥挤环境下寻呼成功率会明显下降。
4. **手柄有电吗？** 亏电时能广播但握手容易失败。
5. **地址对吗？** 用 `SCAN_ONLY_MODE 1` 扫一遍，确认地址和名称都在。

**判定**：看到 `st 0x0`（哪怕后面还有别的错）才说明链路层通了，可以往下看 3.2。

### 3.2 阶段二：配对

**特征日志**

```
I (4262) ESP_HID_GAP: BT GAP PIN_REQ_EVT min_16_digit:0
I (4262) ESP_HID_GAP: Input pin code: 1234
W (4262) BT_HCI: opcode=0x040d, status= 0c: Cmd Disallowed     ← PIN 根本没发出去
...
E (26552) BT_BTC: btc_dm_auth_cmpl_evt() Authentication fail reason 14
I (26552) ESP_HID_GAP: BT GAP AUTH_CMPL: ... stat:0x09 lk_type:0
```

**根因**：IO 能力设错。`ESP_BT_IO_CAP_IO`（有显示 + 可确认）会让栈去协商数字比较/密码，手柄这类无屏设备只能回落到 legacy PIN，然后超时。

**正确配置**（`main/esp_hid_gap.c`）：

```c
esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;   // Just Works
esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));
```

同时 PIN 应答留一个兜底，用 `0000`（`1234` 的通过率低得多）：

```c
pin_code[0] = '0'; pin_code[1] = '0'; pin_code[2] = '0'; pin_code[3] = '0';
```

**另一种失败**：`reject connect request from bonding device`

```
E (2047) BT_BTM: Security Manager: reject connect request from bonding device
W (2137) BT_SDP: SDP - Rcvd conn cnf with error: 0xf  CID 0x40
W (2137) BT_HCI: hcif conn complete: hdl 0x80, st 0xf
```

`st 0xf` = Connection Rejected due to Unacceptable BD_ADDR。这是 **#18335 的典型表现**：NVS 里有该设备的 bond，但栈没恢复它的 HID 信息，重连时新旧状态冲突被拒。**打补丁**。

配对成功后应该看到：

```
I (59602) ESP_HID_GAP: BT GAP AUTH_CMPL: a0:5a:5d:bb:a0:e6 stat:0x00 lk_type:4
```

`lk_type 4` = `ESP_BT_LINK_KEY_UNAUTHED_COMB_P192`（未认证组合密钥），Just Works 的正常结果，不是错误。

### 3.3 阶段三：SDP 与 HID 通道

**特征日志**

```
E (6442) BT_HIDH: OPEN ERROR: ERR_SDP
E (6442) ESP_HIDH_DEMO:  OPEN failed! status = 0xffffffff
```

`ERR_SDP` 表示 HID 主机拿不到对端的 HID 服务记录，可能原因按概率排序：

1. **对端不是 HID 设备** —— 扫描兜底误连了附近的手机/耳机。本项目已加过滤，若仍出现，检查日志里的地址对不对。
2. **#18335（复位后重连）** —— 打补丁。
3. **SDP 查询真的超时** —— 链路此时在 sniff 模式，响应慢。可用 `BT_VERBOSE_LOG 1` 看 `BT_SDP` 细节；若看到 `process_service_attr_rsp` 之后没有服务记录，就是这个问题。
4. **手柄固件要求先加密再查 SDP** —— 极少见，确认 `BTA_DmBond()` 在 `esp_hidh_dev_open()` 之后立刻调用。

**另一个相关日志**：

```
W (26212) BT_SDP: SDP - Rcvd ServiceSearchRsp, no matches
```

对端明确回复「没有这个服务」——几乎可以确定连错设备了。

### 3.4 已连上但没有输入

先看心跳：

```
I (124697) ESP_HIDH_DEMO: ALIVE: reports=742 (+220 in last 5s)   ← 有数据
I (129697) ESP_HIDH_DEMO: ALIVE: reports=742 (+0 in last 5s)     ← 没数据
```

| 心跳 | 现象 | 原因 | 处置 |
|---|---|---|---|
| `+0` | 完全无按键日志 | 手柄休眠 / 链路已断 / 手柄在别的模式 | 按 Xbox 键唤醒；确认没断链（看有没有 `LINK LOST`） |
| 数字涨 | 无解码输出 | 报告 ID 不是 1，或长度不足 16 | 看有没有 `INPUT ID:x Len:y`，按 `PROTOCOL.md` 适配 |
| 数字涨 | 有输出但名字不对 | 键位表不匹配 | 以日志里的 `BTN:0x....` 为准改 `s_btn_names` |
| 数字涨 | 输出乱、按一个键报一串 | 日志刷屏拖慢串口 | 关 `BT_VERBOSE_LOG` |

> **最常见的一条**：`BT_VERBOSE_LOG 1` 时手柄每 10 ms 一条报文，每条至少两行 DEBUG，约 200 行/秒。115200 的串口扛不住，日志写入会阻塞协议栈，表现出来就是「按键没反应」。调通之后务必关掉。

### 3.5 空闲掉线

```
W (114957) BT_HCI: hcif mode change: hdl 0x81, mode 2, intv 10, status 0x0   ← 进 sniff
I (129697) ESP_HIDH_DEMO: ALIVE: reports=742 (+0 in last 5s)                 ← 停止操作
W (132137) BT_HCI: hcif disc complete: hdl 0x81, rsn 0x8                     ← 7 秒后超时
I (132137) ESP_HIDH_DEMO: a0:5a:5d:bb:a0:e6 CLOSE:
```

`mode 2` = sniff 省电模式，`rsn 0x8` = supervision timeout。

- 确认 `KEEPALIVE_OUTPUT 1`（每 5 秒发一条全 0 输出报文唤醒链路）。
- 若手柄因此出现异常震动/灯效，改 `0`，改成靠操作维持心跳。
- 断开后本项目会 2 秒后原地重连，日志 `LINK LOST, reconnecting in 2s (no reboot)...`。

---

## 4. 错误码字典

### 4.1 HCI 状态（`hcif conn complete: ... st 0x..`）

| 值 | 名称 | 含义 |
|---|---|---|
| `0x00` | Success | 建链成功 |
| `0x02` | Unknown Connection Identifier | 句柄不存在（常见于链路已断还去操作它） |
| `0x04` | Page Timeout | 寻呼超时，对端没应答 |
| `0x05` | Authentication Failure | 认证失败 |
| `0x06` | PIN or Key Missing | 没有可用密钥 |
| `0x08` | Connection Timeout | 连接超时 |
| `0x0b` | Connection Already Exists | 链路已存在 |
| `0x0c` | Command Disallowed | 命令此时不被允许（如 ACL 未建就发远程特性请求：`opcode=0x0419`） |
| `0x0f` | Connection Rejected due to Unacceptable BD_ADDR | 被对端拒绝（#18335 常见） |
| `0x12` | Invalid HCI Command Parameters | 参数非法 |
| `0x13` | Remote User Terminated Connection | 对端主动断开 |
| `0x16` | Connection Terminated By Local Host | 本机主动断开 |
| `0x22` | LMP/LL Response Timeout | LMP 握手超时 |

### 4.2 断链原因（`hcif disc complete: ... rsn 0x..`）

含义同上表，常见组合：

- `rsn 0x8` —— supervision timeout，**空闲掉线**（sniff 的锅）
- `rsn 0x5` —— 认证失败导致断开
- `rsn 0x13` —— 对端（手柄）主动断开，通常是休眠或切换主机
- `rsn 0x16` —— 本机断开（`esp_hidh_dev_close()` 或示例的超时逻辑）

### 4.3 配对结果（`BT GAP AUTH_CMPL: ... stat:0x.. lk_type:..`）

`stat` 取自 `esp_bt_status_t`：

| 值 | 名称 | 含义 |
|---|---|---|
| `0x00` | SUCCESS | 配对成功 |
| `0x01` | FAIL | 失败 |
| `0x09` | AUTH_FAILURE | 认证失败（常见于 IO 能力不对） |
| `0x0a` | RMT_DEV_DOWN | 对端已断开 |
| `0x0d` | PENDING | 进行中 |

`lk_type` 取自 `esp_bt_link_key_type_t`：

| 值 | 名称 | 含义 |
|---|---|---|
| `0x00` | COMB | 组合密钥（传统配对） |
| `0x04` | UNAUTHED_COMB_P192 | **未认证组合密钥（P-192）—— Just Works 的正常结果** |
| `0x05` | AUTHED_COMB_P192 | 已认证组合密钥（P-192） |
| `0x07` | UNAUTHED_COMB_P256 | 未认证组合密钥（P-256） |
| `0x08` | AUTHED_COMB_P256 | 已认证组合密钥（P-256） |

看到 `stat:0x00 lk_type:4` 就说明配对已经成了，问题在后面。

---

## 5. 已知问题清单

| # | 现象 | 根因 | 状态 |
|---|---|---|---|
| 1 | 复位后无法重连，需重新配对 | ESP-IDF #18335：不恢复 NVS 中的 HID 信息 | 已提供补丁（[`patches/`](../patches/)） |
| 2 | 空闲约 10 秒掉线 | HID 主机自动进 sniff，手柄不应答 → supervision timeout | 已加保活输出 |
| 3 | `Already Connected` 后所有 open 被拒 | 设备表残留上次的假连接 | 跳过该轮 + 断链原地重连，不复位 |
| 4 | 误连附近陌生设备，报 `SDP no matches` | 扫描结果无过滤，拿全部结果兜底 | 已改为只连手柄类设备 |
| 5 | 配对卡在 PIN，`Cmd Disallowed` | IO 能力为 `ESP_BT_IO_CAP_IO` | 已改 `ESP_BT_IO_CAP_NONE` |
| 6 | 按键无反应 | DEBUG 日志刷爆 115200 串口，阻塞协议栈 | 已默认关闭 `BT_VERBOSE_LOG` |
| 7 | 方向键整体错一格 | hat 值 1..8 直接当数组下标 | 已改为 `v-1` |
| 8 | X/Y/LB/RB 键名全错位 | 按标准顺序假设位序，实际 bit2 是空位 | 已按实测修正位表 |
| 9 | `spi_flash: Detected size(4096k) larger than ... (2048k)` | 镜像头写的 2 MB，实际 flash 4 MB | 无害，可忽略；想消除就改 `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` |
| 10 | 日志里中文变 `[?:e4]` | 串口 monitor 编码问题 | 高频日志已改纯 ASCII |

---

## 6. 抓包（协议级视角）

串口日志看不到 SDP/L2CAP 的具体报文时，抓 HCI：

1. `menuconfig` → `Component config` → `Bluetooth` → 打开 **HCI debug mode**（`CONFIG_BT_HCI_LOG_DEBUG_EN`）；
2. 在应用里循环调用 `bt_hci_log_hci_data_show()` 把数据打到串口；
3. 保存串口输出，用 ESP-IDF 的 `tools/bt/bt_hci_to_btsnoop.py` 转成 `.btsnoop.log`；
4. Wireshark 打开，过滤 `sdp` 或 `l2cap`。

能直接看到「SDP 请求发出去了没有、对端回了什么」，比串口日志直观得多。

---

## 7. 提 issue 时请附上

1. **手柄型号 + `VID/PID`**（`OPEN` 日志里就有）；
2. **ESP-IDF 版本**（启动日志里的 `ESP-IDF: v6.0.2`）+ 是否打了补丁；
3. **失败阶段的日志片段**（从 `CONNECTING` 到报错，通常几十行就够），不要贴整段刷屏日志；
4. 已经试过哪些处置。

模板：

```
手柄: Xbox Wireless Controller (VID 0x045E / PID 0x02FD)
IDF: v6.0.2 + patches/esp-idf-18335-hid-reconnect.patch
现象: 复位后无法重连，日志如下
---8<---
（贴 AUTH_CMPL 前后 30 行）
---8<---
已试: 重配对手柄 / 换 USB 供电 / 关闭手机蓝牙
```

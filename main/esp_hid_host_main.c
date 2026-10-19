/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_hidh.h"
#include "esp_hid_gap.h"

static const char *TAG = "ESP_HIDH_DEMO";

#define ESP_BD_ADDR_STR         "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(addr)   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]

/* =====================================================================
 * 用户配置区 —— 日常使用只改这里
 * ===================================================================== */
// 目标手柄的蓝牙地址。换手柄就改这一行（地址从扫描日志里复制，
// 格式 "xx:xx:xx:xx:xx:xx"）。设为 "" 则不指定地址，扫描后自动连接。
#define TARGET_BD_ADDR_STR "a0:5a:5d:bb:a0:e6"   // Xbox Wireless Controller

// 诊断模式：1 = 只扫描并打印附近所有经典蓝牙设备（地址/名字/类别/信号），
//               不连接任何设备；TARGET_BD_ADDR_STR 仅用来做“有没有出现”的比对。
//            0 = 正常流程：先直连 TARGET，失败再扫描并逐个尝试连接。
// 找手柄地址、判断手柄到底在不在可发现状态时，先置 1 跑一轮。
#define SCAN_ONLY_MODE 0

// 蓝牙各层详细日志：1 = HCI/BTM/SDP/APPL/BTC/HIDH 全部 DEBUG（异常详细，会刷屏）
//                  0 = 只保留 WARNING 以上
// 注意：还要 CONFIG_LOG_MAXIMUM_LEVEL >= DEBUG（sdkconfig 里已设为 DEBUG），否则 DEBUG 语句编不进固件。
#define BT_VERBOSE_LOG 0

// 空闲保活：连上后链路空闲会自动进 sniff 省电模式，手柄不应答就会 supervision timeout
// 掉线（日志里 rsn 0x8）。每 5 秒发一条全 0 的输出报文（报告 ID 3，8 字节 = 震动/LED 全关）
// 把链路重新唤醒。如果手柄出现异常震动或灯效，把它改成 0。
#define KEEPALIVE_OUTPUT 1

// 输入解码开关：1 = 打印按键名/摇杆值（推荐，看得懂）；0 = 只打印原始字节变化
#define DECODE_INPUT 1

// 是否转储原始 Report Map（334 字节 hex 说明书，调试用；平时建议关掉）
#define DUMP_REPORT_MAP 0

/* ---------------------------- 行为调参 ---------------------------- */
#define SCAN_DURATION_SECONDS 10   // 每轮扫描时长（秒）
#define CONNECT_WAIT_SECONDS  20   // 每次连接最多等多久（秒）：Xbox 从寻呼到配对、SDP、
                                   // HID 通道建立常要十几秒，等太短会白白断开重试
#define MAX_CANDIDATES         8   // 一轮最多尝试几个候选设备
#define ALIVE_REPORT_SEC       5   // 连上后每隔几秒报一次“收到多少报文”
/* ===================================================================== */

// ESP-IDF 没有提供“主动发起经典蓝牙配对”的公开 API（只有 pin_reply / ssp_confirm_reply 等应答接口）。
// BTA_DmBond 是 Bluedroid 内部接口（libbt 中有该符号），作用与 Android 的 createBond() 相同：
// 主动向对端发起 SSP 配对。Xbox 手柄的 HID 服务要求链路认证+加密，不配对控制器会拒绝 HID 通道。
extern void BTA_DmBond(uint8_t *bd_addr);

// 连接状态：连上了就停止扫描
static volatile bool hid_connected = false;

// 累计收到的输入报文数：用来判断手柄到底有没有在发数据（按键时这个数字要涨）
static volatile uint32_t s_input_reports = 0;

// 当前连接的设备句柄（保活发输出报文要用）
static esp_hidh_dev_t *s_dev = NULL;

static char *bda2str(uint8_t *bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    snprintf(str, size, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
    return str;
}

// 往日志缓冲里追加内容：off 由上次返回值接续，写满后自动停止，不会越界
static int msg_append(char *dst, size_t size, int off, const char *fmt, ...)
{
    if (off < 0) {
        off = 0;
    }
    if ((size_t)off >= size) {
        return (int)size;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + off, size - (size_t)off, fmt, ap);
    va_end(ap);
    return (n < 0) ? (int)size : off + n;
}

#if SCAN_ONLY_MODE
// 把 COD（设备类别）翻成人话，方便一眼认出哪个是手柄（只在诊断扫描模式用到）
// major: 1=电脑 2=手机 3=网络 4=音视频 5=外设 6=成像 7=穿戴 8=玩具/游戏
// 外设(major=5) 的 minor: 1=摇杆 2=手柄 16=键盘 32=鼠标 48=键鼠
static const char *cod_str(const esp_bt_cod_t *cod)
{
    switch (cod->major) {
    case 1:  return "电脑";
    case 2:  return "手机";
    case 3:  return "网络设备";
    case 4:  return "音频/视频";
    case 6:  return "成像设备";
    case 7:  return "穿戴";
    case 8:  return "玩具/游戏";
    case 5:
        switch (cod->minor) {
        case 1:  return "外设/摇杆";
        case 2:  return "外设/手柄";
        case 16: return "外设/键盘";
        case 32: return "外设/鼠标";
        case 48: return "外设/键鼠";
        default: return "外设";
        }
    default: return "其他";
    }
}
#endif

/* =====================================================================
 * Xbox 手柄输入报告解码（Report ID 1，16 字节）
 *
 * Report Map（手柄的“说明书”）告诉我们这 16 字节的布局：
 *   字节      内容                  范围
 *   [0-1]    左摇杆 X (LX)          0..65535，中位约 32768
 *   [2-3]    左摇杆 Y (LY)          0..65535
 *   [4-5]    右摇杆 X (RX)          0..65535
 *   [6-7]    右摇杆 Y (RY)          0..65535
 *   [8-9]    左扳机 LT              10 bit（[8] 整个字节 + [9] 低 2 位），0..1023
 *   [10-11]  右扳机 RT              同上
 *   [12]     方向键 D-pad           低 4 位：1..8 是方向，>=8 算静止
 *   [13-14]  按键位               [13] 是 bit0..7，[14] 低 7 位是 bit8..14
 *                                  位序不是 A,B,X,Y：实测 A=bit0、B=bit1、bit2 空/预留、
 *                                  X=bit3、Y=bit4（详见下面 s_btn_names 的注释）
 *   [15]     Start 键               bit 0（实测是 Start，不是 Xbox 引导键）
 *
 * 想改按键名字？改下面的 s_btn_names 数组即可（顺序对应 bit0..bit14）。
 * ===================================================================== */
// 按键位表：索引 = 报告 ID 1 里的 bit 序号（字节 13 是 bit0..7，字节 14 低 7 位是 bit8..14）
// Xbox 的 BT 报告不是 A,B,X,Y 顺序：实测 A=bit0、B=bit1，bit2 是空/预留位，
// X=bit3、Y=bit4 —— 从 X 开始整体后移一位。NULL 表示还没实测，日志里显示 BITn。
static const char *s_btn_names[15] = {
    "A", "B", NULL, "X", "Y", NULL, "LB", "RB",     // bit0..bit7（已实测）
    NULL, NULL, NULL, "SELECT", NULL, "L3", "R3",   // bit8..14（SELECT=bit11 已实测；13/14 = 摇杆按下，背键并联）
};
static const char *s_dpad_names[9] = {
    "UP", "UP-RIGHT", "RIGHT", "DOWN-RIGHT", "DOWN", "DOWN-LEFT", "LEFT", "UP-LEFT", "CENTER"
};

#define STICK_DEADZONE   3000   // 摇杆死区：离中位小于此值视为静止
#define STICK_STEP       2000   // 摇杆打印步进：量化值跨一步才打印（防止刷屏）
#define TRIGGER_STEP     64     // 扳机打印步进（0..1023 / 64 = 0..15 档）

typedef struct {
    bool     valid;
    uint16_t buttons;            // 15 个按键，bit i 对应 s_btn_names[i]
    bool     home;               // 字节 15 的 bit0，实测是 Start 键
    uint8_t  dpad;               // 方向键原始值
    int16_t  lx, ly, rx, ry;     // 摇杆，已减中位 32768，范围约 -32768..32767
    uint16_t lt, rt;             // 扳机 0..1023
} gamepad_state_t;

static void gamepad_decode(const uint8_t *d, gamepad_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->valid  = true;
    s->lx = (int16_t)((d[0] | (d[1] << 8)) - 32768);
    s->ly = (int16_t)((d[2] | (d[3] << 8)) - 32768);
    s->rx = (int16_t)((d[4] | (d[5] << 8)) - 32768);
    s->ry = (int16_t)((d[6] | (d[7] << 8)) - 32768);
    s->lt = d[8]  | ((d[9]  & 0x03) << 8);   // 10 bit
    s->rt = d[10] | ((d[11] & 0x03) << 8);   // 10 bit
    s->dpad    = d[12] & 0x0F;               // 4 bit
    s->buttons = d[13] | ((d[14] & 0x7F) << 8); // 15 bit
    s->home    = (d[15] & 0x01) != 0;
}

static const char *dpad_name(uint8_t v)
{
    // HID 十字键（hat switch）用 1..8 表示八个方向：1=上、2=右上、3=右、4=右下、
    // 5=下、6=左下、7=左、8=左上；0/9~15 视为静止。所以要减 1 再查表。
    return (v >= 1 && v <= 8) ? s_dpad_names[v - 1] : "CENTER";
}

// 只打印“变化的部分”：按键按下/松开、方向键切换、摇杆跨步进、扳机跨档位
static void gamepad_print_changes(const gamepad_state_t *old_s, const gamepad_state_t *cur)
{
    char msg[256];
    int off = 0;

    for (int i = 0; i < 15; i++) {
        bool o = (old_s->buttons >> i) & 1;
        bool c = (cur->buttons   >> i) & 1;
        if (o != c) {
            char bname[10];
            if (s_btn_names[i] != NULL) {
                snprintf(bname, sizeof(bname), "%s", s_btn_names[i]);
            } else {
                snprintf(bname, sizeof(bname), "BIT%d", i);   // 未实测的位：直接报位号
            }
            off = msg_append(msg, sizeof(msg), off, "%s%s ", bname, c ? "[DOWN]" : "[UP]");
        }
    }
    if (old_s->home != cur->home) {
        off = msg_append(msg, sizeof(msg), off, "START%s ", cur->home ? "[DOWN]" : "[UP]");
    }
    if (old_s->dpad != cur->dpad) {
        off = msg_append(msg, sizeof(msg), off, "DPAD:%s(%u) ", dpad_name(cur->dpad), cur->dpad);
    }

    // 摇杆：量化到 STICK_STEP 的倍数，跨步才打印
    struct { const char *name; int16_t o, c; } sticks[4] = {
        {"LX", old_s->lx, cur->lx}, {"LY", old_s->ly, cur->ly},
        {"RX", old_s->rx, cur->rx}, {"RY", old_s->ry, cur->ry},
    };
    for (int i = 0; i < 4; i++) {
        bool o_active = abs(sticks[i].o) > STICK_DEADZONE;
        bool c_active = abs(sticks[i].c) > STICK_DEADZONE;
        if (o_active != c_active || sticks[i].o / STICK_STEP != sticks[i].c / STICK_STEP) {
            off = msg_append(msg, sizeof(msg), off, "%s:%d ", sticks[i].name, sticks[i].c);
        }
    }
    // 扳机：量化到 0..15 档，跨档才打印
    if (old_s->lt / TRIGGER_STEP != cur->lt / TRIGGER_STEP) {
        off = msg_append(msg, sizeof(msg), off, "LT:%d%% ", cur->lt * 100 / 1023);
    }
    if (old_s->rt / TRIGGER_STEP != cur->rt / TRIGGER_STEP) {
        off = msg_append(msg, sizeof(msg), off, "RT:%d%% ", cur->rt * 100 / 1023);
    }

    if (off > 0) {
        // 附带原始按位值，改映射表时靠它核对
        off = msg_append(msg, sizeof(msg), off, "| BTN:0x%04x", cur->buttons);
        ESP_LOGI(TAG, "%s", msg);

        // ===== 想加自己的控制逻辑？在这里写 =====
        // 例如：
        // if (cur->buttons & 0x0001) { ESP_LOGI(TAG, "A 键被按下了！"); }
        // if (cur->lt > 500)       { ESP_LOGI(TAG, "左扳机深度 %d", cur->lt); }
        // =======================================
    }
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            hid_connected = true;
            s_dev = param->open.dev;
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            if (bda) {
                ESP_LOGI(TAG, ESP_BD_ADDR_STR " OPEN: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->open.dev));
                esp_hidh_dev_dump(param->open.dev, stdout);
#if DUMP_REPORT_MAP
                // 原始 Report Map 逐字节转储（手柄的说明书原文）
                size_t num_maps = 0;
                esp_hid_raw_report_map_t *maps = NULL;
                if (esp_hidh_dev_report_maps_get(param->open.dev, &num_maps, &maps) == ESP_OK && num_maps > 0 && maps != NULL) {
                    for (size_t m = 0; m < num_maps; m++) {
                        ESP_LOGI(TAG, "REPORT MAP[%u/%u] len=%u:", (unsigned)(m + 1), (unsigned)num_maps, (unsigned)maps[m].len);
                        for (uint16_t i = 0; i < maps[m].len; i += 16) {
                            char line[80];
                            int off = snprintf(line, sizeof(line), "  %04x:", i);
                            for (uint16_t j = i; j < i + 16 && j < maps[m].len; j++) {
                                off += snprintf(line + off, sizeof(line) - off, " %02x", maps[m].data[j]);
                            }
                            ESP_LOGI(TAG, "%s", line);
                        }
                    }
                }
#endif
            }
        } else {
            ESP_LOGE(TAG, " OPEN failed! status = 0x%x", param->open.status);
        }
        break;
    }
    case ESP_HIDH_BATTERY_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
        if (bda) {
            ESP_LOGI(TAG, ESP_BD_ADDR_STR " BATTERY: %d%%", ESP_BD_ADDR_HEX(bda), param->battery.level);
        }
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        s_input_reports++;
        const uint8_t *bda = esp_hidh_dev_bda_get(param->input.dev);
        if (bda) {
            int len = param->input.length;
            const uint8_t *data = param->input.data;
#if DECODE_INPUT
            if (param->input.report_id == 1 && len >= 16) {
                // 主输入报告：解码成按键/摇杆语义
                static gamepad_state_t last = {0};
                gamepad_state_t cur;
                gamepad_decode(data, &cur);
                if (!last.valid) {
                    // 第一帧：打印静默基线，让你知道“什么都没按时”各字段的值
                    ESP_LOGI(TAG, "BASELINE: LX:%d LY:%d RX:%d RY:%d LT:%d RT:%d DPAD:%s BTN:0x%04x START:%d",
                             cur.lx, cur.ly, cur.rx, cur.ry, cur.lt, cur.rt,
                             dpad_name(cur.dpad), cur.buttons, cur.home);
                } else {
                    gamepad_print_changes(&last, &cur);
                }
                last = cur;
            } else {
                // 其他 Report ID（比如 Xbox 的分享键走 ID 2）：数据很短，原样打印
                ESP_LOGI(TAG, "INPUT ID:%u Len:%d Data:", param->input.report_id, len);
                ESP_LOG_BUFFER_HEX(TAG, data, len);
            }
#else
            // 不解码：只打印变化的字节 [索引] 旧值->新值
            static uint8_t last_rep[64];
            static bool has_last = false;
            uint8_t cur_buf[64];
            if (len > (int)sizeof(cur_buf)) {
                len = (int)sizeof(cur_buf);
            }
            memcpy(cur_buf, data, len);
            if (!has_last) {
                ESP_LOGI(TAG, "INPUT ID:%u Len:%d Data:", param->input.report_id, len);
                ESP_LOG_BUFFER_HEX(TAG, cur_buf, len);
            } else {
                char msg[320];
                int off = 0;
                int changed = 0;
                for (int i = 0; i < len; i++) {
                    if (cur_buf[i] != last_rep[i]) {
                        changed++;
                    }
                }
                if (changed > 0) {
                    off += snprintf(msg + off, sizeof(msg) - off, "CHANGED %d byte(s):", changed);
                    for (int i = 0; i < len && off < (int)sizeof(msg) - 16; i++) {
                        if (cur_buf[i] != last_rep[i]) {
                            off += snprintf(msg + off, sizeof(msg) - off, "  [%2d] %02x->%02x", i, last_rep[i], cur_buf[i]);
                        }
                    }
                    ESP_LOGI(TAG, "%s", msg);
                }
            }
            memcpy(last_rep, cur_buf, len);
            has_last = true;
#endif
        }
        break;
    }
    case ESP_HIDH_FEATURE_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->feature.dev);
        if (bda) {
            ESP_LOGI(TAG, ESP_BD_ADDR_STR " FEATURE: ID: %3u, Len: %d", ESP_BD_ADDR_HEX(bda),
                    param->feature.report_id, param->feature.length);
            ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        hid_connected = false;
        s_dev = NULL;
        const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
        if (bda) {
            ESP_LOGI(TAG, ESP_BD_ADDR_STR " CLOSE: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->close.dev));
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "EVENT: %d", event);
        break;
    }
}

// 名字里带这些关键词的，优先当成手柄去连
static bool name_looks_like_gamepad(const char *n)
{
    if (n == NULL) {
        return false;
    }
    return (strstr(n, "Xbox") != NULL || strstr(n, "XBOX") != NULL ||
            strstr(n, "Flydigi") != NULL || strstr(n, "Direwolf") != NULL ||
            strstr(n, "GameSir") != NULL || strstr(n, "Gamepad") != NULL ||
            strstr(n, "Controller") != NULL);
}

// 汇总判断一个扫描结果像不像手柄：名字 / BLE 外观 0x03C4(Gamepad) / 经典蓝牙 COD 用法
static bool looks_like_gamepad(esp_hid_scan_result_t *r)
{
    if (r == NULL) {
        return false;
    }
    if (name_looks_like_gamepad(r->name)) {
        return true;
    }
#if CONFIG_BT_BLE_ENABLED
    if (r->transport == ESP_HID_TRANSPORT_BLE && r->ble.appearance == 0x03C4) {
        return true;
    }
#endif
#if CONFIG_BT_HID_HOST_ENABLED
    // 注意：IDF 的 esp_hid_usage_from_cod() 恒返回 GENERIC（r->usage 没用），
    // 所以这里直接看 COD：major=5(外设) 且 minor=1(摇杆)/2(手柄) 才算手柄。
    if (r->transport == ESP_HID_TRANSPORT_BT &&
            r->bt.cod.major == 5 && (r->bt.cod.minor == 1 || r->bt.cod.minor == 2)) {
        return true;
    }
#endif
    return false;
}

static int add_cand(esp_hid_scan_result_t **c, int n, esp_hid_scan_result_t *r)
{
    for (int i = 0; i < n; i++) {
        if (c[i] == r) {
            return n;
        }
    }
    if (n >= MAX_CANDIDATES) {
        return n;
    }
    c[n] = r;
    return n + 1;
}

// 把 "a0:5a:5d:bb:a0:e6" 这样的字符串转成 6 字节地址；失败返回 false
static bool str2bda(const char *str, uint8_t bda[6])
{
    unsigned int b[6];
    if (str == NULL || str[0] == '\0') {
        return false;
    }
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        bda[i] = (uint8_t)b[i];
    }
    return true;
}

// 连接一个经典蓝牙手柄：打开设备 + 主动发起配对
// 注意：esp_hidh_dev_open / BTA_DmBond 的参数不是 const，这里不能加 const，
// 否则会触发 -Werror=discarded-qualifiers 编译错误
static bool connect_classic(uint8_t *bda)
{
    ESP_LOGI(TAG, "CONNECTING to " ESP_BD_ADDR_STR " ...", ESP_BD_ADDR_HEX(bda));
    esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BT, 0);
    if (dev == NULL) {
        // esp_hidh 里还留着这个设备的“已连接”记录（上一次没收干净），
        // 这时再 BONDING 只会刷一堆认证失败日志，直接跳过，等重启清理状态。
        ESP_LOGW(TAG, "设备表里已有 " ESP_BD_ADDR_STR " 的残留记录（假连接），本轮跳过", ESP_BD_ADDR_HEX(bda));
        return false;
    }
    // Xbox 等手柄的 HID 服务要求链路认证+加密，必须主动发起 SSP 配对，
    // 否则控制器会以“链路未认证”拒绝 HID 通道（L2CAP result 5）
    ESP_LOGI(TAG, "BONDING " ESP_BD_ADDR_STR " ...", ESP_BD_ADDR_HEX(bda));
    BTA_DmBond(bda);
    return true;
}

// 连上之后：每 5 秒报一次累计报文数。
// 按键时数字涨但屏幕没解码输出 → 报文格式/报告 ID 不匹配；
// 数字不涨 → 手柄没在发数据，或者链路已经断了。
static void monitor_until_disconnect(void)
{
    uint32_t last = 0;
    ESP_LOGI(TAG, "CONNECTED. Monitoring input (report count every 5s)");
    while (hid_connected) {
        vTaskDelay(pdMS_TO_TICKS(ALIVE_REPORT_SEC * 1000));
        if (!hid_connected) {
            break;
        }
        ESP_LOGI(TAG, "ALIVE: reports=%u (+%u in last %ds)",
                 (unsigned)s_input_reports, (unsigned)(s_input_reports - last), ALIVE_REPORT_SEC);
        last = s_input_reports;
#if KEEPALIVE_OUTPUT
        // 空闲时链路会滑进 sniff，主动发一条全 0 的输出报文把它唤醒，防止 0x8 超时掉线
        if (s_dev != NULL) {
            uint8_t idle_out[8] = {0};
            esp_hidh_dev_output_set(s_dev, 0, 3, idle_out, sizeof(idle_out));
        }
#endif
    }
    ESP_LOGW(TAG, "LINK LOST");
}

// 等连接结果：hid_connected 变 true 就提前返回
static bool wait_for_connect(void)
{
    for (int k = 0; k < CONNECT_WAIT_SECONDS && !hid_connected; k++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return hid_connected;
}

// 扫描一轮。成功返回 true（*results 可能为 NULL，表示一个都没扫到）
static bool scan_once(esp_hid_scan_result_t **results)
{
    size_t count = 0;
    ESP_LOGI(TAG, "SCAN... (%ds) 请让手柄处于可发现/配对状态", SCAN_DURATION_SECONDS);
    if (esp_hid_scan(SCAN_DURATION_SECONDS, HID_SCAN_MASK, &count, results) != ESP_OK) {
        ESP_LOGE(TAG, "scan failed, retry...");
        return false;
    }
    ESP_LOGI(TAG, "SCAN: %u results", (unsigned)count);
    return true;
}

#if SCAN_ONLY_MODE
// 诊断模式：把扫到的设备全打印出来（不连接）
static void report_scan_results(esp_hid_scan_result_t *results, bool cfg_has_addr, const uint8_t *target_bda)
{
    int idx = 0;
    bool cfg_seen = false;
    for (esp_hid_scan_result_t *r = results; r != NULL; r = r->next) {
        if (r->transport != ESP_HID_TRANSPORT_BT) {
            continue;
        }
        char bda_str[18] = {0};
        bda2str(r->bda, bda_str, sizeof(bda_str));
        bool is_cfg = cfg_has_addr && memcmp(r->bda, target_bda, 6) == 0;
        if (is_cfg) {
            cfg_seen = true;
        }
        ESP_LOGI(TAG, "[%d] %s | %s (COD %u/%u) | RSSI %d dBm | 名字:\"%s\"%s",
                 ++idx, bda_str, cod_str(&r->bt.cod),
                 (unsigned)r->bt.cod.major, (unsigned)r->bt.cod.minor,
                 (int)r->rssi,
                 r->name ? r->name : "(EIR 没带名字)",
                 is_cfg ? "   <<== 这就是配置区里的地址" : "");
    }

    if (idx == 0) {
        ESP_LOGW(TAG, "这一轮一个经典蓝牙设备都没扫到：手柄没开机 / 没进配对模式 / 离太远。");
    } else if (cfg_has_addr && cfg_seen) {
        ESP_LOGW(TAG, "配置地址 %s 出现在上面 %d 个设备里。", TARGET_BD_ADDR_STR, idx);
    } else if (cfg_has_addr) {
        ESP_LOGW(TAG, "配置地址 %s 没出现（本轮 %d 个设备）。手柄若开了机，就是不在配对模式、或地址已变。",
                 TARGET_BD_ADDR_STR, idx);
    } else {
        ESP_LOGW(TAG, "本轮 %d 个设备，把你要连的那个地址填进 TARGET_BD_ADDR_STR。", idx);
    }
}
#endif

static void hid_demo_task(void *pvParameters)
{
    uint8_t target_bda[6];
    bool cfg_has_addr = str2bda(TARGET_BD_ADDR_STR, target_bda);
    bool has_target = cfg_has_addr && !SCAN_ONLY_MODE;   // 诊断模式下不做直连

    while (1) {
        // ---- 模式一：直连指定地址的手柄（跳过扫描，秒连）----
        if (has_target) {
            ESP_LOGI(TAG, "直连指定手柄 %s（可在配置区改地址换手柄）", TARGET_BD_ADDR_STR);
            if (connect_classic(target_bda)) {
                wait_for_connect();
            }
            if (hid_connected) {
                monitor_until_disconnect();
                // 断开后原地重连，不要重启：复位后重连带 HID 设备会命中 ESP-IDF #18335
                // （重连走 SDP → 超时 / 连接被拒），原地重连没有这个问题。
                ESP_LOGW(TAG, "LINK LOST, reconnecting in 2s (no reboot)...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            ESP_LOGW(TAG, "直连失败（手柄开机了吗？需要先长按配对键进配对模式），回退扫描模式");
        }

        // ---- 模式二：扫描 + 逐个候选连接（一轮）----
        esp_hid_scan_result_t *results = NULL;
        if (!scan_once(&results)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

#if SCAN_ONLY_MODE
        // 诊断模式：只打印，不连接任何设备
        report_scan_results(results, cfg_has_addr, target_bda);
        esp_hid_scan_results_free(results);
        ESP_LOGI(TAG, "诊断模式：不连接任何设备，3 秒后重扫...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
#endif

        esp_hid_scan_result_t *cands[MAX_CANDIDATES];
        int ncan = 0;

        // 只连“像手柄”的设备；一个都没有就重扫。
        // 以前这里会把全部扫描结果当兜底，误连过附近的陌生设备（a8:4f:...），
        // 白费一轮还会冒出 SDP no matches 的假故障。
        for (esp_hid_scan_result_t *r = results; r != NULL; r = r->next) {
            if (looks_like_gamepad(r)) {
                ncan = add_cand(cands, ncan, r);
            }
        }

        if (ncan == 0) {
            esp_hid_scan_results_free(results);
            ESP_LOGW(TAG, "没扫到像手柄的设备，重新扫描...（手柄没进配对模式？）");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (int i = 0; i < ncan; i++) {
            esp_hid_scan_result_t *cr = cands[i];
            if (cr->transport == ESP_HID_TRANSPORT_BT) {
                if (!connect_classic(cr->bda)) {
                    continue;   // 设备表里有假连接，这轮跳过，不干等
                }
            } else {
                ESP_LOGI(TAG, "CONNECTING [%d/%d] to " ESP_BD_ADDR_STR " (BLE, ADDR_TYPE %d)...",
                         i + 1, ncan, ESP_BD_ADDR_HEX(cr->bda), cr->ble.addr_type);
                esp_hidh_dev_open(cr->bda, cr->transport, cr->ble.addr_type);
            }
            // 等连接结果：连上了就停，失败就换下一个候选
            if (wait_for_connect()) {
                break;
            }
            ESP_LOGW(TAG, "连接未成功，换下一个候选...");
        }

        esp_hid_scan_results_free(results);

        if (hid_connected) {
            monitor_until_disconnect();
            ESP_LOGW(TAG, "LINK LOST, rescanning in 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
#if HID_HOST_MODE == HIDH_IDLE_MODE
    ESP_LOGE(TAG, "Please turn on BT HID host or BLE!");
    return;
#endif
#if BT_VERBOSE_LOG
    // 蓝牙各层详细日志（需要 CONFIG_LOG_MAXIMUM_LEVEL=DEBUG 才编得进去，sdkconfig 已设好）
    esp_log_level_set("BT_BTM",   ESP_LOG_DEBUG);
    esp_log_level_set("BT_SDP",   ESP_LOG_DEBUG);
    esp_log_level_set("BT_L2CAP", ESP_LOG_DEBUG);
    esp_log_level_set("BT_APPL",  ESP_LOG_DEBUG);
    esp_log_level_set("BT_HIDH",  ESP_LOG_DEBUG);
    // 下面两行会打印 HCI 原始报文/转发过程，输出量极大，先别开
    // esp_log_level_set("BT_HCI", ESP_LOG_DEBUG);
    // esp_log_level_set("BT_BTC", ESP_LOG_DEBUG);
#endif

    ESP_LOGI(TAG, "固件构建于 %s %s（对照日志开头的 Compile time，确认烧的是新代码）", __DATE__, __TIME__);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "setting hid gap, mode:%d, scan mask:0x%02x", HID_HOST_MODE, HID_SCAN_MASK);
    ESP_ERROR_CHECK(esp_hid_gap_init(HID_HOST_MODE));
#if CONFIG_BT_BLE_ENABLED
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));
#endif
    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&config));

    char bda_str[18] = {0};
    ESP_LOGI(TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));

    xTaskCreate(&hid_demo_task, "hid_task", 6 * 1024, NULL, 2, NULL);
}

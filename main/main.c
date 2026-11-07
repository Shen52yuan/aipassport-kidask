// main/main.c —— 「Aipassport-KidAsk」：巴掌大、靠手机热点联网的小学生 AI 问答机。
//
// 交互极简：待机（按住 OK 说话）→ 录音 → Ogg Opus 上行 → 服务器 ASR+DeepSeek(儿童
// prompt)+TTS → Ogg Opus 童声下行 + 屏幕要点 → 播放。彻底去除 QCA/PAT 云端实时语音通道。
//
// 按键（全局统一，三键共用 ADC 分压，见 bsp_pins.h）：
//   ▲/▼ 单击   设置页移动选中项（双击 ±2，长按一页）
//   OK  按住   录音（PRESS 开始 / RELEASE 发送；≤10s 自动结束）
//   OK  长按   待机页 / 配置页 → 设备设置；设置页 → 返回
//   OK  单击   错误页 / 回答页 → 返回待机
//
// 线程：app_task 串行跑完整流程（阻塞 HTTPS 只在这里做）；按键回调只入队；界面由
// app_ui 内部持 LVGL 锁绘制。
#include <stdio.h>
#include <string.h>

#include "app_ui.h"
#include "app_wifi.h"
#include "app_portal.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "kid_audio.h"
#include "kid_net.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8
// 户外手机热点：免验证保存后设备重启，用户还需手动打开热点，留出宽松窗口。
// app_wifi_connect 内部有退避重连，热点一出现即拿到 IP 并立即返回。
#define WIFI_TIMEOUT_MS   35000
#define LIST_PAGE_STEP    5

// libopus 的 opus_encode() 会在运行期用 alloca 连续切走约 20KB 栈， HTTPS 阶段
// 又叠加 Ogg/TLS/响应缓冲，逐段记录内存水位以便实机定位瓶颈。
#define LOG_MEM(where) ESP_LOGI(TAG, "[mem] %-10s heap=%u min=%u stack_left=%u", \
    where, (unsigned)esp_get_free_heap_size(), \
    (unsigned)esp_get_minimum_free_heap_size(), \
    (unsigned)uxTaskGetStackHighWaterMark(NULL))

typedef struct {
    bsp_btn_t     btn;
    bsp_btn_ev_t  event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static bool          s_audio_ok;

// 唤醒键守卫：按键唤醒时那颗键在启动期间还按着，先挡住它的动作直到松手（见 key_guard.h）。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 录音停止标志：由按键 RELEASE 置位，ask_should_stop 轮询。
static bool s_record_stop;

// ---------- 按键 ----------
// button 回调运行在共享 esp_timer 任务；只入队并立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // 守卫消费在 readiness 检查之前：松手事件可能在 post-init 电压采样之后、输入分发就绪
    // 之前到达，若在此丢弃会留下守卫持续吞掉下一次真实按压。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    // 按住 OK 说话：PRESS 开始录音、RELEASE 发送；其余语义事件保持原语义。
    if (ev == BSP_BTN_PRESS || ev == BSP_BTN_RELEASE) {
        if (btn == BSP_BTN_OK) {
            const input_event_t in = { .btn = btn, .event = ev };
            xQueueSend(s_input_queue, &in, 0);
        }
        return;
    }
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_DOUBLE && ev != BSP_BTN_LONG) return;
    const input_event_t in = { .btn = btn, .event = ev };
    xQueueSend(s_input_queue, &in, 0);
}

static bool wait_key(input_event_t *out, int timeout_ms) {
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_input_queue, out, t) == pdTRUE;
}

// 阻塞操作（联网、HTTPS）期间按下的键不应在操作结束后“补触发”。
static void flush_keys(void) {
    xQueueReset(s_input_queue);
}

static bool is_ok(const input_event_t *k) {
    return k->btn == BSP_BTN_OK && (k->event == BSP_BTN_CLICK || k->event == BSP_BTN_DOUBLE);
}

static bool is_ok_long(const input_event_t *k) {
    return k->btn == BSP_BTN_OK && k->event == BSP_BTN_LONG;
}

// ▲/▼ 的移动量：单击 1，双击 2，长按一页。OK 返回 0。
static int move_delta(const input_event_t *k) {
    int step = k->event == BSP_BTN_CLICK ? 1
             : k->event == BSP_BTN_DOUBLE ? 2
             : k->event == BSP_BTN_LONG ? LIST_PAGE_STEP : 0;
    if (k->btn == BSP_BTN_UP) return -step;
    if (k->btn == BSP_BTN_DOWN) return step;
    return 0;
}

// 等待任意 OK 动作（PRESS/CLICK/LONG）以关闭错误页。
static void wait_ok_any(void) {
    input_event_t k;
    flush_keys();
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        if (k.btn == BSP_BTN_OK &&
            (k.event == BSP_BTN_PRESS || k.event == BSP_BTN_CLICK || k.event == BSP_BTN_LONG))
            return;
    }
}

// 录音停止检测：排水输入队列，捕获 OK RELEASE；丢弃其余事件避免泄漏到待机页。
static bool ask_should_stop(void) {
    input_event_t k;
    while (xQueueReceive(s_input_queue, &k, 0) == pdTRUE) {
        if (k.btn == BSP_BTN_OK && k.event == BSP_BTN_RELEASE) s_record_stop = true;
    }
    return s_record_stop;
}

static void ask_on_level(int level) { app_ui_set_level(level); }

// ---------- 配网 ----------
static bool settings_menu(bool provisioning);
static void request_provision(void);
static void run_provision(void);
static bool wait_ok_any_restart(void);

// 恢复出厂只能通过设置页的第二次确认，不把进入设置/一次 OK 当作删除授权。
static void confirm_factory_reset(void) {
    bool confirm = false;
    app_ui_show_reset_confirm(confirm);
    flush_keys();
    input_event_t k;
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        const int d = move_delta(&k);
        if (d) {
            confirm = d > 0;
            app_ui_show_reset_confirm(confirm);
        } else if (!confirm && (is_ok(&k) || is_ok_long(&k))) {
            return;
        } else if (confirm && is_ok_long(&k)) {
            esp_err_t e = app_wifi_request_factory_reset();
            // 成功会重启；失败不清除任何配置，也不重启。
            ESP_LOGE(TAG, "重置意图保存失败: %s", esp_err_to_name(e));
            app_ui_show_message(UI_TONE_ERROR, "未能启动恢复出厂", "设置未清除，请稍后重试",
                                "OK 返回", false);
            flush_keys();
            wait_ok_any();
            return;
        }
    }
}

// 设备设置：3 项（重新配置 Wi-Fi / 恢复出厂 / 返回）。
static bool settings_menu(bool provisioning) {
    int selected = 2;   // 默认选中“返回”
    app_ui_show_settings(provisioning, selected);
    flush_keys();
    input_event_t k;
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        const int d = move_delta(&k);
        if (d) {
            selected += d;
            if (selected < 0) selected = 0;
            if (selected > 2) selected = 2;
            app_ui_settings_update(selected);
        } else if (is_ok_long(&k)) {
            return false;   // 长按返回
        } else if (is_ok(&k)) {
            switch (selected) {
            case 0:  // 重新配置 Wi-Fi
                if (provisioning) return false;  // 已在配置，返回显示二维码
                request_provision();
                break;
            case 1:  // 恢复出厂设置
                confirm_factory_reset();
                selected = 2;
                app_ui_show_settings(provisioning, selected);
                flush_keys();
                break;
            default: // 返回
                return false;
            }
        }
    }
}

static void request_provision(void) {
    esp_err_t e = app_wifi_request_provision();
    ESP_LOGE(TAG, "配网标记未保存，原地进入热点: %s", esp_err_to_name(e));
    run_provision(); // 不在失败写入后盲目重启
}

static void run_provision(void) {
    app_ui_show_message(UI_TONE_INFO, "正在启动配置热点", "手机连热点配网", NULL, true);
    if (app_portal_start() != ESP_OK) ESP_LOGE(TAG, "配置热点启动失败");
    app_ui_show_provision();
    flush_keys();
    input_event_t k;
    for (;;) {
        if (wait_key(&k, -1) && is_ok_long(&k)) {
            portal_stage_t st = app_portal_stage();
            if (st == PORTAL_CONNECTING || st == PORTAL_CHECKING || st == PORTAL_DONE) continue;
            (void)settings_menu(true);
            app_ui_show_provision(); // 返回已有门户，不重复创建 HTTP/DNS/worker
            flush_keys();
        }
    }
}

// 新开机专用恢复路径：此时门户、TLS 均未启动。断电/部分失败时保留重置意图，下一次
// 开机继续；不允许带着半清除配置联网。
static void finish_pending_reset(void) {
    for (;;) {
        bool pending = false;
        esp_err_t e = app_wifi_factory_reset_pending(&pending);
        if (e == ESP_OK && !pending) return;
        if (e == ESP_OK) {
            app_ui_show_message(UI_TONE_WARN, "正在恢复出厂设置", "请勿断电，完成后重新配置",
                                NULL, true);
            e = app_wifi_finish_factory_reset();
            if (e == ESP_OK) {
                app_ui_show_message(UI_TONE_OK, "设置已清除", "即将重启并开启配置热点", NULL, false);
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
        }
        ESP_LOGE(TAG, "恢复出厂未完成: %s", esp_err_to_name(e));
        app_ui_show_message(UI_TONE_ERROR, "设置清除未完成", "不会进入正常联网，请重试",
                            "OK 重试  长按 OK 重启", false);
        flush_keys();
        if (!wait_ok_any_restart()) esp_restart();
    }
}

// wait_ok_any 的变体：返回 false 表示用户长按（要求重启），true 表示普通 OK（重试）。
static bool wait_ok_any_restart(void) {
    input_event_t k;
    flush_keys();
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        if (k.btn == BSP_BTN_OK && k.event == BSP_BTN_LONG) return false;
        if (k.btn == BSP_BTN_OK &&
            (k.event == BSP_BTN_PRESS || k.event == BSP_BTN_CLICK)) return true;
    }
}

// ---------- 联网 ----------
static void ensure_wifi(void) {
    if (app_wifi_is_connected()) return;
    char ssid[33];
    app_wifi_get_ssid(ssid, sizeof(ssid));
    char body[80];
    snprintf(body, sizeof(body), "网络：%s", ssid);
    app_ui_show_message(UI_TONE_INFO, "正在连接 Wi-Fi", body, NULL, true);
    esp_err_t e = app_wifi_connect(WIFI_TIMEOUT_MS);
    if (e == ESP_OK) return;
    ESP_LOGW(TAG, "Wi-Fi 未联网，进入配置热点: %s", esp_err_to_name(e));
    app_ui_show_message(UI_TONE_WARN, "Wi-Fi 未连接", "长按 OK 进入配置", "长按 OK 设置", false);
    flush_keys();
    input_event_t k;
    for (;;) {
        if (!wait_key(&k, -1)) continue;
        if (k.btn == BSP_BTN_OK && k.event == BSP_BTN_LONG) run_provision();
    }
}

// ---------- 一次问答：录音 → 上传 → 播放 ----------
static void show_error(const char *msg) {
    // v0.1.9：错误页补一个听觉反馈（网络/限流类错误仍以屏幕文字为主；
    // 「没听清请补充」的童声语音引导由服务端 X-Need-Retry 分支提供）。
    kid_audio_beep(2);
    app_ui_set_state(KID_UI_ERROR, msg);
    wait_ok_any();
}

static void run_ask(void) {
    ESP_LOGI(TAG, "开始问答: 录音 → 上传 → 播放");
    LOG_MEM("ask_start");
    app_ui_set_state(KID_UI_RECORD, NULL);
    s_record_stop = false;

    uint8_t *ogg = NULL;
    size_t   ogg_len = 0;
    esp_err_t re = kid_audio_record_ogg(&ogg, &ogg_len, ask_should_stop, ask_on_level);
    if (re == ESP_ERR_INVALID_STATE) {
        free(ogg);
        app_ui_toast("没听清，再试一次");
        flush_keys();
        return;
    }
    if (re != ESP_OK) {
        free(ogg);
        show_error("录音出问题了");
        return;
    }
    LOG_MEM("rec_done");

    // 上行 → 服务器。
    app_ui_set_state(KID_UI_THINK, NULL);
    kid_resp_t resp = {0};
    LOG_MEM("pre_http");
    esp_err_t pe = kid_net_post_ogg(ogg, ogg_len, &resp);
    free(ogg);
    LOG_MEM("http_done");
    flush_keys();

    if (pe != ESP_OK) {
        show_error("网络开小差了");
        return;
    }

    if (resp.status == 200 && resp.audio && resp.audio_len) {
        const bool need_retry = resp.need_retry;
        // 屏幕要点：优先答案，退而显示识别到的问题，最后兜底占位，保证卡片不空白。
        // need_retry 时服务端下发的 answer 本身就是引导语（如「我没听清楚…」），
        // 直接显示即可；孩子听完提示音回待机，按 OK 再说一遍。
        const char *card = resp.answer_len ? resp.answer
                         : (resp.asr_len ? resp.asr : "正在读给你听~");
        ESP_LOGI(TAG, "屏幕要点 %u 字节 / ASR %u 字节 / 需补充=%d(%s)",
                 (unsigned)resp.answer_len, (unsigned)resp.asr_len,
                 need_retry ? 1 : 0,
                 resp.retry_reason[0] ? resp.retry_reason : "-");
        app_ui_set_state(KID_UI_ANSWER, card);
        kid_audio_play_ogg(resp.audio, resp.audio_len);
        LOG_MEM("play_done");
        kid_resp_free(&resp);
        // need_retry：播完提示音就回待机等待补充。session id 不变，下一轮 POST
        // 会带着这一轮的引导语作为上下文，服务端据此理解孩子的补充说明。
        // 正常答案则多留 3 秒让孩子/家长看清屏幕文字。
        vTaskDelay(pdMS_TO_TICKS(need_retry ? 2000 : 3000));
        return;
    }

    // 业务错误码映射（端点约定：422 再说一次 / 502·500 没听清 / 503 稍等）。
    if (resp.status == 422) {
        show_error("再说一次好吗");
    } else if (resp.status == 503) {
        show_error("稍等一下下");
    } else if (resp.status == 502 || resp.status == 500) {
        show_error("没听清，再问一次");
    } else {
        show_error(resp.answer_len ? resp.answer : "出了点小问题");
    }
    kid_resp_free(&resp);
}

// ---------- 主流程 ----------
static void app_task(void *arg) {
    (void)arg;
    esp_err_t e = app_wifi_init(true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(e));
        app_ui_show_message(UI_TONE_ERROR, "Wi-Fi 初始化失败", esp_err_to_name(e),
                            "长按 OK 重启", false);
        while (!wait_ok_any_restart()) {}
        esp_restart();
    }

    finish_pending_reset(); // 必须先完成已确认的清除，才允许联网/启动门户
    const bool want_prov = app_wifi_take_provision_flag();
    app_wifi_restore_creds();
    if (want_prov || !app_wifi_has_config()) {
        run_provision();   // 不返回（成功重启 / 失败循环）
    }

    for (;;) {
        ensure_wifi();
        app_ui_set_state(KID_UI_IDLE, NULL);
        ESP_LOGI(TAG, "待机就绪, app 栈历史最低余量=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));

        input_event_t k;
        bool rec = false;
        flush_keys();
        while (wait_key(&k, -1)) {
            if (k.btn == BSP_BTN_OK && k.event == BSP_BTN_PRESS) { rec = true; break; }
            if (k.btn == BSP_BTN_OK && k.event == BSP_BTN_LONG) { settings_menu(false); break; }
            // 待机页其余按键给一次可见反馈，避免被误认为死机。
            app_ui_toast("按住 OK 键说话");
        }
        if (!rec) continue;   // 设置完成 → 重新显示待机页
        run_ask();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "KidAsk 问答机 启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    // 按键唤醒时,那颗键在整个启动期间还按着。先挡住它的动作(见 on_key)。
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    app_ui_init();
    app_ui_show_message(UI_TONE_INFO, "正在启动", "KidAsk 问答机", NULL, true);

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列分配失败，停止启动");
        app_ui_show_message(UI_TONE_ERROR, "内存不足", "请重新启动设备", NULL, false);
        return;
    }
    esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
    } else if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。初始化完成后按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }
    s_audio_ok = (bsp_audio_init() == ESP_OK);
    if (!s_audio_ok) {
        ESP_LOGE(TAG, "音频初始化失败");
    } else {
        // BSP 内部音量默认 0（静音），必须显式设置；后续每次 codec open 都会沿用。
        bsp_audio_set_volume(KID_VOLUME);
        ESP_LOGI(TAG, "输出音量已设为 %d%%", KID_VOLUME);
        // v0.1.8: 开机播 1 秒 1kHz 自检音，不依赖网络/按键，用来确认
        // ES8311 输出级是否真的在出声；同时回读 DAC 音量寄存器。
        kid_audio_selftest();
    }
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用");
    s_input_ready = true;

    // v0.1.9：一次开机生成一个会话 id，整段连续对话共用；重启即开新话题。
    kid_net_init_session();

    // 16KB：本任务串行跑「录音 → libopus 编码 → HTTPS/TLS 握手 → Opus 解码播放」，
    // 8192 字节在按住 OK 进入编码时触发过 Stack protection fault（v0.1.2 实机日志）。
    // libopus 的 SILK/CELT 编码链用 alloca 动态切栈（实测峰值 >20KB），16KB 会触发
    // "Stack protection fault" 整机重启；32KB 留出余量，堆侧由 LOG_MEM 逐段监控。
    if (xTaskCreate(app_task, "app", 32768, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败");
    }
    ESP_LOGI(TAG, "就绪:Button=%d Audio=%d 剩余堆 %u", button_err == ESP_OK, s_audio_ok,
             (unsigned)esp_get_free_heap_size());
}

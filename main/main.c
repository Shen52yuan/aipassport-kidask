// main/main.c —— 「Aipassport-KidAsk」：巴掌大、靠手机热点联网的小学生 AI 问答机。
//
// 交互极简（v0.1.10 起改为「一按一说」）：待机按一下 OK 开始录音 → 说完再按一下
// OK 发送 → Ogg Opus 上行 → 服务器 ASR+DeepSeek(儿童 prompt)+TTS → Ogg Opus 童声
// 下行 + 屏幕要点 → 播放。彻底去除 QCA/PAT 云端实时语音通道。
//
// 按键（全局统一，三键共用 ADC 分压，见 bsp_pins.h）：
//   ▲/▼ 单击   设置页移动选中项（双击 ±2，长按一页）；录音中 → 取消本次录音
//   OK  单击   待机页 → 开始录音；录音中 → 结束并发送；错误页 → 返回待机
//   OK  长按   待机页 / 配置页 → 设备设置；设置页 → 返回
//   录音上限   ≤10s 自动结束并发送（不用一直按住，孩子可以自然说话）
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
#include "esp_heap_caps.h"
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
// v0.1.11：补最大连续空闲块。总量够、连续块不足正是连续多轮后分配失败的真凶，
// 只看 heap/min 无法区分这两者。
#define LOG_MEM(where) ESP_LOGI(TAG, "[mem] %-10s heap=%u min=%u largest=%u stack_left=%u", \
    where, (unsigned)esp_get_free_heap_size(), \
    (unsigned)esp_get_minimum_free_heap_size(), \
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), \
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

// v0.1.10：录音停止/取消标志。停止＝第二次按 OK（发送）；取消＝▲/▼ 或长按（丢弃）。
static volatile bool s_record_stop;
static volatile bool s_record_cancel;

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

// v0.1.10 录音停止检测：排水输入队列，捕获「第二次按 OK」并发送。
// 不再以 RELEASE 为停止信号 —— 那样等于「按住说话」，孩子必须一直按着，
// 松手瞬间还会切掉尾字。现在第一次按 OK 只是开录，松手继续录，第二次按下才收尾。
static bool ask_should_stop(void) {
    input_event_t k;
    while (xQueueReceive(s_input_queue, &k, 0) == pdTRUE) {
        if (k.event == BSP_BTN_LONG) {
            // 长按任意键：放弃本次录音（长按 OK 在待机页是「设备设置」，不抢它的语义）
            s_record_cancel = true;
        } else if (k.btn == BSP_BTN_OK) {
            // PRESS 一到就收尾，响应最快；CLICK/DOUBLE 兜住不同驱动的事件投递差异。
            if (k.event == BSP_BTN_PRESS || k.event == BSP_BTN_CLICK ||
                k.event == BSP_BTN_DOUBLE) s_record_stop = true;
        } else if (k.btn == BSP_BTN_UP || k.btn == BSP_BTN_DOWN) {
            s_record_cancel = true;
        }
        // 其余事件（RELEASE 等）一律丢弃，避免泄漏到待机页。
    }
    return s_record_stop || s_record_cancel;
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
    // 三声短「嘀」：错误页必须给听觉反馈，否则一年级孩子根本不会低头看屏。
    // 注意这不是语音；「没听清请补充」的童声引导由服务端 X-Need-Retry 分支提供。
    kid_audio_beep(3);
    app_ui_set_state(KID_UI_ERROR, msg);
    wait_ok_any();
}

static void run_ask(void) {
    ESP_LOGI(TAG, "开始问答: 按 OK 开录 → 再按 OK 发送 → 上传 → 播放");
    LOG_MEM("ask_start");
    app_ui_set_state(KID_UI_RECORD, NULL);
    s_record_stop   = false;
    s_record_cancel = false;
    // 触发本次录音的那次短按会连带投递 PRESS/RELEASE/CLICK，先排空，否则
    // 录音一开始就会被同一次按键的残留事件立刻停掉。
    flush_keys();

    uint8_t *ogg = NULL;
    size_t   ogg_len = 0;
    esp_err_t re = kid_audio_record_ogg(&ogg, &ogg_len, ask_should_stop, ask_on_level);
    if (s_record_cancel) {
        free(ogg);
        ESP_LOGI(TAG, "本次录音已取消");
        app_ui_toast("已取消，按 OK 重新说");
        flush_keys();
        return;
    }
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

    // v0.1.10：服务端日志证实每次都 200 且完整下发了音频（12KB~56KB），但设备
    // 偶发「有文案、没声音」。旧代码有两个静默失败点：kid_net_post_ogg() 在
    // transport err 时把已收到的 body 全部 free；kid_audio_play_ogg() 的返回值
    // 无人检查。现在以「音频完整 + 解码播放成功」为成功条件，缺失自动重发一次，
    // 并把每次实收/期望/传输状态打进串口，杜绝哑掉。
    for (int attempt = 0; attempt < 2; attempt++) {
        app_ui_set_state(KID_UI_THINK, NULL);
        kid_resp_t resp = {0};
        LOG_MEM("pre_http");
        esp_err_t pe = kid_net_post_ogg(ogg, ogg_len, &resp);
        LOG_MEM("http_done");
        flush_keys();

        const bool complete  = (resp.content_len == 0) ||
                               (resp.audio_len >= resp.content_len);
        // v0.1.11：body 必须是真 Ogg。传输异常时保留下来的可能是被污染的半截数据，
        // 长度够但内容错，交给解封装器只会读出匪夷所思的 nseg 与 CRC 垃圾。
        const bool ogg_magic = resp.audio && resp.audio_len >= 64 &&
                               memcmp(resp.audio, "OggS", 4) == 0;
        const bool has_audio = resp.status == 200 && ogg_magic && complete;
        ESP_LOGI(TAG, "第%d次应答: pe=%s status=%d 音频=%uB/%uB 完整=%d Ogg=%d 文案=%uB 补充=%d(%s)",
                 attempt + 1, esp_err_to_name(pe), resp.status,
                 (unsigned)resp.audio_len, (unsigned)resp.content_len, complete ? 1 : 0,
                 ogg_magic ? 1 : 0,
                 (unsigned)resp.answer_len, resp.need_retry ? 1 : 0,
                 resp.retry_reason[0] ? resp.retry_reason : "-");
        if (resp.answer_len) ESP_LOGI(TAG, "屏幕文案: %s", resp.answer);

        if (has_audio) {
            // v0.1.11 关键：播放前立刻释放上行录音 Ogg（10s 录音约 20KB）。
            // 旧代码把它留到播放之后，播放瞬间同时驻留「上行 Ogg + 下行 Ogg(≤56KB)
            // + 解码缓冲 + 解码器」，是堆峰值与分配失败的直接来源。
            free(ogg);
            ogg = NULL;

            // 屏幕要点：优先答案，退而显示识别到的问题，最后兜底占位，保证卡片不空白。
            // need_retry 时服务端下发的 answer 本身就是引导语（如「我没听清楚…」）。
            const char *card = resp.answer_len ? resp.answer
                             : (resp.asr_len ? resp.asr : "正在读给你听~");
            app_ui_set_state(KID_UI_ANSWER, card);
            esp_err_t play = kid_audio_play_ogg(resp.audio, resp.audio_len);
            if (play == ESP_ERR_NO_MEM || play == ESP_ERR_INVALID_STATE ||
                play == ESP_ERR_INVALID_ARG) {
                // 这几类失败都发生在任何 PCM 写出之前，本地重试一次不会造成重播。
                ESP_LOGW(TAG, "首次播放未启动(%s)，300ms 后本地重试一次", esp_err_to_name(play));
                vTaskDelay(pdMS_TO_TICKS(300));
                play = kid_audio_play_ogg(resp.audio, resp.audio_len);
            }
            LOG_MEM("play_done");
            const bool need_retry = resp.need_retry;
            kid_resp_free(&resp);
            if (play == ESP_OK) {
                // need_retry：播完童声引导回待机等补充，session 不变，下一轮 POST
                // 带着这一轮引导语作为上下文来理解孩子的补充说明。
                vTaskDelay(pdMS_TO_TICKS(need_retry ? 2000 : 3000));
                return;
            }
            ESP_LOGE(TAG, "下行 Ogg 解码/播放失败: %s", esp_err_to_name(play));
            show_error("声音没放出来，按 OK 再问一次");
            return;
        }

        const int st = resp.status;
        kid_resp_free(&resp);
        if (attempt == 0 && (st == 200 || st == 0)) {
            // 传输被截断 / 连接提前关闭：同一段录音原样重发一次，服务端重新生成
            // 答案音频。只在异常路径发生，不影响正常问答耗时。
            ESP_LOGW(TAG, "下行音频缺失或不完整，0.5s 后自动重发一次");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        free(ogg);
        if (st == 0 || st == 200) {
            show_error("声音没收到，按 OK 再问一次");
        } else if (st == 422) {
            show_error("再说一次好吗");
        } else if (st == 503) {
            show_error("稍等一下下");
        } else if (st == 502 || st == 500) {
            show_error("没听清，再问一次");
        } else {
            show_error("出了点小问题");
        }
        return;
    }
    free(ogg);
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
            if (k.btn == BSP_BTN_OK && k.event == BSP_BTN_LONG) { settings_menu(false); break; }
            // v0.1.10：以「完整短按」CLICK 开录，不用 PRESS —— 否则长按进设置会
            // 先被 PRESS 抢走变成一次录音。PRESS/RELEASE 在待机页一律忽略。
            if (k.btn == BSP_BTN_OK &&
                (k.event == BSP_BTN_CLICK || k.event == BSP_BTN_DOUBLE)) { rec = true; break; }
            if (k.btn == BSP_BTN_OK &&
                (k.event == BSP_BTN_PRESS || k.event == BSP_BTN_RELEASE)) continue;
            // 待机页其余按键给一次可见反馈，避免被误认为死机。
            app_ui_toast("按一下 OK 开始说话");
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

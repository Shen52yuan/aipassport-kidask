// main/app_ui.h —— 「Aipassport-KidAsk」界面（240x320 竖屏，圆角 30）。
//
// 所有函数内部自行获取 LVGL 锁，可在应用任务里直接调用；不要在持锁时调用。
// 主交互：待机(按住 OK 说话) → 录音(聆听) → 思考 → 回答(童声 + 屏幕要点)。
#pragma once

#include <stdint.h>

#define APP_UI_LIST_ROWS 5

typedef enum {
    UI_TONE_INFO = 0,   // 蓝灰：进行中
    UI_TONE_OK,         // 青绿：完成/就绪
    UI_TONE_WARN,       // 琥珀：需要用户操作
    UI_TONE_ERROR,      // 红：失败
} ui_tone_t;

// 主交互状态（呼吸圈 + 状态词 + 要点卡片）。
typedef enum {
    KID_UI_IDLE = 0,    // 待机：按住 OK 说话
    KID_UI_RECORD,      // 聆听中
    KID_UI_THINK,       // 思考中
    KID_UI_ANSWER,      // 回答中（text 为屏幕要点）
    KID_UI_ERROR,       // 出错/离线提示
} kid_ui_state_t;

void app_ui_init(void);

// 通用提示页：一个色点 + 标题 + 正文 + 底部按键提示。spinner=true 时显示转圈。
void app_ui_show_message(ui_tone_t tone, const char *title, const char *body,
                         const char *hint, bool spinner);

// 主交互页：根据状态显示呼吸圈与提示；ANSWER 时 text 为屏幕要点（X-Answer-Text，≤120 字）。
void app_ui_set_state(kid_ui_state_t st, const char *text);
// 录音电平（0..100），由音频线程周期性喂入，驱动聆听页呼吸圈跳动；-1 表示本次无需更新。
void app_ui_set_level(int level);
// 短暂覆盖要点卡片（如错误码/状态），1.5s 后恢复。
void app_ui_toast(const char *text);

// 热点配置页：显示连接二维码、临时密码、网页地址；轮询 app_portal_stage()。
void app_ui_show_provision(void);

// 设备设置：默认选中返回；热点页首项为取消配置并重启。
void app_ui_show_settings(bool provisioning, int selected);
void app_ui_settings_update(int selected);
// 恢复出厂二次确认：默认取消，选中清除后仍需长按 OK 才执行。
void app_ui_show_reset_confirm(bool confirm_selected);

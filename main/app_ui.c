// main/app_ui.c —— 见 app_ui.h。
//
// 视觉语言：深海军蓝底 + 单一状态色。主交互页上部为呼吸圈（颜色随状态，直径随电平跳动），
// 下方为状态词与操作提示，底部为要点卡片（ANSWER 时显示屏幕要点）。
#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include "app_fonts.h"
#include "app_portal.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"

#define SCR_W 240
#define SCR_H 320
#define PAD   16

#define C_BG        0x0B1020
#define C_CARD      0x161D33
#define C_CARD_SEL  0x123B3F
#define C_TEXT      0xE2E8F0
#define C_DIM       0x94A3B8
#define C_FAINT     0x475569
#define C_TEAL      0x2DD4BF
#define C_VIOLET    0xA78BFA
#define C_SKY       0x38BDF8
#define C_AMBER     0xF59E0B
#define C_RED       0xF87171

static lv_obj_t *s_scr;
static lv_obj_t *s_batt;
static lv_timer_t *s_batt_timer;
static lv_timer_t *s_page_timer;   // 当前页的轮询定时器（主交互 / 配网）

// 主交互页
static lv_obj_t *s_kid_halo;
static lv_obj_t *s_kid_ring;
static lv_obj_t *s_kid_disc;
static int s_kid_cx, s_kid_cy;
static lv_obj_t *s_kid_word;
static lv_obj_t *s_kid_guide;
static lv_obj_t *s_kid_line;
static kid_ui_state_t s_kid_state;
static uint32_t s_kid_last_word;
static uint32_t s_kid_toast_until;
static int s_kid_level_target;
static int s_kid_level_smooth;
static int s_kid_backlight = -1;

// 配网页
static lv_obj_t *s_prov_status;
static portal_stage_t s_prov_last;

// 设置页
static lv_obj_t *s_rows_set[3];

static uint32_t tone_color(ui_tone_t t) {
    switch (t) {
    case UI_TONE_OK:    return C_TEAL;
    case UI_TONE_WARN:  return C_AMBER;
    case UI_TONE_ERROR: return C_RED;
    default:            return C_SKY;
    }
}

static uint32_t kid_state_color(kid_ui_state_t st) {
    switch (st) {
    case KID_UI_RECORD:  return C_TEAL;
    case KID_UI_THINK:   return C_VIOLET;
    case KID_UI_ANSWER:  return C_SKY;
    case KID_UI_ERROR:   return C_RED;
    default:             return C_TEAL;
    }
}

static const char *kid_state_word(kid_ui_state_t st) {
    switch (st) {
    case KID_UI_RECORD:  return "聆听";
    case KID_UI_THINK:   return "思考";
    case KID_UI_ANSWER:  return "回答";
    case KID_UI_ERROR:   return "提示";
    default:             return "待机";
    }
}

static const char *kid_state_guide(kid_ui_state_t st) {
    switch (st) {
    case KID_UI_IDLE:    return "按住 OK 键说话";
    case KID_UI_RECORD:  return "我在听…松手发送";
    case KID_UI_THINK:   return "小问号正在想…";
    case KID_UI_ANSWER:  return "正在读给你听";
    case KID_UI_ERROR:   return "说“再说一次”重试";
    default:             return "";
    }
}

static void set_backlight(int pct) {
    if (pct == s_kid_backlight) return;
    s_kid_backlight = pct;
    bsp_display_backlight((uint8_t)pct);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static lv_obj_t *mk_box(lv_obj_t *parent, int w, int h, uint32_t bg, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// ---------- 电池 ----------
static void battery_refresh(void) {
    if (!s_batt) return;
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *sym = soc >= 85 ? LV_SYMBOL_BATTERY_FULL
                    : soc >= 60 ? LV_SYMBOL_BATTERY_3
                    : soc >= 35 ? LV_SYMBOL_BATTERY_2
                    : soc >= 12 ? LV_SYMBOL_BATTERY_1
                                : LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(s_batt, "%d%% %s", soc, sym);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(soc < 15 ? C_RED : C_DIM), 0);
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
}

static void battery_timer_cb(lv_timer_t *t) {
    (void)t;
    battery_refresh();
}

// ---------- 页面框架 ----------
static void stop_page_timer(void) {
    if (s_page_timer) {
        lv_timer_delete(s_page_timer);
        s_page_timer = NULL;
    }
}

static lv_obj_t *page_begin(void) {
    stop_page_timer();
    s_kid_halo = s_kid_ring = s_kid_disc = s_kid_word = s_kid_guide = s_kid_line = NULL;
    s_prov_status = NULL;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, app_font_text(), 0);

    lv_obj_t *mark = mk_box(scr, 8, 8, C_TEAL, 2);
    lv_obj_set_pos(mark, PAD + 4, 15);
    lv_obj_t *title = mk_label(scr, app_font_text(), C_TEXT, "KidAsk 问答机");
    lv_obj_set_pos(title, PAD + 18, 8);

    s_batt = mk_label(scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -(PAD + 4), 11);
    battery_refresh();

    lv_obj_t *rule = mk_box(scr, SCR_W - 2 * PAD, 1, C_CARD, 0);
    lv_obj_set_pos(rule, PAD, 34);
    return scr;
}

static void page_commit(lv_obj_t *scr) {
    lv_obj_t *old = s_scr;
    lv_screen_load(scr);
    s_scr = scr;
    if (old) lv_obj_delete(old);
}

void app_ui_init(void) {
    app_fonts_init();
    if (!bsp_lvgl_lock(1000)) return;
    s_batt_timer = lv_timer_create(battery_timer_cb, 10000, NULL);
    set_backlight(100);
    bsp_lvgl_unlock();
}

// ---------- 提示页 ----------
void app_ui_show_message(ui_tone_t tone, const char *title, const char *body,
                         const char *hint, bool spinner) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    const uint32_t c = tone_color(tone);

    if (spinner) {
        lv_obj_t *sp = lv_spinner_create(scr);
        lv_spinner_set_anim_params(sp, 1000, 270);
        lv_obj_set_size(sp, 64, 64);
        lv_obj_set_style_arc_width(sp, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, lv_color_hex(c), LV_PART_INDICATOR);
        lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 72);
    } else {
        lv_obj_t *halo = mk_box(scr, 64, 64, c, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(halo, LV_OPA_20, 0);
        lv_obj_align(halo, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_t *dot = mk_box(halo, 24, 24, c, LV_RADIUS_CIRCLE);
        lv_obj_center(dot);
    }

    lv_obj_t *t = mk_label(scr, app_font_text(), C_TEXT, title);
    lv_obj_set_width(t, SCR_W - 2 * PAD - 8);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 152);

    lv_obj_t *b = mk_label(scr, app_font_text(), C_DIM, body);
    lv_obj_set_width(b, SCR_W - 2 * PAD - 8);
    lv_obj_set_height(b, 100);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(b, 2, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 182);

    if (hint && hint[0]) {
        lv_obj_t *h = mk_label(scr, app_font_text(), C_TEAL, hint);
        lv_obj_set_width(h, SCR_W - 2 * PAD - 16);
        lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -12);
    }
    page_commit(scr);
    bsp_lvgl_unlock();
}

// ---------- 主交互页（呼吸圈 + 状态词 + 要点卡片）----------
#define HALO_MAX    64
#define RING_MIN    38
#define RING_MAX    56
#define DISC_D      26
#define STATUS_Y    78
#define WORD_W      84
#define STATUS_GAP  12
#define CARD_Y      170
#define CARD_H      112
#define CARD_PAD_X  8
#define CARD_PAD_Y  6
#define KID_TICK_MS 50

static int tri_wave(uint32_t ms, uint32_t period) {
    uint32_t p = ms % period;
    uint32_t half = period / 2;
    return (int)(p < half ? p * 100 / half : (period - p) * 100 / half);
}

static int breath(uint32_t ms, uint32_t period) {
    const int t = tri_wave(ms, period);
    return t * t * (300 - 2 * t) / 10000;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void place_circle(lv_obj_t *o, int d) {
    d &= ~1;
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, s_kid_cx - d / 2, s_kid_cy - d / 2);
}

static void make_single_line(lv_obj_t *l, const lv_font_t *font, int w) {
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void kid_timer_cb(lv_timer_t *tm) {
    (void)tm;
    if (!s_kid_ring) return;
    const uint32_t now = lv_tick_get();
    const uint32_t c = kid_state_color(s_kid_state);

    // 电平平滑：上升快、下降慢。
    if (s_kid_level_target > s_kid_level_smooth) s_kid_level_smooth = (s_kid_level_smooth + s_kid_level_target * 3) / 4;
    else s_kid_level_smooth = (s_kid_level_smooth * 7 + s_kid_level_target) / 8;

    if (s_kid_state != s_kid_last_word) {
        s_kid_last_word = s_kid_state;
        lv_obj_set_style_border_color(s_kid_ring, lv_color_hex(c), 0);
        lv_obj_set_style_bg_color(s_kid_halo, lv_color_hex(c), 0);
        lv_obj_set_style_bg_color(s_kid_disc, lv_color_hex(c), 0);
        lv_label_set_text(s_kid_word, kid_state_word(s_kid_state));
        lv_obj_set_style_text_color(s_kid_word, lv_color_hex(c), 0);
        lv_label_set_text(s_kid_guide, kid_state_guide(s_kid_state));
        set_backlight(s_kid_state == KID_UI_ERROR ? 60 : 100);
    }

    const int lvl = s_kid_level_smooth;
    int b, ring_d, ring_w, ring_opa, halo_d, halo_opa, disc_d, disc_opa;
    switch (s_kid_state) {
    case KID_UI_RECORD:
        b = breath(now, 1500);
        ring_d = 40 + b * 10 / 100 + lvl * 8 / 100;
        ring_w = 2 + lvl * 4 / 100;
        ring_opa = 150 + b * 105 / 100;
        halo_d = ring_d + 4 + b * 8 / 100 + lvl * 4 / 100;
        halo_opa = 20 + b * 60 / 100 + lvl * 30 / 100;
        disc_d = DISC_D - 2 + b * 6 / 100;
        disc_opa = 60 + b * 110 / 100 + lvl * 60 / 100;
        break;
    case KID_UI_THINK:
        b = breath(now, 800);
        ring_d = 40 + b * 8 / 100;
        ring_w = 3;
        ring_opa = 120 + b * 135 / 100;
        halo_d = ring_d + 2 + b * 10 / 100;
        halo_opa = b * 70 / 100;
        disc_d = DISC_D - 2 + b * 4 / 100;
        disc_opa = 60 + b * 120 / 100;
        break;
    case KID_UI_ANSWER:
        b = breath(now, 1100);
        ring_d = 42 + b * 6 / 100;
        ring_w = 3;
        ring_opa = 255;
        halo_d = ring_d + 6 + b * 6 / 100;
        halo_opa = 30 + b * 30 / 100;
        disc_d = DISC_D - 2 + b * 4 / 100;
        disc_opa = 110 + b * 60 / 100;
        break;
    case KID_UI_ERROR:
        b = breath(now, 3600);
        ring_d = 40 + b * 4 / 100;
        ring_w = 2;
        ring_opa = 90 + b * 120 / 100;
        halo_d = ring_d + 2;
        halo_opa = b * 25 / 100;
        disc_d = DISC_D - 2;
        disc_opa = 40 + b * 60 / 100;
        break;
    default:
        b = breath(now, 3600);
        ring_d = 40 + b * 4 / 100;
        ring_w = 2;
        ring_opa = 90 + b * 120 / 100;
        halo_d = ring_d + 2;
        halo_opa = b * 25 / 100;
        disc_d = DISC_D - 2;
        disc_opa = 40 + b * 60 / 100;
        break;
    }
    ring_d = clampi(ring_d, RING_MIN, RING_MAX);
    halo_d = clampi(halo_d, ring_d, HALO_MAX);
    disc_d = clampi(disc_d, 20, ring_d - 2 * ring_w - 4);
    place_circle(s_kid_halo, halo_d);
    place_circle(s_kid_ring, ring_d);
    place_circle(s_kid_disc, disc_d);
    lv_obj_set_style_bg_opa(s_kid_halo, (lv_opa_t)clampi(halo_opa, 0, 110), 0);
    lv_obj_set_style_border_width(s_kid_ring, clampi(ring_w, 1, 6), 0);
    lv_obj_set_style_border_opa(s_kid_ring, (lv_opa_t)clampi(ring_opa, 0, 255), 0);
    lv_obj_set_style_bg_opa(s_kid_disc, (lv_opa_t)clampi(disc_opa, 0, 230), 0);
}

void app_ui_set_state(kid_ui_state_t st, const char *text) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();

    const uint32_t c = kid_state_color(st);
    const int group_x = (SCR_W - (HALO_MAX + STATUS_GAP + WORD_W)) / 2;
    s_kid_cx = group_x + HALO_MAX / 2;
    s_kid_cy = STATUS_Y + HALO_MAX / 2;

    s_kid_halo = mk_box(scr, RING_MIN, RING_MIN, C_DIM, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_kid_halo, LV_OPA_TRANSP, 0);
    place_circle(s_kid_halo, RING_MIN);

    s_kid_ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_kid_ring);
    lv_obj_set_style_radius(s_kid_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_kid_ring, lv_color_hex(c), 0);
    lv_obj_set_style_border_width(s_kid_ring, 2, 0);
    lv_obj_remove_flag(s_kid_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    place_circle(s_kid_ring, RING_MIN + 2);

    s_kid_disc = mk_box(scr, DISC_D, DISC_D, C_DIM, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(s_kid_disc, LV_OPA_20, 0);
    place_circle(s_kid_disc, DISC_D - 2);

    s_kid_word = mk_label(scr, app_font_big(), C_DIM, kid_state_word(st));
    lv_obj_set_width(s_kid_word, WORD_W);
    lv_obj_set_pos(s_kid_word, group_x + HALO_MAX + STATUS_GAP, s_kid_cy - 14);

    s_kid_guide = mk_label(scr, app_font_text(), C_TEXT, kid_state_guide(st));
    make_single_line(s_kid_guide, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_align(s_kid_guide, LV_ALIGN_TOP_MID, 0, STATUS_Y + HALO_MAX + 4);

    // 要点卡片：ANSWER 时显示 text；其余显示操作提示。
    lv_obj_t *card = mk_box(scr, SCR_W - 2 * PAD, CARD_H, C_CARD, 12);
    lv_obj_set_pos(card, PAD, CARD_Y);
    s_kid_line = mk_label(card, app_font_text(), C_DIM, "");
    lv_obj_set_size(s_kid_line, SCR_W - 2 * PAD - 2 * CARD_PAD_X, CARD_H - 2 * CARD_PAD_Y);
    lv_label_set_long_mode(s_kid_line, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_kid_line, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_kid_line, CARD_PAD_X, CARD_PAD_Y);

    if (st == KID_UI_ANSWER && text && text[0]) {
        lv_label_set_text(s_kid_line, text);
        lv_obj_set_style_text_color(s_kid_line, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_align(s_kid_line, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(s_kid_line, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_height(s_kid_line, CARD_H - 2 * CARD_PAD_Y);
    } else if (st == KID_UI_ERROR && text && text[0]) {
        lv_label_set_text(s_kid_line, text);
        lv_obj_set_style_text_color(s_kid_line, lv_color_hex(C_RED), 0);
    }

    s_kid_state = st;
    s_kid_last_word = (uint32_t)-1;
    s_kid_toast_until = 0;
    s_kid_level_target = 0;
    s_kid_level_smooth = 0;
    page_commit(scr);
    s_page_timer = lv_timer_create(kid_timer_cb, KID_TICK_MS, NULL);
    bsp_lvgl_unlock();
}

void app_ui_set_level(int level) {
    if (level >= 0) s_kid_level_target = level > 100 ? 100 : level;
}

void app_ui_toast(const char *text) {
    if (!bsp_lvgl_lock(500)) return;
    if (s_kid_line) {
        lv_label_set_text(s_kid_line, text);
        lv_obj_set_style_text_color(s_kid_line, lv_color_hex(C_AMBER), 0);
        s_kid_toast_until = lv_tick_get() + 1500;
        if (!s_kid_toast_until) s_kid_toast_until = 1;
    }
    bsp_lvgl_unlock();
}

// ---------- 热点配置页 ----------
static void prov_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_prov_status) return;
    const portal_stage_t st = app_portal_stage();
    if (st == s_prov_last) return;
    s_prov_last = st;
    lv_label_set_text(s_prov_status, app_portal_text(st));
    const bool failed = (st >= PORTAL_WIFI_FAILED && st <= PORTAL_SAVE_FAILED) || st == PORTAL_ERROR;
    lv_obj_set_style_text_color(s_prov_status,
        lv_color_hex(st == PORTAL_DONE ? C_TEAL : failed ? C_RED : C_AMBER), 0);
}

void app_ui_show_provision(void) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    lv_obj_t *t = mk_label(scr, app_font_text(), C_TEAL, "配置网络");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 45);

    if (app_portal_stage() != PORTAL_ERROR) {
        // 口令固定后仍允许编译期自定义（最长 63 字节），留足二维码容量。
        char data[192];
        snprintf(data, sizeof(data), "WIFI:T:WPA;S:%s;P:%s;;",
                 app_portal_ssid(), app_portal_password());
        lv_obj_t *qr = lv_qrcode_create(scr);
        lv_qrcode_set_size(qr, 112);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        lv_qrcode_update(qr, data, strlen(data));
        memset(data, 0, sizeof(data));
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_t *ssid = mk_label(scr, &lv_font_montserrat_14, C_TEXT, app_portal_ssid());
        lv_obj_align(ssid, LV_ALIGN_TOP_MID, 0, 194);
        char password[96];
        snprintf(password, sizeof(password), "热点密码：%s", app_portal_password());
        lv_obj_t *pw = mk_label(scr, app_font_text(), C_DIM, password);
        lv_obj_align(pw, LV_ALIGN_TOP_MID, 0, 215);
        memset(password, 0, sizeof(password));
        lv_obj_t *url = mk_label(scr, app_font_text(), C_SKY, "浏览器：192.168.4.1");
        lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 239);
    }
    s_prov_status = mk_label(scr, app_font_text(), C_AMBER, app_portal_text(app_portal_stage()));
    make_single_line(s_prov_status, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_set_style_text_align(s_prov_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_prov_status, LV_ALIGN_TOP_MID, 0, 265);
    lv_obj_t *hint = mk_label(scr, app_font_text(), C_FAINT, "长按 OK 进入设备设置");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_prov_last = (portal_stage_t)-1;
    page_commit(scr);
    s_page_timer = lv_timer_create(prov_timer_cb, 200, NULL);
    bsp_lvgl_unlock();
}

// ---------- 设备设置 ----------
static void settings_render(int selected) {
    for (int i = 0; i < 3; ++i) {
        if (!s_rows_set[i]) continue;
        const bool sel = i == selected;
        lv_obj_set_style_bg_color(s_rows_set[i], lv_color_hex(sel ? C_CARD_SEL : C_CARD), 0);
        lv_obj_set_style_border_width(s_rows_set[i], sel ? 2 : 0, 0);
    }
}

static lv_obj_t *s_rows_set[3];

void app_ui_show_settings(bool provisioning, int selected) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    lv_obj_t *title = mk_label(scr, app_font_text(), C_TEAL, "设备设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_t *guide = mk_label(scr, app_font_text(), C_DIM, "修改配置或恢复出厂");
    lv_obj_align(guide, LV_ALIGN_TOP_MID, 0, 73);
    const char *items[] = { "重新配置 Wi-Fi", "恢复出厂设置", "返回上一页" };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *row = mk_box(scr, SCR_W - 2 * PAD, 36, C_CARD, 10);
        lv_obj_set_style_border_color(row, lv_color_hex(C_TEAL), 0);
        lv_obj_set_pos(row, PAD, 104 + i * 43);
        s_rows_set[i] = row;
        lv_obj_t *label = mk_label(row, app_font_text(), i == 1 ? C_RED : C_TEXT, items[i]);
        lv_obj_center(label);
    }
    lv_obj_t *hint = mk_label(scr, app_font_text(), C_DIM,
        LV_SYMBOL_UP LV_SYMBOL_DOWN " 选择  OK 确认  长按返回");
    make_single_line(hint, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    settings_render(selected);
    page_commit(scr);
    bsp_lvgl_unlock();
}

void app_ui_settings_update(int selected) {
    if (!bsp_lvgl_lock(1000)) return;
    settings_render(selected);
    bsp_lvgl_unlock();
}

void app_ui_show_reset_confirm(bool confirm_selected) {
    if (!bsp_lvgl_lock(1000)) return;
    set_backlight(100);
    lv_obj_t *scr = page_begin();
    lv_obj_t *title = mk_label(scr, app_font_text(), C_RED, "恢复出厂设置？");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_t *body = mk_label(scr, app_font_text(), C_TEXT,
        "删除 Wi-Fi 设置\n恢复默认音量\n重启后重新配置");
    lv_obj_set_width(body, SCR_W - 2 * PAD);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(body, 4, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 83);
    const char *items[] = { "取消，保留设置", "确认清除并重启" };
    for (int i = 0; i < 2; ++i) {
        const bool sel = (i == 1) == confirm_selected;
        lv_obj_t *row = mk_box(scr, SCR_W - 2 * PAD, 36, sel ? C_CARD_SEL : C_CARD, 10);
        lv_obj_set_style_border_color(row, lv_color_hex(i ? C_RED : C_TEAL), 0);
        lv_obj_set_style_border_width(row, sel ? 2 : 0, 0);
        lv_obj_set_pos(row, PAD, 196 + i * 46);
        lv_obj_t *label = mk_label(row, app_font_text(), i ? C_RED : C_TEXT, items[i]);
        lv_obj_center(label);
    }
    lv_obj_t *hint = mk_label(scr, app_font_text(), C_DIM,
        confirm_selected ? "长按 OK 清除  " LV_SYMBOL_UP " 取消" : "OK 取消  " LV_SYMBOL_DOWN " 选择清除");
    make_single_line(hint, app_font_text(), SCR_W - 2 * PAD);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    page_commit(scr);
    bsp_lvgl_unlock();
}

// main/app_portal.c —— WPA2 热点 + 本地网页配网（无 PAT：KidAsk 不需要云端令牌）。
// 每次进入通过重启隔离语音与配置任务；退出也重启，不让门户常驻语音会话。
#include "app_portal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "app_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "portal_form.h"

static const char *TAG = "app_portal";
static volatile portal_stage_t s_stage;
static char s_ssid[32], s_password[13], s_token[33];
static httpd_handle_t s_http;
static TaskHandle_t s_worker;
static volatile bool s_busy;
static wifi_config_t s_saved;    // 仅配置模式保存一份旧凭据，用于失败恢复
static struct {
    char ssid[33], pass[65];
    bool open;
    bool blind;   // 手机热点模式：配置时目标热点未开，跳过在线验证直接保存
} s_pending;

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");

portal_stage_t app_portal_stage(void) { return s_stage; }
const char *app_portal_ssid(void) { return s_ssid; }
const char *app_portal_password(void) { return s_password; }
const char *app_portal_text(portal_stage_t st) {
    switch (st) {
    case PORTAL_READY:          return "等待浏览器配置";
    case PORTAL_CONNECTING:     return "正在连接 Wi-Fi…";
    case PORTAL_WIFI_FAILED:    return "连接失败，请检查网络和密码";
    case PORTAL_SAVE_FAILED:    return "保存失败，请重试";
    case PORTAL_DONE:           return "配置成功，即将重启";
    case PORTAL_ERROR:          return "热点启动失败，请重启";
    default:                   return "正在启动热点…";
    }
}

// 只接受 AP 的 IPv4 地址，以及双栈 HTTP socket 返回的 IPv4-mapped IPv6。
static bool ap_local_address(const struct sockaddr_storage *local, socklen_t len) {
    if (!local) return false;
    if (local->ss_family == AF_INET) {
        if (len != sizeof(struct sockaddr_in)) return false;
        struct sockaddr_in addr;
        memcpy(&addr, local, sizeof(addr));
        return addr.sin_addr.s_addr == inet_addr("192.168.4.1");
    }
#if CONFIG_LWIP_IPV6
    if (local->ss_family == AF_INET6) {
        if (len != sizeof(struct sockaddr_in6)) return false;
        struct sockaddr_in6 addr;
        memcpy(&addr, local, sizeof(addr));
        static const uint8_t mapped_ap[16] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 4, 1
        };
        return memcmp(addr.sin6_addr.s6_addr, mapped_ap, sizeof(mapped_ap)) == 0;
    }
#endif
    return false; // 原生 IPv6、未知地址族以及截断的结构均不放行。
}

// 限制配置接口只接受发往 AP 地址的连接，而非新连接路由器侧的地址。
static bool ap_request(httpd_req_t *req) {
    if (req->method == HTTP_GET && req->content_len != 0) return false;
    struct sockaddr_storage local = {0};
    socklen_t len = sizeof(local);
    char host[32];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host, "192.168.4.1") != 0 && strcmp(host, "192.168.4.1:80") != 0))
        return false; // 固定 Host，避免 DNS 重绑定把第三方 origin 变成配置 origin
    return getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) == 0 &&
           ap_local_address(&local, len);
}
static void no_cache(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}
static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg) {
    char escaped[384], buf[448];
    form_json_escape(escaped, sizeof(escaped), msg, strlen(msg));
    snprintf(buf, sizeof(buf), "{\"message\":\"%s\"}", escaped);
    no_cache(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    bool rejected = status[0] == '4' || status[0] == '5';
    if (rejected) httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t e = httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return rejected ? ESP_FAIL : e;
}
static esp_err_t root_get(httpd_req_t *req) {
    if (!ap_request(req)) return reply(req, "403 Forbidden", "请连接设备热点");
    no_cache(req);
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; img-src 'none'; frame-ancestors 'none'; form-action 'self'");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, portal_html_start, portal_html_end - portal_html_start - 1);
}
static esp_err_t status_get(httpd_req_t *req) {
    if (!ap_request(req)) return reply(req, "403 Forbidden", "请连接设备热点");
    portal_stage_t st = s_stage;
    char buf[512], ssid[193];
    size_t len = strnlen((const char *)s_saved.sta.ssid, sizeof(s_saved.sta.ssid));
    form_json_escape(ssid, sizeof(ssid), (const char *)s_saved.sta.ssid, len);
    snprintf(buf, sizeof(buf),
        "{\"stage\":%d,\"busy\":%s,\"done\":%s,"
        "\"ssid\":\"%s\",\"token\":\"%s\",\"message\":\"%s\"}",
        (int)st, s_busy ? "true" : "false", st == PORTAL_DONE ? "true" : "false",
        ssid, s_token, app_portal_text(st));
    no_cache(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t scan_get(httpd_req_t *req) {
    if (!ap_request(req)) return reply(req, "403 Forbidden", "请连接设备热点");
    if (s_busy) return reply(req, "409 Conflict", "连接中，请稍候");
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK)
        return reply(req, "503 Service Unavailable", "扫描失败，请重试或手动输入名称");
    uint16_t n = 24;
    wifi_ap_record_t *aps = calloc(n, sizeof(*aps));
    if (!aps) {
        esp_wifi_clear_ap_list();
        return reply(req, "503 Service Unavailable", "内存不足，请重试");
    }
    esp_err_t e = esp_wifi_scan_get_ap_records(&n, aps);
    if (e != ESP_OK) {
        free(aps);
        esp_wifi_clear_ap_list();
        return reply(req, "503 Service Unavailable", "扫描结果读取失败");
    }
    no_cache(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    e = httpd_resp_send_chunk(req, "[", 1);
    for (uint16_t i = 0; i < n && e == ESP_OK; i++) {
        char escaped[193], item[256];
        form_json_escape(escaped, sizeof(escaped), (const char *)aps[i].ssid,
                         strnlen((const char *)aps[i].ssid, sizeof(aps[i].ssid)));
        int len = snprintf(item, sizeof(item),
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
            i ? "," : "", escaped, aps[i].rssi, (int)aps[i].authmode);
        e = httpd_resp_send_chunk(req, item, len);
    }
    free(aps);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, "]", 1);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, NULL, 0);
    return e;
}
static esp_err_t save_post(httpd_req_t *req) {
    if (!ap_request(req)) return reply(req, "403 Forbidden", "请连接设备热点");
    char token[sizeof(s_token)];
    if (httpd_req_get_hdr_value_str(req, "X-Portal-Token", token, sizeof(token)) != ESP_OK ||
        strcmp(token, s_token) != 0)
        return reply(req, "403 Forbidden", "页面已失效，请刷新设备配置页");
    if (s_stage == PORTAL_IDLE || s_stage == PORTAL_ERROR)
        return reply(req, "503 Service Unavailable", "热点服务尚未就绪");
    if (s_busy) return reply(req, "409 Conflict", "正在处理，请勿重复提交");
    if (req->content_len == 0 || req->content_len > 1400)
        return reply(req, "400 Bad Request", "输入过长或为空");
    char *body = calloc(req->content_len + 1, 1);
    if (!body) return reply(req, "503 Service Unavailable", "内存不足");
    size_t got = 0;
    const int64_t deadline = esp_timer_get_time() + 8LL * 1000 * 1000;
    while (got < req->content_len) {
        if (esp_timer_get_time() > deadline) {
            memset(body, 0, req->content_len + 1); free(body);
            return reply(req, "408 Request Timeout", "提交超时，请重试");
        }
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { memset(body, 0, req->content_len + 1); free(body); return ESP_FAIL; }
        got += n;
    }
    char open[2] = {0}, blind[2] = {0};
    bool valid = form_get(body, got, "ssid", s_pending.ssid, sizeof(s_pending.ssid)) >= 0 &&
        form_get(body, got, "pass", s_pending.pass, sizeof(s_pending.pass)) >= 0 &&
        form_get(body, got, "open", open, sizeof(open)) >= 0;
    // blind 为可选字段：旧页面/家庭路由器不带它，按在线验证处理。
    form_get(body, got, "blind", blind, sizeof(blind));
    memset(body, 0, req->content_len + 1);
    free(body);
    s_pending.open = strcmp(open, "1") == 0;
    s_pending.blind = strcmp(blind, "1") == 0;
    if (!valid || !form_ssid_valid(s_pending.ssid) ||
        !form_wifi_pass_valid(s_pending.pass)) {
        memset(&s_pending, 0, sizeof(s_pending));
        return reply(req, "400 Bad Request", "检查名称与密码；首次必须填写 Wi-Fi");
    }
    // 保持旧密码必须显式选同一 SSID，开放网络则显式勾选“无密码”。
    size_t old_len = strnlen((const char *)s_saved.sta.ssid, sizeof(s_saved.sta.ssid));
    bool same = strlen(s_pending.ssid) == old_len &&
                memcmp(s_pending.ssid, s_saved.sta.ssid, old_len) == 0;
    if (!s_pending.open && !s_pending.pass[0] && !same) {
        memset(&s_pending, 0, sizeof(s_pending));
        return reply(req, "400 Bad Request", "新网络请填写密码，或勾选无密码网络");
    }
    s_busy = true;
    s_stage = PORTAL_CONNECTING;
    xTaskNotifyGive(s_worker);
    return reply(req, "202 Accepted", "正在连接并保存，请保持连接设备热点");
}
static esp_err_t redirect_get(httpd_req_t *req) {
    if (req->content_len != 0) return reply(req, "400 Bad Request", "请求格式错误");
    no_cache(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "Open http://192.168.4.1/", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t error_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    return redirect_get(req);
}

static void config_worker(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        wifi_config_t c = s_saved;
        memset(c.sta.ssid, 0, sizeof(c.sta.ssid));
        memcpy(c.sta.ssid, s_pending.ssid, strlen(s_pending.ssid));
        if (s_pending.pass[0] || s_pending.open) {
            memset(c.sta.password, 0, sizeof(c.sta.password));
            if (!s_pending.open) memcpy(c.sta.password, s_pending.pass, strlen(s_pending.pass));
        }
        c.sta.bssid_set = false;
        c.sta.channel = 0;   // 换网络时不带上一个 AP 的信道提示
        c.sta.threshold.authmode = WIFI_AUTH_OPEN;
        // 与 app_wifi_restore_creds 一致的连接策略：扫完全部信道后按信号强度挑
        // 同名 AP 中最好的一个，并允许驱动原地重试一次。
        c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        c.sta.failure_retry_cnt = 1;
        app_wifi_set_auto_reconnect(false);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
        const bool blind = s_pending.blind;
        esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &c); // WIFI_STORAGE_RAM
        if (blind) {
            // 手机热点模式：同一台手机无法同时开热点(AP)并连接设备热点(STA)，
            // 配网时目标热点物理上不在空中，无法当场验证。跳过在线连接，直接
            // 持久化凭据并重启，由开机流程在用户打开热点后自动连上。
            // SSID/密码校验、AP 来源校验与 X-Portal-Token 校验均照常生效。
            ESP_LOGI(TAG, "手机热点模式：跳过在线验证，直接保存凭据");
            if (e == ESP_OK) e = app_wifi_save_creds((const char *)c.sta.ssid,
                                                     (const char *)c.sta.password);
            if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_STA, &c);
            s_stage = e == ESP_OK ? PORTAL_DONE : PORTAL_SAVE_FAILED;
        } else if (e == ESP_OK) {
            e = app_wifi_connect(25000);
            if (e != ESP_OK) {
                s_stage = PORTAL_WIFI_FAILED;
            } else {
                // 网络成功才落盘。NVS 不提供跨 Wi-Fi 事务；保存错误显式报告。
                s_stage = PORTAL_CONNECTING;
                e = app_wifi_save_creds((const char *)c.sta.ssid, (const char *)c.sta.password);
                if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
                if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_STA, &c);
                s_stage = e == ESP_OK ? PORTAL_DONE : PORTAL_SAVE_FAILED;
            }
        } else {
            s_stage = PORTAL_WIFI_FAILED;
        }
        memset(&c, 0, sizeof(c));
        memset(&s_pending, 0, sizeof(s_pending));
        app_wifi_set_auto_reconnect(false);
        esp_wifi_disconnect();
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_config(WIFI_IF_STA, &s_saved);
        s_busy = false;
        if (s_stage == PORTAL_DONE) {
            ESP_LOGI(TAG, "网络已验证并保存，准备重启");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
    }
}

// 最小 DNS responder：只处理一个未压缩 question；只回复 A/IN，其他类型空答案。
static void dns_task(void *arg) {
    int fd = (int)(intptr_t)arg;
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int n = recvfrom(fd, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&peer, &plen);
        if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;
        int p = 12;
        while (p < n && buf[p] > 0 && buf[p] <= 63 && p + buf[p] + 1 < n) p += buf[p] + 1;
        if (p >= n || buf[p] != 0 || p + 5 > n) continue;
        p++;
        bool a = buf[p] == 0 && buf[p+1] == 1 && buf[p+2] == 0 && buf[p+3] == 1;
        p += 4;
        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0; buf[7] = a ? 1 : 0;
        memset(buf + 8, 0, 4);
        if (a) {
            static const uint8_t ans[] = {0xc0,0x0c,0,1,0,1,0,0,0,0,0,4,192,168,4,1};
            memcpy(buf + p, ans, sizeof(ans));
            p += sizeof(ans);
        }
        sendto(fd, buf, p, 0, (struct sockaddr *)&peer, plen);
    }
}

esp_err_t app_portal_start(void) {
    s_stage = PORTAL_IDLE;
    app_wifi_set_auto_reconnect(false);
    esp_err_t e = esp_wifi_get_config(WIFI_IF_STA, &s_saved);
    if (e != ESP_OK) goto fail;
    e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e != ESP_OK) goto fail;
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) { e = ESP_ERR_NO_MEM; goto fail; }
    uint8_t mac[6], random[28];
    e = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (e != ESP_OK) goto fail;
    snprintf(s_ssid, sizeof(s_ssid), "KidAsk-%02X%02X", mac[4], mac[5]);
    esp_fill_random(random, sizeof(random));
    // 12 位 base32 随机口令，避免模糊字形；每次进入热点更新。
    static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    for (int i = 0; i < 12; i++) s_password[i] = alphabet[random[i] % 32];
    s_password[12] = 0;
    for (int i = 0; i < 16; i++) snprintf(s_token + i * 2, 3, "%02x", random[12+i]);
    memset(random, 0, sizeof(random));
    // 显式设定 192.168.4.1 与 DHCP DNS/portal 地址。
    esp_netif_dhcps_stop(ap);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ip.gw.addr = inet_addr("192.168.4.1");
    ip.netmask.addr = inet_addr("255.255.255.0");
    e = esp_netif_set_ip_info(ap, &ip);
    if (e != ESP_OK) goto fail;
    static const char url[] = "http://192.168.4.1/"; // DHCP 保留指针，必须覆盖服务器生命周期
    e = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void *)url, strlen(url));
    if (e != ESP_OK) goto fail;
    e = esp_netif_dhcps_start(ap);
    if (e != ESP_OK) goto fail;
    wifi_config_t ac = {0};
    memcpy(ac.ap.ssid, s_ssid, strlen(s_ssid));
    ac.ap.ssid_len = strlen(s_ssid);
    memcpy(ac.ap.password, s_password, strlen(s_password));
    ac.ap.channel = 1;
    ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ac.ap.max_connection = 2;
    e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ac);
    memset(&ac, 0, sizeof(ac));
    if (e != ESP_OK) goto fail;
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (xTaskCreate(config_worker, "portal_cfg", 8192, NULL, 4, &s_worker) != pdPASS) {
        e = ESP_ERR_NO_MEM; goto fail;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 5120;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 3;
    cfg.send_wait_timeout = 5;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    e = httpd_start(&s_http, &cfg);
    if (e != ESP_OK) goto fail;
    const httpd_uri_t uris[] = {
        {.uri="/", .method=HTTP_GET, .handler=root_get},
        {.uri="/status", .method=HTTP_GET, .handler=status_get},
        {.uri="/scan", .method=HTTP_GET, .handler=scan_get},
        {.uri="/save", .method=HTTP_POST, .handler=save_post},
        {.uri="/*", .method=HTTP_GET, .handler=redirect_get}
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        e = httpd_register_uri_handler(s_http, &uris[i]);
        if (e != ESP_OK) goto fail;
    }
    e = httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, error_404);
    if (e != ESP_OK) goto fail;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { e = ESP_FAIL; goto fail; }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53)};
    addr.sin_addr.s_addr = inet_addr("192.168.4.1");
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        xTaskCreate(dns_task, "portal_dns", 3072, (void *)(intptr_t)fd, 3, NULL) != pdPASS) {
        close(fd); e = ESP_FAIL; goto fail;
    }
    s_stage = PORTAL_READY;
    ESP_LOGI(TAG, "配置热点已启动（192.168.4.1，密码不记录）");
    return ESP_OK;
fail:
    // DNS 在最后一步才启动。其余部分失败时停止 HTTP 与尚未接单的 worker。
    s_stage = PORTAL_ERROR;
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
    if (s_worker) { vTaskDelete(s_worker); s_worker = NULL; }
    ESP_LOGE(TAG, "热点启动失败: %s", esp_err_to_name(e));
    return e;
}

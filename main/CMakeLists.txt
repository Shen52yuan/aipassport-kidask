# 「Aipassport-KidAsk」应用组件。
# 基于 Qoder-Passport fork：去除 QCA/PAT 云端实时语音通道，改为
# 「按住 OK 说话 → Ogg Opus 上行 → 自建服务器 ASR+DeepSeek(儿童 prompt)+TTS
#  → Ogg Opus 下行童声播放 + 屏幕要点」。
# 已删除：qca_pat.c / qca_api.c / qca_list_parse.c / qca_voice.c / b64_stream.c / qjson.c / voice_logic.c
# 新增：   ogg_mux.c / ogg_demux.c / kid_net.c / kid_opus.c / kid_audio.c（录音→Opus→Ogg / Ogg→Opus→播放）
idf_component_register(
    SRCS "main.c"
         "app_ui.c"
         "app_fonts.c"
         "app_wifi.c"
         "app_portal.c"
         "portal_form.c"
         "tls_mfl.c"
         "audio_dsp.c"
         "utf8_util.c"
         "ogg_mux.c"
         "ogg_demux.c"
         "kid_net.c"
         "kid_opus.c"
         "kid_audio.c"
         "../assets/fonts/app_font_cjk_16.c"
         "../assets/fonts/app_font_status_28.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp_http_server lwip esp-tls mbedtls esp_system
    EMBED_TXTFILES "portal.html"
)

# 见 tls_mfl.c：包装 esp-tls 调用的 mbedtls_ssl_config_defaults，为 TLS 客户端请求 4KB
# 最大分片，避免语音/音频收发包中为满长记录申请约 16.7KB 连续接收缓冲。-u 确保包装函数被链接进来。
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=mbedtls_ssl_config_defaults"
    "-Wl,-u,__wrap_mbedtls_ssl_config_defaults")

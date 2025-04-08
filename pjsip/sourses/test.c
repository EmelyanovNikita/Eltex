#include <pjsua-lib/pjsua.h>
#include <pjlib.h>
#include <pjmedia.h>

#define TONE_FREQ 425
#define THIS_FILE "test.c"

static pjmedia_port *tonegen;
static pjsua_conf_port_id toneslot;

static void error_exit(const char *title, pj_status_t status) {
    pjsua_perror(THIS_FILE, title, status);
    pjsua_destroy();
    exit(1);
}

static void create_tone_generator() {
    pj_status_t status;
    pj_str_t name = pj_str("tonegen");
    
    status = pjmedia_tonegen_create2(pjsua_pool_create("tone", 1000, 1000),
                                &name, 8000, 1, 160, 16, 0, &tonegen);
    if (status != PJ_SUCCESS) error_exit("Tonegen create failed", status);

    pjmedia_tone_desc tone;
    tone.freq1 = TONE_FREQ;
    tone.freq2 = 0;
    tone.on_msec = 0; // Бесконечно
    tone.off_msec = 0;
    tone.volume = 1000;
    tone.flags = 0;
    
    status = pjmedia_tonegen_play(tonegen, 1, &tone, 0);
    if (status != PJ_SUCCESS) error_exit("Tone play failed", status);

    status = pjsua_conf_add_port(pjsua_pool_create("conf", 1000, 1000),
                                tonegen, &toneslot);
    if (status != PJ_SUCCESS) error_exit("Conf add failed", status);
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                           pjsip_rx_data *rdata) {
    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);
    PJ_LOG(3, (THIS_FILE, "Incoming call from %.*s", 
              (int)ci.remote_info.slen, ci.remote_info.ptr));
    
    pjsua_call_answer(call_id, 200, NULL, NULL);
}

static void on_call_media_state(pjsua_call_id call_id) {
    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);
    PJ_LOG(3, (THIS_FILE, "Media status: %d", ci.media_status));
    
    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
        pj_status_t status = pjsua_conf_connect(0, ci.conf_slot); // 0 = системный микрофон
        PJ_LOG(3, (THIS_FILE, "Connect status: %d", status));
    }
}

int main() {
    pj_status_t status;
    
    // Инициализация
    status = pjsua_create();
    if (status != PJ_SUCCESS) return 1;

    // Настройки
    pjsua_config cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config media_cfg;

    pjsua_config_default(&cfg);
    cfg.cb.on_incoming_call = &on_incoming_call;
    cfg.cb.on_call_media_state = &on_call_media_state;

    pjsua_logging_config_default(&log_cfg);
    log_cfg.console_level = 5; // Максимальное логирование

    pjsua_media_config_default(&media_cfg);
    media_cfg.clock_rate = 8000;
    media_cfg.snd_clock_rate = 8000;
    media_cfg.quality = 10;
    
    status = pjsua_init(&cfg, &log_cfg, &media_cfg);
    if (status != PJ_SUCCESS) return 1;
    
    // Настройка кодеков (исправленная версия)
    pj_str_t codec_pcmu = pj_str("PCMU/8000/1");
    pj_str_t codec_pcma = pj_str("PCMA/8000/1");
    pjsua_codec_set_priority(&codec_pcmu, PJMEDIA_CODEC_PRIO_HIGHEST);
    pjsua_codec_set_priority(&codec_pcma, 0);

    // Транспорт
    // status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, NULL, NULL);
    // if (status != PJ_SUCCESS) return 1;
    {
        pjsua_transport_config cfg;

        pjsua_transport_config_default(&cfg);
        cfg.port = 5062;
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &cfg, NULL);
        if (status != PJ_SUCCESS) error_exit("Error creating transport", status);
    }

    // Аккаунт
    pjsua_acc_config acc_cfg;
    pjsua_acc_config_default(&acc_cfg);
    acc_cfg.id = pj_str("sip:100@192.168.0.27");
    acc_cfg.reg_uri = pj_str("");
    acc_cfg.cred_count = 0;
    
    status = pjsua_acc_add(&acc_cfg, PJ_TRUE, NULL);
    if (status != PJ_SUCCESS) return 1;

    // Тон
    create_tone_generator();

    // Запуск
    status = pjsua_start();
    if (status != PJ_SUCCESS) return 1;

    PJ_LOG(3, (THIS_FILE, "Server started. Waiting for calls..."));
    
    while (1) pj_thread_sleep(10000);

    return 0;
}

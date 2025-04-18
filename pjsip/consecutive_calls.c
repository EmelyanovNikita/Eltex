#include <pjsip.h>
#include <pjmedia.h>
#include <pjmedia-codec.h>
#include <pjsip_ua.h>
#include <pjsip_simple.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include "util.h"

#define THIS_FILE   "consecutive_calls.c"
#define CLOCK_RATE          16000
#define SAMPLES_PER_FRAME   (CLOCK_RATE/100)
#define BITS_PER_SAMPLE     16
#define NCHANNELS           1
#define MAX_CALLS           30

/* Settings */
#define AF              pj_AF_INET() /* Change to pj_AF_INET6() for IPv6.
                                      * PJ_HAS_IPV6 must be enabled and
                                      * your system must support IPv6.  */               
#define SIP_PORT        5060         /* Listening SIP port              */
#define RTP_PORT        4000         /* RTP port                        */

#define MAX_MEDIA_CNT   1            /* Media count, set to 1 for audio
                                      * only or 2 for audio and video   */

#define PORT_COUNT      254

typedef struct 
{
    pjmedia_tone_desc     tone;
    unsigned              tone_slot;
    pjmedia_port         *tone_pjmedia_port;
} player_tone_t;

typedef struct call_t 
{
    pjsip_inv_session           *inv;
    pjmedia_stream              *stream;
    pjmedia_port                *port;
    unsigned                    slot;
    pj_bool_t                   in_use;
    pjmedia_transport           *transport;
    pj_str_t                    sip_uri_target_user;
} call_t;

static struct app_t 
{
    pj_caching_pool             cp;
    pj_pool_t                   *pool;
    pj_pool_t                   *snd_pool;
    pjmedia_endpt               *med_endpt;
    pjsip_endpoint              *sip_endpt;
    pjmedia_conf                *conf;
    pjmedia_port                *null_port;
    pjmedia_master_port         *null_snd;

    pjmedia_port                *wav_port;
    unsigned                    wav_slot;
    pj_str_t                    wav_player_name;

    pj_str_t                    long_tone_player_name;
    player_tone_t               long_tone;

    pj_str_t                    KVP_tone_player_name;
    player_tone_t               KVP_tone;

    pjmedia_port                *writer_port;
    unsigned                    writer_slot;
    call_t                      calls[MAX_CALLS];
    pj_thread_t                 *worker_thread;
    pj_bool_t                   quit;
    pj_mutex_t                  *mutex;
} app;

/* Прототипы функций */
static pj_status_t init_system();
static pj_status_t init_pjlib();
static pj_status_t init_sip();
static pj_status_t init_media();
static pj_status_t cleanup_call(call_t *call);
static pj_status_t cleanup_resources();
static void call_on_state_changed(pjsip_inv_session *inv, pjsip_event *e);
static void call_on_media_update(pjsip_inv_session *inv, pj_status_t status);
static pj_bool_t on_rx_request(pjsip_rx_data *rdata);
static int worker_proc(void *arg);
static pj_status_t add_media_to_call(call_t *call);
pj_status_t create_and_connect_master_port();
pj_status_t create_and_connect_player_to_conf(const char *filename);
static void call_on_forked(pjsip_inv_session *inv, pjsip_event *e);

static pjsip_module mod_simpleua =
{
    NULL, NULL,                     /* prev, next.              */
    { "mod-simpleua", 12 },         /* Name.                    */
    -1,                             /* Id                       */
    PJSIP_MOD_PRIORITY_APPLICATION, /* Priority                 */
    NULL,                           /* load()                   */
    NULL,                           /* start()                  */
    NULL,                           /* stop()                   */
    NULL,                           /* unload()                 */
    &on_rx_request,                 /* on_rx_request()          */
    NULL,                           /* on_rx_response()         */
    NULL,                           /* on_tx_request.           */
    NULL,                           /* on_tx_response()         */
    NULL,                           /* on_tsx_state()           */
};

/* Notification on incoming messages */
static pj_bool_t logging_on_rx_msg(pjsip_rx_data *rdata)
{
    PJ_LOG(4,(THIS_FILE, "RX %d bytes %s from %s %s:%d:\n"
                         "%.*s\n"
                         "--end msg--",
                         rdata->msg_info.len,
                         pjsip_rx_data_get_info(rdata),
                         rdata->tp_info.transport->type_name,
                         rdata->pkt_info.src_name,
                         rdata->pkt_info.src_port,
                         (int)rdata->msg_info.len,
                         rdata->msg_info.msg_buf));
    
    /* Always return false, otherwise messages will not get processed! */
    return PJ_FALSE;
}

/* Notification on outgoing messages */
static pj_status_t logging_on_tx_msg(pjsip_tx_data *tdata)
{
    
    /* Important note:
     *  tp_info field is only valid after outgoing messages has passed
     *  transport layer. So don't try to access tp_info when the module
     *  has lower priority than transport layer.
     */

    PJ_LOG(4,(THIS_FILE, "TX %ld bytes %s to %s %s:%d:\n"
                         "%.*s\n"
                         "--end msg--",
                         (tdata->buf.cur - tdata->buf.start),
                         pjsip_tx_data_get_info(tdata),
                         tdata->tp_info.transport->type_name,
                         tdata->tp_info.dst_name,
                         tdata->tp_info.dst_port,
                         (int)(tdata->buf.cur - tdata->buf.start),
                         tdata->buf.start));

    /* Always return success, otherwise message will not get sent! */
    return PJ_SUCCESS;
}

/* The module instance. */
static pjsip_module msg_logger = 
{
    NULL, NULL,                         /* prev, next.          */
    { "mod-msg-log", 13 },              /* Name.                */
    -1,                                 /* Id                   */
    PJSIP_MOD_PRIORITY_TRANSPORT_LAYER-1,/* Priority            */
    NULL,                               /* load()               */
    NULL,                               /* start()              */
    NULL,                               /* stop()               */
    NULL,                               /* unload()             */
    &logging_on_rx_msg,                 /* on_rx_request()      */
    &logging_on_rx_msg,                 /* on_rx_response()     */
    &logging_on_tx_msg,                 /* on_tx_request.       */
    &logging_on_tx_msg,                 /* on_tx_response()     */
    NULL,                               /* on_tsx_state()       */

};


/* Основная функция */
int main(int argc, char *argv[]) 
{
    pj_status_t status;
    
    status = init_system();
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable init system", status);
        return 1;
    }

    // // Инициализация
    // status = init_pjlib();
    // if (status != PJ_SUCCESS) return 1;
    
    // status = init_sip();
    // if (status != PJ_SUCCESS) return 1;
    
    // status = init_media();
    // if (status != PJ_SUCCESS) return 1;

    // // создание и присоединение к бриджу тона
    // status = create_and_connect_tone_to_conf(&app.long_tone);
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);


    // задаём название плеера и тонов, на эти номера будет звонить клиент и в 
    // зависимости от того ком набрал клиент ему будет производиться соответсвующтие звуки в трубку
    app.wav_player_name = pj_str("WAV_file");

    // создание и присоединение к бриджу плеера 
    status = create_and_connect_player_to_conf("output_4.wav");
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable create and connect player port", status);
        return 1;
    }

    // инициализация тона
    app.long_tone.tone.freq1 =          425;
    app.long_tone.tone.freq2 =           0;
    app.long_tone.tone.on_msec =         1000;
    app.long_tone.tone.off_msec =        0;
    app.long_tone.tone.volume =          0;
    app.long_tone.tone.flags =           0;
    app.long_tone.tone_slot =            -1;
    app.long_tone.tone_pjmedia_port =   NULL;
    app.long_tone_player_name =         pj_str("long_tone");

    // создание и присоединение к бриджу тона
    status = create_and_connect_tone_to_conf(&app.long_tone);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    // инициализация тона
    app.KVP_tone.tone.freq1 =           425;
    app.KVP_tone.tone.freq2 =           0;
    app.KVP_tone.tone.on_msec =         1000;
    app.KVP_tone.tone.off_msec =        4000;
    app.KVP_tone.tone.volume =          0;
    app.KVP_tone.tone.flags =           0;
    app.KVP_tone.tone_slot =            -1;
    app.KVP_tone.tone_pjmedia_port =   NULL;
    app.KVP_tone_player_name =         pj_str("KVP_tone");

    // создание и присоединение к бриджу тона
    status = create_and_connect_tone_to_conf(&app.KVP_tone);

    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    /* Create event manager */
    status = pjmedia_event_mgr_create(app.pool, 0, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    pj_thread_create(app.pool, "sipecho", &worker_proc, NULL, 0, 0,
        &app.worker_thread);

    // Основной цикл
    for (;;)
    {
        char s[10];

        printf("\nMenu:\n"
               "  h    Hangup all calls\n"
               "  q    Quit\n");

        if (fgets(s, sizeof(s), stdin) == NULL)
            continue;

        if (s[0]=='q')
            break;
    }

    // Очистка
    cleanup_resources();
    return 0;
}

/* Инициализация PJLIB */
static pj_status_t init_pjlib() 
{
    pj_status_t status;
    
    status = pj_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    status = pjlib_util_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    pj_caching_pool_init(&app.cp, NULL, 0);
    app.pool = pj_pool_create(&app.cp.factory, "app", 4000, 4000, NULL);
    if (!app.pool) return PJ_ENOMEM;
    
    // status = pj_mutex_create(app.pool, "mutex", PJ_MUTEX_SIMPLE, &app.mutex);
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    return PJ_SUCCESS;
}

/* Инициализация SIP стека */
static pj_status_t init_sip() 
{
    pj_status_t status;
    
    // Создание SIP endpoint
    const pj_str_t *hostname = pj_gethostname();

    status = pjsip_endpt_create(&app.cp.factory, hostname->ptr, &app.sip_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    // Добавление UDP транспорта
    pj_sockaddr addr;
    pj_sockaddr_init(pj_AF_INET(), &addr, NULL, (pj_uint16_t)SIP_PORT);
    
    status = pjsip_udp_transport_start(app.sip_endpt, &addr.ipv4, NULL, 1, NULL);
    if (status != PJ_SUCCESS) return status;
    
    // Инициализация модулей
    status = pjsip_tsx_layer_init_module(app.sip_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    status = pjsip_ua_init_module(app.sip_endpt, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    // Инициализация модуля INVITE
    pjsip_inv_callback inv_cb;
    pj_bzero(&inv_cb, sizeof(inv_cb));
    inv_cb.on_state_changed = &call_on_state_changed;
    inv_cb.on_new_session = &call_on_forked;
    inv_cb.on_media_update = &call_on_media_update;
    
    status = pjsip_inv_usage_init(app.sip_endpt, &inv_cb);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    /* Initialize 100rel support */
    status = pjsip_100rel_init_module(app.sip_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    // Регистрация модулей
    status = pjsip_endpt_register_module(app.sip_endpt, &mod_simpleua);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    status = pjsip_endpt_register_module( app.sip_endpt, &msg_logger);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    return PJ_SUCCESS;
}

/* Инициализация медиа */
static pj_status_t init_media() 
{
    pj_status_t status;
    
    // Создание медиа endpoint
    status = pjmedia_endpt_create(&app.cp.factory, NULL, 1, &app.med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    app.pool = pjmedia_endpt_create_pool(app.med_endpt, "Media pool", 16000, 16000);   
    if (!app.pool) return PJ_ENOMEM;

    // Инициализация кодеков
    status = pjmedia_codec_g711_init(app.med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    // Создание конференц-моста
    status = pjmedia_conf_create(app.pool, MAX_CALLS+2, CLOCK_RATE, 
                                NCHANNELS, SAMPLES_PER_FRAME, 
                                BITS_PER_SAMPLE, PJMEDIA_CONF_NO_DEVICE, &app.conf);                    
    if (status != PJ_SUCCESS) return status;
    
    // создание и подключение мастер порта
    status = create_and_connect_master_port();
    if (status != PJ_SUCCESS) 
    {
        return status;
    }
    
    return PJ_SUCCESS;
}

/* Обработчик изменения состояния звонка */
static void call_on_state_changed(pjsip_inv_session *inv, pjsip_event *e) 
{
    PJ_UNUSED_ARG(e);
    
    if (inv->state == PJSIP_INV_STATE_DISCONNECTED) {
        // pj_mutex_lock(app.mutex);
        
        // Находим звонок в массиве
        for (int i = 0; i < MAX_CALLS; i++) {
            if (app.calls[i].inv == inv && app.calls[i].in_use) 
            {
                cleanup_call(&app.calls[i]);
                break;
            }
        }
        
        // pj_mutex_unlock(app.mutex);
    }
}

/* Очистка звонка */
static pj_status_t cleanup_call(call_t *call) 
{
    pj_status_t status;
    if (!call->in_use) return PJ_SUCCESS;
    
    if (call->port && call->slot != (unsigned)-1) 
    {
        pjmedia_conf_remove_port(app.conf, call->slot);
        PJ_LOG(3, (THIS_FILE, "pjmedia_conf_remove_port - DONE"));
    }
    
    if (call->port) 
    {
        pjmedia_port_destroy(call->port);
        PJ_LOG(3, (THIS_FILE, "pjmedia_port_destroy - DONE"));
    }
    
    // if (call->stream) 
    // {
    //     pjmedia_stream_destroy(call->stream);
    //     PJ_LOG(3, (THIS_FILE, "pjmedia_stream_destroy - DONE"));
    // }

    if (call->transport) 
    {
        status = pjmedia_transport_close(call->transport);
        PJ_LOG(3, (THIS_FILE, "pjmedia_transport_close - DONE"));
    }

    call->sip_uri_target_user;
    
    call->in_use = PJ_FALSE;
    call->inv = NULL;
    call->port = NULL;
    call->stream = NULL;
    call->slot = (unsigned)-1;
    call->transport = NULL;
    
    return PJ_SUCCESS;
}

/* This callback is called when dialog has forked. */
static void call_on_forked(pjsip_inv_session *inv, pjsip_event *e)
{
    /* To be done... */
    PJ_UNUSED_ARG(inv);
    PJ_UNUSED_ARG(e);
}

/* Очистка всех ресурсов */
static pj_status_t cleanup_resources() 
{
    // Очистка всех звонков
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        cleanup_call(&app.calls[i]);
    }
    
    // Остановка мастер-порта
    if (app.null_snd) {
        pjmedia_master_port_stop(app.null_snd);
        pjmedia_master_port_destroy(app.null_snd, PJ_FALSE);
    }
    
    // Уничтожение портов
    if (app.null_port) {
        pjmedia_port_destroy(app.null_port);
    }
    
    if (app.wav_port) {
        pjmedia_port_destroy(app.wav_port);
    }
    
    if (app.writer_port) {
        pjmedia_port_destroy(app.writer_port);
    }
    
    // Уничтожение конференц-моста
    if (app.conf) {
        pjmedia_conf_destroy(app.conf);
    }
    
    // Уничтожение медиа endpoint
    if (app.med_endpt) {
        pjmedia_endpt_destroy(app.med_endpt);
    }
    
    // Уничтожение SIP endpoint
    if (app.sip_endpt) {
        pjsip_endpt_destroy(app.sip_endpt);
    }
    
    // Освобождение пулов
    if (app.pool) {
        pj_pool_release(app.pool);
    }
    
    pj_caching_pool_destroy(&app.cp);
    pj_shutdown();
    
    return PJ_SUCCESS;
}

static pj_bool_t on_rx_request(pjsip_rx_data *rdata)
{
    PJ_LOG(3, (THIS_FILE, "AAA"));
    
    pj_status_t status;
    pj_sockaddr hostaddr;
    char temp[80], hostip[PJ_INET6_ADDRSTRLEN];
    pj_str_t local_uri;
    pjsip_dialog *dlg;
    pjmedia_sdp_session *local_sdp;
    pjsip_tx_data *tdata;
    unsigned options = 0;
    int call_idx = -1;

    /* Обрабатываем только INVITE запросы */
    if (rdata->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD) 
    {
        /* Для всех остальных запросов (кроме ACK) отвечаем 500 */
        if (rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD) 
        {
            pj_str_t reason = pj_str("Simple UA unable to handle "
                                     "this request");

            pjsip_endpt_respond_stateless( app.sip_endpt, rdata, 
                                           500, &reason,
                                           NULL, NULL);
        }
        return PJ_TRUE;
    }

    /* Проверяем количество активных звонков */
    // pj_mutex_lock(app.mutex);
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        if (!app.calls[i].in_use) 
        {
            call_idx = i;
            break;
        }
    }
    // pj_mutex_unlock(app.mutex);

    if (call_idx == -1) 
    {
        /* Нет свободных слотов - отвечаем 486 Busy Here */
        pj_str_t reason = pj_str("Too many calls");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 
                                    500, &reason, NULL, NULL);
        return PJ_TRUE;
    }

    status = pjsip_inv_verify_request(rdata, &options, NULL, NULL,
        app.sip_endpt, NULL);
    if (status != PJ_SUCCESS) 
    {

        pj_str_t reason = pj_str("Sorry Simple UA can not handle this INVITE");

        pjsip_endpt_respond_stateless( app.sip_endpt, rdata, 
                                       500, &reason,
                                       NULL, NULL);
        return PJ_TRUE;
    } 

    /* Получаем IP-адрес сервера для Contact header */
    if (pj_gethostip(pj_AF_INET(), &hostaddr) != PJ_SUCCESS) 
    {
        pj_str_t reason = pj_str("Server error");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 
                                    500, &reason, NULL, NULL);
        return PJ_TRUE;
    }

    /* Формируем Contact URI */
    pj_sockaddr_print(&hostaddr, hostip, sizeof(hostip), 2);
    pj_ansi_snprintf(temp, sizeof(temp), "<sip:simpleuas@%s:%d>", hostip, SIP_PORT);
    local_uri = pj_str(temp);

    /* Создаем UAS dialog */
    status = pjsip_dlg_create_uas_and_inc_lock(pjsip_ua_instance(),
                                             rdata,
                                             &local_uri,
                                             &dlg);
    if (status != PJ_SUCCESS) 
    {
        pj_str_t reason = pj_str("Failed to create dialog");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 
                                    500, &reason, NULL, NULL);
        return PJ_TRUE;
    }

    /* Создаем медиа транспорт для SDP оффера */
    pjmedia_transport *transport;
    // status = pjmedia_transport_udp_create(app.med_endpt, NULL, 
    //                                      RTP_PORT + (call_idx * 2), 0, 
    //                                      &transport);
    status = pjmedia_transport_udp_create3(app.med_endpt, pj_AF_INET(), NULL, NULL, 
                                            RTP_PORT + (call_idx * 2), 0, 
                                            &transport);
    if (status != PJ_SUCCESS) 
    {
        pjsip_dlg_dec_lock(dlg);
        pj_str_t reason = pj_str("Media transport error");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 
                                    500, &reason, NULL, NULL);
        return PJ_TRUE;
    }

    /* Создаем SDP оффер */
    pjmedia_sock_info sock_info;
    pjmedia_transport_info tp_info;
    pjmedia_transport_info_init(&tp_info);
    pjmedia_transport_get_info(transport, &tp_info);
    pj_memcpy(&sock_info, &tp_info.sock_info, sizeof(pjmedia_sock_info));

    status = pjmedia_endpt_create_sdp(app.med_endpt, dlg->pool, //app.pool
                                     1, &sock_info, &local_sdp);
    if (status != PJ_SUCCESS) {
        pjmedia_transport_close(transport);
        pjsip_dlg_dec_lock(dlg);
        pj_str_t reason = pj_str("SDP creation error");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 
                                    500, &reason, NULL, NULL);
        return PJ_TRUE;
    }

    /* Создаем INVITE сессию */
    status = pjsip_inv_create_uas(dlg, rdata, local_sdp, 0, &app.calls[call_idx].inv);
    if (status != PJ_SUCCESS) 
    {
        pjmedia_transport_close(transport);
        pjsip_dlg_dec_lock(dlg);
        pj_str_t reason = pj_str("Invite session error");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 
                                    500, &reason, NULL, NULL);
        return PJ_TRUE;
    }

    /* Сохраняем информацию о звонке */
    // pj_mutex_lock(app.mutex);
    app.calls[call_idx].in_use = PJ_TRUE;
    app.calls[call_idx].transport = transport;
    app.calls[call_idx].port = NULL;
    app.calls[call_idx].slot = -1;
    app.calls[call_idx].stream = NULL;
    // pj_mutex_unlock(app.mutex);

    pjsip_sip_uri *sip_uri_user = app.calls[call_idx].inv->dlg->target;

    pjsip_sip_uri *sip_uri = (pjsip_sip_uri *) pjsip_uri_get_uri(rdata->msg_info.msg->line.req.uri);

    app.calls[call_idx].sip_uri_target_user.ptr = (char*) pj_pool_alloc(app.pool , 256);
    pj_strcpy(&app.calls[call_idx].sip_uri_target_user, &sip_uri->user);
    
    PJ_LOG(3,(THIS_FILE, "CALL TO %.*s!!", (int)app.calls[call_idx].sip_uri_target_user.slen, app.calls[call_idx].sip_uri_target_user.ptr));

    /* Отправляем 180 Ringing */
    status = pjsip_inv_initial_answer(app.calls[call_idx].inv, rdata, 
                                    180, NULL, NULL, &tdata);
    if (status != PJ_SUCCESS) 
    {
        pjsip_dlg_dec_lock(dlg);
        return PJ_TRUE;
    }
    
    status = pjsip_inv_send_msg(app.calls[call_idx].inv, tdata);
    if (status != PJ_SUCCESS) 
    {
        pjsip_dlg_dec_lock(dlg);
        return PJ_TRUE;
    }
    
    /* Отправляем 200 OK */
    status = pjsip_inv_answer(app.calls[call_idx].inv, 
                            200, NULL, NULL, &tdata);
    if (status != PJ_SUCCESS) {
        pjsip_dlg_dec_lock(dlg);
        return PJ_TRUE;
    }
    
    status = pjsip_inv_send_msg(app.calls[call_idx].inv, tdata);
    if (status != PJ_SUCCESS) {
        pjsip_dlg_dec_lock(dlg);
        return PJ_TRUE;
    }

    /* Разблокируем диалог */
    pjsip_dlg_dec_lock(dlg);

    return PJ_TRUE;
}

pj_status_t create_and_connect_master_port()
{
    pj_status_t status;
    pjmedia_port *conf_port;

    /* Create null port if not exists */
    if (!app.null_port) 
    {
        status = pjmedia_null_port_create(app.pool, CLOCK_RATE, 1,
                                        SAMPLES_PER_FRAME, BITS_PER_SAMPLE,
                                        &app.null_port);
        if (status != PJ_SUCCESS) 
        {
            PJ_LOG(3, (THIS_FILE, "Unable to create null port"));
            pj_log_pop_indent();
            return status;
        }
    }

    /* Get the port0 of the conference bridge. */
    conf_port = pjmedia_conf_get_master_port(app.conf); //pjmedia_conf_remove_port
    pj_assert(conf_port != NULL);

    /* Create master port, connecting port0 of the conference bridge to
    * a null port.
    */
    status = pjmedia_master_port_create(app.snd_pool, app.null_port,
                                        conf_port, 0, &app.null_snd);
    if (status != PJ_SUCCESS)
    {
        PJ_LOG(3, (THIS_FILE, "Unable to create null sound device: %d", (int)status));
        return status;
    }

    /* Start the master port */
    status = pjmedia_master_port_start(app.null_snd);
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(4, (THIS_FILE, "Unable to start null sound device: %d", status));
        return status;
    }
    return PJ_SUCCESS;
}

pj_status_t create_and_connect_writer_to_conf(const char *filename)
{
    pj_status_t status;
    status = pjmedia_wav_writer_port_create(  app.pool, filename,
                                            CLOCK_RATE,
                                            NCHANNELS,
                                            SAMPLES_PER_FRAME,
                                            BITS_PER_SAMPLE,
                                            0, 700000, 
                                            &app.writer_port);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to open WAV file for writing", status);
        return status;
    }

    status = pjmedia_conf_add_port(app.conf, app.pool,
        app.writer_port, NULL, &app.writer_slot);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to add writer to conf", status);
        return status;
    }
    
    return PJ_SUCCESS;
}

pj_status_t create_and_connect_player_to_conf(const char *filename)
{
    pj_status_t status;

    status = pjmedia_wav_player_port_create(
        app.pool, filename,
        SAMPLES_PER_FRAME *
        1000 / NCHANNELS /
        CLOCK_RATE,
        0, 700000, &app.wav_port);

    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to open file for playback", status);
        return 1;
    }

    status = pjmedia_conf_add_port(app.conf, app.pool,
        app.wav_port, NULL, &app.wav_slot);
    if (status != PJ_SUCCESS) 
    {
        pjmedia_port_destroy(app.wav_port);
        app_perror(THIS_FILE, "Unable to add file to conference bridge",
        status);
        return status;
    }

    return PJ_SUCCESS;

}


/* Инициализация всей системы */
static pj_status_t init_system() 
{
    pj_status_t status;
    
    // Инициализация PJLIB
    status = pj_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    pj_log_set_level(5);

    status = pjlib_util_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    /* Must create a pool factory before we can allocate any memory. */
    pj_caching_pool_init(&app.cp, &pj_pool_factory_default_policy, 0);
    
    app.snd_pool = pj_pool_create(&app.cp.factory, "snd", 16000, 16000, NULL);
    if (!app.snd_pool) return PJ_ENOMEM;
    
    // // Мьютекс для синхронизации
    // status = pj_mutex_create(app.pool, "mutex", PJ_MUTEX_SIMPLE, &app.mutex);
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    // Инициализация SIP стека
    status = init_sip();
    if (status != PJ_SUCCESS) return status;
    
    // Инициализация медиа системы
    status = init_media();
    if (status != PJ_SUCCESS) return status;
    
    // Инициализация массива звонков
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        app.calls[i].in_use = PJ_FALSE;
    }

    return PJ_SUCCESS;
}

pj_status_t create_and_connect_tone_to_conf(player_tone_t *player) 
{
    pj_status_t status;
    char name[80];
    pj_str_t label;

    pj_ansi_snprintf(name, sizeof(name), "tone-%d,%d,%d,%d", player->tone.freq1, player->tone.freq2, 
        player->tone.off_msec, player->tone.on_msec);
    
    label = pj_str(name);

    status = pjmedia_tonegen_create2(app.pool, &label, CLOCK_RATE, NCHANNELS, SAMPLES_PER_FRAME, 
        BITS_PER_SAMPLE, PJMEDIA_TONEGEN_LOOP, &player->tone_pjmedia_port);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to create tone generator", status);
        return status;
    }

    status = pjmedia_conf_add_port(app.conf, app.pool,
        player->tone_pjmedia_port, NULL, &player->tone_slot);
    if (status != PJ_SUCCESS) 
    {
        pjmedia_port_destroy(app.wav_port);
        app_perror(THIS_FILE, "Unable to add file to conference bridge",
        status);
        return status;
    }


    status = pjmedia_tonegen_play(player->tone_pjmedia_port, 1, &player->tone, 0);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to play tone", status);
        return status;
    }
    
    PJ_LOG(3, (THIS_FILE, "Tone generator: %s - CREATED", name));
    return PJ_SUCCESS;
}

/* Обработчик обновления медиа */
static void call_on_media_update(pjsip_inv_session *inv, pj_status_t status) 
{
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(3,(THIS_FILE, "Media update failed: %d", status));
        return;
    }
    
    // pj_mutex_lock(app.mutex);
    
    // Находим слот
    int call_idx = -1;
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        if ( (app.calls[i].inv == inv) && (app.calls[i].in_use) ) 
        {
            PJ_LOG(3,(THIS_FILE, "НАЙДЕНО да INV == INV: %d", i));
            call_idx = i;
            break;
        }
    }
    
    // Настраиваем звонок
    call_t *call = &app.calls[call_idx];
    
    // Добавляем медиа к звонку
    status = add_media_to_call(call);
    if (status != PJ_SUCCESS) 
    {
        call->in_use = PJ_FALSE;
        call->inv = NULL;
    }
    
    // pj_mutex_unlock(app.mutex);
}

/* Добавление медиа к звонку */
static pj_status_t add_media_to_call(call_t *call) 
{
    pj_status_t status;
    pjmedia_stream_info stream_info;
    const pjmedia_sdp_session *local_sdp, *remote_sdp;
    
    // Получаем SDP информацию
    pjmedia_sdp_neg_get_active_local(call->inv->neg, &local_sdp);
    pjmedia_sdp_neg_get_active_remote(call->inv->neg, &remote_sdp);
    
    status = pjmedia_stream_info_from_sdp(&stream_info, call->inv->dlg->pool,
                                        app.med_endpt, local_sdp, remote_sdp, 0);
    if (status != PJ_SUCCESS) {
        PJ_LOG(3,(THIS_FILE, "Failed to create stream info"));
        return status;
    }
    
    // // Создаем транспорт для медиа
    // status = pjmedia_transport_udp_create(app.med_endpt, NULL, RTP_PORT, 0, &call->transport);
    // if (status != PJ_SUCCESS) 
    // {
    //     PJ_LOG(3,(THIS_FILE, "Failed to create media transport"));
    //     return status;
    // }
    
    // Создаем медиа поток
    status = pjmedia_stream_create(app.med_endpt, call->inv->dlg->pool, &stream_info,
                                 call->transport, NULL, &call->stream);
    if (status != PJ_SUCCESS) {
        PJ_LOG(3,(THIS_FILE, "Failed to create stream"));
        pjmedia_transport_close(call->transport);
        return status;
    }
    
    // Запускаем поток
    status = pjmedia_stream_start(call->stream);
    if (status != PJ_SUCCESS) {
        PJ_LOG(3,(THIS_FILE, "Failed to start stream"));
        pjmedia_stream_destroy(call->stream);
        pjmedia_transport_close(call->transport);
        return status;
    }
    
    /* Start the UDP media transport */
    status = pjmedia_transport_media_start(call->transport, 0, 0, 0, 0);
    if (status != PJ_SUCCESS) 
    {
        app_perror( THIS_FILE, "Unable to start UDP media transport", status);
        return;
    }

    // Получаем медиа порт
    status = pjmedia_stream_get_port(call->stream, &call->port);
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(3,(THIS_FILE, "Failed to get media port"));
        pjmedia_stream_destroy(call->stream);
        pjmedia_transport_close(call->transport);
        return status;
    }
    
    // Добавляем в конференц-мост
    status = pjmedia_conf_add_port(app.conf, call->inv->dlg->pool,
                                 call->port, NULL, &call->slot);
    if (status != PJ_SUCCESS) {
        PJ_LOG(3,(THIS_FILE, "Failed to add to conference"));
        pjmedia_port_destroy(call->port);
        pjmedia_stream_destroy(call->stream);
        pjmedia_transport_close(call->transport);
        return status;
    }
    

    if ( (pj_strcmp( &(call->sip_uri_target_user), &(app.wav_player_name) ) == 0 ))
    {
        // Подключаем к WAV плееру
        if (app.wav_port) 
        {
            status = pjmedia_conf_connect_port(app.conf, app.wav_slot, call->slot, 0);
            if (status != PJ_SUCCESS) 
            {
                PJ_LOG(3,(THIS_FILE, "Failed to connect to WAV player"));
            }
        }
    }
    else  if ((pj_strcmp( &(call->sip_uri_target_user), &(app.long_tone_player_name) ) == 0 ))
    {
        // Подключаем к WAV плееру
        if (app.wav_port) 
        {
            status = pjmedia_conf_connect_port(app.conf, app.long_tone.tone_slot, call->slot, 0);
            if (status != PJ_SUCCESS) 
            {
                PJ_LOG(3,(THIS_FILE, "Failed to connect to WAV player"));
            }
        }
    }
    else  if ((pj_strcmp( &(call->sip_uri_target_user), &(app.KVP_tone_player_name) ) == 0 ))
    {
        // Подключаем к WAV плееру
        if (app.wav_port) 
        {
            status = pjmedia_conf_connect_port(app.conf, app.KVP_tone.tone_slot, call->slot, 0);
            if (status != PJ_SUCCESS) 
            {
                PJ_LOG(3,(THIS_FILE, "Failed to connect to WAV player"));
            }
        }
    }
    else PJ_LOG(3,(THIS_FILE, "NO TARGET_USER == PLAYER_NAME"));
    
    // // Подключаем к WAV рекордеру
    // if (app.writer_port) {
    //     status = pjmedia_conf_connect_port(app.conf, call->slot, app.writer_slot, 0);
    //     if (status != PJ_SUCCESS) {
    //         PJ_LOG(3,(THIS_FILE, "Failed to connect to WAV recorder"));
    //     }
    // }
    
    return PJ_SUCCESS;
}

static int worker_proc(void *arg)
{
    PJ_UNUSED_ARG(arg);

    while (!app.quit) 
    {
        pj_time_val interval = { 0, 10 };
        pjsip_endpt_handle_events(app.sip_endpt, &interval);
    }

    return 0;
}
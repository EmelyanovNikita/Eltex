//  ./auto_answer output_2.wav <- в какой файл будет записано
#include <pjsip.h>
#include <pjmedia.h>
#include <pjmedia-codec.h>
#include <pjsip_ua.h>
#include <pjsip_simple.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <pjmedia/tonegen.h>

/* For logging purpose. */
#define THIS_FILE   "tone_rec_to_file.c"

#include "util.h"


/* Settings */
#define AF              pj_AF_INET() /* Change to pj_AF_INET6() for IPv6.
                                      * PJ_HAS_IPV6 must be enabled and
                                      * your system must support IPv6.  */               
#define SIP_PORT        5060         /* Listening SIP port              */
#define RTP_PORT        4000         /* RTP port                        */

#define MAX_MEDIA_CNT   2            /* Media count, set to 1 for audio
                                      * only or 2 for audio and video   */

#define MAX_CALLS       30

#define PORT_COUNT      254

// Убедитесь, что параметры совпадают с конференц-бриджем
#define CLOCK_RATE          16000
#define SAMPLES_PER_FRAME   (CLOCK_RATE/100)  // 160 samples
#define BITS_PER_SAMPLE     16
#define NCHANNELS           1                 // Моно


/*
 * Static variables.
 */

static pj_bool_t             g_complete;    /* Quit flag.               */
static pjsip_endpoint       *g_endpt;       /* SIP endpoint.            */
static pj_caching_pool       cp;            /* Global pool factory.     */

static pjmedia_endpt        *g_med_endpt;   /* Media endpoint.          */

static pjmedia_transport_info g_med_tpinfo[MAX_MEDIA_CNT]; 
                                            /* Socket info for media    */
static pjmedia_transport    *g_med_transport[MAX_MEDIA_CNT];
                                            /* Media stream transport   */
static pjmedia_sock_info     g_sock_info[MAX_MEDIA_CNT];  
                                            /* Socket info array        */
static pjmedia_master_port  *master_port;

/* Call variables: */
static pjsip_inv_session    *g_inv;         /* Current invite session.  */
static pjmedia_stream       *g_med_stream;  /* Call's audio stream.     */
static pjmedia_snd_port     *g_snd_port;    /* Sound device.            */

static pjmedia_master_port  *null_snd;      /**< Master port for null sound.    */
static pjmedia_port         *null_port;     /**< Null port.                     */

static struct call_t
{
    pjsip_inv_session   *g_inv;
} call;

typedef struct 
{
    pjmedia_tone_desc     tone;
    unsigned              tone_slot;
    pjmedia_port         *tone_pjmedia_port;
    pj_str_t              name;
} player_tone_t;

/*
* App data
*/                                  
static struct app_t
{
    // Main app data
    pj_caching_pool         cp;             /* Global pool factory         */
    pj_pool_t               *pool;          /* main app pool               */

    // Sound device
    pj_pool_t               *snd_pool;      /* Sound's pool                 */

    // Wav file player
    pjmedia_port            *wav_port;
    unsigned                wav_slot;
    pjmedia_port            *writer_port;
    unsigned                writer_slot;

    // Tone player

    player_tone_t           long_tone;
    player_tone_t           KPV_tone;

    /* Media: */

    pjmedia_conf        *mconf;             /* Conference bridge */

    /* SIP variables */
    pjsip_endpoint      *g_endpt;
    pjmedia_endpt       *g_med_endpt;

    //pjmedia_port        *conf_port;
    
    pjmedia_port        *null_port;
    pjmedia_master_port *null_snd;  /**< Master port for null sound.    */
    
    struct call_t       calls[MAX_CALLS];

    pj_bool_t           quit;
    pj_thread_t         *worker_thread;

    pj_mutex_t          *mutex;
    pj_bool_t           enable_msg_logging;
} app;


/* Create and add to conference bridge Master port - null soud device*/
pj_status_t null_sound_device_master_port();

/* This is a PJSIP module to be registered by application to handle
 * incoming requests outside any dialogs/transactions. The main purpose
 * here is to handle incoming INVITE request message, where we will
 * create a dialog and INVITE session for it.
 */
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
    NULL,                           /* on_rx_request()          */
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

int main(int argc, char *argv[])
{
    app.pool = NULL;
    pj_status_t status;
    unsigned i;

    pjmedia_snd_port *snd_port;
    char tmp[10];

    /* Must init PJLIB first: */
    status = pj_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    pj_log_set_level(5);

    /* Then init PJLIB-UTIL: */
    status = pjlib_util_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);


    /* Must create a pool factory before we can allocate any memory. */
    pj_caching_pool_init(&app.cp, &pj_pool_factory_default_policy, 0);

    // В начале main():
    app.pool = pj_pool_create(&app.cp.factory, "app", 16000, 16000, NULL);
    app.snd_pool = pj_pool_create(&app.cp.factory, "snd", 16000, 16000, NULL);


    PJ_LOG(3, (THIS_FILE, " "));
    /* Create global endpoint: */
    {
        const pj_str_t *hostname;
        const char *endpt_name;

        /* Endpoint MUST be assigned a globally unique name.
         * The name will be used as the hostname in Warning header.
         */

        /* For this implementation, we'll use hostname for simplicity */
        hostname = pj_gethostname();
        endpt_name = hostname->ptr;

        /* Create the endpoint: */

        status = pjsip_endpt_create(&app.cp.factory, endpt_name, 
                                    &app.g_endpt);

        PJ_LOG(3,(THIS_FILE, "HOSTNAME: %.*s", (int)hostname->slen, hostname->ptr));
        PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    }


    /* 
     * Add UDP transport, with hard-coded port 
     * Alternatively, application can use pjsip_udp_transport_attach() to
     * start UDP transport, if it already has an UDP socket (e.g. after it
     * resolves the address with STUN).
     */
    {
        pj_sockaddr addr;
        int af = AF;

        pj_sockaddr_init(af, &addr, NULL, (pj_uint16_t)SIP_PORT);
        
        if (af == pj_AF_INET()) {
            status = pjsip_udp_transport_start( app.g_endpt, &addr.ipv4, NULL, 
                                                1, NULL);
        } else if (af == pj_AF_INET6()) {
            status = pjsip_udp_transport_start6(app.g_endpt, &addr.ipv6, NULL,
                                                1, NULL);
        } else {
            status = PJ_EAFNOTSUP;
        }

        if (status != PJ_SUCCESS) {
            app_perror(THIS_FILE, "Unable to start UDP transport", status);
            return 1;
        }
    }



    status = pjsip_tsx_layer_init_module(app.g_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    status = pjsip_ua_init_module( app.g_endpt, NULL );
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Initialize 100rel support */
    status = pjsip_100rel_init_module(app.g_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /*
     * Register our module to receive incoming requests.
     */
    status = pjsip_endpt_register_module( app.g_endpt, &mod_simpleua);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /*
     * Register message logger module.
     */
    status = pjsip_endpt_register_module( app.g_endpt, &msg_logger);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);


    status = pjmedia_endpt_create(&app.cp.factory, NULL, 1, &app.g_med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    // создание конференц-брниджа
    status = pjmedia_conf_create(app.pool,
            30,
            CLOCK_RATE,
            NCHANNELS, SAMPLES_PER_FRAME, BITS_PER_SAMPLE,
            PJMEDIA_CONF_NO_DEVICE,
            &app.mconf);

    // создание и подключение мастер порта
    status = create_and_connect_master_port();
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable create and connect master port (create_and_connect_master_port)", status);
        return 1;
    }
    // создание и присоединение к бриджу врайтера 
    status = create_and_connect_writer_to_conf(argv[1]);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable create and connect writer port", status);
        return 1;
    }

    // создание и присоединение к бриджу плеера 
    // status = create_and_connect_player_to_conf("output_4.wav");
    // if (status != PJ_SUCCESS) 
    // {
    //     app_perror(THIS_FILE, "Unable create and connect player port", status);
    //     return 1;
    // }

    // включаем однонаправленную преедачу от плеера к записывателю
    // status = pjmedia_conf_connect_port(app.mconf, app.wav_slot, app.writer_slot, 0);
    // if (status != PJ_SUCCESS) 
    // {
    //     app_perror(THIS_FILE, "Unable to connect slot writer_slot to wav_slot",
    //     status);
    //     return status;
    // }

    // // инициалоизация тона
    // app.long_tone.tone.freq1 =         425;
    // app.long_tone.tone.freq2 =         0;
    // app.long_tone.tone.on_msec =       1000;
    // app.long_tone.tone.off_msec =      0;
    // app.long_tone.tone.volume =        0;
    // app.long_tone.tone.flags =         0;
    // app.long_tone.tone_slot =          -1;
    // app.long_tone.tone_pjmedia_port =   NULL;
    // app.long_tone.name =                pj_str("long_tone");
    // // создание и присоединение к бриджу тона
    // status = create_and_connect_tone_to_conf(&app.long_tone);
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    // // включаем однонаправленную преедачу от плеера к записывателю
    // status = pjmedia_conf_connect_port(app.mconf, app.long_tone.tone_slot, app.writer_slot, 0);
    // if (status != PJ_SUCCESS) 
    // {
    //     app_perror(THIS_FILE, "Unable to connect slot writer_slot to wav_slot",
    //     status);
    //     return status;
    // }

    // инициалоизация тона
    app.KPV_tone.tone.freq1 =         425;
    app.KPV_tone.tone.freq2 =         0;
    app.KPV_tone.tone.on_msec =       1000;
    app.KPV_tone.tone.off_msec =      4000;
    app.KPV_tone.tone.volume =        0;
    app.KPV_tone.tone.flags =         0;
    app.KPV_tone.tone_slot =          -1;
    app.KPV_tone.tone_pjmedia_port =   NULL;
    app.KPV_tone.name =                pj_str("KPV_tone");
    // создание и присоединение к бриджу тона
    status = create_and_connect_tone_to_conf(&app.KPV_tone);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    // включаем однонаправленную преедачу от плеера к записывателю
    status = pjmedia_conf_connect_port(app.mconf, app.KPV_tone.tone_slot, app.writer_slot, 0);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to connect slot writer_slot to wav_slot",
        status);
        return status;
    }

    /* Create event manager */
    status = pjmedia_event_mgr_create(app.pool, 0, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Sleep to allow log messages to flush */
    pj_thread_sleep(10);


    printf("Recodring %s..\n", argv[1]);
    puts("");
    puts("Press <ENTER> to stop recording and quit");

    if (fgets(tmp, sizeof(tmp), stdin) == NULL) {
        puts("EOF while reading stdin, will quit now..");
    }

    //pjmedia_conf_disconnect_port(app.mconf, app.wav_slot, app.writer_slot);

    // pjmedia_tonegen_stop(app.long_tone.tone_pjmedia_port);
    // pjmedia_conf_disconnect_port(app.mconf, app.long_tone.tone_slot, app.writer_slot);

    pjmedia_tonegen_stop(app.KPV_tone.tone_pjmedia_port);
    pjmedia_conf_disconnect_port(app.mconf, app.KPV_tone.tone_slot, app.writer_slot);

    // ПРОБЛЕМА: не записывается звук если отчистить (pjmedia_conf_remove_port и pjmedia_port_destroy)
    // но если выключить тогда работает но не освобождается память
    
    // if (app.long_tone.tone_pjmedia_port) 
    // {
    //     pjmedia_conf_remove_port(app.mconf, app.long_tone.tone_slot);
    //     status = tonegen_destroy(app.long_tone.tone_pjmedia_port);
    //     // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    //     app.writer_port = NULL;
    // }

    if (app.wav_port) 
    {
        pjmedia_conf_remove_port(app.mconf, app.wav_slot);
        pjmedia_port_destroy(app.wav_port);
        app.wav_port = NULL;
    }

    if (app.writer_port) 
    {
        pjmedia_conf_remove_port(app.mconf, app.writer_slot);
        pjmedia_port_destroy(app.writer_port);
        app.writer_port = NULL;
    }

    /* Останавливаем и уничтожаем мастер-порт */
    status = pjmedia_master_port_stop(app.null_snd);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    status = pjmedia_master_port_destroy(app.null_snd, PJ_FALSE);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    app.null_snd = NULL;

    status = pjmedia_port_destroy(app.null_port);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    app.null_port = NULL;

    /* Уничтожаем менеджер событий */
    if (pjmedia_event_mgr_instance()) 
    {
        pjmedia_event_mgr_destroy(NULL);
    }

    /* Уничтожаем медиа-эндпоинт */
    status = pjmedia_endpt_destroy(app.g_med_endpt);
    app.g_med_endpt = NULL;

    /* Уничтожаем SIP endpoint */
    if (app.g_endpt) 
    {
        pjsip_endpt_destroy(app.g_endpt);
        app.g_endpt = NULL;
    }

    // Перед уничтожением пулов:


    /* Уничтожаем конференц-мост */
    status = pjmedia_conf_destroy(app.mconf);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    app.mconf = NULL;

    /* Освобождаем пулы памяти (в обратном порядке создания) */
    if (app.snd_pool) 
    {
        pj_pool_release(app.snd_pool);
        app.snd_pool = NULL;
    }
    
    if (app.pool) 
    {
        pj_pool_release(app.pool);
        app.pool = NULL;
    }

    //PJ_LOG(1,(THIS_FILE, "2 Error occurred, cleaning up"));

    /* Уничтожаем кэширующий пул */
    pj_caching_pool_destroy(&app.cp);

    //PJ_LOG(1,(THIS_FILE, "1 Error occurred, cleaning up"));

    /* Завершаем работу PJLIB */
    pj_shutdown();

    //PJ_LOG(1,(THIS_FILE, "Error occurred, cleaning up"));


    return 0;
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
    conf_port = pjmedia_conf_get_master_port(app.mconf); //pjmedia_conf_remove_port
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

    status = pjmedia_conf_add_port(app.mconf, app.pool,
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

    status = pjmedia_conf_add_port(app.mconf, app.pool,
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

pj_status_t create_and_connect_tone_to_conf(player_tone_t *player) 
{
    pj_status_t status;
    char name[80];
    pj_str_t label;

    pj_ansi_snprintf(name, sizeof(name), "tone-%d,%d", player->tone.freq1, player->tone.freq2);
    label = pj_str(name);

    status = pjmedia_tonegen_create2(app.pool, &label, CLOCK_RATE, NCHANNELS, SAMPLES_PER_FRAME, 
        BITS_PER_SAMPLE, PJMEDIA_TONEGEN_LOOP, &player->tone_pjmedia_port);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to create tone generator", status);
        return status;
    }

    status = pjmedia_conf_add_port(app.mconf, app.pool,
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
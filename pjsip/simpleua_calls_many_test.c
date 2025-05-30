#include <pjsip.h>
#include <pjmedia.h>
#include <pjmedia-codec.h>
#include <pjsip_ua.h>
#include <pjsip_simple.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <pjmedia/tonegen.h>

/* For logging purpose. */
#define THIS_FILE   "simpleua_calls_many.c"

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

/*
* App data
*/                                  
static struct app_t
{
    pj_caching_pool      cp;
    pj_pool_t           *pool;
    pj_pool_t           *snd_pool;

    pj_pool_t           *wav_pool;
    pjmedia_port        *wav_port;
    unsigned            wav_slot;

    pj_pool_t           *writer_pool;
    pjmedia_port        *writer_port;
    unsigned            writer_slot;

    pjmedia_conf        *mconf;

    pjsip_endpoint      *g_endpt;
    pjmedia_endpt       *g_med_endpt;

    pjmedia_port        *conf_port;
    
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
    pjmedia_port *conf_port;

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
    app.wav_pool = pj_pool_create(&app.cp.factory, "wav", 16000, 16000, NULL);
    app.writer_pool = pj_pool_create(&app.cp.factory, "writer_pool", 16000, 16000, NULL);

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

    /* Create pool. */
    //app.pool = pjmedia_endpt_create_pool(app.g_med_endpt, "Media pool", 512, 512);      


    status = pjmedia_conf_create(app.pool,
            30,
            CLOCK_RATE,
            NCHANNELS, SAMPLES_PER_FRAME, BITS_PER_SAMPLE,
            PJMEDIA_CONF_NO_DEVICE,
            &app.mconf);

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

    ////////////////////////// создание и присоединение к бриджу врайтера 
    /* Create WAVE file writer port. */
    PJ_LOG(4, (THIS_FILE, "pjmedia_wav_writer_port_create"));

    // status = pjmedia_wav_writer_port_create(  app.writer_pool, argv[1],
    //                                           CLOCK_RATE,
    //                                           NCHANNELS,
    //                                           SAMPLES_PER_FRAME,
    //                                           BITS_PER_SAMPLE,
    //                                           0, 0, 
    //                                           &app.writer_port);

    // PJ_LOG(4, (THIS_FILE, "pjmedia_wav_writer_port_createD"));
    // if (status != PJ_SUCCESS) 
    // {
    //     app_perror(THIS_FILE, "Unable to open WAV file for writing", status);
    //     return 1;
    // }

    status = pjmedia_wav_writer_port_create(  app.pool, argv[1],
                                              CLOCK_RATE,
                                              NCHANNELS,
                                              SAMPLES_PER_FRAME,
                                              BITS_PER_SAMPLE,
                                              0, 0, 
                                              &app.writer_port);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to open WAV file for writing", status);
        return 1;
    }

    PJ_LOG(4, (THIS_FILE, "pjmedia_conf_add_port"));

    status = pjmedia_conf_add_port(app.mconf, app.pool,
        app.writer_port, NULL, &app.writer_slot);

    PJ_LOG(4, (THIS_FILE, "pjmedia_conf_addED_port"));

    ////////////////////////// создание и присоединение к бриджу плеера 
    status = pjmedia_wav_player_port_create(
                                    app.wav_pool, "output_file.wav",
                                    SAMPLES_PER_FRAME *
                                    1000 / NCHANNELS /
                                    CLOCK_RATE,
                                    0, 0, &app.wav_port);
    if (status != PJ_SUCCESS) 
    {
        pjsua_perror(THIS_FILE, "Unable to open file for playback", status);
        return 1;
    }

    status = pjmedia_conf_add_port(app.mconf, app.wav_pool,
                                   app.wav_port, "output_file.wav", &app.wav_slot);
    if (status != PJ_SUCCESS) 
    {
        pjmedia_port_destroy(app.wav_port);
        pjsua_perror(THIS_FILE, "Unable to add file to conference bridge",
                     status);
        return 1;
    }


    // /* Create sound player port. */
    // status = pjmedia_snd_port_create_rec
    // ( 
    //             app.pool,                              /* pool                 */
    //             -1,                                /* use default dev.     */
    //             PJMEDIA_PIA_SRATE(&app.writer_port->info),/* clock rate.         */
    //             PJMEDIA_PIA_CCNT(&app.writer_port->info),/* # of channels.       */
    //             PJMEDIA_PIA_SPF(&app.writer_port->info), /* samples per frame.   */
    //             PJMEDIA_PIA_BITS(&app.writer_port->info),/* bits per sample.     */
    //             0,                                 /* options              */
    //             &snd_port                          /* returned port        */
    //             );
    // if (status != PJ_SUCCESS) {
    //     app_perror(THIS_FILE, "Unable to open sound device", status);
    //     return 1;
    // }

    /* Connect file port to the sound player.
     * Stream playing will commence immediately.
     */
    // status = pjmedia_snd_port_connect(snd_port, app.writer_port); // pjmedia_conf_connect_port
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);



    /* 
     * Recording should be started now. 
     */


    /* Sleep to allow log messages to flush */
    pj_thread_sleep(10);


    printf("Recodring %s..\n", argv[1]);
    puts("");
    puts("Press <ENTER> to stop recording and quit");

    if (fgets(tmp, sizeof(tmp), stdin) == NULL) {
        puts("EOF while reading stdin, will quit now..");
    }

    
    // status = pjmedia_codec_g711_init(app.g_med_endpt);
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    // status =  null_sound_device_master_port();
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    
    /* Create event manager */
    status = pjmedia_event_mgr_create(app.pool, 0, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Loop until one call is completed */
    // for (;;) 
    // {
    //     pj_time_val timeout = {0, 10};
    //     pjsip_endpt_handle_events(app.g_endpt, &timeout);
    // }

    /* On exit, dump current memory usage: */
    // dump_pool_usage(THIS_FILE, &app.cp);

    /* Destroy audio ports. Destroy the audio port first
     * before the stream since the audio port has threads
     * that get/put frames to the stream.
     */
    // if (g_snd_port)
    //     pjmedia_snd_port_destroy(g_snd_port);

    // /* Destroy streams */
    // if (g_med_stream)
    //     pjmedia_stream_destroy(g_med_stream);

    // /* Destroy media transports */
    // for (i = 0; i < MAX_MEDIA_CNT; ++i) {
    //     if (g_med_transport[i])
    //         pjmedia_transport_close(g_med_transport[i]);
    // }

    // if (app.wav_player_id != PJSUA_INVALID_ID) 
    // {
    //     pjsua_player_destroy(players.file_player.wav_player_id);
    // }

    /* Destroy sound device */
    status = pjmedia_snd_port_destroy(snd_port);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    snd_port = NULL;

    /* Останавливаем и уничтожаем мастер-порт */
    status = pjmedia_master_port_stop(app.null_snd);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    status = pjmedia_master_port_destroy(app.null_snd, PJ_FALSE);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    app.null_snd = NULL;

    status = pjmedia_port_destroy(app.null_port);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    app.null_port = NULL;


    /* Уничтожаем конференц-мост */
    status = pjmedia_conf_destroy(app.mconf);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    app.mconf = NULL;

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

    /* Освобождаем пулы памяти (в обратном порядке создания) */
    if (app.snd_pool) 
    {
        pj_pool_release(app.snd_pool);
        app.snd_pool = NULL;
    }
    
    if (app.wav_pool) 
    {
        pj_pool_release(app.wav_pool);
        app.wav_pool = NULL;
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

pj_status_t null_sound_device_master_port()
{
    pjmedia_port *conf_port;
    pj_status_t status;

    PJ_LOG(4,(THIS_FILE, "Setting null sound device.."));
    pj_log_push_indent();

    //pj_mutex_lock(app.mutex);

    /* Close existing sound device */
    //close_snd_dev();

    /* Create memory pool for sound device. */
    app.snd_pool = pj_pool_create(&app.cp.factory, "null_sound_device pool", 4000, 4000, NULL);
    if (!app.snd_pool)
    {
        PJ_LOG(3, (THIS_FILE, "Unable to create pool for null sound device"));
        return PJ_ENOMEM;
    }
    
    PJ_LOG(4,(THIS_FILE, "Opening null sound device.."));

    /* Get the port0 of the conference bridge. */
    conf_port = pjmedia_conf_get_master_port(app.mconf);
    pj_assert(conf_port != NULL);

    /* Create master port, connecting port0 of the conference bridge to
     * a null port.
     */
    status = pjmedia_master_port_create(app.snd_pool, app.null_port,
                                        conf_port, 0, &app.null_snd);
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(3, (THIS_FILE, "Unable to create null sound device"));
                     
         //pj_mutex_unlock(app.mutex);
        pj_log_pop_indent();
        return status;
    }

    /* Start the master port */
    status = pjmedia_master_port_start(app.null_snd);
    if (status != PJ_SUCCESS) 
    {   
        PJ_LOG(3, (THIS_FILE, "Unable to start null sound device"));

        //pj_mutex_unlock(app.mutex);

        pj_log_pop_indent();
        return status;
    }

    //pj_mutex_unlock(app.mutex);

    pj_log_pop_indent();
    return PJ_SUCCESS;
}
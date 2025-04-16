#include <pjsip.h>
#include <pjmedia.h>
#include <pjmedia-codec.h>
#include <pjsip_ua.h>
#include <pjsip_simple.h>
#include <pjlib-util.h>
#include <pjlib.h>

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

#define CLOCK_RATE          16000
#define SAMPLES_PER_FRAME   (CLOCK_RATE/100)


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

    /* Must init PJLIB first: */
    status = pj_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    pj_log_set_level(5);

    /* Then init PJLIB-UTIL: */
    status = pjlib_util_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);


    /* Must create a pool factory before we can allocate any memory. */
    pj_caching_pool_init(&app.cp, &pj_pool_factory_default_policy, 0);


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
    app.pool = pjmedia_endpt_create_pool(app.g_med_endpt, "Media pool", 512, 512);      


    status = pjmedia_conf_create(app.wav_pool,
            30,
            CLOCK_RATE,
            1, SAMPLES_PER_FRAME, 16,
            PJMEDIA_CONF_NO_DEVICE,
            &app.mconf);

    status = pjmedia_codec_g711_init(app.g_med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    // status =  null_sound_device_master_port();
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    
    /* Create event manager */
    status = pjmedia_event_mgr_create(app.pool, 0, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Loop until one call is completed */
    for (;;) 
    {
        pj_time_val timeout = {0, 10};
        pjsip_endpt_handle_events(app.g_endpt, &timeout);
    }

    /* On exit, dump current memory usage: */
    dump_pool_usage(THIS_FILE, &app.cp);

    /* Destroy audio ports. Destroy the audio port first
     * before the stream since the audio port has threads
     * that get/put frames to the stream.
     */
    if (g_snd_port)
        pjmedia_snd_port_destroy(g_snd_port);

    /* Destroy streams */
    if (g_med_stream)
        pjmedia_stream_destroy(g_med_stream);

    /* Destroy media transports */
    for (i = 0; i < MAX_MEDIA_CNT; ++i) {
        if (g_med_transport[i])
            pjmedia_transport_close(g_med_transport[i]);
    }

    /* Destroy event manager */
    pjmedia_event_mgr_destroy(NULL); 

    /* Deinit pjmedia endpoint */
    if (app.g_med_endpt)
        pjmedia_endpt_destroy(app.g_med_endpt);

    /* Deinit pjsip endpoint */
    if (app.g_endpt)
        pjsip_endpt_destroy(app.g_endpt);

    /* Release pool */
    if (app.pool)
        pj_pool_release(app.pool);

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
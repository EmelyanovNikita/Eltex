/* 
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */


/**
 * simpleua.c
 *
 * This is a very simple SIP user agent complete with media. The user
 * agent should do a proper SDP negotiation and start RTP media once
 * SDP negotiation has completed.
 *
 * This program does not register to SIP server.
 *
 * Capabilities to be demonstrated here:
 *  - Basic call
 *  - Should support IPv6 (not tested)
 *  - UDP transport at port 5060 (hard coded)
 *  - RTP socket at port 4000 (hard coded)
 *  - proper SDP negotiation
 *  - PCMA/PCMU codec only.
 *  - Audio/media to sound device.
 *
 *
 * Usage:
 *  - To make outgoing call, start simpleua with the URL of remote
 *    destination to contact.
 *    E.g.:
 *       simpleua sip:user@remote
 *
 *  - Incoming calls will automatically be answered with 180, then 200.
 *
 * This program does not disconnect call.
 *
 * This program will quit once it has completed a single call.
 */

/* Include all headers. */
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
#if 0
#define SIP_PORT        5080         /* Listening SIP port              */
#define RTP_PORT        5000         /* RTP port                        */
#else
#define SIP_PORT        5060         /* Listening SIP port              */
#define RTP_PORT        4000         /* RTP port                        */
#endif

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

    pjmedia_conf        *conf;
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


/*
 * Prototypes:
 */

/* Callback to be called when SDP negotiation is done in the call: */
static void call_on_media_update( pjsip_inv_session *inv,
                                  pj_status_t status);

/* Callback to be called when invite session's state has changed: */
static void call_on_state_changed( pjsip_inv_session *inv, 
                                   pjsip_event *e);

/* Callback to be called when dialog has forked: */
static void call_on_forked(pjsip_inv_session *inv, pjsip_event *e);

/* Callback to be called to handle incoming requests outside dialogs: */
static pj_bool_t on_rx_request( pjsip_rx_data *rdata );

/* Инициализация глобальных данных*/
static pj_status_t init_stack(); 

/* Функция для работы процесса*/
static int worker_proc(void *arg);

/* Функция для завершения сессии */
static void destroy_call(struct call_t *call);

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


/*
 * main()
 *
 * If called with argument, treat argument as SIP URL to be called.
 * Otherwise wait for incoming calls.
 */
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


    /* 
     * Init transaction layer.
     * This will create/initialize transaction hash tables etc.
     */
    status = pjsip_tsx_layer_init_module(app.g_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);


    /* 
     * Initialize UA layer module.
     * This will create/initialize dialog hash tables etc.
     */
    status = pjsip_ua_init_module( app.g_endpt, NULL );
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);


    /* 
     * Init invite session module.
     * The invite session module initialization takes additional argument,
     * i.e. a structure containing callbacks to be called on specific
     * occurence of events.
     *
     * The on_state_changed and on_new_session callbacks are mandatory.
     * Application must supply the callback function.
     *
     * We use on_media_update() callback in this application to start
     * media transmission.
     */
    {
        pjsip_inv_callback inv_cb;

        /* Init the callback for INVITE session: */
        pj_bzero(&inv_cb, sizeof(inv_cb));
        inv_cb.on_state_changed = &call_on_state_changed;
        inv_cb.on_new_session = &call_on_forked;
        inv_cb.on_media_update = &call_on_media_update;

        /* Initialize invite session module:  */
        status = pjsip_inv_usage_init(app.g_endpt, &inv_cb);
        PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    }

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


    /* 
     * Initialize media endpoint.
     * This will implicitly initialize PJMEDIA too.
     */

    status = pjmedia_endpt_create(&app.cp.factory, NULL, 1, &app.g_med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Create pool. */
    app.pool = pjmedia_endpt_create_pool(app.g_med_endpt, "Media pool", 512, 512);      

    status =  null_sound_device_master_port();

    // app.wav_pool = pj_pool_create( &cp.factory,     /* pool factory         */
    //     "wav",           /* pool name.           */
    //     4000,            /* init size            */
    //     4000,            /* increment size       */
    //     NULL             /* callback on error    */
    //     );

    // status = pjmedia_conf_create( app.wav_pool,
    //     PORT_COUNT,
    //     CLOCK_RATE,
    //     1, SAMPLES_PER_FRAME, 16,
    //     PJMEDIA_CONF_NO_DEVICE,
    //     &app.conf);

    // if (status != PJ_SUCCESS) {
    // app_perror(THIS_FILE, "Unable to create conference bridge", status);
    // return 1;
    // }

    // /* Create null port */
    // status = pjmedia_null_port_create(app.wav_pool, CLOCK_RATE, 1, SAMPLES_PER_FRAME, 16,
    // &null_port);
    // PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    // app.conf_port = pjmedia_conf_get_master_port(app.conf);

    // /* Create master port */
    // status = pjmedia_master_port_create(pool, null_port, conf_port, 0, &master_port);


    // pjmedia_master_port_start(master_port);


    /* 
     * Add PCMA/PCMU codec to the media endpoint. 
     */
#if defined(PJMEDIA_HAS_G711_CODEC) && PJMEDIA_HAS_G711_CODEC!=0
    status = pjmedia_codec_g711_init(app.g_med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
#endif



    
    /* Create event manager */
    status = pjmedia_event_mgr_create(app.pool, 0, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* 
     * Create media transport used to send/receive RTP/RTCP socket.
     * One media transport is needed for each call. Application may
     * opt to re-use the same media transport for subsequent calls.
     */
    for (i = 0; i < PJ_ARRAY_SIZE(g_med_transport); ++i) 
    {
        status = pjmedia_transport_udp_create3(app.g_med_endpt, AF, NULL, NULL, 
                                               RTP_PORT + i*2, 0, 
                                               &g_med_transport[i]);
        if (status != PJ_SUCCESS) 
        {
            app_perror(THIS_FILE, "Unable to create media transport", status);
            return 1;
        }

        /* 
         * Get socket info (address, port) of the media transport. We will
         * need this info to create SDP (i.e. the address and port info in
         * the SDP).
         */
        pjmedia_transport_info_init(&g_med_tpinfo[i]);
        pjmedia_transport_get_info(g_med_transport[i], &g_med_tpinfo[i]);

        pj_memcpy(&g_sock_info[i], &g_med_tpinfo[i].sock_info,
                  sizeof(pjmedia_sock_info));
    }

    {
        /* No URL to make call to */

        PJ_LOG(3,(THIS_FILE, "Ready to accept incoming calls..."));
    }


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



/*
 * Callback when INVITE session state has changed.
 * This callback is registered when the invite session module is initialized.
 * We mostly want to know when the invite session has been disconnected,
 * so that we can quit the application.
 */
static void call_on_state_changed( pjsip_inv_session *inv, 
                                   pjsip_event *e)
{
    PJ_UNUSED_ARG(e);

    if (inv->state == PJSIP_INV_STATE_DISCONNECTED) 
    {
        PJ_LOG(3,(THIS_FILE, "Call DISCONNECTED [reason=%d (%s)]", 
                  inv->cause,
                  pjsip_get_status_text(inv->cause)->ptr));

        destroy_call(&call);

        PJ_LOG(3,(THIS_FILE, "One call completed, application quitting..."));
        g_complete = 1;

    } 
    else 
    {

        PJ_LOG(3,(THIS_FILE, "Call state changed to %s", 
                  pjsip_inv_state_name(inv->state)));

    }
}


/* This callback is called when dialog has forked. */
static void call_on_forked(pjsip_inv_session *inv, pjsip_event *e)
{
    /* To be done... */
    PJ_UNUSED_ARG(inv);
    PJ_UNUSED_ARG(e);
}


/*
 * Callback when incoming requests outside any transactions and any
 * dialogs are received. We're only interested to hande incoming INVITE
 * request, and we'll reject any other requests with 500 response.
 */
static pj_bool_t on_rx_request( pjsip_rx_data *rdata )
{
    pj_sockaddr hostaddr;
    char temp[80], hostip[PJ_INET6_ADDRSTRLEN];
    pj_str_t local_uri;
    pjsip_dialog *dlg;
    pjmedia_sdp_session *local_sdp;
    pjsip_tx_data *tdata;
    unsigned options = 0;
    pj_status_t status;


    /* 
     * Respond (statelessly) any non-INVITE requests with 500 
     */
    if (rdata->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD) 
    {

        if (rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD) 
        {
            pj_str_t reason = pj_str("Simple UA unable to handle "
                                     "this request");

            pjsip_endpt_respond_stateless( app.g_endpt, rdata, 
                                           500, &reason,
                                           NULL, NULL);
        }
        return PJ_TRUE;
    }


    /*
     * Reject INVITE if we already have an INVITE session in progress.
     */
    if (call.g_inv) {

        pj_str_t reason = pj_str("Another call is in progress");

        pjsip_endpt_respond_stateless( app.g_endpt, rdata, 
                                       500, &reason,
                                       NULL, NULL);
        return PJ_TRUE;

    }

    /* Verify that we can handle the request. */
    status = pjsip_inv_verify_request(rdata, &options, NULL, NULL,
                                      app.g_endpt, NULL);
    if (status != PJ_SUCCESS) 
    {

        pj_str_t reason = pj_str("Sorry Simple UA can not handle this INVITE");

        pjsip_endpt_respond_stateless( app.g_endpt, rdata, 
                                       500, &reason,
                                       NULL, NULL);
        return PJ_TRUE;
    } 

    /*
     * Generate Contact URI
     */
    if (pj_gethostip(AF, &hostaddr) != PJ_SUCCESS) {
        app_perror(THIS_FILE, "Unable to retrieve local host IP", status);
        return PJ_TRUE;
    }
    pj_sockaddr_print(&hostaddr, hostip, sizeof(hostip), 2);

    pj_ansi_snprintf(temp, sizeof(temp), "<sip:simpleuas@%s:%d>", 
                    hostip, SIP_PORT);
    local_uri = pj_str(temp);

    /*
     * Create UAS dialog.
     */
    status = pjsip_dlg_create_uas_and_inc_lock( pjsip_ua_instance(),
                                                rdata,
                                                &local_uri, /* contact */
                                                &dlg);
    if (status != PJ_SUCCESS) 
    {
        pjsip_endpt_respond_stateless(app.g_endpt, rdata, 500, NULL,
                                      NULL, NULL);
        return PJ_TRUE;
    }

    /* 
     * Get media capability from media endpoint: 
     */

    status = pjmedia_endpt_create_sdp( app.g_med_endpt, rdata->tp_info.pool,
                                       MAX_MEDIA_CNT, g_sock_info, &local_sdp);
    pj_assert(status == PJ_SUCCESS);
    if (status != PJ_SUCCESS) {
        pjsip_dlg_dec_lock(dlg);
        return PJ_TRUE;
    }


    /* 
     * Create invite session, and pass both the UAS dialog and the SDP
     * capability to the session.
     */
    status = pjsip_inv_create_uas( dlg, rdata, local_sdp, 0, &call.g_inv);
    pj_assert(status == PJ_SUCCESS);
    if (status != PJ_SUCCESS) {
        pjsip_dlg_dec_lock(dlg);
        return PJ_TRUE;
    }

    /*
     * Invite session has been created, decrement & release dialog lock.
     */
    pjsip_dlg_dec_lock(dlg);


    /*
     * Initially send 180 response.
     *
     * The very first response to an INVITE must be created with
     * pjsip_inv_initial_answer(). Subsequent responses to the same
     * transaction MUST use pjsip_inv_answer().
     */
    status = pjsip_inv_initial_answer(call.g_inv, rdata, 
                                      180, 
                                      NULL, NULL, &tdata);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);


    /* Send the 180 response. */  
    status = pjsip_inv_send_msg(call.g_inv, tdata); 
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);


    /*
     * Now create 200 response.
     */
    status = pjsip_inv_answer( call.g_inv, 
                               200, NULL,       /* st_code and st_text */
                               NULL,            /* SDP already specified */
                               &tdata);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);

    /*
     * Send the 200 response.
     */
    status = pjsip_inv_send_msg(call.g_inv, tdata);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, PJ_TRUE);


    /* Done. 
     * When the call is disconnected, it will be reported via the callback.
     */

    return PJ_TRUE;
}

 

/*
 * Callback when SDP negotiation has completed.
 * We are interested with this callback because we want to start media
 * as soon as SDP negotiation is completed.
 */
static void call_on_media_update( pjsip_inv_session *inv,
                                  pj_status_t status)
{
    pjmedia_stream_info stream_info;
    const pjmedia_sdp_session *local_sdp;
    const pjmedia_sdp_session *remote_sdp;
    pjmedia_port *media_port;

    if (status != PJ_SUCCESS) {

        app_perror(THIS_FILE, "SDP negotiation has failed", status);

        /* Here we should disconnect call if we're not in the middle 
         * of initializing an UAS dialog and if this is not a re-INVITE.
         */
        return;
    }

    /* Get local and remote SDP.
     * We need both SDPs to create a media session.
     */
    status = pjmedia_sdp_neg_get_active_local(inv->neg, &local_sdp);

    status = pjmedia_sdp_neg_get_active_remote(inv->neg, &remote_sdp);


    /* Create stream info based on the media audio SDP. */
    status = pjmedia_stream_info_from_sdp(&stream_info, inv->dlg->pool,
                                          app.g_med_endpt,
                                          local_sdp, remote_sdp, 0);
    if (status != PJ_SUCCESS) {
        app_perror(THIS_FILE,"Unable to create audio stream info",status);
        return;
    }

    /* If required, we can also change some settings in the stream info,
     * (such as jitter buffer settings, codec settings, etc) before we
     * create the stream.
     */

    /* Create new audio media stream, passing the stream info, and also the
     * media socket that we created earlier.
     */
    status = pjmedia_stream_create(app.g_med_endpt, inv->dlg->pool, &stream_info,
                                   g_med_transport[0], NULL, &g_med_stream);
    if (status != PJ_SUCCESS) {
        app_perror( THIS_FILE, "Unable to create audio stream", status);
        return;
    }

    /* Start the audio stream */
    status = pjmedia_stream_start(g_med_stream);
    if (status != PJ_SUCCESS) 
    {
        app_perror( THIS_FILE, "Unable to start audio stream", status);
        return;
    }

    /* Start the UDP media transport */
    status = pjmedia_transport_media_start(g_med_transport[0], 0, 0, 0, 0);
    if (status != PJ_SUCCESS) {
        app_perror( THIS_FILE, "Unable to start UDP media transport", status);
        return;
    }

    /* Get the media port interface of the audio stream. 
     * Media port interface is basicly a struct containing get_frame() and
     * put_frame() function. With this media port interface, we can attach
     * the port interface to conference bridge, or directly to a sound
     * player/recorder device.
     */
    status = pjmedia_stream_get_port(g_med_stream, &media_port);
    if (status != PJ_SUCCESS) {
        app_perror( THIS_FILE, "Unable to create media port interface of the audio stream", status);
        return;
    }

    /* Create sound port */
    status = pjmedia_snd_port_create(inv->pool,
                            PJMEDIA_AUD_DEFAULT_CAPTURE_DEV,
                            PJMEDIA_AUD_DEFAULT_PLAYBACK_DEV,
                            PJMEDIA_PIA_SRATE(&media_port->info),/* clock rate      */
                            PJMEDIA_PIA_CCNT(&media_port->info),/* channel count    */
                            PJMEDIA_PIA_SPF(&media_port->info), /* samples per frame*/
                            PJMEDIA_PIA_BITS(&media_port->info),/* bits per sample  */
                            0,
                            &g_snd_port);

    if (status != PJ_SUCCESS) 
    {
        app_perror( THIS_FILE, "Unable to create sound port", status);
        PJ_LOG(3,(THIS_FILE, "%d %d %d %d",
                    PJMEDIA_PIA_SRATE(&media_port->info),/* clock rate      */
                    PJMEDIA_PIA_CCNT(&media_port->info),/* channel count    */
                    PJMEDIA_PIA_SPF(&media_port->info), /* samples per frame*/
                    PJMEDIA_PIA_BITS(&media_port->info) /* bits per sample  */
            ));
        return;
    }

    status = pjmedia_snd_port_connect(g_snd_port, media_port);


    /* Get the media port interface of the second stream in the session,
     * which is video stream. With this media port interface, we can attach
     * the port directly to a renderer/capture video device.
     */

    /* Done with media. */
}

static pj_status_t init_stack()
{
    pj_sockaddr addr;
    pjsip_inv_callback inv_cb;
    pj_status_t status;
    int af = AF;

    pj_log_set_level(5);

    CHECK( pjlib_util_init() );

    pj_caching_pool_init(&app.cp, NULL, 0);
    app.pool = pj_pool_create( &app.cp.factory, "simpleua_calls_many", 512, 512, 0);

    CHECK( pjsip_endpt_create(&app.cp.factory, NULL, &app.g_endpt) );



    pj_sockaddr_init((pj_uint16_t)AF, &addr, NULL, (pj_uint16_t)SIP_PORT);

    //pj_sockaddr_init(af, &addr, NULL, (pj_uint16_t)SIP_PORT);
    
    if (af == pj_AF_INET()) 
    {
        status = pjsip_udp_transport_start( app.g_endpt, &addr.ipv4, NULL, 
                                            1, NULL);
    } 
    else if (af == pj_AF_INET6()) 
    {
        status = pjsip_udp_transport_start6(app.g_endpt, &addr.ipv6, NULL,
                                            1, NULL);
    } 
    else 
    {
        status = PJ_EAFNOTSUP;
    }

    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to start UDP transport", status);
        return 1;
    }

    status = pjsip_tsx_layer_init_module(app.g_endpt) ||
             pjsip_ua_init_module( app.g_endpt, NULL );
    CHECK( status );

    pj_bzero(&inv_cb, sizeof(inv_cb));
    inv_cb.on_state_changed = &call_on_state_changed;
    inv_cb.on_new_session = &call_on_forked;
    inv_cb.on_media_update = &call_on_media_update;
    //inv_cb.on_rx_offer = &call_on_rx_offer;

    status = pjsip_inv_usage_init(app.g_endpt, &inv_cb) ||
             pjsip_100rel_init_module(app.g_endpt) ||
             //pjsip_endpt_register_module( app.g_endpt, &mod_sipecho) ||
             pjsip_endpt_register_module( app.g_endpt, &msg_logger);
             //pjmedia_endpt_create(&app.cp.factory,
                //                pjsip_endpt_get_ioqueue(app.g_endpt),
                //                0, &app.med_endpt) ||
            //  pj_thread_create(app.pool, "sipecho", &worker_proc, NULL, 0, 0,
            //                   &app.worker_thread);
    CHECK( status );

    return PJ_SUCCESS;

on_error:
    return status;
}

static void destroy_call(struct call_t *call)
{
    call->g_inv = NULL;
}

static int worker_proc(void *arg)
{
    PJ_UNUSED_ARG(arg);

    while (!app.quit) 
    {
        pj_time_val interval = { 0, 20 };
        pjsip_endpt_handle_events(app.g_endpt, &interval);
    }

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
    close_snd_dev();

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

// /* Close existing sound device */
// static void close_snd_dev(void)
// {
//     pj_log_push_indent();

//     /* Close sound device */
//     if (app.snd_port) {
//         pjmedia_aud_dev_info cap_info, play_info;
//         pjmedia_aud_stream *strm;
//         pjmedia_aud_param param;
//         pj_status_t status;

//         strm = pjmedia_snd_port_get_snd_stream(pjsua_var.snd_port);
//         status = pjmedia_aud_stream_get_param(strm, &param);

//         if (status != PJ_SUCCESS ||
//             param.rec_id == PJSUA_SND_NO_DEV ||
//             pjmedia_aud_dev_get_info(param.rec_id, &cap_info) != PJ_SUCCESS)
//         {
//             cap_info.name[0] = '\0';
//         }
//         if (status != PJ_SUCCESS ||
//             pjmedia_aud_dev_get_info(param.play_id, &play_info) != PJ_SUCCESS)
//         {
//             play_info.name[0] = '\0';
//         }

//         PJ_LOG(4,(THIS_FILE, "Closing %s sound playback device and "
//                              "%s sound capture device",
//                              play_info.name, cap_info.name));

//         /* Unsubscribe from audio device events */
//         pjmedia_event_unsubscribe(NULL, &on_media_event, NULL, strm);

//         pjmedia_snd_port_disconnect(pjsua_var.snd_port);
//         pjmedia_snd_port_destroy(pjsua_var.snd_port);
//         pjsua_var.snd_port = NULL;
//     }

//     /* Close null sound device */
//     if (pjsua_var.null_snd) {
//         PJ_LOG(4,(THIS_FILE, "Closing null sound device.."));
//         pjmedia_master_port_destroy(pjsua_var.null_snd, PJ_FALSE);
//         pjsua_var.null_snd = NULL;
//     }

//     if (pjsua_var.snd_pool)
//         pj_pool_release(pjsua_var.snd_pool);

//     pjsua_var.snd_pool = NULL;
//     pjsua_var.snd_is_on = PJ_FALSE;

//     pj_log_pop_indent();
// }
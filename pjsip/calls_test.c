#include <pjsip.h>
#include <pjmedia.h>
#include <pjmedia-codec.h>
#include <pjsip_ua.h>
#include <pjsip_simple.h>
#include <pjlib-util.h>
#include <pjlib.h>

/* Settings */
#define THIS_FILE                   "calls.c"
#define CLOCK_RATE                  16000
#define SAMPLES_PER_FRAME           (CLOCK_RATE/100)
#define BITS_PER_SAMPLE             16
#define NCHANNELS                   1
#define MAX_CALLS                   30
#define SIP_PORT                    5062            /* Listening SIP port              */
#define RTP_PORT                    4000            /* RTP port                        */
#define AF                          (pj_AF_INET())  /* Change to pj_AF_INET6() for IPv6.
                                                * PJ_HAS_IPV6 must be enabled and
                                                * your system must support IPv6.  */
#define NUMBER_OF_TONES_IN_ARRAY    1
#define FILE_NAME "output_file.wav"

typedef struct 
{
    pjmedia_tone_desc   tone;
    unsigned            tone_slot;
    pjmedia_port        *tone_pjmedia_port;
} player_tone_t;

typedef struct call_t 
{
    pjsip_inv_session           *inv;
    pjsip_dialog                *dlg;
    pjmedia_stream              *stream;
    pjmedia_port                *port;
    unsigned                    slot;
    pj_bool_t                   in_use;
    pjmedia_transport           *transport;
    pj_str_t                    sip_uri_target_user;
    pj_timer_entry              ringing_timer;
    pj_timer_entry              call_media_timer;
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

    pj_str_t                    kvp_tone_player_name;
    player_tone_t               kvp_tone;

    unsigned                    writer_slot;
    call_t                      calls[MAX_CALLS];
    pj_thread_t                 *worker_thread;
    pj_bool_t                   quit;
    pj_mutex_t                  *mutex;

    int                         free_timer_id;
} app;


/* Function prototypes */

static pj_status_t init_system(void);
static pj_status_t init_pjsip(void);
static pj_status_t init_pjmedia(void);

static pj_status_t cleanup_all_resources(void);
static pj_status_t cleanup_call(call_t *call);
static pj_status_t cleanup_ports(void);
static pj_status_t destroy_port(pjmedia_port *port);
static pj_status_t cleanup_media(void);
static void release_all_pools(void);

static void call_on_state_changed_cb(pjsip_inv_session *inv, pjsip_event *e);
static void call_on_media_update_cb(pjsip_inv_session *inv, pj_status_t status);

static pj_bool_t on_rx_request(pjsip_rx_data *rdata);

static void saving_call_information(int call_idx,
                                    pjmedia_transport *transport,
                                    pjsip_dialog *dlg,
                                    pj_str_t *target_uri);

/* Check if the number exist in avaible numbers */
static pj_status_t exists_in_available_numbers(pj_str_t *target_uri);

/* Initialization and start of the timer */
static pj_status_t init_and_schedule_timer(pj_timer_entry *timer,
                                            int call_idx,
                                            long sec,
                                            long msec,
                                            pj_timer_heap_callback *cb);

/* Check the number of active calls */
static int check_number_active_calls(void);

/* function for worker thread */
static int worker_proc(void *arg);

/* Adding media to a call */
static pj_status_t add_media_to_call(call_t *call);

/* Connecting master port */
static pj_status_t create_and_connect_master_port();

/* Add the player to the bridge */
static pj_status_t create_and_connect_player_to_conf(const char *filename);

/* Add tone to the bridge */
static pj_status_t create_and_connect_tone_to_conf(player_tone_t *player);

/* Helper functions for timers*/
static int get_new_timer_id(void);

/* Callback for timers */
static void ringing_timeout_cb(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void call_media_timeout_cb(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);

/* Util to display the error message for the specified error code  */
static int app_perror(const char *sender, const char *title, pj_status_t status);

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

int main()
{
    pj_status_t status;

    status = init_system();
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "Unable init system", status);
        return 1;
    }

    status = pj_mutex_create(app.pool, "mutex", PJ_MUTEX_SIMPLE, &app.mutex);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Initializing the call array */
    for (int i = 0; i < MAX_CALLS; i++)
    {
        app.calls[i].in_use = PJ_FALSE;
    }

    /* Initialize free_timer_id */
    app.free_timer_id = 1;


    /* Set the namber of the player and tones 
     * to choose sound */
    app.wav_player_name = pj_str("100");

    /* Creating and connecting player to the bridge */
    status = create_and_connect_player_to_conf(FILE_NAME);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable create and connect player port", status);
        return 1;
    }

    /* Tone initialization */
    app.long_tone.tone.freq1 =          425;
    app.long_tone.tone.freq2 =           0;
    app.long_tone.tone.on_msec =         1000;
    app.long_tone.tone.off_msec =        0;
    app.long_tone.tone.volume =          0;
    app.long_tone.tone.flags =           0;
    app.long_tone.tone_slot =            -1;
    app.long_tone.tone_pjmedia_port =   NULL;
    app.long_tone_player_name =         pj_str("200");

    /* Creating and attaching a tone to the bridge*/
    status = create_and_connect_tone_to_conf(&app.long_tone);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Tone initialization */
    app.kvp_tone.tone.freq1 =           425;
    app.kvp_tone.tone.freq2 =           0;
    app.kvp_tone.tone.on_msec =         1000;
    app.kvp_tone.tone.off_msec =        4000;
    app.kvp_tone.tone.volume =          0;
    app.kvp_tone.tone.flags =           0;
    app.kvp_tone.tone_slot =            -1;
    app.kvp_tone.tone_pjmedia_port =   NULL;
    app.kvp_tone_player_name =         pj_str("300");

    /* Creating and attaching a tone to the bridge*/
    status = create_and_connect_tone_to_conf(&app.kvp_tone);

    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Create event manager */
    status = pjmedia_event_mgr_create(app.pool, 0, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /*Creating a new thread - it will handle events*/
    pj_thread_create(app.pool, "sipecho", &worker_proc, NULL, 0, 0,
        &app.worker_thread);

    /* Main loop */
    for (;;)
    {
        char s[10];

        printf("\nMenu:\n\tq\tQuit\n");

        if (fgets(s, sizeof(s), stdin) == NULL)
            continue;

        if (s[0]=='q')
            break;
    }

    cleanup_all_resources();

    return 0;
}

/* Initialization SIP */
static pj_status_t init_pjsip(void)
{
    pj_status_t status;
    
    const pj_str_t *hostname = pj_gethostname();

    status = pjsip_endpt_create(&app.cp.factory, hostname->ptr, &app.sip_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Adding UDP transport */
    pj_sockaddr addr;
    pj_sockaddr_init(pj_AF_INET(), &addr, NULL, (pj_uint16_t)SIP_PORT);

    status = pjsip_udp_transport_start(app.sip_endpt, &addr.ipv4, NULL, 1, NULL);
    if (status != PJ_SUCCESS) return status;

    /* Initialization of modules */
    status = pjsip_tsx_layer_init_module(app.sip_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_ua_init_module(app.sip_endpt, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Initialize the INVITE module */
    pjsip_inv_callback inv_cb;
    pj_bzero(&inv_cb, sizeof(inv_cb));
    inv_cb.on_state_changed = &call_on_state_changed_cb;
    inv_cb.on_media_update = &call_on_media_update_cb;

    status = pjsip_inv_usage_init(app.sip_endpt, &inv_cb);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Initialize 100rel support */
    status = pjsip_100rel_init_module(app.sip_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Register modules */
    status = pjsip_endpt_register_module(app.sip_endpt, &mod_simpleua);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_endpt_register_module( app.sip_endpt, &msg_logger);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    return PJ_SUCCESS;
}

/* Initialize media */
static pj_status_t init_pjmedia(void)
{
    pj_status_t status;

    status = pjmedia_endpt_create(&app.cp.factory, NULL, 1, &app.med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    app.pool = pjmedia_endpt_create_pool(app.med_endpt, "Media pool", 16000, 16000);
    if (!app.pool) return PJ_ENOMEM;

    status = pjmedia_codec_g711_init(app.med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjmedia_conf_create(app.pool, MAX_CALLS+4, CLOCK_RATE, 
                                NCHANNELS, SAMPLES_PER_FRAME, 
                                BITS_PER_SAMPLE, PJMEDIA_CONF_NO_DEVICE, &app.conf);
    if (status != PJ_SUCCESS) return status;

    /* Creating and connecting the master port */
    status = create_and_connect_master_port();
    if (status != PJ_SUCCESS) 
    {
        return status;
    }

    return PJ_SUCCESS;
}

/* Call state callback */
static void call_on_state_changed_cb(pjsip_inv_session *inv, pjsip_event *event)
{
    PJ_UNUSED_ARG(event);

    if (inv->state == PJSIP_INV_STATE_DISCONNECTED)
    {
        pj_mutex_lock(app.mutex);

        /* Find the call in the array */
        for (int i = 0; i < MAX_CALLS; i++) 
        {
            if (app.calls[i].inv == inv && app.calls[i].in_use) 
            {
                PJ_LOG(3, (THIS_FILE, "CALL %d - IS CLEANING NOW", app.calls[i].slot));
                cleanup_call(&app.calls[i]);
                PJ_LOG(3, (THIS_FILE, "CALL WAS CLEANED"));
                break;
            }
        }

        pj_mutex_unlock(app.mutex);
    }

    return;
}

/* Clear call */
static pj_status_t cleanup_call(call_t *call)
{
    pj_status_t status;
    if (!call->in_use) return PJ_SUCCESS;

    if (call->inv && call->inv->dlg)
    {
        status = pjsip_inv_terminate(call->inv, PJSIP_SC_OK, PJ_FALSE);
        app_perror(THIS_FILE, "Failed to forcefully terminate and destroy INVITE session", status);
    }

    if (call->port && call->slot != (unsigned)-1)
    {
        status = pjmedia_conf_remove_port(app.conf, call->slot);
        app_perror(THIS_FILE, "Failed to remove the specified port from the conference bridge", status);
    }

    if (call->stream)
    {
        status = pjmedia_stream_destroy(call->stream);
        app_perror(THIS_FILE, "Failed to destroy the media stream", status);
    }

    if (call->transport)
    {
        status = pjmedia_transport_close(call->transport);
        app_perror(THIS_FILE, "Failed to close media transport", status);
    }

    if (call->ringing_timer.id != PJ_FALSE)
    {
        pjsip_endpt_cancel_timer(app.sip_endpt, &call->ringing_timer);
        PJ_LOG(3, (THIS_FILE, "pjsip_endpt_cancel_timer : ringing_timer - DONE"));
    }

    if (call->call_media_timer.id != PJ_FALSE)
    {
        pjsip_endpt_cancel_timer(app.sip_endpt, &call->call_media_timer);
        PJ_LOG(3, (THIS_FILE, "pjsip_endpt_cancel_timer : call_media_timer - DONE"));
    }

    call->in_use = PJ_FALSE;
    call->inv = NULL;
    call->port = NULL;
    call->stream = NULL;
    call->slot = (unsigned)-1;
    call->transport = NULL;

    return PJ_SUCCESS;
}

/* Clear all resources */
static pj_status_t cleanup_all_resources(void) 
{
    pj_status_t status;

    /* Clear all calls */
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        cleanup_call(&app.calls[i]);
    }

    if (app.mutex)
    {
        status = pj_mutex_destroy(app.mutex);
        app.mutex = NULL;

        if (status != PJ_SUCCESS)
            app_perror(THIS_FILE, "Failed to destroy mutex", status);
    }

    /* Clear all media resources */
    cleanup_media();

    if (app.sip_endpt)
    {
        pjsip_endpt_destroy(app.sip_endpt);
        
        PJ_LOG(3, (THIS_FILE, "Destroying endpoint instance"));
    }

    /* Pools releasing */
    release_all_pools();

    pj_shutdown();

    return PJ_SUCCESS;
}


static pj_status_t cleanup_ports(void)
{
    pj_status_t status;

    /* Stop master port */
    if (app.null_snd)
    {
        status = pjmedia_master_port_stop(app.null_snd);
        if (status != PJ_SUCCESS)
            app_perror(THIS_FILE, "Failed to stop the media flow", status);

        status = pjmedia_master_port_destroy(app.null_snd, PJ_FALSE);
        if (status != PJ_SUCCESS)
            app_perror(THIS_FILE, "Failed to destroy the master port", status);
        
        app.null_snd = NULL;
    }

    /* Destroying ports */
    if (app.null_port)
    {
        destroy_port(app.null_port);
    }

    if (app.wav_port)
    {
        status = pjmedia_conf_remove_port(app.conf, (unsigned)app.wav_slot);
        if (status != PJ_SUCCESS)
        {
            app_perror(THIS_FILE, "Failed to remove the specified port from the conference bridge", status);
        }

        destroy_port(app.wav_port);
    }

    if (app.long_tone.tone_pjmedia_port)
    {
        status = pjmedia_conf_remove_port(app.conf, (unsigned)app.long_tone.tone_slot);
        if (status != PJ_SUCCESS)
        {
            app_perror(THIS_FILE, "Failed to remove the specified port from the conference bridge", status);
        }

        destroy_port(app.long_tone.tone_pjmedia_port);

        app.long_tone.tone_pjmedia_port = NULL;
    }

    if (app.kvp_tone.tone_pjmedia_port)
    {
        status = pjmedia_conf_remove_port(app.conf, (unsigned)app.kvp_tone.tone_slot);
        if (status != PJ_SUCCESS)
        {
            app_perror(THIS_FILE, "Failed to remove the specified port from the conference bridge", status);
        }

        destroy_port(app.kvp_tone.tone_pjmedia_port);
    }

    return PJ_SUCCESS;
}

/* Destroying ports */
static pj_status_t destroy_port(pjmedia_port *port)
{
    pj_status_t status;

    if (port)
    {
        status = pjmedia_port_destroy(port);

        if (status != PJ_SUCCESS)
        {
            app_perror(THIS_FILE, "Failed to destroy port (and subsequent downstream ports)", status);
            PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
        }
    }
    return PJ_SUCCESS;
}

/* Clear all media resources */
static pj_status_t cleanup_media(void)
{
    pj_status_t status;

    cleanup_ports();

    /* Conference conferece bridge  */
    if (app.conf)
    {
        status = pjmedia_conf_destroy(app.conf);

        if (status != PJ_SUCCESS)
            app_perror(THIS_FILE, "Failed to destroy conference bridge", status);
    }

    /* Destroy event manager */
    pjmedia_event_mgr_destroy(NULL);

    /* Destroy media endpoint */
    if (app.med_endpt) 
    {
        status = pjmedia_endpt_destroy(app.med_endpt);
        
        if (status != PJ_SUCCESS)
            app_perror(THIS_FILE, "Failed to destroy media endpoint instance and shutdown audio subsystem", status);
    }

    return PJ_SUCCESS;
}

/* Realising pools */
static void release_all_pools(void)
{
    if (app.pool)
    {
        pj_pool_release(app.pool);
    }

    if (app.snd_pool)
    {
        pj_pool_release(app.snd_pool);
    }

    pj_caching_pool_destroy(&app.cp);

    return;
}

static pj_bool_t on_rx_request(pjsip_rx_data *rdata)
{
    pj_status_t status;
    pj_sockaddr hostaddr;
    char temp[80], hostip[PJ_INET6_ADDRSTRLEN];
    pj_str_t local_uri;
    pjsip_dialog *dlg;
    pjmedia_sdp_session *local_sdp;
    pjsip_tx_data *tdata;
    unsigned options = 0;
    int call_idx = -1;
    pjsip_sip_uri *target_sip_uri;
    pj_str_t reason;

    /* Process only INVITE requests */
    if (rdata->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD)
    {
        /* For all other requests (except ACK) we respond with 500 */
        if (rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD)
        {
            reason = pj_str("Simple UA unable to handle this request");

            pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 500, &reason, NULL, NULL);
        }
        return PJ_TRUE;
    }

    /* Check the number of active calls */
    pj_mutex_lock(app.mutex);
    call_idx = check_number_active_calls();

    if (call_idx == -1) 
    {
        reason = pj_str("Too many calls");

        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 486, &reason, NULL, NULL);

        pj_mutex_unlock(app.mutex);

        return PJ_TRUE;
    }

    /* Get target uri */
    target_sip_uri = (pjsip_sip_uri *) pjsip_uri_get_uri(rdata->msg_info.msg->line.req.uri);

    /* Check if the dialed number is correct */
    if ( (exists_in_available_numbers( &(target_sip_uri->user) ) ) != PJ_SUCCESS )
    {
        /* If the number is not entered correctly, we respond with 404 (Not Found)*/
        reason = pj_str("The number is dialed incorrectly");

        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 404, &reason, NULL, NULL);

        pj_mutex_unlock(app.mutex);

        return PJ_TRUE;
    }

    status = pjsip_inv_verify_request(rdata, &options, NULL, NULL, app.sip_endpt, NULL);
    if (status != PJ_SUCCESS)
    {
        reason = pj_str("Sorry Simple UA can not handle this INVITE");

        goto _on_error_respond_stateless;
    } 

    /* Get server IP address for Contact header */
    if (pj_gethostip(pj_AF_INET(), &hostaddr) != PJ_SUCCESS) 
    {
        reason = pj_str("Server error");
        
        goto _on_error_respond_stateless;
    }

    /* Формируем Contact URI */
    pj_sockaddr_print(&hostaddr, hostip, sizeof(hostip), 2);
    pj_ansi_snprintf(temp, sizeof(temp), "<sip:%.*s@%s:%d>", (int)target_sip_uri->user.slen, target_sip_uri->user.ptr, hostip,SIP_PORT);
    local_uri = pj_str(temp);

    /* Создаем UAS dialog */
    status = pjsip_dlg_create_uas_and_inc_lock(pjsip_ua_instance(), rdata, &local_uri, &dlg);
    if (status != PJ_SUCCESS) 
    {
        reason = pj_str("Failed to create dialog");
        
        goto _on_error_respond_stateless;
    }

    /* Создаем медиа транспорт для SDP оффера */
    pjmedia_transport *transport;

    status = pjmedia_transport_udp_create3(app.med_endpt, pj_AF_INET(), NULL, NULL,
                                            RTP_PORT + (call_idx * 2), 0,
                                            &transport);
    if (status != PJ_SUCCESS)
    {
        pjsip_dlg_dec_lock(dlg);
        
        reason = pj_str("Media transport error");
        
        goto _on_error_respond_stateless;
    }

    /* Создаем SDP оффер */
    pjmedia_sock_info sock_info;
    pjmedia_transport_info tp_info;
    pjmedia_transport_info_init(&tp_info);
    pjmedia_transport_get_info(transport, &tp_info);
    pj_memcpy(&sock_info, &tp_info.sock_info, sizeof(pjmedia_sock_info));

    status = pjmedia_endpt_create_sdp(app.med_endpt, dlg->pool, 1, &sock_info, &local_sdp);
    if (status != PJ_SUCCESS) 
    {
        pjmedia_transport_close(transport);

        pjsip_dlg_dec_lock(dlg);

        reason = pj_str("SDP creation error");

        goto _on_error_respond_stateless;
    }

    /* Создаем INVITE сессию */
    status = pjsip_inv_create_uas(dlg, rdata, local_sdp, 0, &app.calls[call_idx].inv);
    if (status != PJ_SUCCESS)
    {
        pjmedia_transport_close(transport);

        pjsip_dlg_dec_lock(dlg);

        reason = pj_str("Invite session error");

        goto _on_error_respond_stateless;
    }

    /* Разблокируем диалог */
    pjsip_dlg_dec_lock(dlg);

    /* Сохраняем информацию о звонке */
    saving_call_information(call_idx, transport, dlg, &target_sip_uri->user);

    PJ_LOG(3,(THIS_FILE, "CALL TO %.*s!!", (int)app.calls[call_idx].sip_uri_target_user.slen, app.calls[call_idx].sip_uri_target_user.ptr));

    pj_mutex_unlock(app.mutex);

    /* Отправляем 180 Ringing */
    status = pjsip_inv_initial_answer(app.calls[call_idx].inv, rdata, 180, NULL, NULL, &tdata);
    if (status != PJ_SUCCESS) 
    {
        return PJ_TRUE;
    }

    status = pjsip_inv_send_msg(app.calls[call_idx].inv, tdata);
    if (status != PJ_SUCCESS)
    {
        return PJ_TRUE;
    }

    // указываем промежуток времни для ringing таймера
    status = init_and_schedule_timer( &(app.calls[call_idx].ringing_timer), call_idx, 3, 0,  &ringing_timeout_cb);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "init_and_schedule_timer", status);
    }

    return PJ_TRUE;

_on_error_respond_stateless:
    pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 500, &reason, NULL, NULL);

    pj_mutex_unlock(app.mutex);

    return PJ_TRUE;
}

// cохранение информации о звонке
static void saving_call_information(int call_idx, pjmedia_transport *transport, pjsip_dialog *dlg, pj_str_t *target_uri)
{
    app.calls[call_idx].in_use = PJ_TRUE;
    app.calls[call_idx].transport = transport;
    app.calls[call_idx].port = NULL;
    app.calls[call_idx].slot = -1;
    app.calls[call_idx].stream = NULL;
    app.calls[call_idx].ringing_timer.id = PJ_FALSE;
    app.calls[call_idx].call_media_timer.id = PJ_FALSE;
    app.calls[call_idx].dlg = dlg;

    // сохрняем target uri в стурктуре звонок
    app.calls[call_idx].sip_uri_target_user.ptr = (char*) pj_pool_alloc(app.pool , 256);
    pj_strcpy(&app.calls[call_idx].sip_uri_target_user, target_uri);

    return;
}

/* Check if the number exist in avaible numbers */
static pj_status_t exists_in_available_numbers(pj_str_t *target_uri)
{
    if ( !( ((pj_strcmp(target_uri, &(app.wav_player_name))) == 0 )
        || ((pj_strcmp( target_uri, &(app.long_tone_player_name))) == 0)
        || ((pj_strcmp( target_uri, &(app.kvp_tone_player_name))) == 0) ))
    {
        return PJ_TRUE;
    }

    return PJ_SUCCESS;
}

/* Initialization and start of the timer */
static pj_status_t init_and_schedule_timer(pj_timer_entry *timer, int call_idx, long sec, long msec, pj_timer_heap_callback *cb)
{
    pj_time_val delay;
    pj_status_t status;

    delay.sec = sec;
    delay.msec = msec;

    // находим своодный id для таймера
    timer->id = get_new_timer_id();

    // инициализируем таймер 
    pj_timer_entry_init(timer, timer->id, (void*)app.calls[call_idx].inv, cb);

    // и запускаем его
    status = pjsip_endpt_schedule_timer(app.sip_endpt, timer, &delay);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "Schedule timer error", status);
        return PJ_TRUE; // 1
    }

    return PJ_SUCCESS;
}

/* Check the number of active calls */
static int check_number_active_calls(void)
{
    int call_idx = -1;

    for (int i = 0; i < MAX_CALLS; i++) 
    {
        if (!app.calls[i].in_use) 
        {
            call_idx = i;
            break;
        }
    }

    return call_idx;
}

/* Connecting master port */
static pj_status_t create_and_connect_master_port()
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
    conf_port = pjmedia_conf_get_master_port(app.conf);
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

/* Add the player to the bridge */
static pj_status_t create_and_connect_player_to_conf(const char *filename)
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
        destroy_port(app.wav_port);
        app_perror(THIS_FILE, "Unable to add file to conference bridge",
        status);
        return status;
    }

    return PJ_SUCCESS;

}

/* Initialize the entire system */
static pj_status_t init_system(void)
{
    pj_status_t status;

    status = pj_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    pj_log_set_level(5);

    status = pjlib_util_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Must create a pool factory before we can allocate any memory. */
    pj_caching_pool_init(&app.cp, &pj_pool_factory_default_policy, 0);

    app.snd_pool = pj_pool_create(&app.cp.factory, "snd", 16000, 16000, NULL);
    if (!app.snd_pool) return PJ_ENOMEM;

    /* Initialization SIP */
    status = init_pjsip();
    if (status != PJ_SUCCESS) return status;

    /* Initialize media */
    status = init_pjmedia();
    if (status != PJ_SUCCESS) return status;

    return PJ_SUCCESS;
}

/* Add tone to the bridge */
static pj_status_t create_and_connect_tone_to_conf(player_tone_t *player)
{
    pj_status_t status;
    char name[80];
    pj_str_t label;

    pj_ansi_snprintf(name,
                    sizeof(name),
                    "tone-%d,%d,%d,%d",
                    player->tone.freq1,
                    player->tone.freq2,
                    player->tone.off_msec,
                    player->tone.on_msec);
    
    label = pj_str(name);

    status = pjmedia_tonegen_create2(app.pool,
                                    &label,
                                    CLOCK_RATE,
                                    NCHANNELS,
                                    SAMPLES_PER_FRAME,
                                    BITS_PER_SAMPLE,
                                    PJMEDIA_TONEGEN_LOOP,
                                    &player->tone_pjmedia_port);

    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "Unable to create tone generator", status);
        return status;
    }

    status = pjmedia_conf_add_port(app.conf, app.pool, player->tone_pjmedia_port, NULL, &player->tone_slot);
    if (status != PJ_SUCCESS)
    {
        destroy_port(player->tone_pjmedia_port);
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

/* Media update handler */
static void call_on_media_update_cb(pjsip_inv_session *inv, pj_status_t status)
{
    call_t *call;

    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(3,(THIS_FILE, "Media update failed: %d", status));
        return;
    }

    /* Find our call by the pointer to the invite session */
    int call_idx = -1;
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        if ( (app.calls[i].inv == inv) && (app.calls[i].in_use) )
        {
            call_idx = i;
            break;
        }
    }
    
    call = &app.calls[call_idx];
    
    /* Add media to the call */
    status = add_media_to_call(call);
    if (status != PJ_SUCCESS) 
    {
        pj_mutex_lock(app.mutex);
        
        call->in_use = PJ_FALSE;
        call->inv = NULL;

        pj_mutex_unlock(app.mutex);

        return;
    }

    /* Initialization and start of the timer */
    status = init_and_schedule_timer( &(call->call_media_timer), call_idx, 7, 0, &call_media_timeout_cb);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "init_and_schedule_timer", status);
    }

    return;
}

/* Adding media to a call */
static pj_status_t add_media_to_call(call_t *call) 
{
    pj_status_t status;
    pjmedia_stream_info stream_info;
    const pjmedia_sdp_session *local_sdp;
    const pjmedia_sdp_session *remote_sdp;

    /* Get active SDP sessions */
    pjmedia_sdp_neg_get_active_local(call->inv->neg, &local_sdp);
    pjmedia_sdp_neg_get_active_remote(call->inv->neg, &remote_sdp);

    /* Create stream info from SDP */
    status = pjmedia_stream_info_from_sdp(&stream_info,
                                        call->inv->dlg->pool,
                                        app.med_endpt,
                                        local_sdp,
                                        remote_sdp,
                                        0);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "Failed to create stream info", status);
        return status;
    }
    
    /* Create media stream */
    status = pjmedia_stream_create(app.med_endpt,
                                 call->inv->dlg->pool,
                                 &stream_info,
                                 call->transport,
                                 NULL,
                                 &call->stream);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "Failed to create media stream", status);
        return status;
    }
    
    /* Start the media stream */
    status = pjmedia_stream_start(call->stream);
    if (status != PJ_SUCCESS) 
    {
        pj_status_t temp_status;
        
        app_perror(THIS_FILE, "Failed to start stream", status);

        temp_status = pjmedia_stream_destroy(call->stream);
        if (temp_status != PJ_SUCCESS)
            app_perror(THIS_FILE, "Failed to destroy the media stream", temp_status);
        
        return status;
    }
    
    /* Start the UDP media transport */
    status = pjmedia_transport_media_start(call->transport, 0, 0, 0, 0);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to start UDP media transport", status);
        return status;
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
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(3,(THIS_FILE, "Failed to add to conference"));
        destroy_port(call->port);
        pjmedia_stream_destroy(call->stream);
        pjmedia_transport_close(call->transport);
        return status;
    }

    if (pj_strcmp( &(call->sip_uri_target_user), &(app.wav_player_name)) == 0)
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
    else  if (pj_strcmp( &(call->sip_uri_target_user), &(app.long_tone_player_name)) == 0)
    {
        // Подключаем к длинному гудку
        if (app.long_tone.tone_pjmedia_port) 
        {
            status = pjmedia_conf_connect_port(app.conf, app.long_tone.tone_slot, call->slot, 0);
            if (status != PJ_SUCCESS) 
            {
                PJ_LOG(3,(THIS_FILE, "Failed to connect to WAV player"));
            }
        }
    }
    else  if (pj_strcmp( &(call->sip_uri_target_user), &(app.kvp_tone_player_name)) == 0)
    {
        // Подключаем к КВП гудку
        if (app.kvp_tone.tone_pjmedia_port) 
        {
            status = pjmedia_conf_connect_port(app.conf, app.kvp_tone.tone_slot, call->slot, 0);
            if (status != PJ_SUCCESS) 
            {
                PJ_LOG(3,(THIS_FILE, "Failed to connect to WAV player"));
            }
        }
    }
    else PJ_LOG(3,(THIS_FILE, "NO TARGET_USER == PLAYER_NAME"));
    
    return PJ_SUCCESS;
}

// static pj_status_t add_media_to_call(call_t *call) 
// {
//     pj_status_t status = PJ_SUCCESS;
//     pjmedia_stream_info stream_info;
//     const pjmedia_sdp_session *local_sdp = NULL;
//     const pjmedia_sdp_session *remote_sdp = NULL;
//     pjmedia_port *port = NULL;

//     /* Get active SDP sessions */
//     pjmedia_sdp_neg_get_active_local(call->inv->neg, &local_sdp);
//     pjmedia_sdp_neg_get_active_remote(call->inv->neg, &remote_sdp);

//     /* Create stream info from SDP */
//     status = pjmedia_stream_info_from_sdp(&stream_info,
//                                         call->inv->dlg->pool,
//                                         app.med_endpt,
//                                         local_sdp,
//                                         remote_sdp,
//                                         0);
//     if (status != PJ_SUCCESS) 
//     {
//         app_perror(THIS_FILE, "Failed to create stream info", status);
//         goto on_return;
//     }

//     /* Create media stream */
//     status = pjmedia_stream_create(app.med_endpt,
//                                  call->inv->dlg->pool,
//                                  &stream_info,
//                                  call->transport,
//                                  NULL,
//                                  &call->stream);
//     if (status != PJ_SUCCESS) 
//     {
//         app_perror(THIS_FILE, "Failed to create media stream", status);
//         goto on_return;
//     }

//     /* Start the media stream */
//     status = pjmedia_stream_start(call->stream);
//     if (status != PJ_SUCCESS) 
//     {
//         app_perror(THIS_FILE, "Failed to start media stream", status);
//         goto on_destroy_stream;
//     }

//     /* Start UDP media transport */
//     status = pjmedia_transport_media_start(call->transport, 0, 0, 0, 0);
//     if (status != PJ_SUCCESS) 
//     {
//         app_perror(THIS_FILE, "Failed to start media transport", status);
//         goto on_stop_stream;
//     }

//     /* Get media port from stream */
//     status = pjmedia_stream_get_port(call->stream, &port);
//     if (status != PJ_SUCCESS) 
//     {
//         app_perror(THIS_FILE, "Failed to get media port", status);
//         goto on_stop_transport;
//     }

//     /* Add port to conference bridge */
//     status = pjmedia_conf_add_port(app.conf,
//                                  call->inv->dlg->pool,
//                                  port,
//                                  NULL,
//                                  &call->slot);
//     if (status != PJ_SUCCESS) 
//     {
//         app_perror(THIS_FILE, "Failed to add port to conference", status);
//         goto on_destroy_port;
//     }

//     /* Connect to appropriate player based on target URI */
//     if (pj_strcmp(&call->sip_uri_target_user, &app.wav_player_name) == 0 && app.wav_port) 
//     {
//         status = pjmedia_conf_connect_port(app.conf, app.wav_slot, call->slot, 0);
//         if (status != PJ_SUCCESS) 
//         {
//             PJ_LOG(3, (THIS_FILE, "Warning: Failed to connect to WAV player"));
//         }
//     }
//     else if (pj_strcmp(&call->sip_uri_target_user, &app.long_tone_player_name) == 0 && 
//              app.long_tone.tone_pjmedia_port) 
//     {
//         status = pjmedia_conf_connect_port(app.conf, app.long_tone.tone_slot, call->slot, 0);
//         if (status != PJ_SUCCESS) 
//         {
//             PJ_LOG(3, (THIS_FILE, "Warning: Failed to connect to long tone"));
//         }
//     }
//     else if (pj_strcmp(&call->sip_uri_target_user, &app.kvp_tone_player_name) == 0 && 
//              app.kvp_tone.tone_pjmedia_port) 
//     {
//         status = pjmedia_conf_connect_port(app.conf, app.kvp_tone.tone_slot, call->slot, 0);
//         if (status != PJ_SUCCESS) 
//         {
//             PJ_LOG(3, (THIS_FILE, "Warning: Failed to connect to KVP tone"));
//         }
//     }

//     /* Success - assign port to call structure */
//     call->port = port;
//     return PJ_SUCCESS;

// /* Error handling cascade */
// on_destroy_port:
//     if (port) 
//     {
//         pjmedia_port_destroy(port);
//         port = NULL;
//     }

// on_stop_transport:
//     pjmedia_transport_media_stop(call->transport);

// on_stop_stream:
//     pjmedia_stream_stop(call->stream);

// on_destroy_stream:
//     pjmedia_stream_destroy(call->stream);
//     call->stream = NULL;

// on_close_transport:
//     pjmedia_transport_close(call->transport);
//     call->transport = NULL;

// on_return:
//     return status;
// }

/* Generate new(free) timer ID */
static int get_new_timer_id(void)
{
    int i;
    int timer_id = 0;
    pj_bool_t quit = PJ_FALSE;

    while (quit == PJ_FALSE)
    {
        quit = PJ_TRUE;
        for (i = 0; i < MAX_CALLS; ++i)
        {
            if ( (app.calls[i].ringing_timer.id == timer_id) || (app.calls[i].call_media_timer.id == timer_id) )
            {
                ++timer_id;
                quit = PJ_FALSE;
                break;
            }
        }
    }
    return timer_id;
}

/* Function for worker thread */
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

static void ringing_timeout_cb(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
    pjsip_inv_session *inv = (pjsip_inv_session *)entry->user_data;
    pjsip_tx_data *tdata;
    pj_status_t status;
    
    PJ_UNUSED_ARG(timer_heap);
    
    // Отправляем 200 OK
    status = pjsip_inv_answer(inv, 200, NULL, NULL, &tdata);
    if (status == PJ_SUCCESS)
    {
        pjsip_inv_send_msg(inv, tdata);
    }

    return;
}


static void call_media_timeout_cb(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
    pjsip_inv_session *inv = (pjsip_inv_session *)entry->user_data;
    pjsip_tx_data *tdata;
    pj_str_t reason = pj_str("Call completed");
    pj_status_t status;

    PJ_UNUSED_ARG(timer_heap);

    /* Отправляем BYE */
    status = pjsip_inv_end_session(inv, PJSIP_SC_OK, &reason, &tdata);
    if (status == PJ_SUCCESS && tdata)
    {
        pjsip_inv_send_msg(inv, tdata);
    }

    return;
}

/* Util to display the error message for the specified error code  */
static int app_perror( const char *sender, const char *title, pj_status_t status)
{
    char errmsg[PJ_ERR_MSG_SIZE];

    pj_strerror(status, errmsg, sizeof(errmsg));

    PJ_LOG(3,(sender, "%s: %s [code=%d]", title, errmsg, status));
    return 1;
}
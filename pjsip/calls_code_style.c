#include <pjsip.h>
#include <pjmedia.h>
#include <pjmedia-codec.h>
#include <pjsip_ua.h>
#include <pjsip_simple.h>
#include <pjlib-util.h>
#include <pjlib.h>

/* Settings */
#define THIS_FILE                   "calls_test.c"
#define FILE_NAME                   "output_4.wav"
#define MOD_AUTOANSWER_NAME         "mod-autoanswerer"
#define MOD_MSG_LOG_NAME            "mod-msg-log"
#define WAV_PLAYER_NAME             "100"
#define LONG_TONE_NAME              "200"
#define KPV_TONE_NAME               "300"
#define THREAD_NAME                 "worker"
#define MUTEX_NAME                  "mutex_calls"
#define CLOCK_RATE                  16000
#define SAMPLES_PER_FRAME           (CLOCK_RATE/100)
#define BITS_PER_SAMPLE             16
#define NCHANNELS                   1
#define MAX_CALLS                   30
#define SIP_PORT                    5062
#define RTP_PORT                    4000
#define AF                          (pj_AF_INET())
#define NUMBER_OF_TONES_IN_ARRAY    1
#define FLAGS_BITMASK_IPV6          2
#define MULTIPLIER_RTP_PORT         2
#define FREQ1                       425
#define FREQ2                       0
#define ON_MSEC                     1000
#define OFF_MSEC_LONG_TONE          0
#define OFF_MSEC_KPV_TONE           4000
#define PTIME                       (SAMPLES_PER_FRAME * 1000 / NCHANNELS / CLOCK_RATE)
#define RINGING_TIMER_SEC           3
#define RINGING_TIMER_MSEC          0
#define MEDIA_TIMER_SEC             7
#define MEDIA_TIMER_MSEC            0
#define BUF_SIZE_WAV_PLAYEER        0
#define OK_ANSWER                   200
#define RINGING_ANSWER              180
#define UNDEFINED_ID                -1
#define POOL_INCREMENT_SIZE         4000
#define POOL_SIZE                   4000
#define NUM_USED_APP_PORTS          4
#define LOG_LEVEL                   5
#define MAX_TIME_EVENTS_WAIT        10
#define LOG_LEVEL_MIDDLE            4
#define MAX_SIP_URI_SIZE            256

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

    pj_str_t                    kpv_tone_player_name;
    player_tone_t               kpv_tone;

    call_t                      calls[MAX_CALLS];
    pj_thread_t                 *worker_thread;
    pj_bool_t                   quit;
    pj_mutex_t                  *mutex;
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
static pj_status_t exists_in_available_numbers(pj_str_t *target_sip_uri);
static int find_free_call_slot(void);
static pj_bool_t process_non_invite_request(pjsip_rx_data *rdata);
static void respond_busy(pjsip_rx_data *rdata);
static pjsip_sip_uri* get_target_uri(pjsip_rx_data *rdata);
static void respond_not_found(pjsip_rx_data *rdata);
static pj_bool_t verify_invite_session(pjsip_rx_data *rdata);
static pj_status_t send_ringing_response(call_t *call, pjsip_rx_data *rdata);
static pj_status_t connect_to_audio_source(call_t *call);
static pj_status_t add_media_to_call(call_t *call);
static pj_status_t create_and_connect_master_port();
static pj_status_t add_to_conference_bridge(call_t *call);
static int get_new_timer_id(void);
static void ringing_timeout_cb(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void call_media_timeout_cb(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static pj_status_t create_invite_session(pjsip_dialog *dlg,
                                        pjsip_rx_data *rdata,
                                        pjmedia_sdp_session *local_sdp,
                                        pjsip_inv_session **inv_session,
                                        int call_idx);
static void saving_call_information(int call_idx,
                                    pjmedia_transport *transport,
                                    pjsip_dialog *dlg,
                                    pj_str_t *target_sip_uri);
static pj_status_t create_call_dialog(pjsip_rx_data *rdata,
                                    int call_idx,
                                    pjsip_sip_uri *target_sip_uri);
static pj_status_t init_and_schedule_timer(pj_timer_entry *timer,
                                        int call_idx,
                                        long sec,
                                        long msec,
                                        pj_timer_heap_callback *cb);
static pj_status_t init_and_schedule_timer(pj_timer_entry *timer,
                                            int call_idx,
                                            long sec,
                                            long msec,
                                            pj_timer_heap_callback *cb);
/* Function for worker thread */
static int worker_proc(void *arg);

/* Util to display the error message for the specified error code  */
static int app_perror(const char *sender, const char *title, pj_status_t status);

/* Add the player to the bridge */
static pj_status_t create_and_connect_player_to_conf(const char *filename);

/* Add tone to the bridge */
static pj_status_t create_and_connect_tone_to_conf(player_tone_t *player);

static pjsip_module mod_simpleua =
{
    NULL, NULL,                     /* prev, next.              */
    {MOD_AUTOANSWER_NAME, (sizeof(MOD_AUTOANSWER_NAME)/sizeof(char))},
    UNDEFINED_ID,
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
    PJ_LOG(4, (THIS_FILE, "RX %d bytes %s from %s %s:%d:\n"
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
    PJ_LOG(4, (THIS_FILE, "TX %ld bytes %s to %s %s:%d:\n"
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
    NULL,
    NULL,
    {MOD_MSG_LOG_NAME, (sizeof(MOD_MSG_LOG_NAME)/sizeof(char))},
    UNDEFINED_ID,
    PJSIP_MOD_PRIORITY_TRANSPORT_LAYER - 1,
    NULL,
    NULL,
    NULL,
    NULL,
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

    status = pj_mutex_create(app.pool, MUTEX_NAME, PJ_MUTEX_SIMPLE, &app.mutex);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Initializing the call array */
    for (int i = 0; i < MAX_CALLS; i++)
    {
        app.calls[i].in_use = PJ_FALSE;
    }

    /* Set the namber of the player and tones 
     * to choose sound */
    app.wav_player_name = pj_str(WAV_PLAYER_NAME);

    status = create_and_connect_player_to_conf(FILE_NAME);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable create and connect player port", status);
        return 1;
    }

    /* Tone initialization */
    app.long_tone.tone.freq1 =          FREQ1;
    app.long_tone.tone.freq2 =          FREQ2;
    app.long_tone.tone.on_msec =        ON_MSEC;
    app.long_tone.tone.off_msec =       OFF_MSEC_LONG_TONE;
    app.long_tone.tone.volume =         0;
    app.long_tone.tone.flags =          0;
    app.long_tone.tone_slot =           (unsigned)UNDEFINED_ID;
    app.long_tone.tone_pjmedia_port =   NULL;
    app.long_tone_player_name =         pj_str(LONG_TONE_NAME);

    /* Creating and attaching a tone to the bridge*/
    status = create_and_connect_tone_to_conf(&app.long_tone);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    
    /* Tone initialization */
    app.kpv_tone.tone.freq1 =           FREQ1; 
    app.kpv_tone.tone.freq2 =           FREQ2;
    app.kpv_tone.tone.on_msec =         ON_MSEC;
    app.kpv_tone.tone.off_msec =        OFF_MSEC_KPV_TONE;
    app.kpv_tone.tone.volume =          0;
    app.kpv_tone.tone.flags =           0;
    app.kpv_tone.tone_slot =            (unsigned)UNDEFINED_ID;
    app.kpv_tone.tone_pjmedia_port =    NULL;
    app.kpv_tone_player_name =          pj_str(KPV_TONE_NAME);

    /* Creating and attaching a tone to the bridge*/
    status = create_and_connect_tone_to_conf(&app.kpv_tone);

    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /* Create event manager */
    status = pjmedia_event_mgr_create(app.pool, 0, NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    /*Creating a new thread - it will handle events*/
    pj_thread_create(app.pool, THREAD_NAME, &worker_proc, NULL, 0, 0, &app.worker_thread);

    /* Main loop */
    for (;;)
    {
        char s[10];

        printf("\nMenu:\n\tq\tQuit\n");

        if (fgets(s, sizeof(s), stdin) == NULL)
            continue;

        if (s[0] =='q')
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

    status = pjsip_endpt_register_module(app.sip_endpt, &msg_logger);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);

    return PJ_SUCCESS;
}

/* Initialize media */
static pj_status_t init_pjmedia(void)
{
    pj_status_t status;

    status = pjmedia_endpt_create(&app.cp.factory, NULL, 1, &app.med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    app.pool = pjmedia_endpt_create_pool(app.med_endpt, "Media pool", POOL_SIZE, POOL_INCREMENT_SIZE);
    if (!app.pool) return PJ_ENOMEM;

    status = pjmedia_codec_g711_init(app.med_endpt);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjmedia_conf_create(app.pool,
                                MAX_CALLS + NUM_USED_APP_PORTS,
                                CLOCK_RATE,
                                NCHANNELS,
                                SAMPLES_PER_FRAME,
                                BITS_PER_SAMPLE,
                                PJMEDIA_CONF_NO_DEVICE,
                                &app.conf);
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

    if ((call->port != NULL) && (call->slot != (unsigned)UNDEFINED_ID))
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
    call->slot = (unsigned)UNDEFINED_ID;
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

    if (app.kpv_tone.tone_pjmedia_port)
    {
        status = pjmedia_conf_remove_port(app.conf, (unsigned)app.kpv_tone.tone_slot);
        if (status != PJ_SUCCESS)
        {
            app_perror(THIS_FILE, "Failed to remove the specified port from the conference bridge", status);
        }

        destroy_port(app.kpv_tone.tone_pjmedia_port);
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
    int call_idx = UNDEFINED_ID;
    pjsip_sip_uri *target_sip_uri;

    /* Process only INVITE requests */
    if (rdata->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD)
    {
        return process_non_invite_request(rdata);
    }

    pj_mutex_lock(app.mutex);
    call_idx = find_free_call_slot();
    if (call_idx == UNDEFINED_ID) 
    {
        respond_busy(rdata);
        pj_mutex_unlock(app.mutex);
        return PJ_TRUE;
    }

    target_sip_uri = get_target_uri(rdata);

    /* Check if the dialed number is correct */
    if (!exists_in_available_numbers(&target_sip_uri->user))
    {
        respond_not_found(rdata);
        pj_mutex_unlock(app.mutex);
        return PJ_TRUE;
    }

    if (!verify_invite_session(rdata))
    {
        pj_mutex_unlock(app.mutex);
        return PJ_TRUE;
    }

    status = create_call_dialog(rdata, call_idx, target_sip_uri);
    if (status != PJ_SUCCESS) 
    {
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 500, NULL, NULL, NULL);
        pj_mutex_unlock(app.mutex);
        return status;
    }

    PJ_LOG(3,(THIS_FILE,
            "CALL TO %.*s!!",
            (int)app.calls[call_idx].sip_uri_target_user.slen,
            app.calls[call_idx].sip_uri_target_user.ptr));

    pj_mutex_unlock(app.mutex);

    status = send_ringing_response(&app.calls[call_idx], rdata);
    if (status != PJ_SUCCESS)
    {
        return status;
    }

    /* Specifies the time interval for the ringing timer */
    status = init_and_schedule_timer( &(app.calls[call_idx].ringing_timer),
                                        call_idx,
                                        RINGING_TIMER_SEC,
                                        RINGING_TIMER_MSEC,
                                        &ringing_timeout_cb);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "init_and_schedule_timer", status);
    }

    return PJ_TRUE;
}

static pj_bool_t process_non_invite_request(pjsip_rx_data *rdata)
{
    if (rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD)
    {
        pj_str_t reason = pj_str("Simple UA unable to handle this request");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 500, &reason, NULL, NULL);
    }
    return PJ_TRUE;
}

static void respond_busy(pjsip_rx_data *rdata)
{
    pj_str_t reason = pj_str("Too many calls");
    pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 486, &reason, NULL, NULL);
}

static pjsip_sip_uri* get_target_uri(pjsip_rx_data *rdata)
{
    return (pjsip_sip_uri*)pjsip_uri_get_uri(rdata->msg_info.msg->line.req.uri);
}

/* Check if the number exist in avaible numbers */
static pj_bool_t exists_in_available_numbers(pj_str_t *number)
{
    return (pj_strcmp(number, &app.wav_player_name) == 0) ||
           (pj_strcmp(number, &app.long_tone_player_name) == 0) ||
           (pj_strcmp(number, &app.kpv_tone_player_name) == 0);
}

static void respond_not_found(pjsip_rx_data *rdata)
{
    pj_str_t reason = pj_str("The number is dialed incorrectly");
    pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 404, &reason, NULL, NULL);
}

static pj_bool_t verify_invite_session(pjsip_rx_data *rdata)
{
    pj_status_t status;
    unsigned options = 0;
    pj_str_t reason;
    
    status = pjsip_inv_verify_request(rdata, &options, NULL, NULL, app.sip_endpt, NULL);
    if (status != PJ_SUCCESS)
    {
        reason = pj_str("Sorry Simple UA can not handle this INVITE");

        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 500, &reason, NULL, NULL);
        return PJ_FALSE;
    }

    return PJ_TRUE;
}

static pj_status_t create_call_dialog(pjsip_rx_data *rdata,
                                    int call_idx,
                                    pjsip_sip_uri *target_sip_uri)
{
    pjmedia_transport *transport;
    pj_sockaddr hostaddr;
    pjsip_dialog *dlg;
    pj_str_t local_uri;
    pjmedia_sdp_session *local_sdp;
    pj_status_t status;
    char temp[80], hostip[PJ_INET6_ADDRSTRLEN];

    /* Get server IP for Contact header */
    status = pj_gethostip(pj_AF_INET(), &hostaddr);
    if (status != PJ_SUCCESS) 
    {
        return status;
    }

    /* Create Contact URI */
    pj_sockaddr_print(&hostaddr, hostip, sizeof(hostip), FLAGS_BITMASK_IPV6);
    pj_ansi_snprintf(temp,
                    sizeof(temp),
                    "<sip:%.*s@%s:%d>",
                    (int)target_sip_uri->user.slen,
                    target_sip_uri->user.ptr,
                    hostip,SIP_PORT);
    local_uri = pj_str(temp);

    /* Create a UAS dialog */
    status = pjsip_dlg_create_uas_and_inc_lock(pjsip_ua_instance(), rdata, &local_uri, &dlg);
    if (status != PJ_SUCCESS) 
    {
        return status;
    }

    /* Create media transport for SDP offer */
    status = pjmedia_transport_udp_create3(app.med_endpt,
                                            pj_AF_INET(),
                                            NULL,
                                            NULL,
                                            RTP_PORT + (call_idx * MULTIPLIER_RTP_PORT),
                                            0,
                                            &transport);
    if (status != PJ_SUCCESS)
    {
        return status;
    }

    /* Create SDP offer */
    pjmedia_sock_info sock_info;
    pjmedia_transport_info tp_info;
    pjmedia_transport_info_init(&tp_info);
    pjmedia_transport_get_info(transport, &tp_info);
    pj_memcpy(&sock_info, &tp_info.sock_info, sizeof(pjmedia_sock_info));

    status = pjmedia_endpt_create_sdp(app.med_endpt, dlg->pool, 1, &sock_info, &local_sdp);
    if (status != PJ_SUCCESS)
    {
        pjmedia_transport_close(transport);
        return status;
    }

    status = create_invite_session(dlg, rdata, local_sdp, &app.calls[call_idx].inv, call_idx);
    if (status != PJ_SUCCESS)
    {
        return status;
    }

    /* Unlock dialog */
    pjsip_dlg_dec_lock(dlg);

    saving_call_information(call_idx, transport, dlg, &target_sip_uri->user);

    return PJ_SUCCESS;
}

static pj_status_t create_invite_session(pjsip_dialog *dlg,
                                        pjsip_rx_data *rdata,
                                        pjmedia_sdp_session *local_sdp,
                                        pjsip_inv_session **inv_session,
                                        int call_idx)
{
    pj_status_t status = pjsip_inv_create_uas(dlg, rdata, local_sdp, 0, inv_session);
    if (status != PJ_SUCCESS) 
    {
        pj_str_t reason = pj_str("Invite session error");
        pjsip_endpt_respond_stateless(app.sip_endpt, rdata, 500, &reason, NULL, NULL);

        pjmedia_transport_close(app.calls[call_idx].transport);
    }

    return status;
}

static pj_status_t send_ringing_response(call_t *call, pjsip_rx_data *rdata)
{
    pjsip_tx_data *tdata;

    pj_status_t status = pjsip_inv_initial_answer(call->inv, rdata, RINGING_ANSWER, NULL, NULL, &tdata);
    if (status != PJ_SUCCESS)
    {
        return status;
    }

    return pjsip_inv_send_msg(call->inv, tdata);
}

/* Saving call information */
static void saving_call_information(int call_idx,
                                    pjmedia_transport *transport,
                                    pjsip_dialog *dlg,
                                    pj_str_t *target_sip_uri)
{
    app.calls[call_idx].in_use = PJ_TRUE;
    app.calls[call_idx].transport = transport;
    app.calls[call_idx].port = NULL;
    app.calls[call_idx].slot = (unsigned)UNDEFINED_ID;
    app.calls[call_idx].stream = NULL;
    app.calls[call_idx].ringing_timer.id = PJ_FALSE;
    app.calls[call_idx].call_media_timer.id = PJ_FALSE;
    app.calls[call_idx].dlg = dlg;

    app.calls[call_idx].sip_uri_target_user.ptr = (char*) pj_pool_alloc(app.pool , MAX_SIP_URI_SIZE);
    pj_strcpy(&app.calls[call_idx].sip_uri_target_user, target_sip_uri);

    return;
}

/* Initialization and start of the timer */
static pj_status_t init_and_schedule_timer(pj_timer_entry *timer,
                                            int call_idx,
                                            long sec,
                                            long msec,
                                            pj_timer_heap_callback *cb)
{
    pj_time_val delay;
    pj_status_t status;

    delay.sec = sec;
    delay.msec = msec;

    timer->id = get_new_timer_id();

    pj_timer_entry_init(timer, timer->id, (void*)app.calls[call_idx].inv, cb);

    status = pjsip_endpt_schedule_timer(app.sip_endpt, timer, &delay);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "Schedule timer error", status);
        return PJ_TRUE;
    }

    return PJ_SUCCESS;
}

/* Check the number of active calls */
static int find_free_call_slot(void)
{
    int call_idx = UNDEFINED_ID;

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
        status = pjmedia_null_port_create(app.pool,
                                        CLOCK_RATE,
                                        1,
                                        SAMPLES_PER_FRAME,
                                        BITS_PER_SAMPLE,
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
    status = pjmedia_master_port_create(app.snd_pool,
                                        app.null_port,
                                        conf_port,
                                        0,
                                        &app.null_snd);
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

    status = pjmedia_wav_player_port_create(app.pool,
                                            filename,
                                            PTIME,
                                            0,
                                            BUF_SIZE_WAV_PLAYEER,
                                            &app.wav_port);

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

    pj_log_set_level(LOG_LEVEL);

    status = pjlib_util_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Must create a pool factory before we can allocate any memory. */
    pj_caching_pool_init(&app.cp, &pj_pool_factory_default_policy, 0);

    app.snd_pool = pj_pool_create(&app.cp.factory, "snd", POOL_SIZE, POOL_INCREMENT_SIZE, NULL);
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
    int call_idx = UNDEFINED_ID;
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        if ( (app.calls[i].inv == inv) && (app.calls[i].in_use) )
        {
            call_idx = i;
            break;
        }
    }
    
    call = &app.calls[call_idx];
    
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
    status = init_and_schedule_timer(&(call->call_media_timer),
                                    call_idx,
                                    MEDIA_TIMER_SEC,
                                    MEDIA_TIMER_MSEC,
                                    &call_media_timeout_cb);
    if (status != PJ_SUCCESS)
    {
        app_perror(THIS_FILE, "init_and_schedule_timer", status);
    }

    return;
}

static pj_status_t create_media_stream(call_t *call, pjmedia_stream_info *stream_info)
{
    pj_status_t status;
    
    status = pjmedia_stream_create(app.med_endpt,
                                 call->inv->dlg->pool,
                                 stream_info,
                                 call->transport,
                                 NULL,
                                 &call->stream);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Failed to create media stream", status);
        return status;
    }

    status = pjmedia_stream_start(call->stream);
    if (status != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Failed to start stream", status);
        pjmedia_stream_destroy(call->stream);
        call->stream = NULL;
        return status;
    }

    return PJ_SUCCESS;
}

static pj_status_t get_media_port(call_t *call)
{
    pj_status_t status = pjmedia_stream_get_port(call->stream, &call->port);
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(3,(THIS_FILE, "Failed to get media port"));
        pjmedia_stream_destroy(call->stream);
        call->stream = NULL;
    }
    return status;
}

static pj_status_t add_to_conference_bridge(call_t *call)
{
    pj_status_t status = pjmedia_conf_add_port(app.conf, 
                                             call->inv->dlg->pool,
                                             call->port, 
                                             NULL, 
                                             &call->slot);
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(3,(THIS_FILE, "Failed to add to conference"));
        pjmedia_port_destroy(call->port);
        call->port = NULL;
    }
    return status;
}

static pj_status_t connect_to_audio_source(call_t *call)
{
    if (pj_strcmp(&call->sip_uri_target_user, &app.wav_player_name) == 0) 
    {
        if (app.wav_port) 
        {
            return pjmedia_conf_connect_port(app.conf, app.wav_slot, call->slot, 0);
        }
    }
    else if (pj_strcmp(&call->sip_uri_target_user, &app.long_tone_player_name) == 0) 
    {
        if (app.long_tone.tone_pjmedia_port) 
        {
            return pjmedia_conf_connect_port(app.conf, app.long_tone.tone_slot, call->slot, 0);
        }
    }
    else if (pj_strcmp(&call->sip_uri_target_user, &app.kpv_tone_player_name) == 0) 
    {
        if (app.kpv_tone.tone_pjmedia_port) 
        {
            return pjmedia_conf_connect_port(app.conf, app.kpv_tone.tone_slot, call->slot, 0);
        }
    }
    
    PJ_LOG(3,(THIS_FILE, "No matching audio source found"));
    return PJ_ENOTFOUND;
}

static pj_status_t add_media_to_call(call_t *call)
{
    pj_status_t status;
    pjmedia_stream_info stream_info;
    const pjmedia_sdp_session *local_sdp, *remote_sdp;

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

    /* Create and start media stream */
    if ((status = create_media_stream(call, &stream_info)) != PJ_SUCCESS) 
    {
        return status;
    }

    /* Start UDP transport */
    if ((status = pjmedia_transport_media_start(call->transport, 0, 0, 0, 0)) != PJ_SUCCESS) 
    {
        app_perror(THIS_FILE, "Unable to start UDP media transport", status);
        pjmedia_stream_destroy(call->stream);
        call->stream = NULL;
        return status;
    }

    /* Get media port */
    if ((status = get_media_port(call)) != PJ_SUCCESS) 
    {
        pjmedia_transport_close(call->transport);
        return status;
    }

    /* Add to conference bridge */
    if ((status = add_to_conference_bridge(call)) != PJ_SUCCESS) 
    {
        pjmedia_stream_destroy(call->stream);
        call->stream = NULL;
        pjmedia_transport_close(call->transport);
        return status;
    }

    /* Connect to audio source */
    connect_to_audio_source(call);

    return PJ_SUCCESS;
}

/* Generate new-free timer ID */
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
        pj_time_val interval = {0, MAX_TIME_EVENTS_WAIT};
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
    
    /* Ansewring 200 (OK) */
    status = pjsip_inv_answer(inv, OK_ANSWER, NULL, NULL, &tdata);
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

    /* Sending BYE */
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
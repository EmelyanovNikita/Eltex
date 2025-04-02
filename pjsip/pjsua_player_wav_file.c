// compile with gcc pjsua_player_wav_file.c -o auto_answer $(pkg-config --cflags --libs libpjproject)
#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>
#include <pjmedia/tonegen.h>

#define THIS_FILE "pjsua_player_wav_file.c"
#define MUSIC_FILE "file.wav"

static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void error_exit(const char *title, pj_status_t status);
pj_status_t registers_wav_file(const char *filename);

typedef struct
{
    pjsua_call_id call_id;
    pj_timer_entry ringing_timer;
    pj_timer_entry answering_timer;
    pj_pool_t *pool;
} call_data_t;

static call_data_t call_data;
static char *wav_filename = NULL;
static pjsua_player_id wav_player_id = PJSUA_INVALID_ID;
static pjsua_conf_port_id wav_port = PJSUA_INVALID_ID;

static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
    pj_status_t status;

    if (call_data.call_id == PJSUA_INVALID_ID) 
    {
        PJ_LOG(1,(THIS_FILE, "Invalid call ID in timer callback"));
        return;
    }
    
    PJ_LOG(3,(THIS_FILE, "After pause"));

    // Answer the call with 200 OK
    status = pjsua_call_answer(call_data.call_id, 200, NULL, NULL);
    if (status != PJ_SUCCESS) error_exit("Error answering call", status);
}

static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
    pj_status_t status;

    if (call_data.call_id == PJSUA_INVALID_ID) 
    {
        PJ_LOG(1,(THIS_FILE, "Invalid call ID in timer callback"));
        return;
    }
    
    PJ_LOG(3,(THIS_FILE, "After answer pause"));
    
    // Hang up the call
    status = pjsua_call_hangup(call_data.call_id, 0, NULL, NULL);
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_call_hangup()", status);
}

/* Callback called by the library upon receiving incoming call */
static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata)
{
    pjsua_call_info ci;
    pj_time_val delay;
    pj_status_t status;

    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);

    pjsua_call_get_info(call_id, &ci);
    
    call_data.call_id = call_id;
    
    PJ_LOG(3,(THIS_FILE, "Incoming call from %.*s!!", (int)ci.remote_info.slen, ci.remote_info.ptr));

    // Send ringing (180 Ringing)
    pjsua_call_answer(call_data.call_id, 180, NULL, NULL);
    
    // 3 sec wait before answering
    delay.sec = 3; 
    delay.msec = 0;
    
    // Set timer ID
    call_data.ringing_timer.id = call_data.call_id != 0 ? call_data.call_id : 1;
    
    // Initialize timer
    pj_timer_entry_init(&call_data.ringing_timer, call_data.ringing_timer.id, 
                       (void*)&call_data.call_id, &ringing_timeout_callback);
    
    PJ_LOG(3,(THIS_FILE, "Before %ld sec pause", delay.sec));
    
    status = pjsua_schedule_timer(&call_data.ringing_timer, &delay);
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_schedule_timer()", status);
}

pj_status_t stream_to_call(pjsua_call_id call_id) 
{
    pj_status_t status;
    
    if (wav_player_id == PJSUA_INVALID_ID) 
    {
        PJ_LOG(1, (THIS_FILE, "No WAV file registered"));
        return PJ_ENOTFOUND;
    }

    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);
    
    /* Connect WAV player to call */
    status = pjsua_conf_connect(wav_port, ci.conf_slot);

    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(1, (THIS_FILE, "Error connecting WAV to call: %d", status));
    }
    
    return status;
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;

    PJ_UNUSED_ARG(e);
    
    pjsua_call_get_info(call_id, &ci);
    
    // Check if call has ended
    if(ci.state == PJSIP_INV_STATE_DISCONNECTED || ci.state == PJSIP_INV_STATE_NULL)
    {
        /* Stop ringing timer if active */
        if (call_data.ringing_timer.id)
        {
            PJ_LOG(3,(THIS_FILE, "TIMER RINGING STOPPED: %d", call_data.ringing_timer.id));
            pjsua_cancel_timer(&call_data.ringing_timer);
            call_data.ringing_timer.id = PJ_FALSE;
        }
        
        /* Stop answering timer if active */
        if (call_data.answering_timer.id)
        {
            PJ_LOG(3,(THIS_FILE, "TIMER ANSWERING STOPPED: %d", call_data.answering_timer.id));
            pjsua_cancel_timer(&call_data.answering_timer);
            call_data.answering_timer.id = PJ_FALSE;
        }
        
        // Reset call ID
        call_data.call_id = PJSUA_INVALID_ID;
        
        PJ_LOG(3,(THIS_FILE, "Call %d state=%.*s", call_id, (int)ci.state_text.slen, ci.state_text.ptr));
        return;
    }
}

static void on_call_media_state(pjsua_call_id call_id) 
{
    pjsua_call_info ci;
    pj_time_val delay;

    pjsua_call_get_info(call_id, &ci);
    
    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) 
    {
        /* Проигравыем WAV файл в звонок */
        stream_to_call(call_id);
        
        /* Устанавливаем таймер */
        delay.sec = 7; 
        delay.msec = 0;
        
        call_data.answering_timer.id = call_data.call_id + 1000;
        pj_timer_entry_init(&call_data.answering_timer, 
                           call_data.answering_timer.id, 
                           (void*)&call_data.call_id, 
                           &answering_timeout_callback);
        
        pjsua_schedule_timer(&call_data.answering_timer, &delay);
    }
}

static void error_exit(const char *title, pj_status_t status)
{
    pjsua_perror(THIS_FILE, title, status);
    pjsua_destroy();
    exit(1);
}

int main()
{
    pjsua_acc_id acc_id;
    pj_status_t status;

    // /* Parse command line arguments */
    // if (argc > 1) 
    // {
    //     wav_filename = argv[1];


    // }

    /* Initialize call data */
    call_data.call_id = PJSUA_INVALID_ID;
    call_data.ringing_timer.id = PJ_FALSE;
    call_data.answering_timer.id = PJ_FALSE;

    /* Create pjsua */
    status = pjsua_create();
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_create()", status);

    /* Init pjsua */
    {
        pjsua_config cfg;
        pjsua_logging_config log_cfg;
        pjsua_media_config media_cfg;

        pjsua_config_default(&cfg);
        cfg.cb.on_incoming_call = &on_incoming_call;
        cfg.cb.on_call_state = &on_call_state;
        cfg.cb.on_call_media_state = &on_call_media_state;

        // Logging level
        pjsua_logging_config_default(&log_cfg);
        log_cfg.console_level = 4;

        pjsua_media_config_default(&media_cfg);
        
        status = pjsua_init(&cfg, &log_cfg, &media_cfg);
        if (status != PJ_SUCCESS) 
            error_exit("Error in pjsua_init()", status);
    }

    /* Add UDP transport. */
    {
        pjsua_transport_config cfg;

        pjsua_transport_config_default(&cfg);
        cfg.port = 5062;
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &cfg, NULL);
        if (status != PJ_SUCCESS) error_exit("Error creating transport", status);
    }

    status = pjsua_start();
    if (status != PJ_SUCCESS) error_exit("Error starting pjsua", status);
    
    {
        pjsua_acc_config cfg;

        pjsua_acc_config_default(&cfg);
        cfg.id = pj_str("sip:autoanswer@192.168.0.27:5062");
        cfg.register_on_acc_add = PJ_FALSE;
        cfg.reg_uri = pj_str("");
        cfg.cred_count = 0;

        status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
        if (status != PJ_SUCCESS) error_exit("Error adding account", status);
    }

    // создаём пллер и порты для файла
    status = registers_wav_file(MUSIC_FILE);

    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(1, (THIS_FILE, "Failed to register WAV file: %s", wav_filename));
    }
    
    for (;;) 
    {
        char option[10];

        puts("Press 'h' to hangup all calls, 'q' to quit");
        if (fgets(option, sizeof(option), stdin) == NULL) 
        {
            puts("EOF while reading stdin, will quit now..");
            break;
        }

        if (option[0] == 'q')
            break;

        if (option[0] == 'h')
            pjsua_call_hangup_all();
    }

    pjsua_destroy();
}

/* Optionally registers WAV file */
pj_status_t registers_wav_file(const char *filename)
{
    PJ_LOG(3, (THIS_FILE, "BEGINIGAAA WAV file registered: %s", filename));
    pj_status_t status;
    pjsua_player_id wav_id;
    unsigned play_options = 0;

    /* If already registered, clean up first */
    if (wav_player_id != PJSUA_INVALID_ID) 
    {
        pjsua_player_destroy(wav_player_id);
        wav_player_id = PJSUA_INVALID_ID;
        wav_port = PJSUA_INVALID_ID;
    }

    // if (app_config.auto_play_hangup)
    //     play_options |= PJMEDIA_FILE_NO_LOOP;

    pj_str_t str = pj_str("file.wav");
    
    status = pjsua_player_create(&str, 0, &wav_player_id);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "Error creating WAV player: %d", status));
        return status;
    }

    wav_port = pjsua_player_get_conf_port(wav_player_id);


    // /* Проверим что у нас есть логи при достижении конца файла*/
    // pjmedia_port *port;
    // pjsua_player_get_port(wav_player_id, &port);
    // pjmedia_wav_player_set_eof_cb2(port, NULL, &on_wav_eof);
    
    PJ_LOG(3, (THIS_FILE, "WAV file registered: %s", filename));
    return PJ_SUCCESS;
}
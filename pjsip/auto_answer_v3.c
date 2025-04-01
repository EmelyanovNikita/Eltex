#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>
#include <pjmedia/tonegen.h>

#define THIS_FILE "auto_answer.c"
#define TONE_FREQ 425  // Frequency of the tone in Hz
#define SAMPLES_PER_FRAME 64
#define ON_DURATION         7000
#define OFF_DURATION        7000


/* Callback from timer when the maximum call duration has been exceeded. */
static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void error_exit(const char *title, pj_status_t status);

typedef struct
{
    pjsua_call_id call_id;
    pj_timer_entry ringing_timer;
    pj_timer_entry answering_timer;
    pjmedia_port *tone_gen;
    pjsua_conf_port_id tone_slot;
    pj_pool_t *pool;
} call_data_t;

static call_data_t call_data;
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
    
    // Stop tone if playing
    if (call_data.tone_gen) {
        if (call_data.tone_slot != PJSUA_INVALID_ID) {
            pjsua_conf_remove_port(call_data.tone_slot);
        }
        pjmedia_port_destroy(call_data.tone_gen);
        call_data.tone_gen = NULL;
        call_data.tone_slot = PJSUA_INVALID_ID;
    }
    
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
    call_data.tone_gen = NULL;
    call_data.tone_slot = PJSUA_INVALID_ID;
    
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

pj_status_t stream_to_call( pjsua_call_id call_id )
{
    pjmedia_snd_port_param param;
    pj_status_t status;
    pjsua_player_id player_id;

    pjmedia_snd_port_param_default(&param);

    status = pjsua_set_null_snd_dev();
    if (status != PJ_SUCCESS)
    {
        PJ_LOG(3,(THIS_FILE, "NOT OKAY pjsua_set_null_snd_dev: %d", status));
        return status;
    }

    PJ_LOG(3,(THIS_FILE, "AAA pjsua_player_create"));

    status = pjsua_player_create("converted_sample-15s.wav", 0, &player_id);

    if (status != PJ_SUCCESS)
    {
        PJ_LOG(3,(THIS_FILE, "NOT OKAY pjsua_player_create: %d", status));
        return status;
    }
    else
        PJ_LOG(3,(THIS_FILE, "OKAY pjsua_player_create"));

    status = pjsua_conf_connect( pjsua_player_get_conf_port(player_id), 0);

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
        
        //~ // Stop tone if playing
        //~ if (call_data.tone_gen)
        //~ {
            //~ if (call_data.tone_slot != PJSUA_INVALID_ID)
            //~ {
                //~ pjsua_conf_remove_port(call_data.tone_slot);
            //~ }
            //~ pjmedia_port_destroy(call_data.tone_gen);
            //~ call_data.tone_gen = NULL;
            //~ call_data.tone_slot = PJSUA_INVALID_ID;
        //~ }

        // Освобождение пула памяти (если создавался отдельно)
        //~ pj_pool_release(call_data.pool);
        
        // Reset call ID
        call_data.call_id = PJSUA_INVALID_ID;
        
        PJ_LOG(3,(THIS_FILE, "Call %d state=%.*s", call_id, (int)ci.state_text.slen, ci.state_text.ptr));
        return;
    }
}

static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info ci;
    //pj_pool_t *pool;
    pj_status_t status;
    pj_time_val delay;

    pjsua_call_get_info(call_id, &ci);
    
    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) 
    {
        status =  stream_to_call(call_id);

        if (status != PJ_SUCCESS)
            PJ_LOG(3,(THIS_FILE, "ERROR stream_to_call"));

        //~ // cоздаем пул памяти
        //~ call_data.pool = pjsua_pool_create("audio", 8000, 8000);

        //~ //cоздаем тон
        //~ pjmedia_port *tonegen;
        //~ pjmedia_tone_desc tone;

        //~ status = pjmedia_tonegen_create(call_data.pool, 8000, 1, SAMPLES_PER_FRAME, 16, 0, &tonegen);

        //~ if (status != PJ_SUCCESS)
            //~ return;
        
        //~ {
            //~ tone.freq1 = 425;
            //~ tone.freq2 = 0;
            //~ tone.on_msec = ON_DURATION;
            //~ tone.off_msec = OFF_DURATION;
            //~ tone.volume = 32767;

            //~ pjmedia_tonegen_play(tonegen, 1, &tone, 0);
            //~ PJ_ASSERT_RETURN(status==PJ_SUCCESS, 1);
        //~ }

        //~ //добавляем в конференцию
        //~ pjsua_conf_port_id slot;
        //~ pjsua_conf_add_port(call_data.pool, tonegen, &slot);
        
        //~ PJ_LOG(3,(THIS_FILE, "BEFORE pjsua_conf_connect"));

        //~ //подключаем к звонку
        //~ status = pjsua_conf_connect(slot, ci.conf_slot);
        //~ if (status != PJ_SUCCESS) 
        //~ { 
            //~ PJ_LOG(3,(THIS_FILE, "NOT GOOD pjsua_conf_connect"));
            //~ error_exit("Error scheduling answering timer", status);
        //~ }
        //~ PJ_LOG(3,(THIS_FILE, "AFTER pjsua_conf_connect"));
    
        
        // 7 секунд ждём
        delay.sec = 7; 
        delay.msec = 0;
        
        PJ_LOG(3,(THIS_FILE, "Before %ld sec pause(answer)", delay.sec));
        

        call_data.answering_timer.id = call_data.call_id + 1000;
        pj_timer_entry_init(&call_data.answering_timer, call_data.answering_timer.id, 
                           (void*)&call_data.call_id, &answering_timeout_callback);
        
        status = pjsua_schedule_timer(&call_data.answering_timer, &delay);
        if (status != PJ_SUCCESS) error_exit("Error scheduling answering timer", status);
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

    /* Initialize call data */
    call_data.call_id = PJSUA_INVALID_ID;
    call_data.ringing_timer.id = PJ_FALSE;
    call_data.answering_timer.id = PJ_FALSE;
    call_data.tone_gen = NULL;
    call_data.tone_slot = PJSUA_INVALID_ID;

    /* Create pjsua first! */
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
        cfg.id = pj_str("sip:autoanswer@10.25.72.123:5062");
        cfg.register_on_acc_add = PJ_FALSE;
        cfg.reg_uri = pj_str("");
        cfg.cred_count = 0;

        status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
        if (status != PJ_SUCCESS) error_exit("Error adding account", status);
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

 
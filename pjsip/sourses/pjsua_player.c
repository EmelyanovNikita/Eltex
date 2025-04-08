#include <stdio.h>
#include <stdlib.h>
#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>
#include <pjmedia/tonegen.h>

#define THIS_FILE "pjsua_player.c"
#define MUSIC_FILE "output_1.wav"
#define NUM_CALLS 30

static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void error_exit(const char *title, pj_status_t status);
pj_status_t registers_wav_file(const char *filename);
void init_calls(call_data_t *calls, int num);
pj_bool_t init_call(call_data_t *p_call, pjsua_call_id call_id);
int choose_timer_num();

typedef struct
{
    pjmedia_tone_desc            tone;
    pj_pool_t                    *pool;
    pjsua_conf_port_id           tone_conf_port_id;
    pjmedia_port                 *tone_pjmedia_port;
} player_tone_t;

typedef struct
{
    pj_str_t wav_filename;
    pjsua_player_id wav_player_id;
    pjsua_conf_port_id wav_port;
} player_wav_file_t;

typedef struct
{
    player_tone_t long_tone;
    player_tone_t KPV_tone;
    player_wav_file_t file_player;
} players_t;

typedef struct
{
    pjsua_call_id call_id;
    pj_timer_entry ring_timer;
    pj_timer_entry ans_timer;
} call_data_t;

static call_data_t call_data;

// void init_calls(call_data_t *p_calls, int num)
// {
//     if (p_calls == NULL)
//     {
//         return;
//     }

//     for (int i = 0; i < num; ++i)
//     {
//         p_calls[i].call_id = PJSUA_INVALID_ID;
//         p_calls[i].ring_timer.id = PJ_FALSE;
//         p_calls[i].ans_timer.id = PJ_FALSE;
//         p_calls[i].wav_filename = pj_str("");
//         p_calls[i].wav_player_id = PJSUA_INVALID_ID;
//         p_calls[i].wav_port = PJSUA_INVALID_ID;
//     }
// }

pj_bool_t init_call(call_data_t *p_call, pjsua_call_id call_id)
{
    pj_status_t status;
    call_data_t *p_call;

    p_call -> call_id = call_id;

    // инициализируем таймер на приём звонка
    p_call -> ring_timer.id = choose_timer_num();

    pj_timer_entry_init(&(p_call -> ring_timer), p_call -> ring_timer.id, 
    (void*)&(p_call -> call_id), &ringing_timeout_callback);

    // инициализируем таймер на разговор
    p_call -> ans_timer.id = choose_timer_num();

    pj_timer_entry_init(&(p_call -> ans_timer), p_call -> ans_timer.id, (void*) &(p_call -> call_id), 
                                                                            &answering_timeout_callback);
}

call_data_t* get_call_data_by_id(pjsua_call_id id)
{

}

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
    call_data_t *p_call;
    pj_bool_t bool_status;

    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);

    pjsua_call_get_info(call_id, &ci);
    
    bool_status = init_call(p_call, call_id);
    if (bool_status == PJ_FALSE)
    {
        PJ_LOG(3,(THIS_FILE, "Error in init_call !!"));
        return;
    }

    p_call -> call_id = call_id;
    
    PJ_LOG(3,(THIS_FILE, "Incoming call from %.*s!!", (int)ci.remote_info.slen, ci.remote_info.ptr));

    // Send ringing (180 Ringing)
    pjsua_call_answer(p_call -> call_id, 180, NULL, NULL);
    
    // 3 sec wait before answering
    delay.sec = 3; 
    delay.msec = 0;
    
    PJ_LOG(3,(THIS_FILE, "Before %ld sec pause", delay.sec));
    
    status = pjsua_schedule_timer(&(p_call -> ring_timer), &delay);
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
    call_data_t p_call;

    PJ_UNUSED_ARG(e);
    
    pjsua_call_get_info(call_id, &ci);
    
    // Check if call has ended
    if(ci.state == PJSIP_INV_STATE_DISCONNECTED || ci.state == PJSIP_INV_STATE_NULL)
    {
        /* Stop ringing timer if active */
        if ( p_call -> ringing_timer.id)
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
        
        pjsua_schedule_timer(&call_data.answering_timer, &delay);


    }
}

static void error_exit(const char *title, pj_status_t status)
{            
    /* Close wav player */

    if (wav_player_id != PJSUA_INVALID_ID) 
    {
        pjsua_player_destroy(wav_player_id);
        wav_player_id = PJSUA_INVALID_ID;
        wav_port = PJSUA_INVALID_ID;
    }

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

    /* Initialize calls */

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
        log_cfg.console_level = 5;

        pjsua_media_config_default(&media_cfg);
        
        status = pjsua_init(&cfg, &log_cfg, &media_cfg);
        if (status != PJ_SUCCESS) 
            error_exit("Error in pjsua_init()", status);
    }

     /* Init default calls */
    init_calls(calls, NUM_CALLS);

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

    // создаём пллер и порты для файла
    status = registers_wav_file(MUSIC_FILE);

    if (status != PJ_SUCCESS) 
    {
        //PJ_LOG(1, (THIS_FILE, "Failed to register WAV file: %s", wav_filename));
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

    /* Close wav player */

    if (wav_player_id != PJSUA_INVALID_ID)
    {
        pjsua_player_destroy(wav_player_id);
        wav_player_id = PJSUA_INVALID_ID;
        wav_port = PJSUA_INVALID_ID;
    }

    pjsua_destroy();
}

/* Optionally registers WAV file */
pj_status_t registers_wav_file(const char *filename)
{

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
    
    pj_str_t str = pj_str(MUSIC_FILE);
    //pj_str_t str = pj_str("output_1.wav");
    
    status = pjsua_player_create(&str, 0, &wav_player_id);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE, "Error creating WAV player: %d", status));
        return status;
    }

    wav_port = pjsua_player_get_conf_port(wav_player_id);
    
    PJ_LOG(3, (THIS_FILE, "WAV file registered: %s", filename));
    return PJ_SUCCESS;
}

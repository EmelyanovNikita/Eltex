// compile with gcc pjsua_player_wav_file.c -o auto_answer $(pkg-config --cflags --libs libpjproject)
// sipp -sn uac  -r 15 -rp 2000 10.25.72.123:5062
#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>
#include <pjmedia/tonegen.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#define THIS_FILE "pjsua_player_wav_file_test.c"
#define MUSIC_FILE "output_1.wav"
#define MAX_CALLS 30
#define NAME_SIZE 128



typedef struct 
{
    pjmedia_tone_desc     tone;
    pj_pool_t            *pool;
    pjsua_conf_port_id    tone_conf_port_id;
    pjmedia_port         *tone_pjmedia_port;
    int                   freq1;
    int                   freq2;
    int                   on_msec;
    int                   off_msec;
} player_tone_t;

typedef struct 
{
    pj_str_t            wav_filename;
    pjsua_player_id     wav_player_id;
    pjsua_conf_port_id  wav_port;
} player_wav_file_t;

typedef struct 
{
    player_tone_t           long_tone;
    player_tone_t           KPV_tone;
    player_wav_file_t       file_player;
} players_t;

typedef struct 
{
    pjsua_call_id           call_id;
    pj_timer_entry          ringing_timer;
    pj_timer_entry          answering_timer;
    char                    *target_uri;
} call_data_t;

static call_data_t      *call_data;
static players_t        players;
char                    avaible_target_uri[4][NAME_SIZE];
pj_mutex_t              *call_data_mutex;
pj_pool_t               *pool_call_data;

static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);
static void error_exit(const char *title, pj_status_t status);
pj_status_t registers_wav_file(player_wav_file_t *player);
pj_status_t registers_tones_player(player_tone_t *player);

pj_bool_t parse_local_info_to_target_name(const pj_str_t *local_info, char *buffer, size_t buf_size) 
{
    const char *start = strchr(local_info->ptr, ':');
    if (!start) return PJ_FALSE;

    start++;
    const char *end = strchr(start, '@');
    if (!end || (end - start) <= 0) return PJ_FALSE;
    
    size_t len = end - start;
    if (len >= buf_size) len = buf_size - 1;
    
    strncpy(buffer, start, len);
    buffer[len] = '\0';
    
    return PJ_SUCCESS;
}

pj_bool_t check_uri(char *target_uri)
{
    for (int i = 0; i < 4; ++i) // 4 num_avaible_uri
    {
        if (strstr(avaible_target_uri[i], target_uri)) 
        {
            return PJ_TRUE;
        } 
    }
    return PJ_FALSE;
}

// test variant заполение всех URIs 
void fill_URIs()
{
    strcpy(avaible_target_uri[0], "long_tone");
    strcpy(avaible_target_uri[1], "KPV_tone");
    strcpy(avaible_target_uri[2], "WAV_player");
    strcpy(avaible_target_uri[3], "service");
}

static call_data_t* get_call_data(pjsua_call_id call_id) 
{
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        if (call_data[i].call_id == call_id) 
        {
            return &call_data[i];
        }
    }

    return NULL;
}

static call_data_t* allocate_call_data(pjsua_call_id call_id) 
{
    pj_mutex_lock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "LOCK allocate_call_data"));

    for (int i = 0; i < MAX_CALLS; i++) 
    {
        
        if (call_data[i].call_id == PJSUA_INVALID_ID) 
        {
            call_data[i].call_id = call_id;
            call_data[i].ringing_timer.id = PJ_FALSE;
            call_data[i].answering_timer.id = PJ_FALSE;
            call_data[i].target_uri = malloc(sizeof(char) * NAME_SIZE);

            pj_mutex_unlock(call_data_mutex);
            PJ_LOG(3,(THIS_FILE, "UNLOCK allocate_call_data if (call_data[i].call_id == PJSUA_INVALID_ID)"));

            return &call_data[i];
        }
       
    }
    
    pj_mutex_unlock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "UNLOCK allocate_call_data"));

    return NULL;
}

static void free_call_data(pjsua_call_id call_id) 
{
    pj_mutex_lock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "LOCK free_call_data"));
    
    call_data_t *cd = get_call_data(call_id);

    if (cd) 
    {
        if (cd->ringing_timer.id) 
        {
            pjsua_cancel_timer(&cd->ringing_timer);
        }
        if (cd->answering_timer.id) 
        {
            pjsua_cancel_timer(&cd->answering_timer);
        }
        cd->call_id = PJSUA_INVALID_ID;
    }

    pj_mutex_unlock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "UNLOCK free_call_data"));
}

static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry) 
{
    pjsua_call_id call_id = *(pjsua_call_id*)entry->user_data;

    PJ_LOG(3,(THIS_FILE, "LOCK ringing_timeout_callback"));

    call_data_t *cd = get_call_data(call_id);
    pj_status_t status;

    if (!cd || cd->call_id == PJSUA_INVALID_ID) 
    {
        PJ_LOG(1,(THIS_FILE, "Invalid call ID in timer callback"));

        return;
    }
    
    PJ_LOG(3,(THIS_FILE, "After pause for call %d", call_id));

    status = pjsua_call_answer(call_id, 200, NULL, NULL);
    if (status != PJ_SUCCESS) 
    {
        error_exit("Error answering call", status);
    }
    PJ_LOG(3,(THIS_FILE, "UNLOCK ringing_timeout_callback"));
}

static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry) 
{
    pjsua_call_id call_id = *(pjsua_call_id*)entry->user_data;

    pj_mutex_lock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "LOCK answering_timeout_callback : %d", gettid()));
    
    call_data_t *cd = get_call_data(call_id);
    pj_status_t status;

    if (!cd || cd->call_id == PJSUA_INVALID_ID)
    {
        PJ_LOG(1,(THIS_FILE, "Invalid call ID in timer callback"));

        pj_mutex_unlock(call_data_mutex);
        PJ_LOG(3,(THIS_FILE, "UNLOCK answering_timeout_callback    if (!cd || cd->call_id == PJSUA_INVALID_ID)"));

        return;
    }
    
    PJ_LOG(3,(THIS_FILE, "After answer pause for call %d", call_id));
    
    status = pjsua_call_hangup(call_id, 0, NULL, NULL);
    if (status != PJ_SUCCESS)
    {
        pj_mutex_unlock(call_data_mutex);
        PJ_LOG(3,(THIS_FILE, "UNLOCK answering_timeout_callback  if (status != PJ_SUCCESS)"));

        error_exit("Error in pjsua_call_hangup()", status);
    }
    pj_mutex_unlock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "UNLOCK answering_timeout_callback"));
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata) 
{
    pjsua_call_info ci;
    pj_time_val delay;
    pj_status_t status;
    call_data_t *cd;
    pj_bool_t bool_status;

    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);

    cd = allocate_call_data(call_id);

    pj_mutex_lock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "LOCK on_incoming_call : %d", gettid()));

    if (!cd) 
    {
        PJ_LOG(1,(THIS_FILE, "Failed to allocate call data for call %d", call_id));
        pjsua_call_answer(call_id, 500, NULL, NULL);

        pj_mutex_unlock(call_data_mutex);
        PJ_LOG(3,(THIS_FILE, "UNLOCK on_incoming_call  if (!cd) "));

        return;
    }

    pjsua_call_get_info(call_id, &ci);

    bool_status = parse_local_info_to_target_name( &(ci.local_info), cd -> target_uri, NAME_SIZE);
    
    PJ_LOG(3,(THIS_FILE, "Incoming call from %.*s!!", (int)ci.remote_info.slen, ci.remote_info.ptr));
    PJ_LOG(3,(THIS_FILE, "CALL TO  %s!!", cd -> target_uri));

    bool_status = check_uri(cd -> target_uri);
    PJ_LOG(3,(THIS_FILE, "AAAA check_uri: %d", bool_status));
    
    if (bool_status == PJ_TRUE)
    {
        pjsua_call_answer(call_id, 180, NULL, NULL);
    }
    else 
    {
        status = pjsua_call_hangup(call_id, 0, NULL, NULL);
        if (status != PJ_SUCCESS) PJ_LOG(3,(THIS_FILE, "pjsua_call_hangup"));

        pj_mutex_unlock(call_data_mutex);
        PJ_LOG(3,(THIS_FILE, "UNLOCK on_incoming_call if (bool_status == PJ_TRUE)"));
    }
    
    
    delay.sec = 3; 
    delay.msec = 0;
    
    cd->ringing_timer.id = call_id;
    pj_timer_entry_init(&cd->ringing_timer, cd->ringing_timer.id, 
                       (void*)&cd->call_id, &ringing_timeout_callback);
    
    PJ_LOG(3,(THIS_FILE, "Before %ld sec pause for call %d", delay.sec, call_id));
    
    status = pjsua_schedule_timer(&cd->ringing_timer, &delay);
    
    pj_mutex_unlock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "UNLOCK on_incoming_call"));
    
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_schedule_timer()", status);
}

static pj_status_t connect_audio_to_call(pjsua_call_id call_id, const char* target_uri) 
{
    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);

    // Выбираем нужный плеер в зависимости от номера
    if (strstr(target_uri, "long_tone")) 
    {
        return pjsua_conf_connect(players.long_tone.tone_conf_port_id, ci.conf_slot);
    } 
    else if (strstr(target_uri, "KPV_tone")) 
    {
        return pjsua_conf_connect(players.KPV_tone.tone_conf_port_id, ci.conf_slot);
    } 
    else if (strstr(target_uri, "WAV_player"))
    {
        return pjsua_conf_connect(players.file_player.wav_port, ci.conf_slot);
    }
    else if (strstr(target_uri, "service")) // service@10.25.72.123
    {
        return pjsua_conf_connect(players.file_player.wav_port, ci.conf_slot);
    }
}



static void on_call_state(pjsua_call_id call_id, pjsip_event *e) 
{
    pjsua_call_info ci;
    PJ_UNUSED_ARG(e);
    
    pjsua_call_get_info(call_id, &ci);
    
    if(ci.state == PJSIP_INV_STATE_DISCONNECTED || ci.state == PJSIP_INV_STATE_NULL) 
    {
        free_call_data(call_id);
        PJ_LOG(3,(THIS_FILE, "Call %d state=%.*s", call_id, (int)ci.state_text.slen, ci.state_text.ptr));
    }
}

static void on_call_media_state(pjsua_call_id call_id) 
{
    pjsua_call_info ci;
    pj_time_val delay;
    
    pj_mutex_lock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "LOCK on_call_media_state"));

    call_data_t *cd = get_call_data(call_id);

    pjsua_call_get_info(call_id, &ci);
    
    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE && cd) 
    {
        pj_status_t status = connect_audio_to_call(call_id, cd->target_uri);
        if (status != PJ_SUCCESS) 
        {
            PJ_LOG(1, (THIS_FILE, "Error connecting audio to call %d: %d", call_id, status));
        }
        

        delay.sec = 7; 
        delay.msec = 0;
        
        cd->answering_timer.id = call_id + 1000;
        pj_timer_entry_init(&cd->answering_timer, 
                           cd->answering_timer.id, 
                           (void*)&cd->call_id, 
                           &answering_timeout_callback);
        
        pjsua_schedule_timer(&cd->answering_timer, &delay);
    }

    pj_mutex_unlock(call_data_mutex);
    PJ_LOG(3,(THIS_FILE, "UNLOCK on_call_media_state"));
}

static void error_exit(const char *title, pj_status_t status) 
{
    // Cleanup players
    if (players.file_player.wav_player_id != PJSUA_INVALID_ID) 
    {
        pjsua_player_destroy(players.file_player.wav_player_id);
    }
    if (players.long_tone.tone_pjmedia_port) 
    {
        pjmedia_port_destroy(players.long_tone.tone_pjmedia_port);
    }
    if (players.KPV_tone.tone_pjmedia_port) 
    {
        pjmedia_port_destroy(players.KPV_tone.tone_pjmedia_port);
    }
    
    pjsua_perror(THIS_FILE, title, status);
    pj_mutex_destroy(call_data_mutex);
    pjsua_destroy();
    exit(1);
}

int main() 
{
    pjsua_acc_id acc_id;
    pj_status_t status;

    status = pjsua_create();
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_create()", status);

    {
        pjsua_config cfg;
        pjsua_logging_config log_cfg;
        pjsua_media_config media_cfg;

        pjsua_config_default(&cfg);
        cfg.max_calls = MAX_CALLS;
        cfg.cb.on_incoming_call = &on_incoming_call;
        cfg.cb.on_call_state = &on_call_state;
        cfg.cb.on_call_media_state = &on_call_media_state;

        pjsua_logging_config_default(&log_cfg);
        log_cfg.console_level = 5;

        pjsua_media_config_default(&media_cfg);
        
        status = pjsua_init(&cfg, &log_cfg, &media_cfg);
        if (status != PJ_SUCCESS) 
            error_exit("Error in pjsua_init()", status);
    }

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

    // Инициализация плееров
    players.long_tone.freq1 = 425;
    players.long_tone.freq2 = 0;
    players.long_tone.on_msec = 1000;
    players.long_tone.off_msec = 0;
    players.long_tone.pool = pjsua_pool_create("long_tone", 1000, 1000);
    players.long_tone.tone_conf_port_id = PJSUA_INVALID_ID;
    players.long_tone.tone_pjmedia_port = NULL;
    status = registers_tones_player(&players.long_tone);

    players.KPV_tone.freq1 = 425;
    players.KPV_tone.freq2 = 0;
    players.KPV_tone.on_msec = 1000;
    players.KPV_tone.off_msec = 4000;
    players.KPV_tone.pool = pjsua_pool_create("KPV_tone", 1000, 1000);
    players.KPV_tone.tone_conf_port_id = PJSUA_INVALID_ID;
    players.KPV_tone.tone_pjmedia_port = NULL;
    status = registers_tones_player(&players.KPV_tone);

    players.file_player.wav_filename = pj_str(MUSIC_FILE);
    players.file_player.wav_player_id = PJSUA_INVALID_ID;
    players.file_player.wav_port = PJSUA_INVALID_ID;
    status = registers_wav_file(&players.file_player);

    // заполняем все возможные URIs
    fill_URIs();

    /* Initial pool holding the buffer elements */
    pool_call_data =  pjsua_pool_create("Pool_call_data", (sizeof(call_data_t) * MAX_CALLS), (sizeof(call_data_t) * MAX_CALLS));

    if (!pool_call_data) 
    {
        PJ_PERROR(1, (THIS_FILE, status,"ERROR pj_pool_create POOL"));
        error_exit("Error in pjsua_pool_create()", status);
    }

    // выдеяем pool для наашего массива с информацией о звонках
    call_data = pj_pool_alloc(pool_call_data, (sizeof(call_data_t) * MAX_CALLS));
    
    // Инициализация call_data
    for (int i = 0; i < MAX_CALLS; i++) 
    {
        call_data[i].call_id = PJSUA_INVALID_ID;
    }
    
    // создаем мьютекс 
    status = pj_mutex_create_simple(pool_call_data, NULL, &call_data_mutex);
    if (status != PJ_SUCCESS)
    {
        PJ_LOG(1, (THIS_FILE, "Error creating MUTEX (pj_mutex_create_simple): %d", status));
        error_exit("Error in pj_mutex_create_simple()", status);
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

        if (option[0] == 'q') break;
        if (option[0] == 'h') pjsua_call_hangup_all();
    }

    pj_pool_release(pool_call_data);

    pj_mutex_destroy(call_data_mutex);
    pjsua_destroy();
    return 0;
}

pj_status_t registers_wav_file(player_wav_file_t *player) 
{
    pj_status_t status;

    if (player->wav_player_id != PJSUA_INVALID_ID) 
    {
        pjsua_player_destroy(player->wav_player_id);
        player->wav_player_id = PJSUA_INVALID_ID;
        player->wav_port = PJSUA_INVALID_ID;
    }
    
    status = pjsua_player_create(&player->wav_filename, 0, &player->wav_player_id);
    if (status != PJ_SUCCESS) 
    {
        PJ_LOG(1, (THIS_FILE, "Error creating WAV player: %d", status));
        return status;
    }

    player->wav_port = pjsua_player_get_conf_port(player->wav_player_id);
    PJ_LOG(3, (THIS_FILE, "WAV file registered"));
    return PJ_SUCCESS;
}

pj_status_t registers_tones_player(player_tone_t *player) 
{
    pj_status_t status;
    char name[80];
    pj_str_t label;

    pj_ansi_snprintf(name, sizeof(name), "tone-%d,%d", player->freq1, player->freq2);
    label = pj_str(name);

    player->tone.freq1 = player->freq1;
    player->tone.freq2 = player->freq2;
    player->tone.on_msec = player->on_msec;
    player->tone.off_msec = player->off_msec;
    player->tone.volume = 0;
    player->tone.flags = 0;

    status = pjmedia_tonegen_create2(player->pool, &label, 16000, 1, 160, 16, PJMEDIA_TONEGEN_LOOP, &player->tone_pjmedia_port);
    if (status != PJ_SUCCESS) 
    {
        pjsua_perror(THIS_FILE, "Unable to create tone generator", status);
        return status;
    }

    status = pjsua_conf_add_port(player->pool, player->tone_pjmedia_port, &player->tone_conf_port_id);
    if (status != PJ_SUCCESS) 
    {
        pjsua_perror(THIS_FILE, "Unable to add tone port to conference", status);
        return status;
    }

    status = pjmedia_tonegen_play(player->tone_pjmedia_port, 1, &player->tone, 0);
    if (status != PJ_SUCCESS) 
    {
        pjsua_perror(THIS_FILE, "Unable to play tone", status);
        return status;
    }
    
    PJ_LOG(3, (THIS_FILE, "Tone generator: %s - CREATED", name));
    return PJ_SUCCESS;
}
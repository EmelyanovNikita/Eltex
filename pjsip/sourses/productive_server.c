#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>
#include <pjmedia/tonegen.h>
#include <pjsip.h>
#include <pjlib.h>
#include <pjlib-util.h>

#define THIS_FILE "auto_answer.c"
#define MUSIC_FILE "output_file.wav"  // 16-bit mono 16kHz WAV
#define MAX_CALLS 30
#define RING_DELAY 3
#define CALL_DURATION 7

typedef struct {
    pjsua_call_id call_id;
    pj_timer_entry ringing_timer;
    pj_timer_entry answering_timer;
    pj_pool_t *pool;
    pjsua_player_id wav_player;
    pjsua_conf_port_id wav_port;
    pj_bool_t in_use;
} call_data_t;

static call_data_t call_data[MAX_CALLS];
static pj_mutex_t *call_data_mutex;

static void error_exit(const char *title, pj_status_t status);

/* Получение данных вызова */
static call_data_t* get_call_data(pjsua_call_id call_id) {
    int i;
    for (i = 0; i < MAX_CALLS; ++i) {
        if (call_data[i].in_use && call_data[i].call_id == call_id) {
            return &call_data[i];
        }
    }
    return NULL;
}

/* Выделение данных для нового вызова */
static call_data_t* allocate_call_data(pjsua_call_id call_id) {
    int i;
    pj_mutex_lock(call_data_mutex);
    
    for (i = 0; i < MAX_CALLS; ++i) {
        if (!call_data[i].in_use) {
            call_data[i].in_use = PJ_TRUE;
            call_data[i].call_id = call_id;
            call_data[i].ringing_timer.id = PJ_FALSE;
            call_data[i].answering_timer.id = PJ_FALSE;
            call_data[i].wav_player = PJSUA_INVALID_ID;
            call_data[i].wav_port = PJSUA_INVALID_ID;
            
            /* Создаем пул памяти для вызова */
            call_data[i].pool = pjsua_pool_create("call", 512, 512);
            if (!call_data[i].pool) {
                pj_mutex_unlock(call_data_mutex);
                return NULL;
            }
            
            /* Создаем WAV-плеер для этого вызова */
            pj_str_t wav_file = pj_str(MUSIC_FILE);
            if (pjsua_player_create(&wav_file, 0, &call_data[i].wav_player) != PJ_SUCCESS) {
                pj_pool_release(call_data[i].pool);
                call_data[i].in_use = PJ_FALSE;
                pj_mutex_unlock(call_data_mutex);
                return NULL;
            }
            
            call_data[i].wav_port = pjsua_player_get_conf_port(call_data[i].wav_player);
            pj_mutex_unlock(call_data_mutex);
            return &call_data[i];
        }
    }
    
    pj_mutex_unlock(call_data_mutex);
    return NULL;
}

/* Освобождение данных вызова */
static void free_call_data(pjsua_call_id call_id) {
    call_data_t *cd = get_call_data(call_id);
    if (!cd) return;
    
    pj_mutex_lock(call_data_mutex);
    
    /* Отменяем таймеры */
    if (cd->ringing_timer.id) {
        pjsua_cancel_timer(&cd->ringing_timer);
        cd->ringing_timer.id = PJ_FALSE;
    }
    if (cd->answering_timer.id) {
        pjsua_cancel_timer(&cd->answering_timer);
        cd->answering_timer.id = PJ_FALSE;
    }
    
    /* Закрываем WAV-плеер */
    if (cd->wav_player != PJSUA_INVALID_ID) {
        pjsua_player_destroy(cd->wav_player);
        cd->wav_player = PJSUA_INVALID_ID;
        cd->wav_port = PJSUA_INVALID_ID;
    }
    
    /* Освобождаем пул памяти */
    if (cd->pool) {
        pj_pool_release(cd->pool);
        cd->pool = NULL;
    }
    
    cd->in_use = PJ_FALSE;
    cd->call_id = PJSUA_INVALID_ID;
    
    pj_mutex_unlock(call_data_mutex);
}

/* Таймер для автоответа */
static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, 
                                   struct pj_timer_entry *entry) {
    pjsua_call_id call_id = *(pjsua_call_id*)entry->user_data;
    call_data_t *cd = get_call_data(call_id);
    
    if (!cd) return;
    
    pjsua_call_answer(call_id, 200, NULL, NULL);
}

/* Таймер для завершения вызова */
static void answering_timeout_callback(pj_timer_heap_t *timer_heap, 
                                     struct pj_timer_entry *entry) {
    pjsua_call_id call_id = *(pjsua_call_id*)entry->user_data;
    pjsua_call_hangup(call_id, 0, NULL, NULL);
}

/* Входящий вызов */
static void on_incoming_call(pjsua_acc_id acc_id, 
                           pjsua_call_id call_id, 
                           pjsip_rx_data *rdata) {
    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);
    
    call_data_t *cd = allocate_call_data(call_id);
    if (!cd) {
        pjsua_call_answer(call_id, 500, NULL, NULL);
        return;
    }
    
    pjsua_call_answer(call_id, 180, NULL, NULL);
    
    /* Устанавливаем таймер автоответа */
    pj_time_val delay = {RING_DELAY, 0};
    cd->ringing_timer.id = call_id;
    pj_timer_entry_init(&cd->ringing_timer, call_id, 
                       (void*)&cd->call_id, &ringing_timeout_callback);
    pjsua_schedule_timer(&cd->ringing_timer, &delay);
}

/* Изменение состояния вызова */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e) {
    PJ_UNUSED_ARG(e);
    
    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);
    
    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        free_call_data(call_id);
    }
}

/* Изменение медиа-состояния */
static void on_call_media_state(pjsua_call_id call_id) {
    pjsua_call_info ci;
    pjsua_call_get_info(call_id, &ci);
    
    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
        call_data_t *cd = get_call_data(call_id);
        if (!cd) return;
        
        /* Подключаем WAV-плеер к вызову */
        pjsua_conf_connect(cd->wav_port, ci.conf_slot);
        
        /* Устанавливаем таймер завершения */
        pj_time_val delay = {CALL_DURATION, 0};
        cd->answering_timer.id = call_id + 1000;
        pj_timer_entry_init(&cd->answering_timer, call_id + 1000, 
                           (void*)&cd->call_id, &answering_timeout_callback);
        pjsua_schedule_timer(&cd->answering_timer, &delay);
    }
}

int main() {
    pj_caching_pool cp;
    pj_pool_t *temp_pool;
    pj_status_t status;
    int i;
    
    /* Инициализация PJLIB */
    pj_init();
    
    /* Инициализация кэширующего пула */
    pj_caching_pool_init(&cp, NULL, 1024);
    
    /* Инициализация данных вызовов */
    for (i = 0; i < MAX_CALLS; ++i) {
        call_data[i].call_id = PJSUA_INVALID_ID;
        call_data[i].in_use = PJ_FALSE;
    }
    
    /* Создаем мьютекс */
    temp_pool = pj_pool_create(&cp.factory, "temp", 512, 512, NULL);
    pj_mutex_create_simple(temp_pool, "call_data", &call_data_mutex);
    pj_pool_release(temp_pool);
    
    /* Инициализация PJSUA */
    status = pjsua_create();
    if (status != PJ_SUCCESS) error_exit("pjsua_create() failed", status);
    
    /* Настройка */
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
        log_cfg.console_level = 4;
        
        pjsua_media_config_default(&media_cfg);
        media_cfg.clock_rate = 16000;
        media_cfg.snd_clock_rate = 16000;
        
        status = pjsua_init(&cfg, &log_cfg, &media_cfg);
        if (status != PJ_SUCCESS) error_exit("pjsua_init() failed", status);
    }
    
    /* Транспорт */
    {
        pjsua_transport_config cfg;
        pjsua_transport_config_default(&cfg);
        cfg.port = 5062;
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &cfg, NULL);
        if (status != PJ_SUCCESS) error_exit("Transport failed", status);
    }
    
    /* Аккаунт */
    {
        pjsua_acc_config cfg;
        pjsua_acc_config_default(&cfg);
        cfg.id = pj_str("sip:autoanswer@192.168.0.27:5062");
        cfg.register_on_acc_add = PJ_FALSE;
        
        pjsua_acc_id acc_id;
        status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
        if (status != PJ_SUCCESS) error_exit("Account failed", status);
    }
    
    /* Запуск */
    status = pjsua_start();
    if (status != PJ_SUCCESS) error_exit("pjsua_start() failed", status);
    
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
    
    /* Очистка */
    pj_caching_pool_destroy(&cp);
    pjsua_destroy();
    return 0;
}

static void error_exit(const char *title, pj_status_t status) {
    pjsua_perror(THIS_FILE, title, status);
    pjsua_destroy();
    exit(1);
}

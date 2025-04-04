#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>

#define THIS_FILE "auto_answer.c"

/* Callback from timer when the maximum call duration has been exceeded. */
static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);

/* Callback from timer when the maximum ./a	call duration has been exceeded. */
static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);

/* Display error and exit application */
static void error_exit(const char *title, pj_status_t status);

typedef struct
{
	pjsua_call_id call_id;
	pj_timer_entry ringing_timer;
	pj_timer_entry answering_timer;
} call_data_t;

static call_data_t call_data;

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

	// Отправляем ringing (180 Ringing)
    pjsua_call_answer(call_data.call_id, 180, NULL, NULL);
    
    // 3 сек ждём перед ответом 
    delay.sec = 3; 
    delay.msec = 0;
    
    // выбираем номер для таймера, чтобы он не был равен 0
    call_data.ringing_timer.id = call_data.call_id != 0 ? call_data.call_id : call_data.call_id + 1;
    
    // инициализация таймера
    pj_timer_entry_init(&call_data.ringing_timer, call_data.ringing_timer.id, (void*)&call_data.call_id, &ringing_timeout_callback);
    
    PJ_LOG(3,(THIS_FILE, "Before %ld sek pause", delay.sec));
    
    status = pjsua_schedule_timer(&call_data.ringing_timer, &delay);
    
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_schedule_timer()", status);
}

/* Callback from timer when the maximum ./a	call duration has been exceeded. */
static void ringing_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
	pj_status_t status;
	pj_time_val delay;
	pjsua_call_info ci;
	
    if (call_data.call_id == PJSUA_INVALID_ID) 
    {
        PJ_LOG(1,(THIS_FILE, "Invalid call ID in timer callback"));
        return;
    }
    
    PJ_LOG(3,(THIS_FILE, "After pause"));

    // Принимаем вызов 200 OK
    status = pjsua_call_answer(call_data.call_id, 200, NULL, NULL);
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_schedule_timer()", status);    
    
    // 7 сек ждём перед завершением 
    delay.sec = 7; 
    delay.msec = 0;
    
    PJ_LOG(3,(THIS_FILE, "Before %ld sek pause(answer)", delay.sec));
    
    pj_timer_entry_init(&call_data.answering_timer, (call_data.call_id + 1), (void*)&call_data.call_id, &answering_timeout_callback);
    status = pjsua_schedule_timer(&call_data.answering_timer, &delay);
}

/* Callback from timer when the maximum ./a	call duration has been exceeded. */
static void answering_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
	pj_status_t status;
	pjsua_call_info ci;

    if (call_data.call_id == PJSUA_INVALID_ID) 
    {
        PJ_LOG(1,(THIS_FILE, "Invalid call ID in timer callback"));
        return;
    }
    
    PJ_LOG(3,(THIS_FILE, "After answer pause"));
	
    // Завершаем вызов 
    status = pjsua_call_hangup(call_data.call_id, 0, NULL, NULL);
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_call_hangup()", status);
}

/* Callback called by the library when call's state has changed */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;

    PJ_UNUSED_ARG(e);
    
    pjsua_call_get_info(call_data.call_id, &ci);
    
    // проверка на состояние звонка - не успел ли он завершится раньше, чем мы сами его завершаем
    if(ci.state == PJSIP_INV_STATE_DISCONNECTED | ci.state == PJSIP_INV_STATE_NULL)
    {
		/* Stop hangup timer, if it is active. */
		if (call_data.ringing_timer.id)
		{
			PJ_LOG(3,(THIS_FILE, "TIMER RINGING STOPED: %d", call_data.ringing_timer.id));
			
			pjsua_cancel_timer(&call_data.ringing_timer);
			call_data.ringing_timer.id = PJ_FALSE;
		}
		
		/* Stop hangup timer, if it is active. */
		if (call_data.answering_timer.id)
		{
			PJ_LOG(3,(THIS_FILE, "TIMER ANSWERING STOPED: %d", call_data.answering_timer.id));
			
			pjsua_cancel_timer(&call_data.answering_timer);
			call_data.answering_timer.id = PJ_FALSE;
		}
		
		PJ_LOG(3,(THIS_FILE, "Call %d state=%.*s", call_id, (int)ci.state_text.slen, ci.state_text.ptr));
		return;
    }
}

/* Callback called by the library when call's media state has changed */
static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info ci;

    pjsua_call_get_info(call_id, &ci);

    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
        // When media is active, connect call to sound device.
        pjsua_conf_connect(ci.conf_slot, 0);
        pjsua_conf_connect(0, ci.conf_slot);
    }
}

/* Display error and exit application */
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
    pj_timer_id_t timer_id;

    /* Create pjsua first! */
    status = pjsua_create();
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_create()", status);

    /* Init pjsua */
    {
        pjsua_config cfg;
        pjsua_logging_config log_cfg;

        pjsua_config_default(&cfg);
        cfg.cb.on_incoming_call = &on_incoming_call;
        cfg.cb.on_call_state = &on_call_state;

        // уровень логгирования
        pjsua_logging_config_default(&log_cfg);
        log_cfg.console_level = 4;

        status = pjsua_init(&cfg, &log_cfg, NULL);
        
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

    /* Initialization is done, now start pjsua */
    status = pjsua_start();
    if (status != PJ_SUCCESS) error_exit("Error starting pjsua", status);

    /* Register to SIP server by creating SIP account. */
    {
        pjsua_acc_config cfg;

        pjsua_acc_config_default(&cfg);
        cfg.id = pj_str("sip:autoanswer@192.168.0.27:5062"); // sip:autoanswer@10.25.72.123:5062 
        cfg.register_on_acc_add = PJ_FALSE; // не регистрируемся
        cfg.reg_uri = pj_str("");
        cfg.cred_count = 0;

        status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
        if (status != PJ_SUCCESS) error_exit("Error adding account", status);
    }

    /* Wait until user press "q" to quit. */
    for (;;) {
        char option[10];

        puts("Press 'h' to hangup all calls, 'q' to quit");
        if (fgets(option, sizeof(option), stdin) == NULL) {
            puts("EOF while reading stdin, will quit now.."); // pjsua_schedule_timer pjsua_schedule_timer2
            break;
        }

        if (option[0] == 'q')
            break;

        if (option[0] == 'h')
            pjsua_call_hangup_all();
    }

    /* Destroy pjsua */
    pjsua_destroy();

    return 0;
}

#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>
//#include <timer.h>
//#include <pjlib.h>

#define THIS_FILE "auto_answer.c"

/* Callback from timer when the maximum call duration has been exceeded. */
static void call_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry);

/* Display error and exit application */
static void error_exit(const char *title, pj_status_t status);

/* Callback called by the library upon receiving incoming call */
static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata)
{
	pjsua_call_info ci;
	pj_time_val delay;
	pj_status_t status;
	pj_timer_entry timer;

    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);

    pjsua_call_get_info(call_id, &ci);

    PJ_LOG(3,(THIS_FILE, "Incoming call from %.*s!!", (int)ci.remote_info.slen, ci.remote_info.ptr));

	// Отправляем ringing (180 Ringing)
    pjsua_call_answer(call_id, 180, NULL, NULL);
    
    /* Stop hangup timer, if it is active. */
    if (timer.id)
    {
        pjsua_cancel_timer(&timer);
        timer.id = PJ_FALSE;
    }
    
    // 3 сек ждём перед ответом 
    delay.sec = 3; 
    delay.msec = 0;
    
    pj_timer_entry_init(&timer, call_id, (void*)&call_id, &call_timeout_callback); //(void*)&call_id
    
    PJ_LOG(3,(THIS_FILE, "Before %ld sek pause", delay.sec));
    
    status = pjsua_schedule_timer(&timer, &delay);
    
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_schedule_timer()", status);
}

/* Callback from timer when the maximum ./a	call duration has been exceeded. */
static void call_timeout_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
    pjsua_call_id call_id = (*(pjsua_call_id *)entry->user_data);
	//pjsua_call_id call_id = entry->id;
	pj_status_t status;

    if (call_id == PJSUA_INVALID_ID) 
    {
        PJ_LOG(1,(THIS_FILE, "Invalid call ID in timer callback"));
        return;
    }
    
    PJ_LOG(3,(THIS_FILE, "After pause"));
    
    // Принимаем вызов 200 OK
    status = pjsua_call_answer(call_id, 200, NULL, NULL);
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_schedule_timer()", status);
}

/* Callback called by the library when call's state has changed */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;

    PJ_UNUSED_ARG(e);

    pjsua_call_get_info(call_id, &ci);
    PJ_LOG(3,(THIS_FILE, "Call %d state=%.*s", call_id, (int)ci.state_text.slen, ci.state_text.ptr));
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
        cfg.id = pj_str("sip:autoanswer@10.25.72.123"); // sip:autoanswer@10.25.72.123:5062 
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

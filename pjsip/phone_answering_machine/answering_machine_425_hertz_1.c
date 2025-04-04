

#include <pjsua-lib/pjsua.h>
#include <pjmedia.h>

#define THIS_FILE       "APP"

#define SIP_DOMAIN      "sip.linphone.org"
#define SIP_USER        "nikitaemelyanov"
#define SIP_PASSWD      "En32483248"


/* Callback called by the library upon receiving incoming call */
static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata)
{
    pjsua_call_info ci;

    PJ_UNUSED_ARG(acc_id);
    PJ_UNUSED_ARG(rdata);

    pjsua_call_get_info(call_id, &ci);

    PJ_LOG(3,(THIS_FILE, "Incoming call from %.*s!!",
                         (int)ci.remote_info.slen,
                         ci.remote_info.ptr));


	 // Отправляем ringing (180 Ringing)
    pjsua_call_answer(call_id, 180, NULL, NULL);

    // Ждем 3 секунды
    //pj_thread_sleep(RING_DURATION); // ???
    
    
    // После отправляем уже 200 OK
    pjsua_call_answer(call_id, 200, NULL, NULL);
    
    //~ // Воспроизводим тон 425 Гц
    //~ pjmedia_tone_desc tone;
    //~ tone.freq1 = 425; // Частота 425 Гц
    //~ tone.freq2 = 0;   // Нет второй частоты
    //~ tone.on_msec = 1000; // Длительность сигнала (1 секунда)
    //~ tone.off_msec = 0;   // Без паузы
    //~ tone.volume = 0;     // Громкость (0 - максимальная)
    
	//~ tones[0].freq1 = 200;
	//~ tones[0].freq2 = 0;
	//~ tones[0].on_msec = ON_DURATION;
	//~ tones[0].off_msec = OFF_DURATION;
	//~ tones[0].volume = PJMEDIA_TONEGEN_VOLUME;

    //~ pjmedia_tone_dial dial;
    //~ dial.count = 1;
    //~ dial.tone = &tone;
	//~ dial.loop = PJ_TRUE; 

    //~ pjsua_conf_port_id tone_port;
    //~ pj_status_t status = pjsua_dial_dtmf(&dial, &tone_port);
    
    //~ if (status == PJ_SUCCESS) 
    //~ {
        //~ // Подключаем тон к звонку
        //~ pjsua_conf_connect(tone_port, ci.conf_slot);
    //~ }
    
    //~ if (status == PJ_SUCCESS) // ???
    //~ {
		//~ pjsua_conf_connect(tone_port, ci.conf_slot);

		//~ // Ждем 10 секунд
		//~ pj_thread_sleep(10000); // ???

		//~ // Останавливаем воспроизведение
		//~ pjsua_conf_disconnect(tone_port, ci.conf_slot);
	//~ }
}

/* Callback called by the library when call's state has changed */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;

    PJ_UNUSED_ARG(e);

    pjsua_call_get_info(call_id, &ci);
    PJ_LOG(3,(THIS_FILE, "Call %d state=%.*s", call_id,
                         (int)ci.state_text.slen,
                         ci.state_text.ptr));
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
        if (status != PJ_SUCCESS) error_exit("Error in pjsua_init()", status);
    }

    /* Add UDP transport. */
    {
        pjsua_transport_config cfg;

        pjsua_transport_config_default(&cfg);
        cfg.port = 5061;
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
        cfg.id = pj_str("sip:" SIP_USER "@" SIP_DOMAIN);
        cfg.register_on_acc_add = PJ_FALSE; // не регистрируемся
        //cfg.reg_uri = pj_str("sip:" SIP_DOMAIN);
        cfg.cred_count = 1;
        cfg.cred_info[0].realm = pj_str(SIP_DOMAIN);
        cfg.cred_info[0].scheme = pj_str("digest");
        cfg.cred_info[0].username = pj_str(SIP_USER);
        cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        cfg.cred_info[0].data = pj_str(SIP_PASSWD);

        status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
        if (status != PJ_SUCCESS) error_exit("Error adding account", status);
    }

    /* Wait until user press "q" to quit. */
    for (;;) {
        char option[10];

        puts("Press 'h' to hangup all calls, 'q' to quit");
        if (fgets(option, sizeof(option), stdin) == NULL) {
            puts("EOF while reading stdin, will quit now..");
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

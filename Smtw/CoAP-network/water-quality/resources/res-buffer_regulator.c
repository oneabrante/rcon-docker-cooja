#include <stdlib.h>
#include <string.h>
#include "contiki.h"
#include "coap-engine.h"
#include "os/dev/leds.h"

#include "sys/log.h"

/* Log configuration */
#define LOG_MODULE "buffer-regulator"
#define LOG_LEVEL LOG_LEVEL_APP

static void buffer_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset);

RESOURCE(res_buffer_regulator,
         "title=\"Pool Buffer Regulator\";rt=\"Control\"",
         NULL,
         NULL,
         buffer_put_handler,
         NULL);

//sets the ON/OFF buffer regulator
bool buffer_release = false;

//tracks if the buffer regulator is in manual mode
bool manual = false;

//change the status of the buffer regulator based on a CoAP request from the collector
static void buffer_put_handler(coap_message_t *request, coap_message_t *response, uint8_t *buffer, uint16_t preferred_size, int32_t *offset) {
	size_t len = 0;
	const char *text = NULL;
	char status[4];
	memset(status, 0, 3);

    bool response_status = true;

	len = coap_get_post_variable(request, "status", &text);
	if(len > 0 && len < 4) {
		memcpy(status, text, len);
		if(strncmp(status, "ON", len) == 0) {
			buffer_release = true;
			LOG_INFO("Buffer regulator ON\n");
		} else if(strncmp(status, "OFF", len) == 0) {
			buffer_release = false;
			LOG_INFO("Buffer regulator OFF\n");
		} else {
			response_status = false;
		}
	} else {
		response_status = false;
	}
	
	if(!response_status) {
    		coap_set_status_code(response, BAD_REQUEST_4_00);
 	}
}

//let the actuator resource handle the manual mode
void manual_handler() {
    manual = !manual;
    buffer_release = !buffer_release;

    if (buffer_release) {
        LOG_INFO("Buffer regulator is ON\n");
    } else
        LOG_INFO("Buffer regulator is OFF\n");
}

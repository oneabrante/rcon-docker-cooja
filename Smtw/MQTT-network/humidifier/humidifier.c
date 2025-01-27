#include "contiki.h"
#include "net/routing/routing.h"
#include "mqtt.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "lib/sensors.h"
#include "os/dev/button-hal.h"
#include "os/dev/leds.h"
#include "os/sys/log.h"
#include "mqtt-client.h"
#include "random.h"

#include <string.h>
#include <strings.h>

#include <sys/node-id.h>

#define LOG_MODULE "humidifier"
#ifdef  MQTT_CLIENT_CONF_LOG_LEVEL
#define LOG_LEVEL MQTT_CLIENT_CONF_LOG_LEVEL
#else
#define LOG_LEVEL LOG_LEVEL_DBG
#endif

/* MQTT broker address. */
#define MQTT_CLIENT_BROKER_IP_ADDR "fd00::1"

static const char *broker_ip = MQTT_CLIENT_BROKER_IP_ADDR;

// Default config values
#define DEFAULT_BROKER_PORT         1883
#define DEFAULT_PUBLISH_INTERVAL    (30 * CLOCK_SECOND)
#define PUBLISH_INTERVAL	        (5 * CLOCK_SECOND)

// We assume that the broker does not require authentication

/* Various states */
static uint8_t state;

#define STATE_INIT    		    0	// initial state
#define STATE_NET_OK    	    1	// Network is initialized
#define STATE_CONNECTING      	2	// Connecting to MQTT broker
#define STATE_CONNECTED       	3	// Connection successful
#define STATE_SUBSCRIBED      	4	// Topics subscription done
#define STATE_DISCONNECTED    	5	// Disconnected from MQTT broker

PROCESS_NAME(humidity_control_process);
PROCESS_NAME(blinking_led);
AUTOSTART_PROCESSES(&humidity_control_process, &blinking_led);

/* Maximum TCP segment size for outgoing segments of our socket */
#define MAX_TCP_SEGMENT_SIZE    32
#define CONFIG_IP_ADDR_STR_LEN  64

/*
 * Buffers for Client ID and Topics.
 */
#define BUFFER_SIZE 64

static char client_id[BUFFER_SIZE];
static char pub_topic[BUFFER_SIZE];
static char sub_topic[BUFFER_SIZE];

// Periodic timer to check the state of the MQTT client
#define STATE_MACHINE_PERIODIC (CLOCK_SECOND >> 1)
static struct etimer periodic_timer;

/*
 * The main MQTT buffers.
 * We will need to increase if we start publishing more data.
 */
#define APP_BUFFER_SIZE 512
static char app_buffer[APP_BUFFER_SIZE];

static struct mqtt_message *msg_ptr = 0;

static struct mqtt_connection conn;

PROCESS(humidity_control_process, "Humidity control process");

//sets the humidifier ON/OFF
static bool humidifier_on = false;

//humidity level sensed by the humidity sensor
static int humidity_level = 80;


//tracks the manual mode for the actuator
static bool manual = false;

static mqtt_status_t status;
static char broker_address[CONFIG_IP_ADDR_STR_LEN];

int random_in_range(int a, int b) {
    int v = random_rand() % (b-a);
    return v + a;
}

// Incoming message handling
static void pub_handler(const char *topic, uint16_t topic_len, const uint8_t *chunk, uint16_t chunk_len) {
}

// MQTT event handler
static void mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data) {
	switch(event) {
		case MQTT_EVENT_CONNECTED: {
			LOG_INFO("Application has a MQTT connection\n");
			state = STATE_CONNECTED;
			break;
		}
		case MQTT_EVENT_DISCONNECTED: {
			msg_ptr = data;
			pub_handler(msg_ptr->topic, strlen(msg_ptr->topic), msg_ptr->payload_chunk, msg_ptr->payload_length);
			
			#if MQTT_311
                mqtt_suback_event_t *suback_event = (mqtt_suback_event_t *)data;
                if(suback_event->success){
                    LOG_INFO("Application has subscribed to topic successfully\n");
                } 
                else {
                    LOG_ERR("Application failed to subscribe to topic (ret code %x)\n", suback_event->return_code);
                }
			#else
			    LOG_INFO("Application has subscribed to the topic\n");
			#endif
			state = STATE_DISCONNECTED;
			break;

		}
		case MQTT_EVENT_UNSUBACK: {
			LOG_INFO("Application is unsubscribed to topic successfully\n");
			break;
		}
		case MQTT_EVENT_PUBACK: {
			LOG_INFO("Publishing complete.\n");
			break;
		}
		default:
			LOG_INFO("Application got a unhandled MQTT event: %i\n", event);
	}
}

static bool have_connectivity(void) {
	if(uip_ds6_get_global(ADDR_PREFERRED) == NULL || uip_ds6_defrt_choose() == NULL) {
		return false;
	}
	return true;
}

// let the actuator resource handle the manual mode
static void manual_handler(){
    manual = !manual;
	humidifier_on = !humidifier_on;
	if (humidifier_on) {
	    LOG_INFO("[MANUAL] Humidifier is ON\n");
	} else {
	    LOG_INFO("[MANUAL] Humidifier is OFF\n");
	}
}


static void simulate_humidity_level(){
	if(humidifier_on && humidity_level < 98) {

		humidity_level = 91;

	} else {
		if(humidity_level >= 5){
		
			humidity_level = 91;
		}
	}

	LOG_INFO("New humidity value: %d\n", humidity_level);
}


PROCESS_THREAD(humidity_control_process, ev, data) {

	PROCESS_BEGIN();

	// Initialize the ClientID as MAC address
	sprintf(client_id,"%02x%02x%02x%02x%02x%02x",
		     linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
		     linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[5],
		     linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);
	sprintf(pub_topic, "%s", "humidity");

	// Change the client ID for SmartWellnessCollector:
	sprintf(client_id, "%s", "SmartWellnessCollector");

	// Broker registration					 
	mqtt_register(&conn, &humidity_control_process, client_id, mqtt_event, MAX_TCP_SEGMENT_SIZE);

	static char sensorType[20] = "humiditySensor";
			
	state=STATE_INIT;
				    
	// Initialize periodic timer to check the status 
	etimer_set(&periodic_timer, PUBLISH_INTERVAL);

	while(1) {
		PROCESS_YIELD();

		if((ev == PROCESS_EVENT_TIMER && data == &periodic_timer) || ev == PROCESS_EVENT_POLL || ev == button_hal_press_event) {			  			  
			if(state==STATE_INIT) {
				if(have_connectivity()==true) { 
					state = STATE_NET_OK;
				}
			}   
			if(state == STATE_NET_OK) {
			  	memcpy(broker_address, broker_ip, strlen(broker_ip));
			  
			  	mqtt_connect(&conn, broker_address, DEFAULT_BROKER_PORT,
						   (DEFAULT_PUBLISH_INTERVAL * 3) / CLOCK_SECOND,
						   MQTT_CLEAN_SESSION_ON);
			  	state = STATE_CONNECTING;
                LOG_INFO("Connected to MQTT server\n");
				LOG_INFO("Broker address: %s\n", broker_address);
				LOG_INFO("Broker port: %d\n", DEFAULT_BROKER_PORT);
				LOG_INFO("Client ID: %s\n", client_id);
				LOG_INFO("Topic: %s\n", pub_topic);
				// topic subscription
				strcpy(sub_topic,"humidity_control");
				status = mqtt_subscribe(&conn, NULL, sub_topic, MQTT_QOS_LEVEL_0);
				if(status == MQTT_STATUS_OUT_QUEUE_FULL){
					LOG_ERR("Tried to subscribe but command queue was full!\n");
					
				}
				state = STATE_SUBSCRIBED; 
			    // simulate sensed values and publishing of the information for the collector

				sprintf(pub_topic, "%s", "humidity");

				simulate_humidity_level();

				if(ev == button_hal_press_event){
					manual_handler();
				}

                snprintf(app_buffer, APP_BUFFER_SIZE, 
                            "{\"node\": %d, \"value\": %d, \"manual\": %d, \"sensorType\": \"%s\"}",
                            (unsigned int) node_id, (int)humidity_level, (int)manual, sensorType);

				LOG_INFO("message: %s\n", app_buffer);

				mqtt_publish(&conn, NULL, pub_topic, (uint8_t *)app_buffer, strlen(app_buffer), 
                                MQTT_QOS_LEVEL_0, MQTT_RETAIN_OFF);

			} else if(state == STATE_CONNECTED) {
				simulate_humidity_level();
			} else if(state == STATE_DISCONNECTED) {
				mqtt_disconnect(&conn);
				state = STATE_INIT;
			}

			etimer_set(&periodic_timer, PUBLISH_INTERVAL);
		}
	}
	PROCESS_END();
}

PROCESS(blinking_led, "Led blinking process");

static struct etimer registration_led_timer;
static struct etimer humidifier_led_timer;
static struct etimer led_on_timer;

PROCESS_THREAD(blinking_led, ev, data)
{
	PROCESS_BEGIN();

	etimer_set(&registration_led_timer, 1*CLOCK_SECOND);

	leds_set(LEDS_NUM_TO_MASK(LEDS_YELLOW));

	//yellow led blinking until the connection to the border router and the collector is not complete
	while(state != STATE_SUBSCRIBED){
		PROCESS_YIELD();
		if (ev == PROCESS_EVENT_TIMER){
			if(etimer_expired(&registration_led_timer)){
				leds_toggle(LEDS_NUM_TO_MASK(LEDS_YELLOW));
				etimer_restart(&registration_led_timer);
			}
		}
	}

	etimer_set(&humidifier_led_timer, 7*CLOCK_SECOND);
	etimer_set(&led_on_timer, 1*CLOCK_SECOND);

	// if the humidifier is ON, green and yellow leds are blinking
	// if the humidifier is OFF, only green led is blinking
	while(1){
		PROCESS_YIELD();
		if (ev == PROCESS_EVENT_TIMER){
			if(etimer_expired(&humidifier_led_timer)){
				if(humidifier_on){
					leds_on(LEDS_NUM_TO_MASK(LEDS_YELLOW));
				}
				leds_on(LEDS_NUM_TO_MASK(LEDS_GREEN));
				etimer_restart(&humidifier_led_timer);
				etimer_restart(&led_on_timer);
			}
			if(etimer_expired(&led_on_timer)){
				leds_off(LEDS_NUM_TO_MASK(LEDS_YELLOW));
				leds_off(LEDS_NUM_TO_MASK(LEDS_GREEN));
			}
		}
	}
	PROCESS_END();
}
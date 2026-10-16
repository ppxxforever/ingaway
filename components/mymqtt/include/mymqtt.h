#ifndef INGAWAY_MYMQTT_H
#define INGAWAY_MYMQTT_H

/* Call on GOT_IP; starts one worker and marks the network available. */
void mqtt_start(void);
/* Nonblocking; worker stops the old MQTT session and waits for GOT_IP. */
void mqtt_network_down(void);

#endif

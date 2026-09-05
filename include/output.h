#ifndef OUTPUT_H
#define OUTPUT_H

#include "types.h"

/* TCP server functions */
int tcp_init(struct output_state *s, int port);
void tcp_accept_clients(struct output_state *s);
void tcp_remove_client(struct output_state *s, int client_slot);
void tcp_broadcast(struct output_state *s, int16_t *samples, int count);
void tcp_close(struct output_state *s);

/* UDP client functions */
int udp_init(struct output_state *s, const char *host, int port);
int udp_write(struct output_state *s, int16_t *samples, int count);
void udp_close(struct output_state *s);

#endif /* OUTPUT_H */

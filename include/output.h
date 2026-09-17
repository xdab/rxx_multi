#ifndef OUTPUT_H
#define OUTPUT_H

/**
 * @file output.h
 * @brief Audio sinks: multi-client TCP broadcast and fire-and-forget UDP.
 */

#include "types.h"

/* TCP server functions */

/**
 * @brief Create the listening socket, bind and listen (SO_REUSEADDR on).
 *
 * @param[in,out] s Output state; the TCP sub-struct is initialized.
 * @param[in] port Port to listen on, INADDR_ANY.
 *
 * @retval 0 Success.
 * @retval -1 socket/bind/listen failure; stderr gets the reason.
 */
int tcp_init(struct output_state *s, int port);

/**
 * @brief Non-blocking accept of pending connections into free slots.
 *
 * Capped at MAX_TCP_CLIENTS; accepted sockets get TCP_NODELAY. Returns
 * immediately when nothing is pending or the client table is full.
 *
 * @param[in,out] s Output state.
 */
void tcp_accept_clients(struct output_state *s);

/**
 * @brief Close a client socket and release its slot.
 *
 * @param[in,out] s Output state.
 * @param[in] client_slot Slot index; no-op when the slot is already empty.
 */
void tcp_remove_client(struct output_state *s, int client_slot);

/**
 * @brief Accept pending connections, then send PCM16 to every client.
 *
 * Drops clients on send error or partial send (slow consumer); the
 * disconnect is logged and the slot freed in place.
 *
 * @param[in,out] s Output state.
 * @param[in] samples PCM16 buffer to send.
 * @param[in] count Number of samples (2 bytes each).
 */
void tcp_broadcast(struct output_state *s, int16_t *samples, int count);

/**
 * @brief Close all client sockets and the listener; destroys the client mutex.
 */
void tcp_close(struct output_state *s);

/* UDP client functions */

/**
 * @brief Create the UDP socket and set the numeric IPv4 destination.
 *
 * @param[in,out] s Output state.
 * @param[in] host Destination IPv4 address string (no DNS resolution).
 * @param[in] port Destination port.
 *
 * @retval 0 Success.
 * @retval -1 Socket creation or address parse failure.
 */
int udp_init(struct output_state *s, const char *host, int port);

/**
 * @brief Fire-and-forget send of PCM16 to the configured destination.
 *
 * Transient failures (EINTR/EAGAIN) are reported as errors and simply
 * retried with the next chunk; loss is accepted by design.
 *
 * @param[in] s Output state.
 * @param[in] samples PCM16 buffer to send.
 * @param[in] count Number of samples (2 bytes each).
 *
 * @retval 0 Success.
 * @retval -1 Send failure, socket not open, or transient block.
 */
int udp_write(struct output_state *s, int16_t *samples, int count);

/**
 * @brief Close the UDP socket.
 */
void udp_close(struct output_state *s);

#endif /* OUTPUT_H */

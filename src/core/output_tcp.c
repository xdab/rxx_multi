#include "types.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int tcp_init(struct output_state *s, int port)
{
    int reuse = 1;

    /* Initialize client array */
    for (int i = 0; i < MAX_TCP_CLIENTS; i++)
        s->net.tcp.client_fd[i] = -1;
    s->net.tcp.client_count = 0;
    pthread_mutex_init(&s->net.tcp.clients_m, NULL);

    /* Create listening socket */
    s->net.tcp.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->net.tcp.listen_fd < 0)
    {
        fprintf(stderr, "TCP: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Allow address reuse for quick restarts */
    if (setsockopt(s->net.tcp.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        fprintf(stderr, "TCP: setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
        close(s->net.tcp.listen_fd);
        return -1;
    }

    /* Bind to port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(s->net.tcp.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "TCP: bind() failed on port %d: %s\n", port, strerror(errno));
        close(s->net.tcp.listen_fd);
        return -1;
    }

    /* Listen for connections */
    if (listen(s->net.tcp.listen_fd, 5) < 0)
    {
        fprintf(stderr, "TCP: listen() failed: %s\n", strerror(errno));
        close(s->net.tcp.listen_fd);
        return -1;
    }

    fprintf(stderr, "TCP: listening on port %d (max %d clients)\n", port, MAX_TCP_CLIENTS);
    return 0;
}

void tcp_accept_clients(struct output_state *s)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    /* Accept all pending connections */
    while (1)
    {
        /* Check if we have room for more clients */
        int available_slot = -1;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++)
        {
            if (s->net.tcp.client_fd[i] < 0)
            {
                available_slot = i;
                break;
            }
        }

        if (available_slot < 0)
        {
            /* No room for more clients */
            return;
        }

        /* Non-blocking accept */
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0; /* Non-blocking */

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s->net.tcp.listen_fd, &rfds);

        int ret = select(s->net.tcp.listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0)
            return; /* No connection pending or error */

        /* Accept the connection */
        int fd = accept(s->net.tcp.listen_fd, (struct sockaddr *)&client_addr, &client_len);

        if (fd < 0)
        {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
                fprintf(stderr, "TCP: accept() failed: %s\n", strerror(errno));
            return;
        }

        /* Disable Nagle (send immediately for real-time audio) */
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        /* Store client socket */
        pthread_mutex_lock(&s->net.tcp.clients_m);
        s->net.tcp.client_fd[available_slot] = fd;
        s->net.tcp.client_count++;
        pthread_mutex_unlock(&s->net.tcp.clients_m);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        fprintf(stderr, "TCP: client [%d] connected from %s:%d (total: %d)\n",
                available_slot, client_ip, ntohs(client_addr.sin_port),
                s->net.tcp.client_count);
    }
}

void tcp_remove_client(struct output_state *s, int client_slot)
{
    pthread_mutex_lock(&s->net.tcp.clients_m);

    if (s->net.tcp.client_fd[client_slot] >= 0)
    {
        close(s->net.tcp.client_fd[client_slot]);
        s->net.tcp.client_fd[client_slot] = -1;
        s->net.tcp.client_count--;
        fprintf(stderr, "TCP: client [%d] disconnected (total: %d)\n",
                client_slot, s->net.tcp.client_count);
    }

    pthread_mutex_unlock(&s->net.tcp.clients_m);
}

void tcp_broadcast(struct output_state *s, int16_t *samples, int count)
{
    /* First, accept any new connections */
    tcp_accept_clients(s);

    if (s->net.tcp.client_count == 0)
    {
        return; /* No clients to send to */
    }

    size_t data_len = count * 2; /* 16-bit samples */

    pthread_mutex_lock(&s->net.tcp.clients_m);

    /* Send to all connected clients */
    for (int i = 0; i < MAX_TCP_CLIENTS; i++)
    {
        int fd = s->net.tcp.client_fd[i];
        if (fd < 0)
            continue; /* Empty slot */

        /* Send data to this client (MSG_NOSIGNAL prevents SIGPIPE on disconnect) */
        ssize_t sent = send(fd, samples, data_len, MSG_NOSIGNAL);

        if (sent < 0)
        {
            if (errno == EPIPE || errno == ECONNRESET)
            {
                /* Client disconnected, mark for removal */
                close(fd);
                s->net.tcp.client_fd[i] = -1;
                s->net.tcp.client_count--;
                fprintf(stderr, "TCP: client [%d] lost (total: %d)\n",
                        i, s->net.tcp.client_count);
            }
            /* EAGAIN/EWOULDBLOCK: try again next buffer */
            /* EINTR: try again next buffer */
        }
        else if ((size_t)sent < data_len)
        {
            /* Partial send - client buffer full, disconnect */
            close(fd);
            s->net.tcp.client_fd[i] = -1;
            s->net.tcp.client_count--;
            fprintf(stderr, "TCP: client [%d] buffer overflow (total: %d)\n",
                    i, s->net.tcp.client_count);
        }
    }

    pthread_mutex_unlock(&s->net.tcp.clients_m);
}

void tcp_close(struct output_state *s)
{
    pthread_mutex_lock(&s->net.tcp.clients_m);

    /* Close all client sockets */
    for (int i = 0; i < MAX_TCP_CLIENTS; i++)
    {
        if (s->net.tcp.client_fd[i] >= 0)
        {
            close(s->net.tcp.client_fd[i]);
            s->net.tcp.client_fd[i] = -1;
        }
    }
    s->net.tcp.client_count = 0;

    pthread_mutex_unlock(&s->net.tcp.clients_m);

    /* Close listening socket */
    if (s->net.tcp.listen_fd >= 0)
    {
        close(s->net.tcp.listen_fd);
        s->net.tcp.listen_fd = -1;
    }

    pthread_mutex_destroy(&s->net.tcp.clients_m);
}

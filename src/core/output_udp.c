#include "types.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int udp_init(struct output_state *s, const char *host, int port)
{
    /* Create UDP socket */
    s->net.udp.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->net.udp.sock < 0)
    {
        fprintf(stderr, "UDP: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /* Set destination address */
    memset(&s->net.udp.dest, 0, sizeof(s->net.udp.dest));
    s->net.udp.dest.sin_family = AF_INET;
    s->net.udp.dest.sin_port = htons((uint16_t)port);

    /* Convert host to IP address */
    if (inet_pton(AF_INET, host, &s->net.udp.dest.sin_addr) <= 0)
    {
        fprintf(stderr, "UDP: invalid address '%s'\n", host);
        close(s->net.udp.sock);
        s->net.udp.sock = -1;
        return -1;
    }

    fprintf(stderr, "UDP: will send to %s:%d\n", host, port);
    return 0;
}

int udp_write(struct output_state *s, int16_t *samples, int count)
{
    if (s->net.udp.sock < 0)
        return -1;

    /* Send raw PCM data (16-bit samples, MSG_NOSIGNAL prevents SIGPIPE) */
    ssize_t sent = sendto(s->net.udp.sock, samples, count * 2, MSG_NOSIGNAL,
                          (struct sockaddr *)&s->net.udp.dest,
                          sizeof(s->net.udp.dest));

    if (sent < 0)
    {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return -1; /* Try again next time */
        fprintf(stderr, "UDP: sendto() failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

void udp_close(struct output_state *s)
{
    if (s->net.udp.sock >= 0)
    {
        close(s->net.udp.sock);
        s->net.udp.sock = -1;
    }
}

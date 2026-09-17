#ifndef TYPES_H
#define TYPES_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <complex.h>
#include <liquid/liquid.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>

/* Constants */
#define WBFM_SAMPLE_RATE 170000
#define DEFAULT_SAMPLE_RATE 12000
#define DEFAULT_OUTPUT_RATE 48000
#define DEFAULT_BUF_LENGTH (1 * 16384)
#define MAXIMUM_OVERSAMPLE 16
#define MAXIMUM_BUF_LENGTH (MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#define MAXIMUM_IQ_LENGTH (MAXIMUM_BUF_LENGTH / 2)
#define AUTO_GAIN -100
#define FREQUENCIES_LIMIT 128
#define MAX_TCP_CLIENTS 32
#define STATIC_STRING_SIZE 256

extern volatile int do_exit;

struct iq_buffer;
struct real_buffer;
struct channel_pipeline;
struct output_state;

typedef void (*demodulate_fn)(
    struct channel_pipeline *pipeline, const struct iq_buffer *input, struct real_buffer *output
);

struct iq_buffer
{
    float complex samples[MAXIMUM_IQ_LENGTH];
    int len;
};

struct real_buffer
{
    float samples[MAXIMUM_BUF_LENGTH];
    int len;
};

struct channel_pipeline
{
    int input_rate;
    int demod_rate;
    int output_rate;
    int downsample_factor;
    float output_scale;
    int deemph_enabled;
    float deemph_alpha;
    int dc_block_enabled;
    resamp_rrrf audio_resampler;
    /* Hand-rolled linear-buffer FIR decimator: decimator_tail always
     * holds decim_taps_len-1 history samples (zero-filled at creation,
     * slid forward by in_len after every chunk). Sized for M <= 256
     * with the m=4 prototype (2*4*256). */
    float *decim_taps;           /* kaiser prototype, decim_taps_len taps */
    unsigned int decim_taps_len; /* filter length 2*m*M + 1 */
    /* AVX2 decimation kernels only: the same taps broadcast to lane
     * pairs, 8 floats per 4 taps (h0,h0,h1,h1,h2,h2,h3,h3). NULL on
     * non-AVX2 builds or if its setup allocation fails, in which case
     * the scalar dot runs instead. */
    float *decim_taps_pairs;
    iirfilt_rrrf deemph_filter;
    iirfilt_rrrf dc_block_filter;
    /* Channel-shift oscillator: 32-bit DDS phase accumulator; the
     * unsigned wraparound is the 2pi wrap. shift_step = phase increment
     * per sample in 1/2^32 cycles, derived once from frequency_offset */
    uint32_t shift_step;
    uint32_t shift_acc;
    uint32_t target_frequency;
    double frequency_offset;
    int frequency_shift_enabled;
    float complex prev_sample;
    /* Working buffer for the in-place stages (NCO shift, decimation):
     * private per pipeline, so the shared device slot is only ever
     * read. Allocated once as part of the static demods[] array. */
    struct iq_buffer work;
    float complex decimator_tail[2048];
    unsigned int decimator_tail_len;
    /* Decimator carry: unconsumed input samples mod M (stream position
     * mod M), so outputs land at global multiples of M. Always < M. */
    unsigned int decim_rem;
    demodulate_fn demodulate;
};

/* Device state - capture parameters plus the union of all backend gain
 * controls. Each backend uses the fields it knows and ignores the rest;
 * the opaque dev handle is cast to the backend's own type internally. */
struct device_state
{
    pthread_t thread;
    void *dev;
    int dev_index;
    uint32_t freq;
    uint32_t rate;
    /* RTL-SDR gain: tenths of a dB, AUTO_GAIN = automatic */
    int gain;
    /* RSP gain: IF gain reduction 0-59 dB + LNA state; agc = API AGC */
    int gain_rdb;
    int lna_state;
    int agc;
    /* Ping-pong chunk slots: chunk k lives in slots[k & 1], so the
     * producer fills one slot while the demods still read the other.
     * A slot may be refilled only after every demod has fully consumed
     * its previous occupant (see acquire_fill_slot in the backends). */
    struct iq_buffer slots[2];
    /* Chunks published so far; written only by the single producer
     * thread, polled by the demods (volatile, inherited seq style) */
    volatile unsigned long chunk_seq;
    /* Producer refill bar: demods broadcast here after bumping
     * seq_processed so acquire_fill_slot / wait_demods_drained (in the
     * backends) can block on the seq predicates instead of polling */
    pthread_cond_t slots_drained;
    pthread_mutex_t slots_drained_m;
    uint32_t buf_len;
    int ppm_error;                        /* RTL-SDR only */
    int direct_sampling;                  /* RTL-SDR only */
    int mute;                             /* RTL-SDR only */
    int biastee;                          /* RTL-SDR only */
    char input_path[STATIC_STRING_SIZE];  /* -I: play this file instead of hardware */
    char record_path[STATIC_STRING_SIZE]; /* -R: record capture to this file */
    FILE *record_file;                    /* open while recording; NULL = recording off */
};

/* Demod state - demodulation and signal processing */
struct demod_state
{
    pthread_t thread;
    struct real_buffer output;
    /* Lossless handoff, index-based: chunk k is always in
     * device.slots[k & 1], so a demod only tracks how many chunks it
     * has consumed. The producer bumps device.chunk_seq per published
     * chunk and signals this demod's condvar; equal counts mean the
     * demod holds no unconsumed chunk. seq_processed is bumped without
     * a lock after consumption (volatile, inherited seq style). */
    volatile unsigned long seq_processed;
    pthread_cond_t ready;
    pthread_mutex_t ready_m;
    struct channel_pipeline pipeline;
    struct output_state *output_target;
};

/* Output modes */
typedef enum
{
    OUTPUT_FILE,
    OUTPUT_TCP,
    OUTPUT_UDP
} output_mode_t;

/* TCP server state */
struct tcp_state
{
    int listen_fd;                  /* Server socket */
    int client_fd[MAX_TCP_CLIENTS]; /* Array of client sockets */
    int client_count;               /* Active client count */
    pthread_mutex_t clients_m;      /* Protect client array */
};

/* UDP client state */
struct udp_state
{
    int sock;                /* UDP socket */
    struct sockaddr_in dest; /* Destination address */
};

/* Output state - file I/O and networking */
struct output_state
{
    pthread_t thread;
    FILE *file;
    char filename[STATIC_STRING_SIZE];
    output_mode_t mode;
    union
    {
        struct tcp_state tcp;
        struct udp_state udp;
    } net;
    int16_t result[MAXIMUM_BUF_LENGTH];
    int result_len;
    int rate;
    int data_ready;
    /* Lossless handoff: demod bumps seq_packed after posting audio,
     * output bumps seq_written after writing it; equal counts mean the
     * output has drained everything packed so far */
    volatile unsigned long seq_packed;
    volatile unsigned long seq_written;
    /* Demod blocks here while the output stage drains (seq_written
     * catches up to seq_packed); broadcast after every seq_written++ */
    pthread_cond_t written;
    pthread_mutex_t written_m;
    pthread_rwlock_t rw;
    pthread_cond_t ready;
    pthread_mutex_t ready_m;
};

/* Global instances */
extern struct device_state device;
extern struct demod_state demods[FREQUENCIES_LIMIT];
extern struct output_state outputs[FREQUENCIES_LIMIT];
extern int freq_len;

#endif /* TYPES_H */

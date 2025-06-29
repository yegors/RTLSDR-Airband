#include <arpa/inet.h>
#include <srt/logging_api.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <vector>

#include "rtl_airband.h"

#ifdef WITH_SRT

static bool srt_initialized = false;
static void srt_stream_accept(srt_stream_data* sdata);
static void srt_stream_send_header(srt_stream_data* sdata, srt_client& client);

static void srt_log_dummy(void*, int, const char*, int, const char*, const char*) {}

static void srt_try_startup() {
    if (!srt_initialized) {
        if (srt_startup() != 0) {
            log(LOG_ERR, "srt_stream: srt_startup failed: %s\n", srt_getlasterror_str());
        } else {
            srt_initialized = true;
            /* Reduce noise from libsrt by disabling log output */
            srt_setloglevel(LOG_CRIT);
            srt_setloghandler(NULL, srt_log_dummy);
        }
    }
}

static void srt_stream_send(srt_stream_data* sdata, const char* data, size_t len) {
    if (sdata->listen_socket == SRT_INVALID_SOCK)
        return;

    srt_stream_accept(sdata);
    for (auto it = sdata->clients.begin(); it != sdata->clients.end();) {
        if (sdata->format == SRT_STREAM_WAV && !it->header_sent) {
            srt_stream_send_header(sdata, *it);
            it->header_sent = true;
        }

        const char* ptr = data;
        size_t remaining = len;
        while (remaining > 0) {
            int chunk = remaining > (size_t)sdata->payload_size ? sdata->payload_size : remaining;
            int ret = srt_send(it->sock, ptr, chunk);
            if (ret == SRT_ERROR) {
                int serr;
                srt_getlasterror(&serr);
                if (serr != SRT_EASYNCSND) {
                    srt_close(it->sock);
                    it = sdata->clients.erase(it);
                    goto next_client;
                }
            }
            ptr += chunk;
            remaining -= chunk;
        }
        ++it;
        continue;
    next_client:;
    }
}

bool srt_stream_init(srt_stream_data* sdata, mix_modes mode, size_t len) {
    srt_try_startup();
    if (!srt_initialized) {
        return false;
    }

    sdata->mode = mode;

    if (sdata->format != SRT_STREAM_MP3 && mode == MM_STEREO) {
        sdata->stereo_buffer_len = (len / sizeof(float)) * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    if (sdata->format == SRT_STREAM_WAV) {
        sdata->pcm_buffer_len = (len / sizeof(float)) * (mode == MM_STEREO ? 2 : 1);
        sdata->pcm_buffer = (int16_t*)XCALLOC(sdata->pcm_buffer_len, sizeof(int16_t));
    } else {
        sdata->pcm_buffer_len = 0;
        sdata->pcm_buffer = NULL;
    }

    sdata->listen_socket = srt_create_socket();
    if (sdata->listen_socket == SRT_INVALID_SOCK) {
        log(LOG_ERR, "srt_stream: socket failed: %s\n", srt_getlasterror_str());
        return false;
    }

    int len_tmp = sizeof(sdata->payload_size);
    if (srt_getsockopt(sdata->listen_socket, 0, SRTO_PAYLOADSIZE, &sdata->payload_size, &len_tmp) == SRT_ERROR) {
        sdata->payload_size = SRT_LIVE_DEF_PLSIZE;
    }

    int blocking = 0;
    srt_setsockopt(sdata->listen_socket, 0, SRTO_SNDSYN, &blocking, sizeof(blocking));
    srt_setsockopt(sdata->listen_socket, 0, SRTO_RCVSYN, &blocking, sizeof(blocking));

    /* Disable timestamp-based packet delivery for minimal latency */
    int tsbpd = 0;
    srt_setsockopt(sdata->listen_socket, 0, SRTO_TSBPDMODE, &tsbpd, sizeof(tsbpd));
    int zero = 0;
    srt_setsockopt(sdata->listen_socket, 0, SRTO_LATENCY, &zero, sizeof(zero));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(sdata->listen_port));
    if (inet_aton(sdata->listen_address, &addr.sin_addr) == 0) {
        log(LOG_ERR, "srt_stream: invalid listen address %s\n", sdata->listen_address);
        return false;
    }

    if (srt_bind(sdata->listen_socket, (struct sockaddr*)&addr, sizeof(addr)) == SRT_ERROR) {
        log(LOG_ERR, "srt_stream: bind failed: %s\n", srt_getlasterror_str());
        return false;
    }
    if (srt_listen(sdata->listen_socket, 5) == SRT_ERROR) {
        log(LOG_ERR, "srt_stream: listen failed: %s\n", srt_getlasterror_str());
        return false;
    }

    sdata->clients.clear();
    log(LOG_INFO, "srt_stream: listening on %s:%s\n", sdata->listen_address, sdata->listen_port);
    return true;
}

static void srt_stream_send_header(srt_stream_data* sdata, srt_client& client) {
    if (sdata->format != SRT_STREAM_WAV)
        return;

    const int channels = (sdata->mode == MM_STEREO) ? 2 : 1;
    const int sample_rate = WAVE_RATE;
    const int bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    uint16_t block_align = channels * bits_per_sample / 8;

    char header[44];
    memcpy(header, "RIFF", 4);
    /* use 0 for indefinite length so players avoid warnings */
    uint32_t sz = 0u;
    memcpy(header + 4, &sz, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    memcpy(header + 16, &fmt_size, 4);
    uint16_t audio_format = 1; /* PCM */
    memcpy(header + 20, &audio_format, 2);
    uint16_t ch = channels;
    memcpy(header + 22, &ch, 2);
    uint32_t sr = sample_rate;
    memcpy(header + 24, &sr, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    uint16_t bps = bits_per_sample;
    memcpy(header + 34, &bps, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &sz, 4);

    srt_send(client.sock, header, sizeof(header));
}

static void srt_stream_accept(srt_stream_data* sdata) {
    while (true) {
        struct sockaddr_storage rem;
        int len = sizeof(rem);
        SRTSOCKET s = srt_accept(sdata->listen_socket, (struct sockaddr*)&rem, &len);
        if (s == SRT_INVALID_SOCK) {
            int serr;
            srt_getlasterror(&serr);
            if (serr == SRT_EASYNCRCV)
                break;
            break;
        }
        int blocking = 0;
        srt_setsockopt(s, 0, SRTO_SNDSYN, &blocking, sizeof(blocking));
        srt_setsockopt(s, 0, SRTO_RCVSYN, &blocking, sizeof(blocking));
        int tsbpd = 0;
        srt_setsockopt(s, 0, SRTO_TSBPDMODE, &tsbpd, sizeof(tsbpd));
        int zero = 0;
        srt_setsockopt(s, 0, SRTO_LATENCY, &zero, sizeof(zero));
        srt_client c{s, false};
        sdata->clients.push_back(c);
    }
}

void srt_stream_write(srt_stream_data* sdata, const float* data, size_t len) {
    if (sdata->format == SRT_STREAM_WAV) {
        size_t sample_count = len / sizeof(float);
        if (sample_count > sdata->pcm_buffer_len)
            return;
        for (size_t i = 0; i < sample_count; ++i) {
            float v = data[i];
            if (v > 1.0f)
                v = 1.0f;
            else if (v < -1.0f)
                v = -1.0f;
            sdata->pcm_buffer[i] = (int16_t)(v * 32767.0f);
        }
        srt_stream_send(sdata, (const char*)sdata->pcm_buffer, sample_count * sizeof(int16_t));
    } else {
        srt_stream_send(sdata, (const char*)data, len);
    }
}

void srt_stream_send_bytes(srt_stream_data* sdata, const unsigned char* data, size_t len) {
    srt_stream_send(sdata, (const char*)data, len);
}

void srt_stream_write(srt_stream_data* sdata, const float* left, const float* right, size_t len) {
    if (!sdata->stereo_buffer)
        return;
    size_t sample_count = len / sizeof(float);
    if (sample_count * 2 > sdata->stereo_buffer_len)
        return;
    for (size_t i = 0; i < sample_count; ++i) {
        sdata->stereo_buffer[2 * i] = left[i];
        sdata->stereo_buffer[2 * i + 1] = right[i];
    }
    if (sdata->format == SRT_STREAM_WAV) {
        if (sample_count * 2 > sdata->pcm_buffer_len)
            return;
        for (size_t i = 0; i < sample_count * 2; ++i) {
            float v = sdata->stereo_buffer[i];
            if (v > 1.0f)
                v = 1.0f;
            else if (v < -1.0f)
                v = -1.0f;
            sdata->pcm_buffer[i] = (int16_t)(v * 32767.0f);
        }
        srt_stream_send(sdata, (const char*)sdata->pcm_buffer, sample_count * 2 * sizeof(int16_t));
    } else {
        srt_stream_send(sdata, (const char*)sdata->stereo_buffer, sample_count * 2 * sizeof(float));
    }
}

void srt_stream_shutdown(srt_stream_data* sdata) {
    for (auto& c : sdata->clients) {
        srt_close(c.sock);
    }
    sdata->clients.clear();
    free(sdata->stereo_buffer);
    sdata->stereo_buffer = NULL;
    free(sdata->pcm_buffer);
    sdata->pcm_buffer = NULL;
    if (sdata->listen_socket != SRT_INVALID_SOCK) {
        srt_close(sdata->listen_socket);
        sdata->listen_socket = SRT_INVALID_SOCK;
    }
}

#endif  // WITH_SRT

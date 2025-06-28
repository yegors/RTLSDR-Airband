#include <arpa/inet.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <vector>

#include "rtl_airband.h"

#ifdef WITH_SRT

static bool srt_initialized = false;

static void srt_try_startup() {
    if (!srt_initialized) {
        if (srt_startup() != 0) {
            log(LOG_ERR, "srt_stream: srt_startup failed: %s\n", srt_getlasterror_str());
        } else {
            srt_initialized = true;
        }
    }
}

bool srt_stream_init(srt_stream_data* sdata, mix_modes mode, size_t len) {
    srt_try_startup();
    if (!srt_initialized) {
        return false;
    }

    if (mode == MM_STEREO) {
        sdata->stereo_buffer_len = (len / sizeof(float)) * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    sdata->listen_socket = srt_create_socket();
    if (sdata->listen_socket == SRT_INVALID_SOCK) {
        log(LOG_ERR, "srt_stream: socket failed: %s\n", srt_getlasterror_str());
        return false;
    }

    int len_tmp = sizeof(sdata->payload_size);
    if (srt_getsockopt(sdata->listen_socket, 0, SRTO_PAYLOADSIZE,
                       &sdata->payload_size, &len_tmp) == SRT_ERROR) {
        sdata->payload_size = SRT_LIVE_DEF_PLSIZE;
    }

    int blocking = 0;
    srt_setsockopt(sdata->listen_socket, 0, SRTO_SNDSYN, &blocking, sizeof(blocking));
    srt_setsockopt(sdata->listen_socket, 0, SRTO_RCVSYN, &blocking, sizeof(blocking));

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
        sdata->clients.push_back(s);
    }
}

void srt_stream_write(srt_stream_data* sdata, const float* data, size_t len) {
    if (sdata->listen_socket == SRT_INVALID_SOCK)
        return;

    srt_stream_accept(sdata);
    for (auto it = sdata->clients.begin(); it != sdata->clients.end();) {
        const char* ptr = (const char*)data;
        size_t remaining = len;
        while (remaining > 0) {
            int chunk = remaining > (size_t)sdata->payload_size ? sdata->payload_size : remaining;
            int ret = srt_send(*it, ptr, chunk);
            if (ret == SRT_ERROR) {
                int serr;
                srt_getlasterror(&serr);
                if (serr != SRT_EASYNCSND) {
                    srt_close(*it);
                    it = sdata->clients.erase(it);
                    goto next_client;
                }
            }
            ptr += chunk;
            remaining -= chunk;
        }
        ++it;
        continue;
    next_client:
        ;
    }
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
    srt_stream_write(sdata, sdata->stereo_buffer, sample_count * 2 * sizeof(float));
}

void srt_stream_shutdown(srt_stream_data* sdata) {
    for (auto s : sdata->clients) {
        srt_close(s);
    }
    sdata->clients.clear();
    if (sdata->listen_socket != SRT_INVALID_SOCK) {
        srt_close(sdata->listen_socket);
        sdata->listen_socket = SRT_INVALID_SOCK;
    }
}

#endif  // WITH_SRT

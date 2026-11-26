/**
 * @file test_zm_sdp.c
 * @brief SDP -> zm_sdp_track parsing.
 */
#include "zm/transport/sdp.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                               \
        }                                                                               \
    } while (0)

int main(void)
{
    static const char k_sdp[] =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=zms-test\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=video 5000 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "a=control:trackID=0\r\n"
        "m=audio 5002 RTP/AVP 97\r\n"
        "a=rtpmap:97 mpeg4-generic/44100/2\r\n"
        "a=fmtp:97 config=1210\r\n"
        "a=control:trackID=1\r\n";
    zm_sdp *sdp = zm_sdp_parse(k_sdp, sizeof(k_sdp) - 1);
    const zm_sdp_track *t;
    size_t i;
    int saw_video = 0;
    int saw_audio = 0;

    CHECK(sdp != NULL);
    if (!sdp) {
        return 1;
    }
    CHECK(zm_sdp_track_count(sdp) == 2);
    for (i = 0; i < zm_sdp_track_count(sdp); ++i) {
        t = zm_sdp_track_at(sdp, i);
        CHECK(t != NULL);
        if (!t) {
            continue;
        }
        if (t->kind == ZM_KIND_VIDEO && t->codec == ZM_CODEC_H264) {
            saw_video = 1;
            CHECK(t->payload_type == 96);
            CHECK(t->clock_hz == 90000);
            CHECK(strstr(t->fmtp, "packetization-mode=1") != NULL);
        }
        if (t->kind == ZM_KIND_AUDIO && t->codec == ZM_CODEC_AAC) {
            saw_audio = 1;
            CHECK(t->payload_type == 97);
            CHECK(t->clock_hz == 44100);
            CHECK(t->channels == 2);
            CHECK(strstr(t->fmtp, "config=1210") != NULL);
        }
    }
    CHECK(saw_video);
    CHECK(saw_audio);
    CHECK(zm_sdp_track_at(sdp, 2) == NULL);

    zm_sdp_destroy(sdp);

    if (g_failures) {
        fprintf(stderr, "test_zm_sdp: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_zm_sdp ok\n");
    return 0;
}

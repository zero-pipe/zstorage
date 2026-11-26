#include "zm/transport/sdp.h"

#include "zm/core/codec.h"

#include "sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct zm_sdp {
    zm_sdp_track *tracks;
    size_t count;
    size_t cap;
};

typedef struct sdp_ctx {
    struct zm_sdp *out;
    sdp_t *sdp;
    int media;
    zm_kind kind;
    const char *control;
} sdp_ctx;

static int sdp_add_track(struct zm_sdp *s, const zm_sdp_track *track)
{
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4;
        zm_sdp_track *p = (zm_sdp_track *)realloc(s->tracks, cap * sizeof(*p));
        if (!p) {
            return -1;
        }
        s->tracks = p;
        s->cap = cap;
    }
    s->tracks[s->count++] = *track;
    return 0;
}

static zm_kind sdp_media_kind(const char *media)
{
    if (!media) {
        return ZM_KIND_UNKNOWN;
    }
    if (strcmp(media, "video") == 0) {
        return ZM_KIND_VIDEO;
    }
    if (strcmp(media, "audio") == 0) {
        return ZM_KIND_AUDIO;
    }
    if (strcmp(media, "text") == 0 || strcmp(media, "application") == 0) {
        return ZM_KIND_SUBTITLE;
    }
    return ZM_KIND_UNKNOWN;
}

static void sdp_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int sdp_parse_rtpmap(const char *value, int *pt, char *encoding, size_t encoding_cap,
                            int *clock_hz, int *channels)
{
    char enc[64];
    int n;

    if (!value) {
        return 0;
    }
    *channels = 0;
    n = sscanf(value, "%d %63[^/]/%d/%d", pt, enc, clock_hz, channels);
    if (n < 3) {
        return 0;
    }
    sdp_copy(encoding, encoding_cap, enc);
    return 1;
}

typedef struct fmtp_ctx {
    int target_pt;
    char *out;
    size_t cap;
} fmtp_ctx;

static void sdp_on_fmtp(void *param, const char *name, const char *value)
{
    fmtp_ctx *ctx = (fmtp_ctx *)param;
    int pt;
    char text[512];

    (void)name;
    if (!ctx || !value) {
        return;
    }
    if (sscanf(value, "%d %511[^\n]", &pt, text) != 2 || pt != ctx->target_pt) {
        return;
    }
    sdp_copy(ctx->out, ctx->cap, text);
}

static void sdp_on_rtpmap(void *param, const char *name, const char *value)
{
    sdp_ctx *ctx = (sdp_ctx *)param;
    zm_sdp_track track;
    zm_codec_id codec;
    char encoding[64];
    int pt;
    int clock_hz;
    int channels;
    fmtp_ctx fc;

    (void)name;
    if (!ctx || !ctx->out || !ctx->sdp) {
        return;
    }
    if (!sdp_parse_rtpmap(value, &pt, encoding, sizeof(encoding), &clock_hz, &channels)) {
        return;
    }
    codec = zm_codec_from_rtpmap(encoding, clock_hz);
    if (codec == ZM_CODEC_INVALID) {
        codec = zm_codec_from_name(encoding);
    }
    if (codec == ZM_CODEC_INVALID) {
        return;
    }

    memset(&track, 0, sizeof(track));
    track.kind = ctx->kind;
    track.codec = codec;
    track.payload_type = pt;
    track.clock_hz = clock_hz;
    track.channels = channels;
    sdp_copy(track.encoding, sizeof(track.encoding), encoding);
    sdp_copy(track.control, sizeof(track.control), ctx->control);

    fc.target_pt = pt;
    fc.out = track.fmtp;
    fc.cap = sizeof(track.fmtp);
    sdp_media_attribute_list(ctx->sdp, ctx->media, "fmtp", sdp_on_fmtp, &fc);
    sdp_add_track(ctx->out, &track);
}

zm_sdp *zm_sdp_parse(const char *text, size_t len)
{
    sdp_t *sdp;
    struct zm_sdp *out;
    int count;
    int i;

    if (!text || len == 0 || len > 0x7fffffff) {
        return NULL;
    }
    sdp = sdp_parse(text, (int)len);
    if (!sdp) {
        return NULL;
    }
    out = (struct zm_sdp *)calloc(1, sizeof(*out));
    if (!out) {
        sdp_destroy(sdp);
        return NULL;
    }

    count = sdp_media_count(sdp);
    for (i = 0; i < count; ++i) {
        sdp_ctx ctx;
        const char *control = sdp_media_attribute_find(sdp, i, "control");

        ctx.out = out;
        ctx.sdp = sdp;
        ctx.media = i;
        ctx.kind = sdp_media_kind(sdp_media_type(sdp, i));
        if (ctx.kind == ZM_KIND_UNKNOWN) {
            continue;
        }
        ctx.control = control ? control : sdp_attribute_find(sdp, "control");
        sdp_media_attribute_list(sdp, i, "rtpmap", sdp_on_rtpmap, &ctx);
    }

    sdp_destroy(sdp);
    return out;
}

size_t zm_sdp_track_count(const zm_sdp *s)
{
    return s ? s->count : 0;
}

const zm_sdp_track *zm_sdp_track_at(const zm_sdp *s, size_t index)
{
    if (!s || index >= s->count) {
        return NULL;
    }
    return &s->tracks[index];
}

void zm_sdp_destroy(zm_sdp *s)
{
    if (!s) {
        return;
    }
    free(s->tracks);
    free(s);
}

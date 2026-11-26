#include "zm/core/reader.h"

int zm_reader_input(zm_reader *r, const void *buf, size_t len)
{
    if (!r || !r->ops || !r->ops->input) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return r->ops->input(r, buf, len);
}

int zm_reader_read(zm_reader *r, zm_packet *out)
{
    if (!r || !out || !r->ops || !r->ops->read) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return r->ops->read(r, out);
}

int zm_reader_read_raw(zm_reader *r, zm_raw_unit *out)
{
    if (!r || !out || !r->ops || !r->ops->read_raw) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return r->ops->read_raw(r, out);
}

int zm_reader_seek(zm_reader *r, int64_t *ms)
{
    if (!r || !r->ops || !r->ops->seek) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return r->ops->seek(r, ms);
}

const zm_track *zm_reader_tracks(zm_reader *r, size_t *count)
{
    if (!r || !r->ops || !r->ops->tracks) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    return r->ops->tracks(r, count);
}

void zm_reader_close(zm_reader *r)
{
    if (r && r->ops && r->ops->close) {
        r->ops->close(r);
    }
}

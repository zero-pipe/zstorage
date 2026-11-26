#include "zm/core/writer.h"

int zm_writer_write(zm_writer *w, const zm_packet *pkt)
{
    if (!w || !pkt || !w->ops || !w->ops->write) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return w->ops->write(w, pkt);
}

int zm_writer_write_raw(zm_writer *w, const zm_raw_unit *unit)
{
    if (!w || !unit || !w->ops || !w->ops->write_raw) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return w->ops->write_raw(w, unit);
}

int zm_writer_flush(zm_writer *w, int force_boundary)
{
    if (!w) {
        return ZM_ERR_INVALID;
    }
    if (!w->ops || !w->ops->flush) {
        return ZM_OK;
    }
    return w->ops->flush(w, force_boundary);
}

int zm_writer_finish(zm_writer *w)
{
    if (!w) {
        return ZM_ERR_INVALID;
    }
    if (!w->ops || !w->ops->finish) {
        return ZM_OK;
    }
    return w->ops->finish(w);
}

void zm_writer_close(zm_writer *w)
{
    if (w && w->ops && w->ops->close) {
        w->ops->close(w);
    }
}

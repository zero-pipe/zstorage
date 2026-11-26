#include "zm/packager/packager.h"

int zm_packager_write(zm_packager *p, const zm_packet *pkt)
{
    if (!p || !p->ops || !p->ops->write) {
        return ZM_ERR_NOT_SUPPORTED;
    }
    return p->ops->write(p, pkt);
}

int zm_packager_flush(zm_packager *p, int force)
{
    if (!p) {
        return ZM_ERR_INVALID;
    }
    if (!p->ops || !p->ops->flush) {
        return ZM_OK;
    }
    return p->ops->flush(p, force);
}

void zm_packager_close(zm_packager *p)
{
    if (p && p->ops && p->ops->close) {
        p->ops->close(p);
    }
}

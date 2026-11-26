#include "zm/core/packet.h"

#include <string.h>

void zm_packet_init(zm_packet *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

void zm_packet_clear(zm_packet *p)
{
    if (!p) {
        return;
    }
    zm_mem_unref(&p->mem);
    zm_packet_init(p);
}

void zm_raw_unit_init(zm_raw_unit *u)
{
    if (u) {
        memset(u, 0, sizeof(*u));
    }
}

void zm_raw_unit_clear(zm_raw_unit *u)
{
    if (!u) {
        return;
    }
    zm_mem_unref(&u->body);
    zm_raw_unit_init(u);
}

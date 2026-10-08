/* net_host.h -- the built-in lobby/relay ("Host a LAN game"): ONE room of the hosting game, speaking NMN2 (and RRN1 when the
 * game is Rave Racer, so released builds on the LAN can join), inside the game, so a LAN needs no separate server.
 * Behaviour mirrors server/ (nmn-server). Polled from eng_net_poll once per frame; inert until started. */
#ifndef ENG_NET_HOST_H
#define ENG_NET_HOST_H
#include <stdbool.h>
#include "net.h"
bool eng_nethost_start(int port, const eng_net_game *g);   /* false = the port is busy (another host / a server) */
void eng_nethost_stop(void);
bool eng_nethost_running(void);
void eng_nethost_set_name(const char *name);               /* the host's name, sent in discovery answers */
void eng_nethost_poll(void);
#endif

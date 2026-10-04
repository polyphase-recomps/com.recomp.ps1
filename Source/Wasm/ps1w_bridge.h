/*
 * Script bridge, host side (see Runtime/port/include/port_bridge.h). The backend turns
 * the guest's port_bridge_* imports into these calls, with guest addresses; the host
 * reads the published tables through ps1w_guest_ptr.
 */
#ifndef PS1W_BRIDGE_H
#define PS1W_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Game thread. vars / requests: guest addresses of the PortBridgeVar / PortBridgeRequest
 * arrays (wasm32 layout: 6 and 3 32-bit words per entry). */
void ps1w_host_bridge_publish(unsigned vars, int nvars, unsigned requests, int nrequests);
/* Game thread: the next queued request, as in port_bridge_poll. */
int ps1w_host_bridge_poll(char *name, unsigned name_cap, int *args, int max_args, int *nargs);
void ps1w_host_bridge_done(int id, int result);

/* Any thread: host pointer to `len` bytes of guest memory at guest address `addr`, or
 * NULL when no game runs or the range is outside guest memory. Guest memory is little
 * endian on every host. */
void *ps1w_guest_ptr(unsigned addr, unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* PS1W_BRIDGE_H */

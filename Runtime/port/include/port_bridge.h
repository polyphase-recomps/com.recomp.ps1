/*
 * Script bridge: what a game publishes to Polyphase scripts and UI.
 *
 * A game package (its mod code) describes named variables and named requests once, and
 * calls port_bridge_pump() once per frame at a safe point of its main loop. Polyphase
 * (Lua `Ps1.*`, C++ Ps1GuestHost::Bridge*) then
 *  - reads variables straight from game memory, any time (values are those of the last
 *    frame, good for UI);
 *  - writes variables and runs requests through a queue the game drains in
 *    port_bridge_pump(), so game code only ever runs on the game thread, between frames.
 *
 * Example (a game's mod code):
 *
 *     extern int MONEY;
 *     static int warp(const int *args, int n) { ... changeMap(args[0], args[1]); return 0; }
 *
 *     static const PortBridgeVar kVars[] = {
 *         { "money", &MONEY, PB_S32, 1, 0, "bits" },
 *     };
 *     static const PortBridgeRequest kRequests[] = {
 *         { "warp", warp, "map, exit: go to a map" },
 *     };
 *     ...
 *     port_bridge_init(kVars, 1, kRequests, 1);   // once
 *     port_bridge_pump();                           // every frame
 *
 * Every variable can also be written: Lua Ps1.Set(name, value[, index]) queues the
 * built-in request "set <name>", done in port_bridge_pump() like any other.
 */
#ifndef PORT_BRIDGE_H
#define PORT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* variable types */
enum
{
    PB_U8 = 1,
    PB_S8,
    PB_U16,
    PB_S16,
    PB_U32,
    PB_S32,
    PB_STR, /* text: `count` strings of `stride` bytes each (NUL-terminated or full);
             * stride 0 = a single string of `count` bytes */
};

/* Request results the bridge itself reports (handlers return their own values). */
#define PB_RESULT_UNKNOWN (-1000)  /* no such request or variable */
#define PB_RESULT_BAD_ARGS (-1001) /* wrong number of arguments / index out of range */

typedef struct PortBridgeVar
{
    const char *name;
    void *addr;     /* first element */
    int type;       /* PB_* */
    int count;      /* elements (array length); see PB_STR for text */
    int stride;     /* bytes between elements; 0 = the element size */
    const char *help;
} PortBridgeVar;

/* Runs on the game thread; returns a result for the script (>= 0 by convention for
 * success, negative for "not now" / errors the request documents). */
typedef int (*PortBridgeFn)(const int *args, int nargs);

typedef struct PortBridgeRequest
{
    const char *name;
    PortBridgeFn fn;
    const char *help; /* arguments and what it does, shown by Ps1.Requests() */
} PortBridgeRequest;

/* Publishes the tables (they must stay valid: make them static const). */
void port_bridge_init(const PortBridgeVar *vars, int nvars, const PortBridgeRequest *requests, int nrequests);
/* Runs the queued requests; call once per frame where game functions may be called. */
void port_bridge_pump(void);

/* ---- host imports used by port/guest/bridge.c (not for game code) ---------------- */
void port_bridge_publish(const void *vars, int nvars, const void *requests, int nrequests);
/* Takes the next queued request: its name and integer arguments. Returns its id (> 0),
 * or 0 when the queue is empty. */
int port_bridge_poll(char *name, unsigned name_cap, int *args, int max_args, int *nargs);
void port_bridge_done(int id, int result);

#ifdef __cplusplus
}
#endif

#endif /* PORT_BRIDGE_H */

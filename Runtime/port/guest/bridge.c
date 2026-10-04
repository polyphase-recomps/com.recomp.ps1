/* Script bridge, game side (see port_bridge.h): keeps the tables the game published and
 * runs the requests the host queued, on the game thread. */
#include "port_bridge.h"
#include "port_host.h"

#define MAX_ARGS 8

static const PortBridgeVar *sVars;
static int sVarCount;
static const PortBridgeRequest *sRequests;
static int sRequestCount;

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

void port_bridge_init(const PortBridgeVar *vars, int nvars, const PortBridgeRequest *requests, int nrequests)
{
    sVars = vars;
    sVarCount = nvars;
    sRequests = requests;
    sRequestCount = nrequests;
    port_bridge_publish(vars, nvars, requests, nrequests);
    port_log("bridge: %d variables, %d requests", nvars, nrequests);
}

static int element_size(int type)
{
    switch (type)
    {
    case PB_U16:
    case PB_S16: return 2;
    case PB_U32:
    case PB_S32: return 4;
    default: return 1;
    }
}

/* "set <name>": args value[, index] */
static int set_variable(const char *name, const int *args, int nargs)
{
    int i;

    for (i = 0; i < sVarCount; i++)
    {
        const PortBridgeVar *v = &sVars[i];
        int index = nargs > 1 ? args[1] : 0;
        unsigned char *p;

        if (!same(v->name, name)) continue;
        if (nargs < 1 || v->type == PB_STR || index < 0 || index >= v->count) return PB_RESULT_BAD_ARGS;
        p = (unsigned char *)v->addr + index * (v->stride ? v->stride : element_size(v->type));
        switch (v->type)
        {
        case PB_U8:
        case PB_S8: *p = (unsigned char)args[0]; break;
        case PB_U16:
        case PB_S16: *(unsigned short *)p = (unsigned short)args[0]; break;
        default: *(unsigned *)p = (unsigned)args[0]; break;
        }
        return 0;
    }
    return PB_RESULT_UNKNOWN;
}

void port_bridge_pump(void)
{
    char name[64];
    int args[MAX_ARGS], nargs, id;

    while ((id = port_bridge_poll(name, sizeof(name), args, MAX_ARGS, &nargs)) > 0)
    {
        int result = PB_RESULT_UNKNOWN, i;

        if (name[0] == 's' && name[1] == 'e' && name[2] == 't' && name[3] == ' ')
        {
            result = set_variable(name + 4, args, nargs);
        }
        else
        {
            for (i = 0; i < sRequestCount; i++)
            {
                if (same(sRequests[i].name, name))
                {
                    result = sRequests[i].fn(args, nargs);
                    break;
                }
            }
        }
        port_bridge_done(id, result);
    }
}

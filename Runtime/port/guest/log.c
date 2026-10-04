/* port_log / port_fatal: formatted on the guest side, written by the host. */
#include <port_host.h>

typedef __builtin_va_list va_list_t;
int port_vsnprintf(char *buf, int cap, const char *fmt, va_list_t ap);

void port_log(const char *fmt, ...)
{
    char buf[1024];
    va_list_t ap;

    __builtin_va_start(ap, fmt);
    port_vsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    port_host_log(buf);
}

void port_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list_t ap;

    __builtin_va_start(ap, fmt);
    port_vsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    port_host_fatal(buf);
    for (;;)
    {
    }
}

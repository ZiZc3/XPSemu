/* XOG: Android's system properties, the debug knobs of hakuX's core
 * (debug.hakux.*). The PS5 has none: every knob reads as not set, so each one
 * takes its default. A settings file can answer these later. */
#ifndef REXEMU_SYS_SYSTEM_PROPERTIES_H
#define REXEMU_SYS_SYSTEM_PROPERTIES_H

#define PROP_VALUE_MAX 92

static inline int __system_property_get(const char *name, char *value)
{
    (void)name;
    value[0] = 0;
    return 0;
}

#endif

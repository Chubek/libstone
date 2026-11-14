#ifndef MOONSTONE_EXOML_PARSER_BRIDGE_H
#define MOONSTONE_EXOML_PARSER_BRIDGE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Callbacks must not throw through the generated C parser. */
typedef void (*exoml_command_callback)(void *, int, const char *, const char *, const char *,
                                       size_t, size_t);
int exoml_parse_commands(const char *, size_t, size_t, exoml_command_callback, void *, size_t *);
#ifdef __cplusplus
}
#endif
#endif

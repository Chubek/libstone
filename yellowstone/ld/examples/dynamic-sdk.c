/* cc -std=c11 -Wall -Wextra -Werror -I<prefix>/include -c dynamic-sdk.c
 * c++ dynamic-sdk.o -L<prefix>/lib -lqld -llibstone_script_lua -lm -ldl -o dynamic-sdk
 * ./dynamic-sdk input-pic.o libexample.so
 */
#include "ld.h"
#include <stdio.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s input-pic.o output.so\n", argv[0]);
    return 2;
  }
  const char *inputs[] = {argv[1]};
  const char *arguments[] = {"--soname=libexample.so", "-z", "defs", "-z", "relro"};
  qld_options_t options;
  qld_options_init(&options);
  options.inputs = inputs;
  options.n_inputs = 1;
  options.output = argv[2];
  options.mode = QLD_MODE_SHARED;
  qld_elf_options_t elf;
  qld_elf_options_init(&elf);
  elf.arguments = arguments;
  elf.n_arguments = sizeof(arguments) / sizeof(arguments[0]);
  char *error = NULL;
  int status = qld_link_elf(&options, &elf, &error);
  if (status) fprintf(stderr, "%s\n", error ? error : "link failed");
  qld_free_string(error);
  return status ? 1 : 0;
}

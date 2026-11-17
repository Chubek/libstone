/* Real C11 consumer: formats-sdk pe|macho input.o output linker */
#include "ld.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 5) return 2;
  qld_options_t options;
  qld_options_init(&options);
  options.inputs = (const char *const *)&argv[2];
  options.n_inputs = 1;
  options.output = argv[3];
  options.mode = QLD_MODE_SHARED;
  qld_pe_options_t config;
  qld_pe_options_init(&config);
  config.linker = argv[4];
  const char *pe[] = {"/noentry"};
  const char *macho[] = {"-arch", "x86_64", "-platform_version", "macos", "11.0", "11.0"};
  int is_pe = strcmp(argv[1], "pe") == 0;
  config.arguments = is_pe ? pe : macho;
  config.n_arguments = is_pe ? 1 : 6;
  char *error = NULL;
  int status = is_pe ? qld_link_pe(&options, &config, &error)
                     : qld_link_macho(&options, &config, &error);
  if (error) fprintf(stderr, "%s\n", error);
  qld_free_string(error);
  return status;
}

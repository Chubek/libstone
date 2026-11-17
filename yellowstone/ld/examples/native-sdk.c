/* Native C11 SDK consumer: native-sdk entry output object [object/archive ...] */
#include "ld.h"
#include <stdio.h>

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  qld_options_t options;
  qld_options_init(&options);
  options.entry = argv[1];
  options.output = argv[2];
  options.inputs = (const char *const *)&argv[3];
  options.n_inputs = (size_t)(argc - 3);
  char *error = NULL;
  int status = qld_link(&options, &error);
  if (status) fprintf(stderr, "%s\n", error ? error : "native link failed");
  qld_free_string(error);
  return status ? 1 : 0;
}

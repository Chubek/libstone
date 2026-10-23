#include "ArchBFD.h"
#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv) {
  archbfd_object *object = NULL;
  archbfd_error error;
  archbfd_object_info info;
  size_t i;
  if (argc != 2) {
    fprintf(stderr, "usage: %s OBJECT\n", argv[0]);
    return 2;
  }
  if (archbfd_open_file(argv[1], NULL, &object, &error) != ARCHBFD_OK)
    goto failed;
  if (archbfd_get_object_info(object, &info, &error) != ARCHBFD_OK)
    goto failed;
  printf("%s %s\n", archbfd_format_name(info.format), archbfd_arch_name(info.arch));
  for (i = 0; i < info.section_count; ++i) {
    archbfd_section section;
    if (archbfd_get_section(object, i, &section, &error) != ARCHBFD_OK)
      goto failed;
    printf("%" PRIu32 " 0x%" PRIx64 " %" PRIu64 " ",
           section.index, section.vma, section.size);
    fwrite(section.name.data, 1, section.name.size, stdout);
    putchar('\n');
  }
  archbfd_close(object);
  return 0;
failed:
  fprintf(stderr, "%s: %s\n", argv[1], error.message);
  archbfd_close(object);
  return 1;
}

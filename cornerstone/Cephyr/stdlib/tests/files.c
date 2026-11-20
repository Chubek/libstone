#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void regular_files(void)
{
    char first[L_tmpnam];
    char second[L_tmpnam];
    char buf[24];
    char provided[8];
    fpos_t bookmark;
    FILE *file;
    FILE *peer;

    assert(tmpnam(first) == first && tmpnam(second) == second);
    assert(strcmp(first, second) != 0);
    assert(fopen(first, "rb") == NULL && errno == ENOENT);
    file = fopen(first, "w+bx");
    assert(file != NULL && fopen(first, "wx") == NULL && errno == EEXIST);
    assert(setvbuf(file, provided, _IOFBF, sizeof(provided)) == 0);
    assert(fwrite("abcdef", 2, 3, file) == 3 && ftell(file) == 6);
    peer = fopen(first, "rb");
    assert(peer != NULL && fgetc(peer) == EOF && feof(peer) && !ferror(peer));
    assert(fflush(NULL) == 0);
    clearerr(peer);
    assert(fread(buf, 2, 3, peer) == 3 && memcmp(buf, "abcdef", 6) == 0);
    assert(fclose(peer) == 0);

    assert(fseek(file, 0, SEEK_SET) == 0);
    assert(fgetc(file) == 'a' && ftell(file) == 1);
    assert(ungetc('X', file) == 'X' && ftell(file) == 0);
    assert(ungetc(EOF, file) == EOF && fgetc(file) == 'X');
    assert(fgetpos(file, &bookmark) == 0 && fgetc(file) == 'b');
    assert(fsetpos(file, &bookmark) == 0 && fgetc(file) == 'b');
    assert(ungetc('Y', file) == 'Y' && fseek(file, 1, SEEK_CUR) == 0);
    assert(ftell(file) == 2 && fgetc(file) == 'c');
    assert(fread(buf, 2, 2, file) == 1 && memcmp(buf, "def", 3) == 0);
    assert(feof(file) && !ferror(file));
    assert(ungetc('!', file) == '!' && !feof(file) && fgetc(file) == '!');
    assert(fgetc(file) == EOF && feof(file));
    assert(fseek(file, 0, SEEK_SET) == 0 && !feof(file));
    assert(fclose(file) == 0);

    assert(rename(first, second) == 0);
    file = fopen(second, "a+b");
    assert(file != NULL && fgetc(file) == 'a');
    assert(fseek(file, 0, SEEK_SET) == 0 && fputs("!\n", file) >= 0);
    assert(ftell(file) == 8 && fclose(file) == 0);
    file = fopen(second, "r");
    assert(file != NULL && fgets(buf, sizeof(buf), file) == buf);
    assert(strcmp(buf, "abcdef!\n") == 0);
    assert(fputc('?', file) == EOF && ferror(file) && errno == EBADF);
    rewind(file);
    assert(!feof(file) && !ferror(file) && fgetc(file) == 'a');
    assert(freopen(second, "w", file) == file);
    assert(setvbuf(file, NULL, _IOLBF, 8192) == 0);
    assert(fputs("line", file) >= 0);
    peer = fopen(second, "r");
    assert(peer != NULL && fgetc(peer) == EOF);
    assert(fputc('\n', file) == '\n');
    clearerr(peer);
    assert(fgets(buf, sizeof(buf), peer) == buf && strcmp(buf, "line\n") == 0);
    assert(fclose(peer) == 0);
    assert(freopen(first, "r", file) == NULL && errno == ENOENT);
    assert(remove(second) == 0 && remove(second) != 0 && errno == ENOENT);
}

static void temporary_files(void)
{
    FILE *file = tmpfile();
    char storage[BUFSIZ];
    char data[BUFSIZ + 3];
    assert(file != NULL);
    setbuf(file, storage);
    memset(data, 't', sizeof(data));
    data[sizeof(data) - 1] = 'z';
    assert(fwrite(data, sizeof(data), 1, file) == 1);
    assert(fseek(file, -2, SEEK_END) == 0);
    assert(fgetc(file) == 't' && fgetc(file) == 'z' && fgetc(file) == EOF);
    assert(feof(file) && !ferror(file) && fclose(file) == 0);
}

static void failed_flush(void)
{
    FILE *file = fopen("/dev/full", "w");
    assert(file != NULL);
    assert(setvbuf(file, NULL, _IOFBF, 0) == 0);
    assert(fputs("buffered", file) >= 0 && !ferror(file));
    assert(fflush(NULL) == EOF && ferror(file) && errno == ENOSPC);
    assert(fclose(file) == EOF && errno == ENOSPC);
    assert(fflush(NULL) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && remove(argv[1]) == 0);
    regular_files();
    temporary_files();
    failed_flush();
    return 0;
}

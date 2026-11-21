#include <wchar.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

int main(void)
{
    wchar_t line[8] = {0};
    fpos_t saved;
    FILE *file;
    char name[L_tmpnam];
    char bytes[8];

    CHECK(fwide(stdin, 0) == 0 && fwide(stdout, 0) == 0 && fwide(stderr, 0) == 0);
    CHECK(getwchar() == L'Z' && fwide(stdin, -1) > 0);
    CHECK(ungetwc(L'Y', stdin) == L'Y');
    CHECK(fseek(stdin, 0, SEEK_CUR) != 0 && errno == ESPIPE);
    CHECK(getwchar() == L'Y' && getwchar() == L'!' && getwchar() == WEOF);
    CHECK(putwchar(L'Q') == L'Q' && fwide(stdout, 0) > 0);
    errno = EINVAL;
    perror("wide stream test");
    CHECK(fwide(stderr, 0) == 0);
    CHECK(fwide(stderr, 42) > 0);
    perror(NULL);
    CHECK(fwide(stderr, 0) > 0 && !ferror(stderr));

    file = tmpfile();
    CHECK(file != NULL && setvbuf(file, NULL, _IOFBF, sizeof(bytes)) == 0);
    CHECK(fwide(file, 0) == 0);
    CHECK(fread(bytes, 0, 1, file) == 0 && fwrite(bytes, 1, 0, file) == 0);
    CHECK(ungetwc(WEOF, file) == WEOF && ungetc(EOF, file) == EOF);
    CHECK(fflush(file) == 0 && ftell(file) == 0 && fwide(file, 0) == 0);
    CHECK(fputws(L"ab\ncd", file) >= 0 && fwide(file, 0) > 0);
    CHECK(putwc(L'\0', file) == 0);
    CHECK(fputwc(L'!', file) == L'!' && ftell(file) == 7);
    CHECK(fwide(file, -88) > 0);
    rewind(file);
    CHECK(fwide(file, 0) > 0 && !feof(file) && !ferror(file));
    CHECK(fgetws(line, 1, file) == line && line[0] == 0 && ftell(file) == 0);
    CHECK(fgetws(line, 3, file) == line && wcscmp(line, L"ab") == 0);
    CHECK(fgetws(line, 8, file) == line && wcscmp(line, L"\n") == 0);
    CHECK(fgetpos(file, &saved) == 0);
    CHECK(getwc(file) == L'c' && ftell(file) == 4);
    CHECK(ungetwc(0x10000, file) == 0x10000);
    CHECK(ungetwc(L'x', file) == WEOF);
    CHECK(ungetwc(WEOF, file) == WEOF && fgetwc(file) == 0x10000 && ftell(file) == 4);
    CHECK(ungetwc(L'x', file) == L'x');
    CHECK(fsetpos(file, &saved) == 0 && fwide(file, 0) > 0);
    CHECK(fgetws(line, 8, file) == line);
    CHECK(line[0] == L'c' && line[1] == L'd' && line[2] == 0 && line[3] == L'!' &&
          line[4] == 0);
    CHECK(feof(file) && !ferror(file));
    line[0] = L'X';
    CHECK(fgetws(line, 8, file) == NULL && line[0] == L'X');
    CHECK(ungetwc(L'p', file) == L'p' && !feof(file) && ftell(file) == 7);
    CHECK(fgetwc(file) == L'p' && fgetwc(file) == WEOF && feof(file));
    clearerr(file);
    CHECK(!feof(file) && !ferror(file));
    CHECK(ungetwc(L'x', file) == L'x');
    CHECK(fseek(file, 0, SEEK_CUR) == 0 && ftell(file) == 7);
    CHECK(fgetwc(file) == WEOF && feof(file));
    CHECK(fclose(file) == 0);

    file = tmpfile();
    CHECK(file != NULL && fputws(L"", file) >= 0 && fwide(file, 0) > 0);
    CHECK(fclose(file) == 0);
    file = tmpfile();
    CHECK(file != NULL && fputs("", file) >= 0 && fwide(file, 0) < 0);
    CHECK(fwide(file, 10) < 0 && fclose(file) == 0);
    file = tmpfile();
    CHECK(file != NULL && fprintf(file, "%s", "") == 0 && fwide(file, 0) < 0);
    CHECK(fclose(file) == 0);
    file = tmpfile();
    CHECK(file != NULL && fscanf(file, "%n", &(int){-1}) == 0 && fwide(file, 0) < 0);
    CHECK(fclose(file) == 0);
    file = tmpfile();
    CHECK(file != NULL && fgets(bytes, 1, file) == bytes && fwide(file, 0) < 0);
    CHECK(fclose(file) == 0);
    file = tmpfile();
    CHECK(file != NULL && fwide(file, -42) < 0 && fwide(file, 1) < 0);
    CHECK(fclose(file) == 0);

    CHECK(tmpnam(name) == name);
    file = fopen(name, "wx");
    CHECK(file != NULL && fwide(file, 0) == 0);
    CHECK(fputwc(L'A', file) == L'A');
    errno = 0;
    CHECK(fputwc((wchar_t)0xe9, file) == WEOF && errno == EILSEQ);
    CHECK(ferror(file) && !feof(file));
    CHECK(fputwc(L'\0', file) == 0 && fputws(L"B\n", file) >= 0);
    CHECK(freopen(name, "r", file) == file && fwide(file, 0) == 0);
    CHECK(fread(bytes, 1, sizeof(bytes), file) == 4 && fwide(file, 0) < 0);
    CHECK(memcmp(bytes, "A\0B\n", 4) == 0);
    CHECK(freopen(name, "w", file) == file && fwide(file, 0) == 0);
    CHECK(fputc('a', file) == 'a' && fputc(0x80, file) == 0x80);
    CHECK(fputc('b', file) == 'b');
    CHECK(freopen(name, "r", file) == file && fwide(file, 0) == 0);
    errno = 0;
    CHECK(fgetws(line, 8, file) == NULL && errno == EILSEQ);
    CHECK(ferror(file) && !feof(file));
    clearerr(file);
    CHECK(fgetwc(file) == L'b' && !ferror(file));
    CHECK(fputwc(L'c', file) == WEOF && ferror(file) && errno == EBADF);
    CHECK(fclose(file) == 0 && remove(name) == 0);
    return 0;
}

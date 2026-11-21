#define SMILE u8"\U0001f642"

static const char bytes[] = "\u00a3" SMILE "\u0040";

_Static_assert(sizeof bytes == 8, u8"UTF-8 concatenation includes one terminator");
_Static_assert('\u0040' == '@', "allowed basic-range UCN");

int main(void) {
    return (unsigned char)bytes[0] != 0xc2 || (unsigned char)bytes[1] != 0xa3 ||
           (unsigned char)bytes[2] != 0xf0 || (unsigned char)bytes[3] != 0x9f ||
           (unsigned char)bytes[4] != 0x99 || (unsigned char)bytes[5] != 0x82 ||
           bytes[6] != '@' || bytes[7] != 0;
}

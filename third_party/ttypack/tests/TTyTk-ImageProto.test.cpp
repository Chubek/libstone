#include <TTyTk/TTyTk-ImageProto.hpp>
#include <cassert>
#include <vector>
int main() {
    using namespace ttytk::image;
    const std::vector<ttytk::color::Rgb> pixels{{255, 0, 0}, {0, 255, 0}, {255, 0, 0}, {0, 255, 0}};
    assert(Kitty::encode({pixels.data(), 1}, 1, 1) == "\x1b_Ga=T,f=24,s=1,v=1,m=0;/wAA\x1b\\");
    assert(Kitty::encode(pixels, 1, 1).empty());
    assert(Sixel::encode(pixels, 2, 2) == "\x1bPq\"1;1;2;2#0;2;100;0;0#1;2;0;100;0#0B$#1?B\x1b\\");
    assert(ITerm2::encode(pixels, 2, 2).starts_with("\x1b]1337;File=inline=1;size=70:Qk"));
    assert(ITerm2::encode_file("test") == "\x1b]1337;File=inline=1;size=4:dGVzdA==\x07");
    const std::vector<ttytk::color::Rgb> many(1025);
    const auto chunked = Kitty::encode(many, 1025, 1);
    assert(chunked.find("m=1;") + 4 + 4096 == chunked.find("\x1b\\"));
    assert(chunked.ends_with("\x1b_Gm=0;AAAA\x1b\\"));
}

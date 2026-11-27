#include <TTyTk/TTyTk-InputProto.hpp>

#include <cassert>

int main() {
    const auto key = ttytk::input::decode_seq("\x1B[97;5u");
    assert(key);
    const auto* decoded_key = std::get_if<ttytk::input::Key>(&key.value());
    assert(decoded_key != nullptr);
    assert(decoded_key->codepoint == U'a');
    assert(decoded_key->modifiers == 4);

    const auto mouse = ttytk::input::decode_seq("\x1B[<0;12;4M");
    assert(mouse);
    const auto* decoded_mouse = std::get_if<ttytk::input::Mouse>(&mouse.value());
    assert(decoded_mouse != nullptr);
    assert(decoded_mouse->pressed && decoded_mouse->x == 12 && decoded_mouse->y == 4);

    const auto paste = ttytk::input::decode_seq("\x1B[200~hello\x1B[201~");
    assert(paste);
    assert(std::get<ttytk::input::Paste>(paste.value()).text == "hello");

    using namespace ttytk::input;
    const auto extended = decode_seq("\x1B[97:65:113;6:2;65:769u");
    assert(extended);
    const auto& k = std::get<Key>(extended.value());
    assert(k.codepoint == U'a' && k.shifted == U'A' && k.base_layout == U'q');
    assert(k.event_type == 2 && k.modifiers == 5 && k.text == U"A\u0301");
    assert(encode_seq(k).value() == "\x1B[97:65:113;6:2;65:769u");
    const auto text = decode_seq("\x1B[0;;229u");
    assert(text && std::get<Key>(text.value()).text == U"\u00E5");
    assert(std::get<Key>(decode_seq("\xC3\xA5").value()).codepoint == U'\u00E5');
    assert(std::get<Key>(decode_seq("\x1BOA").value()).codepoint == 57352);
    assert(std::get<Key>(decode_seq("\x9B" "1;5A").value()).modifiers == 4);
    assert(!decode_seq("\x1B[1;5R")); // Cursor position report, not F3.
    assert(encode_seq(Key{57364}).value() == "\x1BOP");
    assert(encode_seq(Key{57366}).value() == "\x1B[13~");
    assert(encode_seq(Key{57352, 4, 3}).value() == "\x1B[1;5:3A");
    assert(!decode_seq("\x1B[55296u"));
    assert(!decode_seq("\x1B[97;1:4u"));
    assert(!decode_seq("\x1B[97;;10u"));
    assert(!decode_seq("\x1B[97:65:u"));

    const auto drag = decode_seq("\x1B[<44;5;6M");
    const auto& d = std::get<Mouse>(drag.value());
    assert(d.motion && d.pressed && d.modifiers == 12 && d.button == 0);
    assert(encode_seq(d).value() == "\x1B[<44;5;6M");
    const auto hover = decode_seq("\x1B[<35;5;6M");
    const auto& h = std::get<Mouse>(hover.value());
    assert(h.motion && !h.pressed && h.button == 3);
    assert(encode_seq(h).value() == "\x1B[<35;5;6M");
    const auto release = decode_seq("\x1B[<128;5;6m");
    const auto& r = std::get<Mouse>(release.value());
    assert(!r.motion && !r.pressed && r.button == 128);
    assert(encode_seq(r).value() == "\x1B[<128;5;6m");
    assert(!decode_seq("\x1B[<0;0;6M"));
    assert(!decode_seq("\x1B[<0;5;6;7M"));
    assert(encode_seq(Paste{"first\nsecond"}).value() == "\x1B[200~first\nsecond\x1B[201~");
    assert(!decode_seq("\x1B[200~unfinished"));
    assert(!encode_seq(Paste{"\x1B[201~"}));
}

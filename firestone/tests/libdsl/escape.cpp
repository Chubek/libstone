#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "LibDSL.hpp"

namespace
{
using namespace dsl::terminal;
using namespace dsl::terminal::escape;

template <class F> void rejects(F test)
{
  try { test(); }
  catch (const std::invalid_argument &) { return; }
  assert(false && "expected invalid_argument");
}

void test_width_and_safety()
{
  constexpr std::string_view combined = "e\xCC\x81"; // Decomposed é.
  constexpr std::string_view family = "👨‍👩‍👧‍👦";
  constexpr std::string_view flags = "🇩🇪🇺🇸";
  constexpr std::string_view keycap = "#️⃣";
  assert(display_width(combined) == 1);
  assert(display_width("字") == 2);
  assert(display_width(family) == 2 && glyphs(family).size() == 1);
  assert(display_width(flags) == 4 && glyphs(flags).size() == 2);
  assert(display_width(keycap) == 2 && glyphs(keycap).size() == 1);
  assert(display_width("क्‍ष") == 1 && glyphs("क्‍ष").size() == 1);
  assert(display_width("\xE2\x82") == 1);
  assert(sanitize_text("\xE2\x82") == "\xEF\xBF\xBD");
  assert(sanitize_text("\xC0\x9B") == "\xEF\xBF\xBD\xEF\xBF\xBD");
  constexpr char dirty_bytes[] = "a\0b\x1b[c\xC2\x9D" "d\x7f\t\r\ne\xE2\x80\xAE";
  const std::string_view dirty{dirty_bytes, sizeof(dirty_bytes) - 1};
  assert(sanitize_text(dirty) == "ab[cde");
  assert(sanitize_text(family) == family);
  const auto end = next_glyph(family, std::numeric_limits<std::size_t>::max());
  assert(end.begin == family.size() && end.end == family.size() && end.width == 0);
  const auto second = next_glyph(std::string_view("a字"), 1);
  assert(second.begin == 1 && second.end == 4 && second.width == 2);
}

void test_styles()
{
  Style Red;
  Red.foreground = Color{255, 0, 0};
  Red.bold = true;
  StyledText value;
  value.span("one", Red).text(" two").line_break();
  const auto parsed = StyledText::parse(R"((styled (span (bold) (fg #ff0000) "one")
      (text " two") (line)))");
  for (const auto mode : {RenderMode::true_color, RenderMode::color256, RenderMode::ansi,
       RenderMode::grayscale, RenderMode::monochrome, RenderMode::plain, RenderMode::debug})
    assert(value.render(mode) == parsed.render(mode));
  assert(value.render(RenderMode::true_color) == "\x1b[1;38;2;255;0;0mone\x1b[0m two\n");
  assert(value.render(RenderMode::monochrome) == "\x1b[1mone\x1b[0m two\n");
  assert(value.render() == "one two\n");
  assert(value.render(RenderMode::debug) == "[style fg=#ff0000 bold]one[/style] two\\n");
  assert(value.render(RenderMode::grayscale).find("\x1b[1;38;5;") == 0);
  RenderCapabilities caps;
  caps.true_color = false;
  assert(value.render(RenderMode::true_color, caps) == value.render(RenderMode::color256));
  caps.color256 = false;
  assert(value.render(RenderMode::true_color, caps) == value.render(RenderMode::ansi));
  caps.ansi = false;
  assert(value.render(RenderMode::true_color, caps) == value.render(RenderMode::monochrome));
  caps.attributes = false;
  assert(value.render(RenderMode::true_color, caps) == value.render(RenderMode::plain));

  StyledText mixed;
  mixed.text("e").span("\xCC\x81", Red).span("字", Red).text("z");
  const auto wrapped = mixed.wrap(2);
  assert(mixed.width() == 4);
  assert(wrapped.width() == 2);
  assert(wrapped.render() == "e\xCC\x81\n字\nz");
  assert(wrapped.render(RenderMode::true_color).find("\x1b[1;38;2;255;0;0m字") != std::string::npos);
  rejects([&] { (void)mixed.wrap(0); });
  rejects([] { (void)StyledText::parse(R"((styled (span (fg 0 256 0) "x")))"); });
  rejects([] { (void)StyledText::parse(R"((styled (span (bold) (bold) "x")))"); });
  Red.foreground->red = -1;
  rejects([&] { value.span("", Red); });
}

void test_operations()
{
  Style outer, inner;
  outer.foreground = Color{255, 0, 0};
  outer.bold = true;
  inner.foreground = Color{0, 0, 255};
  inner.underline = true;
  Sequence native, body, nested;
  nested.text("B");
  body.text("A").styled(inner, nested).text("C");
  native.cursor_to(3, 5).cursor_move(csi::Direction::left, 2)
    .erase(csi::EraseTarget::line, csi::EraseMode::all)
    .styled(outer, body).title("a\nb").hyperlink("https://example.com", Sequence{}.text("site"));
  const auto parsed = Sequence::parse(R"((seq (cursor 3 5) (move left 2) (erase line all)
    (style (fg 255 0 0) (bold) (seq (text "A")
        (style (underline) (fg #0000ff) (text "B")) (text "C")))
    (title "a\nb") (link "https://example.com" (text "site"))))");
  for (const auto mode : {RenderMode::true_color, RenderMode::color256, RenderMode::ansi,
       RenderMode::grayscale, RenderMode::monochrome, RenderMode::plain, RenderMode::debug})
    assert(native.render(mode) == parsed.render(mode));
  assert(native.render() == "ABCsite" && native.width() == 7);
  assert(native.render(RenderMode::true_color)
    == "\x1b[3;5H\x1b[2D\x1b[2K\x1b[1;38;2;255;0;0mA"
       "\x1b[1;4;38;2;0;0;255mB\x1b[0m\x1b[1;38;2;255;0;0mC\x1b[0m"
       "\x1b]2;ab\x1b\\\x1b]8;;https://example.com\x1b\\site\x1b]8;;\x1b\\");
  assert(native.render(RenderMode::debug).find('\x1b') == std::string::npos);
  const auto links = Sequence::parse(R"((link "https://outer.example" (seq
    (text "a") (link "https://inner.example" (text "b")) (text "c"))))");
  assert(links.render(RenderMode::monochrome)
    == "\x1b]8;;https://outer.example\x1b\\a\x1b]8;;https://inner.example\x1b\\b"
       "\x1b]8;;https://outer.example\x1b\\c\x1b]8;;\x1b\\");
  RenderCapabilities absent;
  absent.cursor = absent.erase = absent.title = absent.hyperlinks = false;
  assert(native.render(RenderMode::monochrome, absent) == "\x1b[1mA\x1b[1;4mB\x1b[0m\x1b[1mC\x1b[0msite");
  Sequence raw;
  raw.raw("\x1bP\x9C", RawPermission::trusted_terminal_bytes);
  assert(raw.render().empty());
  assert(raw.render(RenderMode::debug) == "[raw\\x1B\\x50\\x9C]");
  rejects([&] { (void)raw.render(RenderMode::true_color); });
  absent.raw = true;
  assert(raw.render(RenderMode::true_color, absent) == "\x1bP\x9C");

  Sequence fragments;
  fragments.text("a").text("b").line_break().text("字");
  const auto flat = fragments.normalized();
  std::size_t visits = 0;
  flat.visit([&](const auto &) { ++visits; });
  assert(visits == 3 && flat.render() == fragments.render() && flat.width() == 2);
  rejects([] { Sequence{}.cursor_to(0, 1); });
  rejects([] { Sequence{}.cursor_move(static_cast<csi::Direction>(-1)); });
  rejects([] { (void)Sequence::parse("(cursor 1 9223372036854775808)"); });
  rejects([] { (void)Sequence::parse(R"((link "https://example.com\n" (text "x")))"); });
  rejects([] { (void)Sequence::parse(R"((link "file:///etc/passwd" (text "x")))"); });
  rejects([] { (void)Sequence::parse(R"((raw "x"))"); });
  rejects([] { (void)Sequence::parse(R"((erase display scrollback))"); });
  auto malformed = dsl::parse_sexpr(R"((text "x"))");
  malformed.items.back().items.push_back(dsl::SExpr::atom("unexpected"));
  rejects([&] { (void)Sequence::from_sexpr(malformed); });
}
} // namespace

int main()
{
  struct EndUser : dsl::DSL<EndUser, dsl::GlobalEscapeCodes> {};
  assert(EndUser::escape_sequence(R"((text "hello"))").render() == "hello");
  test_width_and_safety();
  test_styles();
  test_operations();
}

#include "LibDSL.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

namespace
{
using namespace dsl::terminal;
namespace ti = dsl::terminal::terminfo;
using namespace std::string_view_literals;

void bytes (std::string& out, std::int32_t n, unsigned width = 2)
{
  for (unsigned i = 0; i < width; ++i)
    out.push_back (static_cast<char> ((static_cast<std::uint32_t> (n) >> (8 * i)) & 255));
}

std::string binary (bool wide)
{
  std::string b;
  for (auto n : { wide ? 01036 : 0432, 4, 3, 3, 3, 4 }) bytes (b, n);
  b.append ("a|b\0\1\xfe\0\0"sv);
  for (auto n : { wide ? 72000 : 80, -1, -2 }) bytes (b, n, wide ? 4 : 2);
  for (auto n : { 0, -1, -2 }) bytes (b, n);
  b.append ("abc\0"sv);
  return b;
}

std::string extended (std::string b, bool wide)
{
  for (auto n : { 1, 1, 2, 5, 16 }) bytes (b, n);
  b.append ("\1\0"sv);
  bytes (b, wide ? 1'000'000 : 1234, wide ? 4 : 2);
  for (auto n : { 0, -2, 0, 3, 6, 9 }) bytes (b, n);
  b.append ("xyz\0EF\0EN\0ES\0EC\0"sv);
  return b;
}

struct TerminalLanguage : dsl::DSL<TerminalLanguage, dsl::Terminfo> {};

void adapter ()
{
  static_assert (std::is_same_v<TerminalLanguage::TerminfoEntry, ti::Entry>);
  static_assert (std::is_same_v<TerminalLanguage::InputDecoder, InputDecoder>);
  auto entry = TerminalLanguage::load_terminfo (binary (false));
  assert (entry.is_ok () && entry.unwrap ().num ("cols") == 80);
  assert (TerminalLanguage::load_terminfo ("invalid").is_err ());
}

void entries ()
{
  for (bool wide : { false, true })
    {
      auto base = binary (wide);
      assert (ti::load_from (base).is_ok ());
      auto b = extended (base, wide);
      auto parsed = ti::load_from (b);
      assert (parsed.is_ok ());
      auto e = parsed.unwrap ();
      assert (e.names == "a|b" && e.flag ("bw") && !e.flag ("am"));
      assert (e.num ("cols") == (wide ? 72000 : 80) && e.str ("cbt") == "abc");
      assert (e.flag_state ("am") == ti::SlotState::cancelled);
      assert (e.num_state (1) == ti::SlotState::absent);
      assert (e.num_state (2) == ti::SlotState::cancelled);
      assert (e.str_state (2) == ti::SlotState::cancelled);
      assert (e.str_state (4000) == ti::SlotState::absent);
      assert (e.flag ("EF") && e.num ("EN") == (wide ? 1'000'000 : 1234) && e.str ("ES") == "xyz");
      assert (!e.str ("EC") && e.str_state ("EC") == ti::SlotState::cancelled);
      for (std::size_t n = 0; n < b.size (); ++n)
        if (n != base.size ()) assert (ti::load_from (std::string_view (b).substr (0, n)).is_err ());

      auto bad = base;
      bad.back () = 'x'; // unterminated string
      assert (ti::load_from (bad).is_err ());
      bad = base;
      bad[19] = 1; // incorrect alignment byte
      assert (ti::load_from (bad).is_err ());
      bad = base;
      bad[2] = bad[3] = '\xff'; // negative names size
      assert (ti::load_from (bad).is_err ());
      bad = base;
      bad[20 + 3 * (wide ? 4 : 2)] = 4; // one-past string table
      assert (ti::load_from (bad).is_err ());
      bad = b;
      bad[base.size () + 6] = 6; // inconsistent extended item count
      assert (ti::load_from (bad).is_err ());
      bad = b;
      bad.back () = 'x'; // unterminated extended name
      assert (ti::load_from (bad).is_err ());
    }
  assert (ti::load ("../etc/passwd").is_err ());
  assert (ti::load ("x\\other").is_err ());
  assert (ti::load ("vt100", {}).is_err ());
}

void expansion ()
{
  assert (ti::tparm ("\x1b[%i%p1%d;%p2%dH", 1, 2).unwrap () == "\x1b[2;3H");
  assert (ti::tparm ("%?%p1%{8}%<%tlo%e%p1%{16}%<%tmid%ehi%;", 12).unwrap () == "mid");
  assert (ti::tparm ("%?%p1%t%?%p2%ta%eb%;%ec%;", 0, 1).unwrap () == "c");
  assert (ti::tparm ("%?%p1%t%?%p2%ta%eb%;%ec%;", 1, 0).unwrap () == "b");
  assert (ti::tparm ("%p1%:-6.3s/%p2%:#06x/%p1%l%d/%%", "hello", 255).unwrap () == "hel   /0x00ff/5/%");
  assert (ti::tparm ("%'z'%Pa%ga%c").unwrap () == "z");
  assert (ti::tparm ("%{9223372036854775807}%{1}%+%d").unwrap () == "-9223372036854775808");
  assert (ti::tparm ("%{-9223372036854775808}%{-1}%/%d").unwrap () == "-9223372036854775808");
  assert (ti::tparm ("%{-9223372036854775808}%{-1}%m%d").unwrap () == "0");
  for (auto pattern : { "%", "%p0", "%{a}", "%d", "%{1", "%'x", "%po", "%q",
                        "%?%{1}%tduh", "%?%e%;", "%999999s", "%:%n", "%{1}%s" })
    assert (ti::tparm (pattern).is_err ());
  const std::string full (65536, '.');
  assert (ti::tparm ("%p1%s%p1%s", full).is_err ());
  assert (ti::tparm ("%p1%s", static_cast<const char*> (nullptr)).is_err ());
  ti::ParameterState state;
  assert (ti::expand ("%p1%PA", { "owned"sv }, &state).is_ok ());
  assert (ti::expand ("%p1%PA%d", { "do not commit"sv }, &state).is_err ());
  assert (ti::expand ("%gA%s", {}, &state).unwrap () == "owned");
  assert (ti::expand ("%gA%d").unwrap () == "0");
  auto parts = ti::padding ("abc$<2.5*/>def$<1>ghi", 3).unwrap ();
  assert (parts.size () == 5 && std::get<std::string> (parts[2]) == "def");
  auto p = std::get<ti::Padding> (parts[1]);
  assert (p.delay.count () == 7500 && p.proportional && p.mandatory);
  assert (std::get<ti::Padding> (parts[3]).delay.count () == 1000);
  for (auto invalid : { "$<>", "$<.>", "$<4", "$<2**>", "$<abc>", "$<100000000>", "$<10001*>", "$<6000>$<6000>" })
    assert (ti::padding (invalid, 4).is_err ());
}

void input ()
{
  InputDecoder decoder (16);
  assert (!decoder.escape_pending ());
  assert (decoder.feed ("\x1b").empty ());
  assert (decoder.escape_pending ());
  assert (decoder.flush ()[0].key == Key::escape);
  assert (!decoder.escape_pending ());
  assert (decoder.feed ("\x1b[").empty ());
  assert (!decoder.escape_pending ());
  auto events = decoder.feed ("1;5A\x1bOP\x1b[Z\r\x7f\x03");
  assert (events.size () == 6 && events[0].key == Key::up && events[0].modifiers == ctrl);
  assert (events[1].key == Key::function && events[1].code == 1);
  assert (events[2].key == Key::tab && events[2].modifiers == shift);
  assert (events[3].key == Key::enter && events[4].key == Key::backspace);
  assert (events[5].key == Key::control && events[5].code == 3);
  assert (decoder.feed ("\x1b\xf0\x9f").empty ());
  assert (!decoder.escape_pending ());
  events = decoder.feed ("\x8c\x8d");
  assert (events.size () == 1 && events[0].text == "\xf0\x9f\x8c\x8d" && events[0].modifiers == alt);
  events = decoder.feed ("\xff\xc2X");
  assert (events.size () == 3 && events[0].text == "\xef\xbf\xbd" && events[2].text == "X");
  assert (decoder.feed ("\x1b[200~a\x1b[A\0\x1b[20"sv).empty ());
  assert (!decoder.escape_pending ());
  events = decoder.feed ("1~b");
  assert (events.size () == 2 && events[0].key == Key::paste && events[0].text == "a\x1b[A\0"sv);
  assert (events[1].text == "b");
  events = decoder.feed ("\x1b[200~abcdefghijklmnopq\x1b[201~");
  assert (events.size () == 1 && events[0].key == Key::unknown && events[0].text.empty ());
  events = decoder.feed ("\x1b]0;no text\x07\x1b[<36;10;20M\x1b[I\x1b[O");
  assert (events.size () == 4 && events[0].key == Key::unknown && events[1].key == Key::mouse);
  assert (events[1].column == 10 && events[1].row == 20 && events[1].motion && events[1].modifiers == shift);
  assert (events[2].key == Key::focus_in && events[3].key == Key::focus_out);
}

void transport ()
{
  Backend memory;
  auto caps = discover_capabilities (memory, [] () -> dsl::Result<ti::Entry, std::string> {
    assert (false); // do not load for noninteractive transport
    return ti::Entry{};
  });
  assert (!caps.interactive && !caps.entry);
  memory.interactive = [] { return true; };
  caps = discover_capabilities (memory, [] { return ti::load_from (extended (binary (true), true)); });
  assert (caps.interactive && caps.entry && caps.entry->flag ("EF"));
  WindowSize size{ 20, 50 };
  memory.size = [&] { return std::optional (size); };
  ResizeMonitor monitor;
  assert (monitor.observe (memory) == size && !monitor.observe (memory));
  size.rows = 21;
  assert (monitor.observe (memory) == size);
  assert (mode_sequence (PrivateMode::bracketed_paste, false) == "\x1b[?2004l");
  assert (mode_sequence (static_cast<PrivateMode> (0), true).empty ());
  RawMode invalid (-1);
  assert (!invalid.active () && invalid.error () && !invalid.restore ());
#if LIBDSL_LIVE_POSIX
  const int master = ::posix_openpt (O_RDWR | O_NOCTTY);
  if (master < 0) return;
  assert (::grantpt (master) == 0 && ::unlockpt (master) == 0);
  const char* name = ::ptsname (master);
  assert (name);
  const int slave = ::open (name, O_RDWR | O_NOCTTY);
  assert (slave >= 0 && is_terminal (slave));
  ::termios before{}, after{};
  assert (::tcgetattr (slave, &before) == 0);
  {
    RawMode raw (slave);
    assert (raw.active () && !raw.error ());
    assert (::tcgetattr (slave, &after) == 0 && !(after.c_lflag & (ECHO | ICANON | ISIG)));
    auto endpoint = descriptor_backend (slave, slave);
    assert (endpoint.interactive ());
    char input_buffer[32];
    assert (endpoint.read (input_buffer, std::chrono::milliseconds (0)).status == IOStatus::timeout);
    assert (::write (master, "x", 1) == 1);
    auto read = endpoint.read (input_buffer, std::chrono::milliseconds (50));
    assert (read.status == IOStatus::data && read.bytes == 1 && input_buffer[0] == 'x');
    auto written = endpoint.write ("y");
    assert (written.status == IOStatus::data && written.bytes == 1);
    assert (::read (master, input_buffer, 1) == 1 && input_buffer[0] == 'y');
    ::winsize requested{ 40, 120, 200, 100 };
    assert (::ioctl (slave, TIOCSWINSZ, &requested) == 0);
    assert ((endpoint.size () == WindowSize{ 40, 120, 200, 100 }));
  }
  assert (::tcgetattr (slave, &after) == 0 && before.c_lflag == after.c_lflag);
  assert (before.c_iflag == after.c_iflag && before.c_oflag == after.c_oflag);
  ::close (slave);
  ::close (master);
#else
  assert (!descriptor_backend (0, 1).read && !window_size (0));
#endif
}

void host ()
{
  const std::vector<std::string> roots{ "/etc/terminfo", "/lib/terminfo", "/usr/share/terminfo" };
  const auto exists = [&] (std::string_view name, std::string_view hex) {
    for (const auto& root : roots)
      for (auto dir : { name.substr (0, 1), hex })
        if (std::filesystem::is_regular_file (std::filesystem::path (root) / dir / name))
          return true;
    return false;
  };
  if (exists ("vt100", "76"))
    {
      const auto vt100 = ti::load ("vt100", roots);
      assert (vt100.is_ok ());
      auto e = vt100.unwrap ();
      assert (e.flag ("am") && e.str ("cup"));
      auto rendered = ti::tparm (*e.str ("cup"), 1, 2);
      assert (rendered.is_ok () && rendered.unwrap ().starts_with ("\x1b[2;3H"));
      assert (ti::padding (rendered.unwrap ()).is_ok ());
    }
  if (exists ("xterm-256color", "78"))
    {
      const auto xterm = ti::load ("xterm-256color", roots);
      assert (xterm.is_ok ());
      auto e = xterm.unwrap ();
      assert (e.num ("colors") == 256 && e.str ("setaf"));
      assert (ti::tparm (*e.str ("setaf"), 33).unwrap () == "\x1b[38;5;33m");
    }
}
} // namespace

int main ()
{
  adapter ();
  entries ();
  expansion ();
  input ();
  transport ();
  host ();
}

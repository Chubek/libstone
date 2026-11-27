#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cctype>
#include <cstdio>
#include "LibDSL.hpp"

namespace
{
using namespace dsl::repl;
using dsl::terminal::Key;

struct MemoryIO
{
  std::string data, output{};
  std::size_t position = 0, read_chunk = 2, write_chunk = 3;
  std::size_t read (std::span<std::byte> bytes)
  {
    const auto size = std::min ({ data.size () - position, bytes.size (), read_chunk });
    if (size)
      std::memcpy (bytes.data (), data.data () + position, size);
    position += size;
    return size;
  }
  std::size_t write (std::span<const std::byte> bytes)
  {
    const auto size = std::min (bytes.size (), write_chunk);
    output.append (reinterpret_cast<const char *> (bytes.data ()), size);
    return size;
  }
};

void editor_test ()
{
  Editor edit ({ 32, 2, 8, 2 });
  const std::string accent = "e\xcc\x81";
  const std::string wide = "\xe7\x8c\xab";
  const auto original = accent + wide + "\nAB";
  assert (edit.insert (original));
  assert (edit.move_line (false));
  assert (edit.cursor () == accent.size ());
  assert (!edit.move_to (1));
  edit.set_mark ();
  assert (edit.move_left ());
  assert ((edit.selection () == std::pair<std::size_t, std::size_t>{ 0, accent.size () }));
  assert (edit.kill (0, accent.size ()));
  assert (edit.text () == wide + "\nAB");
  assert (edit.undo () && edit.text () == original && edit.mark () == accent.size ());
  assert (edit.redo () && edit.text () == wide + "\nAB");
  assert (edit.yank () && edit.text () == original);
  assert (!edit.replace (1, 2, "x"));
  assert (!edit.insert (std::string (33, 'z')));
  assert (edit.text () == original);

  assert (edit.remember ("first"));
  assert (edit.remember ("second\nline"));
  assert (edit.remember ("third"));
  assert (edit.history ().size () == 2);
  assert (edit.move_to (accent.size ()));
  assert (edit.history_previous () && edit.text () == "third");
  assert (edit.history_previous () && edit.text () == "second\nline");
  assert (edit.history_next () && edit.text () == "third");
  assert (edit.history_next () && edit.text () == original && edit.cursor () == accent.size ());

  MemoryIO buffer{};
  dsl::IO io (buffer);
  edit.save_history (io);
  buffer.data = buffer.output;
  Editor loaded;
  loaded.load_history (io);
  assert (loaded.history () == edit.history ());
  buffer.data.pop_back ();
  buffer.position = 0;
  bool threw = false;
  try { loaded.load_history (io); }
  catch (const std::runtime_error &) { threw = true; }
  assert (threw && loaded.history () == edit.history ());

  Editor ring;
  assert (ring.insert ("old,new"));
  assert (ring.kill (0, 3, false) && ring.kill (4, 7, false));
  ring.clear ();
  assert (ring.yank () && ring.text () == "new");
  assert (ring.yank (true) && ring.text () == "old");
  assert (ring.move_left () && !ring.yank (true));
}

dsl::live::SyntaxStatus continuation (std::string_view source)
{
  using S = dsl::live::SyntaxStatus;
  if (source.starts_with ("!"))
    return S::invalid;
  return source.starts_with ("(") && !source.ends_with (")") ? S::incomplete : S::complete;
}

void session_test ()
{
  std::vector<std::string> calls;
  Session session ({ continuation, [] (std::string_view text, std::size_t)
    {
      return std::vector<Completion>{ { 0, text.size (), "hello", "label" },
                                      { 0, text.size () + 1, "bad", {} } };
    } }, [&] (std::string_view text)
    {
      calls.emplace_back (text);
      return Evaluation{ true, "\x1b[2Janswer" };
    });
  assert (session.editor ().insert ("("));
  assert (session.syntax_status () == dsl::live::SyntaxStatus::incomplete);
  assert (session.completions ().size () == 1 && calls.empty ());
  assert (session.submit ().status == SubmitStatus::continued);
  assert (session.editor ().insert (")"));
  assert (session.submit ().status == SubmitStatus::evaluated && calls.back () == "(\n)");
  assert (session.editor ().insert ("!"));
  assert (session.submit ().status == SubmitStatus::rejected && calls.size () == 1);
  assert (session.handle ({ Key::control, {}, dsl::terminal::ctrl, 3 }).interrupted);
  assert (session.editor ().text ().empty ());

  auto &keymap = session.keymap ();
  assert (keymap.bind (KeyMode::insert, { { Key::text, "g" }, { Key::text, "o" } },
                       Command::complete));
  assert (!keymap.bind (KeyMode::insert, { { Key::text, "g" } }, Command::left));
  session.handle ({ Key::text, "g" });
  assert (session.editor ().text ().empty ());
  session.handle ({ Key::text, "o" });
  assert (session.editor ().text () == "hello");
  session.cancel ();
  session.set_key_style (KeyStyle::vi);
  session.handle ({ Key::text, "A" });
  session.handle ({ Key::escape });
  assert (session.key_mode () == KeyMode::normal);
  session.handle ({ Key::text, "d" });
  session.handle ({ Key::text, "d" });
  assert (session.editor ().text ().empty ());
  session.handle ({ Key::text, "u" });
  assert (session.editor ().text () == "A");
  session.handle ({ Key::text, "a" });
  session.handle ({ Key::text, "B" });
  assert (session.editor ().text () == "AB");
  session.handle ({ Key::paste, "\x03\n\x1b[31m" });
  assert (calls.size () == 1);

  std::stop_source cancelled;
  cancelled.request_stop ();
  assert (session.submit (cancelled.get_token ()).status == SubmitStatus::rejected);
  session.set_cancellable_evaluator ([n = 0] (std::string_view, std::stop_token token) mutable
    {
      assert (token.stop_possible ());
      return Evaluation{ true, std::to_string (++n) };
    });
  std::stop_source running;
  assert (session.submit (running.get_token ()).result.output == "1");
  session.editor ().insert ("repeat");
  assert (session.submit (running.get_token ()).result.output == "2");

  Session indented ({ continuation, {}, [] (std::string_view source, std::size_t cursor)
    {
      assert (source.substr (0, cursor).starts_with ("("));
      return 2;
    } }, [] (std::string_view) { return Evaluation{}; });
  indented.set_auto_indent (true);
  indented.editor ().insert ("(");
  assert (indented.submit ().status == SubmitStatus::continued);
  assert (indented.editor ().text () == "(\n  ");
  indented.cancel ();
  MemoryIO batch{ "(\n" };
  dsl::IO io (batch);
  assert (indented.run_lines (io) == RunStatus::unfinished_input);
  assert (indented.editor ().text () == "(\n");

  Session choices ({ {}, [](std::string_view, std::size_t)
    { return std::vector<Completion>{ { 0, 0, "a", "\x1b[2J" }, { 0, 0, "b", {} } }; } });
  assert (choices.handle ({ Key::tab }).notice == "[2J\nb\n");
  assert (choices.editor ().text ().empty ());
}

void streams_test ()
{
  std::vector<std::string> calls;
  Session session ({ continuation, {} }, [&] (std::string_view source)
    {
      calls.emplace_back (source);
      return Evaluation{ true, "safe\x1b[2J\nline" };
    });
  MemoryIO batch{ "(one\r\n\t)\ntwo" };
  dsl::IO io (batch);
  assert (session.run_lines (io) == RunStatus::eof);
  assert ((calls == std::vector<std::string>{ "(one\n\t)", "two" }));
  assert (batch.output == "safe[2J\nline\nsafe[2J\nline\n");

  MemoryIO interactive{ "\x1b[200~x\r\ny\x1b[201~\n\x04" };
  dsl::IO mock (interactive);
  unsigned rendered = 0;
  session.set_prompt ([] (std::string_view, std::size_t, bool)
    {
      dsl::terminal::Style style;
      style.bold = true;
      return std::vector<dsl::terminal::StyledText>{
        dsl::terminal::StyledText{}.span ("one", style), dsl::terminal::StyledText{}.text ("two> ") };
    });
  session.set_renderer ([&] (const EditView &view)
    {
      ++rendered;
      assert (view.visible_begin <= view.visible_end && view.visible_end <= view.document.size ());
      return dsl::terminal::StyledText{}.text (view.text);
    });
  assert (session.run (mock, { .redraw = true, .columns = 20, .rows = 3,
                              .color = dsl::terminal::RenderMode::ansi }) == RunStatus::stopped);
  assert (rendered != 0 && calls.back () == "x\r\ny");
  assert (interactive.output.find ("\x1b[1mone\x1b[0m\r\n") != std::string::npos);
  assert (interactive.output.find ("safe\x1b[2J") == std::string::npos);

  MemoryIO overflow{ "toolong\n" };
  dsl::IO limited (overflow);
  Session bounded ({}, [&] (std::string_view text)
    {
      calls.emplace_back (text);
      return Evaluation{};
    }, { .input_bytes = 3 });
  auto count = calls.size ();
  assert (bounded.run_lines (limited) == RunStatus::rejected && calls.size () == count);
}

#if LIBDSL_LIVE_POSIX
struct Fd
{
  int get;
  ~Fd () { if (get >= 0) ::close (get); }
};

bool term_equal (const termios &left, const termios &right)
{
  return left.c_iflag == right.c_iflag && left.c_oflag == right.c_oflag
         && left.c_cflag == right.c_cflag && left.c_lflag == right.c_lflag
         && std::equal (std::begin (left.c_cc), std::end (left.c_cc), std::begin (right.c_cc));
}

void pseudo_terminal_test ()
{
  Fd master{ ::posix_openpt (O_RDWR | O_NOCTTY) };
  assert (master.get >= 0 && ::grantpt (master.get) == 0 && ::unlockpt (master.get) == 0);
  auto path = ::ptsname (master.get);
  assert (path);
  Fd slave{ ::open (path, O_RDWR | O_NOCTTY) };
  assert (slave.get >= 0);
  termios before{}, now{};
  assert (::tcgetattr (slave.get, &before) == 0);
  winsize dimensions{};
  dimensions.ws_col = 20;
  dimensions.ws_row = 5;
  assert (::ioctl (slave.get, TIOCSWINSZ, &dimensions) == 0);
  Session session ({}, [] (std::string_view buffer)
    {
      assert (buffer == "hi");
      return Evaluation{};
    });
  session.set_key_style (KeyStyle::vi);
  bool sent = false, tail = false, narrow = false;
  auto send = [&] (std::string_view keys)
    { assert (::write (master.get, keys.data (), keys.size ()) == static_cast<ssize_t> (keys.size ())); };
  session.set_renderer ([&] (const EditView &view)
    {
      if (tail && view.document == "hi" && view.cursor_byte == 2)
        narrow = view.text == "i";
      return dsl::terminal::StyledText{}.text (view.text);
    });
  session.set_prompt ([&] (std::string_view, std::size_t, bool)
    {
      if (!sent)
        {
          assert (::tcgetattr (slave.get, &now) == 0 && !(now.c_lflag & ICANON));
          send ("hi\x1b");
          sent = true;
        }
      if (!tail && session.key_mode () == KeyMode::normal)
        {
          tail = true;
          dimensions.ws_col = 2;
          assert (::ioctl (slave.get, TIOCSWINSZ, &dimensions) == 0);
          send ("a\r\x04");
        }
      return std::vector<dsl::terminal::StyledText>{};
    });
  assert (session.run_terminal (slave.get, slave.get) == RunStatus::stopped);
  assert (sent && tail && narrow && ::tcgetattr (slave.get, &now) == 0 && term_equal (before, now));
  session.set_prompt ([] (std::string_view, std::size_t, bool) -> std::vector<dsl::terminal::StyledText>
    { throw std::runtime_error ("test failure inside raw mode"); });
  bool threw = false;
  try { session.run_terminal (slave.get, slave.get); }
  catch (const std::runtime_error &) { threw = true; }
  assert (threw && ::tcgetattr (slave.get, &now) == 0 && term_equal (before, now));
  auto transport = dsl::terminal::descriptor_backend (master.get, master.get);
  std::string terminal_output;
  std::array<char, 1024> bytes{};
  while (true)
    {
      auto read = transport.read (bytes, std::chrono::milliseconds (0));
      if (read.status != dsl::terminal::IOStatus::data)
        break;
      terminal_output.append (bytes.data (), read.bytes);
    }
  assert (terminal_output.find ("\x1b[?2004h") != std::string::npos);
  assert (terminal_output.ends_with ("\x1b[?2004l"));
}
#endif
} // namespace

int main ()
{
  editor_test ();
  session_test ();
  streams_test ();
#if LIBDSL_LIVE_POSIX
  pseudo_terminal_test ();
#endif
}

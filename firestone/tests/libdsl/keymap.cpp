#include "LibDSL.hpp"

#include <cassert>

int main ()
{
  using namespace dsl;
  using namespace dsl::repl;
  Session session;
  terminal::InputDecoder decoder;
  auto keys = [&] (std::string_view bytes)
    {
      for (const auto &event : decoder.feed (bytes))
        session.handle (event);
    };
  auto escape = [&]
    {
      keys ("\033");
      for (const auto &event : decoder.flush ())
        session.handle (event);
    };
  auto &editor = session.editor ();

  // A combining sequence is one glyph, and CRLF is traversed as one boundary.
  assert (editor.set_text ("e\xcc\x81" "_2, \r\nlast"));
  keys ("\033<\033f");
  assert (editor.cursor () == 5);
  keys ("\033f");
  assert (editor.cursor () == 6);
  keys ("\033d");
  assert (editor.text () == "e\xcc\x81" "_2,");
  keys ("\031"); // Ctrl+Y restores the kill.
  assert (editor.text () == "e\xcc\x81" "_2, \r\nlast");
  keys ("\033\177");
  assert (editor.text () == "e\xcc\x81" "_2, \r\n");
  keys ("\037\033>\033b");
  assert (editor.cursor () == 9);
  editor.set_mark ();
  keys ("\033<\007");
  assert (!editor.mark () && editor.cursor () == 0);
  keys ("\033b\033\177");
  assert (editor.cursor () == 0 && editor.text () == "e\xcc\x81" "_2, \r\nlast");
  editor.set_mark ();
  keys ("\033f\033d");
  assert (editor.text () == ", \r\nlast");
  keys ("\031");
  assert (editor.text () == "e\xcc\x81" "_2, \r\nlast");

  session.set_key_style (KeyStyle::vi);
  assert (editor.set_text (" \talpha beta\n  gamma"));
  escape ();
  assert (session.key_mode () == KeyMode::normal);
  keys ("g");
  assert (editor.cursor () == editor.text ().size () - 1);
  keys ("g^");
  assert (editor.cursor () == 2);
  keys ("w");
  assert (editor.cursor () == 8);
  keys ("db");
  assert (editor.text () == " \tbeta\n  gamma");
  keys ("dw");
  assert (editor.text () == " \tgamma");
  keys ("u");
  assert (editor.text () == " \tbeta\n  gamma");
  keys ("gG"); // Failed prefix replays its final stroke as a standalone key.
  assert (editor.cursor () == editor.text ().size ());
  keys ("I!");
  assert (session.key_mode () == KeyMode::insert);
  assert (editor.text () == " \tbeta\n  !gamma");
  escape ();
  keys ("A?");
  assert (editor.text () == " \tbeta\n  !gamma?");
  keys ("\027"); // Vi insert Ctrl+W kills the punctuation run.
  assert (editor.text () == " \tbeta\n  !gamma");
  escape ();
  keys ("X");
  assert (editor.text () == " \tbeta\n  !gama");
  keys ("ggY");
  assert (editor.text () == " \tbeta\n  !gama");
  keys ("Gp");
  assert (editor.text () == " \tbeta\n  !gama \tbeta\n");
  keys ("ggyydd");
  assert (editor.text () == "  !gama \tbeta\n");
}

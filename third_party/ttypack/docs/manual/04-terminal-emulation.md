\page manual_emulation Terminal Emulation

# Terminal Emulation

## Input protocols

`InputProto` decodes modern keyboard and pointer protocols, including Kitty
keyboard input, CSI-u, SGR mouse mode, and bracketed paste. The listener
headers add application-level dispatch:

- `KeyListener` binds keys, modifiers, and multi-key sequences;
- `MouseListener` tracks clicks, movement, dragging, and hit testing;
- `EventListener` unifies input, resize, and custom events.

Keep protocol decoding separate from actions. A key event should describe what
arrived; the application key map decides what it means.

## Terminal modes

`TermMode` models DEC private and ANSI modes such as alternate screen,
bracketed paste, mouse reporting, cursor keys, and autowrap. Save and restore
mode state around temporary application behavior.

`TermInfo` and `TermCap` provide capability discovery and database parsing.
Use terminfo when available for compiled capability data; use termcap for
legacy consumers or the explicit bridge. `CapTools` can issue runtime
queries such as DA, DSR, and DECRQSS when a database entry is insufficient.
The terminfo loader supports directory trees, not hashed databases.
Termcap preserves padding directives without timing the output.
Capability probes leave transport and response delivery to the caller.

## Virtual terminals

`VirtTools` is the headless terminal emulator path:

```cpp
ttytk::VirtualTerm terminal{24, 80};  // rows, columns
terminal.input(output_bytes);
auto& screen = terminal.screen();
```

The virtual terminal applies parser events to a `CellBuffer`, making it useful
for tests, log processors, recording playback, and remote-session servers.
It implements a VT/ANSI subset, including cursor, erase, scroll, rendition,
and alternate-screen operations. It does not emulate every xterm extension
or answer terminal queries. `output()` is the raw input transcript; use
`screen()` to read the rendered cell state.
`Terminal` in TTyUtils builds the interactive widget and PTY lifecycle on top
of the same idea.

## Clipboard, hyperlinks, and images

Terminal integrations are capability-sensitive:

- `OSCTools` builds OSC 8 hyperlinks, titles, notifications, and palette
  operations;
- `ClipboardTools` handles OSC 52 and local clipboard fallbacks;
- `ImageProto` encodes Sixel, Kitty, and iTerm2 image sequences;
- `NotifyTools` wraps terminal notification and progress conventions.

Applications should provide a fallback for terminals that do not advertise or
support an extension. Never make a core interaction depend on a graphics or
notification protocol.

## Recording and automation

`RecordTools` captures terminal streams for asciicast or binary playback.
Use binary recording when exact byte preservation matters; asciicast
encodes text and substitutes malformed UTF-8 in each event.
`PuppetTools` synthesizes key and mouse input and can wait for text in its
print transcript. `Fuzz` generates valid and invalid escape streams for parser
and renderer stress tests.

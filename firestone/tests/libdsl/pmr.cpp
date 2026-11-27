#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "LibDSL.hpp"

namespace
{
using namespace dsl::repl;
using dsl::live::Storage;

class CountingResource : public std::pmr::memory_resource
{
  void *do_allocate (std::size_t bytes, std::size_t align) override
  {
    auto *result = std::pmr::new_delete_resource ()->allocate (bytes, align);
    ++allocations;
    ++live;
    return result;
  }
  void do_deallocate (void *p, std::size_t bytes, std::size_t align) override
  {
    --live;
    std::pmr::new_delete_resource ()->deallocate (p, bytes, align);
  }
  bool do_is_equal (const std::pmr::memory_resource &rhs) const noexcept override
  {
    return this == &rhs;
  }

public:
  std::size_t allocations = 0, live = 0;
};

struct DefaultGuard
{
  std::pmr::memory_resource *saved
    = std::pmr::set_default_resource (std::pmr::null_memory_resource ());
  ~DefaultGuard () { std::pmr::set_default_resource (saved); }
};

struct Input
{
  std::string_view pending;
  std::size_t read (std::span<std::byte> bytes)
  {
    auto n = std::min (bytes.size (), pending.size ());
    if (n)
      std::memcpy (bytes.data (), pending.data (), n);
    pending.remove_prefix (n);
    return n;
  }
  std::size_t write (std::span<const std::byte> bytes) { return bytes.size (); }
};
}

int main ()
{
  DefaultGuard guard;
  CountingResource a, b;
  const std::string line (512, 'x'), older (512, 'y');
  {
    Editor edit ({}, Storage (&a));
    static_assert (std::is_same_v<decltype (edit.history ()),
                                  const std::deque<std::string> &>);
    auto before = a.allocations;
    assert (edit.insert (line) && a.allocations > before);
    assert (edit.remember (older));
    assert (edit.history_previous () && edit.text () == older);
    Editor copy (edit);
    assert (copy.storage ().resource () == &a);
    assert (copy.history_next () && copy.text () == line);
    before = a.allocations;
    assert (copy.kill (0, line.size ()) && a.allocations >= before + 2);
    assert (copy.undo () && copy.text () == line);
    assert (copy.redo () && copy.text ().empty ());

    Editor assigned ({}, Storage (&b));
    before = b.allocations;
    assigned = copy;
    assert (assigned.storage ().resource () == &b && b.allocations > before);
    assert (assigned.yank () && assigned.text () == line);
    assigned = std::move (edit);
    assert (assigned.storage ().resource () == &b && assigned.text () == older);
    assert (assigned.history_next () && assigned.text () == line);
    Editor moved (std::move (copy));
    assert (moved.storage ().resource () == &a && moved.yank () && moved.text () == line);
    copy = std::move (moved);
    assert (copy.text () == line && moved.text ().empty ());

    bool evaluated = false;
    Session session ({ {}, {}, [](auto, auto) { return 128; } },
                     [&](std::string_view input)
                     {
                       assert (input == line && session.editor ().text ().empty ());
                       evaluated = true;
                       return Evaluation{};
                     }, {}, Storage (&b));
    session.set_auto_indent (true);
    before = b.allocations;
    session.command (Command::newline);
    assert (session.editor ().text ().size () == 129 && b.allocations >= before + 2);
    session.cancel ();
    const auto data = line + '\n';
    Input input{data};
    dsl::IO io (input);
    before = b.allocations;
    session.run_lines (io);
    assert (evaluated && b.allocations > before);
  }
  assert (a.live == 0 && b.live == 0);

  auto bytes = std::make_shared<std::array<std::byte, 16384>> ();
  std::weak_ptr weak = bytes;
  std::optional<Editor> surviving;
  {
    Editor edit ({}, Storage::fixed (std::move (bytes)));
    assert (edit.insert (line));
    surviving.emplace (edit);
  }
  assert (!weak.expired () && surviving->text () == line);
  surviving.reset ();
  assert (weak.expired ());
}

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory_resource>

#include "LibDSL.hpp"

namespace
{
using namespace dsl::live;

class CountingResource final : public std::pmr::memory_resource
{
  void* do_allocate(std::size_t size, std::size_t alignment) override
  {
    if (fail)
      throw std::bad_alloc{};
    auto* result = std::pmr::new_delete_resource()->allocate(size, alignment);
    ++allocations;
    ++outstanding;
    return result;
  }

  void do_deallocate(void* ptr, std::size_t size, std::size_t alignment) override
  {
    std::pmr::new_delete_resource()->deallocate(ptr, size, alignment);
    --outstanding;
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
  {
    return this == &other;
  }

public:
  std::size_t allocations = 0;
  std::size_t outstanding = 0;
  bool fail = false;
};

template <typename E, typename Fn>
void expect_throw(Fn&& fn)
{
  try
    {
      fn();
    }
  catch (const E&)
    {
      return;
    }
  assert(false && "expected exception");
}

void test_sources(CountingResource& resource)
{
  SourceManager sources(&resource);
  auto name = std::string("doc");
  auto text = std::string("ab\r\nc\rd\n");
  const auto original = sources.add(name, text);
  name.clear();
  text.clear();
  const auto held = sources.text(original);
  assert(sources.name(original) == "doc");
  assert(held == "ab\r\nc\rd\n");
  assert((sources.locate(original, 3) == SourceLocation{original, 3, 1, 4}));
  assert((sources.locate(original, 4) == SourceLocation{original, 4, 2, 1}));
  assert((sources.locate(original, 6) == SourceLocation{original, 6, 3, 1}));
  assert((sources.locate(original, 8) == SourceLocation{original, 8, 4, 1}));
  assert(sources.slice({4, 5, original}) == "c");
  assert(sources.slice({8, 8, original}).empty());
  const auto empty = sources.add("doc", "");
  assert(empty != original);
  assert((sources.locate(empty, 0) == SourceLocation{empty, 0, 1, 1}));
  const auto binary = sources.add("binary", std::string_view("a\0b\xc3\xa9", 5));
  assert(sources.slice({1, 2, binary}).front() == '\0');
  assert(sources.locate(binary, 5).column == 6);
  for (int n = 0; n < 128; ++n)
    sources.add("extra", "revision");
  assert(sources.text(original).data() == held.data());
  assert(held == "ab\r\nc\rd\n");
  assert(!sources.contains(SourceId{0}));
  assert(!sources.contains(SourceSpan{0, 1, 0}));
  expect_throw<std::out_of_range>([&] { sources.slice({2, 1, original}); });
  expect_throw<std::out_of_range>([&] { sources.locate(original, 9); });
  expect_throw<std::out_of_range>([&] { sources.text(sources.size() + 1); });
  const auto old_size = sources.size();
  resource.fail = true;
  expect_throw<std::bad_alloc>([&] { sources.add("fail", "fail"); });
  resource.fail = false;
  assert(sources.size() == old_size);
}

void test_provenance_and_diagnostics(CountingResource& resource)
{
  auto backing = Storage::arena(&resource);
  OriginTable table(backing);
  DiagnosticBag diagnostics(backing);
  backing = Storage{};
  const auto file = table.add({OriginKind::source, {0, 10, 1}, 0, {}, "file"});
  const auto include = table.add({OriginKind::include, {2, 4, 1}, file, {}, "inc"});
  std::string macro = "macro";
  const auto expand = table.add({OriginKind::expansion, {0, 1, 2}, include,
                                 SourceSpan{6, 9, 1}, macro});
  macro = "changed";
  const auto trace = table.trace(expand, 3);
  assert(trace.size() == 3 && trace[2].label == "file");
  assert(trace.front().label == "macro");
  assert((trace.front().definition == SourceSpan{6, 9, 1}));
  expect_throw<std::length_error>([&] { table.trace(expand, 2); });
  expect_throw<std::invalid_argument>([&] {
    table.add({OriginKind::expansion, {}, table.size() + 1, {}, ""});
  });
  expect_throw<std::invalid_argument>([&] { table.add({OriginKind::source, {2, 1, 1}}); });
  expect_throw<std::out_of_range>([&] { table.trace(table.size() + 1); });
  assert(table.size() == 3 && table.trace(0, 0).empty());

  diagnostics.add({Severity::warning, "warning", "a warning", {4, 5}});
  assert(!diagnostics.has_errors());
  diagnostics.add({Severity::error, "expected", "expected a value", {0, 1, 2},
                   {{"expanded from here", {6, 9, 1}}}, expand,
                   {{{1, 1, 2}, ")"}}, 17});
  assert(diagnostics.has_errors());
  assert(diagnostics.entries().size() == 2);
  assert(diagnostics.entries()[1].origin == expand);
  assert(diagnostics.entries()[1].notes.front().span.source == 1);
  const auto& fix = diagnostics.entries()[1].fixits.front();
  assert(fix.span.empty() && fix.replacement == ")");
  assert(diagnostics.entries()[1].node == 17);
  diagnostics.clear();
  assert(!diagnostics.has_errors() && diagnostics.entries().empty());
}

void test_nodes()
{
  SourceManager sources;
  const auto source = sources.add("input", "xxx (tag \"data\") ;comment\n xxx");
  const SourceSpan input{3, 27, source};
  NodeTable nodes;
  const auto expression = parse_located_sexpr(sources, nodes, input, {}, 42);
  const auto ast = to_located_ast(expression);
  assert(expression.span == input && ast.span == input);
  assert(ast.id == expression.id && nodes.at(ast.id).origin == 42);
  assert(ast.value.tag() == "tag" && ast.value.value() == "data");
  const auto native = nodes.attach(dsl::ASTNode("tag", std::string("data")), input);
  assert(native.id != ast.id && native.value.value() == "data");
  assert(nodes.at(native.id).span == input && !nodes.contains(0));
  expect_throw<std::out_of_range>([&] { nodes.at(0); });
  expect_throw<std::invalid_argument>([&] {
    nodes.attach(dsl::SExpr::atom("x"), {2, 1, source});
  });
  expect_throw<std::invalid_argument>([&] {
    parse_located_sexpr(sources, nodes, input, {.max_nodes = 1});
  });
  expect_throw<std::invalid_argument>([&] { to_located_ast(expression, {.max_depth = 0}); });
  expect_throw<std::invalid_argument>([&] { to_located_ast(expression, {.max_bytes = 3}); });
  expect_throw<std::out_of_range>([&] {
    parse_located_sexpr(sources, nodes, {input.begin, input.end, 0});
  });
  assert(nodes.size() == 2);
}

void test_fixed_storage()
{
  std::array<std::byte, 128> bytes{};
  auto bounded = Storage::fixed(bytes);
  assert(bounded.resource()->allocate(bytes.size(), 1) == bytes.data());
  expect_throw<std::bad_alloc>([&] { (void)bounded.resource()->allocate(1, 1); });
  SourceManager no_capacity(bounded);
  expect_throw<std::bad_alloc>([&] { no_capacity.add("overflow", "x"); });
  assert(no_capacity.size() == 0);
  NodeTable no_nodes(bounded);
  expect_throw<std::bad_alloc>([&] { no_nodes.add({}); });
  assert(no_nodes.size() == 0);

  auto zero = Storage::fixed(std::span<std::byte>{});
  expect_throw<std::bad_alloc>([&] { (void)zero.resource()->allocate(1); });

  auto owned = std::make_shared<std::array<std::byte, 2048>>();
  std::weak_ptr weak = owned;
  {
    auto arena = Storage::fixed(owned);
    SourceManager sources(arena);
    const auto id = sources.add("small", "stable");
    arena = Storage{};
    owned.reset();
    assert(!weak.expired() && sources.text(id) == "stable");
    const std::string huge(4096, 'x');
    expect_throw<std::bad_alloc>([&] { sources.add("huge", huge); });
    assert(sources.size() == 1 && sources.text(id) == "stable");
  }
  assert(weak.expired());
  expect_throw<std::invalid_argument>([&] { Storage::fixed(owned); });
}

void test_callbacks(CountingResource& resource)
{
  std::shared_ptr<const std::function<int(int)>> retained;
  {
    CallbackRegistry callbacks(&resource);
    const CallbackRef<int(int)> ref{"scale"};
    assert(callbacks.resolve(ref).unwrap_err() == CallbackError::missing);
    assert(!callbacks.bind<int(int)>("", [](int x) { return x; }));
    assert(!callbacks.bind<int(int)>(ref.name, nullptr));
    assert(callbacks.bind<int(int)>(ref.name, [count = 0](int x) mutable {
      return 2 * x + count++;
    }));
    assert(!callbacks.bind<int(int)>(ref.name, [](int x) { return x; }));
    assert(!callbacks.bind<void()>(ref.name, [] {}));
    assert(callbacks.find<int(long)>(ref.name) == nullptr);
    assert(callbacks.resolve(CallbackRef<int(long)>{ref.name}).unwrap_err()
           == CallbackError::wrong_signature);
    const auto* borrowed = callbacks.find<int(int)>(ref.name);
    assert(borrowed && (*borrowed)(7) == 14);
    for (int n = 0; n < 100; ++n)
      assert(callbacks.bind<int(int)>(std::to_string(n), [](int x) { return x; }));
    assert(callbacks.find<int(int)>(ref.name) == borrowed);
    retained = callbacks.resolve(ref).unwrap();
    assert(retained.get() == borrowed);
  }
  assert(resource.outstanding == 0);
  assert((*retained)(7) == 15);
}
} // namespace

int main()
{
  CountingResource resource;
  test_sources(resource);
  assert(resource.outstanding == 0);
  test_provenance_and_diagnostics(resource);
  assert(resource.outstanding == 0);
  test_callbacks(resource);
  test_nodes();
  test_fixed_storage();
  assert(resource.allocations > 0);
}

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "LibDSL.hpp"

namespace
{
using namespace dsl::repl;
using namespace dsl::repl::scripts;

void portable ()
{
  assert (encode_request ({ "completion", "\"a\n" }, "id-1")
          == "(libdsl-script 1 request id-1 completion \"\\\"a\\n\")\n");
  assert (decode_response ("(libdsl-script 1 ok id \"foo\")", "id").payload == "foo");
  assert (decode_response ("(libdsl-script 1 error id \"bad\")", "id").status
          == Status::script_error);
  for (auto wire : { "(libdsl-script 2 ok id \"foo\")", "(libdsl-script 1 ok stale \"foo\")",
                     "(libdsl-script 1 ok id foo)", "(libdsl-script 1 ok id (\"foo\"))",
                     "(libdsl-script 1 ok id \"foo\") ()", "not a reply" })
    assert (decode_response (wire, "id").status == Status::protocol_error);
  assert (decode_response ("ignored", "id", 6).status == Status::output_limit);

  dsl::cli::Command root{};
  root.name = "repl";
  dsl::cli::Command group{};
  group.name = "group";
  dsl::cli::Command show{};
  show.name = "show";
  show.aliases.push_back ("s");
  dsl::cli::Option name{};
  name.name = "name";
  name.takes_value = name.required = true;
  show.options.push_back (name);
  group.subcommands.push_back (show);
  root.subcommands.push_back (group);
  auto commands = std::make_shared<Directives> (root);
  auto invoked = 0;
  commands->bind ({ "group", "s" }, [&] (const dsl::cli::ParseResult &parsed) {
    ++invoked;
    return Evaluation{ true, std::string (*parsed.value ("name")) };
  });
  assert (!commands->dispatch ("group show"));
  assert (!commands->dispatch (":group show")->success);
  assert (!commands->dispatch (":group show --name \"")->success);
  assert (!commands->dispatch (":group show --name 'unfinished")->success);
  assert (commands->dispatch (":group show --help")->output.find ("--name") != std::string::npos);
  assert (invoked == 0);
  assert (commands->dispatch ("  :group show --name 'Alice Bob'")->output == "Alice Bob");
  assert (invoked == 1);
  assert (commands->dispatch (":group s --name aliased")->output == "aliased");
  const auto completion = commands->complete (":group sho!", 10);
  assert (completion.size () == 1 && completion[0].text == "show"
          && completion[0].begin == 7 && completion[0].end == 11);
  assert (commands->complete (":group show --na", 16).front ().text == "--name");
  Session session (Directives::analysis (commands), Directives::route (commands,
    [] (std::string_view source) { return Evaluation{ true, std::string (source) }; }));
  assert (session.editor ().insert (":group show --name session"));
  assert (session.submit ().result.output == "session");
  assert (session.editor ().insert ("language expression"));
  assert (session.submit ().result.output == "language expression");
  assert (!Directives::from_text ("(command repl (command scripted))")
             .dispatch (":scripted")->success);

  bool rejected = false;
  try { (void) Runner::trusted ({ "command-from-PATH" }); }
  catch (const std::invalid_argument &) { rejected = true; }
  assert (rejected);
  auto runner = Runner::trusted ({ "/nonexistent-libdsl-script" });
  if (!process_supported ())
    assert (runner.run ({ "prompt", "" }).status == Status::unsupported);
  assert (runner.run ({ "(not an operation)", "" }).status == Status::invalid_request);
  Extension extensions;
  assert (extensions.invoke (Hook::prompt, "").status == Status::unsupported);
  extensions.install (Hook::prompt, runner);
  assert (extensions.has (Hook::prompt));
}

#if LIBDSL_LIVE_POSIX && defined(__linux__)
int fixture (std::string_view mode)
{
  if (mode == "timeout")
    {
      if (::fork () < 0)
        return 8;
      for (;;)
        ::pause ();
    }
  if (mode == "exit")
    return 37;
  if (mode == "overflow")
    {
      std::cerr << std::string (1024, 'x');
      return 0;
    }
  if (mode == "malformed")
    {
      std::cout << "(libdsl-script 100 ok fake \"bad\")\n";
      return 0;
    }
  const std::string transport = std::getenv ("LIBDSL_SCRIPT_TRANSPORT");
  std::string request;
  if (transport == "environment")
    {
      request = std::getenv ("LIBDSL_SCRIPT_REQUEST");
      ::setenv ("LIBDSL_TEST_ENV_DO_NOT_PROPAGATE", "child", 1);
    }
  else if (transport == "shared_memory")
    {
      struct stat stat{};
      if (::fstat (3, &stat) || (stat.st_mode & 0777) != 0600 || stat.st_nlink != 0)
        return 9;
      unsigned char header[8];
      if (::pread (3, header, sizeof (header), 0) != sizeof (header))
        return 8;
      if (mode == "truncate")
        return ::ftruncate (3, 0) != 0;
      std::uint32_t length = 0;
      for (int i = 0; i < 4; ++i)
        length |= std::uint32_t (header[i]) << (8 * i);
      if (length > max_wire_bytes)
        return 8;
      request.resize (length);
      if (::pread (3, request.data (), length, 8) != static_cast<ssize_t> (length))
        return 8;
    }
  else
    {
      char buffer[4096];
      ssize_t n;
      while ((n = ::read (transport == "pipes" ? 3 : 0, buffer, sizeof (buffer))) > 0)
        request.append (buffer, static_cast<std::size_t> (n));
      if (n < 0)
        return 8;
    }
  if (mode == "quiet")
    return 0;
  const auto form = dsl::parse_sexpr (request);
  if (form.items.size () != 6 || form.items[0].text != "libdsl-script"
      || form.items[1].text != "1" || form.items[2].text != "request")
    return 8;
  const std::string id = form.items[3].text;
  const auto response = dsl::SExpr::list ({
    dsl::SExpr::atom ("libdsl-script"), dsl::SExpr::atom ("1"), dsl::SExpr::atom ("ok"),
    dsl::SExpr::atom (id), dsl::SExpr::atom (form.items[5].text, true) }).dump ();
  if (transport == "shared_memory")
    {
      unsigned char length[4];
      for (unsigned i = 0; i < 4; ++i)
        length[i] = static_cast<unsigned char> (response.size () >> (8 * i));
      if (::pwrite (3, response.data (), response.size (), 8 + request.size ())
          != static_cast<ssize_t> (response.size ())
          || ::pwrite (3, length, sizeof (length), 4) != sizeof (length))
        return 8;
    }
  else if (transport == "pipes")
    {
      if (::write (4, response.data (), response.size ()) != static_cast<ssize_t> (response.size ()))
        return 8;
    }
  else
    std::cout << response;
  std::cerr << "diagnostics";
  return 0;
}

void linux_process (const std::string &executable)
{
  if (!process_supported ())
    return;
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  auto fd_count = [] {
    return std::distance (fs::directory_iterator ("/proc/self/fd"), fs::directory_iterator ());
  };
  const auto before = fd_count ();
  const std::string payload = "some \"quoted\" text\n";
  ::setenv ("LIBDSL_TEST_ENV_DO_NOT_PROPAGATE", "parent", 1);
  for (auto transport : { Transport::streams, Transport::environment, Transport::pipes,
                          Transport::shared_memory })
    if (process_supported (transport))
      {
        auto runner = Runner::trusted ({ executable, "--fixture", "good" }, { 3s }, transport);
        auto answer = runner.run ({ "evaluation", payload });
        assert (answer.status == Status::ok && answer.payload == payload
                && answer.stderr_data == "diagnostics" && answer.exit_code == 0);
        std::vector<std::thread> runs;
        for (int i = 0; i < 4; ++i)
          runs.emplace_back ([&, i] {
            auto answer = runner.run ({ "completion", std::to_string (i) });
            assert (answer.status == Status::ok && answer.payload == std::to_string (i));
          });
        for (auto &run : runs)
          run.join ();
      }
  assert (std::string (::getenv ("LIBDSL_TEST_ENV_DO_NOT_PROPAGATE")) == "parent");
  ::unsetenv ("LIBDSL_TEST_ENV_DO_NOT_PROPAGATE");
  auto run_mode = [&] (const char *mode, Limits limits = { 3s },
                       Transport transport = Transport::streams) {
    return Runner::trusted ({ executable, "--fixture", mode }, limits, transport)
      .run ({ "prompt", "" });
  };
  assert (run_mode ("exit").status == Status::exit_failure && run_mode ("exit").exit_code == 37);
  assert (run_mode ("malformed").status == Status::protocol_error);
  assert (run_mode ("overflow", { 3s, 1024, 64 }).status == Status::output_limit);
  assert (run_mode ("timeout", { 125ms }).status == Status::timeout);
  assert (run_mode ("truncate", { 3s }, Transport::shared_memory).status == Status::protocol_error);
  assert (Runner::trusted ({ "/nonexistent-libdsl-script" }).run ({ "prompt", "" }).status
          == Status::spawn_error);

  std::string path = (fs::temp_directory_path () / "libdsl-w09-XXXXXX").string ();
  assert (::mkdtemp (path.data ()));
  auto script = (fs::path (path) / "script ; with spaces").string ();
  {
    std::ofstream out (script);
    out << "#!/bin/sh\n"
           "read -r tag ver kind id operation payload || exit 9\n"
           "test \"$operation\" = prompt || exit 9\n"
           "printf '(libdsl-script 1 ok %s \"shebang\")\\n' \"$id\"\n";
  }
  auto shebang = Runner::trusted ({ script }, { 3s });
  assert (::chmod (script.c_str (), 0600) == 0);
  assert (shebang.run ({ "prompt", "" }).status == Status::spawn_error);
  assert (::chmod (script.c_str (), 0700) == 0);
  auto result = shebang.run ({ "prompt", "" });
  assert (result.status == Status::ok && result.payload == "shebang");
  Extension extensions;
  extensions.install (Hook::prompt, shebang);
  assert (extensions.invoke (Hook::prompt, "").payload == "shebang");
  {
    std::ofstream out (script, std::ios::trunc);
    out << "exit 0\n";
  }
  // No executable-format fallback through a shell.
  assert (shebang.run ({ "prompt", "" }).status == Status::spawn_error);
  auto directives = Directives::from_text ("(command repl (command script))");
  directives.bind_script ({ "script" }, Runner::trusted ({ executable, "--fixture", "good" }));
  const auto dispatched = directives.dispatch (":script");
  assert (dispatched && dispatched->success && dispatched->output == "(directive (\"script\") () ())");
  fs::remove (script);
  fs::remove (path);
  assert (fd_count () == before);
  int status;
  assert (::waitpid (-1, &status, WNOHANG) == -1 && errno == ECHILD);
}
#endif
}

int main (int argc, char **argv)
{
#if LIBDSL_LIVE_POSIX && defined(__linux__)
  if (argc == 3 && std::string_view (argv[1]) == "--fixture")
    return fixture (argv[2]);
  linux_process (std::filesystem::absolute (argv[0]).string ());
#else
  (void) argc;
  (void) argv;
#endif
  portable ();
}

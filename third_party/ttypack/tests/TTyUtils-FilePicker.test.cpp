#include <TTyUtils/TTyUtils-FilePicker.hpp>
#include <cassert>
#include <fstream>
#include <unistd.h>
using namespace ttyutils::ui;

int main() {
    namespace fs = std::filesystem;
    const auto path_template = (fs::temp_directory_path() / "ttypack-picker-XXXXXX").string();
    auto buffer = std::vector<char>(path_template.begin(), path_template.end());
    buffer.push_back('\0');
    const auto* made = ::mkdtemp(buffer.data());
    assert(made);
    struct Fixture {
        fs::path root;
        ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    } fixture{made};
    const auto& root = fixture.root;
    fs::create_directory(root / "child");
    std::ofstream(root / "a.cpp") << "cpp";
    std::ofstream(root / "b.txt") << "txt";
    std::ofstream(root / ".x.cpp") << "hidden";
    FilePicker picker;
    picker.set_directory(root);
    picker.filter({"*.cpp", "?.hpp"});
    std::error_code error;
    assert(picker.entries(error) == std::vector<fs::path>({root / "a.cpp", root / "child"}) && !error);
    picker.set_hidden(true);
    assert(picker.entries(error).size() == 3);
    assert(!picker.show());
    picker.set_selection("b.txt");
    assert(!picker.show());
    picker.set_selection("a.cpp");
    assert(picker.show() == root / "a.cpp");
    picker.mode(PickMode::Save);
    assert(!picker.show());
    picker.set_overwrite_confirm([](const auto&) { return true; });
    assert(picker.show() == root / "a.cpp");
    picker.set_selection("new.cpp");
    assert(picker.show() == root / "new.cpp");
    picker.mode(PickMode::Directory);
    picker.add_bookmark({"child", root / "child"});
    assert(picker.goto_bookmark(0) && picker.show() == root / "child");
    picker.cancel();
    assert(!picker.show());
}

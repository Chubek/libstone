#include <TTyTk/TTyTk-RecordTools.hpp>
#include <cassert>
#include <filesystem>
#include <iterator>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "ttytk-record-test.cast";
    ttytk::Recorder recorder;
    recorder.start(90, 35);
    recorder.record_output(std::string_view{"\x1b[\t\0\xffé\"\\", 9});
    recorder.record_input("q");
    recorder.stop();
    recorder.record_output("ignored");
    assert(recorder.save_asciicast(path.string()));
    {
        std::ifstream input{path};
        std::string line;
        assert(std::getline(input, line) && line == "{\"version\":2,\"width\":90,\"height\":35}");
        assert(std::getline(input, line) && line.ends_with(",\"o\",\"\\u001b[\\u0009\\u0000\\ufffdé\\\"\\\\\"]"));
        assert(std::getline(input, line) && line.ends_with(",\"i\",\"q\"]"));
        assert(!std::getline(input, line));
    }
    assert(recorder.save_binary(path.string()));
    {
        std::ifstream input{path, std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
        assert(bytes.starts_with("TTYPACK1"));
        assert(bytes[8] == 90 && bytes[16] == 35 && bytes[24] == 'o' && bytes[33] == 9);
        assert(bytes.substr(41, 9) == std::string_view("\x1b[\t\0\xffé\"\\", 9));
        assert(bytes[50] == 'i' && bytes.back() == 'q');
    }
    std::filesystem::remove(path);
}

// A UEFI application in C++23 beneath the C library and C++ run-time of openkal-platform's row,
// over this implementation: the Shell's arguments as `main' receives them, files and a directory
// through the standard library (openkal.fs), and std::random_device, which with the C library's
// allocator draws on openkal.random. Started by the UEFI Shell as `std.efi --shell "a b" c "two   words here"'.
import std;

namespace {
int failures = 0;
void check(bool ok, std::string_view what) {
    std::println("  {} {}", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
}  // namespace

int main(int argc, char** argv) {
    std::println("hello from the C library over openkal-uefi");
    const std::vector<std::string_view> args(argv, argv + argc);
    check(args.size() == 5 && args[1] == "--shell" && args[2] == "a b" && args[3] == "c" && args[4] == "two   words here",
          "main receives the Shell's arguments, a quoted one whole");

    namespace fs = std::filesystem;
    check(fs::current_path() == fs::path("/"), "the working directory is the volume's root");
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator("/")) names.push_back(e.path().filename().string());
    std::ranges::sort(names);
    std::println("  the volume: {}", names);
    // FAT keeps a name's case where it records one; the comparison is the volume's, without case.
    const auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    check(std::ranges::any_of(names, [&](const std::string& n) { return lower(n) == "std.efi"; }), "the root is listed");
    {
        std::ofstream out("/okstd/written.txt");
        check(!out, "a file beneath a directory that does not exist is not created");
    }
    check(fs::create_directory("/okstd"), "a directory is made");
    {
        std::ofstream out("/okstd/written.txt");
        out << "written through the C library\n";
    }
    std::ifstream in("/okstd/written.txt");
    std::string line;
    std::getline(in, line);
    check(line == "written through the C library", "a file is written and read back");
    check(fs::file_size("/okstd/written.txt") == 30, "its size");

    std::random_device rd;
    const auto a = rd(), b = rd(), c = rd();
    check(!(a == b && b == c), "std::random_device draws on the firmware's source");

    std::println("{}", failures ? "FAILED" : "all held");
    return failures ? 1 : 42;
}

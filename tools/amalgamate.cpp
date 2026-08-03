// A small, dependency-free build tool for producing the distributable cloud header.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

class failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& message) {
    throw failure(message);
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool is_horizontal_space(char value) {
    return value == ' ' || value == '\t';
}

std::string_view trim_left(std::string_view value) {
    while (!value.empty() && is_horizontal_space(value.front())) {
        value.remove_prefix(1);
    }
    return value;
}

std::string_view trim(std::string_view value) {
    value = trim_left(value);
    while (!value.empty() && is_horizontal_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

bool is_identifier_start(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool is_identifier_character(char value) {
    return is_identifier_start(value) || (value >= '0' && value <= '9');
}

struct directive {
    std::string_view name;
    std::string_view rest;
};

std::optional<directive> parse_directive(std::string_view line) {
    line = trim_left(line);
    if (line.empty() || line.front() != '#') {
        return std::nullopt;
    }

    line.remove_prefix(1);
    line = trim_left(line);
    if (line.empty() || !is_identifier_start(line.front())) {
        return directive{{}, line};
    }

    std::size_t length = 1;
    while (length < line.size() && is_identifier_character(line[length])) {
        ++length;
    }

    return directive{line.substr(0, length), trim_left(line.substr(length))};
}

std::string display_path(const fs::path& path) {
    return path.generic_string();
}

std::string read_bytes(const fs::path& path, const std::string& description) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        fail("cannot read " + description + ": " + display_path(path));
    }

    std::string contents((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    if (stream.bad()) {
        fail("cannot read " + description + ": " + display_path(path));
    }
    return contents;
}

std::string normalise_newlines(std::string_view input) {
    std::string output;
    output.reserve(input.size());

    for (std::size_t index = 0; index < input.size(); ++index) {
        const char value = input[index];
        if (value == '\0') {
            fail("project header contains a NUL byte");
        }
        if (value != '\r') {
            output.push_back(value);
            continue;
        }

        if (index + 1 < input.size() && input[index + 1] == '\n') {
            ++index;
        }
        output.push_back('\n');
    }

    return output;
}

std::vector<std::string> split_lines(const std::string& contents) {
    std::vector<std::string> lines;
    std::size_t beginning = 0;

    while (beginning < contents.size()) {
        const std::size_t ending = contents.find('\n', beginning);
        if (ending == std::string::npos) {
            lines.emplace_back(contents.substr(beginning));
            return lines;
        }

        lines.emplace_back(contents.substr(beginning, ending - beginning));
        beginning = ending + 1;
    }

    return lines;
}

bool is_safe_project_path(std::string_view path) {
    constexpr std::string_view prefix = "cloud/";
    if (!starts_with(path, prefix) || path.size() == prefix.size()) {
        return false;
    }

    std::size_t segment_beginning = prefix.size();
    for (std::size_t index = segment_beginning; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/') {
            if (!is_identifier_character(path[index]) && path[index] != '-' &&
                path[index] != '.') {
                return false;
            }
            continue;
        }

        const std::string_view segment = path.substr(segment_beginning, index - segment_beginning);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        segment_beginning = index + 1;
    }

    return true;
}

std::optional<std::string> project_include(std::string_view line,
                                           std::string_view line_after_leading_comments,
                                           const std::string& module_name) {
    const bool mentions_project_header =
        line.find("\"cloud/") != std::string_view::npos ||
        line.find("<cloud/") != std::string_view::npos;
    const std::optional<directive> parsed = parse_directive(line);
    if (!parsed) {
        const std::optional<directive> obscured =
            parse_directive(line_after_leading_comments);
        if ((obscured && (obscured->name == "include" || mentions_project_header)) ||
            starts_with(trim_left(line_after_leading_comments), "%:include")) {
            fail("ambiguous project include in " + module_name);
        }
        return std::nullopt;
    }

    if (parsed->name != "include") {
        const std::string_view complete_line = trim(line);
        const bool split_include =
            (parsed->name.empty() || starts_with("include", parsed->name)) &&
            !complete_line.empty() && complete_line.back() == '\\';
        const bool obscured_directive =
            parsed->name.empty() && !trim(parsed->rest).empty();
        if (mentions_project_header || starts_with(parsed->name, "include") ||
            split_include || obscured_directive) {
            fail("ambiguous project include in " + module_name);
        }
        return std::nullopt;
    }

    const std::string_view remainder = trim(parsed->rest);
    if (remainder.empty()) {
        fail("missing include operand in " + module_name);
    }

    if (remainder.front() == '"') {
        const std::size_t closing = remainder.find('"', 1);
        if (closing == std::string_view::npos) {
            if (remainder.find("cloud/") != std::string_view::npos) {
                fail("malformed project include in " + module_name);
            }
            return std::nullopt;
        }

        const std::string_view target = remainder.substr(1, closing - 1);
        if (!starts_with(target, "cloud/")) {
            return std::nullopt;
        }
        if (!trim(remainder.substr(closing + 1)).empty() || !is_safe_project_path(target)) {
            fail("malformed project include in " + module_name);
        }
        return std::string(target);
    }

    if (remainder.front() == '<') {
        const std::size_t closing = remainder.find('>', 1);
        if (closing != std::string_view::npos &&
            starts_with(remainder.substr(1, closing - 1), "cloud/")) {
            fail("project includes must use quotes in " + module_name);
        }
        return std::nullopt;
    }

    // Macro operands can expand to project headers and would leave the
    // generated header dependent on the private source tree.
    fail("include operands must use quotes or angle brackets in " + module_name);
}

bool comment_or_blank_line(std::string_view line, bool& in_block_comment) {
    std::size_t position = 0;

    for (;;) {
        while (position < line.size() && is_horizontal_space(line[position])) {
            ++position;
        }
        if (position == line.size()) {
            return true;
        }

        if (in_block_comment) {
            const std::size_t ending = line.find("*/", position);
            if (ending == std::string_view::npos) {
                return true;
            }
            in_block_comment = false;
            position = ending + 2;
            continue;
        }

        if (line.substr(position, 2) == "//") {
            return true;
        }
        if (line.substr(position, 2) == "/*") {
            in_block_comment = true;
            position += 2;
            continue;
        }
        return false;
    }
}

std::vector<bool> comment_or_blank_lines(const std::vector<std::string>& lines) {
    std::vector<bool> result(lines.size(), false);
    bool in_block_comment = false;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        result[index] = comment_or_blank_line(lines[index], in_block_comment);
    }
    return result;
}

std::vector<std::string_view>
without_leading_comments(const std::vector<std::string>& lines) {
    std::vector<std::string_view> result;
    result.reserve(lines.size());
    bool in_block_comment = false;

    for (const std::string& line : lines) {
        std::string_view remaining(line);
        for (;;) {
            remaining = trim_left(remaining);
            if (remaining.empty()) {
                break;
            }
            if (in_block_comment) {
                const std::size_t ending = remaining.find("*/");
                if (ending == std::string_view::npos) {
                    remaining = {};
                    break;
                }
                in_block_comment = false;
                remaining.remove_prefix(ending + 2);
                continue;
            }
            if (starts_with(remaining, "//")) {
                remaining = {};
                break;
            }
            if (starts_with(remaining, "/*")) {
                in_block_comment = true;
                remaining.remove_prefix(2);
                continue;
            }
            break;
        }
        result.push_back(remaining);
    }
    return result;
}

bool is_pragma_once(std::string_view line) {
    const std::optional<directive> parsed = parse_directive(line);
    return parsed && parsed->name == "pragma" && trim(parsed->rest) == "once";
}

bool is_comment_suffix(std::string_view value) {
    value = trim(value);
    return value.empty() || starts_with(value, "//") ||
           (starts_with(value, "/*") && value.size() >= 4 &&
            value.substr(value.size() - 2) == "*/");
}

std::optional<std::string_view> guard_identifier(std::string_view rest) {
    rest = trim_left(rest);
    if (rest.empty() || !is_identifier_start(rest.front())) {
        return std::nullopt;
    }

    std::size_t length = 1;
    while (length < rest.size() && is_identifier_character(rest[length])) {
        ++length;
    }
    if (!is_comment_suffix(rest.substr(length))) {
        return std::nullopt;
    }
    return rest.substr(0, length);
}

struct include_guard {
    std::size_t ifndef_line;
    std::size_t define_line;
    std::size_t endif_line;
};

std::optional<include_guard> find_include_guard(const std::vector<std::string>& lines) {
    if (lines.empty()) {
        return std::nullopt;
    }

    std::vector<bool> ignorable = comment_or_blank_lines(lines);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        ignorable[index] = ignorable[index] || is_pragma_once(lines[index]);
    }

    std::size_t ifndef_line = 0;
    while (ifndef_line < lines.size() && ignorable[ifndef_line]) {
        ++ifndef_line;
    }
    if (ifndef_line == lines.size()) {
        return std::nullopt;
    }

    const std::optional<directive> ifndef = parse_directive(lines[ifndef_line]);
    if (!ifndef || ifndef->name != "ifndef") {
        return std::nullopt;
    }
    const std::optional<std::string_view> identifier = guard_identifier(ifndef->rest);
    if (!identifier) {
        return std::nullopt;
    }

    std::size_t define_line = ifndef_line + 1;
    while (define_line < lines.size() && ignorable[define_line]) {
        ++define_line;
    }
    if (define_line == lines.size()) {
        return std::nullopt;
    }

    const std::optional<directive> define = parse_directive(lines[define_line]);
    if (!define || define->name != "define") {
        return std::nullopt;
    }
    const std::optional<std::string_view> defined_identifier = guard_identifier(define->rest);
    if (!defined_identifier || *defined_identifier != *identifier) {
        return std::nullopt;
    }

    std::size_t endif_line = lines.size();
    while (endif_line > 0 && ignorable[endif_line - 1]) {
        --endif_line;
    }
    if (endif_line == 0) {
        return std::nullopt;
    }
    --endif_line;

    const std::optional<directive> endif = parse_directive(lines[endif_line]);
    if (!endif || endif->name != "endif" || !is_comment_suffix(endif->rest)) {
        return std::nullopt;
    }

    int depth = 0;
    for (std::size_t index = ifndef_line; index <= endif_line; ++index) {
        if (ignorable[index]) {
            continue;
        }
        const std::optional<directive> current = parse_directive(lines[index]);
        if (!current) {
            continue;
        }
        if (current->name == "if" || current->name == "ifdef" || current->name == "ifndef") {
            ++depth;
        } else if ((current->name == "else" || current->name == "elif" ||
                    current->name == "elifdef" || current->name == "elifndef") &&
                   depth == 1) {
            // An alternate branch belongs to the module guard itself. Removing
            // that guard would attach the branch to the generated outer guard.
            return std::nullopt;
        } else if (current->name == "endif") {
            --depth;
            if (depth < 0 || (depth == 0 && index != endif_line)) {
                return std::nullopt;
            }
        }
    }
    if (depth != 0) {
        return std::nullopt;
    }

    return include_guard{ifndef_line, define_line, endif_line};
}

bool path_is_within(const fs::path& path, const fs::path& root) {
    auto path_part = path.begin();
    for (auto root_part = root.begin(); root_part != root.end(); ++root_part, ++path_part) {
        if (path_part == path.end() || *path_part != *root_part) {
            return false;
        }
    }
    return true;
}

fs::path existing_directory(const fs::path& path, const std::string& description) {
    std::error_code error;
    const fs::path canonical = fs::canonical(path, error);
    if (error || !fs::is_directory(canonical, error) || error) {
        fail(description + " is not a directory: " + display_path(path));
    }
    return canonical;
}

fs::path existing_file(const fs::path& path, const std::string& description) {
    std::error_code error;
    const fs::path canonical = fs::canonical(path, error);
    if (error || !fs::is_regular_file(canonical, error) || error) {
        fail(description + " is missing: " + display_path(path));
    }
    return canonical;
}

struct amalgamation {
    std::string header;
    std::vector<std::string> modules;
};

class amalgamator {
public:
    amalgamator(fs::path root, fs::path include_root)
        : include_root_(existing_directory(include_root, "include root")),
          root_(existing_file(root, "root header")) {
        if (!path_is_within(root_, include_root_)) {
            fail("root header is outside the include root: " + display_path(root));
        }
    }

    amalgamation generate() {
        output_ = "// cloud\n"
                  "// Generated from the private cloud source fragments.\n"
                  "// Do not edit this file directly.\n"
                  "\n"
                  "#ifndef NJLANE314_CLOUD_INCLUDED\n"
                  "#define NJLANE314_CLOUD_INCLUDED\n"
                  "\n";

        emit(root_);
        output_ += "#endif // NJLANE314_CLOUD_INCLUDED\n";
        return amalgamation{std::move(output_), std::move(modules_)};
    }

private:
    std::string module_name(const fs::path& file) const {
        return file.lexically_relative(include_root_).generic_string();
    }

    fs::path resolve(const std::string& include, const std::string& source_module) const {
        const fs::path candidate = include_root_ / fs::path(include);
        std::error_code error;
        const fs::path resolved = fs::canonical(candidate, error);
        if (error || !fs::is_regular_file(resolved, error) || error) {
            fail("missing project header " + include + " included by " + source_module);
        }
        if (!path_is_within(resolved, include_root_)) {
            fail("project header is outside the include root: " + include);
        }
        return resolved;
    }

    void emit(const fs::path& file) {
        const std::string key = file.generic_string();
        if (emitted_.find(key) != emitted_.end()) {
            return;
        }

        const std::string name = module_name(file);
        if (!active_.insert(key).second) {
            fail("cyclic project include: " + name);
        }

        const std::string contents = normalise_newlines(read_bytes(file, "project header"));
        const std::vector<std::string> lines = split_lines(contents);
        const std::vector<bool> comment_only = comment_or_blank_lines(lines);
        const std::vector<std::string_view> uncommented_lines =
            without_leading_comments(lines);
        const std::optional<include_guard> guard = find_include_guard(lines);

        // Resolve dependencies first so each section marker sits immediately
        // above that module's own text in the generated header.
        bool module_body_started = false;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (comment_only[index] || is_pragma_once(lines[index]) ||
                (guard && (index == guard->ifndef_line || index == guard->define_line ||
                           index == guard->endif_line))) {
                continue;
            }
            const std::optional<std::string> include =
                project_include(lines[index], uncommented_lines[index], name);
            if (include) {
                if (module_body_started) {
                    fail("project includes must precede module content in " + name);
                }
                emit(resolve(*include, name));
            } else {
                module_body_started = true;
            }
        }

        modules_.push_back(name);
        output_ += "// -----------------------------------------------------------------------------\n";
        output_ += "// BEGIN " + name + "\n";
        output_ += "// -----------------------------------------------------------------------------\n";

        for (std::size_t index = 0; index < lines.size(); ++index) {
            if ((!comment_only[index] && is_pragma_once(lines[index])) ||
                (guard && (index == guard->ifndef_line || index == guard->define_line ||
                           index == guard->endif_line))) {
                continue;
            }

            const std::optional<std::string> include =
                comment_only[index]
                    ? std::nullopt
                    : project_include(lines[index], uncommented_lines[index], name);
            if (!include) {
                output_ += lines[index];
                output_.push_back('\n');
            }
        }

        output_.push_back('\n');
        active_.erase(key);
        emitted_.insert(key);
    }

    fs::path include_root_;
    fs::path root_;
    std::string output_;
    std::vector<std::string> modules_;
    std::unordered_set<std::string> active_;
    std::unordered_set<std::string> emitted_;
};

enum class action {
    write,
    check,
    standard_output,
    list_modules
};

struct options {
    fs::path root = "include/cloud/cloud.hpp";
    fs::path include_root = "include";
    fs::path output = "cloud";
    action selected_action = action::write;
    bool help = false;
};

void print_usage(std::ostream& stream) {
    stream << "usage: amalgamate [--write | --check | --stdout | --list-modules]\n"
              "                  [--root PATH] [--include-root PATH] [--output PATH]\n";
}

options parse_arguments(int argument_count, char** arguments) {
    options parsed;
    bool action_given = false;
    bool root_given = false;
    bool include_root_given = false;
    bool output_given = false;

    for (int index = 1; index < argument_count; ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--help" || argument == "-h") {
            if (argument_count != 2) {
                fail("--help cannot be combined with other options");
            }
            parsed.help = true;
            return parsed;
        }

        std::optional<action> requested_action;
        if (argument == "--write") {
            requested_action = action::write;
        } else if (argument == "--check") {
            requested_action = action::check;
        } else if (argument == "--stdout") {
            requested_action = action::standard_output;
        } else if (argument == "--list-modules") {
            requested_action = action::list_modules;
        }

        if (requested_action) {
            if (action_given) {
                fail("choose exactly one action");
            }
            parsed.selected_action = *requested_action;
            action_given = true;
            continue;
        }

        const auto take_path = [&](bool& already_given, const std::string& option) -> fs::path {
            if (already_given) {
                fail("option repeated: " + option);
            }
            if (++index >= argument_count || std::string_view(arguments[index]).empty()) {
                fail("missing path after " + option);
            }
            already_given = true;
            return fs::path(arguments[index]);
        };

        if (argument == "--root") {
            parsed.root = take_path(root_given, "--root");
        } else if (argument == "--include-root") {
            parsed.include_root = take_path(include_root_given, "--include-root");
        } else if (argument == "--output") {
            parsed.output = take_path(output_given, "--output");
        } else {
            fail("unrecognised option: " + std::string(argument));
        }
    }

    return parsed;
}

class temporary_path {
public:
    explicit temporary_path(fs::path path) : path_(std::move(path)) {}

    ~temporary_path() {
        if (!keep_) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }

    const fs::path& get() const noexcept { return path_; }
    void keep() noexcept { keep_ = true; }

private:
    fs::path path_;
    bool keep_ = false;
};

fs::path unused_temporary_path(const fs::path& output) {
    for (unsigned int suffix = 0; suffix < 10000; ++suffix) {
        fs::path candidate = output;
        candidate += suffix == 0 ? ".tmp" : ".tmp." + std::to_string(suffix);

        std::error_code error;
        const bool exists = fs::exists(candidate, error);
        if (error) {
            fail("cannot inspect temporary output: " + display_path(candidate));
        }
        if (!exists) {
            return candidate;
        }
    }
    fail("cannot allocate a temporary output beside " + display_path(output));
}

void write_atomically(const fs::path& requested_output, const std::string& contents) {
    std::error_code error;
    fs::path output = fs::absolute(requested_output, error);
    if (error || output.filename().empty()) {
        fail("invalid output path: " + display_path(requested_output));
    }
    output = output.lexically_normal();

    if (fs::exists(output, error)) {
        if (error || !fs::is_regular_file(output, error) || error) {
            fail("output is not a file: " + display_path(requested_output));
        }
        if (read_bytes(output, "output") == contents) {
            return;
        }
    } else if (error) {
        fail("cannot inspect output: " + display_path(requested_output));
    }

    const fs::path parent = output.parent_path();
    fs::create_directories(parent, error);
    if (error) {
        fail("cannot create output directory: " + display_path(parent));
    }

    temporary_path temporary(unused_temporary_path(output));
    {
        std::ofstream stream(temporary.get(), std::ios::binary | std::ios::trunc);
        if (!stream) {
            fail("cannot create temporary output: " + display_path(temporary.get()));
        }
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.close();
        if (!stream) {
            fail("cannot write temporary output: " + display_path(temporary.get()));
        }
    }

    fs::rename(temporary.get(), output, error);
    if (error) {
        fail("cannot atomically replace output: " + display_path(requested_output));
    }
    temporary.keep();
}

void write_standard_output(const std::string& contents) {
    std::cout.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!std::cout) {
        fail("cannot write standard output");
    }
}

void perform(const options& parsed) {
    amalgamation generated = amalgamator(parsed.root, parsed.include_root).generate();

    switch (parsed.selected_action) {
    case action::write:
        write_atomically(parsed.output, generated.header);
        return;
    case action::check: {
        std::error_code error;
        if (!fs::is_regular_file(parsed.output, error) || error) {
            fail("generated header is missing: " + display_path(parsed.output));
        }
        if (read_bytes(parsed.output, "generated header") != generated.header) {
            fail("generated header is out of date: " + display_path(parsed.output));
        }
        return;
    }
    case action::standard_output:
        write_standard_output(generated.header);
        return;
    case action::list_modules:
        for (const std::string& module : generated.modules) {
            std::cout << module << '\n';
        }
        if (!std::cout) {
            fail("cannot write standard output");
        }
        return;
    }
}

} // namespace

int main(int argument_count, char** arguments) {
    try {
        const options parsed = parse_arguments(argument_count, arguments);
        if (parsed.help) {
            print_usage(std::cout);
            return 0;
        }
        perform(parsed);
        return 0;
    } catch (const failure& exception) {
        std::cerr << "amalgamate: " << exception.what() << '\n';
        return 1;
    } catch (const std::exception& exception) {
        std::cerr << "amalgamate: " << exception.what() << '\n';
        return 1;
    }
}

#include "code_generator/code_generator.h"
#include "parser/parser.h"
#include "run.h"
#include "semantic/semantic.h"
#include "tokeniser/tokeniser.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
constexpr std::string_view usage = "Usage: jaqr [--exec] <source.jaqr> [-o <output.cpp>]\n";

// TODO: add --help and --version (both print and exit 0 without needing a source file).
// TODO: add --no-optimize; code_generator::gen_code already takes optimization_options
//       (src/optimizer/optimizer.h) but the driver always passes the defaults.
// TODO: accept `-o -` to write the generated C++ to stdout.
// TODO: accept several source files once the language has a notion of separate units.
struct command_line {
    std::string source;
    std::string output = "a.cpp";
    bool run = false;
};

// A command line jaqr does not accept; main reports it followed by the usage line.
class usage_error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

command_line parse_arguments(int argc, char *argv[]) {
    command_line parsed;
    bool has_source = false;
    bool named_output = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--exec") {
            if (parsed.run)
                throw usage_error("--exec given more than once");
            parsed.run = true;
        } else if (arg == "-o") {
            if (named_output)
                throw usage_error("-o given more than once");
            if (i + 1 >= argc || !*argv[i + 1])
                throw usage_error("-o needs a non-empty output file name");
            named_output = true;
            parsed.output = argv[++i];
        } else if (arg.empty()) {
            throw usage_error("empty argument");
        } else if (arg.front() == '-') {
            throw usage_error("unknown option '" + std::string(arg) + "'");
        } else if (has_source) {
            throw usage_error("more than one source file given ('" + parsed.source + "' and '" +
                              std::string(arg) + "')");
        } else {
            has_source = true;
            parsed.source = arg;
        }
    }
    if (!has_source)
        throw usage_error("no source file given");
    return parsed;
}

// One entry per line, each ending in '\n': the lexer counts lines from these boundaries, and the
// newline separates tokens on adjacent lines.
std::vector<std::vector<char>> read_source(const std::string &filename) {
    std::ifstream file(filename);
    if (!file)
        throw std::runtime_error("cannot open source file '" + filename + "'");
    std::vector<std::vector<char>> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.emplace_back(line.begin(), line.end());
        lines.back().push_back('\n');
    }
    if (file.bad())
        throw std::runtime_error("cannot read source file '" + filename + "'");
    return lines;
}

int compile(const command_line &options) {
    std::error_code error;
    if (std::filesystem::equivalent(options.source, options.output, error))
        throw std::runtime_error("generated C++ output '" + options.output +
                                 "' would overwrite the source file");
    // Validate before optimising/emitting: unreachable code must still obey JAQR's rules.
    // The analysis result owns the AST while the generator borrows its declaration indexes.
    auto analysed = analyse(parse(tokenise(read_source(options.source))), options.source);
    auto &[root, impls, invs] = analysed;
    return run_generated(code_generator{impls, invs}.gen_code(*root), options.output, options.run);
}
} // namespace

int main(int argc, char *argv[]) {
    // TODO: report every phase through one diagnostic type (file, 1-based start/end line and
    //       column, code, message, and the offending source line) so main needs a single
    //       handler. Today lexer and parser failures are plain std::runtime_error text such as
    //       "LEX001: ... at line N, column M" with zero-based positions and no file name, while
    //       semantic_error is already formatted as "file:l:c-l:c: SEMxxx: ...".
    try {
        return compile(parse_arguments(argc, argv));
    } catch (const usage_error &error) {
        std::cerr << "Error: " << error.what() << '\n' << usage;
        return 1;
    } catch (const semantic_error &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

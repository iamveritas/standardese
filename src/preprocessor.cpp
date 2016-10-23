// Copyright (C) 2016 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include <standardese/preprocessor.hpp>

#include <boost/config.hpp>
#include <boost/filesystem.hpp>
#include <boost/version.hpp>

#if (BOOST_VERSION / 100000) != 1
#error "require Boost 1.x"
#endif

#if ((BOOST_VERSION / 100) % 1000) < 55
#warning "Boost less than 1.55 isn't tested"
#endif

#include <standardese/config.hpp>
#include <standardese/error.hpp>
#include <standardese/parser.hpp>

// treat the tiny-process-library as header only
#include <process.hpp>
#include <process.cpp>
#ifdef BOOST_WINDOWS
#include <process_win.cpp>
#else
#include <process_unix.cpp>
#endif

using namespace standardese;

namespace fs = boost::filesystem;

namespace
{
    std::string get_command(const compile_config& c, const char* full_path)
    {
        std::string cmd("clang++ -E -C ");
        for (auto flag : c.get_flags())
        {
            cmd += flag;
            cmd += ' ';
        }

        cmd += full_path;
        return cmd;
    }

    std::string get_full_preprocess_output(const parser& p, const compile_config& c,
                                           const char* full_path)
    {
        std::string preprocessed;

        auto    cmd = get_command(c, full_path);
        Process process(cmd, "",
                        [&](const char* str, std::size_t n) { preprocessed.append(str, n); },
                        [&](const char* str, std::size_t n) {
                            p.get_logger()->error("[preprocessor] {}", std::string(str, n));
                        });

        auto exit_code = process.get_exit_status();
        if (exit_code != 0)
            throw process_error(cmd, exit_code);

        return preprocessed;
    }

    struct line_marker
    {
        enum flag_t
        {
            enter_new = 1, // flag 1
            enter_old = 2, // flag 2
            system    = 4, // flag 3
        };

        std::string file_name;
        unsigned    line, flags;

        line_marker() : line(0u), flags(0u)
        {
        }

        void set_flag(flag_t f)
        {
            flags |= f;
        }

        bool is_set(flag_t f) const
        {
            return (flags & f) != 0;
        }
    };

    // preprocessor line marker
    // format: # <line> "<file-name>" <flags>
    // flag 1 - start of a new file
    // flag 2 - returning to previous file
    // flag 3 - system header
    // flag 4 is irrelevant
    line_marker parse_line_marker(const char*& ptr)
    {
        line_marker result;

        assert(*ptr == '#');
        ++ptr;

        while (*ptr == ' ')
            ++ptr;

        std::string line;
        while (std::isdigit(*ptr))
            line += *ptr++;
        result.line = unsigned(std::stoi(line));

        while (*ptr == ' ')
            ++ptr;

        assert(*ptr == '"');
        ++ptr;

        std::string file_name;
        while (*ptr != '"')
            file_name += *ptr++;
        ++ptr;
        result.file_name = std::move(file_name);

        while (*ptr == ' ')
            ++ptr;

        while (*ptr != '\n')
        {
            if (*ptr == ' ')
                ++ptr;

            switch (*ptr)
            {
            case '1':
                result.set_flag(line_marker::enter_new);
                break;
            case '2':
                result.set_flag(line_marker::enter_old);
                break;
            case '3':
                result.set_flag(line_marker::system);
                break;
            case '4':
                break;
            default:
                assert(false);
            }
            ++ptr;
        }

        return result;
    }
}

std::string preprocessor::preprocess(const parser& p, const compile_config& c,
                                     const char* full_path) const
{
    std::string preprocessed;

    auto full_preprocessed = get_full_preprocess_output(p, c, full_path);
    auto file_depth        = 0;
    auto was_newl = true, in_c_comment = false, write_char = true;
    for (auto ptr = full_preprocessed.c_str(); *ptr; ++ptr)
    {
        if (*ptr == '\n')
        {
            was_newl = true;
        }
        else if (in_c_comment && ptr[0] == '*' && ptr[1] == '/')
        {
            in_c_comment = false;
            was_newl     = false;
        }
        else if (*ptr == '/' && ptr[1] == '*')
        {
            in_c_comment = true;
            was_newl     = false;
        }
        else if (was_newl && !in_c_comment && *ptr == '#' && ptr[1] != 'p')
        {
            auto marker = parse_line_marker(ptr);
            assert(*ptr == '\n');

            if (marker.file_name == full_path)
            {
                assert(file_depth <= 1);
                file_depth = 0;
                write_char = false;
            }
            else if (marker.is_set(line_marker::enter_new))
            {
                ++file_depth;
                if (file_depth == 1 && marker.file_name != "<built-in>"
                    && marker.file_name != "<command line>")
                {
                    // write include
                    preprocessed += "#include ";
                    if (marker.is_set(line_marker::system))
                        preprocessed += '<';
                    else
                        preprocessed += '"';
                    preprocessed += marker.file_name;
                    if (marker.is_set(line_marker::system))
                        preprocessed += '>';
                    else
                        preprocessed += '"';
                    preprocessed += '\n';
                }
            }
            else if (marker.is_set(line_marker::enter_old))
                --file_depth;
        }
        else
            was_newl = false;

        if (file_depth == 0 && write_char)
            preprocessed += *ptr;
        else if (file_depth == 0)
            write_char = true;
    }

    return preprocessed;
}

void preprocessor::add_preprocess_directory(std::string dir)
{
    auto path = fs::system_complete(dir).normalize().generic_string();
    if (!path.empty() && path.back() == '.')
        path.pop_back();
    preprocess_dirs_.insert(std::move(path));
}

bool preprocessor::is_preprocess_directory(const std::string& dir) const STANDARDESE_NOEXCEPT
{
    return preprocess_dirs_.count(dir) != 0;
}

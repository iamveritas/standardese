// Copyright (C) 2016 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include <standardese/translation_unit.hpp>

#include <fstream>
#include <iterator>
#include <string>

#include <standardese/detail/scope_stack.hpp>
#include <standardese/detail/tokenizer.hpp>
#include <standardese/detail/wrapper.hpp>
#include <standardese/comment.hpp>
#include <standardese/cpp_preprocessor.hpp>
#include <standardese/cpp_template.hpp>
#include <standardese/parser.hpp>

using namespace standardese;

struct translation_unit::impl
{
    detail::context            context;
    cpp_name                   full_path;
    std::string                source;
    cpp_file*                  file;
    const standardese::parser* parser;

    impl(const standardese::parser& p, const char* path, cpp_file* file,
         const compile_config& config)
    : context(path), full_path(path), file(file), parser(&p)
    {
        using namespace boost::wave;

        auto lang = support_cpp | support_option_variadics | support_option_long_long
                    | support_option_insert_whitespace | support_option_single_line;
        context.set_language(language_support(lang));

        config.setup_context(context);

        // need to open in binary mode as libclang does it (apparently)
        // otherwise the offsets are incompatible under Windows
        std::filebuf filebuf;
        filebuf.open(path, std::ios_base::in | std::ios_base::binary);
        assert(filebuf.is_open());

        source =
            std::string(std::istreambuf_iterator<char>(&filebuf), std::istreambuf_iterator<char>());
        if (source.back() != '\n')
            source.push_back('\n');

        parse_comments(p, full_path, source);
    }
};

detail::context& detail::tokenizer_access::get_context(translation_unit& tu)
{
    return tu.pimpl_->context;
}

const std::string& detail::tokenizer_access::get_source(translation_unit& tu)
{
    return tu.pimpl_->source;
}

translation_unit::translation_unit(translation_unit&& other) STANDARDESE_NOEXCEPT
    : pimpl_(std::move(other.pimpl_))
{
}

translation_unit::~translation_unit() STANDARDESE_NOEXCEPT
{
}

translation_unit& translation_unit::operator=(translation_unit&& other) STANDARDESE_NOEXCEPT
{
    pimpl_ = std::move(other.pimpl_);
    return *this;
}

const parser& translation_unit::get_parser() const STANDARDESE_NOEXCEPT
{
    return *pimpl_->parser;
}

const cpp_name& translation_unit::get_path() const STANDARDESE_NOEXCEPT
{
    return pimpl_->full_path;
}

CXFile translation_unit::get_cxfile() const STANDARDESE_NOEXCEPT
{
    auto file = clang_getFile(get_cxunit(), get_path().c_str());
    detail::validate(file);
    return file;
}

cpp_file& translation_unit::get_file() STANDARDESE_NOEXCEPT
{
    return *pimpl_->file;
}

const cpp_file& translation_unit::get_file() const STANDARDESE_NOEXCEPT
{
    return *pimpl_->file;
}

CXTranslationUnit translation_unit::get_cxunit() const STANDARDESE_NOEXCEPT
{
    return pimpl_->file->get_cxunit();
}

const cpp_entity_registry& translation_unit::get_registry() const STANDARDESE_NOEXCEPT
{
    return pimpl_->parser->get_entity_registry();
}

namespace
{
    std::string include_guard(const translation_unit& tu, const std::string& source)
    {
        if (!clang_isFileMultipleIncludeGuarded(tu.get_cxunit(), tu.get_cxfile()))
            return "";

        auto ptr = source.c_str();
        while (true)
        {
            std::string cur_line;
            for (; *ptr && *ptr != '\n'; ++ptr)
                cur_line += *ptr;
            ++ptr;

            if (cur_line.empty() || cur_line.compare(0, 2, "/**") == 0
                || cur_line.compare(0, 2, "//") == 0)
                // comment line
                continue;
            else if (cur_line[0] != '#')
                break;

            if (cur_line == "#pragma once")
                return "";
            else if (cur_line.compare(0, 3, "#if") == 0)
                // wait for macro
                continue;
            else if (cur_line.compare(0, std::strlen("#define"), "#define") == 0)
            {
                auto macro = cur_line.c_str() + std::strlen("#define");
                while (*macro && std::isspace(*macro))
                    ++macro;

                tu.get_parser().get_logger()->debug("detected include guard macro '{}'", macro);
                return macro;
            }
        }

        tu.get_parser().get_logger()->warn("unable to find include guard for file '{}'",
                                           tu.get_path().c_str());
        return "";
    }

    void register_macro(detail::context& context, translation_unit& tu, cpp_cursor macro)
    {
        auto definition = detail::get_cmd_definition(tu, macro);
        auto registered = context.add_macro_definition(definition);
        if (registered && tu.get_parser().get_logger()->level() <= spdlog::level::debug)
            tu.get_parser().get_logger()->debug("registered macro '{}'", definition);
    }
}

translation_unit::translation_unit(const parser& par, const char* path, cpp_file* file,
                                   const compile_config& config)
: pimpl_(new impl(par, path, file, config))
{
    detail::scope_stack stack(pimpl_->file);
    auto                guard = include_guard(*this, pimpl_->source);

    detail::visit_tu(get_cxunit(), get_cxfile(),
                     [&](cpp_cursor cur, cpp_cursor parent) {
                         stack.pop_if_needed(parent);

                         if (clang_getCursorSemanticParent(cur) != parent
                             && clang_getCursorSemanticParent(cur) != cpp_cursor())
                             // out of class definition, some weird other stuff with extern templates, implicit dtors
                             return CXChildVisit_Continue;

                         try
                         {
                             if (clang_getCursorKind(cur) == CXCursor_MacroExpansion)
                                 // register macro here as well
                                 // because of the heuristic in the actual macro definition visitor
                                 // it won't catch internal macros, even if they're used
                                 register_macro(pimpl_->context, *this,
                                                clang_getCursorReferenced(cur));
                             else if (clang_getCursorKind(cur) == CXCursor_Namespace
                                      || clang_getCursorKind(cur) == CXCursor_LinkageSpec
                                      || is_full_specialization(*this, cur)
                                      || cur == clang_getCanonicalCursor(
                                                    cur)) // only parse the canonical cursor
                             {
                                 if (get_parser().get_logger()->level() <= spdlog::level::debug)
                                 {
                                     auto location = source_location(cur);
                                     get_parser()
                                         .get_logger()
                                         ->debug("parsing entity '{}' ({}:{}) of type '{}'",
                                                 string(clang_getCursorDisplayName(cur)).c_str(),
                                                 location.file_name, location.line,
                                                 string(clang_getCursorKindSpelling(
                                                            clang_getCursorKind(cur)))
                                                     .c_str());
                                 }

                                 auto entity =
                                     cpp_entity::try_parse(*this, cur, stack.cur_parent());
                                 if (!entity)
                                     return CXChildVisit_Continue;
                                 else if (entity->get_entity_type()
                                              == cpp_entity::macro_definition_t
                                          && entity->get_name() == guard.c_str())
                                 {
                                     get_parser()
                                         .get_logger()
                                         ->debug("skipping include guard macro '{}'",
                                                 entity->get_name().c_str());
                                     return CXChildVisit_Continue;
                                 }

                                 get_parser().get_entity_registry().register_entity(*entity);

                                 auto container = stack.add_entity(std::move(entity), parent);
                                 if (container)
                                     return CXChildVisit_Recurse;
                             }
                             else
                             {
                                 if (get_parser().get_logger()->level() <= spdlog::level::debug)
                                 {
                                     auto location = source_location(cur);
                                     get_parser()
                                         .get_logger()
                                         ->debug("rejected entity '{}' ({}:{}) of type '{}'",
                                                 string(clang_getCursorDisplayName(cur)).c_str(),
                                                 location.file_name, location.line,
                                                 string(clang_getCursorKindSpelling(
                                                            clang_getCursorKind(cur)))
                                                     .c_str());
                                 }

                                 get_parser().get_entity_registry().register_alternative(cur);
                             }

                             return CXChildVisit_Continue;
                         }
                         catch (parse_error& ex)
                         {
                             if (ex.get_severity() == severity::warning)
                                 get_parser().get_logger()->warn("when parsing '{}' ({}:{}): {}",
                                                                 ex.get_location().entity_name,
                                                                 ex.get_location().file_name,
                                                                 ex.get_location().line, ex.what());
                             else
                                 get_parser().get_logger()->error("when parsing '{}' ({}:{}): {}",
                                                                  ex.get_location().entity_name,
                                                                  ex.get_location().file_name,
                                                                  ex.get_location().line,
                                                                  ex.what());
                             return CXChildVisit_Continue;
                         }
                         catch (boost::wave::cpp_exception& ex)
                         {
                             using namespace boost::wave;
                             if (ex.get_errorcode() == preprocess_exception::alreadydefined_name
                                 || ex.get_errorcode() == preprocess_exception::illegal_redefinition
                                 || ex.get_errorcode() == preprocess_exception::macro_redefinition
                                 || ex.get_errorcode() == preprocess_exception::warning_directive)
                                 return CXChildVisit_Continue;
                             else if (!is_recoverable(ex))
                                 throw;
                             else if (ex.get_severity() >= util::severity_error)
                                 get_parser()
                                     .get_logger()
                                     ->error("when parsing '{}' ({}:{}): {} (Boost.Wave)",
                                             ex.get_related_name(), ex.file_name(), ex.line_no(),
                                             preprocess_exception::error_text(ex.get_errorcode()));
                             else
                                 get_parser()
                                     .get_logger()
                                     ->warn("when parsing '{}' ({}:{}): {} (Boost.Wave)",
                                            ex.get_related_name(), ex.file_name(), ex.line_no(),
                                            preprocess_exception::error_text(ex.get_errorcode()));
                             return CXChildVisit_Continue;
                         }
                     },
                     [&](cpp_cursor macro) {
                         if (detail::parse_name(macro).c_str()[0] != '_')
                             // horrible heuristic to prevent including tons and tons of standard library macros
                             // if macro starts with an underscore, don't register
                             register_macro(pimpl_->context, *this, macro);
                     });
}

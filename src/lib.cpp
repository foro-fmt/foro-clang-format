//===-- clang-format/ClangFormat.cpp - Clang format tool ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements a clang-format tool that automatically formats
/// (fragments of) C++ code.
///
//===----------------------------------------------------------------------===//

// ----------------------------------------------------------------------------
// This file also uses the wasm port (src/lib.cc) from
// https://github.com/wasm-fmt/clang-format as a reference. Copyright (c) 2023
// wasm-fmt The original license is
// https://github.com/wasm-fmt/clang-format/blob/80aa6c2fa728927dcc21d4fb7bee1e2c2c6853ed/LICENSE
// ----------------------------------------------------------------------------

#include <fstream>

#include "lib.h"
#include "match_file_path.cpp"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Format/Format.h"
#include "clang/Rewrite/Core/Rewriter.h"

using namespace llvm;
using clang::tooling::Replacements;

static std::string FallbackStyle{clang::format::DefaultFallbackStyle};

static unsigned Cursor{0};

static bool SortIncludes{false};

static std::string QualifierAlignment{""};

static auto Ok(const std::string content) -> Result {
    return {false, std::move(content)};
}

static auto Err(const std::string content) -> Result {
    return {true, std::move(content)};
}

namespace clang {
namespace format {

static FileID createInMemoryFile(StringRef FileName, MemoryBufferRef Source,
                                 SourceManager &Sources, FileManager &Files,
                                 llvm::vfs::InMemoryFileSystem *MemFS) {
    MemFS->addFileNoOwn(FileName, 0, Source);
    auto File = Files.getOptionalFileRef(FileName);
    assert(File && "File not added to MemFS?");
    return Sources.createFileID(*File, SourceLocation(), SrcMgr::C_User);
}

static auto fillRanges(MemoryBuffer *Code, std::vector<tooling::Range> &Ranges)
    -> void {
    Ranges.push_back(tooling::Range(0, Code->getBuffer().size()));
}

static auto isPredefinedStyle(StringRef style) -> bool {
    return StringSwitch<bool>(style.lower())
        .Cases("llvm", "chromium", "mozilla", "google", "webkit", "gnu",
               "microsoft", "none", "file", true)
        .Default(false);
}

static auto format_range(const std::unique_ptr<llvm::MemoryBuffer> code,
                         const std::string assumedFileName,
                         const std::string style,
                         std::vector<tooling::Range> ranges) -> Result {
    StringRef BufStr = code->getBuffer();

    const char *InvalidBOM = SrcMgr::ContentCache::getInvalidBOM(BufStr);

    if (InvalidBOM) {
        std::stringstream err;
        err << "encoding with unsupported byte order mark \"" << InvalidBOM
            << "\" detected.";

        return Err(err.str());
    }

    StringRef AssumedFileName = assumedFileName;
    if (AssumedFileName.empty()) {
        AssumedFileName = "<stdin>";
    }

    llvm::Expected<FormatStyle> FormatStyle =
        getStyle(style, AssumedFileName, FallbackStyle);

    if (!FormatStyle) {
        std::string err = llvm::toString(FormatStyle.takeError());
        return Err(err);
    }

    StringRef QualifierAlignmentOrder = QualifierAlignment;

    FormatStyle->QualifierAlignment =
        StringSwitch<FormatStyle::QualifierAlignmentStyle>(
            QualifierAlignmentOrder.lower())
            .Case("right", FormatStyle::QAS_Right)
            .Case("left", FormatStyle::QAS_Left)
            .Default(FormatStyle->QualifierAlignment);

    if (FormatStyle->QualifierAlignment == FormatStyle::QAS_Left) {
        FormatStyle->QualifierOrder = {"const", "volatile", "type"};
    } else if (FormatStyle->QualifierAlignment == FormatStyle::QAS_Right) {
        FormatStyle->QualifierOrder = {"type", "const", "volatile"};
    } else if (QualifierAlignmentOrder.contains("type")) {
        FormatStyle->QualifierAlignment = FormatStyle::QAS_Custom;
        SmallVector<StringRef> Qualifiers;
        QualifierAlignmentOrder.split(Qualifiers, " ", /*MaxSplit=*/-1,
                                      /*KeepEmpty=*/false);
        FormatStyle->QualifierOrder = {Qualifiers.begin(), Qualifiers.end()};
    }

    FormatStyle->SortIncludes.Enabled = SortIncludes;
    FormatStyle->SortIncludes.IgnoreCase = false;

    unsigned CursorPosition = Cursor;
    Replacements Replaces =
        sortIncludes(*FormatStyle, code->getBuffer(), ranges, AssumedFileName,
                     &CursorPosition);

    // To format JSON insert a variable to trick the code into thinking its
    // JavaScript.
    if (FormatStyle->isJson() && !FormatStyle->DisableFormat) {
        auto err = Replaces.add(tooling::Replacement(
            tooling::Replacement(AssumedFileName, 0, 0, "x = ")));
        if (err)
            return Err("Bad Json variable insertion");
    }

    auto ChangedCode =
        cantFail(tooling::applyAllReplacements(code->getBuffer(), Replaces));

    // Get new affected ranges after sorting `#includes`.
    ranges = tooling::calculateRangesAfterReplacements(Replaces, ranges);
    FormattingAttemptStatus Status;
    Replacements FormatChanges =
        reformat(*FormatStyle, ChangedCode, ranges, AssumedFileName, &Status);
    Replaces = Replaces.merge(FormatChanges);

    return Ok(
        cantFail(tooling::applyAllReplacements(code->getBuffer(), Replaces)));
}

static auto format_range(const std::string str,
                         const std::string assumedFileName,
                         const std::string style, const bool is_line_range,
                         const std::vector<unsigned> ranges) -> Result {
    ErrorOr<std::unique_ptr<MemoryBuffer>> CodeOrErr =
        MemoryBuffer::getMemBuffer(str);

    if (std::error_code EC = CodeOrErr.getError()) {
        return Err(EC.message());
    }
    std::unique_ptr<llvm::MemoryBuffer> Code = std::move(CodeOrErr.get());
    if (Code->getBufferSize() == 0)
        return Ok(""); // Empty files are formatted correctly.

    std::vector<tooling::Range> Ranges;

    if (ranges.empty()) {
        fillRanges(Code.get(), Ranges);
        return format_range(std::move(Code), assumedFileName, style,
                            std::move(Ranges));
    }

    IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem(
        new llvm::vfs::InMemoryFileSystem);
    FileManager Files(FileSystemOptions(), InMemoryFileSystem);
    DiagnosticOptions DiagOpts;
    DiagnosticsEngine Diagnostics(
        IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs), DiagOpts);
    SourceManager Sources(Diagnostics, Files);
    FileID ID = createInMemoryFile("<irrelevant>", *Code, Sources, Files,
                                   InMemoryFileSystem.get());

    if (is_line_range) {
        for (auto FromLine = begin(ranges); FromLine < end(ranges);
             FromLine += 2) {
            auto ToLine = FromLine + 1;

            SourceLocation Start = Sources.translateLineCol(ID, *FromLine, 1);
            SourceLocation End =
                Sources.translateLineCol(ID, *ToLine, UINT_MAX);
            if (Start.isInvalid() || End.isInvalid()) {
                return Err("invalid line number");
            }
            unsigned Offset = Sources.getFileOffset(Start);
            unsigned Length = Sources.getFileOffset(End) - Offset;
            Ranges.push_back(tooling::Range(Offset, Length));
        }
    } else {
        if (ranges.size() > 2 && ranges.size() % 2 != 0) {
            return Err("number of -offset and -length arguments must match");
        }

        if (ranges.size() == 1) {
            auto offset = begin(ranges);
            if (*offset >= Code->getBufferSize()) {
                std::stringstream err;
                err << "offset " << *offset << " is outside the file";
                return Err(err.str());
            }
            SourceLocation Start =
                Sources.getLocForStartOfFile(ID).getLocWithOffset(*offset);
            SourceLocation End = Sources.getLocForEndOfFile(ID);

            unsigned Offset = Sources.getFileOffset(Start);
            unsigned Length = Sources.getFileOffset(End) - Offset;

            Ranges.push_back(tooling::Range(Offset, Length));
        } else {
            for (auto offset = begin(ranges); offset < end(ranges);
                 offset += 2) {
                auto length = offset + 1;

                if (*offset >= Code->getBufferSize()) {
                    std::stringstream err;
                    err << "offset " << *offset << " is outside the file";
                    return Err(err.str());
                }

                unsigned end = *offset + *length;
                if (end > Code->getBufferSize()) {
                    std::stringstream err;
                    err << "invalid length " << *length << ", offset + length ("
                        << end << ") is outside the file.";
                    return Err(err.str());
                }

                SourceLocation Start =
                    Sources.getLocForStartOfFile(ID).getLocWithOffset(*offset);
                SourceLocation End = Start.getLocWithOffset(*length);

                unsigned Offset = Sources.getFileOffset(Start);
                unsigned Length = Sources.getFileOffset(End) - Offset;

                Ranges.push_back(tooling::Range(Offset, Length));
            }
        }
    }

    return format_range(std::move(Code), assumedFileName, style,
                        std::move(Ranges));
}

static auto format(const std::string str, const std::string assumedFileName,
                   const std::string style) -> Result {
    ErrorOr<std::unique_ptr<MemoryBuffer>> CodeOrErr =
        MemoryBuffer::getMemBuffer(str);

    if (std::error_code EC = CodeOrErr.getError()) {
        return Err(EC.message());
    }
    std::unique_ptr<llvm::MemoryBuffer> Code = std::move(CodeOrErr.get());
    if (Code->getBufferSize() == 0)
        return Ok(""); // Empty files are formatted correctly.

    std::vector<tooling::Range> Ranges;
    fillRanges(Code.get(), Ranges);

    return format_range(std::move(Code), assumedFileName, style,
                        std::move(Ranges));
}

using String = SmallString<128>;
static String IgnoreDir;
static String PrevDir;
static SmallVector<String> Patterns;

static bool is_ignored(StringRef FilePath) {
    using namespace llvm::sys::fs;
    if (!is_regular_file(FilePath))
        return false;

    String Path;
    String AbsPath{FilePath};

    using namespace llvm::sys::path;
    make_absolute(AbsPath);
    remove_dots(AbsPath, /*remove_dot_dot=*/true);

    if (StringRef Dir{parent_path(AbsPath)}; PrevDir != Dir) {
        PrevDir = Dir;

        for (;;) {
            Path = Dir;
            append(Path, ".clang-format-ignore");
            if (is_regular_file(Path))
                break;
            Dir = parent_path(Dir);
            if (Dir.empty())
                return false;
        }

        IgnoreDir = convert_to_slash(Dir);

        std::ifstream IgnoreFile{Path.c_str()};
        if (!IgnoreFile.good())
            return false;

        Patterns.clear();

        for (std::string Line; std::getline(IgnoreFile, Line);) {
            if (const auto Pattern{StringRef{Line}.trim()};
                // Skip empty and comment lines.
                !Pattern.empty() && Pattern[0] != '#') {
                Patterns.push_back(Pattern);
            }
        }
    }

    if (IgnoreDir.empty())
        return false;

    const auto Pathname{convert_to_slash(AbsPath)};
    for (const auto &Pat : Patterns) {
        const bool IsNegated = Pat[0] == '!';
        StringRef Pattern{Pat};
        if (IsNegated)
            Pattern = Pattern.drop_front();

        if (Pattern.empty())
            continue;

        Pattern = Pattern.ltrim();

        // `Pattern` is relative to `IgnoreDir` unless it starts with a slash.
        // This doesn't support patterns containing drive names (e.g. `C:`).
        if (Pattern[0] != '/') {
            Path = IgnoreDir;
            append(Path, Style::posix, Pattern);
            remove_dots(Path, /*remove_dot_dot=*/true, Style::posix);
            Pattern = Path;
        }

        if (clang::format::matchFilePath(Pattern, Pathname) == !IsNegated)
            return true;
    }

    return false;
}

} // namespace format
} // namespace clang

auto version() -> std::string {
    return clang::getClangToolFullVersion("clang-format");
}

auto format(const std::string str, const std::string assumedFileName,
            const std::string style) -> Result {
    return clang::format::format(str, assumedFileName, style);
}

auto format_byte(const std::string str, const std::string assumedFileName,
                 const std::string style, const std::vector<unsigned> ranges)
    -> Result {
    return clang::format::format_range(str, assumedFileName, style, false,
                                       std::move(ranges));
}

auto format_line(const std::string str, const std::string assumedFileName,
                 const std::string style, const std::vector<unsigned> ranges)
    -> Result {
    return clang::format::format_range(str, assumedFileName, style, true,
                                       std::move(ranges));
}

auto set_fallback_style(const std::string style) -> void {
    FallbackStyle = style;
}

auto set_sort_includes(const bool sort) -> void { SortIncludes = sort; }

auto dump_config(const std::string style, const std::string FileName,
                 const std::string code) -> Result {
    llvm::Expected<clang::format::FormatStyle> FormatStyle =
        clang::format::getStyle(style, FileName, FallbackStyle, code);
    if (!FormatStyle) {
        return Err(llvm::toString(FormatStyle.takeError()));
    }
    std::string Config = clang::format::configurationAsText(*FormatStyle);
    return Ok(Config);
}

auto is_ignored(const std::string path) -> bool {
    return clang::format::is_ignored(path);
}

auto defaultFormatStyle() -> std::string {
    return clang::format::DefaultFormatStyle;
}

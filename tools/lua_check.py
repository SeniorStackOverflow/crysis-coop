#!/usr/bin/env python3
"""
Crysis Coop - minimal Lua 5.0/5.1-compatible syntax sanity checker.

Usage: python lua_check.py <file-or-dir> [<file-or-dir> ...]
Exit code: 1 if any file reported an error, 0 otherwise.

Checks:
  - Tokenizes the source, correctly skipping comments and strings, so that
    keywords/braces found *inside* them are never counted.
  - Block-keyword balance: function/if/do/repeat must close with matching
    end/until.
  - Bracket balance for (), {}, [].
  - Warns (does not error) about Lua-5.1-only syntax that is illegal in the
    5.0/5.1-compatible subset this mod targets: '#' length operator,
    '...' varargs, '%' arithmetic operator, '//' (not valid Lua at all,
    but sometimes crept in from C-style edits).

This is NOT a real Lua parser - it is a structural/lexical sanity check
intended to catch obvious mistakes (typos, unbalanced blocks, mismatched
brackets) before the file is dropped into the game.
"""

import sys
import os


class Token(object):
    __slots__ = ("kind", "text", "line")

    def __init__(self, kind, text, line):
        self.kind = kind      # 'word', 'op', 'string', 'comment', 'other'
        self.text = text
        self.line = line


def tokenize(src):
    """Yield Token objects for the parts of `src` that matter to us.
    Strings and comments are emitted as single tokens of kind 'string' /
    'comment' (their contents are never inspected for keywords/brackets).
    """
    tokens = []
    i = 0
    n = len(src)
    line = 1

    def count_lines(s):
        return s.count("\n")

    while i < n:
        c = src[i]

        # newline bookkeeping is handled per-consumed-chunk below
        if c == "\n":
            line += 1
            i += 1
            continue

        # line comment or long comment
        if c == "-" and i + 1 < n and src[i + 1] == "-":
            # check for long comment --[[ or --[=[ ...
            j = i + 2
            eq = 0
            k = j
            if k < n and src[k] == "[":
                k2 = k + 1
                eqcount = 0
                while k2 < n and src[k2] == "=":
                    eqcount += 1
                    k2 += 1
                if k2 < n and src[k2] == "[":
                    # long comment
                    closer = "]" + ("=" * eqcount) + "]"
                    end = src.find(closer, k2 + 1)
                    start_line = line
                    if end == -1:
                        chunk = src[i:]
                        line += count_lines(chunk)
                        tokens.append(Token("comment", chunk, start_line))
                        i = n
                        continue
                    else:
                        end_full = end + len(closer)
                        chunk = src[i:end_full]
                        line += count_lines(chunk)
                        tokens.append(Token("comment", chunk, start_line))
                        i = end_full
                        continue
            # plain line comment: goes to end of line
            end = src.find("\n", i)
            if end == -1:
                tokens.append(Token("comment", src[i:], line))
                i = n
            else:
                tokens.append(Token("comment", src[i:end], line))
                i = end  # newline handled by main loop next iteration
            continue

        # long string [[ ]] or [=[ ]=]
        if c == "[":
            k = i + 1
            eqcount = 0
            while k < n and src[k] == "=":
                eqcount += 1
                k += 1
            if k < n and src[k] == "[":
                closer = "]" + ("=" * eqcount) + "]"
                end = src.find(closer, k + 1)
                start_line = line
                if end == -1:
                    chunk = src[i:]
                    line += count_lines(chunk)
                    tokens.append(Token("string", chunk, start_line))
                    i = n
                    continue
                else:
                    end_full = end + len(closer)
                    chunk = src[i:end_full]
                    line += count_lines(chunk)
                    tokens.append(Token("string", chunk, start_line))
                    i = end_full
                    continue
            # else fall through: it's just a '[' bracket, handled below

        # short strings "..." or '...'
        if c == '"' or c == "'":
            quote = c
            j = i + 1
            start_line = line
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == quote:
                    j += 1
                    break
                if src[j] == "\n":
                    # unterminated string on this line; stop here (error-ish,
                    # but we just emit what we have so far as a string token)
                    break
                j += 1
            chunk = src[i:j]
            line += count_lines(chunk)
            tokens.append(Token("string", chunk, start_line))
            i = j
            continue

        # identifiers / keywords
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            tokens.append(Token("word", src[i:j], line))
            i = j
            continue

        # everything else: operators/punctuation, one char at a time
        # (multi-char operators like '...' / '..' are handled specially so
        # the 5.1-only-syntax warning can detect '...' as a unit)
        if src[i:i + 3] == "...":
            tokens.append(Token("op", "...", line))
            i += 3
            continue
        if src[i:i + 2] == "//":
            tokens.append(Token("op", "//", line))
            i += 2
            continue

        tokens.append(Token("other", c, line))
        i += 1

    return tokens


OPENERS = {"function": "end", "if": "end", "do": "end", "repeat": "until"}
CLOSERS = {"end": "function/if/do", "until": "repeat"}

# words that can appear before 'do'/'end' etc. but must NOT themselves push
# a new block (for/while push via their trailing 'do', which is a separate
# 'do' token already handled generically)
NON_PUSHING = set(["then", "else", "elseif", "for", "while", "function"])
# NOTE: 'function' IS a pusher (see OPENERS); listed here only for clarity
NON_PUSHING.discard("function")


def check_file(path):
    errors = []
    warnings = []

    try:
        with open(path, "rb") as f:
            raw = f.read()
    except IOError as e:
        return (["cannot read file: %s" % e], [])

    # decode leniently; stock Crysis Lua is plain ASCII
    try:
        src = raw.decode("utf-8")
    except UnicodeDecodeError:
        src = raw.decode("latin-1")

    tokens = tokenize(src)

    # ---- bracket balance -------------------------------------------------
    pairs = {")": "(", "}": "{", "]": "["}
    openers_stack = []
    for t in tokens:
        if t.kind != "other":
            continue
        if t.text in "({[":
            openers_stack.append((t.text, t.line))
        elif t.text in ")}]":
            expected = pairs[t.text]
            if not openers_stack:
                errors.append("line %d: unmatched closing '%s'" % (t.line, t.text))
            else:
                top, topline = openers_stack.pop()
                if top != expected:
                    errors.append(
                        "line %d: mismatched '%s' (expected closer for '%s' opened at line %d)"
                        % (t.line, t.text, top, topline)
                    )
    for (opch, opline) in openers_stack:
        errors.append("line %d: unclosed '%s'" % (opline, opch))

    # ---- block keyword balance -------------------------------------------
    block_stack = []  # list of (openerWord, line)
    for t in tokens:
        if t.kind != "word":
            continue
        w = t.text
        if w in OPENERS:
            block_stack.append((w, t.line))
        elif w in CLOSERS:
            if not block_stack:
                errors.append(
                    "line %d: '%s' with no matching opener (depth went negative)" % (t.line, w)
                )
                continue
            opener, openline = block_stack[-1]
            expected_closer = OPENERS[opener]
            if expected_closer != w:
                errors.append(
                    "line %d: '%s' does not match opener '%s' opened at line %d (expected '%s')"
                    % (t.line, w, opener, openline, expected_closer)
                )
                # don't pop on mismatch guess-fix: pop anyway to keep going
                block_stack.pop()
            else:
                block_stack.pop()

    if block_stack:
        opener, openline = block_stack[-1]
        errors.append(
            "end of file: %d unclosed block(s); last unclosed '%s' opened at line %d"
            % (len(block_stack), opener, openline)
        )

    # ---- 5.1-only syntax warnings -----------------------------------------
    for t in tokens:
        if t.kind == "other" and t.text == "#":
            warnings.append("line %d: '#' length operator (not valid Lua 5.0)" % t.line)
        elif t.kind == "other" and t.text == "%":
            warnings.append("line %d: '%%' arithmetic operator (not valid Lua 5.0)" % t.line)
        elif t.kind == "op" and t.text == "...":
            warnings.append("line %d: '...' varargs (not valid Lua 5.0)" % t.line)
        elif t.kind == "op" and t.text == "//":
            warnings.append("line %d: '//' is not valid Lua syntax" % t.line)

    return (errors, warnings)


def iter_lua_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                for fn in files:
                    if fn.lower().endswith(".lua"):
                        yield os.path.join(root, fn)
        else:
            yield p


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: python lua_check.py <file-or-dir> [...]\n")
        return 1

    had_error = False
    for path in iter_lua_files(argv[1:]):
        errors, warnings = check_file(path)
        if errors or warnings:
            print("== %s ==" % path)
            for e in errors:
                print("  ERROR: %s" % e)
            for w in warnings:
                print("  WARNING: %s" % w)
        else:
            print("== %s == OK" % path)
        if errors:
            had_error = True

    return 1 if had_error else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

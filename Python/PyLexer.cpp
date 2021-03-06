/******************************************************************************
 * Copyright (c) 2014-2016 Leandro T. C. Melo (ltcmelo@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 *****************************************************************************/

/*--------------------------*/
/*--- The UaiSo! Project ---*/
/*--------------------------*/

#include "Python/PyLexer.h"
#include "Python/PyKeywords.h"
#include "Python/PyLang.h"
#include "Parsing/Lexeme.h"
#include "Parsing/ParsingContext.h"
#include "Common/Assert.h"
#include "Common/Trace__.h"
#include <cctype>
#include <iostream>

#define TRACE_NAME "PyLexer"

using namespace uaiso;

namespace {

const PyLang pyLang;

} // anonymous

PyLexer::PyLexer()
    : bits_(0)
{
    bit_.atLineStart_ = true;
    indentStack_.push(0);
}

PyLexer::~PyLexer()
{}

namespace {

// The ASCII value of the operator/delimiter character is used to index a
// position corresponding to its token. "Composite" tokens are one adjacent
// to another.
const int oprtrDelimTable[] =
{
    TK_PERCENT, TK_PERCENT_EQ, // 0, 1
    TK_AMPER, TK_AMPER_EQ, // 2, 3
    TK_STAR, TK_STAR_EQ, TK_STAR_STAR, TK_STAR_STAR_EQ, // 4...7
    TK_PLUS, TK_PLUS_EQ, // 8, 9
    TK_MINUS, TK_MINUS_EQ, // 10, 11
    TK_SLASH, TK_SLASH_EQ, TK_SLASH_SLASH, TK_SLASH_SLASH_EQ, // 12...15
    TK_LS, TK_LS_EQ, TK_LS_LS, TK_LS_LS_EQ, // 16...19
    TK_EQ, TK_EQ_EQ, // 20, 21
    TK_GR, TK_GR_EQ, TK_GR_GR, TK_GR_GR_EQ, // 22...25
    TK_CARET, TK_CARET_EQ, // 26, 27
    TK_PIPE, TK_PIPE_EQ, // 28, 29

    // Indexing
    0, 0, 0, 0, 0, 0, 0,
    0,                                                      // 37 (%)
    2,                                                      // 38 (&)
    0, 0, 0,
    4,                                                      // 42 (*)
    8,                                                      // 43 (+)
    0,
    10,                                                     // 45 (-)
    0,
    12,                                                     // 47 (/)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    16,                                                     // 60 (<)
    20,                                                     // 61 (=)
    22,                                                     // 62 (>)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    26,                                                     // 94 (^)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    28                                                      // 124 (|)
};

} // namespace anonymous

Token PyLexer::lex()
{
    Token tk = TK_INVALID;
    updatePos();

LexNextToken:
    UAISO_ASSERT(!indentStack_.empty(), return tk);

    mark_ = curr_; // Mark the start of the upcoming token.

    char ch = peekChar();

    // If at the start of a line, ensure indentation rules.
    if (bit_.atLineStart_) {
        size_t count = 0;
        if (ch == '\t' || ch == ' ') {
            do {
                ch = consumeCharPeekNext();
                ++count;
            } while (ch == '\t' || ch == ' ');
        }
        col_ += count;
        mark_ += count;

        // Store whether we're in completion area, to avoid calling
        // the function and re-calculating line and columns again.
        bool completionArea = inCompletionArea();

        // Blank or comment lines have no effect.
        if (ch && ((ch != '#' && ch != '\n') || completionArea)) {
            bit_.indent_ += count;
            size_t largest = indentStack_.top();
            if (bit_.indent_ > largest) {
                // Relax completion triggering location. Otherwise,
                // and indent would be sent causing a parse error.
                if (completionArea) {
                    context_->clearStopMark();
                    return TK_COMPLETION;
                }

                // Indents happen one at a time, always.
                indentStack_.push(bit_.indent_);
                return TK_INDENT;
            }
            while (bit_.indent_ < largest) {
                // Dedents may "accumulate".
                indentStack_.pop();
                ++bit_.pendingDedent_;
                UAISO_ASSERT(!indentStack_.empty(), return tk);
                largest = indentStack_.top();
            }
        }
    }

    // Take care of pending dedents.
    if (bit_.pendingDedent_) {
        --bit_.pendingDedent_;
        return TK_DEDENT;
    }

    // The check whether we are at a completion point must be done after
    // spaces/indents/dedents are processed.
    if (maybeRealizeCompletion())
        return TK_COMPLETION;

    switch (ch) {
    case 0:
        if (indentStack_.top() > 0) {
            indentStack_.pop();
            return TK_DEDENT;
        }
        return TK_EOP;

    case '\n':
        bit_.indent_ = 0;
        consumeChar();
        handleNewLine();
        if (!bit_.atLineStart_ && !bit_.brackets_) {
            bit_.atLineStart_ = true;
            return TK_NEWLINE;
        }
        updatePos();
        goto LexNextToken;

    case '\t':
    case '\f':
    case ' ':
        skipSpaces(ch);
        goto LexNextToken;

    case '\\':
        ch = consumeCharPeekNext();
        if (ch != '\n')
            break;
        consumeChar();
        handleNewLine();
        updatePos();
        goto LexNextToken;

    case '"':
    case '\'':
        tk = lexStrLit(ch);
        context_->trackLexeme<StrLit>(mark_, curr_ - mark_, LineCol(line_, col_));
        break;

    case 'r':
    case 'R':
    case 'u':
    case 'U':
    case 'b':
    case 'B': {
        char next = peekChar(1);
        if (next == '"' || next == '\'') {
            consumeChar();
            tk = lexStrLit(next);
            context_->trackLexeme<StrLit>(mark_, curr_ - mark_, LineCol(line_, col_));
            break;
        }
        // Either a string literal or an identifier.
        if (next != ch && (next == 'r' || next == 'R')) {
            char next2 = peekChar(2);
            if (next2 == '"' || next2 == '\'') {
                consumeChar(1);
                tk = lexStrLit(next2);
                context_->trackLexeme<StrLit>(mark_, curr_ - mark_, LineCol(line_, col_));
                break;
            }
            tk = lexIdentOrKeyword(ch, &pyLang);
            break;
        }
        // Certainly an identifier.
        tk = lexIdentOrKeyword(ch, &pyLang);
        break;
    }

    case '.':
        if (std::isdigit(peekChar(1))) {
            tk = lexNumLit(ch, &pyLang);
            context_->trackLexeme<NumLit>(mark_, curr_ - mark_, LineCol(line_, col_));
            break;
        }
        ch = consumeCharPeekNext();
        if (ch == '.' && peekChar(1) == '.') {
            consumeChar(1);
            tk = TK_DOT_DOT_DOT;
            break;
        }
        tk = TK_DOT;
        break;

    case '*':
        tk = lexOprtrOrDelim(ch);
        context_->trackLexeme<Ident>(mark_, curr_ - mark_, LineCol(line_, col_));
        break;

    case '+':
    case '-':
    case '%':
    case '&':
    case '|':
    case '^':
    case '=':
    case '/':
    case '<':
    case '>':
        tk = lexOprtrOrDelim(ch);
        break;

    case '~':
        consumeChar();
        tk = TK_TILDE;
        break;

    case ',':
        consumeChar();
        tk = TK_COMMA;
        break;

    case ':':
        consumeChar();
        tk = TK_COLON;
        break;

    case ';':
        consumeChar();
        tk = TK_SEMICOLON;
        break;

    case '@':
        consumeChar();
        tk = TK_AT;
        break;

    case '(':
        ++bit_.brackets_;
        consumeChar();
        tk = TK_LPAREN;
        break;

    case ')':
        --bit_.brackets_;
        consumeChar();
        tk = TK_RPAREN;
        break;

    case '[':
        ++bit_.brackets_;
        consumeChar();
        tk = TK_LBRACKET;
        break;

    case ']':
        --bit_.brackets_;
        consumeChar();
        tk = TK_RBRACKET;
        break;

    case '{':
        ++bit_.brackets_;
        consumeChar();
        tk = TK_LBRACE;
        break;

    case '}':
        --bit_.brackets_;
        consumeChar();
        tk = TK_RBRACE;
        break;

    case '!':
        ch = consumeCharPeekNext();
        if (ch == '=') {
            consumeChar();
            tk = TK_EXCLAM_EQ;
            break;
        }
        // erro report
        break;

    case '#':
        ch = consumeCharPeekNext();
        while (ch && ch != '\n')
            ch = consumeCharPeekNext();
        if (context_->allowComments()) {
            tk = TK_COMMENT;
            // Immediately return the comment token. Otherwise, if we simply
            // break, we would mess with the line start logic.
            LineCol lineCol(line_, col_);
            context_->trackToken(tk, lineCol);
            context_->trackPhrase(tk, lineCol, curr_ - mark_);
            return tk;
        }
        goto LexNextToken;

    default:
        if (pyLang.isIdentFirstChar(ch)) {
            tk = lexIdentOrKeyword(ch, &pyLang);
            break;
        }

        if (std::isdigit(ch)) {
            tk = lexNumLit(ch, &pyLang);
            context_->trackLexeme<NumLit>(mark_, curr_ - mark_, LineCol(line_, col_));
            break;
        }

        // Don't know what this is.
        consumeChar();
        PRINT_TRACE("Unknown char %c at %d,%d\n", ch, 0, 0);
        break;
    }

    bit_.atLineStart_ = false;

    LineCol lineCol(line_, col_);
    context_->trackToken(tk, lineCol);
    context_->trackPhrase(tk, lineCol, curr_ - mark_);

    return tk;
}

Token PyLexer::lexStrLit(char& ch)
{
    UAISO_ASSERT(ch == '"' || ch == '\'', return TK_INVALID);

    const char quote = ch;

    // Check whether this is a triple-quoted string. If so, consume the input
    // until the last (third) quote to setup for base's string literal lex.
    bool triple = false;
    if (quote == peekChar(1) && quote == peekChar(2)) {
        ch = consumeCharPeekNext(1);
        triple = true;
    }

    Token tk = Base::lexStrLit(ch, triple, &pyLang);
    if (!triple)
        return tk;

    while (ch) {
        if (quote == ch) {
            ch = consumeCharPeekNext();
            if (quote == ch)
                break;
        }
        tk = lexStrLitEnd(ch, quote, triple, &pyLang);
    }

    if (ch)
        ch = consumeCharPeekNext();
    else
        context_->trackReport(Diagnostic::UnterminatedString, tokenLoc());

    return tk;
}

Token PyLexer::lexOprtrOrDelim(char& ch)
{
    // The "base" token for the operator/delimiter.
    const int base = oprtrDelimTable[ch];

    const char prev = ch;
    ch = consumeCharPeekNext();

    // A token like <=, >=, |=, etc. That means means base + 1.
    if (ch == '=') {
        consumeChar();
        return Token(oprtrDelimTable[base + 1]);
    }
    // Either a token like << or <<=, etc. That means base + 2 or +3.
    if (ch == prev) {
        ch = consumeCharPeekNext();
        if (ch == '=') {
            consumeChar();
            return Token(oprtrDelimTable[base + 3]);
        }
        return Token(oprtrDelimTable[base + 2]);
    }

    return Token(oprtrDelimTable[base]);
}

Token PyLexer::filterKeyword(const char* spell, size_t len) const
{
    return PyKeywords::filter(spell, len);
}

Token PyLexer::classifyIdent(char&)
{
    context_->trackLexeme<Ident>(mark_, curr_ - mark_, LineCol(line_, col_));

    return TK_IDENT;
}

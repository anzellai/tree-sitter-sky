// External scanner for Sky tree-sitter grammar.
//
// Sky uses indentation-sensitive layout (like Elm, Haskell).
// This scanner tracks indentation levels and emits three virtual tokens:
//
//   _virtual_open_section  (INDENT):  pushed when a new indented block begins
//   _virtual_end_decl      (NEWLINE): emitted between siblings at the same indent level
//   _virtual_end_section   (DEDENT):  emitted when indentation drops below current level
//
// Key design decisions:
//   - Leading commas and operators at line start suppress layout tokens
//   - Closing delimiters ) ] } on any line close sections
//   - "in" keyword detected via lookahead WITHOUT consuming characters
//   - Same-line content after layout keywords uses column tracking via get_column()

#include "tree_sitter/parser.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum TokenType {
  VIRTUAL_END_DECL,     // same-level separator (;)
  VIRTUAL_OPEN_SECTION, // open block ({)
  VIRTUAL_END_SECTION,  // close block (})
  BLOCK_COMMENT_CONTENT,
};

#define MAX_INDENT_STACK 64
#define MAX_RUNBACK 64

typedef struct {
  int indent_stack[MAX_INDENT_STACK];
  int stack_len;
  uint8_t runback[MAX_RUNBACK];
  int runback_len;
} Scanner;

void *tree_sitter_sky_external_scanner_create(void) {
  Scanner *s = calloc(1, sizeof(Scanner));
  s->indent_stack[0] = 0;
  s->stack_len = 1;
  s->runback_len = 0;
  return s;
}

void tree_sitter_sky_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_sky_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = (Scanner *)payload;
  unsigned size = 0;
  buffer[size++] = (char)s->stack_len;
  for (int i = 0; i < s->stack_len && size + 1 < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
    int val = s->indent_stack[i];
    buffer[size++] = (char)(val & 0xFF);
    buffer[size++] = (char)((val >> 8) & 0xFF);
  }
  if (size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = (char)s->runback_len;
  }
  for (int i = 0; i < s->runback_len && size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
    buffer[size++] = s->runback[i];
  }
  return size;
}

void tree_sitter_sky_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = (Scanner *)payload;
  s->stack_len = 1;
  s->indent_stack[0] = 0;
  s->runback_len = 0;

  if (length == 0) return;

  unsigned pos = 0;
  int slen = (unsigned char)buffer[pos++];
  if (slen > MAX_INDENT_STACK) slen = MAX_INDENT_STACK;
  s->stack_len = 0;
  for (int i = 0; i < slen && pos + 1 < length; i++) {
    int val = (unsigned char)buffer[pos] | ((unsigned char)buffer[pos + 1] << 8);
    pos += 2;
    if (s->stack_len < MAX_INDENT_STACK) {
      s->indent_stack[s->stack_len++] = val;
    }
  }
  if (s->stack_len == 0) {
    s->indent_stack[0] = 0;
    s->stack_len = 1;
  }
  if (pos < length) {
    int rlen = (unsigned char)buffer[pos++];
    if (rlen > MAX_RUNBACK) rlen = MAX_RUNBACK;
    s->runback_len = 0;
    for (int i = 0; i < rlen && pos < length; i++) {
      if (s->runback_len < MAX_RUNBACK) {
        s->runback[s->runback_len++] = (unsigned char)buffer[pos++];
      }
    }
  }
}

static int current_indent(Scanner *s) {
  return s->stack_len > 0 ? s->indent_stack[s->stack_len - 1] : 0;
}

// Check if a character starts a continuation token that should suppress
// layout decisions (leading commas, operators, pipes, closing delimiters).
static bool is_continuation_char(int32_t ch) {
  return ch == ',' || ch == '|' || ch == ')' || ch == ']' || ch == '}';
}

// Advance past whitespace (including newlines) to find the column and
// content of the next significant line. Returns the column of the first
// non-whitespace character after the last newline.
static int scan_to_content(TSLexer *lexer, bool *found_newline) {
  int col = 0;
  *found_newline = false;

  while (!lexer->eof(lexer)) {
    int32_t ch = lexer->lookahead;
    if (ch == '\n' || ch == '\r') {
      *found_newline = true;
      col = 0;
      lexer->advance(lexer, true);
      // Handle \r\n
      if (ch == '\r' && !lexer->eof(lexer) && lexer->lookahead == '\n') {
        lexer->advance(lexer, true);
      }
      continue;
    }
    if (ch == ' ') {
      if (*found_newline) col++;
      lexer->advance(lexer, true);
      continue;
    }
    if (ch == '\t') {
      if (*found_newline) col += 4;
      lexer->advance(lexer, true);
      continue;
    }
    // Skip comment-only lines (-- comments don't affect layout)
    if (*found_newline && ch == '-') {
      // Peek: is this a line comment?
      lexer->advance(lexer, true);
      if (!lexer->eof(lexer) && lexer->lookahead == '-') {
        // Skip to end of line
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
          lexer->advance(lexer, true);
        }
        continue; // will hit the \n handler above
      }
      // Single minus — not a comment. We consumed one char but that's ok
      // since we're in skip mode. The column is already set.
      break;
    }
    break;
  }
  return col;
}

// Check if lookahead matches "in" followed by whitespace/EOF (keyword, not identifier)
static bool lookahead_is_in_keyword(TSLexer *lexer) {
  if (lexer->eof(lexer) || lexer->lookahead != 'i') return false;
  // We need to peek ahead without consuming. Use mark_end to not extend the token.
  lexer->mark_end(lexer);
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 'n') return false;
  lexer->advance(lexer, false);
  // "in" must be followed by whitespace, EOF, or certain punctuation (not alphanumeric)
  if (lexer->eof(lexer)) return true;
  int32_t after = lexer->lookahead;
  return after == ' ' || after == '\n' || after == '\r' || after == '\t' ||
         after == '(' || after == ')' || after == '{' || after == '}' ||
         after == '[' || after == ']';
}

bool tree_sitter_sky_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *s = (Scanner *)payload;

  // ── 1. Drain runback buffer ──────────────────────────
  if (s->runback_len > 0) {
    uint8_t token_type = s->runback[0];
    for (int i = 1; i < s->runback_len; i++) {
      s->runback[i - 1] = s->runback[i];
    }
    s->runback_len--;

    if (token_type == 0 && valid_symbols[VIRTUAL_END_DECL]) {
      lexer->result_symbol = VIRTUAL_END_DECL;
      return true;
    }
    if (token_type == 1 && valid_symbols[VIRTUAL_END_SECTION]) {
      lexer->result_symbol = VIRTUAL_END_SECTION;
      return true;
    }
    s->runback_len = 0;
  }

  // ── 2. VIRTUAL_OPEN_SECTION ──────────────────────────
  // Fires after layout keywords (let, of, case body, etc.)
  if (valid_symbols[VIRTUAL_OPEN_SECTION]) {
    lexer->mark_end(lexer);
    if (lexer->eof(lexer)) return false;

    bool found_newline = false;
    int col = scan_to_content(lexer, &found_newline);

    if (lexer->eof(lexer)) return false;

    if (!found_newline) {
      // Same-line: use the lexer's column position.
      // tree-sitter provides get_column() for the current position.
      col = lexer->get_column(lexer);
      // Ensure we're strictly deeper than the current indent
      if (col <= current_indent(s)) {
        col = current_indent(s) + 1;
      }
    }

    if (s->stack_len < MAX_INDENT_STACK) {
      s->indent_stack[s->stack_len++] = col;
    }

    lexer->result_symbol = VIRTUAL_OPEN_SECTION;
    return true;
  }

  // ── 3. END_DECL / END_SECTION ────────────────────────
  if (!valid_symbols[VIRTUAL_END_DECL] && !valid_symbols[VIRTUAL_END_SECTION]) {
    return false;
  }

  lexer->mark_end(lexer);

  // At EOF, close remaining sections
  if (lexer->eof(lexer)) {
    if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
      s->stack_len--;
      lexer->result_symbol = VIRTUAL_END_SECTION;
      return true;
    }
    if (valid_symbols[VIRTUAL_END_DECL]) {
      lexer->result_symbol = VIRTUAL_END_DECL;
      return true;
    }
    return false;
  }

  // Scan forward to find the next line's indentation
  bool saw_newline = false;
  int indent = 0;

  while (true) {
    if (lexer->eof(lexer)) {
      if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
        s->stack_len--;
        lexer->result_symbol = VIRTUAL_END_SECTION;
        return true;
      }
      if (valid_symbols[VIRTUAL_END_DECL]) {
        lexer->result_symbol = VIRTUAL_END_DECL;
        return true;
      }
      return false;
    }

    int32_t ch = lexer->lookahead;

    if (ch == '\n' || ch == '\r') {
      saw_newline = true;
      indent = 0;
      lexer->advance(lexer, true);
      continue;
    }

    if (saw_newline && (ch == ' ' || ch == '\t')) {
      if (ch == ' ') indent++;
      else indent += 4;
      lexer->advance(lexer, true);
      continue;
    }

    if (!saw_newline) {
      // Same-line tokens: check for closing delimiters and "in" keyword
      if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
        if (ch == ')' || ch == ']' || ch == '}') {
          s->stack_len--;
          lexer->result_symbol = VIRTUAL_END_SECTION;
          return true;
        }
        // "in" keyword closes let sections — detect WITHOUT consuming
        if (lookahead_is_in_keyword(lexer)) {
          s->stack_len--;
          lexer->result_symbol = VIRTUAL_END_SECTION;
          return true;
        }
      }
      return false;
    }

    // Skip blank lines (content is another newline)
    if (ch == '\n' || ch == '\r') {
      continue;
    }

    // Skip comment-only lines
    if (ch == '-') {
      // Peek for line comment
      lexer->advance(lexer, true);
      if (!lexer->eof(lexer) && lexer->lookahead == '-') {
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
          lexer->advance(lexer, true);
        }
        continue;
      }
      // Not a comment — it's a minus sign or negative number. Use current indent.
      break;
    }

    // Found non-whitespace content
    break;
  }

  int cur = current_indent(s);

  // ── Continuation lines: suppress layout tokens ───────
  // Lines starting with commas, pipes, operators, or closing delimiters
  // are continuations of the previous expression, not new declarations.
  if (saw_newline && !lexer->eof(lexer)) {
    int32_t ch = lexer->lookahead;
    if (is_continuation_char(ch)) {
      return false;
    }

    // "in" keyword on a new line: close the let section
    if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
      if (lookahead_is_in_keyword(lexer)) {
        s->stack_len--;
        cur = current_indent(s);
        // Queue additional END_SECTIONs if we need to close multiple levels
        while (s->stack_len > 1 && indent < cur) {
          if (s->runback_len < MAX_RUNBACK) {
            s->runback[s->runback_len++] = 1;
          }
          s->stack_len--;
          cur = current_indent(s);
        }
        lexer->result_symbol = VIRTUAL_END_SECTION;
        return true;
      }
    }
  }

  // ── Dedent ───────────────────────────────────────────
  if (indent < cur && valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
    s->stack_len--;
    cur = current_indent(s);

    while (s->stack_len > 1 && indent < cur) {
      if (s->runback_len < MAX_RUNBACK) {
        s->runback[s->runback_len++] = 1; // VIRTUAL_END_SECTION
      }
      s->stack_len--;
      cur = current_indent(s);
    }

    if (indent == cur && s->runback_len < MAX_RUNBACK) {
      s->runback[s->runback_len++] = 0; // VIRTUAL_END_DECL
    }

    lexer->result_symbol = VIRTUAL_END_SECTION;
    return true;
  }

  // ── Same level ───────────────────────────────────────
  if (indent == cur && valid_symbols[VIRTUAL_END_DECL]) {
    lexer->result_symbol = VIRTUAL_END_DECL;
    return true;
  }

  // ── Dedent fallback ──────────────────────────────────
  if (indent <= cur && valid_symbols[VIRTUAL_END_DECL]) {
    lexer->result_symbol = VIRTUAL_END_DECL;
    return true;
  }

  return false;
}

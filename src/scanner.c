// External scanner for Sky tree-sitter grammar.
//
// Sky uses indentation-sensitive layout (like Elm, Haskell).
// This scanner tracks indentation levels and emits three virtual tokens:
//
//   _virtual_open_section  (INDENT):  pushed when a new indented block begins
//   _virtual_end_decl      (NEWLINE): emitted between siblings at the same indent level
//   _virtual_end_section   (DEDENT):  emitted when indentation drops below current level

#include "tree_sitter/parser.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum TokenType {
  VIRTUAL_END_DECL,
  VIRTUAL_OPEN_SECTION,
  VIRTUAL_END_SECTION,
  BLOCK_COMMENT_CONTENT,
};

#define MAX_INDENT_STACK 64
#define MAX_RUNBACK 64

typedef struct {
  int indent_stack[MAX_INDENT_STACK];
  int stack_len;
  uint8_t runback[MAX_RUNBACK];
  int runback_len;
  bool just_opened; // true after OPEN_SECTION, suppresses same-level END_DECL
} Scanner;

void *tree_sitter_sky_external_scanner_create(void) {
  Scanner *s = calloc(1, sizeof(Scanner));
  s->indent_stack[0] = 0;
  s->stack_len = 1;
  s->runback_len = 0;
  s->just_opened = false;
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
  if (size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = s->just_opened ? 1 : 0;
  }
  return size;
}

void tree_sitter_sky_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = (Scanner *)payload;
  s->stack_len = 1;
  s->indent_stack[0] = 0;
  s->runback_len = 0;
  s->just_opened = false;
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
  if (pos < length) {
    s->just_opened = buffer[pos++] != 0;
  }
}

static int current_indent(Scanner *s) {
  return s->stack_len > 0 ? s->indent_stack[s->stack_len - 1] : 0;
}

// Only commas and pipes are true continuation characters.
// Closing delimiters ) ] } go through normal indent/dedent logic.
static bool is_continuation_char(int32_t ch) {
  return ch == ',' || ch == '|';
}

// Advance past whitespace/comments to find the column of the next content line.
static int scan_to_content(TSLexer *lexer, bool *found_newline) {
  int col = 0;
  *found_newline = false;
  while (!lexer->eof(lexer)) {
    int32_t ch = lexer->lookahead;
    if (ch == '\n' || ch == '\r') {
      *found_newline = true;
      col = 0;
      lexer->advance(lexer, true);
      if (ch == '\r' && !lexer->eof(lexer) && lexer->lookahead == '\n')
        lexer->advance(lexer, true);
      continue;
    }
    if (ch == ' ') { if (*found_newline) col++; lexer->advance(lexer, true); continue; }
    if (ch == '\t') { if (*found_newline) col += 4; lexer->advance(lexer, true); continue; }
    // Skip comment-only lines
    if (*found_newline && ch == '-') {
      lexer->advance(lexer, true);
      if (!lexer->eof(lexer) && lexer->lookahead == '-') {
        while (!lexer->eof(lexer) && lexer->lookahead != '\n')
          lexer->advance(lexer, true);
        continue;
      }
      break;
    }
    break;
  }
  return col;
}

static bool is_keyword_boundary(int32_t ch) {
  if (ch >= 'a' && ch <= 'z') return false;
  if (ch >= 'A' && ch <= 'Z') return false;
  if (ch >= '0' && ch <= '9') return false;
  if (ch == '_') return false;
  return true;
}

// Peek for "in" keyword without consuming
static bool lookahead_is_in(TSLexer *lexer) {
  if (lexer->eof(lexer) || lexer->lookahead != 'i') return false;
  lexer->mark_end(lexer);
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 'n') return false;
  lexer->advance(lexer, false);
  return lexer->eof(lexer) || is_keyword_boundary(lexer->lookahead);
}

// Peek for "else" keyword without consuming
static bool lookahead_is_else(TSLexer *lexer) {
  if (lexer->eof(lexer) || lexer->lookahead != 'e') return false;
  lexer->mark_end(lexer);
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 'l') return false;
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 's') return false;
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 'e') return false;
  lexer->advance(lexer, false);
  return lexer->eof(lexer) || is_keyword_boundary(lexer->lookahead);
}

// Peek for "then" keyword without consuming
static bool lookahead_is_then(TSLexer *lexer) {
  if (lexer->eof(lexer) || lexer->lookahead != 't') return false;
  lexer->mark_end(lexer);
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 'h') return false;
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 'e') return false;
  lexer->advance(lexer, false);
  if (lexer->eof(lexer) || lexer->lookahead != 'n') return false;
  lexer->advance(lexer, false);
  return lexer->eof(lexer) || is_keyword_boundary(lexer->lookahead);
}

// Skip spaces on the same line (used before keyword checks)
static void skip_same_line_spaces(TSLexer *lexer) {
  while (!lexer->eof(lexer) && lexer->lookahead == ' ')
    lexer->advance(lexer, true);
}

bool tree_sitter_sky_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *s = (Scanner *)payload;

  // ── 1. Drain runback buffer ──────────────────────────
  if (s->runback_len > 0) {
    uint8_t token_type = s->runback[0];
    for (int i = 1; i < s->runback_len; i++)
      s->runback[i - 1] = s->runback[i];
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
  if (valid_symbols[VIRTUAL_OPEN_SECTION]) {
    lexer->mark_end(lexer);
    if (lexer->eof(lexer)) return false;

    bool found_newline = false;
    int col = scan_to_content(lexer, &found_newline);
    if (lexer->eof(lexer)) return false;

    if (!found_newline) {
      col = lexer->get_column(lexer);
      if (col <= current_indent(s))
        col = current_indent(s) + 1;
    }

    if (s->stack_len < MAX_INDENT_STACK)
      s->indent_stack[s->stack_len++] = col;

    s->just_opened = true;
    lexer->result_symbol = VIRTUAL_OPEN_SECTION;
    return true;
  }

  // ── 3. END_DECL / END_SECTION ────────────────────────
  if (!valid_symbols[VIRTUAL_END_DECL] && !valid_symbols[VIRTUAL_END_SECTION])
    return false;

  lexer->mark_end(lexer);

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
      // Same-line: skip spaces then check for closing tokens
      if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
        skip_same_line_spaces(lexer);
        if (lexer->eof(lexer)) {
          s->stack_len--;
          lexer->result_symbol = VIRTUAL_END_SECTION;
          return true;
        }
        ch = lexer->lookahead;
        if (ch == ')' || ch == ']' || ch == '}') {
          s->stack_len--;
          lexer->result_symbol = VIRTUAL_END_SECTION;
          return true;
        }
        if (lookahead_is_in(lexer)) {
          s->stack_len--;
          lexer->result_symbol = VIRTUAL_END_SECTION;
          return true;
        }
        if (lookahead_is_else(lexer)) {
          s->stack_len--;
          lexer->result_symbol = VIRTUAL_END_SECTION;
          return true;
        }
      }
      return false;
    }

    // Skip blank lines
    if (ch == '\n' || ch == '\r') continue;

    // Skip comment-only lines
    if (ch == '-') {
      lexer->advance(lexer, true);
      if (!lexer->eof(lexer) && lexer->lookahead == '-') {
        while (!lexer->eof(lexer) && lexer->lookahead != '\n')
          lexer->advance(lexer, true);
        continue;
      }
      break;
    }

    break;
  }

  int cur = current_indent(s);

  // ── Continuation lines ───────────────────────────────
  if (saw_newline && !lexer->eof(lexer)) {
    int32_t ch = lexer->lookahead;
    if (is_continuation_char(ch))
      return false;

    // "in" on new line: close let section
    if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1 && lookahead_is_in(lexer)) {
      s->stack_len--;
      cur = current_indent(s);
      while (s->stack_len > 1 && indent < cur) {
        if (s->runback_len < MAX_RUNBACK) s->runback[s->runback_len++] = 1;
        s->stack_len--;
        cur = current_indent(s);
      }
      lexer->result_symbol = VIRTUAL_END_SECTION;
      return true;
    }

    // "else" on new line: close exactly one section (then-branch)
    if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1 && lookahead_is_else(lexer)) {
      s->stack_len--;
      lexer->result_symbol = VIRTUAL_END_SECTION;
      return true;
    }

    // Note: "then" keyword is NOT handled here because the if-condition
    // uses a virtual section that closes via normal indent tracking.
  }

  s->just_opened = false; // Clear after first non-open scan

  // ── Dedent ───────────────────────────────────────────
  if (indent < cur && valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
    s->stack_len--;
    cur = current_indent(s);
    while (s->stack_len > 1 && indent < cur) {
      if (s->runback_len < MAX_RUNBACK) s->runback[s->runback_len++] = 1;
      s->stack_len--;
      cur = current_indent(s);
    }
    if (indent == cur && s->runback_len < MAX_RUNBACK)
      s->runback[s->runback_len++] = 0;
    lexer->result_symbol = VIRTUAL_END_SECTION;
    return true;
  }

  // ── Same level ───────────────────────────────────────
  if (indent == cur && valid_symbols[VIRTUAL_END_DECL]) {
    // After OPEN_SECTION, the first content at the same level is NOT a separator
    if (s->just_opened) {
      s->just_opened = false;
      return false;
    }
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

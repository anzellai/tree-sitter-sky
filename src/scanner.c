// External scanner for Sky tree-sitter grammar.
//
// Sky uses indentation-sensitive layout (like Elm, Haskell, Python).
// This scanner tracks indentation levels and emits virtual tokens:
//
//   _virtual_end_decl:    emitted when indentation returns to or below
//                         the level of the enclosing context
//   _virtual_open_section: emitted when indentation increases, opening
//                         a new indented block

#include "tree_sitter/parser.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum TokenType {
  VIRTUAL_END_DECL,
  VIRTUAL_OPEN_SECTION,
  BLOCK_COMMENT_CONTENT,
};

#define MAX_INDENT_STACK 64

typedef struct {
  int indent_stack[MAX_INDENT_STACK];
  int stack_len;
} Scanner;

void *tree_sitter_sky_external_scanner_create(void) {
  Scanner *s = calloc(1, sizeof(Scanner));
  s->indent_stack[0] = 0;
  s->stack_len = 1;
  return s;
}

void tree_sitter_sky_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_sky_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = (Scanner *)payload;
  unsigned size = 0;

  int len = s->stack_len;
  if (len > MAX_INDENT_STACK) len = MAX_INDENT_STACK;

  buffer[size++] = (char)len;
  for (int i = 0; i < len && size + 1 < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
    int val = s->indent_stack[i];
    buffer[size++] = (char)(val & 0xFF);
    buffer[size++] = (char)((val >> 8) & 0xFF);
  }
  return size;
}

void tree_sitter_sky_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = (Scanner *)payload;
  s->stack_len = 1;
  s->indent_stack[0] = 0;

  if (length == 0) return;

  unsigned pos = 0;
  int len = (unsigned char)buffer[pos++];
  if (len > MAX_INDENT_STACK) len = MAX_INDENT_STACK;

  s->stack_len = 0;
  for (int i = 0; i < len && pos + 1 < length; i++) {
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
}

static int current_indent(Scanner *s) {
  return s->stack_len > 0 ? s->indent_stack[s->stack_len - 1] : 0;
}

bool tree_sitter_sky_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *s = (Scanner *)payload;

  if (!valid_symbols[VIRTUAL_END_DECL] && !valid_symbols[VIRTUAL_OPEN_SECTION]) {
    return false;
  }

  lexer->mark_end(lexer);

  // At EOF, emit end-decl to close any open declarations
  if (lexer->eof(lexer)) {
    if (valid_symbols[VIRTUAL_END_DECL]) {
      lexer->result_symbol = VIRTUAL_END_DECL;
      return true;
    }
    return false;
  }

  // Scan forward through whitespace to find the next line's indentation
  bool saw_newline = false;
  int indent = 0;

  // Check current position — if we're at the start of a line already
  // from the lexer's perspective, we might need to emit tokens
  while (true) {
    if (lexer->eof(lexer)) {
      if (valid_symbols[VIRTUAL_END_DECL]) {
        lexer->result_symbol = VIRTUAL_END_DECL;
        return true;
      }
      return false;
    }

    if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
      saw_newline = true;
      indent = 0;
      lexer->advance(lexer, true);
      continue;
    }

    if (saw_newline && (lexer->lookahead == ' ' || lexer->lookahead == '\t')) {
      if (lexer->lookahead == ' ') indent++;
      else indent += 4;
      lexer->advance(lexer, true);
      continue;
    }

    if (!saw_newline) {
      return false;
    }

    // We found a non-whitespace character after a newline
    break;
  }

  // Skip blank lines (next char is another newline)
  if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
    return false;
  }

  // Don't emit layout tokens for lines starting with -- (comments)
  // The comment is handled by extras, not the layout system
  // (We let the regular parser handle comments)

  int cur = current_indent(s);

  // Same level or less: end the current declaration/block
  if (indent <= cur && valid_symbols[VIRTUAL_END_DECL]) {
    // Pop indent levels that are greater than the new indent
    while (s->stack_len > 1 && s->indent_stack[s->stack_len - 1] > indent) {
      s->stack_len--;
    }
    lexer->result_symbol = VIRTUAL_END_DECL;
    return true;
  }

  // Greater indent: open a new section
  if (indent > cur && valid_symbols[VIRTUAL_OPEN_SECTION]) {
    if (s->stack_len < MAX_INDENT_STACK) {
      s->indent_stack[s->stack_len++] = indent;
    }
    lexer->result_symbol = VIRTUAL_OPEN_SECTION;
    return true;
  }

  return false;
}

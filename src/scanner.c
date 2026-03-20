// External scanner for Sky tree-sitter grammar.
//
// Sky uses indentation-sensitive layout (like Elm, Haskell).
// This scanner tracks indentation levels and emits three virtual tokens:
//
//   _virtual_open_section  (INDENT):  pushed when a new indented block begins
//   _virtual_end_decl      (NEWLINE): emitted between siblings at the same indent level
//   _virtual_end_section   (DEDENT):  emitted when indentation drops below current level
//
// The approach follows elm-tree-sitter: open/close/separator as virtual {/}/;

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
  // Runback buffer for pending tokens when multiple sections need closing
  // 0 = VIRTUAL_END_DECL, 1 = VIRTUAL_END_SECTION
  uint8_t runback[MAX_RUNBACK];
  int runback_len;
  int indent_length;  // column of first non-whitespace on current line
  bool newline_pending; // true when we've seen a newline and need to process it
} Scanner;

void *tree_sitter_sky_external_scanner_create(void) {
  Scanner *s = calloc(1, sizeof(Scanner));
  s->indent_stack[0] = 0;
  s->stack_len = 1;
  s->runback_len = 0;
  s->indent_length = 0;
  s->newline_pending = false;
  return s;
}

void tree_sitter_sky_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_sky_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = (Scanner *)payload;
  unsigned size = 0;

  // Stack length
  buffer[size++] = (char)s->stack_len;
  // Stack entries
  for (int i = 0; i < s->stack_len && size + 1 < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
    int val = s->indent_stack[i];
    buffer[size++] = (char)(val & 0xFF);
    buffer[size++] = (char)((val >> 8) & 0xFF);
  }
  // Runback length
  if (size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = (char)s->runback_len;
  }
  // Runback entries
  for (int i = 0; i < s->runback_len && size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
    buffer[size++] = s->runback[i];
  }
  // indent_length
  if (size + 1 < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = (char)(s->indent_length & 0xFF);
    buffer[size++] = (char)((s->indent_length >> 8) & 0xFF);
  }
  // newline_pending
  if (size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    buffer[size++] = s->newline_pending ? 1 : 0;
  }
  return size;
}

void tree_sitter_sky_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = (Scanner *)payload;
  s->stack_len = 1;
  s->indent_stack[0] = 0;
  s->runback_len = 0;
  s->indent_length = 0;
  s->newline_pending = false;

  if (length == 0) return;

  unsigned pos = 0;

  // Stack length
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

  // Runback length
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

  // indent_length
  if (pos + 1 < length) {
    s->indent_length = (unsigned char)buffer[pos] | ((unsigned char)buffer[pos + 1] << 8);
    pos += 2;
  }

  // newline_pending
  if (pos < length) {
    s->newline_pending = buffer[pos++] != 0;
  }
}

static int current_indent(Scanner *s) {
  return s->stack_len > 0 ? s->indent_stack[s->stack_len - 1] : 0;
}

bool tree_sitter_sky_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *s = (Scanner *)payload;

  // Drain runback buffer first
  if (s->runback_len > 0) {
    uint8_t token_type = s->runback[0];
    // Shift buffer left
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
    // If the buffered token isn't valid, clear remaining runback and fall through
    s->runback_len = 0;
  }

  // Handle VIRTUAL_OPEN_SECTION: immediately push current column
  // This fires right after "let", "of", etc. without needing a newline
  if (valid_symbols[VIRTUAL_OPEN_SECTION]) {
    // Skip whitespace to find the column of the next significant token
    lexer->mark_end(lexer);

    // If we're at EOF, don't open a section
    if (lexer->eof(lexer)) {
      return false;
    }

    // Scan past whitespace (including newlines) to find the next token's column
    int col = 0;
    bool found_newline = false;
    while (!lexer->eof(lexer)) {
      if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
        found_newline = true;
        col = 0;
        lexer->advance(lexer, true);
        continue;
      }
      if (lexer->lookahead == ' ') {
        col++;
        lexer->advance(lexer, true);
        continue;
      }
      if (lexer->lookahead == '\t') {
        col += 4;
        lexer->advance(lexer, true);
        continue;
      }
      // Found non-whitespace
      break;
    }

    if (lexer->eof(lexer)) {
      return false;
    }

    // Skip line comments to find the actual content
    while (lexer->lookahead == '-') {
      lexer->advance(lexer, true);
      if (lexer->lookahead == '-') {
        // Line comment - skip to end of line
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
          lexer->advance(lexer, true);
        }
        if (!lexer->eof(lexer)) {
          lexer->advance(lexer, true); // skip \n
          col = 0;
          found_newline = true;
          // Skip whitespace on next line
          while (!lexer->eof(lexer) && (lexer->lookahead == ' ' || lexer->lookahead == '\t')) {
            if (lexer->lookahead == ' ') col++;
            else col += 4;
            lexer->advance(lexer, true);
          }
          continue;
        }
        return false;
      }
      // Not a comment, just a minus sign - use current col
      break;
    }

    if (!found_newline) {
      // Same-line content: the column is wherever we are.
      // We don't have reliable column info for same-line, so push cur+1 as a
      // sentinel that anything on the next line at the same or lesser indent
      // will close this section.
      int sentinel = current_indent(s) + 1;
      if (s->stack_len < MAX_INDENT_STACK) {
        s->indent_stack[s->stack_len++] = sentinel;
      }
    } else {
      // Push the column of the first token on the new line
      if (s->stack_len < MAX_INDENT_STACK) {
        s->indent_stack[s->stack_len++] = col;
      }
    }

    lexer->result_symbol = VIRTUAL_OPEN_SECTION;
    return true;
  }

  // For END_DECL and END_SECTION, we need to see a newline first
  if (!valid_symbols[VIRTUAL_END_DECL] && !valid_symbols[VIRTUAL_END_SECTION]) {
    return false;
  }

  lexer->mark_end(lexer);

  // At EOF, emit closing tokens
  if (lexer->eof(lexer)) {
    if (valid_symbols[VIRTUAL_END_SECTION]) {
      if (s->stack_len > 1) {
        s->stack_len--;
      }
      lexer->result_symbol = VIRTUAL_END_SECTION;
      return true;
    }
    if (valid_symbols[VIRTUAL_END_DECL]) {
      lexer->result_symbol = VIRTUAL_END_DECL;
      return true;
    }
    return false;
  }

  // Scan forward through whitespace to find the next line's indentation
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
      // Check for closing delimiters on the same line
      // that should trigger end_section (like Elm's approach)
      if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
        if (lexer->lookahead == ')' || lexer->lookahead == ']' || lexer->lookahead == '}') {
          s->stack_len--;
          lexer->result_symbol = VIRTUAL_END_SECTION;
          return true;
        }
      }
      // Check for 'in' keyword on same line (closes let section)
      if (valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
        if (lexer->lookahead == 'i') {
          lexer->advance(lexer, false);
          if (lexer->lookahead == 'n') {
            lexer->advance(lexer, false);
            // Check it's followed by whitespace or EOF (not part of a longer identifier)
            if (lexer->eof(lexer) || lexer->lookahead == ' ' || lexer->lookahead == '\n' ||
                lexer->lookahead == '\r' || lexer->lookahead == '\t') {
              // This is the 'in' keyword - close the section
              lexer->mark_end(lexer); // don't consume 'in'
              s->stack_len--;
              lexer->result_symbol = VIRTUAL_END_SECTION;
              return true;
            }
          }
        }
      }
      return false;
    }

    // Skip blank lines
    if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
      continue;
    }

    // Found non-whitespace after newline
    break;
  }

  // Skip comment-only lines
  if (lexer->lookahead == '-') {
    // Peek ahead to check for line comment
    // If it's a comment line, treat it as if we haven't found content yet
    // (comments don't affect layout)
  }

  int cur = current_indent(s);

  // Dedent: indent < current level -> close sections
  if (indent < cur && valid_symbols[VIRTUAL_END_SECTION] && s->stack_len > 1) {
    // Pop one level, queue additional pops in runback
    s->stack_len--;
    cur = current_indent(s);

    // Queue additional END_SECTION tokens for remaining levels above indent
    while (s->stack_len > 1 && indent < cur) {
      if (s->runback_len < MAX_RUNBACK) {
        s->runback[s->runback_len++] = 1; // VIRTUAL_END_SECTION
      }
      s->stack_len--;
      cur = current_indent(s);
    }

    // If after all pops we're at the same level, also queue a NEWLINE
    if (indent == cur && s->runback_len < MAX_RUNBACK) {
      s->runback[s->runback_len++] = 0; // VIRTUAL_END_DECL
    }

    lexer->result_symbol = VIRTUAL_END_SECTION;
    return true;
  }

  // Same level: emit separator
  if (indent == cur && valid_symbols[VIRTUAL_END_DECL]) {
    lexer->result_symbol = VIRTUAL_END_DECL;
    return true;
  }

  // Dedent but END_SECTION not valid, try END_DECL
  if (indent <= cur && valid_symbols[VIRTUAL_END_DECL]) {
    lexer->result_symbol = VIRTUAL_END_DECL;
    return true;
  }

  return false;
}

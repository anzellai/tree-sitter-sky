/// <reference types="tree-sitter-cli/dsl" />
// Tree-sitter grammar for the Sky programming language
// Sky is an Elm-inspired language that compiles to Go.

const PREC = {
  PIPE: 1,
  OR: 2,
  AND: 3,
  COMPARE: 4,
  CONCAT: 5,
  ADD: 6,
  MUL: 7,
  COMPOSE: 9,
  CONS: 5,
  UNARY: 10,
  CALL: 11,
  FIELD: 12,
  QUALIFY: 13,
};

module.exports = grammar({
  name: "sky",

  extras: ($) => [/\s/, $.line_comment, $.block_comment],

  word: ($) => $.lower_identifier,

  externals: ($) => [
    $._virtual_end_decl,
    $._virtual_open_section,
    $._virtual_end_section,
    $.block_comment_content,
  ],

  conflicts: ($) => [
    [$.function_declaration, $.type_annotation_declaration],
    [$.record_expression, $.record_update_expression],
    [$._type_non_func, $.type_application],
  ],

  rules: {
    // ── Top-level ──────────────────────────────────────
    source_file: ($) =>
      seq(
        optional(seq($.module_declaration, $._virtual_end_decl)),
        repeat(seq(
          choice(
            $.import_declaration,
            $.foreign_import_declaration,
            $.type_declaration,
            $.type_alias_declaration,
            $.type_annotation_declaration,
            $.function_declaration,
            $.port_declaration,
          ),
          $._virtual_end_decl,
        )),
      ),

    // ── Module declaration ─────────────────────────────
    module_declaration: ($) =>
      seq("module", $.module_name, optional($.exposing_clause)),

    module_name: ($) => sep1($.upper_identifier, "."),

    exposing_clause: ($) =>
      seq(
        "exposing",
        "(",
        choice($.double_dot, commaSep1($._exposed_item)),
        ")",
      ),

    double_dot: (_$) => "..",

    _exposed_item: ($) =>
      choice($.exposed_value, $.exposed_type),

    exposed_value: ($) => $.lower_identifier,

    exposed_type: ($) =>
      seq($.upper_identifier, optional(seq("(", $.double_dot, ")"))),

    // ── Imports ────────────────────────────────────────
    import_declaration: ($) =>
      seq(
        "import",
        $.module_name,
        optional($.import_alias),
        optional($.exposing_clause),
      ),

    import_alias: ($) => seq("as", choice($.upper_identifier, "_")),

    foreign_import_declaration: ($) =>
      seq("foreign", "import", $.string, $.exposing_clause),

    // ── Port ───────────────────────────────────────────
    port_declaration: ($) =>
      seq("port", $.lower_identifier, ":", $._type_expression),

    // ── Type annotation ────────────────────────────────
    type_annotation_declaration: ($) =>
      seq(
        field("name", choice($.lower_identifier, $.upper_identifier)),
        ":",
        field("type", $._type_expression),
      ),

    // ── Function ───────────────────────────────────────
    function_declaration: ($) =>
      seq(
        field("name", $.lower_identifier),
        repeat($._simple_pattern),
        "=",
        field("body", $._expression),
      ),

    // ── Type declarations ──────────────────────────────
    type_declaration: ($) =>
      seq(
        "type",
        field("name", $.upper_identifier),
        repeat($.type_variable),
        "=",
        sep1($.type_variant, "|"),
      ),

    type_variant: ($) =>
      prec.left(seq($.upper_identifier, repeat($._single_type_expression))),

    type_alias_declaration: ($) =>
      seq(
        "type",
        "alias",
        field("name", $.upper_identifier),
        repeat($.type_variable),
        "=",
        field("type", $._type_expression),
      ),

    // ── Type expressions ───────────────────────────────
    _type_expression: ($) =>
      choice(
        $.function_type,
        $._type_non_func,
      ),

    _type_non_func: ($) =>
      choice(
        $.type_application,
        $._type_atom,
      ),

    function_type: ($) =>
      prec.right(seq($._type_non_func, "->", $._type_expression)),

    type_application: ($) =>
      prec.left(seq($._type_atom, repeat1($._type_atom))),

    _single_type_expression: ($) =>
      choice(
        alias($.type_ref_without_args, $.type_reference),
        $.type_variable,
        $.unit_type,
        $.tuple_type,
        $.record_type,
        $.parenthesized_type,
      ),

    type_ref_without_args: ($) =>
      prec.left(PREC.QUALIFY, sep1($.upper_identifier, ".")),

    _type_atom: ($) =>
      choice(
        $.type_reference,
        $.type_variable,
        $.unit_type,
        $.tuple_type,
        $.record_type,
        $.parenthesized_type,
      ),

    type_reference: ($) =>
      prec.left(PREC.QUALIFY, sep1($.upper_identifier, ".")),

    type_variable: ($) => $.lower_identifier,

    unit_type: (_$) => seq("(", ")"),

    tuple_type: ($) =>
      seq("(", $._type_expression, ",", commaSep1($._type_expression), ")"),

    record_type: ($) =>
      seq("{", commaSep1($.record_type_field), "}"),

    record_type_field: ($) =>
      seq(field("name", $.lower_identifier), ":", field("type", $._type_expression)),

    parenthesized_type: ($) => seq("(", $._type_expression, ")"),

    // ── Expressions ────────────────────────────────────
    _expression: ($) =>
      choice(
        $.binary_expression,
        $.negate_expression,
        $._call_or_atom,
      ),

    negate_expression: ($) =>
      prec(PREC.UNARY, seq("-", $._call_or_atom)),

    binary_expression: ($) =>
      choice(
        prec.left(PREC.OR, seq($._expression, "||", $._expression)),
        prec.left(PREC.AND, seq($._expression, "&&", $._expression)),
        prec.left(PREC.COMPARE, seq($._expression, choice("==", "!=", "/=", "<", "<=", ">", ">="), $._expression)),
        prec.right(PREC.CONCAT, seq($._expression, "++", $._expression)),
        prec.left(PREC.ADD, seq($._expression, choice("+", "-"), $._expression)),
        prec.left(PREC.MUL, seq($._expression, choice("*", "/", "//", "%"), $._expression)),
        prec.right(PREC.COMPOSE, seq($._expression, choice(">>", "<<"), $._expression)),
        prec.right(PREC.CONS, seq($._expression, "::", $._expression)),
        prec.left(PREC.PIPE, seq($._expression, "|>", $._expression)),
        prec.right(PREC.PIPE, seq($._expression, "<|", $._expression)),
      ),

    _call_or_atom: ($) =>
      choice(
        $.call_expression,
        $._atom,
      ),

    call_expression: ($) =>
      prec.left(PREC.CALL, seq($._atom, repeat1($._atom))),

    _atom: ($) =>
      choice(
        $.variable_expression,
        $.constructor_expression,
        $.integer,
        $.float,
        $.string,
        $.char,
        $.lambda_expression,
        $.let_expression,
        $.if_expression,
        $.case_expression,
        $.record_expression,
        $.record_update_expression,
        $.tuple_expression,
        $.list_expression,
        $.unit_expression,
        $.parenthesized_expression,
        $.field_access_expression,
        $.field_accessor,
      ),

    variable_expression: ($) => $.lower_identifier,

    constructor_expression: ($) =>
      prec.left(PREC.QUALIFY, sep1($.upper_identifier, ".")),

    field_access_expression: ($) =>
      prec.left(PREC.FIELD, seq($._atom, ".", $.lower_identifier)),

    field_accessor: ($) => seq(".", $.lower_identifier),

    parenthesized_expression: ($) =>
      seq("(", $._expression, ")"),

    unit_expression: (_$) => seq("(", ")"),

    tuple_expression: ($) =>
      seq("(", $._expression, ",", commaSep1($._expression), ")"),

    list_expression: ($) =>
      seq("[", optional(commaSep1($._expression)), "]"),

    record_expression: ($) =>
      seq("{", commaSep1($.record_field), "}"),

    record_field: ($) =>
      seq(field("name", $.lower_identifier), "=", field("value", $._expression)),

    record_update_expression: ($) =>
      seq("{", $.lower_identifier, "|", commaSep1($.record_field), "}"),

    lambda_expression: ($) =>
      seq("\\", repeat1($._simple_pattern), "->", $._expression),

    // let OPEN binding (NEWLINE binding)* CLOSE in expr
    let_expression: ($) =>
      seq(
        "let",
        $._virtual_open_section,
        $.let_binding,
        repeat(seq($._virtual_end_decl, $.let_binding)),
        $._virtual_end_section,
        "in",
        $._expression,
      ),

    let_binding: ($) =>
      seq(
        $._simple_pattern,
        optional(seq(":", $._type_expression)),
        "=",
        $._expression,
      ),

    if_expression: ($) =>
      seq("if", $._expression, "then", $._expression, "else", $._expression),

    // case expr of OPEN branch (NEWLINE branch)* CLOSE
    case_expression: ($) =>
      seq(
        "case", $._expression, "of",
        $._virtual_open_section,
        $.case_branch,
        repeat(seq($._virtual_end_decl, $.case_branch)),
        $._virtual_end_section,
      ),

    case_branch: ($) =>
      seq($._pattern, "->", $._expression),

    // ── Patterns ───────────────────────────────────────
    _pattern: ($) =>
      choice(
        $.constructor_pattern,
        $.cons_pattern,
        $.as_pattern,
        $._simple_pattern,
      ),

    _simple_pattern: ($) =>
      choice(
        $.variable_pattern,
        $.wildcard_pattern,
        $.literal_pattern,
        $.tuple_pattern,
        $.list_pattern,
        $.unit_pattern,
        $.record_pattern,
        $.parenthesized_pattern,
      ),

    constructor_pattern: ($) =>
      prec.left(seq(
        sep1($.upper_identifier, "."),
        repeat($._simple_pattern),
      )),

    variable_pattern: ($) => $.lower_identifier,

    wildcard_pattern: (_$) => "_",

    literal_pattern: ($) =>
      choice($.integer, $.float, $.string, $.char),

    tuple_pattern: ($) =>
      seq("(", $._pattern, ",", commaSep1($._pattern), ")"),

    list_pattern: ($) =>
      seq("[", optional(commaSep1($._pattern)), "]"),

    cons_pattern: ($) =>
      prec.right(seq($._pattern, "::", $._pattern)),

    as_pattern: ($) =>
      prec.left(seq($._simple_pattern, "as", $.lower_identifier)),

    unit_pattern: (_$) => seq("(", ")"),

    parenthesized_pattern: ($) => seq("(", $._pattern, ")"),

    record_pattern: ($) =>
      seq("{", commaSep1($.lower_identifier), "}"),

    // ── Terminals ──────────────────────────────────────
    lower_identifier: (_$) => /[a-z_][a-zA-Z0-9_]*/,

    upper_identifier: (_$) => /[A-Z][a-zA-Z0-9_]*/,

    integer: (_$) =>
      token(choice(
        /0[xX][0-9a-fA-F]+/,
        /0[oO][0-7]+/,
        /0[bB][01]+/,
        /[0-9]+/,
      )),

    float: (_$) => token(/[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?/),

    string: ($) =>
      token(seq(
        '"',
        repeat(choice(/[^"\\]+/, /\\[nrt\\"'0]/, /\\u\{[0-9a-fA-F]+\}/)),
        '"',
      )),

    char: ($) =>
      seq("'", choice(/[^'\\]/, $.escape_sequence), "'"),

    escape_sequence: (_$) =>
      token.immediate(choice(
        /\\[nrt\\"'0]/,
        /\\u\{[0-9a-fA-F]+\}/,
      )),

    // ── Comments ───────────────────────────────────────
    line_comment: (_$) => token(prec(-1, seq("--", /.*/))),

    block_comment: ($) =>
      seq(
        "{-",
        repeat(choice(
          $.block_comment,
          /[^{}-]+/,
          /[{}]/,
          "-",
        )),
        "-}",
      ),
  },
});

function sep1(rule, separator) {
  return seq(rule, repeat(seq(separator, rule)));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}

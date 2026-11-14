#pragma once

#include <cstdint>
#include <string>

enum class token_type : std::uint8_t {
    // keywords
    INT,
    BOOL,
    CHAR,
    BYTE,
    FLOAT,
    IF,
    ELIF,
    ELSE,
    WHILE,
    FOR,
    RETURN,
    STEP,
    REJECT,
    INV,
    UPDATE,
    LET,
    IMUT,
    STATE,
    DEFINE,
    FN,
    STRUCT,
    IMPL,
    THIS,
    NEW,

    // identifier
    IDENTIFIER,

    // operators
    PLUS,             // '+'
    MINUS,            // '-'
    MULTIPLY,         // '*'
    DIVIDE,           // '/'
    DIVIDE_FLOOR,     // '\'
    EXPONENT,         // '~'
    XOR,              // '^'
    BIT_AND,          // '&'
    BIT_OR,           // '|'
    LOGICAL_AND,      // '&&'
    LOGICAL_OR,       // '||'
    NOT,              // '!'
    ADDRESS,          // '@'
    DEREFERENCE,      // '<-'
    POINTER,          // '->'
    ACCESS,           // '.'
    EQUAL,            // '=='
    ASSIGN,           // '='
    NOT_EQUAL,        // '!='
    LESSER,           // '<'
    GREATER,          // '>'
    LESS_OR_EQUAL,    // '<='
    GREATER_OR_EQUAL, // '>='
    LEFT_SHIFT,       // '<<'
    RIGHT_SHIFT,      // '>>'

    // literals
    INT_LITERAL,
    FLOAT_LITERAL,
    CHAR_LITERAL,
    BOOL_LITERAL,
    BASE_2_LITERAL,
    BASE_16_LITERAL,
    STRING_LITERAL,

    // punctuators
    CURLY_OPEN,        // '{'
    CURLY_CLOSE,       // '}'
    PARENTHESES_OPEN,  // '('
    PARENTHESES_CLOSE, // ')'
    SQUARE_OPEN,       // '['
    SQUARE_CLOSE,      // ']'
    SEMI_COLON,        // ';'
    COLON,             // ':'
    COMMA,             // ','

    WHITESPACE, // Helps with parsing - allows me to have an accepting state in the graph for
                // whitespace

    COMMENT // '#'
};

struct token {
    token_type type;
    std::string lexeme;
    // Zero-based position of the lexeme's first char.
    // TODO: the tokeniser counts in std::size_t and narrows to int when it builds a token.
    // Storing std::size_t would remove that cast, but it ripples into the parser (its
    // source_position and the int source_span in src/ast/source.h) and into every test that
    // builds or compares tokens with int positions.
    int line;
    int column;
};

%skeleton "lalr1.cc" // -*- C++ -*-
%require "3.5.1"    // That's what's present on ubuntu 20.04.

%defines  // %header starts from 3.8.1

%define api.namespace {dfly::search}

%define api.token.raw
%define api.token.constructor
%define api.value.type variant
%define api.parser.class {Parser}
%define parse.assert

// Added to header file before parser declaration.
%code requires {
  #include "core/search/ast_expr.h"

  namespace dfly {
  namespace search {
    class QueryDriver;
  }
  }
}

// Added to cc file
%code {
#include "core/search/query_driver.h"

#define yylex driver->scanner()->Lex

using namespace std;
}

%parse-param { QueryDriver *driver  }

%locations

%define parse.trace
%define parse.error verbose  // detailed
%define parse.lac full
%define api.token.prefix {TOK_}

%token
  LPAREN   "("
  RPAREN   ")"
  STAR     "*"
  ARROW    "=>"
  COLON    ":"
  LBRACKET "["
  RBRACKET "]"
  OR_OP    "|"
;

%token AND_OP

// Needed 0 at the end to satisfy bison 3.5.1
%token YYEOF 0
%token <std::string> TERM "term" PARAM "param" FIELD "field"

%precedence TERM
%left OR_OP AND_OP
%right NOT_OP
%precedence LPAREN RPAREN

%token <int64_t> INT64 "int64"
%nterm <AstExpr> final_query filter search_expr field_cond field_cond_expr

%printer { yyo << $$; } <*>;

%%

final_query:
  filter { driver->Set($1); }

filter:
  search_expr { $$ = $1; }

search_expr:
 LPAREN search_expr RPAREN              { $$ = $2; }
 | search_expr search_expr %prec AND_OP { $$ = MakeExpr<AstLogicalNode>($1, $2, AstLogicalNode::kAnd); }
 | search_expr OR_OP search_expr        { $$ = MakeExpr<AstLogicalNode>($1, $3, AstLogicalNode::kOr); }
 | NOT_OP search_expr                   { $$ = MakeExpr<AstNegateNode>($2); }
 | TERM                                 { $$ = MakeExpr<AstTermNode>($1); }
 | FIELD COLON field_cond               { $$ = MakeExpr<AstFieldNode>($1, $3); }

field_cond:
  TERM                                  { $$ = MakeExpr<AstTermNode>($1); }
  | NOT_OP field_cond                   { $$ = MakeExpr<AstNegateNode>($2); }
  | LPAREN field_cond_expr RPAREN       { $$ = $2; }
  | LBRACKET INT64 INT64 RBRACKET       { $$ = MakeExpr<AstRangeNode>($2, $3); }

field_cond_expr:
  LPAREN field_cond_expr RPAREN                   { $$ = $2; }
  | field_cond_expr field_cond_expr %prec AND_OP  { $$ = MakeExpr<AstLogicalNode>($1, $2, AstLogicalNode::kAnd); }
  | field_cond_expr OR_OP field_cond_expr         { $$ = MakeExpr<AstLogicalNode>($1, $3, AstLogicalNode::kOr); }
  | NOT_OP field_cond_expr                        { $$ = MakeExpr<AstNegateNode>($2); };
  | TERM                                          { $$ = MakeExpr<AstTermNode>($1); }
%%

void
dfly::search::Parser::error(const location_type& l, const string& m)
{
  cerr << l << ": " << m << '\n';
}
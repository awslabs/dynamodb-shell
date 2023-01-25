/*
 * lexer.l
 *
 * The lexical analyzer for the ddbsh parser.
 *
 * Amrith. 2022-08-02
 */

%option noyywrap

%{

#pragma GCC diagnostic ignored "-Wdeprecated-register"

#define YY_DECL int yylex()

#include <stdio.h>
#include <stdlib.h>
#include "parser-defs.hpp"
#include "keywords.hpp"
#include "ddbsh.hpp"
#include "logging.hpp"
#include "input-source.hpp"
#include "allocation.hpp"

// readinput is a function pointer defined in input-source.[ch]pp

#define YY_INPUT(buffer, result, size) result = readinput(buffer, size);

void resetinput()
{
     yyrestart(yyin);
}

#include "parser.hpp"

int yycolumnno = 1;
static void count()
{
   int ix;
   for (ix = 0; yytext[ix] != 0; ix ++)
   {
       if (yytext[ix] == '\n' || yytext[ix] == '\r')
       {
           yylineno ++;
           yycolumnno = 1;
       }
       else
       {
           yycolumnno += 1;
       }
   }

   return;
}

%}

dquote                 [\"]
not_dquote             [^\"]

%%

   /* Handle comments in input. There are two kinds of single-line comments */

"--".* {
   /* single line comment starting with "--". Everything till
    * the end of the line is a comment */

   count();
}

"//".* {
   /* similar single line comment with a "//". multi-line comments are
    * not supported */
   count();
}

[\n\r] {
    count();
}

{dquote}[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z{dquote}  {
    count();
    yylval.strval = STRDUP(yytext);
    return T_TIMESTAMP;
}

{dquote}({not_dquote}|{dquote}{dquote})*{dquote} {
   /* double quoted string - to have a double-quote character in the
    * string, you must double it (SQL Style) not escale it (with a \). */
   count();
   yylval.strval = STRDUP(yytext);
   return T_DQUOTED_STRING;
}

-?[1-9][0-9]* |
0 {
    count();
    yylval.strval = STRDUP(yytext);
    return T_WHOLE_NUMBER;
}

-?[0-9]+"."[0-9]* |
-?"."[0-9]+ |
-?[0-9]+E[-+]?[0-9]+ |
-?[0-9]+"."[0-9]*E[-+]?[0-9]+ |
-?"."[0-9]+E[-+]?[0-9]+ {
    count();
    yylval.strval = STRDUP(yytext);
    return T_FLOATING_POINT_NUMBER;
}

[ \t] {
    count();
}

[a-zA-Z0-9_][a-zA-Z0-9_-]* {
    int token;
    count();
    if (lookup_keyword(yytext, &token) == 0)
        return token;
    else
    {
        yylval.strval = STRDUP(yytext);
        return T_UNQUOTED_STRING;
    }
}

"<" {
    count();
    yylval.strval = STRDUP(yytext);
    return T_LESS_THAN;
}

">" {
    count();
    yylval.strval = STRDUP(yytext);
    return T_GREATER_THAN;
}

"<=" {
    count();
    yylval.strval = STRDUP(yytext);
    return T_LESS_THAN_OR_EQUAL;
}

">=" {
    count();
    yylval.strval = STRDUP(yytext);
    return T_GREATER_THAN_OR_EQUAL;
}

"=" {
    count();
    yylval.strval = STRDUP(yytext);
    return T_EQUAL;
}

"!=" {
    count();
    yylval.strval = STRDUP(yytext);
    return T_NOT_EQUAL;
}

"<>" {
    count();
    yylval.strval = STRDUP(yytext);
    return T_NOT_EQUAL;
}

<<EOF>> {
    /* terminate with a \n, send back yytext[0] and set the quit flag */
    CDDBSh::getInstance()->quit();
    return (yytext[0]);
}

. {
    /* didn't find any other rule that matches this. return it as is. This
     * is not a bad thing as this is the expected behavior */
    count();
    logdebug("[%s, %d] lexer returning char %c\n", __FILENAME__, __LINE__, yytext[0]);
    return(yytext[0]);
}


%%

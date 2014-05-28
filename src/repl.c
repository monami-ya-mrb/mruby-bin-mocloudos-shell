/*
** Mocloudos shell
**
** Copyright (C) 2014 Monami-ya LLC, Japan.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/
/*
** mirb - Embeddable Interactive Ruby Shell
**
** Copyright (c) 2014 mruby developers
**
** This program takes code from the user in
** an interactive way and executes it
** immediately. It's a REPL...
*/

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "mruby.h"
#include "mruby/array.h"
#include "mruby/proc.h"
#include "mruby/compile.h"
#include "mruby/string.h"

static void
p(mrb_state *mrb, mrb_value obj, int prompt, int sessionfd)
{
  obj = mrb_funcall(mrb, obj, "inspect", 0);
  if (prompt) {
    if (!mrb->exc) {
      write(sessionfd, " => ", 4);
    }
    else {
      obj = mrb_funcall(mrb, mrb_obj_value(mrb->exc), "inspect", 0);
    }
  }
  write(sessionfd, RSTRING_PTR(obj), RSTRING_LEN(obj));
  write(sessionfd, "\n", 1);
}

/* Guess if the user might want to enter more
 * or if he wants an evaluation of his code now */
static mrb_bool
is_code_block_open(struct mrb_parser_state *parser)
{
  mrb_bool code_block_open = FALSE;

  /* check for heredoc */
  if (parser->parsing_heredoc != NULL) return TRUE;
  if (parser->heredoc_end_now) {
    parser->heredoc_end_now = FALSE;
    return FALSE;
  }

  /* check for unterminated string */
  if (parser->lex_strterm) return TRUE;

  /* check if parser error are available */
  if (0 < parser->nerr) {
    const char *unexpected_end = "syntax error, unexpected $end";
    const char *message = parser->error_buffer[0].message;

    /* a parser error occur, we have to check if */
    /* we need to read one more line or if there is */
    /* a different issue which we have to show to */
    /* the user */

    if (strncmp(message, unexpected_end, strlen(unexpected_end)) == 0) {
      code_block_open = TRUE;
    }
    else if (strcmp(message, "syntax error, unexpected keyword_end") == 0) {
      code_block_open = FALSE;
    }
    else if (strcmp(message, "syntax error, unexpected tREGEXP_BEG") == 0) {
      code_block_open = FALSE;
    }
    return code_block_open;
  }

  switch (parser->lstate) {

  /* all states which need more code */

  case EXPR_BEG:
    /* an expression was just started, */
    /* we can't end it like this */
    code_block_open = TRUE;
    break;
  case EXPR_DOT:
    /* a message dot was the last token, */
    /* there has to come more */
    code_block_open = TRUE;
    break;
  case EXPR_CLASS:
    /* a class keyword is not enough! */
    /* we need also a name of the class */
    code_block_open = TRUE;
    break;
  case EXPR_FNAME:
    /* a method name is necessary */
    code_block_open = TRUE;
    break;
  case EXPR_VALUE:
    /* if, elsif, etc. without condition */
    code_block_open = TRUE;
    break;

  /* now all the states which are closed */

  case EXPR_ARG:
    /* an argument is the last token */
    code_block_open = FALSE;
    break;

  /* all states which are unsure */

  case EXPR_CMDARG:
    break;
  case EXPR_END:
    /* an expression was ended */
    break;
  case EXPR_ENDARG:
    /* closing parenthese */
    break;
  case EXPR_ENDFN:
    /* definition end */
    break;
  case EXPR_MID:
    /* jump keyword like break, return, ... */
    break;
  case EXPR_MAX_STATE:
    /* don't know what to do with this token */
    break;
  default:
    /* this state is unexpected! */
    break;
  }

  return code_block_open;
}

#ifndef ENABLE_READLINE
/* Print the command line prompt of the REPL */
static void
print_cmdline(int code_block_open, int sessionfd)
{
  if (code_block_open) {
    write(sessionfd, "* ", 2);
  }
  else {
    write(sessionfd, "> ", 2);
  }
}
#endif

void mrb_codedump_all(mrb_state*, struct RProc*);

int
mirb_repl(mrb_state* mrb, int sessionfd, mrb_bool verbose)
{
  char ruby_code[1024] = { 0 };
  char last_code_line[1024] = { 0 };
#ifndef ENABLE_READLINE
  char last_char;
  int char_index;
#else
  char *home = NULL;
#endif
  mrbc_context *cxt;
  struct mrb_parser_state *parser;
  mrb_value result;
  mrb_bool code_block_open = FALSE;
  int ai;
  unsigned int stack_keep = 0;

  mrb_define_global_const(mrb, "ARGV", mrb_ary_new_capa(mrb, 0));

  cxt = mrbc_context_new(mrb);
  cxt->capture_errors = 1;
  cxt->lineno = 1;
  mrbc_filename(mrb, cxt, "(mirb)");
  if (verbose) cxt->dump_result = 1;

  ai = mrb_gc_arena_save(mrb);

  while (TRUE) {
#ifndef ENABLE_READLINE
    print_cmdline(code_block_open, sessionfd);
  line_start:
    char_index = 0;

    while (1) {
      int len;
      len = read(sessionfd, &last_char, 1);
      if (len == 0 || last_char == '\004') {
	last_char = EOF;
	break;
      }
      else if (last_char == '\n') {
	break;
      }
      else if (last_char == '\003') {
	goto line_start;
      }
      else {
	last_code_line[char_index++] = last_char;
      }
    }
    if (last_char == EOF) {
      write(sessionfd, "\n", 1);
      break;
    }

    last_code_line[char_index] = '\0';
#else
    char* line = readline(code_block_open ? "* " : "> ");
    if (line == NULL) {
      printf("\n");
      break;
    }
    strncpy(last_code_line, line, sizeof(last_code_line)-1);
    add_history(line);
    free(line);
#endif

    if ((strcmp(last_code_line, "quit") == 0) || (strcmp(last_code_line, "exit") == 0)) {
      if (!code_block_open) {
        break;
      }
      else{
        /* count the quit/exit commands as strings if in a quote block */
        strcat(ruby_code, "\n");
        strcat(ruby_code, last_code_line);
      }
    }
    else {
      if (code_block_open) {
        strcat(ruby_code, "\n");
        strcat(ruby_code, last_code_line);
      }
      else {
        strcpy(ruby_code, last_code_line);
      }
    }

    /* parse code */
    parser = mrb_parser_new(mrb);
    parser->s = ruby_code;
    parser->send = ruby_code + strlen(ruby_code);
    parser->lineno = cxt->lineno;
    mrb_parser_parse(parser, cxt);
    code_block_open = is_code_block_open(parser);

    if (code_block_open) {
      /* no evaluation of code */
    }
    else {
      if (0 < parser->nerr) {
        /* syntax error */
        char buf[4096];
        sprintf(buf, "line %d: %s\n", parser->error_buffer[0].lineno, parser->error_buffer[0].message);
        write(sessionfd, buf, strlen(buf));
      }
      else {
        /* generate bytecode */
        struct RProc *proc = mrb_generate_code(mrb, parser);

        if (verbose) {
          mrb_codedump_all(mrb, proc);
        }
        /* pass a proc for evaulation */
        /* evaluate the bytecode */
        result = mrb_context_run(mrb,
            proc,
            mrb_top_self(mrb),
            stack_keep);
        stack_keep = proc->body.irep->nlocals;
        /* did an exception occur? */
        if (mrb->exc) {
          p(mrb, mrb_obj_value(mrb->exc), 0, sessionfd);
          mrb->exc = 0;
        }
        else {
          /* no */
          if (!mrb_respond_to(mrb, result, mrb_intern_lit(mrb, "inspect"))){
            result = mrb_any_to_s(mrb,result);
          }
          p(mrb, result, 1, sessionfd);
        }
      }
      ruby_code[0] = '\0';
      last_code_line[0] = '\0';
      mrb_gc_arena_restore(mrb, ai);
    }
    mrb_parser_free(parser);
    cxt->lineno++;
  }

  mrbc_context_free(mrb, cxt);

  return 0;
}

void
mrb_mruby_bin_mocloudos_shell_gem_init(mrb_state *mrb)
{
}

void
mrb_mruby_bin_mocloudos_shell_gem_final(mrb_state *mrb)
{
}

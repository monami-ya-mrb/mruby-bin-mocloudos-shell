/* 
 * daytime.c: a simple network service based on lwIP and mini-os
 * 
 * Tim Deegan <Tim.Deegan@eu.citrix.net>, July 2007
 */
/*
** mirb - Embeddable Interactive Ruby Shell
**
** This program takes code from the user in
** an interactive way and executes it
** immediately. It's a REPL...
*/

#include <os.h>
#include <xmalloc.h>
#include <console.h>
#include <netfront.h>
#include <lwip/api.h>
#include <blkfront.h>

#include <stdlib.h>
#include <string.h>
#include "mruby.h"
#include "mruby/array.h"
#include "mruby/proc.h"
#include "mruby/compile.h"
#include "mruby/string.h"

#ifdef ENABLE_READLINE
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>

static const char *history_file_name = ".mirb_history";
char history_path[PATH_MAX];
#endif


static void
p(mrb_state *mrb, mrb_value obj, int prompt, struct netconn *session)
{
  obj = mrb_funcall(mrb, obj, "inspect", 0);
  if (prompt) {
    if (!mrb->exc) {
      netconn_write(session, " => ", 4, NETCONN_COPY);
    }
    else {
      obj = mrb_funcall(mrb, mrb_obj_value(mrb->exc), "inspect", 0);
    }
  }
  netconn_write(session, RSTRING_PTR(obj), RSTRING_LEN(obj), NETCONN_COPY);
  netconn_write(session, "\n", 1, NETCONN_COPY);
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

void mrb_show_version(mrb_state *);
void mrb_show_copyright(mrb_state *);

struct _args {
  mrb_bool verbose      : 1;
  int argc;
  char** argv;
};

static void
usage(const char *name)
{
  static const char *const usage_msg[] = {
  "switches:",
  "-v           print version number, then run in verbose mode",
  "--verbose    run in verbose mode",
  "--version    print the version",
  "--copyright  print the copyright",
  NULL
  };
  const char *const *p = usage_msg;

  printf("Usage: %s [switches]\n", name);
  while (*p)
    printf("  %s\n", *p++);
}

static int
parse_args(mrb_state *mrb, int argc, char **argv, struct _args *args)
{
  static const struct _args args_zero = { 0 };

  *args = args_zero;

  for (argc--,argv++; argc > 0; argc--,argv++) {
    char *item;
    if (argv[0][0] != '-') break;

    item = argv[0] + 1;
    switch (*item++) {
    case 'v':
      if (!args->verbose) mrb_show_version(mrb);
      args->verbose = 1;
      break;
    case '-':
      if (strcmp((*argv) + 2, "version") == 0) {
        mrb_show_version(mrb);
        exit(EXIT_SUCCESS);
      }
      else if (strcmp((*argv) + 2, "verbose") == 0) {
        args->verbose = 1;
        break;
      }
      else if (strcmp((*argv) + 2, "copyright") == 0) {
        mrb_show_copyright(mrb);
        exit(EXIT_SUCCESS);
      }
    default:
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

static void
cleanup(mrb_state *mrb, struct _args *args)
{
  mrb_close(mrb);
}

/* Print a short remark for the user */
static void
print_hint(struct netconn *session)
{
  static char msg[] = "mocloudos-shell - based on Embeddable Interactive Ruby Shell\n"
    "\nThis is a very early version, please test and report errors.\n"
    "Thanks :)\n\n";
  netconn_write(session, msg, strlen(msg), NETCONN_COPY);
}

#ifndef ENABLE_READLINE
/* Print the command line prompt of the REPL */
static void
print_cmdline(int code_block_open, struct netconn *session)
{
  if (code_block_open) {
    netconn_write(session, "* ", 2, NETCONN_COPY);
  }
  else {
    netconn_write(session, "> ", 2, NETCONN_COPY);
  }
}
#endif 

void mrb_codedump_all(mrb_state*, struct RProc*);

int
mirb_main(struct netconn *session, int argc, char **argv)
{
  char ruby_code[1024] = { 0 };
  char last_code_line[1024] = { 0 };
#ifndef ENABLE_READLINE
  int last_char;
  int char_index;
#else
  char *home = NULL;
#endif
  mrbc_context *cxt;
  struct mrb_parser_state *parser;
  mrb_state *mrb;
  mrb_value result;
  struct _args args;
  int n;
  mrb_bool code_block_open = FALSE;
  int ai;
  unsigned int stack_keep = 0;

  /* new interpreter instance */
  mrb = mrb_open();
  if (mrb == NULL) {
    fputs("Invalid mrb interpreter, exiting mirb\n", stderr);
    return EXIT_FAILURE;
  }
  mrb_define_global_const(mrb, "ARGV", mrb_ary_new_capa(mrb, 0));

  n = parse_args(mrb, argc, argv, &args);
  if (n == EXIT_FAILURE) {
    cleanup(mrb, &args);
    usage(argv[0]);
    return n;
  }

  print_hint(session);

  cxt = mrbc_context_new(mrb);
  cxt->capture_errors = 1;
  cxt->lineno = 1;
  mrbc_filename(mrb, cxt, "(mirb)");
  if (args.verbose) cxt->dump_result = 1;

  ai = mrb_gc_arena_save(mrb);

#ifdef ENABLE_READLINE
  using_history();
  home = getenv("HOME");
#ifdef _WIN32
  if (!home)
    home = getenv("USERPROFILE");
#endif
  if (home) {
    strcpy(history_path, home);
    strcat(history_path, "/");
    strcat(history_path, history_file_name);
    read_history(history_path);
  }
#endif

  char *buf = NULL;
  u16_t buflen = 0;

  while (TRUE) {
#ifndef ENABLE_READLINE
    struct netbuf *inbuf;

    print_cmdline(code_block_open, session);
netread:
    if (buflen == 0) {
      if (buf != NULL) {
        netbuf_delete(inbuf);
      }
      inbuf = netconn_recv(session);
      if (netconn_err(session) != ERR_OK) {
        fprintf(stderr, "something wrong?");
        break;
      }
      netbuf_data(inbuf, (void **)&buf, &buflen);
    }
    char_index = 0;
    while (1) {
      if (buflen-- == 0) {
        goto netread;
      }
      last_char = *buf++;
      if (last_char == '\n' ||
          last_char == EOF) break;
      last_code_line[char_index++] = last_char;
    }
    if (last_char == EOF) {
      netconn_write(session, "\n", 1, NETCONN_COPY);
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
        netconn_write(session, buf, strlen(buf), NETCONN_COPY);
      }
      else {
        /* generate bytecode */
        struct RProc *proc = mrb_generate_code(mrb, parser);

        if (args.verbose) {
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
          p(mrb, mrb_obj_value(mrb->exc), 0, session);
          mrb->exc = 0;
        }
        else {
          /* no */
          if (!mrb_respond_to(mrb, result, mrb_intern_lit(mrb, "inspect"))){
            result = mrb_any_to_s(mrb,result);
          }
          p(mrb, result, 1, session);
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
  mrb_close(mrb);

#ifdef ENABLE_READLINE
  write_history(history_path);
#endif

  return 0;
}

static const char argv_base[] = "mirb";

void
run_mirb(void *session)
{
  mirb_main(session, 1, &argv_base);
  (void) netconn_disconnect(session);
  (void) netconn_delete(session);
}

int
main(int argc, char **argv)
{
    struct ip_addr listenaddr = { 0 };
    struct netconn *listener;
    struct netconn *session;
    struct timeval tv;
    err_t rc;

    initialize_block_devices();

    printf("Opening connection\n");

    listener = netconn_new(NETCONN_TCP);
    printf("Connection at %p\n", listener);

    rc = netconn_bind(listener, &listenaddr, 13);
    if (rc != ERR_OK) {
        printf("Failed to bind connection: %i\n", rc);
        return rc;
    }
    rc = netconn_listen(listener);
    if (rc != ERR_OK) {
        printf("Failed to listen on connection: %i\n", rc);
        return rc;
    }

    while (1) {
        session = netconn_accept(listener);
        if (session == NULL) 
            continue;
	create_thread("mirb", run_mirb, session);
    }

    return 0;
}

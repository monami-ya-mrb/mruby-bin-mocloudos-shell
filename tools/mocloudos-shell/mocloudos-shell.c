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
#include <sys/socket.h>

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

static int
parse_args(mrb_state *mrb, int argc, char **argv, mrb_bool *p_verbose)
{
  for (argc--,argv++; argc > 0; argc--,argv++) {
    char *item;
    if (argv[0][0] != '-') break;

    item = argv[0] + 1;
    switch (*item++) {
    case 'v':
      if (!*p_verbose) mrb_show_version(mrb);
      *p_verbose = 1;
      break;
    case '-':
      if (strcmp((*argv) + 2, "version") == 0) {
        mrb_show_version(mrb);
        exit(EXIT_SUCCESS);
      }
      else if (strcmp((*argv) + 2, "verbose") == 0) {
        *p_verbose = 1;
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

/* Print a short remark for the user */
static void
print_hint(int sessionfd)
{
  static char msg[] = "mocloudos-shell - based on Embeddable Interactive Ruby Shell\n"
    "\nThis is a very early version, please test and report errors.\n"
    "Thanks :)\n\n";
  write(sessionfd, msg, strlen(msg));
}

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

extern void mrb_show_version(mrb_state *);
extern void mrb_show_copyright(mrb_state *);

static const char argv_base[] = "mirb";

int
mirb_main(int sessionfd, int argc, char **argv)
{
  mrb_state *mrb;
  mrb_bool verbose;
  int n;

  /* new interpreter instance */
  mrb = mrb_open();
  if (mrb == NULL) {
    fputs("Invalid mrb interpreter, exiting mirb\n", stderr);
    return EXIT_FAILURE;
  }

  n = parse_args(mrb, argc, argv, &verbose);
  if (n == EXIT_FAILURE) {
    mrb_close(mrb);
    usage(argv[0]);
    return n;
  }

  print_hint(sessionfd);

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

  mirb_repl(sessoinfd, mrb);

  mrb_close(mrb);

#ifdef ENABLE_READLINE
  write_history(history_path);
#endif
}

static void
run_mirb(void *p_sessionfd)
{
  int sessionfd = *(int *)p_sessionfd;
  free(p_sessionfd);

  mirb_main(sessionfd, 1, (char **)&argv_base);
  (void) shutdown(sessionfd, 2);
  (void) close(sessionfd);
}

int
main(int argc, char **argv)
{
  int sockfd;
  struct sockaddr_in sin;
  const unsigned short port = 25;

  printf("Opening connection\n");

  sockfd = socket(AF_INET, SOCK_STREAM, 0);

  sin.sin_family = PF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sockfd, (struct sockaddr *)&sin, sizeof(struct sockaddr_in)) < 0) {
    close(sockfd);
    return 1;
  }

  if (listen(sockfd, 2) < 0) {
    close(sockfd);
    return 1;
  }

  while (1) {
    int *p_acceptfd;
    struct sockaddr_in addr;
    socklen_t addr_len;

    p_acceptfd = malloc(sizeof(int));
    *p_acceptfd = accept(sockfd, (struct sockaddr *)&addr, &addr_len);

    if (*p_acceptfd < 0) {
      break;
    }

    create_thread("mirb", run_mirb, p_acceptfd);
  }

    return 0;
}

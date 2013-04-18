/* Copyright 2011 Nick Mathewson, George Kadianakis
   See LICENSE for other credits and copying information
*/

/**
 * \file main.c
 * \headerfile main.h
 * \brief Entry point of obfsproxy. Does command-line parsing, and
 * switches into 'external' or 'managed' proxy mode.
 *
 * \details Practically, obfs_main.c is the actual entry point of
 * obfsproxy, but all it really does is call obfs_main().
 **/

#include "util.h"

#include "container.h"
#include "crypt.h"
#include "network.h"
#include "protocol.h"

#include "managed.h"
#include "external.h"

#include "status.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>

#include <event2/event.h>
#include <event2/dns.h>

static struct event_base *the_event_base;
static struct event *sig_int;
static struct event *sig_term;
static struct event *heartbeat;

/* Pluggable transport proxy mode. ('External' or 'Managed') */
static int is_external_proxy=1;
/* Whether to scrub connection addresses -- on by default */
int safe_logging=1;

/**
   Prints the obfsproxy usage instructions then exits.
*/
void ATTR_NORETURN
usage(void)
{
  int i;
  fputs("Usage: obfsproxy [obfsproxy_args] protocol_name [protocol_args] "
        "protocol_options protocol_name ...\n"
        "* Available protocols:\n", stderr);
  /* this is awful. */
  for (i=0;i<n_supported_protocols;i++)
    fprintf(stderr,"[%s] ", supported_protocols[i]->name);
  fprintf(stderr, "\n* obfsproxy_args:\n"
          "--log-file=<file> ~ set logfile\n"
          "--log-min-severity=warn|notice|info|debug ~ "
          "set minimum logging severity (default: notice)\n"
          "--no-log ~ disable logging\n"
          "--no-safe-logging ~ disable safe (scrubbed address) logging\n");

  exit(1);
}

/**
   This is called when we receive a signal.
   It figures out the signal type and acts accordingly.

   Current behavior:
   SIGINT: On a single SIGINT we stop accepting new connections,
           keep the already existing connections open,
           and terminate when they all close.
           On a second SIGINT we shut down immediately but cleanly.
   SIGTERM: Shut down obfsproxy immediately but cleanly.
*/
static void
handle_signal_cb(evutil_socket_t fd, short what, void *arg)
{
  int signum = (int) fd;
  static int got_sigint=0;

  switch (signum) {
  case SIGINT:
    close_all_listeners();
    if (!got_sigint) {
      log_notice("Got SIGINT. Preparing shutdown.");
      start_shutdown(0);
      got_sigint++;
    } else {
      log_notice("Got SIGINT for the second time. Terminating.");
      start_shutdown(1);
    }
    break;
  case SIGTERM:
    log_notice("Got SIGTERM. Terminating.");
    start_shutdown(1);
    break;
  }
}

/**
   Returns the libevent event base used by obfsproxy.
*/
struct event_base *
get_event_base(void)
{
  return the_event_base;
}

/** Stop obfsproxy's event loop. Final cleanup happens in main(). */
void
finish_shutdown(void)
{
  log_debug("Finishing shutdown.");
  event_base_loopexit(the_event_base, NULL);
}

/** Return 1 if 'name' is the name of a supported protocol, otherwise 0. */
int
is_supported_protocol(const char *name)
{
  int i;
  for (i = 0; i < n_supported_protocols; i++)
    if (!strcmp(name, supported_protocols[i]->name))
      return 1;

  return 0;
}

/**
   Receives 'argv' and scans for any obfsproxy optional arguments and
   tries to set them in effect.

   If it succeeds it returns the number of argv arguments its caller
   should skip to get past the optional arguments we already handled.
   If it fails, it exits obfsproxy.
*/
static int
handle_obfsproxy_args(const char *const *argv)
{
  int logfile_set=0;
  int logsev_set=0;
  int i=1;

  while (argv[i] &&
         !strncmp(argv[i],"--",2)) {
    if (!strncmp(argv[i], "--log-file=", 11)) {
      if (logfile_set)
        log_error("You've already set a log file!");
      if (log_set_method(LOG_METHOD_FILE, (char *)argv[i]+11) < 0)
        log_error("Failed creating logfile.");

      logfile_set=1;
    } else if (!strncmp(argv[i], "--log-min-severity=", 19)) {
      if (logsev_set)
        log_error("You've already set a min. log severity!");
      if (log_set_min_severity((char *)argv[i]+19) < 0)
        log_error("Error at setting logging severity");

      logsev_set=1;
    } else if (!strncmp(argv[i], "--no-log", 9)) {
      if (logsev_set) {
        printf("You've already set a min. log severity!\n");
        exit(1);
      }
      if (log_set_method(LOG_METHOD_NULL, NULL) < 0) {
        printf("Error at setting logging severity.\n");
        exit(1);
      }

      logsev_set=1;
    } else if (!strncmp(argv[i], "--no-safe-logging", 18)) {
      safe_logging=0;
    } else if (!strncmp(argv[i], "--managed", 10)) {
      is_external_proxy=0;
    } else {
      log_error("Unrecognizable obfsproxy argument '%s'", argv[i]);
    }

    i++;
  }

  if (!is_external_proxy && !logfile_set && logsev_set) {
    printf("obfsproxy in managed proxy mode can only log to a file.\n");
    exit(1);
  } else if (!is_external_proxy && !logfile_set) { /* && !logsev_set */
    /* managed proxies without a log file must not log at all. */
    if (log_set_method(LOG_METHOD_NULL, NULL) < 0) {
      printf("Error at setting logging severity.\n");
      exit(1);
    }
  }

  return i;
}

/** Libevent callback: Log heartbeat message once every hour. */
static void
heartbeat_cb(evutil_socket_t fd, short what, void *arg)
{
  (void) fd;
  (void) what;
  (void) arg;
  status_log_heartbeat();
}

/**
   Initialize basic components of obfsproxy.
*/
void
obfsproxy_init()
{
  /* Ugly method to fix a Windows problem:
     http://archives.seul.org/libevent/users/Oct-2010/msg00049.html */
#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(0x101, &wsaData);
#endif

  /* Initialize crypto */
  if (initialize_crypto() < 0) {
    log_error("Failed to initialize cryptography.");
  }

  /* Initialize libevent */
  the_event_base = event_base_new();
  if (!the_event_base) {
    log_error("Failed to initialize networking.");
  }

  /* ASN should this happen only when SOCKS is enabled? */
  if (init_evdns_base(the_event_base)) {
    log_error("Failed to initialize DNS resolver.");
  }

  /* Handle signals. */
#ifdef SIGPIPE
   signal(SIGPIPE, SIG_IGN);
#endif
  sig_int = evsignal_new(the_event_base, SIGINT,
                         handle_signal_cb, NULL);
  sig_term = evsignal_new(the_event_base, SIGTERM,
                          handle_signal_cb, NULL);
  if (event_add(sig_int,NULL) || event_add(sig_term,NULL)) {
    log_error("Failed to initialize signal handling.");
  }

  /* Initialize hourly heartbeat messages. */
  struct timeval one_hour;
  one_hour.tv_sec = 3600;
  one_hour.tv_usec = 0;
  status_init();
  heartbeat = event_new(the_event_base, -1, EV_PERSIST, heartbeat_cb, NULL);
  if (event_add(heartbeat, &one_hour)) {
    log_error("Failed to initialize heartbeat logs.");
  }
}

/**
   Clean the mess that obfsproxy made all over this computer's memory.
*/
void
obfsproxy_cleanup()
{
  /* We have landed. */
  log_notice("Exiting.");

  close_all_listeners();
  evdns_base_free(get_evdns_base(), 0);
  event_free(sig_int);
  event_free(sig_term);
  if (heartbeat)
    event_free(heartbeat);

  event_base_free(get_event_base());

  cleanup_crypto();
  status_connections_cleanup();
  close_obfsproxy_logfile();
}


/** Entry point */
int
obfs_main(int argc, const char *const *argv)
{
  const char *const *begin;

  /* Handle optional obfsproxy arguments. */
  begin = argv + handle_obfsproxy_args(argv);
  log_notice("Starting.");

  if (is_external_proxy) {
    if (launch_external_proxy(begin) < 0)
      return 1;
  } else {
    if (launch_managed_proxy() < 0)
      return 1;
  }

  return 0;
}


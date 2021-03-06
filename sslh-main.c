/*
# main: processing of config file, command line options and start the main
# loop.
#
# Copyright (C) 2007-2012  Yves Rutschle
# 
# This program is free software; you can redistribute it
# and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later
# version.
# 
# This program is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied
# warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
# PURPOSE.  See the GNU General Public License for more
# details.
# 
# The full text for the General Public License is here:
# http://www.gnu.org/licenses/gpl.html

*/

#define _GNU_SOURCE
#ifdef LIBCONFIG
#include <libconfig.h>
#endif
#include <regex.h>
#include <sys/stat.h>

#include "common.h"
#include "probe.h"
#include "ip-map.h"

const char* USAGE_STRING =
"sslh " VERSION "\n" \
"usage:\n" \
"\tsslh  [-v] [-i] [-V] [-f] [-n] [-F <file>]\n"
"\t[-t <timeout>] [-P <pidfile>] -u <username> -p <add> [-p <addr> ...] \n" \
"%s\n\n" /* Dynamically built list of builtin protocols */  \
"\t[--on-timeout <addr>]\n" \
"-v: verbose\n" \
"-V: version\n" \
"-f: foreground\n" \
"-n: numeric output\n" \
"-F: use configuration file\n" \
"--on-timeout: connect to specified address upon timeout (default: ssh address)\n" \
"-t: seconds to wait before connecting to --on-timeout address.\n" \
"-p: address and port to listen on.\n    Can be used several times to bind to several addresses.\n" \
"--[ssh,ssl,...]: where to connect connections from corresponding protocol.\n" \
"-F: specify a configuration file\n" \
"-P: PID file.\n" \
"-i: Run as a inetd service.\n" \
"";

/* Constants for options that have no one-character shorthand */
#define OPT_ONTIMEOUT   257

static struct option const_options[] = {
    { "inetd",      no_argument,            &inetd,         1 },
    { "foreground", no_argument,            &foreground,    1 },
    { "background", no_argument,            &background,    1 },
    { "numeric",    no_argument,            &numeric,       1 },
    { "verbose",    no_argument,            &verbose,       1 },
    { "user",       required_argument,      0,              'u' },
    { "config",     required_argument,      0,              'F' },
    { "pidfile",    required_argument,      0,              'P' },
    { "timeout",    required_argument,      0,              't' },
    { "on-timeout", required_argument,      0,              OPT_ONTIMEOUT },
    { "listen",     required_argument,      0,              'p' },
    {}
};
static struct option* all_options;
static struct proto* builtins;
static const char *optstr = "vt:T:p:VP:F:";



static void print_usage(void)
{
    struct proto *p;
    int i;
    char *prots = "";

    p = get_builtins();
    for (i = 0; i < get_num_builtins(); i++)
        asprintf(&prots, "%s\t[--%s <addr>]\n", prots, p[i].description);

    fprintf(stderr, USAGE_STRING, prots);
}

static void printsettings(void)
{
    char buf[NI_MAXHOST];
    struct addrinfo *a;
    struct proto *p;
    
    for (p = get_first_protocol(); p; p = p->next) {
        fprintf(stderr,
                "%s addr: %s. libwrap service: %s family %d %d\n", 
                p->description, 
                sprintaddr(buf, sizeof(buf), p->saddr), 
                p->service,
                p->saddr->ai_family,
                p->saddr->ai_addr->sa_family);
    }
    fprintf(stderr, "listening on:\n");
    for (a = addr_listen; a; a = a->ai_next) {
        fprintf(stderr, "\t%s\n", sprintaddr(buf, sizeof(buf), a));
    }
    fprintf(stderr, "timeout: %d\non-timeout: %s\n", probing_timeout,
            timeout_protocol()->description);
}


/* Extract configuration on addresses and ports on which to listen.
 * out: newly allocated list of addrinfo to listen to
 */
#ifdef LIBCONFIG
static int config_listen(config_t *config, struct addrinfo **listen) 
{
    config_setting_t *setting, *addr;
    int len, i;
    const char *hostname, *port;

    setting = config_lookup(config, "listen");
    if (setting) {
        len = config_setting_length(setting);
        for (i = 0; i < len; i++) {
            addr = config_setting_get_elem(setting, i);
            if (! (config_setting_lookup_string(addr, "host", &hostname) &&
                   config_setting_lookup_string(addr, "port", &port))) {
                fprintf(stderr,
                            "line %d:Incomplete specification (hostname and port required)\n",
                            config_setting_source_line(addr));
                return -1;
            }

            resolve_split_name(listen, hostname, port);

            /* getaddrinfo returned a list of addresses corresponding to the
             * specification; move the pointer to the end of that list before
             * processing the next specification */
            for (; *listen; listen = &((*listen)->ai_next));
        }
    }

    return 0;
}
#endif



#ifdef LIBCONFIG
static void setup_regex_probe(struct proto *p, config_setting_t* probes)
{
    int num_probes, errsize, i, res;
    char *err;
    const char * expr;
    regex_t** probe_list;

    num_probes = config_setting_length(probes);
    if (!num_probes) {
        fprintf(stderr, "%s: no probes specified\n", p->description);
        exit(1);
    }

    p->probe = get_probe("regex");
    probe_list = calloc(num_probes + 1, sizeof(*probe_list));
    p->data = (void*)probe_list;

    for (i = 0; i < num_probes; i++) {
        probe_list[i] = malloc(sizeof(*(probe_list[i])));
        expr = config_setting_get_string_elem(probes, i);
        res = regcomp(probe_list[i], expr, 0);
        if (res) {
            err = malloc(errsize = regerror(res, probe_list[i], NULL, 0));
            regerror(res, probe_list[i], err, errsize);
            fprintf(stderr, "%s:%s\n", expr, err);
            free(err);
            exit(1);
        }
    }
}
#endif

/* Extract configuration for protocols to connect to.
 * out: newly-allocated list of protocols
 */
#ifdef LIBCONFIG
static int config_protocols(config_t *config, struct proto **prots)
{
    config_setting_t *setting, *prot, *probes;
    const char *hostname, *port, *name;
    int i, num_prots;
    struct proto *p, *prev = NULL;

    setting = config_lookup(config, "protocols");
    if (setting) {
        num_prots = config_setting_length(setting);
        for (i = 0; i < num_prots; i++) {
            p = calloc(1, sizeof(*p));
            if (i == 0) *prots = p;
            if (prev) prev->next = p;
            prev = p;

            prot = config_setting_get_elem(setting, i);
            if ((config_setting_lookup_string(prot, "name", &name) &&
                 config_setting_lookup_string(prot, "host", &hostname) &&
                 config_setting_lookup_string(prot, "port", &port)
                )) {
                p->description = name;
                config_setting_lookup_string(prot, "service", &(p->service));

                resolve_split_name(&(p->saddr), hostname, port);


                probes = config_setting_get_member(prot, "probe");
                if (probes) {
                    if (config_setting_is_array(probes)) {
                        /* If 'probe' is an array, setup a regex probe using the
                         * array of strings as pattern */

                        setup_regex_probe(p, probes);

                    } else {
                        /* if 'probe' is 'builtin', set the probe to the
                         * appropriate builtin protocol */
                        if (!strcmp(config_setting_get_string(probes), "builtin")) {
                            p->probe = get_probe(name);
                            if (!p->probe) {
                                fprintf(stderr, "%s: no builtin probe for this protocol\n", name);
                                exit(1);
                            }
                        } else {
                            fprintf(stderr, "%s: illegal probe name\n", name);
                            exit(1);
                        }
                    }
                }
            }
        }
    }

    return 0;
}
#endif

/* Parses a config file
 * in: *filename
 * out: *listen, a newly-allocated linked list of listen addrinfo
 *      *prots, a newly-allocated linked list of protocols
 */
#ifdef LIBCONFIG
static int config_parse(char *filename, struct addrinfo **listen, struct proto **prots)
{
    config_t config;
    long int timeout;
    const char* str;

    config_init(&config);
    if (config_read_file(&config, filename) == CONFIG_FALSE) {
        fprintf(stderr, "%s:%d:%s\n", 
                    filename,
                    config_error_line(&config),
                    config_error_text(&config));
        exit(1);
    }

    config_lookup_bool(&config, "verbose", &verbose);
    config_lookup_bool(&config, "inetd", &inetd);
    config_lookup_bool(&config, "foreground", &foreground);
    config_lookup_bool(&config, "numeric", &numeric);

    if (config_lookup_int(&config, "timeout", &timeout) == CONFIG_TRUE) {
        probing_timeout = timeout;
    }

    if (config_lookup_string(&config, "on-timeout", &str)) {
        set_ontimeout(str);
    }

    config_lookup_string(&config, "user", &user_name);
    config_lookup_string(&config, "pidfile", &pid_file);
    config_lookup_string(&config, "mapsock", &map_sock_path);

    config_listen(&config, listen);
    config_protocols(&config, prots);

    return 0;
}
#endif

/* Adds protocols to the list of options, so command-line parsing uses the
 * protocol definition array 
 * options: array of options to add to; must be big enough
 * n_opts: number of options in *options before calling (i.e. where to append)
 * prot: array of protocols
 * n_prots: number of protocols in *prot
 * */
static void append_protocols(struct option *options, int n_opts, struct proto *prot , int n_prots)
{
    int o, p;

    for (o = n_opts, p = 0; p < n_prots; o++, p++) {
        options[o].name = prot[p].description;
        options[o].has_arg = required_argument;
        options[o].flag = 0;
        options[o].val = p + PROT_SHIFT;
    }
}

static void make_alloptions(void)
{
    builtins = get_builtins();

    /* Create all_options, composed of const_options followed by one option per
     * known protocol */
    all_options = calloc(ARRAY_SIZE(const_options) + get_num_builtins(), sizeof(struct option));
    memcpy(all_options, const_options, sizeof(const_options));
    append_protocols(all_options, ARRAY_SIZE(const_options) - 1, builtins, get_num_builtins());
}

/* Performs a first scan of command line options to see if a configuration file
 * is specified. If there is one, parse it now before all other options (so
 * configuration file settings can be overridden from the command line).
 *
 * prots: newly-allocated list of configured protocols, if any.
 */
static void cmdline_config(int argc, char* argv[], struct proto** prots)
{
#ifdef LIBCONFIG
    int c, res;
    char *config_filename;
#endif

    make_alloptions();

#ifdef LIBCONFIG
    optind = 1;
    opterr = 0; /* we're missing protocol options at this stage so don't output errors */
    while ((c = getopt_long_only(argc, argv, optstr, all_options, NULL)) != -1) {
        if (c == 'F') {
            config_filename = optarg;
            /* find the end of the listen list */
            res = config_parse(config_filename, &addr_listen, prots);
            if (res)
                exit(4);
            break;
        }
    }
#endif
}


/* Parse command-line options. prots points to a list of configured protocols,
 * potentially non-allocated */
static void parse_cmdline(int argc, char* argv[], struct proto* prots)
{
    int c;
    struct addrinfo **a;
    struct proto *p;

    optind = 1;
    opterr = 1;
next_arg:
    while ((c = getopt_long_only(argc, argv, optstr, all_options, NULL)) != -1) {
        if (c == 0) continue;

        if (c >= PROT_SHIFT) {
            if (prots)
                for (p = prots; p && p->next; p = p->next) {
                    /* override if protocol was already defined by config file 
                     * (note it only overrides address and use builtin probe) */
                    if (!strcmp(p->description, builtins[c-PROT_SHIFT].description)) {
                        resolve_name(&(p->saddr), optarg);
                        p->probe = builtins[c-PROT_SHIFT].probe;
                        goto next_arg;
                    }
                }
            /* At this stage, it's a new protocol: add it to the end of the
             * list */
            if (!prots) {
                /* No protocols yet -- create the list */
                p = prots = calloc(1, sizeof(*p));
            } else {
                p->next = calloc(1, sizeof(*p));
                p = p->next;
            }
            memcpy(p, &builtins[c-PROT_SHIFT], sizeof(*p));
            resolve_name(&(p->saddr), optarg);
            continue;
        }

        switch (c) {

        case 'F':
            /* Legal option, but do nothing, it was already processed in
             * cmdline_config() */
#ifndef LIBCONFIG
            fprintf(stderr, "Built without libconfig support: configuration file not available.\n");
            exit(1);
#endif
            break;

        case 't':
             probing_timeout = atoi(optarg);
            break;

        case OPT_ONTIMEOUT:
            set_ontimeout(optarg);
            break;

        case 'p':
            /* find the end of the listen list */
            for (a = &addr_listen; *a; a = &((*a)->ai_next));
            /* append the specified addresses */
            resolve_name(a, optarg);
            
            break;

        case 'V':
            printf("%s %s\n", server_type, VERSION);
            exit(0);

        case 'u':
            user_name = optarg;
            break;

        case 'P':
            pid_file = optarg;
            break;

        case 'm':
            map_sock_path = optarg;
            break;

        case 'v':
            verbose++;
            break;

        default:
            print_usage();
            exit(2);
        }
    }

    if (!prots) {
        fprintf(stderr, "At least one target protocol must be specified.\n");
        exit(2);
    }

    set_protocol_list(prots);

    if (!addr_listen) {
        fprintf(stderr, "No listening address specified; use at least one -p option\n");
        exit(1);
    }

    /* Did command-line override foreground setting? */
    if (background)
        foreground = 0;

}

int main(int argc, char *argv[])
{

   extern char *optarg;
   extern int optind;
   int res, num_addr_listen;
   struct proto* protocols = NULL;

   int *listen_sockets, *map_socket;

   /* Init defaults */
   pid_file = NULL;
   user_name = NULL;
   map_sock_path = NULL;

   cmdline_config(argc, argv, &protocols);
   parse_cmdline(argc, argv, protocols);

   if (inetd)
   {
       verbose = 0;
       start_shoveler(0);
       exit(0);
   }

   if (verbose)
       printsettings();

   num_addr_listen = start_listen_sockets(&listen_sockets, addr_listen);

   if(map_sock_path)
   {
      struct sockaddr_un map_sockaddr;
      map_sockaddr.sun_family = AF_UNIX;
      strcpy(map_sockaddr.sun_path, map_sock_path);
      unlink(map_sockaddr.sun_path);

      struct addrinfo map_addr_listen;
      memset(&map_addr_listen, 0, sizeof(map_addr_listen));
      map_addr_listen.ai_addr = (struct sockaddr *)&map_sockaddr;
      map_addr_listen.ai_addrlen = strlen(map_sockaddr.sun_path) + sizeof(map_sockaddr.sun_family);

      int s;
      map_socket = &s;
      mode_t umask_ = umask(0000);
      res = start_listen_sockets(&map_socket, &map_addr_listen);
      umask(umask_);
   }
   else
      map_socket = NULL;

   if (!foreground) {
       if (fork() > 0) exit(0); /* Detach */

       /* New session -- become group leader */
       if (getuid() == 0) {
           res = setsid();
           CHECK_RES_DIE(res, "setsid: already process leader");
       }
   }

   setup_signals();

   if (pid_file)
       write_pid_file(pid_file);

   if (user_name)
       drop_privileges(user_name);

   /* Open syslog connection */
   setup_syslog(argv[0]);

   ip_map_init();

   main_loop(listen_sockets, num_addr_listen, map_socket);

   ip_map_close();

   if(map_sock_path)
       unlink(map_sock_path);

   if (pid_file)
       unlink(pid_file);

   return 0;
}

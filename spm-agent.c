/*
 * spm-agent.c - Background update agent for spm
 *
 * Runs as a daemon, periodically checks repositories for package
 * updates. Writes status to /opt/sst/var/update.status so the
 * GUI and CLI can report available updates.
 *
 * Started via /etc/init.d/spm-agent or manually:
 *   spm-agent [-f] [-i interval]
 *
 *   -f            Run in foreground (don't daemonize)
 *   -i <seconds>  Check interval (default: from config, or 6 hours)
 *
 * Signals:
 *   SIGHUP   - Reload config and re-check immediately
 *   SIGTERM   - Clean shutdown
 *   SIGUSR1   - Force immediate check
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "config.h"
#include "http.h"
#include "json.h"
#include "html.h"
#include "pkgdb.h"

/* ================================================================
 * GLOBALS
 * ================================================================ */

static spm_config_t g_config;
static volatile int g_running = 1;
static volatile int g_reload = 0;
static volatile int g_force_check = 0;
static int g_foreground = 0;
static FILE *g_logfp;

/* ================================================================
 * LOGGING
 * ================================================================ */

static void agent_log(const char *fmt, ...)
{
    va_list ap;
    time_t now;
    struct tm *tm;
    char timebuf[64];
    FILE *out;

    out = g_logfp ? g_logfp : stderr;

    now = time(NULL);
    tm = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(out, "[%s] ", timebuf);

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fprintf(out, "\n");
    fflush(out);
}

/* ================================================================
 * PID FILE
 * ================================================================ */

static int write_pid_file(void)
{
    FILE *f;

    f = fopen(SPM_AGENT_PID, "w");
    if (!f) {
        agent_log("cannot write PID file %s: %s",
                  SPM_AGENT_PID, strerror(errno));
        return -1;
    }
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    return 0;
}

static void remove_pid_file(void)
{
    unlink(SPM_AGENT_PID);
}

static int check_already_running(void)
{
    FILE *f;
    char line[64];
    pid_t pid;

    f = fopen(SPM_AGENT_PID, "r");
    if (!f)
        return 0;  /* No PID file, not running */

    if (fgets(line, sizeof(line), f)) {
        pid = (pid_t)atoi(line);
        fclose(f);

        if (pid > 0 && kill(pid, 0) == 0) {
            /* Process exists */
            return 1;
        }
        /* Stale PID file */
        unlink(SPM_AGENT_PID);
        return 0;
    }

    fclose(f);
    return 0;
}

/* ================================================================
 * SIGNAL HANDLERS
 * ================================================================ */

static void sig_term(int sig)
{
    (void)sig;
    g_running = 0;
}

static void sig_hup(int sig)
{
    (void)sig;
    g_reload = 1;
}

static void sig_usr1(int sig)
{
    (void)sig;
    g_force_check = 1;
}

/* ================================================================
 * DAEMONIZE
 * ================================================================ */

static int daemonize(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0) {
        perror("spm-agent: fork");
        return -1;
    }
    if (pid > 0) {
        /* Parent exits */
        _exit(0);
    }

    /* Child: new session */
    setsid();

    /* Second fork to prevent terminal reacquisition */
    pid = fork();
    if (pid < 0) {
        perror("spm-agent: fork2");
        return -1;
    }
    if (pid > 0) {
        _exit(0);
    }

    /* Close stdin/stdout/stderr, redirect to /dev/null */
    close(0);
    close(1);
    close(2);
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
    }

    /* Change to root dir so we don't hold any mount */
    chdir("/");

    umask(022);

    return 0;
}

/* ================================================================
 * UPDATE CHECK
 * ================================================================ */

static int check_for_updates(void)
{
    pkgdb_t *db;
    pkgdb_t *fresh_db;
    int updates_available = 0;
    int i;
    FILE *sf;

    agent_log("checking for updates...");

    /* Load current installed state */
    db = pkgdb_new();
    pkgdb_load_index(db);
    pkgdb_load_installed(db);
    pkgdb_load_spool(db, "/var/spool/pkg");

    /* Fetch fresh index data by calling spm update in a subprocess.
     * This is simpler than duplicating the full update logic and ensures
     * we use the same code paths as the CLI. */
    {
        int ret;
        ret = system(SPM_CLI " update >/dev/null 2>&1");
        if (ret != 0) {
            if (ret == -1)
                agent_log("cannot start update command: %s", strerror(errno));
            else if (WIFEXITED(ret))
                agent_log("update command failed (exit %d)", WEXITSTATUS(ret));
            else if (WIFSIGNALED(ret))
                agent_log("update command terminated by signal %d", WTERMSIG(ret));
            else
                agent_log("update command failed (wait status %d)", ret);
            pkgdb_free(db);
            return -1;
        }
    }

    /* Reload with fresh index */
    fresh_db = pkgdb_new();
    pkgdb_load_index(fresh_db);
    pkgdb_load_installed(fresh_db);
    pkgdb_load_spool(fresh_db, "/var/spool/pkg");

    /* Compare: find installed packages that have newer versions available */
    for (i = 0; i < fresh_db->inst_count; i++) {
        const installed_pkg_t *inst = &fresh_db->installed[i];
        int avail_idx;

        avail_idx = pkgdb_find_avail(fresh_db, inst->name);
        if (avail_idx >= 0) {
            const avail_pkg_t *avail = &fresh_db->available[avail_idx];
            /* Simple version string comparison */
            if (strcmp(avail->version, inst->version) != 0) {
                agent_log("  update available: %s %s -> %s",
                          inst->name, inst->version, avail->version);
                updates_available++;
            }
        }
    }

    /* Write status file */
    sf = fopen(SPM_UPDATE_STATUS, "w");
    if (sf) {
        time_t now = time(NULL);
        fprintf(sf, "updates=%d\n", updates_available);
        fprintf(sf, "checked=%ld\n", (long)now);
        fprintf(sf, "total_available=%d\n", fresh_db->avail_count);
        fprintf(sf, "total_installed=%d\n", fresh_db->inst_count);
        fclose(sf);
    }

    agent_log("check complete: %d update%s available",
              updates_available, updates_available != 1 ? "s" : "");

    pkgdb_free(db);
    pkgdb_free(fresh_db);

    return updates_available;
}

/* ================================================================
 * MAIN LOOP
 * ================================================================ */

static void run_agent(void)
{
    int interval;
    int elapsed;

    interval = g_config.agent_interval;
    if (interval < 60)
        interval = 60;

    agent_log("spm-agent started (pid %ld, interval %d sec)",
              (long)getpid(), interval);

    /* Check if we've already run recently (avoid redundant check on quick restart) */
    {
        struct stat sb;
        if (stat(SPM_UPDATE_STATUS, &sb) == 0) {
            time_t now = time(NULL);
            if (now - sb.st_mtime < 300) {
                agent_log("last check was %ld seconds ago, skipping initial check",
                          (long)(now - sb.st_mtime));
                goto skip_initial;
            }
        }
    }

    /* Initial check on startup */
    check_for_updates();

    /* Boot notification via console */
    {
        FILE *con = fopen("/dev/console", "w");
        if (con) {
            fprintf(con, "spm-agent: update check complete\n");
            fclose(con);
        }
    }

skip_initial:

    elapsed = 0;

    while (g_running) {
        sleep(1);
        elapsed++;

        /* Handle reload signal */
        if (g_reload) {
            g_reload = 0;
            agent_log("reloading configuration...");
            config_defaults(&g_config);
            config_load(&g_config);
            interval = g_config.agent_interval;
            if (interval < 60)
                interval = 60;
            agent_log("new interval: %d sec", interval);
            /* Force immediate re-check after reload */
            elapsed = interval;
        }

        /* Handle forced check */
        if (g_force_check) {
            g_force_check = 0;
            agent_log("forced check requested");
            elapsed = interval;
        }

        /* Time to check? */
        if (elapsed >= interval) {
            elapsed = 0;
            check_for_updates();
        }
    }

    agent_log("spm-agent shutting down");
}

/* ================================================================
 * USAGE
 * ================================================================ */

static void usage(void)
{
    fprintf(stderr,
        "spm-agent %s - Background update agent\n"
        "\n"
        "Usage: spm-agent [-f] [-i interval]\n"
        "\n"
        "Options:\n"
        "  -f             Run in foreground (don't daemonize)\n"
        "  -i <seconds>   Check interval (default: from config)\n"
        "  -k             Kill running agent\n"
        "  -s             Show agent status\n"
        "\n"
        "Signals:\n"
        "  SIGHUP   Reload config, re-check immediately\n"
        "  SIGUSR1  Force immediate check\n"
        "  SIGTERM  Clean shutdown\n"
        "\n"
        "Config: %s [agent] section\n"
        "PID: %s\n"
        "Log: %s\n"
        "Status: %s\n",
        SPM_VERSION,
        SPM_CONF, SPM_AGENT_PID,
        SPM_AGENT_LOG, SPM_UPDATE_STATUS);
}

/* Kill running agent */
static int kill_agent(void)
{
    FILE *f;
    char line[64];
    pid_t pid;

    f = fopen(SPM_AGENT_PID, "r");
    if (!f) {
        fprintf(stderr, "spm-agent: no PID file; agent not running?\n");
        return 1;
    }

    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        fprintf(stderr, "spm-agent: empty PID file\n");
        return 1;
    }
    fclose(f);

    pid = (pid_t)atoi(line);
    if (pid <= 0) {
        fprintf(stderr, "spm-agent: invalid PID in file\n");
        return 1;
    }

    if (kill(pid, 0) != 0) {
        fprintf(stderr, "spm-agent: process %ld not running (stale PID)\n",
                (long)pid);
        unlink(SPM_AGENT_PID);
        return 1;
    }

    printf("Sending SIGTERM to agent (pid %ld)...\n", (long)pid);
    if (kill(pid, SIGTERM) != 0) {
        perror("spm-agent: kill");
        return 1;
    }

    /* Wait briefly for it to die */
    {
        int i;
        for (i = 0; i < 10; i++) {
            usleep(500000);
            if (kill(pid, 0) != 0) {
                printf("Agent stopped.\n");
                unlink(SPM_AGENT_PID);
                return 0;
            }
        }
    }

    printf("Agent did not stop; sending SIGKILL...\n");
    kill(pid, SIGKILL);
    unlink(SPM_AGENT_PID);
    return 0;
}

/* Show agent status */
static int show_status(void)
{
    FILE *f;
    char line[256];
    struct stat sb;

    f = fopen(SPM_AGENT_PID, "r");
    if (!f) {
        printf("Agent: not running\n");
    } else {
        char pidline[64];
        pid_t pid = 0;
        if (fgets(pidline, sizeof(pidline), f))
            pid = (pid_t)atoi(pidline);
        fclose(f);

        if (pid > 0 && kill(pid, 0) == 0) {
            printf("Agent: running (pid %ld)\n", (long)pid);
        } else {
            printf("Agent: not running (stale PID file)\n");
        }
    }

    if (stat(SPM_UPDATE_STATUS, &sb) != 0) {
        printf("Status: no data (never checked)\n");
        return 0;
    }

    f = fopen(SPM_UPDATE_STATUS, "r");
    if (!f) {
        printf("Status: cannot read\n");
        return 0;
    }

    printf("Last check status:\n");
    while (fgets(line, sizeof(line), f)) {
        /* Parse key=value pairs */
        if (strncmp(line, "updates=", 8) == 0) {
            printf("  Updates available: %s", line + 8);
        } else if (strncmp(line, "checked=", 8) == 0) {
            time_t t = (time_t)atol(line + 8);
            if (t > 0) {
                char tbuf[64];
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S",
                         localtime(&t));
                printf("  Last checked: %s\n", tbuf);
            }
        } else if (strncmp(line, "total_available=", 16) == 0) {
            printf("  Total available: %s", line + 16);
        } else if (strncmp(line, "total_installed=", 16) == 0) {
            printf("  Total installed: %s", line + 16);
        }
    }
    fclose(f);

    return 0;
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(int argc, char *argv[])
{
    int i;
    int custom_interval = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            g_foreground = 1;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            custom_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-k") == 0) {
            return kill_agent();
        } else if (strcmp(argv[i], "-s") == 0) {
            config_defaults(&g_config);
            config_load(&g_config);
            return show_status();
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "-?") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "spm-agent: unknown option: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    /* Load config */
    config_defaults(&g_config);
    config_load(&g_config);
    config_init_dirs();

    if (custom_interval > 0)
        g_config.agent_interval = custom_interval;

    /* Check if already running */
    if (check_already_running()) {
        fprintf(stderr,
            "spm-agent: already running (see %s)\n",
            SPM_AGENT_PID);
        return 1;
    }

    /* Initialize SSL */
    if (g_config.ca_bundle[0])
        http_set_ca_bundle(g_config.ca_bundle);
    if (http_init() != 0) {
        fprintf(stderr, "spm-agent: warning: SSL init failed\n");
    }

    /* Install signal handlers */
    signal(SIGTERM, sig_term);
    signal(SIGINT, sig_term);
    signal(SIGHUP, sig_hup);
    signal(SIGUSR1, sig_usr1);
    signal(SIGPIPE, SIG_IGN);

    /* Daemonize unless foreground mode */
    if (!g_foreground) {
        if (daemonize() != 0) {
            fprintf(stderr, "spm-agent: daemonize failed\n");
            return 1;
        }
    }

    /* Open log file */
    g_logfp = fopen(SPM_AGENT_LOG, "a");
    if (!g_logfp && !g_foreground) {
        /* Can't log anywhere; just continue silently */
        g_logfp = NULL;
    }

    /* Write PID file */
    if (write_pid_file() != 0) {
        agent_log("failed to write PID file");
        /* Continue anyway */
    }

    /* Run the main loop */
    run_agent();

    /* Cleanup */
    remove_pid_file();
    unlink(SPM_UPDATE_STATUS);
    http_shutdown();

    if (g_logfp) {
        fclose(g_logfp);
    }

    return 0;
}

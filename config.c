/*
 * config.c - Configuration management for spm
 *
 * INI-style config parser for repos.conf:
 *
 *   [ssl]
 *   ca_bundle = /opt/sst/etc/ssl/cert.pem
 *
 *   [repo:firefly128]
 *   type = github
 *   name = firefly128 SPARC Apps
 *   owner = firefly128
 *   repos = pizzafool,sparccord,wesnoth-sparc
 *   enabled = yes
 *
 *   [repo:sunstorm]
 *   type = github
 *   name = Sunstorm Distribution
 *   owner = firefly128
 *   repos = sunstorm
 *   enabled = yes
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* Trim leading/trailing whitespace in-place */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}

/* Create a directory with parents (like mkdir -p) */
static int mkdirs(const char *path)
{
    char tmp[512];
    char *p;
    struct stat st;

    if (stat(path, &st) == 0) return 0; /* exists */

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(tmp, &st) != 0)
                mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

int config_init_dirs(void)
{
    mkdirs(SPM_ETC);
    mkdirs(SPM_VAR);
    mkdirs(SPM_CACHE);
    mkdirs(SPM_ROLLBACK);
    mkdirs(SPM_IDX_DIR);
    return 0;
}

void config_defaults(spm_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Use the package-owned trust store independently of host filesystem state. */
    strcpy(cfg->ca_bundle, SPM_CA_BUNDLE);

    /* Agent defaults */
    cfg->agent_interval = AGENT_DEFAULT_INTERVAL;
    cfg->agent_notify = 1;

    /* firefly128 GitHub releases */
    cfg->repos[0].type = REPO_TYPE_GITHUB;
    cfg->repos[0].enabled = 1;
    strcpy(cfg->repos[0].name, "firefly128");
    strcpy(cfg->repos[0].display_name, "firefly128 SPARC Apps");
    strcpy(cfg->repos[0].owner, "firefly128");
    strcpy(cfg->repos[0].repos[0], "pizzafool");
    strcpy(cfg->repos[0].repos[1], "sparccord");
    strcpy(cfg->repos[0].repos[2], "wesnoth-sparc");
    cfg->repos[0].repo_count = 3;

    /* Sunstorm distribution packages */
    cfg->repos[1].type = REPO_TYPE_GITHUB;
    cfg->repos[1].enabled = 1;
    strcpy(cfg->repos[1].name, "sunstorm");
    strcpy(cfg->repos[1].display_name, "Sunstorm Distribution");
    strcpy(cfg->repos[1].owner, "Sunstorm-Project");
    strcpy(cfg->repos[1].repos[0], "sunstorm");
    cfg->repos[1].repo_count = 1;

    cfg->repo_count = 2;
}

int config_load(spm_config_t *cfg)
{
    FILE *f;
    char line[1024];
    char section[128] = "";
    repo_def_t *cur_repo = NULL;

    config_defaults(cfg);

    f = fopen(SPM_CONF, "r");
    if (!f) {
        /* No config file; use defaults and create it */
        config_init_dirs();
        config_save(cfg);
        return 0;
    }

    /* Reset before parsing */
    memset(cfg, 0, sizeof(*cfg));

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        char *eq, *key, *val;

        /* Skip comments and empty lines */
        if (*s == '#' || *s == ';' || *s == '\0') continue;

        /* Section header */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                strncpy(section, s + 1, sizeof(section) - 1);

                /* Check if this is a repo section */
                if (strncmp(section, "repo:", 5) == 0) {
                    if (cfg->repo_count < MAX_REPOS) {
                        cur_repo = &cfg->repos[cfg->repo_count++];
                        memset(cur_repo, 0, sizeof(*cur_repo));
                        strncpy(cur_repo->name, section + 5,
                                sizeof(cur_repo->name) - 1);
                    } else {
                        cur_repo = NULL;
                    }
                } else {
                    cur_repo = NULL;
                }
            }
            continue;
        }

        /* Key = Value */
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);

        if (strcmp(section, "ssl") == 0) {
            if (strcmp(key, "ca_bundle") == 0) {
                strncpy(cfg->ca_bundle, val, sizeof(cfg->ca_bundle) - 1);
            }
        } else if (strcmp(section, "agent") == 0) {
            if (strcmp(key, "interval") == 0) {
                cfg->agent_interval = atoi(val);
                if (cfg->agent_interval < 60) cfg->agent_interval = 60;
            } else if (strcmp(key, "notify") == 0) {
                cfg->agent_notify = (strcmp(val, "yes") == 0 ||
                                     strcmp(val, "1") == 0);
            }
        } else if (cur_repo) {
            if (strcmp(key, "type") == 0) {
                if (strcmp(val, "tgcware") == 0)
                    cur_repo->type = REPO_TYPE_TGCWARE;
                else if (strcmp(val, "github") == 0)
                    cur_repo->type = REPO_TYPE_GITHUB;
            } else if (strcmp(key, "name") == 0) {
                strncpy(cur_repo->display_name, val,
                        sizeof(cur_repo->display_name) - 1);
            } else if (strcmp(key, "url") == 0) {
                strncpy(cur_repo->url, val, sizeof(cur_repo->url) - 1);
            } else if (strcmp(key, "source") == 0) {
                strncpy(cur_repo->source_url, val,
                        sizeof(cur_repo->source_url) - 1);
            } else if (strcmp(key, "owner") == 0) {
                strncpy(cur_repo->owner, val, sizeof(cur_repo->owner) - 1);
            } else if (strcmp(key, "repos") == 0) {
                /* Parse comma-separated repo names */
                char *tok = strtok(val, ",");
                cur_repo->repo_count = 0;
                while (tok && cur_repo->repo_count < MAX_GH_REPOS) {
                    char *t = trim(tok);
                    if (*t) {
                        strncpy(cur_repo->repos[cur_repo->repo_count],
                                t, 63);
                        cur_repo->repo_count++;
                    }
                    tok = strtok(NULL, ",");
                }
            } else if (strcmp(key, "enabled") == 0) {
                cur_repo->enabled = (strcmp(val, "yes") == 0 ||
                                     strcmp(val, "1") == 0);
            }
        }
    }

    fclose(f);

    return 0;
}

int config_save(const spm_config_t *cfg)
{
    FILE *f;
    int i, j;

    config_init_dirs();

    f = fopen(SPM_CONF, "w");
    if (!f) {
        fprintf(stderr, "spm: cannot write %s: %s\n",
                SPM_CONF, strerror(errno));
        return -1;
    }

    fprintf(f, "# spm repository configuration\n");
    fprintf(f, "# Generated by spm %s\n\n", SPM_VERSION);

    fprintf(f, "[ssl]\n");
    fprintf(f, "ca_bundle = %s\n\n", cfg->ca_bundle);

    fprintf(f, "[agent]\n");
    fprintf(f, "interval = %d\n", cfg->agent_interval);
    fprintf(f, "notify = %s\n\n", cfg->agent_notify ? "yes" : "no");

    for (i = 0; i < cfg->repo_count; i++) {
        const repo_def_t *r = &cfg->repos[i];

        fprintf(f, "[repo:%s]\n", r->name);
        fprintf(f, "type = %s\n",
                r->type == REPO_TYPE_TGCWARE ? "tgcware" : "github");
        fprintf(f, "name = %s\n", r->display_name);
        fprintf(f, "enabled = %s\n", r->enabled ? "yes" : "no");

        if (r->type == REPO_TYPE_TGCWARE) {
            fprintf(f, "url = %s\n", r->url);
            if (r->source_url[0])
                fprintf(f, "source = %s\n", r->source_url);
        } else if (r->type == REPO_TYPE_GITHUB) {
            fprintf(f, "owner = %s\n", r->owner);
            if (r->repo_count > 0) {
                fprintf(f, "repos = ");
                for (j = 0; j < r->repo_count; j++) {
                    if (j > 0) fprintf(f, ",");
                    fprintf(f, "%s", r->repos[j]);
                }
                fprintf(f, "\n");
            }
        }
        fprintf(f, "\n");
    }

    fclose(f);
    return 0;
}

repo_def_t *config_find_repo(spm_config_t *cfg, const char *name)
{
    int i;
    for (i = 0; i < cfg->repo_count; i++) {
        if (strcmp(cfg->repos[i].name, name) == 0)
            return &cfg->repos[i];
    }
    return NULL;
}

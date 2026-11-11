/*
 * config.h - Configuration management for spm
 *
 * Reads/writes the master repo config from /opt/sst/etc/repos.conf
 * and manages SSL settings, repository definitions, and paths.
 */

#ifndef SPM_CONFIG_H
#define SPM_CONFIG_H

#define SPM_VERSION   "0.1.0"
#define SPM_BASE      "/opt/sst"
#define SPM_ETC       SPM_BASE "/etc"
#define SPM_VAR       SPM_BASE "/var"
#define SPM_CACHE     SPM_VAR  "/cache"
#define SPM_ROLLBACK  SPM_VAR  "/rollback"
#define SPM_CONF      SPM_ETC  "/repos.conf"
#define SPM_INSTALLED SPM_VAR  "/installed.db"
#define SPM_IDX_DIR   SPM_VAR  "/index"
#define SPM_AGENT_PID SPM_VAR  "/agent.pid"
#define SPM_AGENT_LOG SPM_VAR  "/agent.log"
#define SPM_UPDATE_STATUS SPM_VAR "/update.status"

#define SPM_CA_BUNDLE SPM_BASE "/etc/ssl/cert.pem"

#define REPO_TYPE_TGCWARE   1
#define REPO_TYPE_GITHUB    2

#define MAX_REPOS           16
#define MAX_GH_REPOS        32

/* Repository definition */
typedef struct {
    char name[64];            /* Short ID: "sunstorm", "firefly128" */
    char display_name[128];   /* Human-readable repository name */
    int  type;                /* REPO_TYPE_TGCWARE or REPO_TYPE_GITHUB */
    int  enabled;             /* 1 = enabled, 0 = disabled */

    /* TGCware specific */
    char url[512];            /* Base URL for package index */
    char source_url[256];     /* GitHub URL for TGCware source */

    /* GitHub specific */
    char owner[64];           /* GitHub username: "firefly128" */
    char repos[MAX_GH_REPOS][64]; /* List of repo names */
    int  repo_count;
} repo_def_t;

/* Agent settings */
#define AGENT_DEFAULT_INTERVAL  (6 * 3600)   /* 6 hours */

/* Global config */
typedef struct {
    char ca_bundle[512];      /* Path to CA cert bundle for SSL */

    int  agent_interval;      /* Update check interval in seconds */
    int  agent_notify;        /* 1 = show CDE notification on updates */

    repo_def_t repos[MAX_REPOS];
    int repo_count;
} spm_config_t;

/*
 * Load config from SPM_CONF.
 * Returns 0 on success, -1 on error (creates defaults if missing).
 */
int config_load(spm_config_t *cfg);

/*
 * Save config to SPM_CONF.
 * Returns 0 on success, -1 on error.
 */
int config_save(const spm_config_t *cfg);

/*
 * Initialize default config (TGCware + firefly128 repos).
 */
void config_defaults(spm_config_t *cfg);

/*
 * Create required directories under SPM_BASE.
 */
int config_init_dirs(void);

/*
 * Find a repo by name.
 * Returns pointer to the repo, or NULL.
 */
repo_def_t *config_find_repo(spm_config_t *cfg, const char *name);

#endif /* SPM_CONFIG_H */

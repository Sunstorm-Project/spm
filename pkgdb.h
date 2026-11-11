/*
 * pkgdb.h - Package database, rollback, and dependency tracking
 *
 * Manages the local database of available and installed packages,
 * provides pkgadd/pkgrm wrappers, rollback support, and dependency
 * resolution for Solaris SVR4 packages.
 */

#ifndef SPM_PKGDB_H
#define SPM_PKGDB_H

#include "config.h"
#include "html.h"
#include "json.h"

/* ================================================================
 * AVAILABLE PACKAGE (unified across repos)
 * ================================================================ */

#define SRC_TGCWARE  1
#define SRC_GITHUB   2

typedef struct {
    char name[128];         /* Human name: "curl", "pizzafool" */
    char version[64];       /* "8.18.0", "1.0" */
    int  release;           /* Release number (TGCware) or 0 */
    char arch[32];          /* "sparc", "sparcv8", "sparc64" */
    char pkg_code[64];      /* SVR4 pkg code: "TGCcurl", "JWpzfool" */
    char description[256];
    char download_url[512]; /* Full download URL */
    char filename[320];     /* Filename of the package file */
    char md5[80];           /* Legacy field name: MD5 or sha256:<64 hex> */
    char deps[512];         /* Comma-sep dep codes (TGCware) */
    char size_str[16];      /* "720K" */
    char repo_name[64];     /* "tgcware" or "firefly128" */
    int  source_type;       /* SRC_TGCWARE or SRC_GITHUB */
    char source_url[256];   /* Link to source on GitHub */
    char source_repo[128];  /* GitHub source repo (for TGCware correlation) */
} avail_pkg_t;

/* ================================================================
 * INSTALLED PACKAGE
 * ================================================================ */

typedef struct {
    char name[128];
    char version[64];
    char pkg_code[64];
    char repo_name[64];
    char install_date[32];
    char pkg_file[256];     /* Cached .pkg path for rollback */
    int  source_type;
} installed_pkg_t;

/* ================================================================
 * PACKAGE DATABASE
 * ================================================================ */

typedef struct {
    avail_pkg_t   *available;
    int            avail_count;
    int            avail_alloc;

    installed_pkg_t *installed;
    int              inst_count;
    int              inst_alloc;
} pkgdb_t;

/*
 * Create a new empty package database.
 */
pkgdb_t *pkgdb_new(void);

/*
 * Free the package database.
 */
void pkgdb_free(pkgdb_t *db);

/*
 * Add available packages from a TGCware index.
 * base_url: the base URL of the TGCware repo (for download URLs).
 * source_base: GitHub URL for TGCware source correlation.
 */
void pkgdb_add_tgcware(pkgdb_t *db, const tgc_index_t *idx,
                        const char *base_url, const char *source_base,
                        const char *repo_name);

/*
 * Add available packages from GitHub releases JSON.
 * json_body: raw JSON string from GitHub API.
 * repo_name: "firefly128" (the spm repo name).
 * gh_repo: "pizzafool" (the GitHub repository name).
 * owner: "firefly128" (the GitHub username).
 */
void pkgdb_add_github(pkgdb_t *db, const char *json_body,
                       const char *repo_name, const char *gh_repo,
                       const char *owner);

/*
 * Save available package index to disk.
 */
int pkgdb_save_index(const pkgdb_t *db);

/*
 * Load available package index from disk.
 */
int pkgdb_load_index(pkgdb_t *db);

/*
 * Save/load installed package list.
 */
int pkgdb_save_installed(const pkgdb_t *db);
int pkgdb_load_installed(pkgdb_t *db);

/*
 * Search available packages by name/description.
 * Returns count of matches. Writes indices into results[].
 */
int pkgdb_search(const pkgdb_t *db, const char *term,
                 int *results, int max_results);

/*
 * Find an available package by name (latest version).
 * Returns index or -1.
 */
int pkgdb_find_avail(const pkgdb_t *db, const char *name);

/*
 * Find an installed package by name.
 * Returns index or -1.
 */
int pkgdb_find_installed(const pkgdb_t *db, const char *name);

/*
 * Find available package by SVR4 package code.
 * Returns index or -1.
 */
int pkgdb_find_by_code(const pkgdb_t *db, const char *code);

/*
 * Install a package. Downloads, runs pkgadd, records in DB.
 * Returns 0 on success, -1 on error.
 */
int pkgdb_install(pkgdb_t *db, int avail_idx);

/*
 * Remove a package. Runs pkgrm, removes from DB.
 * Returns 0 on success, -1 on error.
 */
int pkgdb_remove(pkgdb_t *db, int inst_idx);

/*
 * Rollback an installed package to the previous version.
 * Requires recorded previous-version history and a verified cached artifact.
 * Returns 0 on success, -1 on error.
 */
int pkgdb_rollback(pkgdb_t *db, int inst_idx);

/*
 * Resolve dependencies for a package.
 * Returns a list of available package indices that need installing.
 * The result is malloc'd; caller frees. *count is set.
 * Returns NULL on error or if all deps are satisfied.
 */
int *pkgdb_resolve_deps(pkgdb_t *db, int avail_idx, int *count);

/*
 * Print dependency tree for a package (to stdout).
 */
void pkgdb_print_deps(const pkgdb_t *db, int avail_idx, int depth);

/*
 * Record an installation in the database.
 */
void pkgdb_record_install(pkgdb_t *db, const avail_pkg_t *pkg,
                           const char *pkg_file);

/*
 * Check if a system has a package installed via pkginfo.
 * Returns 1 if installed, 0 if not.
 */
int pkgdb_sys_installed(const char *pkg_code);

/*
 * Load packages from local SVR4 spool directory (/var/spool/pkg).
 * Adds them to the available package list with repo "local-spool".
 */
int pkgdb_load_spool(pkgdb_t *db, const char *spool_dir);

/*
 * Check if an available package is installed on the system.
 * Matches by pkg_code against the installed list.
 * Returns 1 if installed, 0 if not.
 */
int pkgdb_avail_is_installed(const pkgdb_t *db, int avail_idx);

/*
 * Check if the given avail_idx is the latest version of its package name.
 * Returns 1 if this is the entry pkgdb_find_avail would return, 0 otherwise.
 * Used to de-duplicate the list when multiple versions exist.
 */
int pkgdb_is_latest_version(const pkgdb_t *db, int avail_idx);

/*
 * Find all available versions of a package by name.
 * Writes indices (newest first) into results[]. Returns count.
 */
int pkgdb_find_all_versions(const pkgdb_t *db, const char *name,
                            int *results, int max_results);

/*
 * Check if ANY version of a named package is installed.
 * Returns 1 if at least one version is installed, 0 if not.
 */
int pkgdb_any_version_installed(const pkgdb_t *db, const char *name);

#endif /* SPM_PKGDB_H */

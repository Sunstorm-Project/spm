/*
 * pkgdb.c - Package database, rollback, and dependency tracking
 *
 * Manages available/installed package data, wraps pkgadd/pkgrm,
 * resolves dependencies using TGCware's embedded dep codes, and
 * provides rollback by caching previous package files.
 */

#include "pkgdb.h"
#include "http.h"
#include "integrity.h"
#include "process.h"
#include <fcntl.h>
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

/* ================================================================
 * DATABASE MANAGEMENT
 * ================================================================ */

pkgdb_t *pkgdb_new(void)
{
    return (pkgdb_t *)calloc(1, sizeof(pkgdb_t));
}

void pkgdb_free(pkgdb_t *db)
{
    if (!db) return;
    if (db->available) free(db->available);
    if (db->installed) free(db->installed);
    free(db);
}

static void add_avail(pkgdb_t *db, const avail_pkg_t *pkg)
{
    if (db->avail_count >= db->avail_alloc) {
        db->avail_alloc = db->avail_alloc ? db->avail_alloc * 2 : 128;
        db->available = (avail_pkg_t *)realloc(db->available,
                         db->avail_alloc * sizeof(avail_pkg_t));
    }
    db->available[db->avail_count++] = *pkg;
}

static void add_installed(pkgdb_t *db, const installed_pkg_t *pkg)
{
    if (db->inst_count >= db->inst_alloc) {
        db->inst_alloc = db->inst_alloc ? db->inst_alloc * 2 : 32;
        db->installed = (installed_pkg_t *)realloc(db->installed,
                         db->inst_alloc * sizeof(installed_pkg_t));
    }
    db->installed[db->inst_count++] = *pkg;
}

/* ================================================================
 * TGCWARE INDEX IMPORT
 * ================================================================ */

void pkgdb_add_tgcware(pkgdb_t *db, const tgc_index_t *idx,
                        const char *base_url, const char *source_base,
                        const char *repo_name)
{
    int i;
    for (i = 0; i < idx->count; i++) {
        avail_pkg_t pkg;
        const tgc_entry_t *e = &idx->entries[i];

        memset(&pkg, 0, sizeof(pkg));
        /* Differentiate TGCX (extended/64-bit) from TGC (standard) */
        if (strncmp(e->pkg_code, "TGCX", 4) == 0) {
            snprintf(pkg.name, sizeof(pkg.name), "%s (64-bit)", e->name);
        } else {
            strncpy(pkg.name, e->name, sizeof(pkg.name) - 1);
        }
        strncpy(pkg.version, e->version, sizeof(pkg.version) - 1);
        pkg.release = e->release;
        strncpy(pkg.arch, e->arch, sizeof(pkg.arch) - 1);
        strncpy(pkg.pkg_code, e->pkg_code, sizeof(pkg.pkg_code) - 1);
        strncpy(pkg.description, e->description, sizeof(pkg.description) - 1);
        strncpy(pkg.filename, e->filename, sizeof(pkg.filename) - 1);
        strncpy(pkg.md5, e->md5, sizeof(pkg.md5) - 1);
        strncpy(pkg.deps, e->deps, sizeof(pkg.deps) - 1);
        strncpy(pkg.size_str, e->size_str, sizeof(pkg.size_str) - 1);
        strncpy(pkg.repo_name, repo_name, sizeof(pkg.repo_name) - 1);
        pkg.source_type = SRC_TGCWARE;

        /* Build download URL */
        snprintf(pkg.download_url, sizeof(pkg.download_url),
                 "%s%s", base_url, e->filename);

        /* Build source URL (correlate TGCware binary to GitHub source) */
        if (source_base && source_base[0]) {
            snprintf(pkg.source_url, sizeof(pkg.source_url),
                     "%s/tree/master/%s", source_base, e->name);
            snprintf(pkg.source_repo, sizeof(pkg.source_repo),
                     "%s", e->name);
        }

        add_avail(db, &pkg);
    }
}

/* ================================================================
 * GITHUB RELEASES IMPORT
 * ================================================================ */

void pkgdb_add_github(pkgdb_t *db, const char *json_body,
                       const char *repo_name, const char *gh_repo,
                       const char *owner)
{
    json_value_t *root;
    int i, j, nrel;

    root = json_parse(json_body);
    if (!root) return;

    /* GitHub API returns an array of releases */
    if (root->type == JSON_ARRAY) {
        nrel = json_array_len(root);
    } else {
        /* Single release object - wrap it */
        nrel = 1;
    }

    for (i = 0; i < nrel; i++) {
        json_value_t *rel;
        const char *tag, *rel_name, *body;
        json_value_t *assets;

        if (root->type == JSON_ARRAY) {
            rel = json_array_get(root, i);
        } else {
            rel = root;
        }
        if (!rel) continue;

        tag = json_get_str(rel, "tag_name");
        rel_name = json_get_str(rel, "name");
        body = json_get_str(rel, "body");

        assets = json_get(rel, "assets");
        if (!assets) continue;

        for (j = 0; j < json_array_len(assets); j++) {
            json_value_t *asset = json_array_get(assets, j);
            const char *aname, *dl_url, *digest;
            int asize;
            avail_pkg_t pkg;

            if (!asset) continue;

            aname = json_get_str(asset, "name");
            dl_url = json_get_str(asset, "browser_download_url");
            asize = json_get_int(asset, "size");
            digest = json_get_str(asset, "digest");

            if (!aname || !dl_url) continue;

            /* Only index .pkg files (SVR4 packages for Solaris) */
            if (!strstr(aname, ".pkg"))
                continue;

            memset(&pkg, 0, sizeof(pkg));
            strncpy(pkg.filename, aname, sizeof(pkg.filename) - 1);
            strncpy(pkg.download_url, dl_url, sizeof(pkg.download_url) - 1);
            strncpy(pkg.repo_name, repo_name, sizeof(pkg.repo_name) - 1);
            pkg.source_type = SRC_GITHUB;

            /* Extract version from filename if possible, else from tag.
             * Filename patterns:
             *   SSTgcc-11.4.0-sparc.pkg.Z
             *   SSTgcc-11.4.0-1.sst-sunos5.7-sparc.pkg.Z
             * Version is the segment after the first '-' that starts
             * with a digit, up to the next '-' that doesn't look like
             * a continuation (i.e. doesn't start with a digit). */
            {
                const char *dash = strchr(aname, '-');
                int got_ver = 0;
                if (dash && dash[1] >= '0' && dash[1] <= '9') {
                    const char *vs = dash + 1;
                    const char *ve = vs;
                    /* Walk forward: keep "11.4.0" or "11.4.0-1" */
                    while (*ve && *ve != '.' &&
                           !(ve > vs && *ve == '-' &&
                             (ve[1] < '0' || ve[1] > '9'))) {
                        ve++;
                        /* Consume ".digit" sequences */
                        while (*ve == '.' &&
                               ve[1] >= '0' && ve[1] <= '9') {
                            ve++;
                            while (*ve >= '0' && *ve <= '9') ve++;
                        }
                        /* Consume "-digit" (sub-release like -1) */
                        if (*ve == '-' && ve[1] >= '0' && ve[1] <= '9') {
                            ve++;
                            while (*ve >= '0' && *ve <= '9') ve++;
                        }
                    }
                    if (ve > vs) {
                        int vlen = ve - vs;
                        if (vlen >= (int)sizeof(pkg.version))
                            vlen = sizeof(pkg.version) - 1;
                        memcpy(pkg.version, vs, vlen);
                        pkg.version[vlen] = '\0';
                        got_ver = 1;
                    }
                }
                if (!got_ver && tag) {
                    const char *v = tag;
                    if (*v == 'v' || *v == 'V') v++;
                    strncpy(pkg.version, v, sizeof(pkg.version) - 1);
                }
            }

            /* Description: prefer human-readable name from filename
             * (e.g., "SSTgcc" → "gcc 11.4.0") over shared release name */
            if (rel_name) {
                strncpy(pkg.description, rel_name,
                        sizeof(pkg.description) - 1);
            }

            /* Arch: assume SPARC for these repos */
            strcpy(pkg.arch, "sparc");

            /* Size string */
            if (asize > 0) {
                if (asize >= 1048576)
                    snprintf(pkg.size_str, sizeof(pkg.size_str),
                             "%.1fM", asize / 1048576.0);
                else
                    snprintf(pkg.size_str, sizeof(pkg.size_str),
                             "%dK", asize / 1024);
            }

            /* Digest */
            if (digest) {
                strncpy(pkg.md5, digest, sizeof(pkg.md5) - 1);
            }

            /* Source URL */
            snprintf(pkg.source_url, sizeof(pkg.source_url),
                     "https://github.com/%s/%s", owner, gh_repo);
            snprintf(pkg.source_repo, sizeof(pkg.source_repo),
                     "%s/%s", owner, gh_repo);

            /* Try to extract pkg_code from filename pattern  */
            /* Sunstorm: SSTgcc-11.4.0-1.sst-sunos5.7-sparc.pkg.Z
             * The pkg_code IS the part before the first '-' (e.g. SSTgcc).
             * Other GitHub: name-version.pkg → prepend "JW" */
            {
                char *dash = strchr(aname, '-');
                if (dash) {
                    int nlen = dash - aname;
                    char pcode[64];
                    if (nlen > 0 && nlen < (int)sizeof(pcode) - 1) {
                        if (nlen >= 3 && aname[0] == 'S' &&
                            aname[1] == 'S' && aname[2] == 'T') {
                            /* SST prefix: pkg_code is already in filename */
                            snprintf(pcode, sizeof(pcode), "%.*s",
                                     nlen, aname);
                            /* Human name: strip SST prefix (SSTgcc → gcc) */
                            snprintf(pkg.name, sizeof(pkg.name),
                                     "%.*s", nlen - 3, aname + 3);
                        } else {
                            snprintf(pcode, sizeof(pcode), "JW%.*s",
                                     nlen, aname);
                            /* Human name: part before first dash */
                            snprintf(pkg.name, sizeof(pkg.name),
                                     "%.*s", nlen, aname);
                        }
                        strncpy(pkg.pkg_code, pcode,
                                sizeof(pkg.pkg_code) - 1);
                    }
                }
                /* No-dash SST filename (e.g., SSTprngd.pkg.Z) */
                if (!dash && aname[0] == 'S' && aname[1] == 'S' &&
                    aname[2] == 'T') {
                    const char *dot = strchr(aname + 3, '.');
                    if (dot) {
                        int nlen = dot - aname;
                        char pcode[64];
                        snprintf(pcode, sizeof(pcode), "%.*s",
                                 nlen, aname);
                        strncpy(pkg.pkg_code, pcode,
                                sizeof(pkg.pkg_code) - 1);
                        snprintf(pkg.name, sizeof(pkg.name),
                                 "%.*s", nlen - 3, aname + 3);
                    }
                }
                /* Fallback: if still no name, use repo name */
                if (!pkg.name[0])
                    strncpy(pkg.name, gh_repo, sizeof(pkg.name) - 1);
            }

            /* The canonical SST suffix delimits the complete upstream+REV
             * identity, including prerelease hyphens and letter versions. */
            if (strncmp(pkg.pkg_code, "SST", 3) == 0) {
                const char *end = strstr(aname, ".sst-sunos5.7-sparc.pkg");
                const char *start = aname + strlen(pkg.pkg_code);
                if (end && *start == '-' && end > start + 1) {
                    size_t length = (size_t)(end - ++start);
                    if (length < sizeof(pkg.version)) {
                        memcpy(pkg.version, start, length);
                        pkg.version[length] = '\0';
                    }
                }
            }

            /* Per-package description when name was extracted from
             * filename (avoids all sunstorm assets sharing one desc) */
            if (pkg.name[0] && pkg.version[0]) {
                snprintf(pkg.description, sizeof(pkg.description),
                         "%s %s", pkg.name, pkg.version);
            }

            add_avail(db, &pkg);
        }
    }

    json_free(root);
}

/* ================================================================
 * INDEX PERSISTENCE (tab-separated flat file)
 * ================================================================ */

int pkgdb_save_index(const pkgdb_t *db)
{
    FILE *f;
    int i;
    char path[512];

    snprintf(path, sizeof(path), "%s/available.idx", SPM_IDX_DIR);
    f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# spm available package index\n");
    fprintf(f, "# name|version|release|arch|pkg_code|desc|download|"
               "filename|md5|deps|size|repo|src_type|src_url|src_repo\n");

    for (i = 0; i < db->avail_count; i++) {
        const avail_pkg_t *p = &db->available[i];
        fprintf(f, "%s|%s|%d|%s|%s|%s|%s|%s|%s|%s|%s|%s|%d|%s|%s\n",
                p->name, p->version, p->release, p->arch,
                p->pkg_code, p->description, p->download_url,
                p->filename, p->md5, p->deps, p->size_str,
                p->repo_name, p->source_type, p->source_url,
                p->source_repo);
    }

    fclose(f);
    return 0;
}

static void load_avail_line(pkgdb_t *db, char *line)
{
    avail_pkg_t pkg;
    char *fields[15];
    int nf = 0;
    char *tok;

    memset(&pkg, 0, sizeof(pkg));
    memset(fields, 0, sizeof(fields));

    tok = line;
    while (tok && nf < 15) {
        fields[nf++] = tok;
        tok = strchr(tok, '|');
        if (tok) *tok++ = '\0';
    }

    if (nf < 12) return;

    strncpy(pkg.name, fields[0], sizeof(pkg.name) - 1);
    strncpy(pkg.version, fields[1], sizeof(pkg.version) - 1);
    pkg.release = atoi(fields[2]);
    strncpy(pkg.arch, fields[3], sizeof(pkg.arch) - 1);
    strncpy(pkg.pkg_code, fields[4], sizeof(pkg.pkg_code) - 1);
    strncpy(pkg.description, fields[5], sizeof(pkg.description) - 1);
    strncpy(pkg.download_url, fields[6], sizeof(pkg.download_url) - 1);
    strncpy(pkg.filename, fields[7], sizeof(pkg.filename) - 1);
    strncpy(pkg.md5, fields[8], sizeof(pkg.md5) - 1);
    strncpy(pkg.deps, fields[9], sizeof(pkg.deps) - 1);
    strncpy(pkg.size_str, fields[10], sizeof(pkg.size_str) - 1);
    strncpy(pkg.repo_name, fields[11], sizeof(pkg.repo_name) - 1);
    if (nf > 12) pkg.source_type = atoi(fields[12]);
    if (nf > 13) strncpy(pkg.source_url, fields[13], sizeof(pkg.source_url) - 1);
    if (nf > 14) strncpy(pkg.source_repo, fields[14], sizeof(pkg.source_repo) - 1);

    add_avail(db, &pkg);
}

int pkgdb_load_index(pkgdb_t *db)
{
    FILE *f;
    char line[4096];
    char path[512];

    snprintf(path, sizeof(path), "%s/available.idx", SPM_IDX_DIR);
    f = fopen(path, "r");
    if (!f) return -1;

    while (fgets(line, sizeof(line), f)) {
        /* Trim newline */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (line[0] == '#' || line[0] == '\0') continue;
        load_avail_line(db, line);
    }

    fclose(f);
    return 0;
}

/* Match native SST VERSION metadata to the revision-bearing release filename.
 * Keep the complete upstream version; REV defaults to the writer's value 1.
 * Other namespaces retain their existing version conventions. */
static void normalize_sst_pkginfo_version(char *version, size_t size)
{
    char input[128], normalized[128];
    char *upstream, *field, *next, *revision = NULL;
    size_t length;
    int written;
    if (strlen(version) >= sizeof(input)) return;
    strcpy(input, version);
    upstream = input;
    length = strlen(upstream);
    if (length >= 2 && (upstream[0] == '\"' || upstream[0] == '\'')) {
        if (upstream[length - 1] != upstream[0]) return;
        upstream[length - 1] = '\0';
        upstream++;
    }
    field = strchr(upstream, ',');
    if (field) *field++ = '\0';
    while (field) {
        next = strchr(field, ',');
        if (next) *next++ = '\0';
        if (strncmp(field, "REV=", 4) == 0) {
            if (revision) return;
            revision = field + 4;
        }
        field = next;
    }
    if (!revision) revision = "1";
    if (!spm_safe_component(upstream) || !spm_safe_component(revision)) return;
    written = snprintf(normalized, sizeof(normalized), "%s-%s", upstream, revision);
    if (written < 0 || written >= (int)sizeof(normalized) || written >= (int)size)
        return;
    strcpy(version, normalized);
}

int pkgdb_save_installed(const pkgdb_t *db)
{
    FILE *f;
    char temporary[512];
    int i, fd, error = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp-XXXXXX", SPM_INSTALLED) >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); return -1; }
    if (fprintf(f, "# spm installed packages\n") < 0) error = 1;
    for (i = 0; i < db->inst_count; i++) {
        const installed_pkg_t *p = &db->installed[i];
        if (fprintf(f, "%s|%s|%s|%s|%s|%s|%d\n",
                    p->name, p->version, p->pkg_code, p->repo_name,
                    p->install_date, p->pkg_file, p->source_type) < 0) error = 1;
    }
    if (fflush(f) != 0 || fsync(fd) != 0) error = 1;
    if (fclose(f) != 0) error = 1;
    if (error || rename(temporary, SPM_INSTALLED) != 0) { unlink(temporary); return -1; }
    return 0;
}

int pkgdb_load_installed(pkgdb_t *db)
{
    /*
     * Read the SVR4 package database directly from /var/sadm/pkg/.
     * Each installed package has a directory /var/sadm/pkg/<CODE>/
     * containing a "pkginfo" file with KEY=value pairs:
     *   PKG=TGCbash
     *   NAME=bash - GNU Bourne Again SHell
     *   VERSION=4.4.23,REV=1
     *   ARCH=sparc
     *   CATEGORY=application
     *   DESC=An sh-compatible shell
     *   INSTDATE=Sep 23 2021 22:31
     *   VENDOR=...
     *   BASEDIR=/usr/tgcware
     *
     * This is the same data that /usr/bin/pkginfo reads, but we
     * avoid forking a process for every package on a 110MHz SPARC.
     */
    DIR *d;
    struct dirent *de;
    char path[512];
    char line[1024];
    FILE *fp;

    /* Clear existing installed list */
    db->inst_count = 0;

    d = opendir("/var/sadm/pkg");
    if (!d) return -1;

    while ((de = readdir(d)) != NULL) {
        installed_pkg_t pkg;
        struct stat st;

        /* Skip . and .. */
        if (de->d_name[0] == '.') continue;

        /* Each directory has a pkginfo file */
        snprintf(path, sizeof(path),
                 "/var/sadm/pkg/%s/pkginfo", de->d_name);
        if (stat(path, &st) != 0) continue;

        memset(&pkg, 0, sizeof(pkg));

        /* Default: use directory name as pkg_code and name */
        strncpy(pkg.pkg_code, de->d_name, sizeof(pkg.pkg_code) - 1);
        strncpy(pkg.name, de->d_name, sizeof(pkg.name) - 1);

        /* Determine source from code prefix */
        if (strncmp(de->d_name, "TGC", 3) == 0) {
            strncpy(pkg.repo_name, "tgcware", sizeof(pkg.repo_name) - 1);
            pkg.source_type = SRC_TGCWARE;
        } else if (strncmp(de->d_name, "JW", 2) == 0) {
            strncpy(pkg.repo_name, "local", sizeof(pkg.repo_name) - 1);
            pkg.source_type = SRC_GITHUB;
        } else {
            strncpy(pkg.repo_name, "system", sizeof(pkg.repo_name) - 1);
            pkg.source_type = 0;
        }

        /* Parse the pkginfo file for all metadata */
        fp = fopen(path, "r");
        if (fp) {
            while (fgets(line, sizeof(line), fp)) {
                char *val;
                int len = strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                    line[--len] = '\0';

                if (strncmp(line, "PKG=", 4) == 0) {
                    val = line + 4;
                    strncpy(pkg.pkg_code, val, sizeof(pkg.pkg_code) - 1);
                } else if (strncmp(line, "NAME=", 5) == 0) {
                    val = line + 5;
                    /* For TGCware: NAME is "shortname - description".
                     * Use just the short name as the display name. */
                    {
                        char *dash = strstr(val, " - ");
                        if (dash && strncmp(pkg.pkg_code, "TGC", 3) == 0) {
                            int nlen = dash - val;
                            if (nlen > 0 && nlen < (int)sizeof(pkg.name)) {
                                memcpy(pkg.name, val, nlen);
                                pkg.name[nlen] = '\0';
                            }
                        } else {
                            strncpy(pkg.name, val, sizeof(pkg.name) - 1);
                        }
                    }
                } else if (strncmp(line, "VERSION=", 8) == 0) {
                    val = line + 8;
                    strncpy(pkg.version, val, sizeof(pkg.version) - 1);
                } else if (strncmp(line, "INSTDATE=", 9) == 0) {
                    val = line + 9;
                    strncpy(pkg.install_date, val,
                            sizeof(pkg.install_date) - 1);
                }
            }
            fclose(fp);
        }

        if (strncmp(pkg.pkg_code, "SST", 3) == 0)
            normalize_sst_pkginfo_version(pkg.version, sizeof(pkg.version));
        add_installed(db, &pkg);
    }

    closedir(d);
    return 0;
}

/* ================================================================
 * SEARCH AND LOOKUP
 * ================================================================ */

/* Case-insensitive substring match */
static int ci_match(const char *hay, const char *needle)
{
    int nlen, hlen, i;
    if (!hay || !needle) return 0;
    nlen = strlen(needle);
    hlen = strlen(hay);
    if (nlen > hlen) return 0;

    for (i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(hay + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

int pkgdb_search(const pkgdb_t *db, const char *term,
                 int *results, int max_results)
{
    int i, count = 0;
    for (i = 0; i < db->avail_count && count < max_results; i++) {
        if (ci_match(db->available[i].name, term) ||
            ci_match(db->available[i].description, term) ||
            ci_match(db->available[i].pkg_code, term)) {
            results[count++] = i;
        }
    }
    return count;
}

/* Version comparison */
static int ver_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            long na = strtol(a, (char **)&a, 10);
            long nb = strtol(b, (char **)&b, 10);
            if (na != nb) return (na > nb) ? 1 : -1;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++;
            b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

int pkgdb_find_avail(const pkgdb_t *db, const char *name)
{
    int i, best = -1;
    for (i = 0; i < db->avail_count; i++) {
        if (strcasecmp(db->available[i].name, name) == 0) {
            if (best < 0 || ver_cmp(db->available[i].version,
                                     db->available[best].version) > 0) {
                best = i;
            }
        }
    }
    return best;
}

int pkgdb_find_installed(const pkgdb_t *db, const char *name)
{
    int i;
    for (i = 0; i < db->inst_count; i++) {
        if (strcasecmp(db->installed[i].name, name) == 0)
            return i;
        /* Also match by pkg_code since installed list uses codes */
        if (strcasecmp(db->installed[i].pkg_code, name) == 0)
            return i;
    }
    return -1;
}

int pkgdb_find_by_code(const pkgdb_t *db, const char *code)
{
    int i, best = -1;
    for (i = 0; i < db->avail_count; i++) {
        if (strcmp(db->available[i].pkg_code, code) == 0) {
            if (best < 0 || ver_cmp(db->available[i].version,
                                     db->available[best].version) > 0) {
                best = i;
            }
        }
    }
    return best;
}

/* ================================================================
 * SYSTEM PACKAGE QUERIES
 * ================================================================ */

int pkgdb_sys_installed(const char *pkg_code)
{
    char path[256];
    struct stat st;
    snprintf(path, sizeof(path), "/var/sadm/pkg/%s/pkginfo", pkg_code);
    return (stat(path, &st) == 0) ? 1 : 0;
}

/* ================================================================
 * SPOOL DIRECTORY SCANNING (/var/spool/pkg)
 * ================================================================ */

int pkgdb_load_spool(pkgdb_t *db, const char *spool_dir)
{
    DIR *d;
    struct dirent *de;
    char cmd[512];
    char line[1024];
    FILE *fp;

    d = opendir(spool_dir);
    if (!d) return -1;

    while ((de = readdir(d)) != NULL) {
        avail_pkg_t pkg;
        char pkginfo_path[512];
        struct stat st;

        /* Skip . and .. */
        if (de->d_name[0] == '.') continue;

        /* Each directory under /var/spool/pkg is a spooled package */
        snprintf(pkginfo_path, sizeof(pkginfo_path),
                 "%s/%s/pkginfo", spool_dir, de->d_name);
        if (stat(pkginfo_path, &st) != 0) continue;

        memset(&pkg, 0, sizeof(pkg));
        strncpy(pkg.pkg_code, de->d_name, sizeof(pkg.pkg_code) - 1);
        strncpy(pkg.name, de->d_name, sizeof(pkg.name) - 1);
        strncpy(pkg.repo_name, "local-spool", sizeof(pkg.repo_name) - 1);
        strncpy(pkg.arch, "sparc", sizeof(pkg.arch) - 1);
        pkg.source_type = 0;

        /* Build install path */
        snprintf(pkg.download_url, sizeof(pkg.download_url),
                 "%s/%s", spool_dir, de->d_name);

        /* Parse pkginfo file for metadata */
        fp = fopen(pkginfo_path, "r");
        if (fp) {
            while (fgets(line, sizeof(line), fp)) {
                char *val;
                int len = strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                    line[--len] = '\0';

                if (strncmp(line, "NAME=", 5) == 0) {
                    val = line + 5;
                    strncpy(pkg.description, val, sizeof(pkg.description) - 1);
                } else if (strncmp(line, "VERSION=", 8) == 0) {
                    val = line + 8;
                    strncpy(pkg.version, val, sizeof(pkg.version) - 1);
                } else if (strncmp(line, "ARCH=", 5) == 0) {
                    val = line + 5;
                    strncpy(pkg.arch, val, sizeof(pkg.arch) - 1);
                } else if (strncmp(line, "PKG=", 4) == 0) {
                    val = line + 4;
                    strncpy(pkg.pkg_code, val, sizeof(pkg.pkg_code) - 1);
                    strncpy(pkg.name, val, sizeof(pkg.name) - 1);
                }
            }
            fclose(fp);
        }

        if (strncmp(pkg.pkg_code, "SST", 3) == 0)
            normalize_sst_pkginfo_version(pkg.version, sizeof(pkg.version));

        /* Don't add if already in available index */
        if (pkgdb_find_by_code(db, pkg.pkg_code) < 0) {
            add_avail(db, &pkg);
        }
    }

    closedir(d);
    return 0;
}

/* ================================================================
 * INSTALLED STATUS CHECK
 * ================================================================ */

int pkgdb_avail_is_installed(const pkgdb_t *db, int avail_idx)
{
    const avail_pkg_t *p;
    int i;

    if (avail_idx < 0 || avail_idx >= db->avail_count) return 0;
    p = &db->available[avail_idx];

    /* Check by pkg_code and version against installed list */
    if (p->pkg_code[0]) {
        for (i = 0; i < db->inst_count; i++) {
            if (strcmp(db->installed[i].pkg_code, p->pkg_code) == 0 &&
                strcmp(db->installed[i].version, p->version) == 0)
                return 1;
        }
    }

    /* Also check by name and version */
    for (i = 0; i < db->inst_count; i++) {
        if (strcasecmp(db->installed[i].name, p->name) == 0 &&
            strcmp(db->installed[i].version, p->version) == 0)
            return 1;
    }

    return 0;
}

/* ================================================================
 * VERSION ROLLUP HELPERS
 * ================================================================ */

int pkgdb_is_latest_version(const pkgdb_t *db, int avail_idx)
{
    int latest;
    if (avail_idx < 0 || avail_idx >= db->avail_count) return 0;
    latest = pkgdb_find_avail(db, db->available[avail_idx].name);
    return (latest == avail_idx) ? 1 : 0;
}

int pkgdb_find_all_versions(const pkgdb_t *db, const char *name,
                            int *results, int max_results)
{
    int i, count = 0;
    /* Collect all matching indices */
    for (i = 0; i < db->avail_count && count < max_results; i++) {
        if (strcasecmp(db->available[i].name, name) == 0) {
            results[count++] = i;
        }
    }
    /* Sort newest first (simple bubble sort - small N) */
    {
        int j, tmp;
        for (i = 0; i < count - 1; i++) {
            for (j = 0; j < count - i - 1; j++) {
                if (ver_cmp(db->available[results[j]].version,
                            db->available[results[j+1]].version) < 0) {
                    tmp = results[j];
                    results[j] = results[j+1];
                    results[j+1] = tmp;
                }
            }
        }
    }
    return count;
}

int pkgdb_any_version_installed(const pkgdb_t *db, const char *name)
{
    int i;
    /* Check installed list by name */
    for (i = 0; i < db->inst_count; i++) {
        if (strcasecmp(db->installed[i].name, name) == 0)
            return 1;
    }
    /* Check all available versions of this name by pkg_code */
    for (i = 0; i < db->avail_count; i++) {
        if (strcasecmp(db->available[i].name, name) == 0) {
            if (pkgdb_avail_is_installed(db, i))
                return 1;
        }
    }
    return 0;
}

/* ================================================================
 * DOWNLOAD PROGRESS
 * ================================================================ */

static void download_progress(long current, long total)
{
    if (total > 0) {
        int pct = (int)((current * 100) / total);
        fprintf(stderr, "\r  downloading... %ld/%ld bytes (%d%%)",
                current, total, pct);
    } else {
        fprintf(stderr, "\r  downloading... %ld bytes", current);
    }
    fflush(stderr);
}

/* ================================================================
 * ADMIN FILE FOR NON-INTERACTIVE PKGADD
 * ================================================================ */

static char admin_path[512];
static void cleanup_admin(void)
{
    if (admin_path[0]) { unlink(admin_path); admin_path[0] = '\0'; }
}

static const char *pkgadd_admin_file(void)
{
    FILE *f;
    int fd;
    if (admin_path[0]) return admin_path;
    if (snprintf(admin_path, sizeof(admin_path), "%s/admin-XXXXXX", SPM_VAR) >= (int)sizeof(admin_path)) return NULL;
    fd = mkstemp(admin_path);
    if (fd < 0) { admin_path[0] = '\0'; return NULL; }
    f = fdopen(fd, "w");
    if (!f) { close(fd); cleanup_admin(); return NULL; }
    if (fprintf(f, "mail=\ninstance=overwrite\npartial=nocheck\nrunlevel=nocheck\n"
                   "idepend=nocheck\nrdepend=nocheck\nspace=nocheck\nsetuid=nocheck\n"
                   "conflict=nocheck\naction=nocheck\nbasedir=default\n") < 0) {
        fclose(f); cleanup_admin(); return NULL;
    }
    if (fclose(f) != 0) { cleanup_admin(); return NULL; }
    if (atexit(cleanup_admin) != 0) { cleanup_admin(); return NULL; }
    return admin_path;
}

/* Decompress completely before invoking pkgadd. No shell or prior pkgrm. */
static int install_cached(const char *path, const char *filename)
{
    char temporary[512];
    const char *input = path, *admin;
    size_t length = strlen(filename);
    int fd = -1, rc = -1, compressed = 0;
    struct stat st;
    char *arguments[9];
    temporary[0] = '\0';
    if (length > 6 && strcmp(filename + length - 6, ".pkg.Z") == 0) compressed = 1;
    else if (length > 3 && strcmp(filename + length - 3, ".gz") == 0) compressed = 2;
    else if (length < 4 || strcmp(filename + length - 4, ".pkg") != 0) return -1;
    if (compressed) {
        if (snprintf(temporary, sizeof(temporary), "%s/.unpack-XXXXXX", SPM_CACHE) >= (int)sizeof(temporary)) return -1;
        fd = mkstemp(temporary);
        if (fd < 0) return -1;
        arguments[0] = compressed == 1 ? "/usr/bin/uncompress" : "/opt/sst/bin/gzip";
        arguments[1] = compressed == 1 ? "-c" : "-dc";
        arguments[2] = (char *)path; arguments[3] = NULL;
        rc = spm_run(arguments, fd);
        if (rc && compressed == 2) {
            if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) { rc = -1; goto done; }
            arguments[0] = "/usr/bin/gzip";
            rc = spm_run(arguments, fd);
        }
        if (rc && compressed == 2) {
            if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) { rc = -1; goto done; }
            arguments[0] = "/usr/tgcware/bin/gzip";
            rc = spm_run(arguments, fd);
        }
        if (rc || fstat(fd, &st) != 0 || st.st_size <= 0) { rc = -1; goto done; }
        if (close(fd) != 0) { fd = -1; rc = -1; goto done; }
        fd = -1; input = temporary;
    }
    admin = pkgadd_admin_file();
    if (!admin) { rc = -1; goto done; }
    arguments[0] = "/usr/sbin/pkgadd"; arguments[1] = "-n";
    arguments[2] = "-a"; arguments[3] = (char *)admin;
    arguments[4] = "-d"; arguments[5] = (char *)input;
    arguments[6] = "all"; arguments[7] = NULL;
    rc = spm_run(arguments, -1);
done:
    if (fd >= 0) close(fd);
    if (temporary[0]) unlink(temporary);
    return rc;
}

/* ================================================================
 * INSTALL PACKAGE
 * ================================================================ */

/* One verified previous package per code. Legacy cache paths are not history. */
static int version_matches(const char *a, const char *b)
{
    size_t alen, blen;
    if (*a == '"') a++;
    if (*b == '"') b++;
    alen = strcspn(a, ",\""); blen = strcspn(b, ",\"");
    return alen > 0 && alen == blen && strncmp(a, b, alen) == 0;
}

static int history_path(const char *code, char *path, size_t size)
{
    if (!spm_safe_component(code)) return -1;
    return snprintf(path, size, "%s/%s.previous", SPM_ROLLBACK, code) >= (int)size ? -1 : 0;
}

static int previous_candidate(const pkgdb_t *db, const avail_pkg_t *next, avail_pkg_t *previous)
{
    int i, j;
    char path[512];
    for (i = 0; i < db->inst_count; i++) {
        const installed_pkg_t *installed = &db->installed[i];
        if (strcmp(installed->pkg_code, next->pkg_code) != 0 ||
            version_matches(installed->version, next->version)) continue;
        for (j = 0; j < db->avail_count; j++) {
            const avail_pkg_t *old = &db->available[j];
            if (strcmp(old->pkg_code, installed->pkg_code) != 0 ||
                !version_matches(installed->version, old->version) ||
                !spm_safe_component(old->name) || !spm_safe_component(old->version) ||
                !spm_safe_component(old->filename) || !spm_safe_component(old->repo_name) ||
                (old->source_type != SRC_GITHUB && old->source_type != SRC_TGCWARE)) continue;
            if (snprintf(path, sizeof(path), "%s/%s", SPM_CACHE, old->filename) >= (int)sizeof(path)) continue;
            if (spm_verify_file(path, old->md5, old->source_type == SRC_GITHUB) != 0) continue;
            *previous = *old;
            return 1;
        }
    }
    return 0;
}

static int save_history(const avail_pkg_t *current, const avail_pkg_t *previous)
{
    char path[512], temporary[512];
    int fd, error;
    FILE *file;
    if (history_path(current->pkg_code, path, sizeof(path)) != 0) return -1;
    if (!previous) {
        if (unlink(path) != 0 && errno != ENOENT) return -1;
        return 0;
    }
    if (snprintf(temporary, sizeof(temporary), "%s/.history-XXXXXX", SPM_ROLLBACK) >= (int)sizeof(temporary)) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) return -1;
    file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(temporary); return -1; }
    error = fprintf(file, "%s|%s|%s|%s|%s|%d|%s|%s\n", current->version,
                    previous->name, previous->version, previous->filename, previous->md5,
                    previous->source_type, previous->repo_name, previous->pkg_code) < 0;
    if (fflush(file) != 0 || fsync(fd) != 0) error = 1;
    if (fclose(file) != 0) error = 1;
    if (error || rename(temporary, path) != 0) { unlink(temporary); return -1; }
    return 0;
}

static int load_history(const installed_pkg_t *current, avail_pkg_t *previous, char *journal, size_t size)
{
    char line[2048], *fields[8], *cursor;
    struct stat st;
    FILE *file;
    int i;
    if (history_path(current->pkg_code, journal, size) != 0 ||
        lstat(journal, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    file = fopen(journal, "r");
    if (!file) return -1;
    if (!fgets(line, sizeof(line), file) || !strchr(line, '\n')) { fclose(file); return -1; }
    if (fgetc(file) != EOF || ferror(file)) { fclose(file); return -1; }
    fclose(file);
    line[strcspn(line, "\r\n")] = '\0';
    cursor = line;
    for (i = 0; i < 8; i++) {
        fields[i] = cursor;
        cursor = strchr(cursor, '|');
        if (i < 7) { if (!cursor) return -1; *cursor++ = '\0'; }
        else if (cursor) return -1;
    }
    if (!version_matches(fields[0], current->version) ||
        version_matches(fields[2], current->version) || strcmp(fields[7], current->pkg_code) != 0 ||
        !spm_safe_component(fields[1]) || !spm_safe_component(fields[2]) ||
        !spm_safe_component(fields[3]) || !spm_safe_component(fields[6]) ||
        strlen(fields[1]) >= sizeof(previous->name) || strlen(fields[2]) >= sizeof(previous->version) ||
        strlen(fields[3]) >= sizeof(previous->filename) || strlen(fields[4]) >= sizeof(previous->md5) ||
        strlen(fields[6]) >= sizeof(previous->repo_name) || strlen(fields[7]) >= sizeof(previous->pkg_code)) return -1;
    memset(previous, 0, sizeof(*previous));
    strcpy(previous->name, fields[1]); strcpy(previous->version, fields[2]);
    strcpy(previous->filename, fields[3]); strcpy(previous->md5, fields[4]);
    strcpy(previous->repo_name, fields[6]); strcpy(previous->pkg_code, fields[7]);
    if (strcmp(fields[5], "1") == 0) previous->source_type = SRC_TGCWARE;
    else if (strcmp(fields[5], "2") == 0) previous->source_type = SRC_GITHUB;
    else return -1;
    return 0;
}

void pkgdb_record_install(pkgdb_t *db, const avail_pkg_t *pkg,
                           const char *pkg_file)
{
    installed_pkg_t inst;
    time_t now;
    struct tm *tm;

    memset(&inst, 0, sizeof(inst));
    strncpy(inst.name, pkg->name, sizeof(inst.name) - 1);
    strncpy(inst.version, pkg->version, sizeof(inst.version) - 1);
    strncpy(inst.pkg_code, pkg->pkg_code, sizeof(inst.pkg_code) - 1);
    strncpy(inst.repo_name, pkg->repo_name, sizeof(inst.repo_name) - 1);
    inst.source_type = pkg->source_type;

    now = time(NULL);
    tm = localtime(&now);
    snprintf(inst.install_date, sizeof(inst.install_date),
             "%04d-%02d-%02d %02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min);

    if (pkg_file)
        strncpy(inst.pkg_file, pkg_file, sizeof(inst.pkg_file) - 1);

    /* Remove existing entry for this package (upgrade path) */
    {
        int ii;
        for (ii = 0; ii < db->inst_count; ii++)
            if (pkg->pkg_code[0] ? strcmp(db->installed[ii].pkg_code, pkg->pkg_code) == 0 :
                strcmp(db->installed[ii].name, pkg->name) == 0) break;
        if (ii == db->inst_count) ii = -1;
        if (ii >= 0) {
            /* Keep old .pkg file for rollback */
            memmove(&db->installed[ii], &db->installed[ii + 1],
                    (db->inst_count - ii - 1) * sizeof(installed_pkg_t));
            db->inst_count--;
        }
    }

    add_installed(db, &inst);
    if (pkgdb_save_installed(db) != 0)
        fprintf(stderr, "spm: package installed, but installed database could not be saved\n");
}

int pkgdb_install(pkgdb_t *db, int avail_idx)
{
    const avail_pkg_t *pkg;
    char cache_path[512];
    int rc, has_previous, i;
    avail_pkg_t previous;

    if (avail_idx < 0 || avail_idx >= db->avail_count) return -1;
    pkg = &db->available[avail_idx];

    printf("Installing %s %s from %s...\n", pkg->name, pkg->version,
           pkg->repo_name);

    /* Validate identities and constrain cache paths before package operations. */
    if (!spm_safe_component(pkg->filename) || !spm_safe_component(pkg->name) ||
        !spm_safe_component(pkg->version) ||
        (pkg->pkg_code[0] && !spm_safe_component(pkg->pkg_code))) {
        fprintf(stderr, "spm: invalid package filename or identity\n");
        return -1;
    }
    if (snprintf(cache_path, sizeof(cache_path), "%s/%s",
                 SPM_CACHE, pkg->filename) >= (int)sizeof(cache_path)) return -1;
    {
        struct stat st;
        int require_sha256 = pkg->source_type == SRC_GITHUB;
        if (lstat(cache_path, &st) == 0) {
            if (spm_verify_file(cache_path, pkg->md5, require_sha256) != 0) {
                fprintf(stderr, "spm: cached package rejected; refresh the index and remove the invalid cache file\n");
                return -1;
            }
            printf("  Using verified cached %s\n", pkg->filename);
        } else {
            char temporary[512];
            int fd;
            if (errno != ENOENT) return -1;
            if (snprintf(temporary, sizeof(temporary), "%s/.download-XXXXXX",
                         SPM_CACHE) >= (int)sizeof(temporary)) return -1;
            fd = mkstemp(temporary);
            if (fd < 0) return -1;
            close(fd);
            printf("  Downloading %s (%s)...\n", pkg->filename, pkg->size_str);
            if (http_download(pkg->download_url, temporary, download_progress) < 0 ||
                spm_verify_file(temporary, pkg->md5, require_sha256) != 0 ||
                rename(temporary, cache_path) != 0) {
                unlink(temporary);
                fprintf(stderr, "spm: download or integrity verification failed\n");
                return -1;
            }
            fprintf(stderr, "\n");
        }
    }

    has_previous = previous_candidate(db, pkg, &previous);
    if (!has_previous) {
        for (i = 0; i < db->inst_count; i++) {
            if (strcmp(db->installed[i].pkg_code, pkg->pkg_code) == 0) {
                fprintf(stderr, "spm: no verified previous artifact; rollback will be unavailable after this installation\n");
                break;
            }
        }
    }
    rc = install_cached(cache_path, pkg->filename);

    if (rc != 0) {
        fprintf(stderr, "spm: pkgadd returned %d\n", rc);
        return -1;
    }

    if (save_history(pkg, has_previous ? &previous : NULL) != 0)
        fprintf(stderr, "spm: installed package, but could not update verified rollback history\n");

    /* Record in our database */
    pkgdb_record_install(db, pkg, cache_path);

    printf("  %s %s installed successfully.\n", pkg->name, pkg->version);
    return 0;
}

/* ================================================================
 * REMOVE PACKAGE
 * ================================================================ */

int pkgdb_remove(pkgdb_t *db, int inst_idx)
{
    const installed_pkg_t *pkg;
    char *arguments[4];
    char removed_name[128];
    int rc;

    if (inst_idx < 0 || inst_idx >= db->inst_count) return -1;
    pkg = &db->installed[inst_idx];

    if (!spm_safe_component(pkg->pkg_code)) {
        fprintf(stderr, "spm: no package code for %s\n", pkg->name);
        return -1;
    }

    printf("Removing %s %s (pkg: %s)...\n", pkg->name, pkg->version,
           pkg->pkg_code);

    strncpy(removed_name, pkg->name, sizeof(removed_name) - 1);
    removed_name[sizeof(removed_name) - 1] = '\0';
    arguments[0] = "/usr/sbin/pkgrm"; arguments[1] = "-n";
    arguments[2] = (char *)pkg->pkg_code; arguments[3] = NULL;
    rc = spm_run(arguments, -1);

    if (rc != 0) {
        fprintf(stderr, "spm: pkgrm returned %d\n", rc);
        return -1;
    }

    /* Remove from installed list */
    memmove(&db->installed[inst_idx], &db->installed[inst_idx + 1],
            (db->inst_count - inst_idx - 1) * sizeof(installed_pkg_t));
    db->inst_count--;
    if (pkgdb_save_installed(db) != 0) {
        fprintf(stderr, "spm: package removed, but installed database could not be saved\n");
        return -1;
    }

    printf("  %s removed.\n", removed_name);
    return 0;
}

/* ================================================================
 * ROLLBACK
 * ================================================================ */

int pkgdb_rollback(pkgdb_t *db, int inst_idx)
{
    avail_pkg_t previous;
    char journal[512], path[512];
    const installed_pkg_t *current;
    int rc;
    if (inst_idx < 0 || inst_idx >= db->inst_count) return -1;
    current = &db->installed[inst_idx];
    if (load_history(current, &previous, journal, sizeof(journal)) != 0) {
        fprintf(stderr, "spm: no verified previous-version history; rollback refused\n");
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/%s", SPM_CACHE, previous.filename) >= (int)sizeof(path) ||
        spm_verify_file(path, previous.md5, previous.source_type == SRC_GITHUB) != 0) return -1;
    printf("Rolling back %s to verified version %s...\n", previous.name, previous.version);
    /* pkgadd's overwrite policy replaces the package; never remove it first. */
    rc = install_cached(path, previous.filename);
    if (rc != 0) {
        fprintf(stderr, "spm: rollback installation failed (%d); package state may require repair\n", rc);
        return -1;
    }
    pkgdb_record_install(db, &previous, path);
    if (unlink(journal) != 0) fprintf(stderr, "spm: could not remove consumed rollback history\n");
    printf("  Rollback installation completed.\n");
    return 0;
}

/* ================================================================
 * DEPENDENCY RESOLUTION
 * ================================================================ */

int *pkgdb_resolve_deps(pkgdb_t *db, int avail_idx, int *count)
{
    const avail_pkg_t *pkg;
    int *result = NULL;
    int alloc = 0, n = 0;
    char deps_copy[512];
    char *tok, *saveptr;

    *count = 0;
    if (avail_idx < 0 || avail_idx >= db->avail_count) return NULL;
    pkg = &db->available[avail_idx];

    if (!pkg->deps[0]) return NULL;

    strncpy(deps_copy, pkg->deps, sizeof(deps_copy) - 1);
    deps_copy[sizeof(deps_copy) - 1] = '\0';

    tok = strtok_r(deps_copy, ",", &saveptr);
    while (tok) {
        /* Trim whitespace */
        while (*tok == ' ') tok++;
        {
            int len = strlen(tok);
            while (len > 0 && tok[len-1] == ' ') tok[--len] = '\0';
        }

        /* Skip self-reference (TGCware index includes pkg's own code) */
        if (*tok && strcmp(tok, pkg->pkg_code) == 0) {
            tok = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        if (*tok) {
            /* Check if this dep is already installed on the system */
            if (!pkgdb_sys_installed(tok)) {
                /* Find in available packages */
                int dep_idx = pkgdb_find_by_code(db, tok);
                if (dep_idx >= 0) {
                    /* Check if not already in our result list */
                    int found = 0, k;
                    for (k = 0; k < n; k++) {
                        if (result[k] == dep_idx) { found = 1; break; }
                    }
                    if (!found) {
                        if (n >= alloc) {
                            alloc = alloc ? alloc * 2 : 16;
                            result = (int *)realloc(result, alloc * sizeof(int));
                        }
                        result[n++] = dep_idx;

                        /* Recursively resolve sub-dependencies */
                        {
                            int sub_count = 0;
                            int *sub = pkgdb_resolve_deps(db, dep_idx,
                                                          &sub_count);
                            if (sub) {
                                int j;
                                for (j = 0; j < sub_count; j++) {
                                    int dup = 0;
                                    for (k = 0; k < n; k++) {
                                        if (result[k] == sub[j]) {
                                            dup = 1;
                                            break;
                                        }
                                    }
                                    if (!dup) {
                                        if (n >= alloc) {
                                            alloc = alloc ? alloc * 2 : 16;
                                            result = (int *)realloc(result,
                                                        alloc * sizeof(int));
                                        }
                                        result[n++] = sub[j];
                                    }
                                }
                                free(sub);
                            }
                        }
                    }
                }
            }
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    *count = n;
    return n > 0 ? result : NULL;
}

/* Track visited packages during dep tree printing to avoid cycles */
static int dep_visited[4096];
static int dep_visited_count;

static int dep_is_visited(int idx)
{
    int i;
    for (i = 0; i < dep_visited_count; i++)
        if (dep_visited[i] == idx) return 1;
    return 0;
}

static void dep_mark_visited(int idx)
{
    if (dep_visited_count < 4096)
        dep_visited[dep_visited_count++] = idx;
}

void pkgdb_print_deps(const pkgdb_t *db, int avail_idx, int depth)
{
    const avail_pkg_t *pkg;
    char deps_copy[512];
    char *tok, *saveptr;
    int i;

    if (avail_idx < 0 || avail_idx >= db->avail_count) return;
    if (depth > 10) { printf("  ... (max depth)\n"); return; }

    /* Reset visited list at top level */
    if (depth == 0)
        dep_visited_count = 0;

    pkg = &db->available[avail_idx];

    for (i = 0; i < depth; i++) printf("  ");
    printf("%s %s", pkg->name, pkg->version);
    if (pkg->pkg_code[0])
        printf(" [%s]", pkg->pkg_code);

    if (pkgdb_sys_installed(pkg->pkg_code))
        printf(" (installed)");

    if (depth > 0 && dep_is_visited(avail_idx)) {
        printf(" (see above)\n");
        return;
    }
    printf("\n");

    dep_mark_visited(avail_idx);

    if (!pkg->deps[0]) return;

    strncpy(deps_copy, pkg->deps, sizeof(deps_copy) - 1);
    deps_copy[sizeof(deps_copy) - 1] = '\0';

    tok = strtok_r(deps_copy, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        /* Skip self-reference (TGCware index includes pkg's own code) */
        if (*tok && strcmp(tok, pkg->pkg_code) == 0) {
            tok = strtok_r(NULL, ",", &saveptr);
            continue;
        }
        if (*tok) {
            int dep_idx = pkgdb_find_by_code(db, tok);
            if (dep_idx >= 0) {
                pkgdb_print_deps(db, dep_idx, depth + 1);
            } else {
                for (i = 0; i <= depth; i++) printf("  ");
                printf("%s", tok);
                if (pkgdb_sys_installed(tok))
                    printf(" (installed)");
                else
                    printf(" (NOT FOUND)");
                printf("\n");
            }
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
}

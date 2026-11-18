/*
 * spm.c - Sunstorm Package Manager
 *
 * Main CLI entry point.  Uses package-owned OpenSSL3 for
 * HTTPS access to configured repositories and GitHub releases.
 *
 * Commands:
 *   update                  - Refresh package indices
 *   search <term>           - Search available packages
 *   install <pkg> [pkg...]  - Install packages (with deps)
 *   remove <pkg> [pkg...]   - Remove packages
 *   list [--installed]      - List packages
 *   info <pkg>              - Show package details
 *   rollback <pkg>          - Roll back to previous version
 *   upgrade [pkg]           - Upgrade packages
 *   deps <pkg>              - Show dependency tree
 *   repo list               - List configured repos
 *   cache clean             - Clear download cache
 *
 * Build: see Makefile (runtime-loaded OpenSSL3; -lsolcompat -lsocket -lnsl -ldl)
 */

#include "config.h"
#include "http.h"
#include "html.h"
#include "json.h"
#include "pkgdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

static spm_config_t g_config;
static pkgdb_t *g_db;

/* ================================================================
 * USAGE
 * ================================================================ */

static void usage(void)
{
    printf(
        "spm %s - Sunstorm Package Manager\n"
        "\n"
        "Usage: spm <command> [arguments]\n"
        "\n"
        "Commands:\n"
        "  update              Refresh package index from all repos\n"
        "  search <term>       Search available packages\n"
        "  install <pkg>...    Install packages (resolves deps)\n"
        "  remove <pkg>...     Remove packages\n"
        "  list                List available packages\n"
        "  list --installed    List installed packages\n"
        "  info <pkg>          Show package details\n"
        "  deps <pkg>          Show dependency tree\n"
        "  rollback <pkg>      Roll back package to previous version\n"
        "  upgrade [pkg]       Upgrade all or specific package\n"
        "  repo list           List configured repositories\n"
        "  cache clean         Clear download cache\n"
        "\n"
        "Configuration: %s/repos.conf\n"
        "SSL: package-owned OpenSSL3\n"
        "\n",
        SPM_VERSION, SPM_ETC);
}

/* ================================================================
 * INIT
 * ================================================================ */

static int init(void)
{
    config_defaults(&g_config);
    config_load(&g_config);
    config_init_dirs();

    /* Initialize OpenSSL */
    if (g_config.ca_bundle[0])
        http_set_ca_bundle(g_config.ca_bundle);
    if (http_init() != 0) {
        fprintf(stderr, "spm: warning: SSL initialization failed.\n");
        fprintf(stderr, "  HTTPS connections may not work.\n");
    }

    g_db = pkgdb_new();
    pkgdb_load_index(g_db);
    pkgdb_load_installed(g_db);
    pkgdb_load_spool(g_db, "/var/spool/pkg");

    return 0;
}

static void cleanup(void)
{
    pkgdb_free(g_db);
    http_shutdown();
}

/* ================================================================
 * UPDATE COMMAND
 * ================================================================ */

static int cmd_update(void)
{
    int i;
    http_response_t resp;
    int total_pkgs = 0;

    /* Clear existing available packages */
    g_db->avail_count = 0;

    printf("Updating package index...\n");

    for (i = 0; i < g_config.repo_count; i++) {
        repo_def_t *r = &g_config.repos[i];

        if (!r->enabled) {
            printf("  [skip] %s (disabled)\n", r->name);
            continue;
        }

        if (r->type == REPO_TYPE_TGCWARE) {
            printf("  [tgcware] %s\n", r->name);
            printf("    Connecting to %s ...\n", r->url);
            fflush(stdout);

            memset(&resp, 0, sizeof(resp));
            if (http_get(r->url, &resp) == 0 && resp.body) {
                tgc_index_t *idx = tgc_parse_index(resp.body);
                if (idx) {
                    printf("    %d packages found\n", idx->count);
                    pkgdb_add_tgcware(g_db, idx, r->url,
                                       r->source_url, r->name);
                    total_pkgs += idx->count;
                    tgc_index_free(idx);
                } else {
                    fprintf(stderr, "    warning: failed to parse index\n");
                }
                free(resp.body);
            } else {
                fprintf(stderr, "    error: failed to fetch %s\n", r->url);
                if (resp.body) free(resp.body);
            }

        } else if (r->type == REPO_TYPE_GITHUB) {
            int j;
            printf("  [github] %s ...\n", r->name);

            for (j = 0; j < r->repo_count; j++) {
                char api_url[512];
                int before = g_db->avail_count;

                snprintf(api_url, sizeof(api_url),
                         "https://api.github.com/repos/%s/%s/releases",
                         r->owner, r->repos[j]);

                printf("    %s/%s ... ", r->owner, r->repos[j]);
                fflush(stdout);

                printf("connecting... ");
                fflush(stdout);

                memset(&resp, 0, sizeof(resp));
                if (http_get(api_url, &resp) == 0 && resp.body) {
                    pkgdb_add_github(g_db, resp.body, r->name,
                                      r->repos[j], r->owner);
                    printf(" %d releases\n",
                           g_db->avail_count - before);
                    total_pkgs += g_db->avail_count - before;
                    free(resp.body);
                } else {
                    printf(" failed\n");
                    if (resp.body) free(resp.body);
                }
            }
        }
    }

    /* Save the updated index */
    if (pkgdb_save_index(g_db) == 0) {
        printf("\nIndex updated: %d packages available.\n", total_pkgs);
    } else {
        fprintf(stderr, "\nWarning: failed to save index\n");
    }

    return 0;
}

/* ================================================================
 * SEARCH COMMAND
 * ================================================================ */

static int cmd_search(const char *term)
{
    int results[512];
    int count, i, shown;

    if (g_db->avail_count == 0) {
        fprintf(stderr, "No packages in index. Run 'spm update' first.\n");
        return 1;
    }

    count = pkgdb_search(g_db, term, results, 512);

    if (count == 0) {
        printf("No packages matching '%s'\n", term);
        return 0;
    }

    printf("%-24s %-12s %-8s %-10s %s\n",
           "NAME", "VERSION", "ARCH", "REPO", "DESCRIPTION");
    printf("%-24s %-12s %-8s %-10s %s\n",
           "----", "-------", "----", "----", "-----------");

    shown = 0;
    for (i = 0; i < count; i++) {
        const avail_pkg_t *p = &g_db->available[results[i]];
        char desc_short[50];

        /* Version rollup: skip non-latest versions */
        if (!pkgdb_is_latest_version(g_db, results[i]))
            continue;

        strncpy(desc_short, p->description, 49);
        desc_short[49] = '\0';
        printf("%-24s %-12s %-8s %-10s %s\n",
               p->name, p->version, p->arch, p->repo_name, desc_short);
        shown++;
    }

    printf("\n%d package(s) found.\n", shown);
    return 0;
}

/* ================================================================
 * LIST COMMAND
 * ================================================================ */

static int cmd_list(int installed_only)
{
    int i;

    if (installed_only) {
        if (g_db->inst_count == 0) {
            printf("No packages installed via spm.\n");
            return 0;
        }
        printf("%-24s %-12s %-10s %-18s %s\n",
               "NAME", "VERSION", "PKG CODE", "INSTALLED", "REPO");
        printf("%-24s %-12s %-10s %-18s %s\n",
               "----", "-------", "--------", "---------", "----");
        for (i = 0; i < g_db->inst_count; i++) {
            const installed_pkg_t *p = &g_db->installed[i];
            printf("%-24s %-12s %-10s %-18s %s\n",
                   p->name, p->version, p->pkg_code,
                   p->install_date, p->repo_name);
        }
        printf("\n%d package(s) installed.\n", g_db->inst_count);
    } else {
        if (g_db->avail_count == 0) {
            fprintf(stderr, "No packages in index. Run 'spm update' first.\n");
            return 1;
        }
        printf("%-24s %-12s %-8s %-6s %-10s %s\n",
               "NAME", "VERSION", "ARCH", "SIZE", "REPO", "DESCRIPTION");
        printf("%-24s %-12s %-8s %-6s %-10s %s\n",
               "----", "-------", "----", "----", "----", "-----------");
        {
            int shown = 0;
            for (i = 0; i < g_db->avail_count; i++) {
                const avail_pkg_t *p = &g_db->available[i];
                char desc_short[40];

                /* Version rollup: only show latest version */
                if (!pkgdb_is_latest_version(g_db, i))
                    continue;

                strncpy(desc_short, p->description, 39);
                desc_short[39] = '\0';
                printf("%-24s %-12s %-8s %-6s %-10s %s\n",
                       p->name, p->version, p->arch, p->size_str,
                       p->repo_name, desc_short);
                shown++;
            }
            printf("\n%d package(s) available (%d total versions).\n",
                   shown, g_db->avail_count);
        }
    }

    return 0;
}

/* ================================================================
 * INFO COMMAND
 * ================================================================ */

static int cmd_info(const char *name)
{
    int idx = pkgdb_find_avail(g_db, name);
    int iidx = pkgdb_find_installed(g_db, name);

    if (idx < 0 && iidx < 0) {
        fprintf(stderr, "Package '%s' not found.\n", name);
        return 1;
    }

    if (idx >= 0) {
        const avail_pkg_t *p = &g_db->available[idx];
        printf("Package:      %s\n", p->name);
        printf("Version:      %s-%d\n", p->version, p->release);
        printf("Architecture: %s\n", p->arch);
        printf("Package Code: %s\n", p->pkg_code);
        printf("Description:  %s\n", p->description);
        printf("Size:         %s\n", p->size_str);
        printf("Repository:   %s\n", p->repo_name);
        printf("Source Type:  %s\n",
               p->source_type == SRC_TGCWARE ? "TGCware" : "GitHub");
        printf("Download:     %s\n", p->download_url);
        if (p->deps[0])
            printf("Dependencies: %s\n", p->deps);
        else
            printf("Dependencies: none\n");
        if (p->source_url[0])
            printf("Source:       %s\n", p->source_url);
        if (p->md5[0])
            printf("MD5:          %s\n", p->md5);
    }

    if (iidx >= 0) {
        const installed_pkg_t *ip = &g_db->installed[iidx];
        printf("\n--- Installed ---\n");
        printf("Version:      %s\n", ip->version);
        printf("Installed:    %s\n", ip->install_date);
        printf("Cached File:  %s\n", ip->pkg_file);
    } else {
        printf("\nStatus:       not installed\n");
    }

    /* Show all available versions if more than one */
    if (idx >= 0) {
        int ver_list[64];
        int nver, vi;
        nver = pkgdb_find_all_versions(g_db,
                   g_db->available[idx].name, ver_list, 64);
        if (nver > 1) {
            printf("\nAvailable Versions:\n");
            for (vi = 0; vi < nver; vi++) {
                const avail_pkg_t *vp = &g_db->available[ver_list[vi]];
                int ver_inst = pkgdb_avail_is_installed(g_db, ver_list[vi]);
                printf("  %s-%d  [%s]%s%s\n",
                    vp->version, vp->release,
                    vp->pkg_code,
                    ver_inst ? " (installed)" : "",
                    ver_list[vi] == idx ? " *latest" : "");
            }
        }
    }

    return 0;
}

/* ================================================================
 * DEPS COMMAND
 * ================================================================ */

static int cmd_deps(const char *name)
{
    int idx = pkgdb_find_avail(g_db, name);

    if (idx < 0) {
        /* Try by package code */
        idx = pkgdb_find_by_code(g_db, name);
    }

    if (idx < 0) {
        fprintf(stderr, "Package '%s' not found.\n", name);
        return 1;
    }

    printf("Dependency tree for %s:\n\n", name);
    pkgdb_print_deps(g_db, idx, 0);

    /* Also show summary of uninstalled deps */
    {
        int count = 0;
        int *deps = pkgdb_resolve_deps(g_db, idx, &count);
        if (deps && count > 0) {
            int j;
            printf("\n%d uninstalled dependencies:\n", count);
            for (j = 0; j < count; j++) {
                const avail_pkg_t *d = &g_db->available[deps[j]];
                printf("  %s %s [%s]\n", d->name, d->version, d->pkg_code);
            }
            free(deps);
        } else {
            printf("\nAll dependencies satisfied.\n");
        }
    }

    return 0;
}

/* ================================================================
 * INSTALL COMMAND
 * ================================================================ */

static int cmd_install(int argc, char **argv)
{
    int i;

    if (g_db->avail_count == 0) {
        fprintf(stderr, "No packages in index. Run 'spm update' first.\n");
        return 1;
    }

    for (i = 0; i < argc; i++) {
        int idx = pkgdb_find_avail(g_db, argv[i]);
        if (idx < 0) {
            /* Try package code */
            idx = pkgdb_find_by_code(g_db, argv[i]);
        }
        if (idx < 0) {
            fprintf(stderr, "Package '%s' not found. Skipping.\n", argv[i]);
            continue;
        }

        /* Check if already installed with same version */
        {
            int iidx = pkgdb_find_installed(g_db, g_db->available[idx].name);
            if (iidx >= 0) {
                if (strcmp(g_db->installed[iidx].version,
                           g_db->available[idx].version) == 0) {
                    printf("%s %s is already installed.\n",
                           g_db->available[idx].name,
                           g_db->available[idx].version);
                    continue;
                }
            }
        }

        /* Resolve and install dependencies first */
        {
            int dep_count = 0;
            int *deps = pkgdb_resolve_deps(g_db, idx, &dep_count);
            if (deps && dep_count > 0) {
                int j;
                printf("\nResolving %d dependencies for %s:\n",
                       dep_count, argv[i]);
                for (j = 0; j < dep_count; j++) {
                    printf("  -> %s %s [%s]\n",
                           g_db->available[deps[j]].name,
                           g_db->available[deps[j]].version,
                           g_db->available[deps[j]].pkg_code);
                }
                printf("\n");

                /* Install each dependency */
                for (j = 0; j < dep_count; j++) {
                    if (pkgdb_install(g_db, deps[j]) != 0) {
                        fprintf(stderr, "Failed to install dependency %s\n",
                                g_db->available[deps[j]].name);
                        free(deps);
                        return 1;
                    }
                }
                free(deps);
            }
        }

        /* Install the main package */
        if (pkgdb_install(g_db, idx) != 0) {
            fprintf(stderr, "Failed to install %s\n", argv[i]);
            return 1;
        }
    }

    return 0;
}

/* ================================================================
 * REMOVE COMMAND
 * ================================================================ */

static int cmd_remove(int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++) {
        int idx = pkgdb_find_installed(g_db, argv[i]);
        if (idx < 0) {
            fprintf(stderr, "Package '%s' is not installed.\n", argv[i]);
            continue;
        }
        if (pkgdb_remove(g_db, idx) != 0) {
            fprintf(stderr, "Failed to remove %s\n", argv[i]);
            return 1;
        }
    }

    return 0;
}

/* ================================================================
 * ROLLBACK COMMAND
 * ================================================================ */

static int cmd_rollback(const char *name)
{
    int idx = pkgdb_find_installed(g_db, name);
    if (idx < 0) {
        fprintf(stderr, "Package '%s' is not installed.\n", name);
        return 1;
    }
    return pkgdb_rollback(g_db, idx);
}

/* ================================================================
 * UPGRADE COMMAND
 * ================================================================ */

static int cmd_upgrade(int argc, char **argv)
{
    int i;
    int upgraded = 0;

    if (g_db->avail_count == 0) {
        fprintf(stderr, "No packages in index. Run 'spm update' first.\n");
        return 1;
    }

    if (argc == 0) {
        /* Upgrade all installed packages */
        printf("Checking for upgrades...\n");
        fflush(stdout);
        for (i = 0; i < g_db->inst_count; i++) {
            const installed_pkg_t *ip = &g_db->installed[i];
            int aidx = pkgdb_find_avail(g_db, ip->name);
            if (aidx >= 0) {
                const avail_pkg_t *ap = &g_db->available[aidx];
                /* Compare versions */
                if (strcmp(ap->version, ip->version) != 0) {
                    printf("  %s: %s -> %s\n", ip->name,
                           ip->version, ap->version);
                    if (pkgdb_install(g_db, aidx) == 0)
                        upgraded++;
                }
            }
        }
        if (upgraded == 0) printf("All packages are up to date.\n");
        else printf("%d package(s) upgraded.\n", upgraded);
    } else {
        /* Upgrade specific packages */
        for (i = 0; i < argc; i++) {
            int aidx = pkgdb_find_avail(g_db, argv[i]);
            if (aidx < 0) {
                fprintf(stderr, "Package '%s' not found.\n", argv[i]);
                continue;
            }
            printf("Upgrading %s...\n", argv[i]);
            if (pkgdb_install(g_db, aidx) == 0)
                upgraded++;
        }
    }

    return 0;
}

/* ================================================================
 * REPO LIST COMMAND
 * ================================================================ */

static int cmd_repo_list(void)
{
    int i;

    printf("Configured repositories:\n\n");

    for (i = 0; i < g_config.repo_count; i++) {
        repo_def_t *r = &g_config.repos[i];
        printf("  [%s] %s\n", r->enabled ? "on " : "off", r->name);
        printf("    Type: %s\n",
               r->type == REPO_TYPE_TGCWARE ? "TGCware" : "GitHub");

        if (r->type == REPO_TYPE_TGCWARE) {
            printf("    URL:  %s\n", r->url);
            if (r->source_url[0])
                printf("    Src:  %s\n", r->source_url);
        } else {
            int j;
            printf("    Owner: %s\n", r->owner);
            printf("    Repos:");
            for (j = 0; j < r->repo_count; j++) {
                printf(" %s", r->repos[j]);
                if (j < r->repo_count - 1) printf(",");
            }
            printf("\n");
        }
        printf("\n");
    }

    printf("SSL: package-owned OpenSSL3\n");
    if (g_config.ca_bundle[0])
        printf("CA bundle: %s\n", g_config.ca_bundle);
    return 0;
}

/* ================================================================
 * CACHE CLEAN COMMAND
 * ================================================================ */

static int cmd_cache_clean(void)
{
    DIR *d;
    struct dirent *ent;
    int count = 0;
    long freed = 0;

    d = opendir(SPM_CACHE);
    if (!d) {
        printf("Cache directory is empty.\n");
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        char path[512];
        struct stat st;

        if (ent->d_name[0] == '.') continue;

        snprintf(path, sizeof(path), "%s/%s", SPM_CACHE, ent->d_name);
        if (stat(path, &st) == 0) {
            freed += st.st_size;
            unlink(path);
            count++;
        }
    }

    closedir(d);
    if (count > 0)
        printf("Removed %d cached file(s), freed %.1f MB.\n",
               count, freed / 1048576.0);
    else
        printf("Cache is empty.\n");

    return 0;
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        init();
        usage();
        cleanup();
        return 0;
    }

    /* Allow --version and --help without full init */
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("spm %s\n", SPM_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        init();
        usage();
        cleanup();
        return 0;
    }

    init();

    if (strcmp(argv[1], "update") == 0) {
        return cmd_update();
    }
    else if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: spm search <term>\n");
            return 1;
        }
        return cmd_search(argv[2]);
    }
    else if (strcmp(argv[1], "list") == 0) {
        int installed = 0;
        if (argc >= 3 && strcmp(argv[2], "--installed") == 0)
            installed = 1;
        return cmd_list(installed);
    }
    else if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: spm info <package>\n");
            return 1;
        }
        return cmd_info(argv[2]);
    }
    else if (strcmp(argv[1], "deps") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: spm deps <package>\n");
            return 1;
        }
        return cmd_deps(argv[2]);
    }
    else if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: spm install <package>...\n");
            return 1;
        }
        return cmd_install(argc - 2, argv + 2);
    }
    else if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: spm remove <package>...\n");
            return 1;
        }
        return cmd_remove(argc - 2, argv + 2);
    }
    else if (strcmp(argv[1], "rollback") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: spm rollback <package>\n");
            return 1;
        }
        return cmd_rollback(argv[2]);
    }
    else if (strcmp(argv[1], "upgrade") == 0) {
        return cmd_upgrade(argc - 2, argv + 2);
    }
    else if (strcmp(argv[1], "repo") == 0) {
        if (argc >= 3 && strcmp(argv[2], "list") == 0) {
            return cmd_repo_list();
        }
        fprintf(stderr, "Usage: spm repo list\n");
        return 1;
    }
    else if (strcmp(argv[1], "cache") == 0) {
        if (argc >= 3 && strcmp(argv[2], "clean") == 0) {
            return cmd_cache_clean();
        }
        fprintf(stderr, "Usage: spm cache clean\n");
        return 1;
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage();
        cleanup();
        return 1;
    }

    cleanup();
    return 0;
}

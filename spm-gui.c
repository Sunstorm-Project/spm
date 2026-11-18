/*
 * spm-gui.c - Motif/CDE GUI for spm
 *
 * Graphical package manager for Solaris 7 / CDE / SPARCstation.
 * Uses Motif widgets for a classic 2-pane layout with package
 * list on the left and info/log on the right.
 *
 * Features:
 *   - Browse available and installed packages
 *   - Search packages by name
 *   - Install, remove, and upgrade packages
 *   - View dependency trees
 *   - Manage repositories
 *   - Monitor background update agent status
 *
 * Build: see Makefile (requires Motif, X11, OpenSSL)
 *
 * Run:
 *   DISPLAY=:0 ./spm-gui
 */

#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/Form.h>
#include <Xm/PanedW.h>
#include <Xm/Label.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/CascadeB.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/List.h>
#include <Xm/ScrolledW.h>
#include <Xm/MessageB.h>
#include <Xm/Frame.h>
#include <Xm/ToggleB.h>
#include <Xm/DrawingA.h>
#include <Xm/Protocols.h>
#include <Xm/SelectioB.h>
#include <X11/Xlib.h>
#include <X11/Shell.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <stropts.h>
#include <termios.h>
#include <errno.h>

#include "config.h"
#include "http.h"
#include "json.h"
#include "html.h"
#include "pkgdb.h"

/* ================================================================
 * GLOBALS
 * ================================================================ */

static XtAppContext    app_context;
static Widget          top_shell;
static Widget          main_window;
static Widget          menu_bar;
static Widget          pkg_list_w;
static Widget          info_text_w;
static Widget          search_tf;
static Widget          status_label;
static Widget          filter_all_btn;
static Widget          filter_inst_btn;
static Widget          filter_avail_btn;

static spm_config_t g_config;
static pkgdb_t        *g_db;

/* Current filter mode */
#define FILTER_ALL       0
#define FILTER_INSTALLED 1
#define FILTER_AVAILABLE 2
static int g_filter = FILTER_ALL;

/* Selected package index (into visible list) */
static int g_selected = -1;

/* Visible list mapping: vis_map[i] = avail index, or -(inst_idx+1) for inst-only */
static int *vis_map;
static int  vis_count;

/* Agent status check timer */
static XtIntervalId agent_timer;

/* Action buttons (global for sensitivity control) */
static Widget install_btn;
static Widget remove_btn;
static Widget upgrade_btn;
static Widget deps_btn_w;
static Widget upgrade_all_btn;

/* Version dropdown (OptionMenu) */
static Widget  version_option_menu;
static Widget  version_pulldown;
static Widget  version_buttons[64];
static int     version_indices[64];  /* avail_idx for each dropdown entry */
static int     version_count;
static int     selected_version_idx; /* avail_idx of the chosen dropdown entry */

/* Selection state: computed when a package is selected */
static int     sel_is_installed;     /* 1 if any version installed */
static int     sel_inst_idx;         /* index into g_db->installed, or -1 */
static int     sel_is_avail;         /* 1 if in available index */
static int     sel_has_upgrade;      /* 1 if newer version available */
static int     sel_nversions;        /* how many versions available */
static int     sel_ver_list[64];     /* all version avail indices */

/* ================================================================
 * FORWARD DECLARATIONS
 * ================================================================ */

static void create_ui(void);
static void populate_list(void);
static void show_pkg_info(int avail_idx);
static void set_status(const char *msg);
static void append_log(const char *text);
static void check_agent_status(XtPointer client_data, XtIntervalId *id);
static void cleanup_and_exit(int code);

/* ================================================================
 * INSTALL PROGRESS DIALOG
 * ================================================================ */

/* State for the progress dialog */
static Widget  progress_dialog;
static Widget  progress_text_w;
static Widget  progress_da_w;          /* drawn progress bar */
static Widget  progress_close_btn;
static int     progress_pct;           /* progress 0-100 */
static GC      progress_bar_gc;
static int     progress_bar_gc_ok;
static Pixel   progress_fill_pixel;
static int     progress_fill_ok;
static XFontStruct *progress_bar_font;
static int     progress_pipe_fd;       /* read end of pipe from child */
static pid_t   progress_child_pid;
static XtInputId progress_input_id;
static int     progress_running;

static void progress_dialog_append(const char *text)
{
    XmTextPosition pos;
    pos = XmTextGetLastPosition(progress_text_w);
    XmTextInsert(progress_text_w, pos, (char *)text);
    XmTextShowPosition(progress_text_w, XmTextGetLastPosition(progress_text_w));
}

/* ---- Custom-drawn progress bar ---- */

static void draw_progress_bar(void)
{
    Display *dpy;
    Window win;
    Dimension w, h;
    int fill_w, len;
    char pct_text[16];
    Pixel bg_px, fg_px;
    Colormap cmap;
    XColor xc, exact;

    if (!progress_da_w || !XtIsRealized(progress_da_w))
        return;

    dpy = XtDisplay(progress_da_w);
    win = XtWindow(progress_da_w);
    XtVaGetValues(progress_da_w, XmNwidth, &w, XmNheight, &h, NULL);

    bg_px = WhitePixelOfScreen(XtScreen(progress_da_w));
    fg_px = BlackPixelOfScreen(XtScreen(progress_da_w));

    if (!progress_bar_gc_ok) {
        XGCValues gcv;
        gcv.foreground = fg_px;
        gcv.background = bg_px;
        progress_bar_gc = XCreateGC(dpy, win,
            GCForeground | GCBackground, &gcv);
        progress_bar_gc_ok = 1;
    }

    if (!progress_fill_ok) {
        cmap = DefaultColormapOfScreen(XtScreen(progress_da_w));
        if (XAllocNamedColor(dpy, cmap, "SteelBlue", &xc, &exact))
            progress_fill_pixel = xc.pixel;
        else
            progress_fill_pixel = fg_px;
        progress_fill_ok = 1;
    }

    /* Clear background */
    XSetForeground(dpy, progress_bar_gc, bg_px);
    XFillRectangle(dpy, win, progress_bar_gc, 0, 0, w, h);

    /* Draw filled portion */
    if (progress_pct > 0) {
        fill_w = (int)((long)w * progress_pct / 100);
        if (fill_w > (int)w) fill_w = (int)w;
        XSetForeground(dpy, progress_bar_gc, progress_fill_pixel);
        XFillRectangle(dpy, win, progress_bar_gc, 0, 0, fill_w, h);
    }

    /* Border */
    XSetForeground(dpy, progress_bar_gc, fg_px);
    XDrawRectangle(dpy, win, progress_bar_gc, 0, 0, w - 1, h - 1);

    /* Centered percentage text */
    snprintf(pct_text, sizeof(pct_text), "%d%%", progress_pct);
    len = strlen(pct_text);
    {
        int text_w, text_x, text_y;

        if (!progress_bar_font)
            progress_bar_font = XQueryFont(dpy,
                XGContextFromGC(progress_bar_gc));

        if (progress_bar_font) {
            text_w = XTextWidth(progress_bar_font, pct_text, len);
            text_x = ((int)w - text_w) / 2;
            text_y = ((int)h + progress_bar_font->ascent
                      - progress_bar_font->descent) / 2;
        } else {
            text_x = ((int)w - len * 7) / 2;
            text_y = ((int)h + 10) / 2;
        }
        if (text_x < 2) text_x = 2;

        /* White outline for readability over fill */
        XSetForeground(dpy, progress_bar_gc, bg_px);
        XDrawString(dpy, win, progress_bar_gc,
            text_x - 1, text_y, pct_text, len);
        XDrawString(dpy, win, progress_bar_gc,
            text_x + 1, text_y, pct_text, len);
        XDrawString(dpy, win, progress_bar_gc,
            text_x, text_y - 1, pct_text, len);
        XDrawString(dpy, win, progress_bar_gc,
            text_x, text_y + 1, pct_text, len);

        /* Black text on top */
        XSetForeground(dpy, progress_bar_gc, fg_px);
        XDrawString(dpy, win, progress_bar_gc,
            text_x, text_y, pct_text, len);
    }
}

static void progress_da_expose_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    draw_progress_bar();
}

static void set_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    progress_pct = pct;
    draw_progress_bar();
}

static void progress_close_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;

    if (progress_running) {
        /* Kill the child process */
        if (progress_child_pid > 0) {
            kill(progress_child_pid, SIGTERM);
            waitpid(progress_child_pid, NULL, WNOHANG);
        }
        if (progress_input_id) {
            XtRemoveInput(progress_input_id);
            progress_input_id = 0;
        }
        if (progress_pipe_fd >= 0) {
            close(progress_pipe_fd);
            progress_pipe_fd = -1;
        }
        progress_running = 0;
    }

    XtPopdown(progress_dialog);

    /* Reload installed list */
    pkgdb_load_installed(g_db);
    pkgdb_load_spool(g_db, "/var/spool/pkg");
    populate_list();
}

static void progress_input_cb(XtPointer client, int *fd, XtInputId *id)
{
    char buf[512];
    int n;

    (void)client; (void)id;

    n = read(*fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        progress_dialog_append(buf);

        /* Try to parse progress info from download output */
        {
            char *pct_str;
            pct_str = strstr(buf, "downloading...");
            if (pct_str) {
                char *paren = strstr(pct_str, "(");
                if (paren) {
                    int pct = atoi(paren + 1);
                    if (pct > 0 && pct <= 100) {
                        set_progress(pct);
                    }
                }
            }
            /* Check for common progress markers */
            if (strstr(buf, "Decompressing"))
                set_progress(60);
            if (strstr(buf, "Running pkgadd"))
                set_progress(70);
            if (strstr(buf, "installed successfully"))
                set_progress(100);
        }
        XmUpdateDisplay(progress_dialog);
    } else {
        /* EOF or error: child finished */
        int status = 0;
        if (progress_child_pid > 0) {
            waitpid(progress_child_pid, &status, 0);
        }
        XtRemoveInput(progress_input_id);
        progress_input_id = 0;
        close(progress_pipe_fd);
        progress_pipe_fd = -1;
        progress_running = 0;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            progress_dialog_append("\n--- Operation completed successfully ---\n");
            set_progress(100);
        } else {
            progress_dialog_append("\n--- Operation failed ---\n");
        }

        /* Enable close button */
        XtSetSensitive(progress_close_btn, True);
        XmUpdateDisplay(progress_dialog);
    }
}

static void create_progress_dialog(void)
{
    Widget form, shell;
    Arg args[20];
    int n;

    shell = XtVaCreatePopupShell("Install Progress",
        topLevelShellWidgetClass, top_shell,
        XmNtitle, "Install Progress",
        XmNwidth, 520,
        XmNheight, 420,
        XmNdeleteResponse, XmDO_NOTHING,
        NULL);

    progress_dialog = shell;

    /* Handle window manager close button */
    {
        Atom wm_delete;
        wm_delete = XInternAtom(XtDisplay(top_shell),
            "WM_DELETE_WINDOW", False);
        XmAddWMProtocolCallback(shell, wm_delete,
            progress_close_cb, NULL);
    }

    form = XtVaCreateManagedWidget("progForm",
        xmFormWidgetClass, shell,
        NULL);

    /* Progress bar (custom drawn with XmFrame border) */
    {
        Widget frame;
        frame = XtVaCreateManagedWidget("progressFrame",
            xmFrameWidgetClass, form,
            XmNshadowType, XmSHADOW_IN,
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNtopOffset, 8,
            XmNleftOffset, 8,
            XmNrightOffset, 8,
            NULL);
        progress_da_w = XtVaCreateManagedWidget("progressBar",
            xmDrawingAreaWidgetClass, frame,
            XmNheight, 22,
            NULL);
        XtAddCallback(progress_da_w, XmNexposeCallback,
                      progress_da_expose_cb, NULL);
    }

    /* Close button at bottom */
    progress_close_btn = XtVaCreateManagedWidget("Close",
        xmPushButtonWidgetClass, form,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomOffset, 8,
        XmNrightOffset, 8,
        NULL);
    XtAddCallback(progress_close_btn, XmNactivateCallback,
                  progress_close_cb, NULL);

    /* Scrolled text area for output */
    n = 0;
    XtSetArg(args[n], XmNeditable, False); n++;
    XtSetArg(args[n], XmNeditMode, XmMULTI_LINE_EDIT); n++;
    XtSetArg(args[n], XmNwordWrap, True); n++;
    XtSetArg(args[n], XmNscrollHorizontal, False); n++;
    XtSetArg(args[n], XmNrows, 15); n++;
    XtSetArg(args[n], XmNcolumns, 60); n++;
    progress_text_w = XmCreateScrolledText(form, "progText", args, n);
    XtVaSetValues(XtParent(progress_text_w),
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, XtParent(progress_da_w),
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, progress_close_btn,
        XmNtopOffset, 4,
        XmNleftOffset, 4,
        XmNrightOffset, 4,
        XmNbottomOffset, 4,
        NULL);
    XtManageChild(progress_text_w);
}

/* Launch a command in the progress dialog */
static void run_in_progress_dialog(const char *title, const char *cmd)
{
    int pipefd[2];
    pid_t pid;

    if (!progress_dialog) {
        create_progress_dialog();
    }

    /* Reset dialog state */
    XmTextSetString(progress_text_w, "");
    progress_pct = 0;
    XtSetSensitive(progress_close_btn, False);

    /* Update title */
    XtVaSetValues(progress_dialog, XmNtitle, title, NULL);

    XtPopup(progress_dialog, XtGrabNone);
    XmUpdateDisplay(progress_dialog);

    /* Create pipe and fork */
    if (pipe(pipefd) < 0) {
        progress_dialog_append("Error: pipe() failed\n");
        XtSetSensitive(progress_close_btn, True);
        return;
    }

    pid = fork();
    if (pid < 0) {
        progress_dialog_append("Error: fork() failed\n");
        close(pipefd[0]);
        close(pipefd[1]);
        XtSetSensitive(progress_close_btn, True);
        return;
    }

    if (pid == 0) {
        /* Child process */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);

    /* Set non-blocking on read end */
    {
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, flags);  /* keep blocking for Xt input */
    }

    progress_pipe_fd = pipefd[0];
    progress_child_pid = pid;
    progress_running = 1;

    /* Register Xt input callback for pipe */
    progress_input_id = XtAppAddInput(app_context, pipefd[0],
        (XtPointer)XtInputReadMask,
        progress_input_cb, NULL);
}

/* ================================================================
 * REPOSITORY DIALOG
 * ================================================================ */

static Widget repo_dialog;
static Widget repo_toggles[MAX_REPOS];

static void repo_dialog_ok_cb(Widget w, XtPointer client, XtPointer call)
{
    int i;
    (void)w; (void)client; (void)call;

    /* Read toggle states back into config */
    for (i = 0; i < g_config.repo_count; i++) {
        Boolean set = False;
        XtVaGetValues(repo_toggles[i], XmNset, &set, NULL);
        g_config.repos[i].enabled = set ? 1 : 0;
    }

    /* Save config to disk */
    config_save(&g_config);
    set_status("Repository settings saved.");
    XtPopdown(repo_dialog);
}

static void repo_dialog_cancel_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    XtPopdown(repo_dialog);
}

static void show_repo_dialog(void)
{
    int i;
    Widget form, btn_row, ok_btn, cancel_btn, rc;
    char label[256];

    if (repo_dialog) {
        /* Update toggle states */
        for (i = 0; i < g_config.repo_count; i++) {
            XtVaSetValues(repo_toggles[i],
                XmNset, g_config.repos[i].enabled ? True : False,
                NULL);
        }
        XtPopup(repo_dialog, XtGrabNone);
        return;
    }

    /* Create popup shell for repo dialog */
    {
        Widget shell;
        shell = XtVaCreatePopupShell("Repositories",
            topLevelShellWidgetClass, top_shell,
            XmNtitle, "Repository Settings",
            XmNwidth, 400,
            XmNheight, 250,
            XmNdeleteResponse, XmUNMAP,
            NULL);

        form = XtVaCreateManagedWidget("repoForm",
            xmFormWidgetClass, shell,
            NULL);

        repo_dialog = shell;
    }

    /* Heading label */
    XtVaCreateManagedWidget("Enable or disable package repositories:",
        xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNtopOffset, 8,
        XmNleftOffset, 8,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);

    /* Button row at bottom */
    btn_row = XtVaCreateManagedWidget("repoBtnRow",
        xmRowColumnWidgetClass, form,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomOffset, 8,
        XmNrightOffset, 8,
        NULL);

    ok_btn = XtVaCreateManagedWidget("OK",
        xmPushButtonWidgetClass, btn_row, NULL);
    XtAddCallback(ok_btn, XmNactivateCallback, repo_dialog_ok_cb, NULL);

    cancel_btn = XtVaCreateManagedWidget("Cancel",
        xmPushButtonWidgetClass, btn_row, NULL);
    XtAddCallback(cancel_btn, XmNactivateCallback, repo_dialog_cancel_cb, NULL);

    /* Scrolled window with toggle buttons */
    rc = XtVaCreateManagedWidget("repoRC",
        xmRowColumnWidgetClass, form,
        XmNtopAttachment, XmATTACH_POSITION,
        XmNtopPosition, 15,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, btn_row,
        XmNleftOffset, 12,
        XmNrightOffset, 12,
        XmNbottomOffset, 4,
        NULL);

    for (i = 0; i < g_config.repo_count; i++) {
        const repo_def_t *r = &g_config.repos[i];
        snprintf(label, sizeof(label), "%s  (%s)",
            r->display_name,
            r->type == REPO_TYPE_TGCWARE ? "TGCware" : "GitHub");
        repo_toggles[i] = XtVaCreateManagedWidget(label,
            xmToggleButtonWidgetClass, rc,
            XmNset, r->enabled ? True : False,
            NULL);
    }

    XtPopup(repo_dialog, XtGrabNone);
}

/* ================================================================
 * STATUS BAR
 * ================================================================ */

static void set_status(const char *msg)
{
    XmString xs;

    xs = XmStringCreateLocalized((char *)msg);
    XtVaSetValues(status_label, XmNlabelString, xs, NULL);
    XmStringFree(xs);
}

/* Word-wrap long text in-place at ~wrap_col chars on word boundaries */
static void word_wrap(char *text, int wrap_col)
{
    int col = 0;
    char *p;
    for (p = text; *p; p++) {
        if (*p == '\n') { col = 0; continue; }
        col++;
        if (col >= wrap_col && (*p == ' ' || *p == ',')) {
            if (*p == ',') { p++; col++; }
            if (*p == ' ' || *p == '\0') {
                *p = '\n';
                col = 0;
            }
        }
    }
}

/* ================================================================
 * LOG / INFO PANEL
 * ================================================================ */

static void clear_info(void)
{
    XmTextSetString(info_text_w, "");
}

static void append_log(const char *text)
{
    XmTextPosition pos;

    pos = XmTextGetLastPosition(info_text_w);
    XmTextInsert(info_text_w, pos, (char *)text);
    XmTextShowPosition(info_text_w, XmTextGetLastPosition(info_text_w));
}

static void update_action_buttons(void);
static void populate_version_dropdown(const char *pkg_name, int default_avail_idx);

static void show_pkg_info(int avail_idx)
{
    const avail_pkg_t *p;
    char buf[2048];

    clear_info();

    /* Reset selection state */
    sel_is_installed = 0;
    sel_inst_idx = -1;
    sel_is_avail = 0;
    sel_has_upgrade = 0;
    sel_nversions = 0;
    selected_version_idx = -1;

    /* Handle installed-only packages (negative index encoding) */
    if (avail_idx < 0) {
        int inst_idx = -(avail_idx + 1);
        if (inst_idx >= 0 && inst_idx < g_db->inst_count) {
            const installed_pkg_t *ip = &g_db->installed[inst_idx];
            sel_is_installed = 1;
            sel_inst_idx = inst_idx;
            sel_is_avail = 0;
            snprintf(buf, sizeof(buf),
                "Package: %s\n"
                "Version: %s\n"
                "SVR4 Code: %s\n"
                "Repository: %s\n"
                "\n"
                "--- Installed ---\n"
                "Installed: %s\n"
                "Cached File: %s\n"
                "\n"
                "Note: This package is installed but not in the\n"
                "current repository index.\n",
                ip->name, ip->version, ip->pkg_code,
                ip->repo_name, ip->install_date, ip->pkg_file);
            append_log(buf);
            populate_version_dropdown(NULL, -1);
        }
        update_action_buttons();
        return;
    }

    if (avail_idx >= g_db->avail_count) {
        update_action_buttons();
        return;
    }

    p = &g_db->available[avail_idx];
    sel_is_avail = 1;

    /* Check install state */
    sel_inst_idx = pkgdb_find_installed(g_db, p->name);
    sel_is_installed = (sel_inst_idx >= 0) ? 1 : 0;

    /* Find all versions */
    sel_nversions = pkgdb_find_all_versions(g_db, p->name,
                                             sel_ver_list, 64);

    /* Check if upgrade available */
    if (sel_is_installed && sel_nversions > 0) {
        const avail_pkg_t *latest = &g_db->available[sel_ver_list[0]];
        const char *inst_ver = g_db->installed[sel_inst_idx].version;
        if (strcmp(latest->version, inst_ver) != 0)
            sel_has_upgrade = 1;
    }

    /* Default version selection: if installed, match installed version;
     * otherwise use the latest (first in list) */
    selected_version_idx = avail_idx;
    if (sel_is_installed && sel_nversions > 0) {
        int vi;
        const char *inst_ver = g_db->installed[sel_inst_idx].version;
        for (vi = 0; vi < sel_nversions; vi++) {
            const avail_pkg_t *vp = &g_db->available[sel_ver_list[vi]];
            if (strcmp(vp->version, inst_ver) == 0) {
                selected_version_idx = sel_ver_list[vi];
                break;
            }
        }
    }

    snprintf(buf, sizeof(buf),
        "Package: %s\n"
        "Version: %s-%d\n"
        "Architecture: %s\n"
        "SVR4 Code: %s\n"
        "Repository: %s (%s)\n"
        "Size: %s\n"
        "\n"
        "Description:\n"
        "  %s\n"
        "\n"
        "Download URL:\n"
        "  %s\n",
        p->name, p->version, p->release,
        p->arch, p->pkg_code,
        p->repo_name,
        p->source_type == SRC_TGCWARE ? "TGCware" : "GitHub",
        p->size_str[0] ? p->size_str : "unknown",
        p->description[0] ? p->description : "(none)",
        p->download_url);

    append_log(buf);

    /* Resolve dependencies to human-readable names */
    append_log("\nDependencies:\n");
    if (p->deps[0]) {
        char deps_copy[512];
        char *tok, *saveptr;
        strncpy(deps_copy, p->deps, sizeof(deps_copy) - 1);
        deps_copy[sizeof(deps_copy) - 1] = '\0';
        tok = strtok_r(deps_copy, ",", &saveptr);
        while (tok) {
            int dep_idx;
            char dep_line[256];
            while (*tok == ' ') tok++;

            /* Skip self-reference (TGCware index includes pkg's own code) */
            if (strcmp(tok, p->pkg_code) == 0) {
                tok = strtok_r(NULL, ",", &saveptr);
                continue;
            }
            dep_idx = pkgdb_find_by_code(g_db, tok);
            if (dep_idx >= 0) {
                const avail_pkg_t *d = &g_db->available[dep_idx];
                int is_inst = pkgdb_sys_installed(tok);
                snprintf(dep_line, sizeof(dep_line),
                    "  %s %s [%s]%s\n",
                    d->name, d->version, tok,
                    is_inst ? " (installed)" : "");
            } else {
                int is_inst = pkgdb_sys_installed(tok);
                snprintf(dep_line, sizeof(dep_line),
                    "  %s%s\n", tok,
                    is_inst ? " (installed)" : " (not found)");
            }
            append_log(dep_line);
            tok = strtok_r(NULL, ",", &saveptr);
        }
    } else {
        append_log("  (none)\n");
    }

    /* Check if installed */
    if (sel_is_installed) {
        snprintf(buf, sizeof(buf),
            "\n--- Installed ---\n"
            "Version: %s\n"
            "Installed: %s\n"
            "SVR4 Code: %s\n",
            g_db->installed[sel_inst_idx].version,
            g_db->installed[sel_inst_idx].install_date,
            g_db->installed[sel_inst_idx].pkg_code);
        append_log(buf);
    } else {
        append_log("\nStatus: Not installed\n");
    }

    /* List all available versions */
    if (sel_nversions > 1) {
        int vi;
        append_log("\nAvailable Versions:\n");
        for (vi = 0; vi < sel_nversions; vi++) {
            const avail_pkg_t *vp = &g_db->available[sel_ver_list[vi]];
            int ver_inst = pkgdb_avail_is_installed(g_db, sel_ver_list[vi]);
            snprintf(buf, sizeof(buf), "  %s-%d  [%s]%s%s\n",
                vp->version, vp->release,
                vp->pkg_code,
                ver_inst ? " (installed)" : "",
                sel_ver_list[vi] == avail_idx ? " *latest" : "");
            append_log(buf);
        }
    }

    /* Populate version dropdown */
    populate_version_dropdown(p->name, selected_version_idx);

    /* Update button sensitivity */
    update_action_buttons();
}

/* ================================================================
 * VERSION DROPDOWN & BUTTON STATE
 * ================================================================ */

/* Count how many installed packages have a newer version available */
static int count_upgradable(void)
{
    int count = 0;
    int i;
    for (i = 0; i < g_db->inst_count; i++) {
        const installed_pkg_t *ip = &g_db->installed[i];
        int avail = pkgdb_find_avail(g_db, ip->name);
        if (avail >= 0) {
            const avail_pkg_t *ap = &g_db->available[avail];
            if (strcmp(ap->version, ip->version) != 0)
                count++;
        }
    }
    return count;
}

/* Version dropdown selection callback */
static void version_select_cb(Widget w, XtPointer client, XtPointer call)
{
    int idx = (int)(long)client;
    (void)w; (void)call;

    if (idx >= 0 && idx < version_count) {
        selected_version_idx = version_indices[idx];
    }
}

/* Populate the version dropdown for the selected package */
static void populate_version_dropdown(const char *pkg_name, int default_avail_idx)
{
    int i;
    int default_btn = 0;

    /* Destroy old dropdown buttons */
    for (i = 0; i < version_count; i++) {
        if (version_buttons[i])
            XtDestroyWidget(version_buttons[i]);
        version_buttons[i] = NULL;
    }
    version_count = 0;

    if (!pkg_name || !version_pulldown) {
        /* No versions — hide/desensitize dropdown */
        if (version_option_menu)
            XtSetSensitive(version_option_menu, False);
        return;
    }

    /* Find all versions */
    {
        int ver_list[64];
        int nver = pkgdb_find_all_versions(g_db, pkg_name, ver_list, 64);
        if (nver <= 0) {
            if (version_option_menu)
                XtSetSensitive(version_option_menu, False);
            return;
        }

        for (i = 0; i < nver && i < 64; i++) {
            const avail_pkg_t *vp = &g_db->available[ver_list[i]];
            char label[128];
            XmString xs;

            snprintf(label, sizeof(label), "%s-%d  (%s)",
                     vp->version, vp->release, vp->repo_name);

            xs = XmStringCreateLocalized(label);
            version_buttons[i] = XtVaCreateManagedWidget(label,
                xmPushButtonWidgetClass, version_pulldown,
                XmNlabelString, xs,
                NULL);
            XmStringFree(xs);

            XtAddCallback(version_buttons[i], XmNactivateCallback,
                          version_select_cb, (XtPointer)(long)i);

            version_indices[i] = ver_list[i];
            if (ver_list[i] == default_avail_idx)
                default_btn = i;
        }
        version_count = (nver < 64) ? nver : 64;
    }

    /* Set the default selection */
    if (version_count > 0 && version_option_menu) {
        XtVaSetValues(version_option_menu,
            XmNmenuHistory, version_buttons[default_btn],
            NULL);
        XtSetSensitive(version_option_menu, (version_count > 1) ? True : False);
        selected_version_idx = version_indices[default_btn];
    }
}

/* Update action button sensitivity based on selection state */
static void update_action_buttons(void)
{
    int upgradable;

    if (g_selected < 0) {
        /* Nothing selected — disable all per-package buttons */
        if (install_btn) XtSetSensitive(install_btn, False);
        if (remove_btn) XtSetSensitive(remove_btn, False);
        if (upgrade_btn) XtSetSensitive(upgrade_btn, False);
        if (deps_btn_w) XtSetSensitive(deps_btn_w, False);
        if (version_option_menu) XtSetSensitive(version_option_menu, False);
    } else {
        /* Install: available in repo (even if installed — re-install) */
        if (install_btn)
            XtSetSensitive(install_btn, sel_is_avail ? True : False);

        /* Remove: only if installed */
        if (remove_btn)
            XtSetSensitive(remove_btn, sel_is_installed ? True : False);

        /* Upgrade: only if installed AND newer version exists */
        if (upgrade_btn)
            XtSetSensitive(upgrade_btn,
                (sel_is_installed && sel_has_upgrade) ? True : False);

        /* Deps: available in repo */
        if (deps_btn_w)
            XtSetSensitive(deps_btn_w, sel_is_avail ? True : False);
    }

    /* Upgrade All: visible when >1 package has an upgrade */
    upgradable = count_upgradable();
    if (upgrade_all_btn) {
        if (upgradable > 1) {
            char label[64];
            XmString xs;
            snprintf(label, sizeof(label), "Upgrade All (%d)", upgradable);
            xs = XmStringCreateLocalized(label);
            XtVaSetValues(upgrade_all_btn, XmNlabelString, xs, NULL);
            XmStringFree(xs);
            XtSetSensitive(upgrade_all_btn, True);
            XtManageChild(upgrade_all_btn);
        } else {
            XtUnmanageChild(upgrade_all_btn);
        }
    }
}

/* ================================================================
 * PACKAGE LIST POPULATION
 * ================================================================ */

static void populate_list(void)
{
    XmString *items;
    char label[256];
    const char *search_term;
    char search_buf[128];
    int i, n;

    /* Get search term */
    {
        char *s;
        s = XmTextFieldGetString(search_tf);
        strncpy(search_buf, s ? s : "", sizeof(search_buf) - 1);
        search_buf[sizeof(search_buf) - 1] = '\0';
        if (s) XtFree(s);
    }
    search_term = search_buf[0] ? search_buf : NULL;

    /* Free old mapping */
    if (vis_map) {
        free(vis_map);
        vis_map = NULL;
    }
    vis_count = 0;

    /* Count maximum entries */
    n = g_db->avail_count + g_db->inst_count;
    vis_map = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
    items = (XmString *)malloc(sizeof(XmString) * (n > 0 ? n : 1));

    /* Build the visible list from available packages (one entry per name) */
    for (i = 0; i < g_db->avail_count; i++) {
        const avail_pkg_t *p = &g_db->available[i];
        int is_inst;
        int nversions;
        const char *status_str;

        /* Version rollup: only show latest version of each package name */
        if (!pkgdb_is_latest_version(g_db, i))
            continue;

        /* Search filter */
        if (search_term) {
            if (!strcasestr(p->name, search_term) &&
                !strcasestr(p->description, search_term))
                continue;
        }

        is_inst = pkgdb_any_version_installed(g_db, p->name);

        /* Filter mode */
        if (g_filter == FILTER_INSTALLED && !is_inst)
            continue;
        if (g_filter == FILTER_AVAILABLE && is_inst)
            continue;

        /* Count how many versions exist */
        {
            int vi;
            nversions = 0;
            for (vi = 0; vi < g_db->avail_count; vi++) {
                if (strcasecmp(g_db->available[vi].name, p->name) == 0)
                    nversions++;
            }
        }

        status_str = is_inst ? "[inst]" : "      ";

        if (nversions > 1) {
            snprintf(label, sizeof(label), "%s %-28.28s  %-14.14s  %s (%d ver)",
                status_str, p->name, p->version, p->repo_name, nversions);
        } else {
            snprintf(label, sizeof(label), "%s %-28.28s  %-14.14s  %s",
                status_str, p->name, p->version, p->repo_name);
        }

        items[vis_count] = XmStringCreateLocalized(label);
        vis_map[vis_count] = i;
        vis_count++;
    }

    /* Also show installed-only packages (not in available index) */
    if (g_filter != FILTER_AVAILABLE) {
        for (i = 0; i < g_db->inst_count; i++) {
            const installed_pkg_t *ip = &g_db->installed[i];

            /* Skip if it already appeared in the available list (by code or name) */
            if (pkgdb_find_avail(g_db, ip->name) >= 0)
                continue;
            if (ip->pkg_code[0] && pkgdb_find_by_code(g_db, ip->pkg_code) >= 0)
                continue;

            /* Search filter */
            if (search_term) {
                if (!strcasestr(ip->name, search_term))
                    continue;
            }

            snprintf(label, sizeof(label), "[inst] %-28.28s  %-14.14s  %s",
                ip->name, ip->version, ip->repo_name);

            items[vis_count] = XmStringCreateLocalized(label);
            /* Encode installed-only index as -(inst_idx+1) */
            vis_map[vis_count] = -(i + 1);
            vis_count++;
        }
    }

    /* Update the list widget */
    XmListDeleteAllItems(pkg_list_w);
    if (vis_count > 0) {
        XmListAddItems(pkg_list_w, items, vis_count, 0);
    }

    /* Free XmStrings */
    for (i = 0; i < vis_count; i++) {
        XmStringFree(items[i]);
    }
    free(items);

    /* Update status */
    {
        char sbuf[128];
        snprintf(sbuf, sizeof(sbuf), "%d packages shown (%d total versions)",
                 vis_count, g_db->avail_count);
        set_status(sbuf);
    }

    g_selected = -1;
    clear_info();
}

/* ================================================================
 * CONFIRMATION DIALOG HELPERS
 * ================================================================ */

/*
 * Show a blocking Yes/No confirmation dialog.
 * Returns 1 if user clicked OK/Yes, 0 if Cancel.
 */
static int g_confirm_result;

static void confirm_ok_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)call;
    g_confirm_result = 1;
    XtDestroyWidget((Widget)client);
}

static void confirm_cancel_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)call;
    g_confirm_result = 0;
    XtDestroyWidget((Widget)client);
}

static int show_confirm_dialog(const char *title, const char *message)
{
    Widget dialog;
    XmString msg_xs, title_xs, ok_xs, cancel_xs;
    XEvent event;

    g_confirm_result = -1;

    msg_xs = XmStringCreateLocalized((char *)message);
    title_xs = XmStringCreateLocalized((char *)title);
    ok_xs = XmStringCreateLocalized("OK");
    cancel_xs = XmStringCreateLocalized("Cancel");

    dialog = XmCreateQuestionDialog(top_shell, "confirmDialog", NULL, 0);
    XtVaSetValues(dialog,
        XmNmessageString, msg_xs,
        XmNdialogTitle, title_xs,
        XmNokLabelString, ok_xs,
        XmNcancelLabelString, cancel_xs,
        XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL,
        NULL);

    XmStringFree(msg_xs);
    XmStringFree(title_xs);
    XmStringFree(ok_xs);
    XmStringFree(cancel_xs);

    /* Hide Help button */
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));

    XtAddCallback(dialog, XmNokCallback,
                  confirm_ok_cb, (XtPointer)dialog);
    XtAddCallback(dialog, XmNcancelCallback,
                  confirm_cancel_cb, (XtPointer)dialog);

    XtManageChild(dialog);

    /* Run a mini event loop until the dialog is dismissed */
    while (g_confirm_result == -1) {
        XtAppNextEvent(app_context, &event);
        XtDispatchEvent(&event);
    }

    return g_confirm_result;
}

/* Build a dependency list string for a package code */
static void get_reverse_deps(const char *pkg_code, char *out, int out_size)
{
    int i;
    char *p = out;
    int remaining = out_size;
    int first = 1;

    out[0] = '\0';
    for (i = 0; i < g_db->avail_count; i++) {
        const avail_pkg_t *ap = &g_db->available[i];
        if (!pkgdb_is_latest_version(g_db, i))
            continue;
        if (ap->deps[0] && strstr(ap->deps, pkg_code)) {
            /* Verify it's an actual token match, not a substring */
            char deps_copy[512];
            char *tok, *saveptr;
            strncpy(deps_copy, ap->deps, sizeof(deps_copy) - 1);
            deps_copy[sizeof(deps_copy) - 1] = '\0';
            tok = strtok_r(deps_copy, ",", &saveptr);
            while (tok) {
                while (*tok == ' ') tok++;
                if (strcmp(tok, pkg_code) == 0) {
                    int n = snprintf(p, remaining, "%s%s",
                                     first ? "" : ", ", ap->name);
                    if (n > 0 && n < remaining) {
                        p += n;
                        remaining -= n;
                    }
                    first = 0;
                    break;
                }
                tok = strtok_r(NULL, ",", &saveptr);
            }
        }
    }
}

/* ================================================================
 * CALLBACKS
 * ================================================================ */

/* Package list selection */
static void pkg_select_cb(Widget w, XtPointer client, XtPointer call)
{
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call;
    int pos;

    (void)w; (void)client;

    pos = cbs->item_position - 1;  /* Motif is 1-based */
    if (pos >= 0 && pos < vis_count) {
        g_selected = pos;
        show_pkg_info(vis_map[pos]);
    }
}

/* Search field activation (Enter key) */
static void search_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    populate_list();
}

/* Filter radio buttons - only act when the toggle is being SET */
static void filter_all_cb(Widget w, XtPointer client, XtPointer call)
{
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    (void)w; (void)client;
    if (!cbs->set) return;  /* ignore deselect events */
    g_filter = FILTER_ALL;
    populate_list();
}

static void filter_inst_cb(Widget w, XtPointer client, XtPointer call)
{
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    (void)w; (void)client;
    if (!cbs->set) return;
    g_filter = FILTER_INSTALLED;
    populate_list();
}

static void filter_avail_cb(Widget w, XtPointer client, XtPointer call)
{
    XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;
    (void)w; (void)client;
    if (!cbs->set) return;
    g_filter = FILTER_AVAILABLE;
    populate_list();
}

/* Refresh / Update index */
static void update_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;

    set_status("Updating package index...");
    XmUpdateDisplay(top_shell);

    clear_info();
    append_log("Refreshing package index from all repositories...\n\n");

    /* Run update in progress dialog (non-blocking with output streaming) */
    run_in_progress_dialog("Updating Package Index", "spm update 2>&1");

    /* Reload the database */
    pkgdb_free(g_db);
    g_db = pkgdb_new();
    pkgdb_load_index(g_db);
    pkgdb_load_installed(g_db);
    pkgdb_load_spool(g_db, "/var/spool/pkg");

    populate_list();
    set_status("Index updated.");
}

/* Install selected package (remove first if installed, then re-add) */
static void install_cb(Widget w, XtPointer client, XtPointer call)
{
    const avail_pkg_t *p;
    char cmd[2048];
    char title[256];
    char msg[1024];
    int use_idx;

    (void)w; (void)client; (void)call;

    if (g_selected < 0 || g_selected >= vis_count) {
        set_status("No package selected.");
        return;
    }

    if (!sel_is_avail) {
        set_status("Package not in repository. Cannot install.");
        return;
    }

    /* Use the version from the dropdown */
    use_idx = (selected_version_idx >= 0) ? selected_version_idx : vis_map[g_selected];
    if (use_idx < 0 || use_idx >= g_db->avail_count) {
        set_status("Invalid version selection.");
        return;
    }

    p = &g_db->available[use_idx];

    /* Check if this is an older version (not the latest) */
    if (sel_is_installed || sel_nversions > 1) {
        int is_latest = pkgdb_is_latest_version(g_db, use_idx);
        int is_downgrade = 0;

        /* Determine if installing an older version than what's installed */
        if (sel_is_installed) {
            const char *inst_ver = g_db->installed[sel_inst_idx].version;
            /* Simple: if it's not the latest AND different from installed */
            if (!is_latest && strcmp(p->version, inst_ver) != 0)
                is_downgrade = 1;
        } else if (!is_latest) {
            is_downgrade = 1;
        }

        if (is_downgrade) {
            /* Build reverse dependency list */
            char rdeps[512];
            get_reverse_deps(p->pkg_code, rdeps, sizeof(rdeps));

            if (rdeps[0]) {
                snprintf(msg, sizeof(msg),
                    "You are installing an older version of %s (%s)\n"
                    "that may be incompatible with other dependencies.\n\n"
                    "Dependent packages: %s\n\n"
                    "Continue?",
                    p->name, p->version, rdeps);
            } else {
                snprintf(msg, sizeof(msg),
                    "You are installing an older version of %s (%s)\n"
                    "that may be incompatible with other dependencies.\n\n"
                    "Continue?",
                    p->name, p->version);
            }

            if (!show_confirm_dialog("Confirm Install", msg)) {
                set_status("Install cancelled.");
                return;
            }
        } else if (sel_is_installed) {
            /* Re-installing same or newer — still confirm */
            snprintf(msg, sizeof(msg),
                "Re-install %s version %s?\n"
                "The current installation will be removed first.",
                p->name, p->version);
            if (!show_confirm_dialog("Confirm Re-install", msg)) {
                set_status("Install cancelled.");
                return;
            }
        }
    }

    /* Build the command: remove first if installed, then install */
    if (sel_is_installed) {
        const char *pkg_code = g_db->installed[sel_inst_idx].pkg_code;
        snprintf(title, sizeof(title), "Re-installing %s %s", p->name, p->version);
        snprintf(cmd, sizeof(cmd),
            "echo 'Removing current installation...'\n"
            "spm remove %s 2>&1\n"
            "echo ''\n"
            "echo 'Installing %s %s...'\n"
            "spm install %s 2>&1",
            p->name, p->name, p->version, p->name);
    } else {
        snprintf(title, sizeof(title), "Installing %s %s", p->name, p->version);
        snprintf(cmd, sizeof(cmd), "spm install %s 2>&1", p->name);
    }

    set_status(title);
    run_in_progress_dialog(title, cmd);
}

/* Remove selected package */
static void remove_cb(Widget w, XtPointer client, XtPointer call)
{
    char cmd[1024];
    char title[256];
    int map_val;
    const char *pkg_name;

    (void)w; (void)client; (void)call;

    if (g_selected < 0 || g_selected >= vis_count) {
        set_status("No package selected.");
        return;
    }

    map_val = vis_map[g_selected];

    /* Get package name whether from available or installed-only */
    if (map_val < 0) {
        int inst_idx = -(map_val + 1);
        pkg_name = g_db->installed[inst_idx].name;
    } else {
        pkg_name = g_db->available[map_val].name;
    }

    if (!sel_is_installed) {
        set_status("Package not installed.");
        return;
    }

    snprintf(title, sizeof(title), "Removing %s", pkg_name);
    snprintf(cmd, sizeof(cmd), "spm remove %s 2>&1", pkg_name);

    set_status(title);
    run_in_progress_dialog(title, cmd);
}

/* Upgrade selected package (remove old, install latest) */
static void upgrade_cb(Widget w, XtPointer client, XtPointer call)
{
    char cmd[2048];
    char title[256];
    const char *uname;
    const avail_pkg_t *latest;
    int avail;

    (void)w; (void)client; (void)call;

    if (g_selected < 0 || g_selected >= vis_count) {
        set_status("No package selected.");
        return;
    }

    if (!sel_is_installed || !sel_has_upgrade) {
        set_status("No upgrade available.");
        return;
    }

    if (vis_map[g_selected] < 0) {
        int inst_idx = -(vis_map[g_selected] + 1);
        uname = g_db->installed[inst_idx].name;
    } else {
        uname = g_db->available[vis_map[g_selected]].name;
    }

    avail = pkgdb_find_avail(g_db, uname);
    if (avail < 0) {
        set_status("Package not in repository.");
        return;
    }
    latest = &g_db->available[avail];

    snprintf(title, sizeof(title), "Upgrading %s to %s",
             uname, latest->version);

    /* Remove then install (upgrade = remove old + install latest) */
    snprintf(cmd, sizeof(cmd),
        "echo 'Removing %s...'\n"
        "spm remove %s 2>&1\n"
        "echo ''\n"
        "echo 'Installing %s %s...'\n"
        "spm install %s 2>&1",
        uname, uname, uname, latest->version, uname);

    set_status(title);
    run_in_progress_dialog(title, cmd);
}

/* Upgrade all packages */
static void upgrade_all_cb(Widget w, XtPointer client, XtPointer call)
{
    char cmd[4096];
    char title[128];
    int n;

    (void)w; (void)client; (void)call;

    n = count_upgradable();
    if (n == 0) {
        set_status("All packages up to date.");
        return;
    }

    snprintf(title, sizeof(title), "Upgrading %d packages", n);

    /* Build a command that upgrades all via spm CLI */
    snprintf(cmd, sizeof(cmd), "spm upgrade 2>&1");

    set_status(title);
    run_in_progress_dialog(title, cmd);
}

/* Install entire distribution (all available SST packages) */
static void install_distro_cb(Widget w, XtPointer client, XtPointer call)
{
    char cmd[8192];
    int pos, i, count;

    (void)w; (void)client; (void)call;

    if (!show_confirm_dialog("Install Entire Distribution",
            "Install all available Sunstorm (SST) packages?\n\n"
            "This may take a while and use significant disk space."))
        return;

    /* Build install command for all SST packages */
    pos = snprintf(cmd, sizeof(cmd), "spm install");
    count = 0;
    for (i = 0; i < g_db->avail_count && pos < (int)sizeof(cmd) - 64; i++) {
        const avail_pkg_t *p = &g_db->available[i];
        if (!pkgdb_is_latest_version(g_db, i)) continue;
        if (strncmp(p->pkg_code, "SST", 3) != 0) continue;
        if (pkgdb_avail_is_installed(g_db, i)) continue;
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " %s", p->name);
        count++;
    }
    snprintf(cmd + pos, sizeof(cmd) - pos, " 2>&1");

    if (count == 0) {
        set_status("All Sunstorm packages already installed.");
        return;
    }

    {
        char title[128];
        snprintf(title, sizeof(title), "Installing %d Sunstorm packages", count);
        set_status(title);
        run_in_progress_dialog(title, cmd);
    }
    populate_list();
}

/* Remove entire distribution (all installed SST packages) */
static void remove_distro_cb(Widget w, XtPointer client, XtPointer call)
{
    char cmd[8192];
    int pos, i, count;

    (void)w; (void)client; (void)call;

    if (!show_confirm_dialog("Remove Entire Distribution",
            "Remove all installed Sunstorm (SST) packages?\n\n"
            "This cannot be undone."))
        return;

    pos = snprintf(cmd, sizeof(cmd), "spm remove");
    count = 0;
    for (i = 0; i < g_db->inst_count && pos < (int)sizeof(cmd) - 64; i++) {
        const installed_pkg_t *p = &g_db->installed[i];
        if (strncmp(p->pkg_code, "SST", 3) != 0) continue;
        pos += snprintf(cmd + pos, sizeof(cmd) - pos, " %s", p->name);
        count++;
    }
    snprintf(cmd + pos, sizeof(cmd) - pos, " 2>&1");

    if (count == 0) {
        set_status("No Sunstorm packages installed.");
        return;
    }

    {
        char title[128];
        snprintf(title, sizeof(title), "Removing %d Sunstorm packages", count);
        set_status(title);
        run_in_progress_dialog(title, cmd);
    }
    populate_list();
}

/* Show dependency tree */
static void deps_cb(Widget w, XtPointer client, XtPointer call)
{
    char cmd[1024];
    char line[512];
    FILE *fp;
    const char *pkg_name;
    int map_val;

    (void)w; (void)client; (void)call;

    if (g_selected < 0 || g_selected >= vis_count) {
        set_status("No package selected.");
        return;
    }

    map_val = vis_map[g_selected];

    /* Get package name from available or installed-only */
    if (map_val < 0) {
        int inst_idx = -(map_val + 1);
        if (inst_idx >= 0 && inst_idx < g_db->inst_count)
            pkg_name = g_db->installed[inst_idx].name;
        else {
            set_status("Invalid package selection.");
            return;
        }
    } else {
        pkg_name = g_db->available[map_val].name;
    }

    clear_info();
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Dependency tree for %s:\n\n", pkg_name);
        append_log(buf);
    }

    snprintf(cmd, sizeof(cmd), "spm deps %s 2>&1", pkg_name);
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            append_log(line);
        }
        pclose(fp);
    }

    set_status("Dependency tree shown.");
}

/* Repo list */
static void repo_list_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    show_repo_dialog();
}

/* About dialog */
static void about_cb(Widget w, XtPointer client, XtPointer call)
{
    Widget dialog;
    XmString msg;
    char buf[512];

    (void)w; (void)client; (void)call;

    snprintf(buf, sizeof(buf),
        "spm %s\n\n"
        "Sunstorm Package Manager\n"
        "Motif/CDE GUI\n\n"
        "Package-owned OpenSSL3\n"
        "Indexes configured package repositories",
        SPM_VERSION);

    msg = XmStringCreateLocalized(buf);
    dialog = XmCreateMessageDialog(top_shell, "aboutDialog", NULL, 0);
    XtVaSetValues(dialog,
        XmNmessageString, msg,
        XmNdialogTitle, XmStringCreateLocalized("About spm"),
        NULL);
    XmStringFree(msg);

    /* Hide Cancel and Help buttons */
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));

    XtManageChild(dialog);
}

/* Quit */
static void quit_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    cleanup_and_exit(0);
}

static void cleanup_and_exit(int code)
{
    pkgdb_free(g_db);
    http_shutdown();
    if (vis_map) free(vis_map);
    exit(code);
}

/* ================================================================
 * AGENT STATUS CHECK
 * ================================================================ */

static void check_agent_status(XtPointer client_data, XtIntervalId *id)
{
    FILE *f;
    char line[256];
    struct stat sb;

    (void)client_data;
    (void)id;

    /* Check if agent PID file exists and agent is running */
    if (stat(SPM_AGENT_PID, &sb) == 0) {
        f = fopen(SPM_AGENT_PID, "r");
        if (f) {
            pid_t pid = 0;
            if (fgets(line, sizeof(line), f)) {
                pid = (pid_t)atoi(line);
            }
            fclose(f);

            if (pid > 0 && kill(pid, 0) == 0) {
                /* Agent is running - check for update status */
                if (stat(SPM_UPDATE_STATUS, &sb) == 0) {
                    f = fopen(SPM_UPDATE_STATUS, "r");
                    if (f) {
                        if (fgets(line, sizeof(line), f)) {
                            /* Remove trailing newline */
                            line[strcspn(line, "\n")] = '\0';
                            if (strncmp(line, "updates=", 8) == 0) {
                                int count = atoi(line + 8);
                                if (count > 0) {
                                    char sbuf[128];
                                    snprintf(sbuf, sizeof(sbuf),
                                        "Agent: %d update%s available",
                                        count, count > 1 ? "s" : "");
                                    set_status(sbuf);
                                }
                            }
                        }
                        fclose(f);
                    }
                }
            }
        }
    }

    /* Re-arm timer: check every 30 seconds */
    agent_timer = XtAppAddTimeOut(app_context, 30000,
                                   check_agent_status, NULL);
}

/* ================================================================
 * CREATE UI
 * ================================================================ */

static void create_ui(void)
{
    Widget form, left_pane, right_pane, paned;
    Widget toolbar, search_label;
    Widget server_pulldown, settings_pulldown;
    Widget update_menu_btn, repos_menu_btn, about_menu_btn, quit_menu_btn;
    Widget pkgs_menu, tools_menu;
    Arg args[20];
    int n;

    /* Main window */
    main_window = XtVaCreateManagedWidget("mainWindow",
        xmMainWindowWidgetClass, top_shell,
        NULL);

    /* ---- Menu bar ---- */
    menu_bar = XmCreateMenuBar(main_window, "menuBar", NULL, 0);
    XtManageChild(menu_bar);

    /* Packages menu */
    server_pulldown = XmCreatePulldownMenu(menu_bar, "pkgsPulldown",
                                            NULL, 0);
    pkgs_menu = XtVaCreateManagedWidget("Packages",
        xmCascadeButtonWidgetClass, menu_bar,
        XmNsubMenuId, server_pulldown,
        NULL);

    update_menu_btn = XtVaCreateManagedWidget("Update Index",
        xmPushButtonWidgetClass, server_pulldown, NULL);
    XtAddCallback(update_menu_btn, XmNactivateCallback, update_cb, NULL);

    XtVaCreateManagedWidget("sep1",
        xmSeparatorWidgetClass, server_pulldown, NULL);

    {
        Widget install_distro_btn, remove_distro_btn;
        install_distro_btn = XtVaCreateManagedWidget(
            "Install Entire Distribution...",
            xmPushButtonWidgetClass, server_pulldown, NULL);
        XtAddCallback(install_distro_btn, XmNactivateCallback,
                      install_distro_cb, NULL);
        remove_distro_btn = XtVaCreateManagedWidget(
            "Remove Entire Distribution...",
            xmPushButtonWidgetClass, server_pulldown, NULL);
        XtAddCallback(remove_distro_btn, XmNactivateCallback,
                      remove_distro_cb, NULL);
    }

    XtVaCreateManagedWidget("sep1b",
        xmSeparatorWidgetClass, server_pulldown, NULL);

    quit_menu_btn = XtVaCreateManagedWidget("Quit",
        xmPushButtonWidgetClass, server_pulldown, NULL);
    XtAddCallback(quit_menu_btn, XmNactivateCallback, quit_cb, NULL);

    /* Tools menu */
    settings_pulldown = XmCreatePulldownMenu(menu_bar, "toolsPulldown",
                                              NULL, 0);
    tools_menu = XtVaCreateManagedWidget("Tools",
        xmCascadeButtonWidgetClass, menu_bar,
        XmNsubMenuId, settings_pulldown,
        NULL);

    repos_menu_btn = XtVaCreateManagedWidget("Repositories...",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(repos_menu_btn, XmNactivateCallback, repo_list_cb, NULL);

    XtVaCreateManagedWidget("sep2",
        xmSeparatorWidgetClass, settings_pulldown, NULL);

    about_menu_btn = XtVaCreateManagedWidget("About spm...",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(about_menu_btn, XmNactivateCallback, about_cb, NULL);

    /* ---- Main form ---- */
    form = XtVaCreateManagedWidget("form",
        xmFormWidgetClass, main_window,
        NULL);

    /* ---- Search / filter toolbar ---- */
    toolbar = XtVaCreateManagedWidget("toolbar",
        xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);

    search_label = XtVaCreateManagedWidget("Search:",
        xmLabelWidgetClass, toolbar,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftOffset, 4,
        NULL);

    search_tf = XtVaCreateManagedWidget("searchTF",
        xmTextFieldWidgetClass, toolbar,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, search_label,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNcolumns, 20,
        XmNleftOffset, 4,
        NULL);
    XtAddCallback(search_tf, XmNactivateCallback, search_cb, NULL);

    {
        Widget filter_rc;
        filter_rc = XtVaCreateManagedWidget("filterRC",
            xmRowColumnWidgetClass, toolbar,
            XmNorientation, XmHORIZONTAL,
            XmNradioBehavior, True,
            XmNradioAlwaysOne, True,
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET,
            XmNleftWidget, search_tf,
            XmNbottomAttachment, XmATTACH_FORM,
            XmNleftOffset, 8,
            NULL);

        filter_all_btn = XtVaCreateManagedWidget("All",
            xmToggleButtonWidgetClass, filter_rc,
            XmNset, True,
            NULL);
        XtAddCallback(filter_all_btn, XmNvalueChangedCallback,
                      filter_all_cb, NULL);

        filter_inst_btn = XtVaCreateManagedWidget("Installed",
            xmToggleButtonWidgetClass, filter_rc,
            NULL);
        XtAddCallback(filter_inst_btn, XmNvalueChangedCallback,
                      filter_inst_cb, NULL);

        filter_avail_btn = XtVaCreateManagedWidget("Available",
            xmToggleButtonWidgetClass, filter_rc,
            NULL);
        XtAddCallback(filter_avail_btn, XmNvalueChangedCallback,
                      filter_avail_cb, NULL);
    }

    /* ---- Paned window: top = pkg list, bottom = info ---- */
    paned = XtVaCreateManagedWidget("paned",
        xmPanedWindowWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, toolbar,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNorientation, XmVERTICAL,
        XmNtopOffset, 2,
        NULL);

    /* Top pane: package list + action buttons */
    left_pane = XtVaCreateManagedWidget("topPane",
        xmFormWidgetClass, paned,
        XmNpaneMinimum, 150,
        NULL);

    /* Scrolled package list */
    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmSINGLE_SELECT); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 20); n++;
    XtSetArg(args[n], XmNlistSizePolicy, XmCONSTANT); n++;
    pkg_list_w = XmCreateScrolledList(left_pane, "pkgList", args, n);

    /* Set fixed-width font for aligned columns */
    {
        XmFontList flist;
        XFontStruct *xfont;
        Display *dpy = XtDisplay(top_shell);

        /* Try common fixed-width fonts available on Solaris 7 */
        xfont = XLoadQueryFont(dpy, "-*-lucidatypewriter-medium-r-*-*-12-*-*-*-*-*-*-*");
        if (!xfont)
            xfont = XLoadQueryFont(dpy, "-*-courier-medium-r-*-*-12-*-*-*-*-*-*-*");
        if (!xfont)
            xfont = XLoadQueryFont(dpy, "fixed");

        if (xfont) {
            flist = XmFontListCreate(xfont, XmSTRING_DEFAULT_CHARSET);
            XtVaSetValues(pkg_list_w, XmNfontList, flist, NULL);
        }
    }
    XtVaSetValues(XtParent(pkg_list_w),
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XtManageChild(pkg_list_w);
    XtAddCallback(pkg_list_w, XmNsingleSelectionCallback,
                  pkg_select_cb, NULL);

    /* Action button row */
    {
        Widget btn_row;
        btn_row = XtVaCreateManagedWidget("btnRow",
            xmRowColumnWidgetClass, left_pane,
            XmNorientation, XmHORIZONTAL,
            XmNpacking, XmPACK_TIGHT,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNbottomAttachment, XmATTACH_FORM,
            NULL);

        /* Attach list to button row */
        XtVaSetValues(XtParent(pkg_list_w),
            XmNbottomAttachment, XmATTACH_WIDGET,
            XmNbottomWidget, btn_row,
            NULL);

        install_btn = XtVaCreateManagedWidget("Install",
            xmPushButtonWidgetClass, btn_row, NULL);
        XtAddCallback(install_btn, XmNactivateCallback,
                      install_cb, NULL);

        remove_btn = XtVaCreateManagedWidget("Remove",
            xmPushButtonWidgetClass, btn_row, NULL);
        XtAddCallback(remove_btn, XmNactivateCallback,
                      remove_cb, NULL);

        upgrade_btn = XtVaCreateManagedWidget("Upgrade",
            xmPushButtonWidgetClass, btn_row, NULL);
        XtAddCallback(upgrade_btn, XmNactivateCallback,
                      upgrade_cb, NULL);

        deps_btn_w = XtVaCreateManagedWidget("Deps",
            xmPushButtonWidgetClass, btn_row, NULL);
        XtAddCallback(deps_btn_w, XmNactivateCallback,
                      deps_cb, NULL);

        /* Version dropdown (OptionMenu + Pulldown) */
        version_pulldown = XmCreatePulldownMenu(btn_row,
            "versionPulldown", NULL, 0);
        {
            /* Create a placeholder button so the OptionMenu is valid */
            XmString xs = XmStringCreateLocalized("(no package)");
            version_buttons[0] = XtVaCreateManagedWidget("placeholder",
                xmPushButtonWidgetClass, version_pulldown,
                XmNlabelString, xs,
                NULL);
            XmStringFree(xs);
            version_count = 1;
            version_indices[0] = -1;
        }
        version_option_menu = XmCreateOptionMenu(btn_row,
            "versionMenu", NULL, 0);
        {
            XmString xs = XmStringCreateLocalized("Version:");
            XtVaSetValues(version_option_menu,
                XmNsubMenuId, version_pulldown,
                XmNlabelString, xs,
                NULL);
            XmStringFree(xs);
        }
        XtManageChild(version_option_menu);
        XtSetSensitive(version_option_menu, False);

        /* Upgrade All button (hidden until needed) */
        upgrade_all_btn = XtVaCreateWidget("Upgrade All",
            xmPushButtonWidgetClass, btn_row, NULL);
        XtAddCallback(upgrade_all_btn, XmNactivateCallback,
                      upgrade_all_cb, NULL);
        /* Initially unmanaged — update_action_buttons will show it */
    }

    /* Bottom pane: info/log text area */
    right_pane = XtVaCreateManagedWidget("bottomPane",
        xmFormWidgetClass, paned,
        XmNpaneMinimum, 80,
        XmNpaneMaximum, 250,
        NULL);

    n = 0;
    XtSetArg(args[n], XmNeditable, False); n++;
    XtSetArg(args[n], XmNeditMode, XmMULTI_LINE_EDIT); n++;
    XtSetArg(args[n], XmNwordWrap, True); n++;
    XtSetArg(args[n], XmNscrollHorizontal, False); n++;
    XtSetArg(args[n], XmNrows, 8); n++;
    XtSetArg(args[n], XmNcolumns, 80); n++;
    info_text_w = XmCreateScrolledText(right_pane, "infoText", args, n);
    XtVaSetValues(XtParent(info_text_w),
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        NULL);
    XtManageChild(info_text_w);

    /* ---- Status bar ---- */
    status_label = XtVaCreateManagedWidget("statusLabel",
        xmLabelWidgetClass, main_window,
        XmNalignment, XmALIGNMENT_BEGINNING,
        NULL);

    XmMainWindowSetAreas(main_window, menu_bar, NULL, NULL, NULL, form);
    XtVaSetValues(main_window,
        XmNmessageWindow, status_label,
        NULL);
}

/* ================================================================
 * ROOT PRIVILEGE ELEVATION VIA su(1M)
 *
 * If not running as root, show a Motif password dialog and re-exec
 * ourselves as root via su(1M) on a pseudo-terminal.  This is the
 * authentic Solaris admin-tool pattern (admintool, etc.).
 * ================================================================ */

/*
 * Open a Solaris STREAMS pseudo-terminal pair.
 * Returns master fd, writes slave name into slave_name.
 * Returns -1 on failure.
 */
static int open_pty(char *slave_name, int sn_len)
{
    int master;
    char *sname;

    master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master < 0) return -1;

    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        close(master);
        return -1;
    }

    sname = ptsname(master);
    if (!sname) { close(master); return -1; }

    strncpy(slave_name, sname, sn_len - 1);
    slave_name[sn_len - 1] = '\0';
    return master;
}

/*
 * Try to authenticate and re-exec as root.
 *
 * Opens a pty, forks, child does:
 *   setsid → open slave → push ldterm/ptem → exec /usr/bin/su
 * Parent writes password to master fd, waits.
 * If su succeeds (child exec's spm-gui as root), parent exits.
 * If su fails, returns -1 so the caller can re-prompt.
 */
static int try_su_reexec(const char *password, const char *display,
                         char *argv0)
{
    int master;
    char slave_name[64];
    pid_t child;
    char cmd[512];
    int status;
    struct timeval tv;
    fd_set rfds;
    char buf[256];
    int n;

    master = open_pty(slave_name, sizeof(slave_name));
    if (master < 0) return -1;

    /* Build the command su will run.
     * We pass DISPLAY through, and transfer the X auth cookie so root
     * can connect to the display, then re-exec the same binary.
     * On Solaris 7 CDE, XAUTHORITY may not be set — default is
     * $HOME/.Xauthority.  We point root's XAUTHORITY at the original
     * user's file so the cookie is available without xauth(1). */
    {
        const char *xauth_file = getenv("XAUTHORITY");
        char xauth_buf[256];
        if (!xauth_file || !xauth_file[0]) {
            const char *home = getenv("HOME");
            if (home) {
                snprintf(xauth_buf, sizeof(xauth_buf),
                         "%s/.Xauthority", home);
                xauth_file = xauth_buf;
            }
        }
        if (xauth_file && xauth_file[0]) {
            snprintf(cmd, sizeof(cmd),
                     "DISPLAY=%s; export DISPLAY; "
                     "XAUTHORITY=%s; export XAUTHORITY; "
                     "exec /opt/sst/bin/spm-gui",
                     display ? display : ":0",
                     xauth_file);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "DISPLAY=%s; export DISPLAY; "
                     "exec /opt/sst/bin/spm-gui",
                     display ? display : ":0");
        }
    }

    child = fork();
    if (child < 0) { close(master); return -1; }

    if (child == 0) {
        /* === CHILD === */
        int slave;

        close(master);
        setsid();

        slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(127);

        /* Push STREAMS modules for terminal line discipline */
        ioctl(slave, I_PUSH, "ptem");
        ioctl(slave, I_PUSH, "ldterm");

        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);

        /* exec: su - root -c "DISPLAY=...; exec /opt/sst/bin/spm-gui" */
        execl("/usr/bin/su", "su", "-", "root", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* === PARENT === */
    /* Wait briefly for su's "Password:" prompt, then send password */
    usleep(500000);  /* 500ms — su needs time to print the prompt */

    {
        int plen = strlen(password);
        write(master, password, plen);
        write(master, "\n", 1);
    }

    /* Wait for su to finish or start the child process.
     * If authentication succeeds, su exec's spm-gui as root
     * and we won't get the child back until spm-gui exits.
     * We wait briefly — if su fails, it exits quickly. */
    usleep(1500000);  /* 1.5s — enough for su to validate */

    /* Check if child is still running (su succeeded → spm-gui started) */
    if (waitpid(child, &status, WNOHANG) == 0) {
        /* Child still running — su succeeded, spm-gui is now running as root.
         * We are the unprivileged parent: exit cleanly. */
        close(master);
        _exit(0);
    }

    /* Child exited — su probably failed (bad password) */
    close(master);

    /* Drain any output (e.g., "su: Sorry") */
    return -1;
}

/*
 * Show a Motif password dialog for root elevation.
 * Returns 0 if elevation succeeded (parent should exit),
 * returns -1 if user cancelled.
 */
static int g_su_done;    /* 0=pending, 1=ok/exited, -1=cancelled */
static int g_su_failed;  /* 1 if last attempt failed (bad password) */
static Widget g_su_dialog;
static Widget g_su_text;
static Widget g_su_error_label;

/* Password masking for Motif 1.2 (no XmNpasswordMode) */
static char g_su_password[256];
static int  g_su_pw_len;

static void su_pw_modify_cb(Widget w, XtPointer client, XtPointer call)
{
    XmTextVerifyCallbackStruct *cbs = (XmTextVerifyCallbackStruct *)call;
    int start, end, new_len, delta;
    char *ins;

    (void)w; (void)client;

    start = cbs->startPos;
    end   = cbs->endPos;

    /* Handle deletion */
    if (start < end) {
        delta = end - start;
        memmove(g_su_password + start, g_su_password + end,
                g_su_pw_len - end);
        g_su_pw_len -= delta;
        g_su_password[g_su_pw_len] = '\0';
    }

    /* Handle insertion */
    ins = cbs->text->ptr;
    new_len = cbs->text->length;
    if (ins && new_len > 0) {
        if (g_su_pw_len + new_len < (int)sizeof(g_su_password) - 1) {
            memmove(g_su_password + start + new_len,
                    g_su_password + start,
                    g_su_pw_len - start);
            memcpy(g_su_password + start, ins, new_len);
            g_su_pw_len += new_len;
            g_su_password[g_su_pw_len] = '\0';
            /* Replace display text with bullets */
            memset(cbs->text->ptr, '*', new_len);
        }
    }
}

static void su_ok_cb(Widget w, XtPointer client, XtPointer call)
{
    const char *display;
    char *argv0 = (char *)client;

    (void)w; (void)call;

    display = getenv("DISPLAY");

    if (try_su_reexec(g_su_password, display, argv0) == 0) {
        /* Should not reach here — parent exits in try_su_reexec */
        g_su_done = 1;
    } else {
        /* Bad password — show error, let user retry */
        g_su_pw_len = 0;
        g_su_password[0] = '\0';
        XmTextFieldSetString(g_su_text, "");
        if (g_su_error_label) {
            XtVaSetValues(g_su_error_label,
                XtVaTypedArg, XmNlabelString, XmRString,
                "Incorrect password. Try again.", 31,
                NULL);
        }
        g_su_failed = 1;
    }
}

static void su_cancel_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    g_su_done = -1;
}

static int elevate_to_root(int *argc_p, char *argv[])
{
    Widget su_shell, form, label, error_label, pw_field;
    Widget btn_form, ok_btn, cancel_btn, sep;
    XtAppContext su_app;
    Arg args[20];
    int n;
    XEvent event;

    /* Already root? */
    if (getuid() == 0) return 0;

    /* Initialize a minimal Xt for just the password dialog */
    su_shell = XtVaAppInitialize(&su_app, "Spm",
        NULL, 0, argc_p, argv, NULL,
        XmNtitle, "Sunstorm Package Manager",
        XmNwidth, 360,
        XmNheight, 180,
        XmNminWidth, 360,
        XmNminHeight, 180,
        XmNmaxWidth, 360,
        XmNmaxHeight, 180,
        NULL);

    form = XtVaCreateManagedWidget("suForm",
        xmFormWidgetClass, su_shell,
        XmNmarginWidth, 16,
        XmNmarginHeight, 12,
        NULL);

    /* "Root password required to manage packages" */
    label = XtVaCreateManagedWidget(
        "Root password required to manage packages:",
        xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 12,
        XmNrightAttachment, XmATTACH_FORM,
        XmNrightOffset, 12,
        NULL);

    /* Password text field (masked) */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, label); n++;
    XtSetArg(args[n], XmNtopOffset, 10); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftOffset, 12); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightOffset, 12); n++;
    XtSetArg(args[n], XmNcolumns, 30); n++;
    pw_field = XmCreateTextField(form, "pwField", args, n);
    XtManageChild(pw_field);

    /* Mask password input via modify-verify callback (Motif 1.2) */
    XtVaSetValues(pw_field, XmNverifyBell, False, NULL);
    g_su_pw_len = 0;
    g_su_password[0] = '\0';
    XtAddCallback(pw_field, XmNmodifyVerifyCallback, su_pw_modify_cb, NULL);

    g_su_text = pw_field;

    /* Error message label (initially blank) */
    error_label = XtVaCreateManagedWidget(" ",
        xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, pw_field,
        XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_FORM,
        XmNleftOffset, 12,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    g_su_error_label = error_label;

    /* Separator */
    sep = XtVaCreateManagedWidget("sep",
        xmSeparatorWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, error_label,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);

    /* OK / Cancel buttons */
    btn_form = XtVaCreateManagedWidget("btnForm",
        xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, sep,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        NULL);

    ok_btn = XtVaCreateManagedWidget("Authenticate",
        xmPushButtonWidgetClass, btn_form,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 15,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 45,
        NULL);

    cancel_btn = XtVaCreateManagedWidget("Cancel",
        xmPushButtonWidgetClass, btn_form,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 55,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 85,
        NULL);

    XtAddCallback(ok_btn, XmNactivateCallback, su_ok_cb, (XtPointer)argv[0]);
    XtAddCallback(cancel_btn, XmNactivateCallback, su_cancel_cb, NULL);

    /* Also activate on Enter in the password field */
    XtAddCallback(pw_field, XmNactivateCallback, su_ok_cb, (XtPointer)argv[0]);

    g_su_done = 0;
    g_su_failed = 0;

    XtRealizeWidget(su_shell);

    /* Run a mini event loop for just this dialog */
    while (g_su_done == 0) {
        XtAppNextEvent(su_app, &event);
        XtDispatchEvent(&event);
    }

    if (g_su_done == -1) {
        /* User cancelled */
        return -1;
    }

    /* Should not reach here normally — parent exits in try_su_reexec.
     * But just in case: */
    return 0;
}

/* ================================================================
 * STARTUP UPGRADE CHECK
 * ================================================================ */

static void check_startup_upgrades(void)
{
    int i, n;
    char msg[4096];
    int pos;

    n = count_upgradable();
    if (n == 0)
        return;

    /* Build a summary of upgradable packages */
    pos = 0;
    pos += snprintf(msg + pos, sizeof(msg) - pos,
        "%d installed package%s %s newer version%s available:\n\n",
        n, n == 1 ? "" : "s",
        n == 1 ? "has a" : "have",
        n == 1 ? "" : "s");

    if (n > 10) {
        /* Compact comma-delimited list for large number of updates */
        int first = 1;
        for (i = 0; i < g_db->inst_count && pos < (int)sizeof(msg) - 64; i++) {
            const installed_pkg_t *ip = &g_db->installed[i];
            int avail = pkgdb_find_avail(g_db, ip->name);
            if (avail >= 0) {
                const avail_pkg_t *ap = &g_db->available[avail];
                if (strcmp(ap->version, ip->version) != 0) {
                    pos += snprintf(msg + pos, sizeof(msg) - pos,
                        "%s%s", first ? "" : ", ", ip->name);
                    first = 0;
                }
            }
        }
        pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
        word_wrap(msg, 60);
    } else {
        for (i = 0; i < g_db->inst_count && pos < (int)sizeof(msg) - 128; i++) {
            const installed_pkg_t *ip = &g_db->installed[i];
            int avail = pkgdb_find_avail(g_db, ip->name);
            if (avail >= 0) {
                const avail_pkg_t *ap = &g_db->available[avail];
                if (strcmp(ap->version, ip->version) != 0) {
                    pos += snprintf(msg + pos, sizeof(msg) - pos,
                        "  %s: %s -> %s\n",
                        ip->name, ip->version, ap->version);
                }
            }
        }
    }

    pos += snprintf(msg + pos, sizeof(msg) - pos,
        "\nUpgrade now?");

    if (show_confirm_dialog("Updates Available", msg)) {
        char cmd[256];
        char title[128];
        snprintf(title, sizeof(title), "Upgrading %d packages", n);
        snprintf(cmd, sizeof(cmd), "spm upgrade 2>&1");
        set_status(title);
        run_in_progress_dialog(title, cmd);
        /* Refresh the package list after upgrade */
        populate_list();
    }
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(int argc, char *argv[])
{
    /* Elevate to root if not already — shows Motif password dialog.
     * This is the classic Solaris admin-tool pattern (admintool, etc.).
     * On success, parent exits and root instance takes over DISPLAY. */
    if (getuid() != 0) {
        if (elevate_to_root(&argc, argv) != 0) {
            fprintf(stderr, "spm-gui: root access required.\n");
            return 1;
        }
        /* If we reach here, elevation succeeded but didn't exec
         * (shouldn't happen). Fall through just in case. */
    }

    /* Initialize config and database */
    config_defaults(&g_config);
    config_load(&g_config);
    config_init_dirs();

    /* Initialize OpenSSL */
    if (g_config.ca_bundle[0])
        http_set_ca_bundle(g_config.ca_bundle);
    if (http_init() != 0) {
        fprintf(stderr, "spm-gui: warning: SSL init failed.\n");
    }

    g_db = pkgdb_new();
    pkgdb_load_index(g_db);
    pkgdb_load_installed(g_db);
    pkgdb_load_spool(g_db, "/var/spool/pkg");

    printf("spm-gui %s - Sunstorm Package Manager\n",
           SPM_VERSION);
    fflush(stdout);

    /* Initialize Xt/Motif */
    top_shell = XtVaAppInitialize(&app_context, "Spm",
        NULL, 0, &argc, argv, NULL,
        XmNtitle, "Sunstorm Package Manager",
        XmNwidth, 840,
        XmNheight, 540,
        NULL);

    /* Build the UI */
    create_ui();

    /* Populate the package list */
    populate_list();

    /* Realize and show */
    XtRealizeWidget(top_shell);

    /* Check for upgradable packages on startup */
    check_startup_upgrades();

    /* Start agent status check timer */
    agent_timer = XtAppAddTimeOut(app_context, 5000,
                                   check_agent_status, NULL);

    /* Enter Xt event loop */
    XtAppMainLoop(app_context);

    cleanup_and_exit(0);
    return 0;
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#ifdef __HAVE_WEBKIT2__
	#include <webkit2/webkit2.h>
#else
	#include <webkit/webkit.h>
#endif
#include <JavaScriptCore/JavaScript.h>
#include "list.h"

enum {
	MODE_NORMAL      = 1<<0,
	MODE_CMD         = 1<<1,
	MODE_INSERT      = 1<<2,
	MODE_PASSTHROUGH = 1<<3,
	MODE_ALL         = MODE_NORMAL|MODE_CMD|MODE_INSERT|MODE_PASSTHROUGH,
};

enum {
	MOD_USE_SHIFT = 1<<0,
	MOD_SHIFT     = 1<<1,
	MOD_CONTROL   = 1<<2,
	MOD_MOD1      = 1<<3,
	MOD_MOD2      = 1<<4,
	MOD_MOD3      = 1<<5,
	MOD_MOD4      = 1<<6,
	MOD_MOD5      = 1<<7,
};

enum {
	BAR_STATUS_PAGE = 0,
	BAR_INPUT_PAGE = 1,
};

enum {
	WKB_SETTING_TYPE_STRING = 1,
	WKB_SETTING_TYPE_BOOL,
	WKB_SETTING_TYPE_INT,
	WKB_SETTING_TYPE_DOUBLE,
};

enum {
	WKB_SETTING_SCOPE_GLOBAL = 1,
	WKB_SETTING_SCOPE_WINDOW,
	WKB_SETTING_SCOPE_TAB,
};

enum {
	WKB_VAR_CONTEXT_DISP = 1,
	WKB_VAR_CONTEXT_EXPAND,
	WKB_VAR_CONTEXT_BOOL_TOGGLE,
};

enum {
	WKB_CMD_RET_WINDOW_DESTROYED = 128,
};

static struct {
	struct list windows;
	struct list vars;
	struct list aliases;
	struct list binds;
	struct list hist;
	struct list downloads;
	int next_download_id, next_window_id;
	gboolean show_window;  /* Show new windows when created. If false, the new window must be shown manually. */
	/* settings; WKB_SETTING_SCOPE_GLOBAL */
	guint mod_mask;
	int default_width, default_height, msg_timeout;
	gboolean allow_popups, dl_auto_open;
	gchar *config_dir, *cookie_file, *cookie_policy, *download_dir,
		*dl_open_cmd, *homepage, *fc_tmp, *clipboard_tmp,
		*load_started, *load_finished, *dom_ready, *create;
	#ifdef __HAVE_WEBKIT2__
		gchar *spell_langs;
		WebKitSettings *settings;
		#if WEBKIT_CHECK_VERSION(2, 5, 1)
			WebKitUserContentManager *cm;
		#endif
	#else
		WebKitWebSettings *settings;
	#endif
} global;

struct window {
	struct node n;
	int id;
	GtkWidget *w, *nb, *bar_nb, *vb, *bar_hb, *bar_evb, *i, *consw, *con,
		*uri_l, *msg_l, *mode_l, *tabs_l, *id_l, *dl_l;
	guint mode, timeout;
	struct list tabs;
	struct hist *current_hist;
	gchar *tmp_line;
	GIOChannel *cmd_fifo_ioch;
	guint cmd_fifo_ioch_sid;
	/* settings; WKB_SETTING_SCOPE_WINDOW */
	gboolean auto_scroll, show_console, print_keyval, fullscreen;
	gchar *cmd_fifo, *find_string;
};

struct var {
	struct node n;
	gchar *name;
	gchar *value;
	int can_unset;
	struct var_handler *handler;
};

struct alias {
	struct node n;
	gchar *name;
	gchar *value;
};

struct bind {
	struct node n;
	guint mode, mod;
	gchar *key;
	struct bind_handler *handler;
	gchar *arg;
};

struct hist {
	struct node n;
	gchar *line;
};

struct tab {
	struct node n;
	GtkWidget *c;
	struct window *w;
	gchar *uri_label_text;
	/* settings; WKB_SETTING_SCOPE_TAB */
	gchar *uri;
};

struct download {
	struct node n;
	int id, status;
	gboolean auto_open;
	WebKitDownload *d;
};

struct token {
	struct node n;
	gchar *value;
	int parse;
};

struct command {
	gchar *name;
	gchar *desc;
	gchar *usage;
	int (*func)(struct window *, WebKitWebView *, struct command *, int argc, gchar **argv);
};

struct var_handler {
	int (*set)(struct window *, WebKitWebView *, struct var *, const gchar *);
	gchar * (*get)(struct window *, WebKitWebView *, struct var *, int);
	void (*init)(struct window *, WebKitWebView *, struct var_handler *);
};

struct bind_handler {
	gchar *name;
	gboolean (*func)(struct window *, struct bind *);
};

union wkb_setting {
	gchar *s;
	gboolean b;
	int i;
	double d;
};

struct default_wkb_setting {
	gchar *name;
	int scope;
	int type;
	union wkb_setting (*get)(struct window *, int);
	void (*set)(struct window *, union wkb_setting);
	union wkb_setting default_value;
};

/* Internal functions */
static void add_gobject_properties(struct window *, struct var_handler *, GObject *, const gchar *);
static struct token * append_token(struct list *, const gchar *, int);
static GString * concat_args(int, gchar **);
static gchar * construct_uri(const gchar *);
static void destroy_alias(struct window *, struct alias *);
static void destroy_bind(struct window *, struct bind *);
static void destroy_cmd_fifo(struct window *);
static void destroy_download(struct download *);
static void destroy_tab(struct tab *);
static struct token * destroy_token(struct list *, struct token *);
static void destroy_token_list(struct list *);
static void destroy_var(struct window *, struct var *);
static GString * escape_string(const gchar *);
static void eval_js(WebKitWebView *, const gchar *, const gchar *, int);
static int exec_line(struct window *, WebKitWebView *, const gchar *);
static void fullscreen_mode(struct window *, gboolean);
static struct alias * get_alias(struct window *, const gchar *);
static struct bind * get_bind(struct window *, guint, guint, const gchar *);
static struct download * get_download(int);
static gchar * get_mod_string(guint, gchar [8]);
static gchar * get_mode_string(guint, gchar [5]);
static struct var * get_var(struct window *, const gchar *);
static GObject * get_var_gobject(struct window *, WebKitWebView *, struct var *, gchar **);
static gchar * get_var_value(struct window *, WebKitWebView *, struct var *, int);
static struct default_wkb_setting * get_wkb_setting(struct window *, const gchar *);
static void init_cmd_fifo(struct window *);
static int modmask_compare(guint, guint);
static gchar * msg(struct window *, gchar *);
static void navigate(struct window *, WebKitWebView *, int);
static struct alias * new_alias(struct window *, const gchar *, const gchar *);
static struct bind * new_bind(struct window *);
static struct download * new_download(WebKitDownload *, const gchar *);
static struct hist * new_hist(struct window *, const gchar *);
static GtkWidget * new_tab(struct window *, WebKitWebView *, const gchar *, gboolean);
static struct var * new_var(struct window *, const gchar *, int);
static struct window * new_window(struct window *, gboolean, const gchar *);
static void open_download(struct window *, WebKitWebView *, struct download *, const gchar *);
static void open_input(struct window *, const gchar *);
static void open_uri(WebKitWebView *, const gchar *);
static gchar * out(struct window *, gboolean, gchar *);
static int parse(struct window *, WebKitWebView *, struct list *, gchar **);
static void parse_mod_mask(struct window *, const gchar *, guint *, const gchar *);
static void parse_mode_mask(struct window *, const gchar *, guint *, const gchar *);
static void print_bind(struct window *, struct bind *);
static void print_download(struct window *, struct download *);
static void quit(void);
static struct tab * remove_tab(struct tab *);
static void set_mode(struct window *, guint);
static int set_var_value(struct window *, WebKitWebView *, struct var *, const gchar *);
static void signal_handler(int);
static void tokenize(const gchar *, struct list *);
static void tokenize_expansion(const gchar *, struct list *);
static void update_dl_l(struct window *);
static void update_tabs_l(struct window *);
static void update_title(struct tab *);
static void update_uri(struct tab *, const gchar *, const gchar *);

/* Callback functions */
#ifdef __HAVE_WEBKIT2__
static void cb_close(WebKitWebView *, struct tab *);
#else
static gboolean cb_close(WebKitWebView *, struct tab *);
#endif
static gboolean cb_cmd_fifo_in(GIOChannel *, GIOCondition, struct window *);
static void cb_console_size_allocate(WebKitWebView *, GdkRectangle *, struct window *);
#ifdef __HAVE_WEBKIT2__
	#if WEBKIT_CHECK_VERSION(2, 5, 1)
		static GtkWidget * cb_create(WebKitWebView *, WebKitNavigationAction *, struct tab *);
	#else
		static GtkWidget * cb_create(WebKitWebView *, struct tab *);
	#endif
	static gboolean cb_decide_policy(WebKitWebView *, WebKitPolicyDecision *, WebKitPolicyDecisionType, struct tab *);
#else
	static GtkWidget * cb_create(WebKitWebView *, WebKitWebFrame *, struct tab *);
	static gboolean cb_decide_mime(WebKitWebView *, WebKitWebFrame *, WebKitNetworkRequest *, gchar *, WebKitWebPolicyDecision *, struct tab *);
	static gboolean cb_decide_policy(WebKitWebView *, WebKitWebFrame *, WebKitNetworkRequest *, WebKitWebNavigationAction *, WebKitWebPolicyDecision *, struct tab *);
	static gboolean cb_decide_window(WebKitWebView *, WebKitWebFrame *, WebKitNetworkRequest *, WebKitWebNavigationAction *, WebKitWebPolicyDecision *, struct tab *);
#endif
static void cb_destroy(WebKitWebView *, struct window *);
#ifdef __HAVE_WEBKIT2__
	static void cb_download(WebKitWebContext *, WebKitDownload *, void *);
	static gboolean cb_download_decide_destination(WebKitDownload *, gchar *, void *);
	static void cb_download_failed(WebKitDownload *, GError *, struct download *);
	static void cb_download_finished(WebKitDownload *, struct download *);
#else
	static void cb_document_load_finished(WebKitWebView *, WebKitWebFrame *, struct tab *);
	static gboolean cb_download(WebKitWebView *, WebKitDownload *, void *);
	static void cb_download_status_changed(WebKitDownload *, GParamSpec *, struct download *);
#endif
static void cb_download_progress_changed(WebKitDownload *, GParamSpec *, struct download *);
#ifdef __HAVE_WEBKIT2__
	static gboolean cb_enter_fullscreen(WebKitWebView *, struct tab *);
#else
	static gboolean cb_enter_fullscreen(WebKitWebView *, WebKitDOMHTMLElement *, struct tab *);
#endif
static void cb_input_end(WebKitWebView *, struct window *);
#ifndef __HAVE_WEBKIT2__
	static GtkWidget * cb_inspect_web_view(GtkWidget *, GtkWidget *, struct tab *);
#endif
static gboolean cb_keypress(WebKitWebView *, GdkEventKey *, struct window *);
#ifdef __HAVE_WEBKIT2__
	static gboolean cb_leave_fullscreen(WebKitWebView *, struct tab *);
	static void cb_load_changed(WebKitWebView *, WebKitLoadEvent, struct tab *);
	static void cb_mouse_target_changed(WebKitWebView *, WebKitHitTestResult *, guint, struct tab *);
#else
	static gboolean cb_leave_fullscreen(WebKitWebView *, WebKitDOMHTMLElement *, struct tab *);
	static void cb_load_changed(WebKitWebView *, GParamSpec *, struct tab *);
	static void cb_mouse_target_changed(WebKitWebView *, gchar *, gchar *, struct tab *);
#endif
static void cb_progress_changed(WebKitWebView *, GParamSpec *, struct tab *);
static void cb_tab_changed(WebKitWebView *, GtkWidget *, guint, struct window *);
static void cb_title_changed(WebKitWebView *, GParamSpec *, struct tab *);
static void cb_uri_changed(WebKitWebView *, GParamSpec *, struct tab *);

/* Bind functions */
static gboolean bind_hist_next(struct window *, struct bind *);
static gboolean bind_hist_prev(struct window *, struct bind *);
static gboolean bind_pass(struct window *, struct bind *);
static gboolean bind_run(struct window *, struct bind *);

/* Command functions */
#ifdef __HAVE_WEBKIT2__
	static int cmd_add_ss(struct window *, WebKitWebView *, struct command *, int, gchar **);
#endif
static int cmd_alias(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_bind(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_clear(struct window *, WebKitWebView *, struct command *, int, gchar **);
#ifdef __HAVE_WEBKIT2__
	static int cmd_clear_cache(struct window *, WebKitWebView *, struct command *, int, gchar **);
	static int cmd_clear_ss(struct window *, WebKitWebView *, struct command *, int, gchar **);
#endif
static int cmd_dl_cancel(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_dl_clear(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_dl_new(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_dl_open(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_dl_status(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_echo(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_find(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_help(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_js(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_js_file(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_last(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_loadconfig(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_move(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_msg(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_nav(struct window *,  WebKitWebView *,struct command *, int, gchar **);
static int cmd_open_input(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_open(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_print(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_quit(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_reload(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_reload_nc(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_reorder(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_set(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_set_mode(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_spawn(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_stop(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_switch(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_tclose(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_topen(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_unalias(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_unbind(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_unbind_all(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_unset(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_wclose(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_window(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_wopen(struct window *, WebKitWebView *, struct command *, int, gchar **);
static int cmd_zoom(struct window *, WebKitWebView *, struct command *, int, gchar **);

/* Set/get/init handler functions */
static gchar * get_handler_gobject(struct window *, WebKitWebView *, struct var *, int);
static gchar * get_handler_wkb(struct window *, WebKitWebView *, struct var *, int);
static void init_handler_gobject(struct window *, WebKitWebView *, struct var_handler *);
static void init_handler_wkb(struct window *, WebKitWebView *, struct var_handler *);
static int set_handler_gobject(struct window *, WebKitWebView *, struct var *, const gchar *);
static int set_handler_wkb(struct window *, WebKitWebView *, struct var *, const gchar *);

/* Wkb setting accessor/mutator functions */
static union wkb_setting get_webkit_api(struct window *, int);
static union wkb_setting get_cmd_fifo(struct window *, int);
static union wkb_setting get_config_dir(struct window *, int);
static void set_config_dir(struct window *, union wkb_setting);
static union wkb_setting get_cookie_file(struct window *, int);
static void set_cookie_file(struct window *, union wkb_setting);
static union wkb_setting get_cookie_policy(struct window *, int);
static void set_cookie_policy(struct window *, union wkb_setting);
static union wkb_setting get_auto_scroll(struct window *, int);
static void set_auto_scroll(struct window *, union wkb_setting);
static union wkb_setting get_show_console(struct window *, int);
static void set_show_console(struct window *, union wkb_setting);
static union wkb_setting get_print_keyval(struct window *, int);
static void set_print_keyval(struct window *, union wkb_setting);
static union wkb_setting get_mod_mask(struct window *, int);
static void set_mod_mask(struct window *, union wkb_setting);
static union wkb_setting get_fullscreen(struct window *, int);
static void set_fullscreen(struct window *, union wkb_setting);
static union wkb_setting get_allow_popups(struct window *, int);
static void set_allow_popups(struct window *, union wkb_setting);
static union wkb_setting get_download_dir(struct window *, int);
static void set_download_dir(struct window *, union wkb_setting);
static union wkb_setting get_dl_open_cmd(struct window *, int);
static void set_dl_open_cmd(struct window *, union wkb_setting);
static union wkb_setting get_dl_auto_open(struct window *, int);
static void set_dl_auto_open(struct window *, union wkb_setting);
static union wkb_setting get_default_width(struct window *, int);
static void set_default_width(struct window *, union wkb_setting);
static union wkb_setting get_default_height(struct window *, int);
static void set_default_height(struct window *, union wkb_setting);
static union wkb_setting get_msg_timeout(struct window *, int);
static void set_msg_timeout(struct window *, union wkb_setting);
#ifdef __HAVE_WEBKIT2__
	static union wkb_setting get_spell_langs(struct window *, int);
	static void set_spell_langs(struct window *, union wkb_setting);
	static union wkb_setting get_spell(struct window *, int);
	static void set_spell(struct window *, union wkb_setting);
	static union wkb_setting get_tls_errors(struct window *, int);
	static void set_tls_errors(struct window *, union wkb_setting);
#endif
static union wkb_setting get_find_string(struct window *, int);
static void set_find_string(struct window *, union wkb_setting);
static union wkb_setting get_homepage(struct window *, int);
static void set_homepage(struct window *, union wkb_setting);
static union wkb_setting get_current_uri(struct window *, int);
static union wkb_setting get_fc_dir(struct window *, int);
static union wkb_setting get_fc_file(struct window *, int);
static union wkb_setting get_clipboard_text(struct window *, int);
static void set_clipboard_text(struct window *, union wkb_setting);
static union wkb_setting get_load_started(struct window *, int);
static void set_load_started(struct window *, union wkb_setting);
static union wkb_setting get_dom_ready(struct window *, int);
static void set_dom_ready(struct window *, union wkb_setting);
static union wkb_setting get_load_finished(struct window *, int);
static void set_load_finished(struct window *, union wkb_setting);
static union wkb_setting get_create(struct window *, int);
static void set_create(struct window *, union wkb_setting);

#include "config.h"

#define LENGTH(x) (sizeof(x) / sizeof(x[0]))
#define PARSE_ERROR(_cond, _m, _l, _r) if (_cond) { ret = _r; _m; goto _l; }
#ifdef __HAVE_WEBKIT2__
	#define GET_VIEW_FROM_CHILD(c) WEBKIT_WEB_VIEW(c)
#else
	#define GET_VIEW_FROM_CHILD(c) WEBKIT_WEB_VIEW(gtk_bin_get_child(GTK_BIN(c)))
#endif
#define GET_NTH_VIEW(w, n) GET_VIEW_FROM_CHILD(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->nb), n))
#define GET_CURRENT_TAB(w) ((struct tab *) (w)->tabs.h)
#define GET_CURRENT_TAB_CHILD(w) ((GET_CURRENT_TAB(w) == NULL) ? NULL : GET_CURRENT_TAB(w)->c)
#define GET_CURRENT_VIEW(w) GET_VIEW_FROM_CHILD(GET_CURRENT_TAB_CHILD(w))
#define SELECT_VIEW(w, wv) ((wv == NULL) ? GET_CURRENT_VIEW(w) : wv)
#define CLEAN_MASK(m, k) (m & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_MOD2_MASK | GDK_MOD3_MASK | GDK_MOD4_MASK | GDK_MOD5_MASK))
#ifdef __HAVE_WEBKIT2__
	enum {
		DOWNLOAD_STATUS_FINISHED = -1,
		DOWNLOAD_STATUS_ACTIVE = 0,
		DOWNLOAD_STATUS_ERROR_NETWORK = WEBKIT_DOWNLOAD_ERROR_NETWORK,
		DOWNLOAD_STATUS_CANCELLED = WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER,
		DOWNLOAD_STATUS_ERROR_DEST = WEBKIT_DOWNLOAD_ERROR_DESTINATION,
	};
	#define DOWNLOAD_IS_ACTIVE(s) (s == DOWNLOAD_STATUS_ACTIVE)
#else
	#define DOWNLOAD_IS_ACTIVE(s) (s == WEBKIT_DOWNLOAD_STATUS_CREATED || s == WEBKIT_DOWNLOAD_STATUS_STARTED)
#endif

/* Begin internal functions */

static void add_gobject_properties(struct window *w, struct var_handler *vh, GObject *obj, const gchar *fmt)
{
	int i = 0;
	unsigned int n_prop = 0;
	gchar *temp;
	GParamSpec **ps;
	GType t;

	ps = g_object_class_list_properties(G_OBJECT_GET_CLASS(obj), &n_prop);
	for (i = 0; i < n_prop; ++i) {
		t = G_PARAM_SPEC_VALUE_TYPE(ps[i]);
#ifdef __HAVE_WEBKIT2__
		if (t == G_TYPE_BOOLEAN || t == G_TYPE_INT || t == G_TYPE_UINT || t == G_TYPE_DOUBLE || t == G_TYPE_FLOAT || t == G_TYPE_STRING) {
#else
		if (t == G_TYPE_BOOLEAN || t == G_TYPE_INT || t == G_TYPE_UINT || t == G_TYPE_DOUBLE || t == G_TYPE_FLOAT || t == G_TYPE_STRING || t == SOUP_TYPE_URI) {
#endif
			new_var(w, temp = g_strdup_printf(fmt, g_param_spec_get_name(ps[i])), 0)->handler = vh;
			g_free(temp);
		}
	}
	g_free(ps);
}

static struct token * append_token(struct list *tok, const gchar *value, int parse)
{
	struct token *t = g_malloc0(sizeof(struct token));
	t->value = g_strdup(value);
	t->parse = parse;
	LIST_ADD_TAIL(tok, (struct node *) t);
	return t;
}

static GString * concat_args(int argc, gchar **argv)
{
	int i;
	GString *str = g_string_new(NULL);
	for (i = 1; i < argc - 1; ++i) g_string_append_printf(str, "%s ", argv[i]);
	if (argc > 1) g_string_append(str, argv[i]);
	return str;
}

static gchar * construct_uri(const gchar *uri)
{
	int i = 0;
	if (uri == NULL) return NULL;
	else if (uri[0] == '\0') return g_strdup(uri);
	else if (uri[0] == '.' && uri[1] == '/') return g_strconcat("file://", getenv("PWD"), &uri[1], NULL);
	else if (uri[0] == '/') return g_strconcat("file://", uri, NULL);
	for (i = 0; uri[i] != '\0'; ++i) {
		if (uri[i] == '.') break;
		else if (uri[i] == ':') return g_strdup(uri);
	}
	return g_strconcat("http://", uri, NULL);
}

static void destroy_alias(struct window *w, struct alias *a)
{
	LIST_REMOVE(&global.aliases, (struct node *) a);
	g_free(a->name);
	g_free(a->value);
	g_free(a);
}

static void destroy_bind(struct window *w, struct bind *b)
{
	LIST_REMOVE(&global.binds, (struct node *) b);
	g_free(b->key);
	g_free(b->arg);
	g_free(b);
}

static void destroy_cmd_fifo(struct window *w)
{
	g_source_remove(w->cmd_fifo_ioch_sid);
	if (w->cmd_fifo_ioch != NULL) {
		g_io_channel_shutdown(w->cmd_fifo_ioch, FALSE, NULL);
		close(g_io_channel_unix_get_fd(w->cmd_fifo_ioch));
		g_io_channel_unref(w->cmd_fifo_ioch);
	}
	remove(w->cmd_fifo);
	g_free(w->cmd_fifo);
	w->cmd_fifo = NULL;
}

static void destroy_download(struct download *dl)
{
	LIST_REMOVE(&global.downloads, (struct node *) dl);
	if (DOWNLOAD_IS_ACTIVE(dl->status)) webkit_download_cancel(dl->d);
	g_object_unref(dl->d);
	g_free(dl);
}

static void destroy_tab(struct tab *t)
{
	webkit_web_view_stop_loading(GET_VIEW_FROM_CHILD(t->c));  /* This is required. Otherwise cb_progress_changed gets called after this function runs... */
	remove_tab(t);
	g_free(t->uri_label_text);
	g_free(t->uri);
	g_free(t);
}

static struct token * destroy_token(struct list *tok, struct token *t)
{
	struct token *r = (struct token *) t->n.n;
	LIST_REMOVE(tok, (struct node *) t);
	g_free(t->value);
	g_free(t);
	return r;
}

static void destroy_token_list(struct list *tok)
{
	struct token *t;
	while ((t = (struct token *) tok->h) != NULL)
		destroy_token(tok, t);
}

static void destroy_var(struct window *w, struct var *v)
{
	LIST_REMOVE(&global.vars, (struct node *) v);
	g_free(v->name);
	g_free(v->value);
	g_free(v);
}

static GString * escape_string(const gchar *s)
{
	GString *r = g_string_new(NULL);
	for (; *s != '\0'; ++s) {
		switch (*s) {
			case '\\': case '"': case '{': case ';':
				g_string_append_c(r, '\\');
			default:
				g_string_append_c(r, *s);
		}
	}
	return r;
}

static void eval_js(WebKitWebView *wv, const gchar *script, const gchar *source, int use_focused_frame)
{
#ifdef __HAVE_WEBKIT2__
	webkit_web_view_run_javascript(wv, script, NULL, NULL, NULL);
#else
	JSGlobalContextRef global_context;
	if (use_focused_frame)
		global_context = webkit_web_frame_get_global_context(webkit_web_view_get_focused_frame(wv));
	else
		global_context = webkit_web_frame_get_global_context(webkit_web_view_get_main_frame(wv));
	JSStringRef script_string = JSStringCreateWithUTF8CString(script);
	JSStringRef source_string = JSStringCreateWithUTF8CString(script);
	JSEvaluateScript(global_context, script_string, NULL, source_string, 0, NULL);
	JSStringRelease(script_string);
	JSStringRelease(source_string);
#endif
}

static int exec_line(struct window *w, WebKitWebView *wv, const gchar *line)
{
	struct list tok = NEW_LIST, alias_tok = NEW_LIST;
	struct token *t = NULL;
	struct alias *a = NULL;
	gchar *arg = NULL, **argv = NULL;
	int i = 0, argc = 0, stop = 0, ret = 0;

	if (line == NULL) return 0;
	tokenize(line, &tok);
	while (tok.h != NULL && !stop) {
		if (parse(w, SELECT_VIEW(w, wv), &tok, &arg)) goto parse_error;
		if (argc == 0 && arg != NULL && (a = get_alias(w, arg)) != NULL) {
			g_free(arg);
			tokenize(a->value, &alias_tok);
			while ((t = (struct token *) alias_tok.t) != NULL) {
				LIST_REMOVE_TAIL(&alias_tok);
				LIST_ADD_HEAD(&tok, (struct node *) t);
			}
			continue;
		}
		if (arg != NULL) {
			argv = g_realloc(argv, sizeof(*argv) * (argc + 1));
			if (argc == 0 && arg[0] == '!') {
				argv[argc++] = g_strdup(&arg[1]);
				g_free(arg);
			}
			else argv[argc++] = arg;
		}
		if ((arg == NULL || tok.h == NULL) && argc != 0) {
			for (i = 0; i < LENGTH(commands); ++i) {
				if (strcmp(argv[0], commands[i].name) == 0) {
					ret = commands[i].func(w, SELECT_VIEW(w, wv), &commands[i], argc, argv);
					if (ret == WKB_CMD_RET_WINDOW_DESTROYED)
						stop = 1;
					break;
				}
			}
			if (i == LENGTH(commands))
				g_free(out(w, TRUE, g_strdup_printf("error: no such command: \"%s\"\n", argv[0])));
			for (i = 0; i < argc; ++i) g_free(argv[i]);
			g_free(argv);
			argv = NULL;
			argc = 0;
		}
	}
	parse_error:
	destroy_token_list(&tok);
	return ret;
}

static void fullscreen_mode(struct window *w, gboolean f)
{
	if (f) {
		gtk_notebook_set_show_tabs(GTK_NOTEBOOK(w->nb), FALSE);
		if (w->mode != MODE_CMD) gtk_widget_hide(w->bar_nb);
	}
	else {
		gtk_notebook_set_show_tabs(GTK_NOTEBOOK(w->nb), TRUE);
		gtk_widget_show(w->bar_nb);
	}
	w->fullscreen = f;
}

static struct alias * get_alias(struct window *w, const gchar *name)
{
	struct alias *a;
	LIST_FOREACH(&global.aliases, a)
		if (strcmp(a->name, name) == 0)
			return a;
	return NULL;
}

static struct bind * get_bind(struct window *w, guint mode, guint mod, const gchar *key)
{
	struct bind *b;
	LIST_FOREACH(&global.binds, b)
		if (b->mode & mode && b->mod == mod && strcmp(b->key, key) == 0)
			return b;
	return NULL;
}

static struct download * get_download(int id)
{
	struct download *dl;
	LIST_FOREACH(&global.downloads, dl) if (dl->id == id) return dl;
	return NULL;
}

static gchar * get_mod_string(guint mod, gchar str_mod[8])
{
	memset(str_mod, '-', 7);
	str_mod[7] = '\0';
	if (mod & MOD_USE_SHIFT) {
		if (mod & MOD_SHIFT) str_mod[0] = 's';
		else str_mod[0] = 'S';
	}
	if (mod & MOD_CONTROL)  str_mod[1] = 'c';
	if (mod & MOD_MOD1)     str_mod[2] = '1';
	if (mod & MOD_MOD2)     str_mod[3] = '2';
	if (mod & MOD_MOD3)     str_mod[4] = '3';
	if (mod & MOD_MOD4)     str_mod[5] = '4';
	if (mod & MOD_MOD5)     str_mod[6] = '5';
	return str_mod;
}

static gchar * get_mode_string(guint mode, gchar str_mode[5])
{
	memset(str_mode, '-', 4);
	str_mode[4] = '\0';
	if (mode & MODE_NORMAL)      str_mode[0] = 'n';
	if (mode & MODE_CMD)         str_mode[1] = 'c';
	if (mode & MODE_INSERT)      str_mode[2] = 'i';
	if (mode & MODE_PASSTHROUGH) str_mode[3] = 'p';
	return str_mode;
}

static struct var * get_var(struct window *w, const gchar *name)
{
	struct var *v;
	LIST_FOREACH(&global.vars, v) if (strcmp(name, v->name) == 0) return v;
	return NULL;
}

static GObject * get_var_gobject(struct window *w, WebKitWebView *wv, struct var *v, gchar **name)
{
	if (strncmp(v->name, VAR_PREFIX_WEBKIT_WEB_VIEW, strlen(VAR_PREFIX_WEBKIT_WEB_VIEW)) == 0) {
		*name = &v->name[strlen(VAR_PREFIX_WEBKIT_WEB_VIEW)];
		return G_OBJECT(wv);
	}
	else if (strncmp(v->name, VAR_PREFIX_WEBKIT_SETTINGS, strlen(VAR_PREFIX_WEBKIT_SETTINGS)) == 0) {
		*name = &v->name[strlen(VAR_PREFIX_WEBKIT_SETTINGS)];
		return G_OBJECT(webkit_web_view_get_settings(wv));
	}
#ifndef __HAVE_WEBKIT2__
	else if (strncmp(v->name, VAR_PREFIX_SOUP_SESSION, strlen(VAR_PREFIX_SOUP_SESSION)) == 0) {
		*name = &v->name[strlen(VAR_PREFIX_SOUP_SESSION)];
		return G_OBJECT(webkit_get_default_session());
	}
#endif
	else return NULL;
}

static gchar * get_var_value(struct window *w, WebKitWebView *wv, struct var *v, int context)
{
	if (v == NULL) return NULL;
	if (v->handler == NULL || v->handler->get == NULL) return g_strdup(v->value);
	else return v->handler->get(w, wv, v, context);
}

static struct default_wkb_setting * get_wkb_setting(struct window *w, const gchar *name)
{
	int i;
	for (i = 0; i < LENGTH(default_wkb_settings); ++i)
		if (strcmp(name, default_wkb_settings[i].name) == 0)
			return &default_wkb_settings[i];
	return NULL;
}

static void init_cmd_fifo(struct window *w)
{
	int fd;
	w->cmd_fifo = g_strdup_printf("/tmp/wkb-%d-%d", getpid(), w->id);
	remove(w->cmd_fifo);
	if (mkfifo(w->cmd_fifo, S_IWUSR | S_IRUSR)) {
		g_free(out(w, TRUE, g_strdup_printf("init_cmd_fifo: FATAL: failed to create fifo \"%s\"\n", w->cmd_fifo)));
		return;
	}
	if ((fd = open(w->cmd_fifo, O_RDONLY | O_NONBLOCK)) == -1) {
		g_free(out(w, TRUE, g_strdup_printf("init_cmd_fifo: FATAL: failed to open fifo \"%s\"\n", w->cmd_fifo)));
		return;
	}
	w->cmd_fifo_ioch = g_io_channel_unix_new(fd);
	w->cmd_fifo_ioch_sid = g_io_add_watch(w->cmd_fifo_ioch, G_IO_IN | G_IO_HUP, (GIOFunc) cb_cmd_fifo_in, w);
}

static int modmask_compare(guint mod, guint gdk_state)
{
	guint m = 0;
	if (mod & MOD_USE_SHIFT) {
		m |= MOD_USE_SHIFT;
		if (gdk_state & GDK_SHIFT_MASK) m |= MOD_SHIFT;
	}
	if (gdk_state & GDK_CONTROL_MASK) m |= MOD_CONTROL;
	if (gdk_state & GDK_MOD1_MASK)    m |= MOD_MOD1;
	if (gdk_state & GDK_MOD2_MASK)    m |= MOD_MOD2;
	if (gdk_state & GDK_MOD3_MASK)    m |= MOD_MOD3;
	if (gdk_state & GDK_MOD4_MASK)    m |= MOD_MOD4;
	if (gdk_state & GDK_MOD5_MASK)    m |= MOD_MOD5;
	return (m & ~global.mod_mask) == (mod & ~global.mod_mask);
}

static gboolean msg_timeout(struct window *w)
{
	w->timeout = 0;
	gtk_widget_hide(w->msg_l);
	return FALSE;
}

static gchar * msg(struct window *w, gchar *s)
{
	gtk_label_set_text(GTK_LABEL(w->msg_l), s);
	gtk_widget_show(w->msg_l);
	if (w->timeout) g_source_remove(w->timeout);
	if (global.msg_timeout > 0)
		w->timeout = g_timeout_add(global.msg_timeout, (GSourceFunc) msg_timeout, w);
	return s;
}

static void navigate(struct window *w, WebKitWebView *v, int n)
{
#ifdef __HAVE_WEBKIT2__
	WebKitBackForwardListItem *item = webkit_back_forward_list_get_nth_item(webkit_web_view_get_back_forward_list(v), n);
	if (item != NULL) webkit_web_view_go_to_back_forward_list_item(v, item);
#else
	webkit_web_view_go_back_or_forward(v, n);
#endif
}

static struct alias * new_alias(struct window *w, const gchar *name, const gchar *value)
{
	struct alias *a = g_malloc0(sizeof(struct alias));
	a->name = g_strdup(name);
	a->value = g_strdup(value);
	LIST_ADD_TAIL(&global.aliases, (struct node *) a);
	return a;
}

static struct bind * new_bind(struct window *w)
{
	struct bind *b = g_malloc0(sizeof(struct bind));
	LIST_ADD_TAIL(&global.binds, (struct node *) b);
	return b;
}

static struct download * new_download(WebKitDownload *d, const gchar *filename)
{
	gchar *file_uri;
	struct download *dl;
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Download", NULL, GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
	GtkWidget *auto_open = gtk_check_button_new_with_label("Auto open");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_open), global.dl_auto_open);
	gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(dialog), auto_open);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), global.download_dir);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), filename);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		dl = g_malloc0(sizeof(struct download));
		dl->d = d;
		dl->id = global.next_download_id++;
		dl->auto_open = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_open));
		LIST_ADD_TAIL(&global.downloads, (struct node *) dl);
#ifdef __HAVE_WEBKIT2__
		dl->status = DOWNLOAD_STATUS_ACTIVE;
		webkit_download_set_destination(d, file_uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(dialog)));
		g_signal_connect(d, "failed", G_CALLBACK(cb_download_failed), dl);
		g_signal_connect(d, "finished", G_CALLBACK(cb_download_finished), dl);
		g_signal_connect(d, "notify::estimated-progress", G_CALLBACK(cb_download_progress_changed), dl);
		g_object_ref(G_OBJECT(d));
#else
		dl->status = webkit_download_get_status(d);
		webkit_download_set_destination_uri(d, file_uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(dialog)));
		g_signal_connect(d, "notify::status", G_CALLBACK(cb_download_status_changed), dl);
		g_signal_connect(d, "notify::progress", G_CALLBACK(cb_download_progress_changed), dl);
#endif
		g_free(file_uri);
		gtk_widget_destroy(dialog);
		return dl;
	}
	gtk_widget_destroy(dialog);
	return NULL;
}

static struct hist * new_hist(struct window *w, const gchar *line)
{
	struct hist *h = g_malloc0(sizeof(struct hist));
	h->line = g_strdup(line);
	LIST_ADD_TAIL(&global.hist, (struct node *) h);
	return h;
}

static GtkWidget * new_tab(struct window *w, WebKitWebView *v, const gchar *uri, gboolean is_bg)
{
	int i;
	GtkWidget *wv;
	struct tab *t = g_malloc0(sizeof(struct tab));
#if __HAVE_WEBKIT2__ && WEBKIT_CHECK_VERSION(2, 5, 1)
	wv = (v == NULL) ? webkit_web_view_new_with_user_content_manager(global.cm) : GTK_WIDGET(v);
#else
	wv = (v == NULL) ? webkit_web_view_new() : GTK_WIDGET(v);
#endif
	webkit_web_view_set_settings(WEBKIT_WEB_VIEW(wv), global.settings);
	gtk_widget_show(wv);
	g_signal_connect(wv, "notify::title", G_CALLBACK(cb_title_changed), t);
	g_signal_connect(wv, "notify::uri", G_CALLBACK(cb_uri_changed), t);
#ifdef __HAVE_WEBKIT2__
	g_signal_connect(wv, "load-changed", G_CALLBACK(cb_load_changed), t);
	g_signal_connect(wv, "decide-policy", G_CALLBACK(cb_decide_policy), t);
	g_signal_connect(wv, "create", G_CALLBACK(cb_create), t);
	g_signal_connect(wv, "notify::estimated-load-progress", G_CALLBACK(cb_progress_changed), t);
	g_signal_connect(wv, "mouse-target-changed", G_CALLBACK(cb_mouse_target_changed), t);
	g_signal_connect(wv, "enter-fullscreen", G_CALLBACK(cb_enter_fullscreen), t);
	g_signal_connect(wv, "leave-fullscreen", G_CALLBACK(cb_leave_fullscreen), t);
	g_signal_connect(wv, "close", G_CALLBACK(cb_close), t);
	t->c = wv;
#else
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(sw), wv);
	gtk_widget_show(sw);
	g_signal_connect(wv, "notify::load-status", G_CALLBACK(cb_load_changed), t);
	g_signal_connect(wv, "document-load-finished", G_CALLBACK(cb_document_load_finished), t);
	g_signal_connect(wv, "mime-type-policy-decision-requested", G_CALLBACK(cb_decide_mime), t);
	g_signal_connect(wv, "navigation-policy-decision-requested", G_CALLBACK(cb_decide_policy), t);
	g_signal_connect(wv, "new-window-policy-decision-requested", G_CALLBACK(cb_decide_window), t);
	g_signal_connect(wv, "download-requested", G_CALLBACK(cb_download), NULL);
	g_signal_connect(wv, "create-web-view", G_CALLBACK(cb_create), t);
	g_signal_connect(wv, "notify::progress", G_CALLBACK(cb_progress_changed), t);
	g_signal_connect(wv, "hovering-over-link", G_CALLBACK(cb_mouse_target_changed), t);
	g_signal_connect(G_OBJECT(webkit_web_view_get_inspector(WEBKIT_WEB_VIEW(wv))), "inspect-web-view", G_CALLBACK(cb_inspect_web_view), t);
	g_signal_connect(wv, "entering-fullscreen", G_CALLBACK(cb_enter_fullscreen), t);
	g_signal_connect(wv, "leaving-fullscreen", G_CALLBACK(cb_leave_fullscreen), t);
	g_signal_connect(wv, "close-web-view", G_CALLBACK(cb_close), t);
	t->c = sw;
#endif
	t->w = w;
	if (is_bg)
		LIST_ADD_TAIL(&w->tabs, (struct node *) t);
	else
		LIST_ADD_HEAD(&w->tabs, (struct node *) t);
	gtk_notebook_insert_page(GTK_NOTEBOOK(w->nb), t->c, gtk_label_new(NULL), (is_bg) ? -1 : gtk_notebook_get_current_page(GTK_NOTEBOOK(w->nb)) + 1);
	gtk_container_child_set(GTK_CONTAINER(w->nb), t->c, "tab-expand", TRUE, NULL);
#ifdef __HAVE_GTK3__
	gtk_widget_set_halign(gtk_notebook_get_tab_label(GTK_NOTEBOOK(w->nb), t->c), GTK_ALIGN_START);
	gtk_widget_set_valign(gtk_notebook_get_tab_label(GTK_NOTEBOOK(w->nb), t->c), GTK_ALIGN_CENTER);
#else
	gtk_misc_set_alignment(GTK_MISC(gtk_notebook_get_tab_label(GTK_NOTEBOOK(w->nb), t->c)), 0, 0.5);
#endif
	gtk_label_set_ellipsize(GTK_LABEL(gtk_notebook_get_tab_label(GTK_NOTEBOOK(w->nb), t->c)), PANGO_ELLIPSIZE_END);
	if (!is_bg)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(w->nb), gtk_notebook_page_num(GTK_NOTEBOOK(w->nb), t->c));
	for (i = 0; i < LENGTH(default_wkb_settings); ++i)
		if (default_wkb_settings[i].scope == WKB_SETTING_SCOPE_TAB && default_wkb_settings[i].set != NULL)
			default_wkb_settings[i].set(w, default_wkb_settings[i].default_value);
	open_uri(WEBKIT_WEB_VIEW(wv), uri);
	LIST_FOREACH(&w->tabs, t) update_title(t);
	if (global.create != NULL) exec_line(w, WEBKIT_WEB_VIEW(wv), global.create);
	return wv;
}

static struct var * new_var(struct window *w, const gchar *name, int can_unset)
{
	struct var *v = g_malloc0(sizeof(struct var));
	v->name = g_strdup(name);
	v->can_unset = can_unset;
	LIST_ADD_TAIL(&global.vars, (struct node *) v);
	return v;
}

static struct window * new_window(struct window *w, gboolean create, const gchar *uri)
{
	int i;
	gchar *tmp;
	GdkGeometry hints = { 100, 100, 0, 0, 100, 100, 0, 0, 0, 0, 0 };

	w->id = global.next_window_id++;

	w->nb = gtk_notebook_new();
	gtk_widget_set_name(w->nb, "wkb-view");
	g_signal_connect(w->nb, "switch-page", G_CALLBACK(cb_tab_changed), w);
	gtk_widget_show(w->nb);

	w->i = gtk_entry_new();
	gtk_widget_set_name(w->i, "wkb-input");
	gtk_entry_set_has_frame(GTK_ENTRY(w->i), FALSE);
	g_signal_connect(w->i, "activate", G_CALLBACK(cb_input_end), w);
	gtk_widget_show(w->i);

	w->uri_l = gtk_label_new(NULL);
	gtk_widget_set_name(w->uri_l, "wkb-uri");
#ifdef __HAVE_GTK3__
	gtk_widget_set_halign(w->uri_l, GTK_ALIGN_START);
	gtk_widget_set_valign(w->uri_l, GTK_ALIGN_CENTER);
#else
	gtk_misc_set_alignment(GTK_MISC(w->uri_l), 0, 0.5);
#endif
	gtk_label_set_ellipsize(GTK_LABEL(w->uri_l), PANGO_ELLIPSIZE_END);
	gtk_widget_show(w->uri_l);
	w->mode_l = gtk_label_new(NULL);
	gtk_widget_set_name(w->mode_l, "wkb-mode");
	w->tabs_l = gtk_label_new("[1/1]");
	gtk_widget_set_name(w->tabs_l, "wkb-tabs");
	gtk_widget_show(w->tabs_l);
	w->id_l = gtk_label_new(tmp = g_strdup_printf("[%d-%d]", getpid(), w->id));
	g_free(tmp);
	gtk_widget_set_name(w->id_l, "wkb-id");
	gtk_widget_show(w->id_l);
	w->dl_l = gtk_label_new("[dl:]");
	gtk_widget_set_name(w->dl_l, "wkb-download");
	w->msg_l = gtk_label_new(NULL);
	gtk_widget_set_name(w->msg_l, "wkb-msg");

#ifdef __HAVE_GTK3__
	w->bar_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
#else
	w->bar_hb = gtk_hbox_new(FALSE, 1);
#endif
	gtk_box_pack_start(GTK_BOX(w->bar_hb), w->uri_l, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(w->bar_hb), w->id_l, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(w->bar_hb), w->tabs_l, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(w->bar_hb), w->mode_l, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(w->bar_hb), w->dl_l, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(w->bar_hb), w->msg_l, FALSE, FALSE, 0);
	gtk_widget_show(w->bar_hb);

	w->bar_evb = gtk_event_box_new();
	gtk_widget_set_name(w->bar_evb, "wkb-status");
	gtk_container_add(GTK_CONTAINER(w->bar_evb), w->bar_hb);
	gtk_widget_show(w->bar_evb);
	
	w->bar_nb = gtk_notebook_new();
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(w->bar_nb), FALSE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(w->bar_nb), FALSE);
	gtk_notebook_append_page(GTK_NOTEBOOK(w->bar_nb), w->bar_evb, NULL);
	gtk_notebook_append_page(GTK_NOTEBOOK(w->bar_nb), w->i, NULL);
	gtk_widget_show(w->bar_nb);

	w->con = gtk_text_view_new();
	gtk_widget_set_name(w->con, "wkb-console");
	gtk_widget_set_can_focus(w->con, FALSE);
	g_signal_connect(w->con, "size-allocate", G_CALLBACK(cb_console_size_allocate), w);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(w->con), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(w->con), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w->con), GTK_WRAP_WORD_CHAR);
	gtk_widget_show(w->con);

	w->consw = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_set_name(w->consw, "wkb-console-sw");
	gtk_widget_set_can_focus(w->consw, FALSE);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w->consw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(w->consw), w->con);

#ifdef __HAVE_GTK3__
	w->vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#else
	w->vb = gtk_vbox_new(FALSE, 0);
#endif
	gtk_box_pack_start(GTK_BOX(w->vb), w->nb, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(w->vb), w->bar_nb, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(w->vb), w->consw, TRUE, TRUE, 0);
	gtk_widget_show(w->vb);

	w->w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_name(w->w, "wkb");
	gtk_window_set_geometry_hints(GTK_WINDOW(w->w), NULL, &hints, GDK_HINT_MIN_SIZE|GDK_HINT_BASE_SIZE);
	gtk_window_set_default_size(GTK_WINDOW(w->w), global.default_width, global.default_height);
	g_signal_connect_after(w->w, "destroy", G_CALLBACK(cb_destroy), w);
	g_signal_connect(w->w, "key-press-event", G_CALLBACK(cb_keypress), w);
	gtk_container_add(GTK_CONTAINER(w->w), w->vb);
	if (global.show_window) gtk_widget_show(w->w);

#ifndef __HAVE_GTK3__
	/* gtk2 style stuff */
	gtk_notebook_set_tab_border(GTK_NOTEBOOK(w->nb), 0);
#endif

	init_cmd_fifo(w);
	update_dl_l(w);
	for (i = 0; i < LENGTH(default_wkb_settings); ++i)
		if (default_wkb_settings[i].scope == WKB_SETTING_SCOPE_WINDOW && default_wkb_settings[i].set != NULL)
			default_wkb_settings[i].set(w, default_wkb_settings[i].default_value);
	if (create)
		new_tab(w, NULL, uri, false);
	set_mode(w, MODE_CMD);  /* workaround for GTK3 incorrectly calculating the size of w->i */
	set_mode(w, MODE_NORMAL);
	LIST_ADD_TAIL(&global.windows, (struct node *) w);
	return w;
}

static void open_download(struct window *w, WebKitWebView *wv, struct download *dl, const gchar *fmt)
{
	int i, k;
	GString *str = g_string_new(NULL), *escaped_filename;
#ifdef __HAVE_WEBKIT2__
	gchar *filename = g_filename_from_uri(webkit_download_get_destination(dl->d), NULL, NULL);
#else
	gchar *filename = g_filename_from_uri(webkit_download_get_destination_uri(dl->d), NULL, NULL);
#endif
	escaped_filename = escape_string(filename);
	g_free(filename);
	i = k = 0;
	while (fmt[k] != '\0') {
		if (fmt[k] == '%' && fmt[k + 1] == 'f') {
			if (k != i) g_string_append_len(str, &fmt[i], k - i);
			g_string_append(str, escaped_filename->str);
			i = (k += 2);
		}
		else ++k;
	}
	if (k != i) g_string_append_len(str, &fmt[i], k - i);
	exec_line(w, NULL, str->str);
	g_string_free(str, TRUE);
	g_string_free(escaped_filename, TRUE);
}

static void open_input(struct window *w, const gchar *str)
{
	set_mode(w, MODE_CMD);
	if (str != NULL) gtk_entry_set_text(GTK_ENTRY(w->i), str);
	else gtk_entry_set_text(GTK_ENTRY(w->i), "");
	gtk_editable_set_position(GTK_EDITABLE(w->i), -1);
}

static void open_uri(WebKitWebView *wv, const gchar *uri)
{
	size_t i = 0;
	gchar *temp = construct_uri(uri);
	if (temp != NULL) {
		for (i = 0; temp[i] != '\0'; ++i) if (temp[i] == ':' && ++i) break;
		if (strncmp(temp, "javascript:", i) == 0)
			eval_js(wv, temp, "local", 1);
		else
			webkit_web_view_load_uri(wv, temp);
		g_free(temp);
	}
}

static gchar * out(struct window *w, gboolean show, gchar *e)
{
	GtkTextIter ti;
	GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->con));
	if (show) set_show_console(w, (union wkb_setting) { .b = TRUE });
	gtk_text_buffer_get_end_iter(b, &ti);
	gtk_text_buffer_insert(b, &ti, e, -1);
	return e;
}

static int parse(struct window *w, WebKitWebView *wv, struct list *in_list, gchar **arg)
{
	struct list tmp_list = NEW_LIST;
	struct token *t = NULL, *tp = NULL, *tt = NULL;
	struct var *v = NULL;
	gchar *tmp;
	int esc = 0, quo = 0, ret = 0;

	for (t = (struct token *) in_list->h; t != NULL && (strcmp(t->value, " ") == 0  || strcmp(t->value, "\t") == 0); t = destroy_token(in_list, t));

	for (t = (struct token *) in_list->h; t != NULL; ) {
		if (t->parse && !quo && !esc && strcmp(t->value, ";") == 0) break;
		else if (t->parse && !esc && !quo && (strcmp(t->value, " ") == 0  || strcmp(t->value, "\t") == 0)) break;
		else if (t->parse && !esc && strcmp(t->value, "\\") == 0) {
			t = (struct token *) t->n.n;
			esc = 1;
		}
		else if (t->parse && !esc && strcmp(t->value, "\"") == 0) {
			t = (struct token *) t->n.n;
			quo = !quo;
		}
		else if (t->parse && !esc && strcmp(t->value, "{") == 0) {
			t = destroy_token(in_list, t);
			PARSE_ERROR(t == NULL, out(w, TRUE, "error: parse: unexpected EOL\n"), error1, 1);
			PARSE_ERROR(strcmp(t->value, "}") == 0, out(w, TRUE, "error: parse: bad substitution\n"), error1, 1);
			v = get_var(w, t->value);
			if (v != NULL) tmp = get_var_value(w, wv, v, WKB_VAR_CONTEXT_EXPAND);
			else tmp = g_strdup(getenv(t->value));
			t = destroy_token(in_list, t);
			PARSE_ERROR(t == NULL, out(w, TRUE, "error: parse: unexpected EOL\n"); g_free(tmp), error1, 1);
			while (strcmp(t->value, ":") == 0) {
				t = destroy_token(in_list, t);
				PARSE_ERROR(t == NULL, out(w, TRUE, "error: parse: unexpected EOL\n"); g_free(tmp), error1, 1);
				if (tmp == NULL) {
					if (strcmp(t->value, "}") != 0) {
						v = get_var(w, t->value);
						g_free(tmp);
						if (v != NULL) tmp = get_var_value(w, wv, v, WKB_VAR_CONTEXT_EXPAND);
						else tmp = g_strdup(getenv(t->value));
						t = destroy_token(in_list, t);
					}
				}
				else if (strcmp(t->value, "}") != 0) t = destroy_token(in_list, t);
				PARSE_ERROR(t == NULL, out(w, TRUE, "error: parse: unexpected EOL\n"); g_free(tmp), error1, 1);
			}
			PARSE_ERROR(strcmp(t->value, "}") != 0, out(w, TRUE, "error: parse: bad substitution\n"); g_free(tmp), error1, 1);
			if (tmp != NULL) tokenize_expansion(tmp, &tmp_list);
			g_free(tmp);
			while ((tt = (struct token *) tmp_list.t) != NULL) {
				LIST_REMOVE(&tmp_list, (struct node *) tt);
				LIST_INSERT(in_list, (struct node *) tt, (struct node *) t);
			}
			t = destroy_token(in_list, t);
		}
		else {
			t = (struct token *) t->n.n;
			esc = 0;
		}
	}
	esc = quo = 0;

	for (t = (struct token *) in_list->h; t != NULL; ) {
		if (t->parse && !quo && !esc && strcmp(t->value, ";") == 0) break;
		else if (t->parse && !esc && strcmp(t->value, "\\") == 0) {
			t = destroy_token(in_list, t);
			esc = 1;
		}
		else if (t->parse && !esc && strcmp(t->value, "\"") == 0) {
			if (t == (struct token *) in_list->h) {
				g_free(t->value);
				t->value = g_strdup("");
			}
			else t = destroy_token(in_list, t);
			quo = !quo;
		}
		else if (t->parse && !esc && !quo && (strcmp(t->value, " ") == 0  || strcmp(t->value, "\t") == 0)) break;
		else {
			t->parse = 0;
			tp = (struct token *) t->n.p;
			if (tp != NULL) {
				tmp = tp->value;
				tp->value = g_strconcat(tp->value, t->value, NULL);
				g_free(tmp);
				t = destroy_token(in_list, t);
			}
			else t = (struct token *) t->n.n;
			esc = 0;
		}
	}
	PARSE_ERROR(quo, out(w, TRUE, "error: parse: unterminated quoted string\n"), error1, 1);

	t = (struct token *) in_list->h;
	if (t == NULL) *arg = NULL;
	else if (t->parse && strcmp(t->value, ";") == 0) {
		*arg = NULL;
		destroy_token(in_list, t);
	}
	else {
		*arg = g_strdup(t->value);
		destroy_token(in_list, t);
	}
	return 0;

	error1:
	*arg = NULL;
	return ret;
}

static void parse_mod_mask(struct window *w, const gchar *mod_str, guint *mod, const char *n)
{
	int i;
	*mod = 0;
	for (i = 0; i < strlen(mod_str); ++i) {
		switch (mod_str[i]) {
			case '-': break;
			case 'S': *mod |= MOD_USE_SHIFT; *mod &= ~MOD_SHIFT; break;
			case 's': *mod |= MOD_USE_SHIFT|MOD_SHIFT; break;
			case 'c': *mod |= GDK_CONTROL_MASK; break;
			case '1': *mod |= GDK_MOD1_MASK; break;
			case '2': *mod |= GDK_MOD2_MASK; break;
			case '3': *mod |= GDK_MOD3_MASK; break;
			case '4': *mod |= GDK_MOD4_MASK; break;
			case '5': *mod |= GDK_MOD5_MASK; break;
			default:  g_free(out(w, TRUE, g_strdup_printf("%s: warning: unrecognized modifier '%c'\n", n, mod_str[i])));
		}
	}
}

static void parse_mode_mask(struct window *w, const gchar *mode_str, guint *mode, const char *n)
{
	int i;
	*mode = 0;
	for (i = 0; i < strlen(mode_str); ++i) {
		switch (mode_str[i]) {
			case '-': break;
			case 'a': *mode = MODE_ALL; break;
			case 'n': *mode |= MODE_NORMAL; break;
			case 'c': *mode |= MODE_CMD; break;
			case 'i': *mode |= MODE_INSERT; break;
			case 'p': *mode |= MODE_PASSTHROUGH; break;
			default:  g_free(out(w, TRUE, g_strdup_printf("%s: warning: unrecognized mode '%c'\n", n, mode_str[i])));
		}
	}
}

static void print_bind(struct window *w, struct bind *b)
{
	gchar str_mode[5], str_mod[8];
	get_mode_string(b->mode, str_mode);
	get_mod_string(b->mod, str_mod);
	if (b->arg == NULL) g_free(out(w, TRUE, g_strdup_printf("%s  %s  %-10s  %s\n", str_mode, str_mod, b->key, b->handler->name)));
	else g_free(out(w, TRUE, g_strdup_printf("%s  %s  %-10s  %s %s\n", str_mode, str_mod, b->key, b->handler->name, b->arg)));
}

static void print_download(struct window *w, struct download *dl)
{
	gchar *status = "Unknown", *filename = NULL;
	int width = (global.next_download_id < 11) ? 1 : (int) log10(global.next_download_id - 1) + 1;
#ifdef __HAVE_WEBKIT2__
	switch (dl->status) {
		case DOWNLOAD_STATUS_FINISHED: status = "Finished"; break;
		case DOWNLOAD_STATUS_ACTIVE: status = "Active"; break;
		case DOWNLOAD_STATUS_ERROR_NETWORK: status = "Error: network"; break;
		case DOWNLOAD_STATUS_CANCELLED: status = "Cancelled"; break;
		case DOWNLOAD_STATUS_ERROR_DEST: status = "Error: destination"; break;
	}
	filename = g_filename_from_uri(webkit_download_get_destination(dl->d), NULL, NULL);
	g_free(out(w, TRUE, g_strdup_printf("[%*d] %s\n%*s   %s %ld/%ld (%d%%) %.2lfs/%.2lfs %.2lfKiB/s\n", width, dl->id,
		filename, width, "", status, (long int) webkit_download_get_received_data_length(dl->d),
		(long int) webkit_uri_response_get_content_length(webkit_download_get_response(dl->d)),
		(int) lround(webkit_download_get_estimated_progress(dl->d) * 100), webkit_download_get_elapsed_time(dl->d),
		(webkit_download_get_estimated_progress(dl->d) == 0) ? INFINITY : webkit_download_get_elapsed_time(dl->d) / webkit_download_get_estimated_progress(dl->d),
		(webkit_download_get_elapsed_time(dl->d) == 0) ? 0 : (webkit_download_get_received_data_length(dl->d) / 1024) / webkit_download_get_elapsed_time(dl->d))));
#else
	switch (dl->status) {
		case WEBKIT_DOWNLOAD_STATUS_ERROR: status = "Error"; break;
		case WEBKIT_DOWNLOAD_STATUS_CREATED: status = "Created"; break;
		case WEBKIT_DOWNLOAD_STATUS_STARTED: status = "Active"; break;
		case WEBKIT_DOWNLOAD_STATUS_CANCELLED: status = "Cancelled"; break;
		case WEBKIT_DOWNLOAD_STATUS_FINISHED: status = "Finished"; break;
	}
	filename = g_filename_from_uri(webkit_download_get_destination_uri(dl->d), NULL, NULL);
	g_free(out(w, TRUE, g_strdup_printf("[%*d] %s\n%*s   %s %ld/%ld (%d%%) %.2lfs/%.2lfs %.2lfKiB/s\n", width, dl->id,
		filename, width, "", status, (long int) webkit_download_get_current_size(dl->d),
		(long int) webkit_download_get_total_size(dl->d), (int) lround(webkit_download_get_progress(dl->d) * 100), webkit_download_get_elapsed_time(dl->d),
		(webkit_download_get_progress(dl->d) == 0) ? INFINITY : webkit_download_get_elapsed_time(dl->d) / webkit_download_get_progress(dl->d),
		(webkit_download_get_elapsed_time(dl->d) == 0) ? 0 : (webkit_download_get_current_size(dl->d) / 1024) / webkit_download_get_elapsed_time(dl->d))));
#endif
	g_free(filename);
}

static void quit(void)
{
	while (global.windows.h != NULL) gtk_widget_destroy(((struct window *) global.windows.h)->w);
	if (gtk_main_level() != 0) gtk_main_quit();
	else exit(0);
}

static struct tab * remove_tab(struct tab *t)
{
	struct tab *tt;
	struct window *w = t->w;
	if (w->tabs.h == (struct node *) t && w->tabs.h->n != NULL)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(w->nb), gtk_notebook_page_num(GTK_NOTEBOOK(w->nb), ((struct tab *) w->tabs.h->n)->c));
	LIST_REMOVE(&w->tabs, (struct node *) t);
	gtk_notebook_remove_page(GTK_NOTEBOOK(w->nb), gtk_notebook_page_num(GTK_NOTEBOOK(w->nb), t->c));
	LIST_FOREACH(&w->tabs, tt) update_title(tt);
	return t;
}

static void set_mode(struct window *w, guint m)
{
	switch(m) {
		case MODE_NORMAL: case MODE_INSERT: case MODE_PASSTHROUGH:
			if (w->fullscreen) gtk_widget_hide(w->bar_nb);
			if (m == MODE_NORMAL) gtk_widget_hide(w->mode_l);
			else if (m == MODE_INSERT) {
				gtk_widget_show(w->mode_l);
				gtk_label_set_text(GTK_LABEL(w->mode_l), "[Insert]");
			}
			else if (m == MODE_PASSTHROUGH) {
				gtk_widget_show(w->mode_l);
				gtk_label_set_text(GTK_LABEL(w->mode_l), "[Passthrough]");
			}
			gtk_widget_set_can_focus(w->i, FALSE);
			gtk_notebook_set_current_page(GTK_NOTEBOOK(w->bar_nb), BAR_STATUS_PAGE);
			gtk_entry_set_text(GTK_ENTRY(w->i), "");
			if (w->mode == MODE_CMD && GET_CURRENT_TAB(w)) gtk_widget_grab_focus(GTK_WIDGET(GET_CURRENT_VIEW(w)));
			w->current_hist = NULL;
			break;
		case MODE_CMD:
			if (w->fullscreen) gtk_widget_show(w->bar_nb);
			gtk_notebook_set_current_page(GTK_NOTEBOOK(w->bar_nb), BAR_INPUT_PAGE);
			gtk_widget_set_can_focus(w->i, TRUE);
			gtk_widget_grab_focus(w->i);
			break;
	}
	w->mode = m;
}

static int set_var_value(struct window *w, WebKitWebView *wv, struct var *v, const gchar *new_value)
{
	if (v->handler != NULL && v->handler->set != NULL)
		return v->handler->set(w, wv, v, new_value);
	g_free(v->value);
	v->value = g_strdup(new_value);
	return 0;
}

static void signal_handler(int sig)
{
	fprintf(stderr, "wkb: signal %d; terminating...\n", sig);
	quit();
}

static void tokenize(const gchar *line, struct list *tok)
{
	int i = 0, k = 0;
	gchar s[2] = { '\0', '\0' };
	gchar *temp;

	while (line[k] == ' ' || line[k] == '\t') ++k;
	i = k;
	if (line[k] == '#') return;
	while (line[k] != '\0') {
		switch (line[k]) {
			case '\\': case '"': case '{': case '}': case ';': case ':': case ' ': case '\t':
				if (k != i) {
					temp = g_strndup(&line[i], k - i);
					append_token(tok, temp, 1);
					g_free(temp);
				}
				s[0] = line[k];
				append_token(tok, s, 1);
				i = ++k;
				break;
			default:
				++k;
		}
	}
	if (k != i) {
		temp = g_strndup(&line[i], k - i);
		append_token(tok, temp, 1);
		g_free(temp);
	}
}

static void tokenize_expansion(const gchar *value, struct list *tok)
{
	int i = 0, k = 0;
	gchar s[2] = { '\0', '\0' };
	gchar *temp;

	while (value[k] != '\0') {
		switch (value[k]) {
			case ' ': case '\t':
				if (k != i) {
					temp = g_strndup(&value[i], k - i);
					append_token(tok, temp, 0);
					g_free(temp);
				}
				s[0] = value[k];
				append_token(tok, s, 1);
				i = ++k;
				break;
			default:
				++k;
		}
	}
	if (k != i) {
		temp = g_strndup(&value[i], k - i);
		append_token(tok, temp, 0);
		g_free(temp);
	}
}

static void update_dl_l(struct window *w)
{
	int n_dl = 0;
	struct download *dl;
	struct window *window;
	GString *lt = g_string_new("[dl: ");
	LIST_FOREACH(&global.downloads, dl) {
		if (DOWNLOAD_IS_ACTIVE(dl->status)) {
			++n_dl;
			g_string_append_printf(lt, "%s%d:%d%%", (n_dl > 1) ? "," : "", dl->id,
#ifdef __HAVE_WEBKIT2__
				(int) lround(webkit_download_get_estimated_progress(dl->d) * 100));
#else
				(int) lround(webkit_download_get_progress(dl->d) * 100));
#endif
		}
	}
	g_string_append(lt, "]");
	if (w == NULL) {
		LIST_FOREACH(&global.windows, window) {
			gtk_label_set_text(GTK_LABEL(window->dl_l), lt->str);
			if (n_dl == 0) gtk_widget_hide(window->dl_l);
			else gtk_widget_show(window->dl_l);
		}
	}
	else {
		gtk_label_set_text(GTK_LABEL(w->dl_l), lt->str);
		if (n_dl == 0) gtk_widget_hide(w->dl_l);
		else gtk_widget_show(w->dl_l);
	}
	g_string_free(lt, TRUE);
}

static void update_tabs_l(struct window *w)
{
	gchar *str = g_strdup_printf("[%d/%d]", gtk_notebook_page_num(GTK_NOTEBOOK(w->nb), GET_CURRENT_TAB_CHILD(w)) + 1, gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb)));
	gtk_label_set_text(GTK_LABEL(w->tabs_l), str);
	g_free(str);
}

static void update_title(struct tab *t)
{
	WebKitWebView *wv = GET_VIEW_FROM_CHILD(t->c);
#ifdef __HAVE_WEBKIT2__
	gdouble progress = webkit_web_view_get_estimated_load_progress(wv);
#else
	gdouble progress = webkit_web_view_get_progress(wv);
#endif
	const gchar *title = webkit_web_view_get_title(wv);
	if (title == NULL || strlen(title) == 0) title = webkit_web_view_get_uri(wv);
	if (title == NULL || strlen(title) == 0) title = null_title;
	gchar *tab_title, *window_title;
	if (progress == 0.0 || progress == 1.0)
		tab_title = g_strdup_printf("%d  %s", gtk_notebook_page_num(GTK_NOTEBOOK(t->w->nb), t->c) + 1, title);
	else
		tab_title = g_strdup_printf("%d  [%d%%] %s", gtk_notebook_page_num(GTK_NOTEBOOK(t->w->nb), t->c) + 1, (int) lround(progress * 100), title);
	gtk_label_set_text(GTK_LABEL(gtk_notebook_get_tab_label(GTK_NOTEBOOK(t->w->nb), t->c)), tab_title);
	if (wv == GET_CURRENT_VIEW(t->w)) {
		window_title = g_strconcat(title, " - wkb", NULL);
		gtk_window_set_title(GTK_WINDOW(t->w->w), window_title);
		g_free(window_title);
	}
	g_free(tab_title);
}

static void update_uri(struct tab *t, const gchar *l, const gchar *s)
{
	g_free(t->uri);
	t->uri = g_strdup(s);
	g_free(t->uri_label_text);
	t->uri_label_text = g_strdup(l);
	if (t == GET_CURRENT_TAB(t->w))
		gtk_label_set_text(GTK_LABEL(t->w->uri_l), l);
}

/* Begin callback functions */

#ifdef __HAVE_WEBKIT2__
static void cb_close(WebKitWebView *wv, struct tab *t)
#else
static gboolean cb_close(WebKitWebView *wv, struct tab *t)
#endif
{
	struct window *w = t->w;
	destroy_tab(t);
	if (w->tabs.h == NULL) new_tab(w, NULL, global.homepage, false);
#ifndef __HAVE_WEBKIT2__
	return TRUE;
#endif
}

static gboolean cb_cmd_fifo_in(GIOChannel *s, GIOCondition c, struct window *w)
{
	GError *err = NULL;
	gchar *line;
	gsize t;
	int fd;
	if (c & (G_IO_IN|G_IO_PRI)) {
		switch (g_io_channel_read_line(s, &line, NULL, &t, &err)) {
		case G_IO_STATUS_ERROR:
		case G_IO_STATUS_AGAIN:
			g_error_free(err);
			return TRUE;
		case G_IO_STATUS_EOF:
			goto reopen_fifo;
		case G_IO_STATUS_NORMAL:
		default:
			line[t] = '\0';
			exec_line(w, NULL, line);
			g_free(line);
		}
	}
	else if (c & (G_IO_HUP|G_IO_ERR|G_IO_NVAL)) {
		reopen_fifo:
		g_source_remove(w->cmd_fifo_ioch_sid);
		g_io_channel_shutdown(w->cmd_fifo_ioch, FALSE, NULL);
		close(g_io_channel_unix_get_fd(w->cmd_fifo_ioch));
		g_io_channel_unref(w->cmd_fifo_ioch);
		if ((fd = open(w->cmd_fifo, O_RDONLY|O_NONBLOCK)) == -1) {
			g_free(out(w, TRUE, g_strdup_printf("cb_cmd_fifo_in: FATAL: could not open \"%s\"\n", w->cmd_fifo)));
			w->cmd_fifo_ioch = NULL;
			return TRUE;
		}
		w->cmd_fifo_ioch = g_io_channel_unix_new(fd);
		w->cmd_fifo_ioch_sid = g_io_add_watch(w->cmd_fifo_ioch, G_IO_IN|G_IO_HUP, (GIOFunc) cb_cmd_fifo_in, w);
	}
	return TRUE;
}

static void cb_console_size_allocate(WebKitWebView *wv, GdkRectangle *allocation, struct window *w)
{
	GtkAdjustment *va;
	if (w->auto_scroll) {
		va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(w->consw));
		gtk_adjustment_set_value(va, gtk_adjustment_get_upper(va) - gtk_adjustment_get_page_size(va));
	}
}

#ifdef __HAVE_WEBKIT2__
#if WEBKIT_CHECK_VERSION(2, 5, 1)
static GtkWidget * cb_create(WebKitWebView *wv, WebKitNavigationAction *na, struct tab *t)
#else
static GtkWidget * cb_create(WebKitWebView *wv, struct tab *t)
#endif
{
	if (global.allow_popups) return new_tab(t->w, WEBKIT_WEB_VIEW(webkit_web_view_new_with_related_view(wv)), NULL, false);
	else {
		msg(t->w, "cb_create(): blocked");
		return NULL;
	}
}

static gboolean cb_decide_policy(WebKitWebView *wv, WebKitPolicyDecision *d, WebKitPolicyDecisionType dt, struct tab *t)
{
#if WEBKIT_CHECK_VERSION(2, 6, 0)
	WebKitNavigationAction *na;
#else
	WebKitNavigationPolicyDecision *nd;
#endif
	switch (dt) {
		case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
#if WEBKIT_CHECK_VERSION(2, 6, 0)
			na = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
			if (webkit_navigation_action_get_mouse_button(na) == 2) {
				if (webkit_navigation_action_get_modifiers(na) & GDK_SHIFT_MASK)
					new_window(g_malloc0(sizeof(struct window)), TRUE, webkit_uri_request_get_uri(webkit_navigation_action_get_request(na)));
				else if (webkit_navigation_action_get_modifiers(na) & GDK_CONTROL_MASK)
					new_tab(t->w, NULL, webkit_uri_request_get_uri(webkit_navigation_action_get_request(na)), true);
				else
					new_tab(t->w, NULL, webkit_uri_request_get_uri(webkit_navigation_action_get_request(na)), false);
				webkit_policy_decision_ignore(d);
				return TRUE;
			}
#else
			nd = WEBKIT_NAVIGATION_POLICY_DECISION(d);
			if (webkit_navigation_policy_decision_get_mouse_button(nd) == 2) {
				if (webkit_navigation_policy_decision_get_modifiers(nd) & GDK_SHIFT_MASK)
					new_window(g_malloc0(sizeof(struct window)), TRUE, webkit_uri_request_get_uri(webkit_navigation_policy_decision_get_request(nd)));
				else
					new_tab(t->w, NULL, webkit_uri_request_get_uri(webkit_navigation_policy_decision_get_request(nd)), false);
				webkit_policy_decision_ignore(d);
				return TRUE;
			}
#endif
			break;
		case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION:
#if WEBKIT_CHECK_VERSION(2, 6, 0)
			na = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
			if (webkit_navigation_action_get_navigation_type(na) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
				webkit_policy_decision_ignore(d);
#else
			nd = WEBKIT_NAVIGATION_POLICY_DECISION(d);
			if (webkit_navigation_policy_decision_get_navigation_type(nd) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
				webkit_policy_decision_ignore(d);
#endif
			break;
		case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:
			if (!webkit_web_view_can_show_mime_type(wv, webkit_uri_response_get_mime_type(webkit_response_policy_decision_get_response(WEBKIT_RESPONSE_POLICY_DECISION(d)))))
				webkit_policy_decision_download(d);
			break;
	}
	return FALSE;
}
#else
static GtkWidget * cb_create(WebKitWebView *wv, WebKitWebFrame *f, struct tab *t)
{
	if (global.allow_popups) return new_tab(t->w, NULL, NULL, false);
	else {
		msg(t->w, "cb_create(): blocked");
		return NULL;
	}
}

static gboolean cb_decide_mime(WebKitWebView *wv, WebKitWebFrame *f, WebKitNetworkRequest *r, gchar *mime, WebKitWebPolicyDecision *d, struct tab *t)
{
	if (webkit_web_view_can_show_mime_type(wv, mime)) webkit_web_policy_decision_use(d);
	else webkit_web_policy_decision_download(d);
	return TRUE;
}

static gboolean cb_decide_policy(WebKitWebView *wv, WebKitWebFrame *f, WebKitNetworkRequest *r, WebKitWebNavigationAction *na, WebKitWebPolicyDecision *d, struct tab *t)
{
	if (webkit_web_navigation_action_get_button(na) == 2) {
		if (webkit_web_navigation_action_get_modifier_state(na) & GDK_SHIFT_MASK)
			new_window(g_malloc0(sizeof(struct window)), TRUE, webkit_network_request_get_uri(r));
		else if (webkit_web_navigation_action_get_modifier_state(na) & GDK_CONTROL_MASK)
			new_tab(t->w, NULL, webkit_network_request_get_uri(r), true);
		else
			new_tab(t->w, NULL, webkit_network_request_get_uri(r), false);
		webkit_web_policy_decision_ignore(d);
		return TRUE;
	}
	return FALSE;
}

static gboolean cb_decide_window(WebKitWebView *wv, WebKitWebFrame *f, WebKitNetworkRequest *r, WebKitWebNavigationAction *na, WebKitWebPolicyDecision *d, struct tab *t)
{
	if (webkit_web_navigation_action_get_reason(na) == WEBKIT_WEB_NAVIGATION_REASON_LINK_CLICKED) {
		new_tab(t->w, NULL, webkit_network_request_get_uri(r), false);
		webkit_web_policy_decision_ignore(d);
		return TRUE;
	}
	return FALSE;
}
#endif

static void cb_destroy(WebKitWebView *wv, struct window *w)
{
	destroy_cmd_fifo(w);
	LIST_REMOVE(&global.windows, (struct node *) w);
	while (w->tabs.h != NULL) destroy_tab((struct tab *) w->tabs.h);
	g_free(w->tmp_line);
	g_free(w->find_string);
	g_free(w);
	if (global.windows.h == NULL) quit();
}

#ifdef __HAVE_WEBKIT2__
static void cb_download(WebKitWebContext *c, WebKitDownload *d, void *v)
{
	g_signal_connect(d, "decide-destination", G_CALLBACK(cb_download_decide_destination), NULL);
}

static gboolean cb_download_decide_destination(WebKitDownload *d, gchar *suggested_filename, void *v)
{
	if (new_download(d, suggested_filename) == NULL) webkit_download_cancel(d);
	else update_dl_l(NULL);
	return TRUE;
}

static void cb_download_failed(WebKitDownload *d, GError *e, struct download *dl)
{
	struct window *wn;
	dl->status = e->code;
	switch (dl->status) {
	case DOWNLOAD_STATUS_ERROR_NETWORK:
		LIST_FOREACH(&global.windows, wn)
			g_free(msg(wn, g_strdup_printf("download %d failed: network error", dl->id)));
		break;
	case DOWNLOAD_STATUS_ERROR_DEST:
		LIST_FOREACH(&global.windows, wn)
			g_free(msg(wn, g_strdup_printf("download %d failed: destination error", dl->id)));
		break;
	}
}

static void cb_download_finished(WebKitDownload *d, struct download *dl)
{
	if (DOWNLOAD_IS_ACTIVE(dl->status)) {
		dl->status = DOWNLOAD_STATUS_FINISHED;
		if (dl->auto_open)
			open_download((struct window *) global.windows.h, NULL, dl, global.dl_open_cmd);  /* FIXME */
	}
	update_dl_l(NULL);
}
#else
static void cb_document_load_finished(WebKitWebView *wv, WebKitWebFrame *f, struct tab *t)
{
	if (global.dom_ready != NULL && f == webkit_web_view_get_main_frame(wv))
		exec_line(t->w, wv, global.dom_ready);
}

static gboolean cb_download(WebKitWebView *wv, WebKitDownload *d, void *v)
{
	if (new_download(d, webkit_download_get_suggested_filename(d)) == NULL)
		return FALSE;
	return TRUE;
}

static void cb_download_status_changed(WebKitDownload *d, GParamSpec *p, struct download *dl)
{
	struct window *wn;
	dl->status = webkit_download_get_status(d);
	if (dl->status == WEBKIT_DOWNLOAD_STATUS_FINISHED && dl->auto_open)
		open_download((struct window *) global.windows.h, NULL, dl, global.dl_open_cmd);  /* FIXME */
	else if (dl->status == WEBKIT_DOWNLOAD_STATUS_ERROR)
		LIST_FOREACH(&global.windows, wn)
			g_free(msg(wn, g_strdup_printf("download %d failed", dl->id)));
	update_dl_l(NULL);
}
#endif

static void cb_download_progress_changed(WebKitDownload *d, GParamSpec *p, struct download *dl)
{
	update_dl_l(NULL);
}

#ifdef __HAVE_WEBKIT2__
static gboolean cb_enter_fullscreen(WebKitWebView *wv, struct tab *t)
#else
static gboolean cb_enter_fullscreen(WebKitWebView *wv, WebKitDOMHTMLElement *e, struct tab *t)
#endif
{
	fullscreen_mode(t->w, TRUE);
	return FALSE;
}

static void cb_input_end(WebKitWebView *wv, struct window *w)
{
	gchar *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(w->i)));
	set_mode(w, MODE_NORMAL);
	g_free(out(w, FALSE, g_strdup_printf("+ %s\n", text)));
	exec_line(w, NULL, text);
	if (text[0] != '\0' && text[0] != ' ' && (global.hist.t == NULL || strcmp(((struct hist *) global.hist.t)->line, text) != 0))
		new_hist(w, text);
	g_free(text);
}

#ifndef __HAVE_WEBKIT2__
static GtkWidget * cb_inspect_web_view(GtkWidget *i, GtkWidget *wv, struct tab *t)
{
	GtkWidget *window, *sw, *new_wv;
	new_wv = webkit_web_view_new();
	webkit_web_view_set_settings(WEBKIT_WEB_VIEW(new_wv), global.settings);
	sw = gtk_scrolled_window_new(NULL, NULL);
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_container_add(GTK_CONTAINER(sw), new_wv);
	gtk_window_set_title(GTK_WINDOW(window), "Inspector");
	gtk_window_set_default_size(GTK_WINDOW(window), global.default_width, global.default_height);
	gtk_container_add(GTK_CONTAINER(window), sw);
	gtk_widget_show_all(window);
	return new_wv;
}
#endif

static gboolean cb_keypress(WebKitWebView *wv, GdkEventKey *ev, struct window *w)
{
	struct bind *b;
	gchar str_mod[8];
	if (w->print_keyval)
		g_free(out(w, FALSE, g_strdup_printf("modifiers: %s | key: \"%s\"\n", get_mod_string(CLEAN_MASK(ev->state, ev->keyval), str_mod), gdk_keyval_name(ev->keyval))));
	if (gdk_keyval_name(ev->keyval) != NULL) {
		LIST_FOREACH(&global.binds, b) {
			if (b->mode & w->mode
					&& modmask_compare(b->mod, ev->state)
					&& strcmp(b->key, gdk_keyval_name(ev->keyval)) == 0) {
				return b->handler->func(w, b);
			}
		}
	}
	/* pass through in cmd, insert, and passthrough modes */
	if (w->mode == MODE_CMD || w->mode == MODE_INSERT || w->mode == MODE_PASSTHROUGH)
		return FALSE;
	return TRUE;
}

#ifdef __HAVE_WEBKIT2__
static gboolean cb_leave_fullscreen(WebKitWebView *wv, struct tab *t)
#else
static gboolean cb_leave_fullscreen(WebKitWebView *wv, WebKitDOMHTMLElement *e, struct tab *t)
#endif
{
	fullscreen_mode(t->w, FALSE);
	return FALSE;
}

#ifdef __HAVE_WEBKIT2__
static void cb_load_changed(WebKitWebView *wv, WebKitLoadEvent e, struct tab *t)
{
	switch (e) {
		case WEBKIT_LOAD_COMMITTED:
			if (global.load_started != NULL) exec_line(t->w, wv, global.load_started);
			break;
		case WEBKIT_LOAD_FINISHED:
			if (global.dom_ready != NULL) exec_line(t->w, wv, global.dom_ready);
			if (global.load_finished != NULL) exec_line(t->w, wv, global.load_finished);
			break;
		default:
			break;
	}
}

static void cb_mouse_target_changed(WebKitWebView *wv, WebKitHitTestResult *ht, guint mod, struct tab *t)
{
	gchar *temp;
	if (webkit_hit_test_result_context_is_link(ht)) {
		update_uri(t, temp = g_strconcat("Link: ", webkit_hit_test_result_get_link_uri(ht), NULL), webkit_hit_test_result_get_link_uri(ht));
		g_free(temp);
	}
	else if (webkit_hit_test_result_context_is_media(ht)) {
		update_uri(t, temp = g_strconcat("Media: ", webkit_hit_test_result_get_media_uri(ht), NULL), webkit_hit_test_result_get_media_uri(ht));
		g_free(temp);
	}
	else
		update_uri(t, webkit_web_view_get_uri(wv), webkit_web_view_get_uri(wv));
}
#else
static void cb_load_changed(WebKitWebView *wv, GParamSpec *p, struct tab *t)
{
	switch (webkit_web_view_get_load_status(wv)) {
		case WEBKIT_LOAD_COMMITTED:
			if (global.load_started != NULL) exec_line(t->w, wv, global.load_started);
			break;
		case WEBKIT_LOAD_FINISHED:
			if (global.load_finished != NULL) exec_line(t->w, wv, global.load_finished);
			break;
		default:
			break;
	}
}

static void cb_mouse_target_changed(WebKitWebView *wv, gchar *title, gchar *uri, struct tab *t)
{
	gchar *temp;
	if (uri != NULL) {
		update_uri(t, temp = g_strconcat("Link: ", uri, NULL), uri);
		g_free(temp);
	}
	else
		update_uri(t, webkit_web_view_get_uri(wv), webkit_web_view_get_uri(wv));
}
#endif

static void cb_progress_changed(WebKitWebView *wv, GParamSpec *p, struct tab *t)
{
	update_title(t);
}

static void cb_tab_changed(WebKitWebView *wv, GtkWidget *page, guint page_num, struct window *w)
{
	struct tab *t = NULL;
	LIST_FOREACH(&w->tabs, t) {
		if (t->c == page) {
			LIST_REMOVE(&w->tabs, (struct node *) t);
			LIST_ADD_HEAD(&w->tabs, (struct node *) t);
			gtk_label_set_text(GTK_LABEL(w->uri_l), GET_CURRENT_TAB(w)->uri_label_text);
			update_title(t);
			update_tabs_l(w);
		}
	}
}

static void cb_title_changed(WebKitWebView *wv, GParamSpec *p, struct tab *t)
{
	update_title(t);
}

static void cb_uri_changed(WebKitWebView *wv, GParamSpec *p, struct tab *t)
{
	update_uri(t, webkit_web_view_get_uri(wv), webkit_web_view_get_uri(wv));
	update_title(t);
}

/* Begin bind functions */

static gboolean bind_hist_next(struct window *w, struct bind *b)
{
	if (w->current_hist == NULL) return TRUE;
	w->current_hist = (struct hist *) w->current_hist->n.n;
	if (w->current_hist == NULL)
		gtk_entry_set_text(GTK_ENTRY(w->i), (w->tmp_line != NULL) ? w->tmp_line : "");
	else
		gtk_entry_set_text(GTK_ENTRY(w->i), w->current_hist->line);
	gtk_editable_set_position(GTK_EDITABLE(w->i), -1);
	return TRUE;
}

static gboolean bind_hist_prev(struct window *w, struct bind *b)
{
	if (w->current_hist == NULL) {
		w->current_hist = (struct hist *) global.hist.t;
		g_free(w->tmp_line);
		w->tmp_line = g_strdup(gtk_entry_get_text(GTK_ENTRY(w->i)));
	}
	else if (w->current_hist->n.p != NULL)
		w->current_hist = (struct hist *) w->current_hist->n.p;
	else return TRUE;
	if (w->current_hist == NULL)
		gtk_entry_set_text(GTK_ENTRY(w->i), (w->tmp_line != NULL) ? w->tmp_line : "");
	else
		gtk_entry_set_text(GTK_ENTRY(w->i), w->current_hist->line);
	gtk_editable_set_position(GTK_EDITABLE(w->i), -1);
	return TRUE;
}

static gboolean bind_pass(struct window *w, struct bind *b)
{
	return FALSE;
}

static gboolean bind_run(struct window *w, struct bind *b)
{
	exec_line(w, NULL, b->arg);
	return TRUE;
}

/* Begin command functions */

#ifdef __HAVE_WEBKIT2__
static int cmd_add_ss(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	gchar **whitelist, **blacklist;
	if (argc != 5) {
		out(w, TRUE, c->usage);
		return 1;
	}
	whitelist = (strlen(argv[3]) > 0) ? g_strsplit(argv[3], ",", -1) : NULL;
	blacklist = (strlen(argv[4]) > 0) ? g_strsplit(argv[4], ",", -1) : NULL;
#if WEBKIT_CHECK_VERSION(2, 5, 1)
	webkit_user_content_manager_add_style_sheet(global.cm,
		webkit_user_style_sheet_new(argv[1], WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
		WEBKIT_USER_STYLE_LEVEL_USER, (const gchar * const *) whitelist, (const gchar * const *) blacklist));
#else
	webkit_web_view_group_add_user_style_sheet(webkit_web_view_get_group(wv),
		argv[1], (strlen(argv[2]) > 0) ? argv[2] : NULL, (const gchar * const *) whitelist,
		(const gchar * const *) blacklist, WEBKIT_INJECTED_CONTENT_FRAMES_ALL);
#endif
	g_strfreev(whitelist);
	g_strfreev(blacklist);
	return 0;
}
#endif

static int cmd_alias(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	gchar *ill;
	struct alias *a = NULL;
	if (argc <= 1) {
		LIST_FOREACH(&global.aliases, a)
			g_free(out(w, TRUE, g_strdup_printf("%s=%s\n", a->name, a->value)));
		return 0;
	}
	else if (argc > 3) {
		out(w, TRUE, c->usage);
		return 1;
	}
	if ((ill = strpbrk(argv[1], " \t")) != NULL) {
		g_free(out(w, TRUE, g_strdup_printf("%s: error: illegal character '%c' in name\n", argv[0], *ill)));
		return 1;
	}
	if (argv[1][0] == '!') {
		g_free(out(w, TRUE, g_strdup_printf("%s: error: illegal first character '%c' in name\n", argv[0], argv[1][0])));
		return 1;
	}
	if (argc == 2) {
		a = get_alias(w, argv[1]);
		if (a != NULL) g_free(out(w, TRUE, g_strdup_printf("%s=%s\n", a->name, a->value)));
		else g_free(out(w, TRUE, g_strdup_printf("%s: alias not found: \"%s\"\n", argv[0], argv[1])));
		return 0;
	}
	a = get_alias(w, argv[1]);
	if (a != NULL) destroy_alias(w, a);
	new_alias(w, argv[1], argv[2]);
	return 0;
}

static int cmd_bind(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	guint mode = 0, mod = 0;
	struct bind *b = NULL, *first_bind = NULL, *new = NULL;
	struct bind_handler *bh = NULL;

	if (argc <= 1) {
		for (i = 0; i < LENGTH(bind_handlers); ++i) g_free(out(w, TRUE, g_strdup_printf("handler: %s\n", bind_handlers[i].name)));
		out(w, TRUE, "\n");
		LIST_FOREACH(&global.binds, b) print_bind(w, b);
		return 0;
	}
	else if (argc > 6 || argc < 4) {
		out(w, TRUE, c->usage);
		return 1;
	}
	parse_mode_mask(w, argv[1], &mode, argv[0]);
	parse_mod_mask(w, argv[2], &mod, argv[0]);
	if (!mode) {
		g_free(out(w, TRUE, g_strdup_printf("%s: error: no valid modes given: \"%s\"\n", argv[0], argv[1])));
		return 1;
	}
	if (argc == 4) {
		b = first_bind = get_bind(w, mode, mod, argv[3]);
		while (b != NULL) {
			print_bind(w, b);
			mode &= ~b->mode;
			b = get_bind(w, mode, mod, argv[3]);
			if (b == first_bind) break;
		}
		if (first_bind == NULL) g_free(out(w, TRUE, g_strdup_printf("%s: no matching binds found\n", argv[0])));
		return 0;
	}
	for (i = 0; i < LENGTH(bind_handlers); ++i) {
		if (strcmp(argv[4], bind_handlers[i].name) == 0) {
			bh = &bind_handlers[i];
			break;
		}
	}
	if (bh == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("%s: error: no such handler \"%s\"\n", argv[0], argv[4])));
		return 1;
	}
	b = get_bind(w, mode, mod, argv[3]);
	while (b != NULL) {
		b->mode &= ~mode;
		if (b->mode == 0) destroy_bind(w, b);
		b = get_bind(w, mode, mod, argv[3]);
	}
	new = new_bind(w);
	new->mode = mode;
	new->mod = mod;
	new->key = g_strdup(argv[3]);
	new->handler = bh;
	if (argc == 6) new->arg = g_strdup(argv[5]);
	else new->arg = NULL;
	b = get_bind(w, ~mode, mod, argv[3]);
	if ((b != NULL && b != new)
			&& ((argc == 6 && b->handler == new->handler && b->arg != NULL && strcmp(b->arg, new->arg) == 0)
				|| (argc == 5 && b->handler == new->handler && b->arg == NULL))) {
		new->mode |= b->mode;
		destroy_bind(w, b);
	}
	return 0;
}

static int cmd_clear(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->con)), "", -1);
	return 0;
}

#ifdef __HAVE_WEBKIT2__
static int cmd_clear_cache(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	webkit_web_context_clear_cache(webkit_web_view_get_context(wv));
	return 0;
}

static int cmd_clear_ss(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
#if WEBKIT_CHECK_VERSION(2, 5, 1)
	webkit_user_content_manager_remove_all_style_sheets(global.cm);
#else
	webkit_web_view_group_remove_all_user_style_sheets(webkit_web_view_get_group(wv));
#endif
	return 0;
}
#endif

static int cmd_dl_cancel(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	struct download *dl = NULL;
	if (argc > 1) {
		for (i = 1; i < argc; ++i) {
			dl = get_download(atoi(argv[i]));
			if (dl != NULL) { if (DOWNLOAD_IS_ACTIVE(dl->status)) webkit_download_cancel(dl->d); }
			else g_free(out(w, TRUE, g_strdup_printf("%s: warning: download %s not found\n", argv[0], argv[i])));
		}
	}
	else LIST_FOREACH(&global.downloads, dl) if (DOWNLOAD_IS_ACTIVE(dl->status)) webkit_download_cancel(dl->d);
	return 0;
}

static int cmd_dl_clear(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	struct download *dl = NULL;
	if (argc > 1) {
		for (i = 1; i < argc; ++i) {
			dl = get_download(atoi(argv[i]));
			if (dl != NULL) destroy_download(dl);
			else g_free(out(w, TRUE, g_strdup_printf("%s: warning: download %s not found\n", argv[0], argv[i])));
		}
	}
	else while (global.downloads.h != NULL) destroy_download((struct download *) global.downloads.h);
	return 0;
}

static int cmd_dl_new(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str;
	gchar *uri;
	if (argc <= 1) str = g_string_new(GET_CURRENT_TAB(w)->uri);
	else str = concat_args(argc, argv);
	uri = construct_uri(str->str);
	g_string_free(str, TRUE);
#ifdef __HAVE_WEBKIT2__
	webkit_web_context_download_uri(webkit_web_view_get_context(wv), uri);
#else
	WebKitDownload *d = webkit_download_new(webkit_network_request_new(uri));
	struct download *dl = new_download(d, webkit_download_get_suggested_filename(d));
	if (dl != NULL) webkit_download_start(dl->d);
#endif
	g_free(uri);
	return 0;
}

static int cmd_dl_open(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	struct download *dl = NULL;
	if (argc > 1) {
		for (i = 1; i < argc; ++i) {
			dl = get_download(atoi(argv[i]));
			if (dl != NULL) open_download(w, wv, dl, global.dl_open_cmd);
			else g_free(out(w, TRUE, g_strdup_printf("%s: warning: download %s not found\n", argv[0], argv[i])));
		}
	}
	else {
		out(w, TRUE, c->usage);
		return 1;
	}
	return 0;
}

static int cmd_dl_status(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	struct download *dl = NULL;
	if (argc > 1) {
		for (i = 1; i < argc; ++i) {
			dl = get_download(atoi(argv[i]));
			if (dl != NULL) print_download(w, dl);
			else g_free(out(w, TRUE, g_strdup_printf("%s: warning: download %s not found\n", argv[0], argv[i])));
		}
	}
	else LIST_FOREACH(&global.downloads, dl) print_download(w, dl);
	return 0;
}

static int cmd_echo(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	gboolean show = (argv[0][0] != 'n');
	for (i = 1; i < argc - 1; ++i) g_free(out(w, show, g_strdup_printf("%s ", argv[i])));
	if (argc > 1) g_free(out(w, show, g_strdup_printf("%s\n", argv[i])));
	else out(w, show, "\n");
	return 0;
}

static int cmd_find(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str;
	if (argc > 1) {
		str = concat_args(argc, argv);
		set_find_string(w, (union wkb_setting) { .s = (strlen(str->str) > 0) ? str->str : NULL });
		g_string_free(str, TRUE);
	}
#ifdef __HAVE_WEBKIT2__
	WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
	webkit_find_controller_search(fc, (w->find_string == NULL) ? "": w->find_string, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE|((argv[0][0] == 'r') ? WEBKIT_FIND_OPTIONS_BACKWARDS : 0)|WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
#else
	webkit_web_view_unmark_text_matches(wv);
	if (w->find_string != NULL) {
		webkit_web_view_mark_text_matches(wv, w->find_string, FALSE, 0);
		webkit_web_view_set_highlight_text_matches(wv, TRUE);
		webkit_web_view_search_text(wv, w->find_string, FALSE, (argv[0][0] == 'r') ? FALSE : TRUE, TRUE);
	}
#endif
	return 0;
}

static int cmd_help(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	if (argc > 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	else if (argc <= 1) {
		out(w, TRUE, "Available commands:\n");
		for (i = 0; i < LENGTH(commands); ++i) g_free(out(w, TRUE, g_strdup_printf("    %s\n", commands[i].name)));
		out(w, TRUE, c->usage);
	}
	else if (argc == 2) {
		for (i = 0; i < LENGTH(commands); ++i) {
			if (strcmp(argv[1], commands[i].name) == 0) {
				out(w, TRUE, commands[i].desc);
				out(w, TRUE, commands[i].usage);
				break;
			}
		}
		if (i == LENGTH(commands)) {
			g_free(out(w, TRUE, g_strdup_printf("%s: error: no such command \"%s\"\n", argv[0], argv[1])));
			return 0;
		}
	}
	return 0;
}

static int cmd_js(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	if (argc < 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	GString *str = concat_args(argc, argv);
	eval_js(wv, str->str, "local", 1);
	g_string_free(str, TRUE);
	return 0;
}

static int cmd_js_file(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	gchar *contents;
	GError *err = NULL;
	if (argc < 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	for (i = 1; i < argc; ++i) {
		if (g_file_get_contents(argv[i], &contents, NULL, &err)) {
			eval_js(wv, contents, argv[i], 0);
			g_free(contents);
		}
		else {
			g_free(out(w, TRUE, g_strdup_printf("%s: error: g_file_get_contents: %s\n", argv[0], err->message)));
			g_error_free(err);
			err = NULL;
		}
	}
	return 0;
}

static int cmd_last(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	if (w->tabs.h->n != NULL)
		gtk_notebook_set_current_page(GTK_NOTEBOOK(w->nb), gtk_notebook_page_num(GTK_NOTEBOOK(w->nb), ((struct tab *) w->tabs.h->n)->c));
	return 0;
}

static int cmd_loadconfig(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i, ret = 0, done = 0;
	gchar *cf_path, *contents, *start_line, *end_line;
	GError *err = NULL;
	if (argc < 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	for (i = 1; i < argc && !done; ++i) {
		if (global.config_dir != NULL && argv[i][0] != '/') cf_path = g_strconcat(global.config_dir, "/", argv[i], NULL);
		else cf_path = g_strdup(argv[i]);
		if (g_file_get_contents(cf_path, &contents, NULL, &err)) {
			start_line = contents;
			while (start_line != NULL && !done) {
				end_line = strchr(start_line, '\n');
				if (end_line != NULL) {
					*end_line = '\0';
					++end_line;
				}
				ret = exec_line(w, wv, start_line);
				if (ret == WKB_CMD_RET_WINDOW_DESTROYED)
					done = 1;
				start_line = end_line;
			}
		}
		else {
			g_free(cf_path);
			g_free(out(w, TRUE, g_strdup_printf("%s: error: g_file_get_contents: %s\n", argv[0], err->message)));
			g_error_free(err);
			err = NULL;
		}
	}
	return ret;
}

static int cmd_move(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int id;
	struct tab *t = GET_CURRENT_TAB(w);
	struct window *wn = NULL;

	if (argc > 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	else if (argc == 2) {
		if (strcmp(argv[1], "first") == 0) wn = (struct window *) global.windows.h;
		else if (strcmp(argv[1], "last") == 0) wn = (struct window *) global.windows.t;
		else {
			id = atoi(argv[1]);
			LIST_FOREACH(&global.windows, wn) if (wn->id == id) break;
			if (wn == NULL) {
				g_free(out(w, TRUE, g_strdup_printf("%s: error: window %s not found\n", argv[0], argv[1])));
				return 1;
			}
		}
	}

	g_object_ref(G_OBJECT(t->c));
	remove_tab(t);
	global.show_window = FALSE;
	if (wn == NULL) wn = new_window(g_malloc0(sizeof(struct window)), false, NULL);
	if (w->tabs.h == NULL && wn != w) gtk_widget_destroy(w->w);
	else update_tabs_l(w);
	t->w = wn;
	LIST_ADD_TAIL(&wn->tabs, (struct node *) t);
	gtk_notebook_insert_page(GTK_NOTEBOOK(wn->nb), t->c, gtk_label_new(NULL), -1);
	gtk_container_child_set(GTK_CONTAINER(wn->nb), t->c, "tab-expand", TRUE, NULL);
#ifdef __HAVE_GTK3__
	gtk_widget_set_halign(gtk_notebook_get_tab_label(GTK_NOTEBOOK(wn->nb), t->c), GTK_ALIGN_START);
	gtk_widget_set_valign(gtk_notebook_get_tab_label(GTK_NOTEBOOK(wn->nb), t->c), GTK_ALIGN_CENTER);
#else
	gtk_misc_set_alignment(GTK_MISC(gtk_notebook_get_tab_label(GTK_NOTEBOOK(wn->nb), t->c)), 0, 0.5);
#endif
	gtk_label_set_ellipsize(GTK_LABEL(gtk_notebook_get_tab_label(GTK_NOTEBOOK(wn->nb), t->c)), PANGO_ELLIPSIZE_END);
	g_object_unref(G_OBJECT(t->c));
	if (wn->tabs.h == (struct node *) t) gtk_widget_grab_focus(GTK_WIDGET(GET_CURRENT_VIEW(wn)));
	LIST_FOREACH(&wn->tabs, t) update_title(t);
	global.show_window = TRUE;
	update_tabs_l(wn);
	gtk_widget_show(wn->w);

	return 0;
}

static int cmd_msg(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str = concat_args(argc, argv);
	if (strlen(str->str) > 0) msg(w, str->str);
	g_string_free(str, TRUE);
	return 0;
}

static int cmd_nav(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	if (argc != 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	navigate(w, wv, atoi(argv[1]));
	return 0;
}

static int cmd_open_input(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str = concat_args(argc, argv), *e;
	if (argv[0][0] == 'e') {
		e = escape_string(str->str);
		open_input(w, e->str);
		g_string_free(e, TRUE);
	}
	else open_input(w, str->str);
	g_string_free(str, TRUE);
	return 0;
}

static int cmd_open(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str;
	if (argc <= 1) open_uri(wv, global.homepage);
	else {
		str = concat_args(argc, argv);
		open_uri(wv, str->str);
		g_string_free(str, TRUE);
	}
	return 0;
}

static int cmd_print(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
#ifdef __HAVE_WEBKIT2__
	WebKitPrintOperation *p = webkit_print_operation_new(wv);
	if (webkit_print_operation_run_dialog(p, GTK_WINDOW(w->w)) == WEBKIT_PRINT_OPERATION_RESPONSE_PRINT) webkit_print_operation_print(p);
#else
	webkit_web_frame_print(webkit_web_view_get_main_frame(wv));
#endif
	return 0;
}

static int cmd_reload(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	webkit_web_view_reload(wv);
	return 0;
}

static int cmd_reload_nc(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	webkit_web_view_reload_bypass_cache(wv);
	return 0;
}

static int cmd_reorder(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	struct tab *t;
	int p, n;
	if (argc != 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb));
	if (argv[1][0] == '+' || argv[1][0] == '-') {
		p = (gtk_notebook_page_num(GTK_NOTEBOOK(w->nb), ((struct tab *) w->tabs.h)->c) + atoi(argv[1])) % n;
		if (p < 0) p = n + p;
		else if (p >= n) p = p - n;
	}
	else {
		if (argv[1][0] == 'e')
			p = n - 1;
		else {
			p = atoi(argv[1]) - 1;
			if (p < 0) p = 0;
			else if (p >= n) p = n - 1;
		}
	}
	gtk_notebook_reorder_child(GTK_NOTEBOOK(w->nb), ((struct tab *) w->tabs.h)->c, p);
	LIST_FOREACH(&w->tabs, t) update_title(t);
	update_tabs_l(w);
	return 0;
}

static int cmd_quit(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	quit();
	return 0;
}

static int cmd_set(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	gchar *ill, *temp;
	struct var *v = NULL;

	if (argc <= 1) {
		LIST_FOREACH(&global.vars, v) {
			temp = get_var_value(w, wv, v, WKB_VAR_CONTEXT_DISP);
			g_free(out(w, TRUE, g_strdup_printf("%s=%s\n", v->name, (temp == NULL) ? "" : temp)));
			g_free(temp);
		}
		return 0;
	}
	else if (argc < 2 || argc > 3) {
		out(w, TRUE, c->usage);
		return 1;
	}
	if (argv[1][0] == '\0') {
		g_free(out(w, TRUE, g_strdup_printf("%s: error: zero length name\n", argv[0])));
		return 1;
	}
	if ((ill = strpbrk(argv[1], "\\\"{};: \t")) != NULL) {
		g_free(out(w, TRUE, g_strdup_printf("%s: error: illegal character '%c' in name\n", argv[0], *ill)));
		return 1;
	}
	if (argc == 2) {
		v = get_var(w, argv[1]);
		if (v != NULL) {
			temp = get_var_value(w, wv, v, WKB_VAR_CONTEXT_DISP);
			g_free(out(w, TRUE, g_strdup_printf("%s=%s\n", v->name, (temp == NULL) ? "" : temp)));
			g_free(temp);
		}
		else g_free(out(w, TRUE, g_strdup_printf("%s: error: var \"%s\" does not exist\n", argv[0], argv[1])));
		return 0;
	}
	v = get_var(w, argv[1]);
	if (v == NULL) {
		if (argv[0][0] == 'n') {
			g_free(out(w, TRUE, g_strdup_printf("%s: error: var \"%s\" does not exist\n", argv[0], argv[1])));
			return 1;
		}
		else v = new_var(w, argv[1], 1);
	}
	set_var_value(w, wv, v, (strlen(argv[2]) > 0) ? argv[2] : NULL);
	return 0;
}

static int cmd_set_mode(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	if (argc != 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	if      (strcmp(argv[1], "n") == 0) set_mode(w, MODE_NORMAL);
	else if (strcmp(argv[1], "c") == 0) set_mode(w, MODE_CMD);
	else if (strcmp(argv[1], "i") == 0) set_mode(w, MODE_INSERT);
	else if (strcmp(argv[1], "p") == 0) set_mode(w, MODE_PASSTHROUGH);
	else {
		g_free(out(w, TRUE, g_strdup_printf("%s: error: invalid mode \"%s\"\n", argv[0], argv[1])));
		return 1;
	}
	return 0;
}

static int cmd_spawn(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int i;
	gchar **exec_argv, *standard_out, *standard_error, *start_line, *end_line;
	gint status;
	GError *err = NULL;
	if (argc <= 1) {
		out(w, TRUE, c->usage);
		return 1;
	}
	exec_argv = g_malloc(sizeof(gchar *) * argc);
	for (i = 0; i < argc - 1; ++i) exec_argv[i] = argv[i + 1];
	exec_argv[i] = NULL;
	if (strcmp(argv[0], "spawn-sync") == 0) {
		if (g_spawn_sync(NULL, exec_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &standard_out, &standard_error, &status, &err) == FALSE) {
			g_free(out(w, TRUE, g_strdup_printf("%s: error: g_spawn_sync: %s\n", argv[0], err->message)));
			g_error_free(err);
		}
		if (status) g_free(out(w, TRUE, g_strdup_printf("%s: info: exit status = %d\n", argv[0], status)));
		if (standard_error != NULL && standard_error[0] != '\0')
			g_free(out(w, TRUE, g_strdup_printf("%s: info: stderr:\n%s", argv[0], standard_error)));
		if (standard_out != NULL && standard_out[0] != '\0') {
			start_line = standard_out;
			while (start_line != NULL) {
				end_line = strchr(start_line, '\n');
				if (end_line != NULL) {
					*end_line = '\0';
					++end_line;
				}
				exec_line(w, wv, start_line);
				start_line = end_line;
			}
		}
		g_free(standard_out);
		g_free(standard_error);
	}
	else {
		if (g_spawn_async(NULL, exec_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err) == FALSE) {
			g_free(out(w, TRUE, g_strdup_printf("%s: error: g_spawn_async: %s\n", argv[0], err->message)));
			g_error_free(err);
		}
	}
	g_free(exec_argv);
	return 0;
}

static int cmd_stop(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	webkit_web_view_stop_loading(wv);
	return 0;
}

static int cmd_switch(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	int n, t;
	if (argc != 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->nb));
	if (argv[1][0] == '+' || argv[1][0] == '-') {
		t = (gtk_notebook_page_num(GTK_NOTEBOOK(w->nb), ((struct tab *) w->tabs.h)->c) + atoi(argv[1])) % n;
		if (t < 0) t = n + t;
		else if (t >= n) t = t - n;
	}
	else {
		if (argv[1][0] == 'e')
			t = n - 1;
		else {
			t = atoi(argv[1]) - 1;
			if (t < 0) t = 0;
			if (t >= n) t = n - 1;
		}
	}
	gtk_notebook_set_current_page(GTK_NOTEBOOK(w->nb), t);
	return 0;
}

static int cmd_tclose(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	destroy_tab(GET_CURRENT_TAB(w));
	if (w->tabs.h == NULL) new_tab(w, NULL, global.homepage, false);
	update_tabs_l(w);
	return 0;
}

static int cmd_topen(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str;
	gboolean bg = (strcmp(argv[0], "topenbg") == 0);
	if (argc <= 1) new_tab(w, NULL, global.homepage, bg);
	else {
		str = concat_args(argc, argv);
		new_tab(w, NULL, str->str, bg);
		g_string_free(str, TRUE);
	}
	return 0;
}

static int cmd_unalias(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	struct alias *a;
	if (argc != 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	a = get_alias(w, argv[1]);
	if (a != NULL) destroy_alias(w, a);
	return 0;
}

static int cmd_unbind(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	guint mode = 0, mod = 0;
	struct bind *b;

	if (argc != 4) {
		out(w, TRUE, c->usage);
		return 1;
	}
	parse_mode_mask(w, argv[1], &mode, argv[0]);
	parse_mod_mask(w, argv[2], &mod, argv[0]);
	b = get_bind(w, mode, mod, argv[3]);
	if (b == NULL) return 0;
	b->mode &= ~mode;
	if (b->mode == 0) destroy_bind(w, b);
	return 0;
}

static int cmd_unbind_all(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	while (global.binds.h)
		destroy_bind(w, (struct bind *) global.binds.h);
	return 0;
}

static int cmd_unset(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	struct var *v;
	if (argc != 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	v = get_var(w, argv[1]);
	if (v != NULL) {
		if (v->can_unset) destroy_var(w, v);
		else g_free(out(w, TRUE, g_strdup_printf("%s: error: variable \"%s\" cannot be unset\n", argv[0], argv[1])));
	}
	return 0;
}

static int cmd_wclose(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	gtk_widget_destroy(w->w);
	return WKB_CMD_RET_WINDOW_DESTROYED;
}

static int cmd_window(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str;
	struct window *wn;
	int id;
	if (argc < 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	if (strcmp(argv[1], "first") == 0) wn = (struct window *) global.windows.h;
	else if (strcmp(argv[1], "last") == 0) wn = (struct window *) global.windows.t;
	else {
		id = atoi(argv[1]);
		LIST_FOREACH(&global.windows, wn) if (wn->id == id) break;
		if (wn == NULL) {
			g_free(out(w, TRUE, g_strdup_printf("%s: error: window %s not found\n", argv[0], argv[1])));
			return 1;
		}
	}
	str = concat_args(argc - 1, &argv[1]);
	exec_line(wn, NULL, str->str);
	g_string_free(str, TRUE);
	return 0;
}

static int cmd_wopen(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	GString *str;
	if (argc <= 1) new_window(g_malloc0(sizeof(struct window)), TRUE, global.homepage);
	else {
		str = concat_args(argc, argv);
		new_window(g_malloc0(sizeof(struct window)), TRUE, str->str);
		g_string_free(str, TRUE);
	}
	return 0;
}

static int cmd_zoom(struct window *w, WebKitWebView *wv, struct command *c, int argc, gchar **argv)
{
	double level;
	if (argc != 2) {
		out(w, TRUE, c->usage);
		return 1;
	}
	level = webkit_web_view_get_zoom_level(wv);
	if (argv[1][0] == '+' || argv[1][0] == '-') level += atof(argv[1]);
	else level = atof(argv[1]);
	webkit_web_view_set_zoom_level(wv, (level > 0) ? level : 0);
	return 0;
}

/* Begin set/get/init handler functions */

static gchar * get_handler_gobject(struct window *w, WebKitWebView *wv, struct var *v, int context)
{
	gboolean bval;
	gint ival;
	gdouble dval;
	gfloat fval;
	gchar *sval, *name;
	GParamSpec *ps;
	GObject *obj;
	GType t;

	obj = get_var_gobject(w, wv, v, &name);
	if (obj == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("BUG: invalid object for setting: \"%s\"\n", v->name)));
		return NULL;
	}

	ps = g_object_class_find_property(G_OBJECT_GET_CLASS(obj), name);
	if (ps == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("BUG: no such property: \"%s\"\n", name)));
		return NULL;
	}

	t = G_PARAM_SPEC_VALUE_TYPE(ps);
	if (t == G_TYPE_BOOLEAN) {
		g_object_get(obj, name, &bval, NULL);
		if (bval == TRUE) return g_strdup("true");
		else return g_strdup("false");
	}
	else if (t == G_TYPE_INT || t == G_TYPE_UINT) {
		g_object_get(obj, name, &ival, NULL);
		return g_strdup_printf("%d", ival);
	}
	else if (t == G_TYPE_DOUBLE) {
	 	g_object_get(obj, name, &dval, NULL);
		return g_strdup_printf("%g", dval);
	}
	else if (t == G_TYPE_FLOAT) {
		g_object_get(obj, name, &fval, NULL);
		return g_strdup_printf("%g", fval);
	}
	else if (t == G_TYPE_STRING) {
		g_object_get(obj, name, &sval, NULL);
		return sval;
	}
#ifndef __HAVE_WEBKIT2__
	else if (t == SOUP_TYPE_URI) {
		SoupURI *tmp_uri;
		g_object_get(obj, name, &tmp_uri, NULL);
		if (tmp_uri != NULL) {
			gchar *tmp = soup_uri_to_string(tmp_uri, FALSE);
			soup_uri_free(tmp_uri);
			return tmp;
		}
		return NULL;
	}
#endif
	else
		g_free(out(w, TRUE, g_strdup_printf("BUG: unhandled type for property \"%s\"\n", name)));
	return NULL;
}

static gchar * get_handler_wkb(struct window *w, WebKitWebView *wv, struct var *v, int context)
{
	struct default_wkb_setting *s = get_wkb_setting(w, v->name);
	if (s == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("BUG: no such setting \"%s\"\n", v->name)));
		return NULL;
	}
	switch (s->type) {
		case WKB_SETTING_TYPE_STRING:
			return g_strdup(s->get(w, context).s);
		case WKB_SETTING_TYPE_BOOL:
			if (s->get(w, context).b == TRUE) return g_strdup("true");
			else return g_strdup("false");
		case WKB_SETTING_TYPE_INT:
			return g_strdup_printf("%d", s->get(w, context).i);
		case WKB_SETTING_TYPE_DOUBLE:
			return g_strdup_printf("%g", s->get(w, context).d);
		default:
			g_free(out(w, TRUE, g_strdup_printf("BUG: unhandled type for setting \"%s\"\n", v->name)));
	}
	return NULL;
}

static void init_handler_gobject(struct window *w, WebKitWebView *wv, struct var_handler *vh)
{
	add_gobject_properties(w, vh, G_OBJECT(wv), VAR_PREFIX_WEBKIT_WEB_VIEW"%s");
	add_gobject_properties(w, vh, G_OBJECT(webkit_web_view_get_settings(wv)), VAR_PREFIX_WEBKIT_SETTINGS"%s");
#ifndef __HAVE_WEBKIT2__
	add_gobject_properties(w, vh, G_OBJECT(webkit_get_default_session()), VAR_PREFIX_SOUP_SESSION"%s");
#endif
}

static void init_handler_wkb(struct window *w, WebKitWebView *wv, struct var_handler *vh)
{
	int i;
	for (i = 0; i < LENGTH(default_wkb_settings); ++i)
		new_var(w, default_wkb_settings[i].name, 0)->handler = vh;
}

static int set_handler_gobject(struct window *w, WebKitWebView *wv, struct var *v, const gchar *new_value)
{
	GParamSpec *ps;
	GObject *obj;
	gboolean bval;
	gchar *name;
	GType t;

	obj = get_var_gobject(w, wv, v, &name);
	if (obj == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("BUG: invalid object for setting: \"%s\"\n", v->name)));
		return 1;
	}

	ps = g_object_class_find_property(G_OBJECT_GET_CLASS(obj), name);
	if (ps == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("BUG: no such property: \"%s\"\n", name)));
		return 1;
	}

	if (!(ps->flags & G_PARAM_WRITABLE)) {
		g_free(out(w, TRUE, g_strdup_printf("error: setting \"%s\" is read-only\n", v->name)));
		return 1;
	}
	t = G_PARAM_SPEC_VALUE_TYPE(ps);
	if (t == G_TYPE_BOOLEAN) {
		if (new_value != NULL && new_value[0] == 't') g_object_set(obj, name, TRUE, NULL);
		else if (new_value != NULL && new_value[0] == 'f') g_object_set(obj, name, FALSE, NULL);
		else if (new_value != NULL && new_value[0] == '!') {
			g_object_get(obj, name, &bval, NULL);
			g_object_set(obj, name, (bval == TRUE) ? FALSE : TRUE, NULL);
		}
		else {
			g_free(out(w, TRUE, g_strdup_printf("error: illegal value: \"%s\"; legal values: t, f, ! (note: only the first character is interpreted)\n", (new_value == NULL) ? "" : new_value)));
			return 1;
		}
	}
	else if (t == G_TYPE_INT || t == G_TYPE_UINT) g_object_set(obj, name, (new_value == NULL) ? 0 : atoi(new_value), NULL);
	else if (t == G_TYPE_DOUBLE) g_object_set(obj, name, (new_value == NULL) ? 0.0 : atof(new_value), NULL);
	else if (t == G_TYPE_FLOAT) g_object_set(obj, name, (new_value == NULL) ? 0.0f : (gfloat) atof(new_value), NULL);
	else if (t == G_TYPE_STRING) g_object_set(obj, name, new_value, NULL);
#ifndef __HAVE_WEBKIT2__
	else if (t == SOUP_TYPE_URI) {
		gchar *tmp = construct_uri((new_value == NULL) ? "" : new_value);
		SoupURI *tmp_uri = soup_uri_new(tmp);
		g_free(tmp);
		g_object_set(obj, name, tmp_uri, NULL);
		if (tmp_uri != NULL) soup_uri_free(tmp_uri);
	}
#endif
	else g_free(out(w, TRUE, g_strdup_printf("BUG: unhandled type for property \"%s\"\n", name)));
	return 0;
}

static int set_handler_wkb(struct window *w, WebKitWebView *wv, struct var *v, const gchar *new_value)
{
	struct default_wkb_setting *s = get_wkb_setting(w, v->name);
	if (s == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("BUG: no such setting \"%s\"\n", v->name)));
		return 1;
	}
	if (s->set == NULL) {
		g_free(out(w, TRUE, g_strdup_printf("error: setting \"%s\" is read-only\n", v->name)));
		return 1;
	}
	switch (s->type) {
		case WKB_SETTING_TYPE_STRING:
			s->set(w, (union wkb_setting) { .s = g_strdup(new_value) });
			break;
		case WKB_SETTING_TYPE_BOOL:
			if (new_value != NULL && new_value[0] == 't') s->set(w, (union wkb_setting) { .b = TRUE });
			else if (new_value != NULL && new_value[0] == 'f') s->set(w, (union wkb_setting) { .b = FALSE });
			else if (new_value != NULL && new_value[0] == '!') s->set(w, (union wkb_setting) { .b = (s->get(w, WKB_VAR_CONTEXT_BOOL_TOGGLE).b == TRUE) ? FALSE : TRUE });
			else g_free(out(w, TRUE, g_strdup_printf("error: illegal value: \"%s\"; legal values: t, f, ! (note: only the first character is interpreted)\n", (new_value == NULL) ? "" : new_value)));
			break;
		case WKB_SETTING_TYPE_INT:
			if (new_value != NULL) s->set(w, (union wkb_setting) { .i = atoi(new_value) });
			break;
		case WKB_SETTING_TYPE_DOUBLE:
			if (new_value != NULL) s->set(w, (union wkb_setting) { .d = atof(new_value) });
			break;
		default:
			g_free(out(w, TRUE, g_strdup_printf("BUG: unhandled type for setting \"%s\"\n", v->name)));
	}
	return 0;
}

/* Begin wkb setting accessor/mutator functions */

static union wkb_setting get_webkit_api(struct window *w, int context)
{
	return (union wkb_setting) { .s = WKB_WEBKIT_API };
}

static union wkb_setting get_cmd_fifo(struct window *w, int context)
{
	return (union wkb_setting) { .s = w->cmd_fifo };
}

static union wkb_setting get_config_dir(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.config_dir };
}

static void set_config_dir(struct window *w, union wkb_setting v)
{
	g_free(global.config_dir);
	global.config_dir = g_strdup(v.s);
}

static union wkb_setting get_cookie_file(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.cookie_file };
}

static void set_cookie_file(struct window *w, union wkb_setting v)
{
	gchar *tmp = g_strdup(global.cookie_policy);
	g_free(global.cookie_file);
	global.cookie_file = g_strdup(v.s);
#ifdef __HAVE_WEBKIT2__
#ifdef __WITH_TEXT_COOKIES__
	webkit_cookie_manager_set_persistent_storage(webkit_web_context_get_cookie_manager(webkit_web_view_get_context(GET_CURRENT_VIEW(w))), (v.s == NULL) ? "" : v.s, WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);
#else
	webkit_cookie_manager_set_persistent_storage(webkit_web_context_get_cookie_manager(webkit_web_view_get_context(GET_CURRENT_VIEW(w))), (v.s == NULL) ? "" : v.s, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
#endif
#else
	soup_session_remove_feature_by_type(webkit_get_default_session(), soup_cookie_jar_get_type());
	if (v.s == NULL || v.s[0] == '\0') soup_session_add_feature(webkit_get_default_session(), SOUP_SESSION_FEATURE(soup_cookie_jar_new()));
	else soup_session_add_feature(webkit_get_default_session(), SOUP_SESSION_FEATURE(soup_cookie_jar_text_new(v.s, FALSE)));
#endif
	set_cookie_policy(w, (union wkb_setting) { .s = tmp });
	g_free(tmp);
}

static union wkb_setting get_cookie_policy(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.cookie_policy };
}

static void set_cookie_policy(struct window *w, union wkb_setting v)
{
#ifdef __HAVE_WEBKIT2__
	WebKitCookieManager *cm = webkit_web_context_get_cookie_manager(webkit_web_view_get_context(GET_CURRENT_VIEW(w)));
	if (v.s == NULL) return;
	else if (strcmp(v.s, "always") == 0)
		webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
	else if (strcmp(v.s, "never") == 0)
		webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NEVER);
	else if (strcmp(v.s, "no-third-party") == 0)
		webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
#else
	SoupCookieJar *j = SOUP_COOKIE_JAR(soup_session_get_feature(webkit_get_default_session(), soup_cookie_jar_get_type()));
	if (v.s == NULL) return;
	else if (strcmp(v.s, "always") == 0)
		soup_cookie_jar_set_accept_policy(j, SOUP_COOKIE_JAR_ACCEPT_ALWAYS);
	else if (strcmp(v.s, "never") == 0)
		soup_cookie_jar_set_accept_policy(j, SOUP_COOKIE_JAR_ACCEPT_NEVER);
	else if (strcmp(v.s, "no-third-party") == 0)
		soup_cookie_jar_set_accept_policy(j, SOUP_COOKIE_JAR_ACCEPT_NO_THIRD_PARTY);
#endif
	else {
		g_free(out(w, TRUE, g_strdup_printf("error: illegal value: \"%s\"; legal values: always, never, no-third-party\n", v.s)));
		return;
	}
	g_free(global.cookie_policy);
	global.cookie_policy = g_strdup(v.s);
}

static union wkb_setting get_auto_scroll(struct window *w, int context)
{
	return (union wkb_setting) { .b = w->auto_scroll };
}

static void set_auto_scroll(struct window *w, union wkb_setting v)
{
	w->auto_scroll = v.b;
}

static union wkb_setting get_show_console(struct window *w, int context)
{
	return (union wkb_setting) { .b = w->show_console };
}

static void set_show_console(struct window *w, union wkb_setting v)
{
	if (v.b) gtk_widget_show(w->consw);
	else gtk_widget_hide(w->consw);
	w->show_console = v.b;
}

static union wkb_setting get_print_keyval(struct window *w, int context)
{
	return (union wkb_setting) { .b = w->print_keyval };
}

static void set_print_keyval(struct window *w, union wkb_setting v)
{
	w->print_keyval = v.b;
}

static union wkb_setting get_mod_mask(struct window *w, int context)
{
	static gchar str_mod[8];
	get_mod_string(global.mod_mask, str_mod);
	return (union wkb_setting) { .s = str_mod };
}

static void set_mod_mask(struct window *w, union wkb_setting v)
{
	parse_mod_mask(w, v.s, &global.mod_mask, "set_mod_mask");
}

static union wkb_setting get_fullscreen(struct window *w, int context)
{
	return (union wkb_setting) { .b = w->fullscreen };
}

static void set_fullscreen(struct window *w, union wkb_setting v)
{
	/* apparently calling gtk_window_unfullscreen() when the window isn't fullscreened causes some problems... */
	if (v.b == w->fullscreen) return;
	fullscreen_mode(w, v.b);
	if (v.b) gtk_window_fullscreen(GTK_WINDOW(w->w));
	else gtk_window_unfullscreen(GTK_WINDOW(w->w));
}

static union wkb_setting get_allow_popups(struct window *w, int context)
{
	return (union wkb_setting) { .b = global.allow_popups };
}

static void set_allow_popups(struct window *w, union wkb_setting v)
{
	global.allow_popups = v.b;
}

static union wkb_setting get_download_dir(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.download_dir };
}

static void set_download_dir(struct window *w, union wkb_setting v)
{
	g_free(global.download_dir);
	global.download_dir = g_strdup(v.s);
}

static union wkb_setting get_dl_open_cmd(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.dl_open_cmd };
}

static void set_dl_open_cmd(struct window *w, union wkb_setting v)
{
	g_free(global.dl_open_cmd);
	global.dl_open_cmd = g_strdup(v.s);
}

static union wkb_setting get_dl_auto_open(struct window *w, int context)
{
	return (union wkb_setting) { .b = global.dl_auto_open };
}

static void set_dl_auto_open(struct window *w, union wkb_setting v)
{
	global.dl_auto_open = v.b;
}

static union wkb_setting get_default_width(struct window *w, int context)
{
	return (union wkb_setting) { .i = global.default_width };
}

static void set_default_width(struct window *w, union wkb_setting v)
{
	global.default_width = (v.i > 0) ? v.i : 1;
}

static union wkb_setting get_default_height(struct window *w, int context)
{
	return (union wkb_setting) { .i = global.default_height };
}

static void set_default_height(struct window *w, union wkb_setting v)
{
	global.default_height = (v.i > 0) ? v.i : 1;
}

static union wkb_setting get_msg_timeout(struct window *w, int context)
{
	return (union wkb_setting) { .i = global.msg_timeout };
}

static void set_msg_timeout(struct window *w, union wkb_setting v)
{
	global.msg_timeout = (v.i >= 0) ? v.i : 0;
}

#ifdef __HAVE_WEBKIT2__
static union wkb_setting get_spell_langs(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.spell_langs };
}

static void set_spell_langs(struct window *w, union wkb_setting v)
{
	g_free(global.spell_langs);
	global.spell_langs = g_strdup(v.s);
	gchar **langs = g_strsplit((v.s != NULL) ? v.s : "", ",", -1);
	webkit_web_context_set_spell_checking_languages(webkit_web_view_get_context(GET_CURRENT_VIEW(w)), (const gchar * const *) langs);
	g_strfreev(langs);
}

static union wkb_setting get_spell(struct window *w, int context)
{
	return (union wkb_setting) { .b = webkit_web_context_get_spell_checking_enabled(webkit_web_view_get_context(GET_CURRENT_VIEW(w))) };
}

static void set_spell(struct window *w, union wkb_setting v)
{
	webkit_web_context_set_spell_checking_enabled(webkit_web_view_get_context(GET_CURRENT_VIEW(w)), v.b);
}

static union wkb_setting get_tls_errors(struct window *w, int context)
{
	return (union wkb_setting) { .b = (webkit_web_context_get_tls_errors_policy(webkit_web_view_get_context(GET_CURRENT_VIEW(w))) == WEBKIT_TLS_ERRORS_POLICY_FAIL) };
}

static void set_tls_errors(struct window *w, union wkb_setting v)
{
	webkit_web_context_set_tls_errors_policy(webkit_web_view_get_context(GET_CURRENT_VIEW(w)), (v.b) ? WEBKIT_TLS_ERRORS_POLICY_FAIL : WEBKIT_TLS_ERRORS_POLICY_IGNORE);
}
#endif

static union wkb_setting get_find_string(struct window *w, int context)
{
	return (union wkb_setting) { .s = w->find_string };
}

static void set_find_string(struct window *w, union wkb_setting v)
{
	g_free(w->find_string);
	w->find_string = g_strdup(v.s);
}

static union wkb_setting get_homepage(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.homepage };
}

static void set_homepage(struct window *w, union wkb_setting v)
{
	g_free(global.homepage);
	global.homepage = g_strdup(v.s);
}

static union wkb_setting get_current_uri(struct window *w, int context)
{
	return (union wkb_setting) { .s = (GET_CURRENT_TAB(w)) ? GET_CURRENT_TAB(w)->uri : NULL };
}

static union wkb_setting get_fc_dir(struct window *w, int context)
{
	GtkWidget *dialog;
	if (context != WKB_VAR_CONTEXT_EXPAND) return (union wkb_setting) { .s = NULL };
	dialog = gtk_file_chooser_dialog_new("Choose Directory", NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		g_free(global.fc_tmp);
		global.fc_tmp = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		gtk_widget_destroy(dialog);
		return (union wkb_setting) { .s = global.fc_tmp };
	}
	gtk_widget_destroy(dialog);
	return (union wkb_setting) { .s = NULL };
}

static union wkb_setting get_fc_file(struct window *w, int context)
{
	GtkWidget *dialog;
	if (context != WKB_VAR_CONTEXT_EXPAND) return (union wkb_setting) { .s = NULL };
	dialog = gtk_file_chooser_dialog_new("Choose File", NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		g_free(global.fc_tmp);
		global.fc_tmp = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		gtk_widget_destroy(dialog);
		return (union wkb_setting) { .s = global.fc_tmp };
	}
	gtk_widget_destroy(dialog);
	return (union wkb_setting) { .s = NULL };
}

static union wkb_setting get_clipboard_text(struct window *w, int context)
{
	g_free(global.clipboard_tmp);
	global.clipboard_tmp = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
	return (union wkb_setting) { .s = global.clipboard_tmp };
}

static void set_clipboard_text(struct window *w, union wkb_setting v)
{
	if (v.s != NULL)
		gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), v.s, -1);
}

static union wkb_setting get_load_started(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.load_started };
}

static void set_load_started(struct window *w, union wkb_setting v)
{
	g_free(global.load_started);
	global.load_started = g_strdup(v.s);
}

static union wkb_setting get_dom_ready(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.dom_ready };
}

static void set_dom_ready(struct window *w, union wkb_setting v)
{
	g_free(global.dom_ready);
	global.dom_ready = g_strdup(v.s);
}

static union wkb_setting get_load_finished(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.load_finished };
}

static void set_load_finished(struct window *w, union wkb_setting v)
{
	g_free(global.load_finished);
	global.load_finished = g_strdup(v.s);
}

static union wkb_setting get_create(struct window *w, int context)
{
	return (union wkb_setting) { .s = global.create };
}

static void set_create(struct window *w, union wkb_setting v)
{
	g_free(global.create);
	global.create = g_strdup(v.s);
}

int main(int argc, char *argv[])
{
	int i;
	struct window *w;
	GString *str;
	memset(&global, 0, sizeof(global));
	gtk_init(NULL, NULL);
#ifdef __HAVE_WEBKIT2__
#ifdef __WITH_SHARED__
	webkit_web_context_set_process_model(webkit_web_context_get_default(), WEBKIT_PROCESS_MODEL_SHARED_SECONDARY_PROCESS);
#else
	webkit_web_context_set_process_model(webkit_web_context_get_default(), WEBKIT_PROCESS_MODEL_MULTIPLE_SECONDARY_PROCESSES);
#endif
	global.settings = webkit_settings_new();
#if WEBKIT_CHECK_VERSION(2, 5, 1)
	global.cm = webkit_user_content_manager_new();
#endif
	g_signal_connect(webkit_web_context_get_default(), "download-started", G_CALLBACK(cb_download), NULL);
#else
	global.settings = webkit_web_settings_new();
#endif
	global.default_width = DEFAULT_WIDTH;
	global.default_height = DEFAULT_HEIGHT;
	w = new_window(g_malloc0(sizeof(struct window)), TRUE, NULL);
	for (i = 0; i < LENGTH(default_wkb_settings); ++i)
		if (default_wkb_settings[i].scope == WKB_SETTING_SCOPE_GLOBAL && default_wkb_settings[i].set != NULL)
			default_wkb_settings[i].set(w, default_wkb_settings[i].default_value);
	for (i = 0; i < LENGTH(var_handlers); ++i) if (var_handlers[i].init != NULL) var_handlers[i].init(w, GET_CURRENT_VIEW(w), &var_handlers[i]);
	for (i = 0; i < LENGTH(builtin_config) && global.windows.h != NULL; ++i) exec_line((struct window *) global.windows.h , NULL, builtin_config[i]);
	if (global.windows.h != NULL) {
		str = concat_args(argc, argv);
		exec_line((struct window *) global.windows.h, NULL, str->str);
		g_string_free(str, TRUE);
	}
	if (global.default_width != DEFAULT_WIDTH || global.default_height != DEFAULT_HEIGHT)
		LIST_FOREACH(&global.windows, w)
			gtk_window_set_default_size(GTK_WINDOW(w->w), global.default_width, global.default_height);
	global.show_window = TRUE;
	LIST_FOREACH(&global.windows, w) gtk_widget_show(w->w);
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGQUIT, signal_handler);
	gtk_main();
	return 0;
}

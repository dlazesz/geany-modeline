// vim: expandtab:ts=8:encoding=UTF-8

#include "geanyplugin.h"

#define DEBUG_MODE 1

GeanyPlugin *geany_plugin;
GeanyData *geany_data;

static void scan_document(GeanyDocument *doc);
static void parse_options(GeanyDocument *doc, gchar *buf);
static void interpret_option(GeanyDocument *doc, gchar *opt);
static void on_document_open(GObject *obj, GeanyDocument *doc, gpointer user_data);
static void on_document_save(GObject *obj, GeanyDocument *doc, gpointer user_data);

static void opt_expand_tab(GeanyDocument *doc, void *arg);
static void opt_tab_stop(GeanyDocument *doc, void *arg);
static void opt_wrap(GeanyDocument *doc, void *arg);
static void opt_enc(GeanyDocument *doc, void *arg);

#define debugf(fmt, ...) \
        do { if (geany_data->app->debug_mode) printf(fmt, ## __VA_ARGS__); } while (0)

/**< Hook into geany */
PluginCallback plugin_callbacks[] = {
        { "document-open", (GCallback) &on_document_open, TRUE, NULL },
        { "document-save", (GCallback) &on_document_save, TRUE, NULL },
        { NULL, NULL, FALSE, NULL }
};

/**
 * @brief Mode option types
 */
enum mode_opt_arg {
        MODE_OPT_ARG_INT, /**< Argument is an integer */
        MODE_OPT_ARG_TRUE, /**< No argument, true */
        MODE_OPT_ARG_FALSE, /**< No argument, false */
        MODE_OPT_ARG_STR, /**< String argument */
};

/**
 * @brief Mode option structure
 */
struct mode_opt {
        const gchar *name; /**< Full name of option */
        const gchar *alias; /**< Short alias of option */
        enum mode_opt_arg arg_type; /**< Argument type for option */
        void (*cb)(GeanyDocument *, void *); /**< */
};

/**< Define mode options, what type of argument it takes, and the callback */
static struct mode_opt opts[] = {
        { "expandtab",   "et", MODE_OPT_ARG_TRUE,  &opt_expand_tab },
        { "noexpandtab", NULL, MODE_OPT_ARG_FALSE, &opt_expand_tab },
        { "tabstop",     "ts", MODE_OPT_ARG_INT,   &opt_tab_stop },
        { "softtabstop", "sts",MODE_OPT_ARG_INT,   &opt_tab_stop },
        { "shiftwidth",  "sw", MODE_OPT_ARG_INT,   &opt_tab_stop },
        { "wrap",        NULL, MODE_OPT_ARG_TRUE,  &opt_wrap },
        { "nowrap",      NULL, MODE_OPT_ARG_FALSE, &opt_wrap },
        { "fileencoding", "encoding", MODE_OPT_ARG_STR,   &opt_enc },
        { NULL,          NULL, -1,                 NULL }
};

/**< These are prefixes we search for to determine what is a modeline */
static const gchar *mode_pre[] = {
        " geany:",
        " vi:",
        " vim:",
        " ex:",
        NULL
};

/**
 * @brief Whether or not to expand tabs to spaces
 *
 * @param doc Document
 * @param arg 1/0 (gint)
 */
static void opt_expand_tab(GeanyDocument *doc, void *arg)
{
        gint *iarg;

        iarg = arg;

        debugf("opt_expand_tab: %d\n", *iarg);

        editor_set_indent_type(doc->editor, (*iarg) ?
                               GEANY_INDENT_TYPE_SPACES :
                               GEANY_INDENT_TYPE_TABS);
}

/**
 * @brief This sets the indent/tab width
 *
 * @param doc Document
 * @param arg Indent/tab width (gint)
 */
static void opt_tab_stop(GeanyDocument *doc, void *arg)
{
        const GeanyIndentPrefs *prefs;
        gint *iarg;

        iarg = arg;
        prefs = editor_get_indent_prefs(doc->editor);

        debugf("opt_tab_stop: %d\n", *iarg);

        editor_set_indent_width(doc->editor, *iarg);
        editor_set_indent_type(doc->editor, prefs->type);
}

/**
 * @brief Whether or not to wrap lines
 *
 * @param doc Document
 * @param arg 1/0 (gint)
 */
static void opt_wrap(GeanyDocument *doc, void *arg)
{
        gint *iarg;

        iarg = arg;

        debugf("opt_wrap: %d\n", *iarg);

        doc->editor->line_wrapping = *iarg;
        scintilla_send_message(doc->editor->sci, SCI_SETWRAPMODE,
                               (*iarg) ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
}

/**
 * @brief This sets specified file encoding
 *
 * @param doc Document
 * @param arg encoding (gchar *)
 */
static void opt_enc(GeanyDocument *doc, void *arg)
{
        const gchar *str = arg;

        debugf("opt_enc: \"%s\"\n", str);

        document_set_encoding(doc, str);

        debugf("Setting \"%s\"\n", doc->encoding);
}

/**
 * @brief Scan a document, line by line, looking for modelines.
 *
 * @param doc Document
 */
static void scan_document(GeanyDocument *doc)
{
        guint lines, line, i;
        gchar *buf, *ptr;

        if (!doc->is_valid)
                return;

        lines = sci_get_line_count(doc->editor->sci);
        for (line = 0; line < MIN(lines, 50); line++) {
                buf = g_strstrip(sci_get_line(doc->editor->sci, line));

                for (i = 0; mode_pre[i] != NULL; i++) {
                        if ((ptr = g_strstr_len(buf, -1, mode_pre[i]))) {
                                parse_options(doc, buf);
                                return;
                        }
                }
        }
}

/**
 * @brief Parse out each key/value pair from a modeline, then send the pair out
 *        to the option interpreter.
 *
 * @param doc Document
 * @param buf Modeline
 */
static void parse_options(GeanyDocument *doc, gchar *buf)
{
        gchar **tok;
        guint i;

        debugf("modeline [%s]\n", buf);

        // XXX Spaces not allowed around = character...
        // Can be separated by colon, space and comma
        tok = g_strsplit_set(buf, ": ,", 0);  // tok[0] is the "comment sign" therefore omited
        for (i = 1; tok[i]; i++) {
                if (tok[i])  // Skip empty parts
                        interpret_option(doc, tok[i]);
        }
        g_strfreev(tok);
}

/**
 * @brief Interpret an option and set it.
 *
 * @param doc Document
 * @param opt Key/value pair
 */
static void interpret_option(GeanyDocument *doc, gchar *opt)
{
        gchar **kv, *key, *val;
        guint i;
        gint iarg;

        debugf("interpret [%s]\n", opt);

        if ((kv = g_strsplit(opt, "=", 2))) {
                key = kv[0];
                val = kv[1];
                if (!key || !val) {
                        g_strfreev(kv);
                        return;  // Starts or ends with = character
                }
        } else {
                key = opt;
                val = NULL;
        }

        for (i = 0; opts[i].name; i++) {
                // name matched OR alias is not NULL AND mached else skip
                if (!g_ascii_strcasecmp(opts[i].name, key) ||
                        (opts[i].alias && !g_ascii_strcasecmp(opts[i].alias, key))) {

                        switch (opts[i].arg_type) {
                        case MODE_OPT_ARG_TRUE:
                                iarg = 1;
                                opts[i].cb(doc, &iarg);
                                break;
                        case MODE_OPT_ARG_FALSE:
                                iarg = 0;
                                opts[i].cb(doc, &iarg);
                                break;
                        case MODE_OPT_ARG_INT:
                                if (val) {
                                        iarg = g_ascii_strtoull(g_strstrip(val), NULL, 10);
                                        opts[i].cb(doc, &iarg);
                                }
                                break;
                        case MODE_OPT_ARG_STR:
                                if (val)
                                        opts[i].cb(doc, val);
                                break;
                        }

                        g_strfreev(kv);
                        return;
                }
        }
}

/**
 * @brief Document open hook
 *
 * @param obj
 * @param doc Document
 * @param user_data
 */
static void on_document_open(GObject *obj, GeanyDocument *doc, gpointer user_data)
{
        scan_document(doc);
        document_reload_force(doc, doc->encoding);  // We set this in scan_document function
}

/**
 * @brief Document save hook
 *
 * @param obj
 * @param doc Document
 * @param user_data
 */
static void on_document_save(GObject *obj, GeanyDocument *doc, gpointer user_data)
{
        scan_document(doc);
}

/**
 * @brief Plugin initialization
 *
 * @param plugin
 * @param data
 */
static gboolean MLplugin_init(GeanyPlugin *plugin, gpointer data)
{
	geany_plugin = plugin;
	geany_data = plugin->geany_data;
	return TRUE;
}

/**
 * @brief Plugin cleanup
 *
 * @param plugin
 * @param data
 */
void MLplugin_cleanup(GeanyPlugin *plugin, gpointer data)
{
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name = _("Modeline");
	plugin->info->description = _("Detect modelines for code formatting");
	plugin->info->version = "1.0";
	plugin->info->author = "Matt Hayes <nobomb@gmail.com>";

	plugin->funcs->init = MLplugin_init;
	plugin->funcs->cleanup = MLplugin_cleanup;
	plugin->funcs->callbacks = plugin_callbacks;

	GEANY_PLUGIN_REGISTER(plugin, 225);
}

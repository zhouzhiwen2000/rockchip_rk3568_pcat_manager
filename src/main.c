#include <unistd.h>
#include <glib.h>
#include "modem-manager.h"
#include "pmu-manager.h"

static gboolean g_pcat_cmd_daemonsize = FALSE;

static GMainLoop *g_pcat_main_loop = NULL;

static GOptionEntry g_pcat_cmd_entries[] =
{
    { "daemon", 'D', 0, G_OPTION_ARG_NONE, &g_pcat_cmd_daemonsize,
        "Run as a daemon", NULL },
    { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int main(int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;

    context = g_option_context_new("- PCat System Manager");
    g_option_context_set_ignore_unknown_options(context, TRUE);
    g_option_context_add_main_entries(context, g_pcat_cmd_entries, "PCATM");
    if(!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_warning("Option parsing failed: %s", error->message);
        g_clear_error(&error);
    }

    if(g_pcat_cmd_daemonsize)
    {
        daemon(0, 0);
    }

    g_pcat_main_loop = g_main_loop_new(NULL, FALSE);

    if(!pcat_pmu_manager_init())
    {
        g_warning("Failed to initialize PMU manager, "
            "power management may not work!");
    }

    if(!pcat_modem_manager_init())
    {
        g_warning("Failed to initialize modem manager, "
            "LTE/5G modem may not work!");
    }

    g_main_loop_run(g_pcat_main_loop);

    g_main_loop_unref(g_pcat_main_loop);
    g_pcat_main_loop = NULL;

    pcat_modem_manager_uninit();
    pcat_pmu_manager_uninit();

    g_option_context_free(context);

    return 0;
}

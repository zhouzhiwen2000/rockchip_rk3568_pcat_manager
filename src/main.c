#include <unistd.h>
#include <glib.h>
#include "common.h"
#include "modem-manager.h"
#include "pmu-manager.h"

#define PCAT_MANAGER_MAIN_CONFIG_FILE "/etc/pcat-manager.conf"

static gboolean g_pcat_cmd_daemonsize = FALSE;

static GMainLoop *g_pcat_main_loop = NULL;

static PCatManagerMainConfigData g_pcat_manager_main_config_data = {0};

static GOptionEntry g_pcat_cmd_entries[] =
{
    { "daemon", 'D', 0, G_OPTION_ARG_NONE, &g_pcat_cmd_daemonsize,
        "Run as a daemon", NULL },
    { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static void pcat_main_config_data_clear()
{
    g_free(g_pcat_manager_main_config_data.pm_serial_device);
    g_pcat_manager_main_config_data.pm_serial_device = NULL;

    g_pcat_manager_main_config_data.valid = FALSE;
}

static gboolean pcat_main_config_data_load()
{
    GKeyFile *keyfile;
    GError *error = NULL;
    gint ivalue;

    g_pcat_manager_main_config_data.valid = FALSE;

    keyfile = g_key_file_new();

    if(!g_key_file_load_from_file(keyfile, PCAT_MANAGER_MAIN_CONFIG_FILE,
        G_KEY_FILE_NONE, &error))
    {
        g_warning("Failed to load keyfile %s: %s!",
            PCAT_MANAGER_MAIN_CONFIG_FILE,
            error->message!=NULL ? error->message : "Unknown");

        g_clear_error(&error);

        return FALSE;
    }

    if(g_pcat_manager_main_config_data.hw_gpio_modem_power_chip!=NULL)
    {
        g_free(g_pcat_manager_main_config_data.hw_gpio_modem_power_chip);
    }
    g_pcat_manager_main_config_data.hw_gpio_modem_power_chip =
        g_key_file_get_string(keyfile, "Hardware", "GPIOModemPowerChip", NULL);

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemPowerLine", NULL);
    g_pcat_manager_main_config_data.hw_gpio_modem_power_line = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemPowerActiveLow", NULL);
    g_pcat_manager_main_config_data.hw_gpio_modem_power_active_low =
        (ivalue!=0);

    if(g_pcat_manager_main_config_data.hw_gpio_modem_rf_kill_chip!=NULL)
    {
        g_free(g_pcat_manager_main_config_data.hw_gpio_modem_rf_kill_chip);
    }
    g_pcat_manager_main_config_data.hw_gpio_modem_rf_kill_chip =
        g_key_file_get_string(keyfile, "Hardware", "GPIOModemRFKillChip", NULL);

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemRFKillLine", NULL);
    g_pcat_manager_main_config_data.hw_gpio_modem_rf_kill_line = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemRFKillActiveLow", NULL);
    g_pcat_manager_main_config_data.hw_gpio_modem_rf_kill_active_low =
        (ivalue!=0);

    if(g_pcat_manager_main_config_data.hw_gpio_modem_reset_chip!=NULL)
    {
        g_free(g_pcat_manager_main_config_data.hw_gpio_modem_reset_chip);
    }
    g_pcat_manager_main_config_data.hw_gpio_modem_reset_chip =
        g_key_file_get_string(keyfile, "Hardware", "GPIOModemResetChip", NULL);

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemResetLine", NULL);
    g_pcat_manager_main_config_data.hw_gpio_modem_reset_line = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemResetActiveLow", NULL);
    g_pcat_manager_main_config_data.hw_gpio_modem_reset_active_low =
        (ivalue!=0);

    if(g_pcat_manager_main_config_data.pm_serial_device!=NULL)
    {
        g_free(g_pcat_manager_main_config_data.pm_serial_device);
    }
    g_pcat_manager_main_config_data.pm_serial_device = g_key_file_get_string(
        keyfile, "PowerManager", "SerialDevice", NULL);

    ivalue = g_key_file_get_integer(keyfile, "PowerManager",
        "SerialBaud", NULL);
    g_pcat_manager_main_config_data.pm_serial_baud = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Debug",
        "ModemExternalExecStdoutLog", NULL);
    g_pcat_manager_main_config_data.debug_modem_external_exec_stdout_log =
        ivalue;

    g_key_file_unref(keyfile);

    g_pcat_manager_main_config_data.valid = TRUE;

    return TRUE;
}

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

    if(!pcat_main_config_data_load())
    {
        g_warning("Failed to load main config data!");

        return 1;
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
    pcat_main_config_data_clear();

    return 0;
}

PCatManagerMainConfigData *pcat_manager_main_config_data_get()
{
    return &g_pcat_manager_main_config_data;
}

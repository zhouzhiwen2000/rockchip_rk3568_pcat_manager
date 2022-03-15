#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <glib.h>
#include <glib-unix.h>
#include <pthread.h>
#include <errno.h>
#include <json.h>
#include "common.h"
#include "modem-manager.h"
#include "pmu-manager.h"
#include "controller.h"

#define PCAT_MAIN_MWAN_STATUS_CHECK_TIMEOUT 120

#define PCAT_MAIN_CONFIG_FILE "/etc/pcat-manager.conf"
#define PCAT_MAIN_USER_CONFIG_FILE "/etc/pcat-manager-userdata.conf"
#define PCAT_MAIN_SHUTDOWN_REQUEST_FILE "/tmp/pcat-shutdown.tmp"

#define PCAT_MAIN_LOG_FILE "/tmp/pcat-manager.log"

typedef enum
{
    PCAT_MAIN_IFACE_WIRED,
    PCAT_MAIN_IFACE_WIRED_V6,
    PCAT_MAIN_IFACE_MOBILE_5G,
    PCAT_MAIN_IFACE_MOBILE_5G_V6,
    PCAT_MAIN_IFACE_MOBILE_LTE,
    PCAT_MAIN_IFACE_MOBILE_LTE_V6,
    PCAT_MAIN_IFACE_LAST
}PCatMainIfaceType;

const gchar * const g_pcat_main_iface_names[
    PCAT_MAIN_IFACE_LAST] =
{
    "wan",
    "wan6",
    "wwan_5g",
    "wwan_5g_v6",
    "wwan_lte",
    "wwan_lte_v6"
};

const PCatManagerRouteMode g_pcat_main_iface_route_mode[
    PCAT_MAIN_IFACE_LAST] =
{
    PCAT_MANAGER_ROUTE_MODE_WIRED,
    PCAT_MANAGER_ROUTE_MODE_WIRED,
    PCAT_MANAGER_ROUTE_MODE_MOBILE,
    PCAT_MANAGER_ROUTE_MODE_MOBILE,
    PCAT_MANAGER_ROUTE_MODE_MOBILE,
    PCAT_MANAGER_ROUTE_MODE_MOBILE
};

static const guint g_pcat_main_shutdown_wait_max = 30;

static gboolean g_pcat_main_cmd_daemonsize = FALSE;

static GMainLoop *g_pcat_main_loop = NULL;
static gboolean g_pcat_main_shutdown = FALSE;
static gboolean g_pcat_main_reboot = FALSE;
static gboolean g_pcat_main_request_shutdown = FALSE;
static gboolean g_pcat_main_request_shutdown_send_pmu_request = TRUE;
static guint g_pcat_main_shutdown_wait_count = 0;
static gboolean g_pcat_main_watchdog_disabled = FALSE;

static gboolean g_pcat_main_net_status_led_work_mode = TRUE;
static guint g_pcat_main_status_check_timeout_id = 0;
static PCatManagerRouteMode g_pcat_main_net_status_led_applied_mode =
    PCAT_MANAGER_ROUTE_MODE_NONE;

static PCatManagerRouteMode g_pcat_main_network_route_mode =
    PCAT_MANAGER_ROUTE_MODE_NONE;
static gboolean g_pcat_main_mwan_route_check_flag = TRUE;

static PCatManagerMainConfigData g_pcat_main_config_data = {0};
static PCatManagerUserConfigData g_pcat_main_user_config_data =
    {0};

static FILE *g_pcat_main_debug_log_file_fp = NULL;

static GOptionEntry g_pcat_cmd_entries[] =
{
    { "daemon", 'D', 0, G_OPTION_ARG_NONE, &g_pcat_main_cmd_daemonsize,
        "Run as a daemon", NULL },
    { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

static void pcat_main_config_data_clear()
{
    g_free(g_pcat_main_config_data.pm_serial_device);
    g_pcat_main_config_data.pm_serial_device = NULL;

    g_pcat_main_config_data.valid = FALSE;
}

static gboolean pcat_main_config_data_load()
{
    GKeyFile *keyfile;
    GError *error = NULL;
    gint ivalue;

    g_pcat_main_config_data.valid = FALSE;

    keyfile = g_key_file_new();

    if(!g_key_file_load_from_file(keyfile, PCAT_MAIN_CONFIG_FILE,
        G_KEY_FILE_NONE, &error))
    {
        g_warning("Failed to load keyfile %s: %s!",
            PCAT_MAIN_CONFIG_FILE,
            error->message!=NULL ? error->message : "Unknown");

        g_clear_error(&error);

        return FALSE;
    }

    if(g_pcat_main_config_data.hw_gpio_modem_power_chip!=NULL)
    {
        g_free(g_pcat_main_config_data.hw_gpio_modem_power_chip);
    }
    g_pcat_main_config_data.hw_gpio_modem_power_chip =
        g_key_file_get_string(keyfile, "Hardware", "GPIOModemPowerChip", NULL);

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemPowerLine", NULL);
    g_pcat_main_config_data.hw_gpio_modem_power_line = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemPowerActiveLow", NULL);
    g_pcat_main_config_data.hw_gpio_modem_power_active_low =
        (ivalue!=0);

    if(g_pcat_main_config_data.hw_gpio_modem_rf_kill_chip!=NULL)
    {
        g_free(g_pcat_main_config_data.hw_gpio_modem_rf_kill_chip);
    }
    g_pcat_main_config_data.hw_gpio_modem_rf_kill_chip =
        g_key_file_get_string(keyfile, "Hardware", "GPIOModemRFKillChip", NULL);

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemRFKillLine", NULL);
    g_pcat_main_config_data.hw_gpio_modem_rf_kill_line = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemRFKillActiveLow", NULL);
    g_pcat_main_config_data.hw_gpio_modem_rf_kill_active_low =
        (ivalue!=0);

    if(g_pcat_main_config_data.hw_gpio_modem_reset_chip!=NULL)
    {
        g_free(g_pcat_main_config_data.hw_gpio_modem_reset_chip);
    }
    g_pcat_main_config_data.hw_gpio_modem_reset_chip =
        g_key_file_get_string(keyfile, "Hardware", "GPIOModemResetChip", NULL);

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemResetLine", NULL);
    g_pcat_main_config_data.hw_gpio_modem_reset_line = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Hardware",
        "GPIOModemResetActiveLow", NULL);
    g_pcat_main_config_data.hw_gpio_modem_reset_active_low =
        (ivalue!=0);

    if(g_pcat_main_config_data.pm_serial_device!=NULL)
    {
        g_free(g_pcat_main_config_data.pm_serial_device);
    }
    g_pcat_main_config_data.pm_serial_device = g_key_file_get_string(
        keyfile, "PowerManager", "SerialDevice", NULL);

    ivalue = g_key_file_get_integer(keyfile, "PowerManager",
        "SerialBaud", NULL);
    g_pcat_main_config_data.pm_serial_baud = ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Debug",
        "ModemExternalExecStdoutLog", NULL);
    g_pcat_main_config_data.debug_modem_external_exec_stdout_log =
        ivalue;

    ivalue = g_key_file_get_integer(keyfile, "Debug",
        "OutputLog", NULL);
    g_pcat_main_config_data.debug_output_log = ivalue;

    g_key_file_unref(keyfile);

    g_pcat_main_config_data.valid = TRUE;

    return TRUE;
}

static gboolean pcat_main_user_config_data_load()
{
    GKeyFile *keyfile;
    GError *error = NULL;
    gint iv;
    guint i;
    gchar item_name[32] = {0};
    PCatManagerUserConfigData *uconfig_data =
        &g_pcat_main_user_config_data;
    PCatManagerPowerScheduleData *sdata;

    uconfig_data->valid = FALSE;

    keyfile = g_key_file_new();

    if(!g_key_file_load_from_file(keyfile, PCAT_MAIN_USER_CONFIG_FILE,
        G_KEY_FILE_NONE, &error))
    {
        g_warning("Failed to load keyfile %s: %s!",
            PCAT_MAIN_USER_CONFIG_FILE,
            error->message!=NULL ? error->message : "Unknown");

        g_clear_error(&error);

        return FALSE;
    }

    if(uconfig_data->power_schedule_data!=NULL)
    {
        g_ptr_array_unref(uconfig_data->power_schedule_data);
    }
    uconfig_data->power_schedule_data = g_ptr_array_new_with_free_func(g_free);

    for(i=0;;i++)
    {
        g_snprintf(item_name, 31, "EnableBits%u", i);
        if(!g_key_file_has_key(keyfile, "Schedule", item_name, NULL))
        {
            break;
        }

        sdata = g_new0(PCatManagerPowerScheduleData, 1);

        iv = g_key_file_get_integer(keyfile, "Schedule", item_name, NULL);

        if(iv & PCAT_MANAGER_POWER_SCHEDULE_ENABLE_MINUTE)
        {
            sdata->enabled = TRUE;
        }
        else
        {
            sdata->enabled = FALSE;
        }

        sdata->enable_bits = iv & 0xFF;

        g_snprintf(item_name, 31, "Date%u", i);
        iv = g_key_file_get_integer(keyfile, "Schedule", item_name, NULL);
        sdata->year = (iv / 10000) % 10000;
        sdata->month = (iv / 100) % 100;
        sdata->day = iv % 100;

        g_snprintf(item_name, 31, "Time%u", i);
        iv = g_key_file_get_integer(keyfile, "Schedule", item_name, NULL);
        sdata->hour = (iv / 100) % 100;
        sdata->minute = iv % 100;

        g_snprintf(item_name, 31, "DOWBits%u", i);
        iv = g_key_file_get_integer(keyfile, "Schedule", item_name, NULL);
        sdata->dow_bits = iv & 0xFF;

        g_snprintf(item_name, 31, "Action%u", i);
        iv = g_key_file_get_integer(keyfile, "Schedule", item_name, NULL);
        sdata->action = (iv!=0);

        g_ptr_array_add(uconfig_data->power_schedule_data, sdata);
    }

    uconfig_data->charger_on_auto_start = (g_key_file_get_integer(keyfile,
        "General", "ChargerOnAutoStart", NULL)!=0);
    uconfig_data->charger_on_auto_start_timeout =
        g_key_file_get_integer(keyfile, "General",
        "ChargerOnAutoStartTimeout", NULL);

    g_key_file_unref(keyfile);

    uconfig_data->valid = TRUE;
    uconfig_data->dirty = FALSE;

    return TRUE;
}

static gboolean pcat_main_user_config_data_save()
{
    GKeyFile *keyfile;
    GError *error = NULL;
    guint i;
    gchar item_name[32] = {0};
    PCatManagerUserConfigData *uconfig_data =
        &g_pcat_main_user_config_data;
    PCatManagerPowerScheduleData *sdata;
    gboolean ret;

    if(!uconfig_data->dirty)
    {
        return TRUE;
    }

    keyfile = g_key_file_new();

    if(uconfig_data->power_schedule_data!=NULL)
    {
        for(i=0;i<uconfig_data->power_schedule_data->len;i++)
        {
            sdata = g_ptr_array_index(uconfig_data->power_schedule_data, i);

            g_snprintf(item_name, 31, "EnableBits%u", i);
            g_key_file_set_integer(keyfile, "Schedule", item_name,
                sdata->enable_bits);

            g_snprintf(item_name, 31, "Date%u", i);
            g_key_file_set_integer(keyfile, "Schedule", item_name,
                (gint)sdata->year * 10000 + (gint)sdata->month * 100 +
                (gint)sdata->day);

            g_snprintf(item_name, 31, "Time%u", i);
            g_key_file_set_integer(keyfile, "Schedule", item_name,
                (gint)sdata->hour * 100 + (gint)sdata->minute);

            g_snprintf(item_name, 31, "DOWBits%u", i);
            g_key_file_set_integer(keyfile, "Schedule", item_name,
                sdata->dow_bits);

            g_snprintf(item_name, 31, "Action%u", i);
            g_key_file_set_integer(keyfile, "Schedule", item_name,
                sdata->action ? 1 : 0);
        }
    }

    g_key_file_set_integer(keyfile, "General", "ChargerOnAutoStart",
        uconfig_data->charger_on_auto_start ? 1 : 0);
    g_key_file_set_integer(keyfile, "General", "ChargerOnAutoStartTimeout",
        uconfig_data->charger_on_auto_start_timeout);

    ret = g_key_file_save_to_file(keyfile, PCAT_MAIN_USER_CONFIG_FILE,
        &error);
    if(ret)
    {
        uconfig_data->dirty = FALSE;
    }
    else
    {
        g_warning("Failed to save user configuration data to file %s: %s",
            PCAT_MAIN_USER_CONFIG_FILE, error->message!=NULL ?
            error->message : "Unknown");
    }

    g_key_file_unref(keyfile);

    return TRUE;
}

static gboolean pcat_main_shutdown_check_timeout_func(
    gpointer user_data)
{
    if(pcat_pmu_manager_shutdown_completed())
    {
        if(g_pcat_main_loop!=NULL)
        {
            g_main_loop_quit(g_pcat_main_loop);

            return FALSE;
        }
    }
    else if(g_pcat_main_shutdown_wait_count > g_pcat_main_shutdown_wait_max)
    {
        g_warning("PMU shutdown request timeout!");

        if(g_pcat_main_loop!=NULL)
        {
            g_main_loop_quit(g_pcat_main_loop);

            return FALSE;
        }
    }

    g_pcat_main_shutdown_wait_count++;

    return TRUE;
}

static gboolean pcat_main_reboot_check_timeout_func(
    gpointer user_data)
{
    if(pcat_pmu_manager_reboot_completed())
    {
        if(g_pcat_main_loop!=NULL)
        {
            g_main_loop_quit(g_pcat_main_loop);

            return FALSE;
        }
    }
    else if(g_pcat_main_shutdown_wait_count > g_pcat_main_shutdown_wait_max)
    {
        g_warning("PMU reboot request timeout!");

        if(g_pcat_main_loop!=NULL)
        {
            g_main_loop_quit(g_pcat_main_loop);

            return FALSE;
        }
    }

    g_pcat_main_shutdown_wait_count++;

    return TRUE;
}

static void pcat_main_system_shutdown()
{
    if(g_pcat_main_shutdown)
    {
        return;
    }

    if(g_pcat_main_request_shutdown_send_pmu_request)
    {
        pcat_pmu_manager_shutdown_request();
        g_timeout_add_seconds(1,
            pcat_main_shutdown_check_timeout_func, NULL);
    }
    else
    {
        g_main_loop_quit(g_pcat_main_loop);
    }

    g_pcat_main_shutdown = TRUE;
}

static void pcat_main_system_reboot()
{
    if(g_pcat_main_reboot)
    {
        return;
    }

    pcat_pmu_manager_reboot_request();
    g_timeout_add_seconds(1,
        pcat_main_reboot_check_timeout_func, NULL);

    g_pcat_main_reboot = TRUE;
}

static gboolean pcat_main_sigterm_func(gpointer user_data)
{
    g_message("SIGTERM detected.");

    if(g_pcat_main_request_shutdown ||
        g_file_test(PCAT_MAIN_SHUTDOWN_REQUEST_FILE,
        G_FILE_TEST_IS_REGULAR | G_FILE_TEST_EXISTS))
    {
        pcat_main_system_shutdown();
    }
    else if(!g_pcat_main_watchdog_disabled)
    {
        pcat_main_system_reboot();
    }
    else
    {
        if(g_pcat_main_loop!=NULL)
        {
            g_main_loop_quit(g_pcat_main_loop);
        }
    }

    return TRUE;
}

static gboolean pcat_main_sigusr1_func(gpointer user_data)
{
    g_pcat_main_watchdog_disabled = TRUE;
    pcat_pmu_manager_watchdog_timeout_set(0);

    return TRUE;
}

static void *pcat_main_mwan_policy_check_thread_func(void *user_data)
{
    guint i, j;
    gchar *command;
    gchar *interface_status_stdout = NULL;
    gchar *mwan3_stdout = NULL;
    struct json_tokener *tokener;
    struct json_object *root, *child, *protocol, *policies, *rules, *rule;
    struct json_object *interfaces, *interface;
    guint rules_len;
    const gchar *iface, *upercent;
    guint percent;
    gboolean ret;
    PCatMainIfaceType route_iface;
    gboolean iface_status[PCAT_MAIN_IFACE_LAST];
    gboolean mwan3_interface_check_flag;
    gint64 mwan3_interface_check_timestamp;

    mwan3_interface_check_timestamp = g_get_monotonic_time();

    while(g_pcat_main_mwan_route_check_flag)
    {
        for(i=0;i<PCAT_MAIN_IFACE_LAST;i++)
        {
            iface_status[i] = FALSE;

            command = g_strdup_printf("ubus call network.interface.%s status",
                g_pcat_main_iface_names[i]);
            g_spawn_command_line_sync(command, &interface_status_stdout,
                NULL, NULL, NULL);
            g_free(command);

            G_STMT_START
            {
                if(interface_status_stdout==NULL)
                {
                    break;
                }

                tokener = json_tokener_new();
                root = json_tokener_parse_ex(tokener, interface_status_stdout,
                    strlen(interface_status_stdout));
                json_tokener_free(tokener);

                if(root==NULL)
                {
                    break;
                }

                if(json_object_object_get_ex(root, "up", &child))
                {
                    if(!json_object_get_boolean(child))
                    {
                        json_object_put(root);

                        break;
                    }
                }
                else
                {
                    json_object_put(root);

                    break;
                }

                ret = FALSE;
                if(json_object_object_get_ex(root, "ipv4-address", &child))
                {
                    if(json_object_get_type(child)==json_type_array &&
                       json_object_array_length(child) > 0)
                    {
                        ret = TRUE;
                    }
                }

                if(json_object_object_get_ex(root, "ipv6-address", &child))
                {
                    if(json_object_get_type(child)==json_type_array &&
                       json_object_array_length(child) > 0)
                    {
                        ret = TRUE;
                    }
                }
                if(ret)
                {
                    iface_status[i] = TRUE;
                }

                json_object_put(root);
            }
            G_STMT_END;

            g_free(interface_status_stdout);
        }

        g_spawn_command_line_sync("ubus call mwan3 status", &mwan3_stdout,
            NULL, NULL, NULL);

        ret = FALSE;

        G_STMT_START
        {
            if(mwan3_stdout==NULL)
            {
                break;
            }

            tokener = json_tokener_new();
            root = json_tokener_parse_ex(tokener, mwan3_stdout,
                strlen(mwan3_stdout));
            json_tokener_free(tokener);

            if(root==NULL)
            {
                break;
            }

            if(!json_object_object_get_ex(root, "interfaces", &interfaces))
            {
                json_object_put(root);

                break;
            }

            mwan3_interface_check_flag = TRUE;
            for(i=0;i<PCAT_MAIN_IFACE_LAST;i++)
            {
                if(!iface_status[i])
                {
                    continue;
                }

                if(!json_object_object_get_ex(interfaces,
                    g_pcat_main_iface_names[i], &interface))
                {
                    continue;
                }

                if(json_object_object_get_ex(interface, "status", &child))
                {
                    if(g_strcmp0(json_object_get_string(child), "online")!=0)
                    {
                        mwan3_interface_check_flag = FALSE;

                        break;
                    }
                }
                else
                {
                    mwan3_interface_check_flag = FALSE;

                    break;
                }
            }

            if(mwan3_interface_check_flag)
            {
                mwan3_interface_check_timestamp = g_get_monotonic_time();
            }

            if(!json_object_object_get_ex(root, "policies", &policies))
            {
                json_object_put(root);

                break;
            }

            if(json_object_object_get_ex(policies, "ipv4", &protocol))
            {
                if(json_object_object_get_ex(protocol, "balanced", &rules))
                {
                    rules_len = json_object_array_length(rules);

                    for(i=0;i<rules_len;i++)
                    {
                        rule = json_object_array_get_idx(rules, i);
                        iface = NULL;
                        percent = 0;

                        if(json_object_object_get_ex(rule, "interface",
                            &child))
                        {
                            iface = json_object_get_string(child);
                        }
                        if(json_object_object_get_ex(rule, "percent",
                            &child))
                        {
                            upercent = json_object_get_string(child);

                            if(upercent!=NULL)
                            {
                                sscanf(upercent, "%d", &percent);
                            }
                        }

                        route_iface = PCAT_MAIN_IFACE_LAST;

                        if(percent > 0)
                        {
                            for(j=0;j<PCAT_MAIN_IFACE_LAST;j++)
                            {
                                if(g_strcmp0(iface,
                                    g_pcat_main_iface_names[j])==0)
                                {
                                    route_iface = j;

                                    ret = TRUE;

                                    break;
                                }
                            }
                        }

                        if(ret)
                        {
                            if(route_iface!=PCAT_MAIN_IFACE_LAST)
                            {
                                g_pcat_main_network_route_mode =
                                    g_pcat_main_iface_route_mode[
                                    route_iface];
                            }

                            break;
                        }
                    }
                }
            }

            if(ret)
            {
                json_object_put(root);

                break;
            }

            if(json_object_object_get_ex(policies, "ipv6", &protocol))
            {
                if(json_object_object_get_ex(protocol, "balanced", &rules))
                {
                    rules_len = json_object_array_length(rules);

                    for(i=0;i<rules_len;i++)
                    {
                        rule = json_object_array_get_idx(rules, i);
                        iface = NULL;
                        percent = 0;

                        if(json_object_object_get_ex(rule, "interface",
                            &child))
                        {
                            iface = json_object_get_string(child);
                        }
                        if(json_object_object_get_ex(rule, "percent",
                            &child))
                        {
                            upercent = json_object_get_string(child);

                            if(upercent!=NULL)
                            {
                                sscanf(upercent, "%d", &percent);
                            }
                        }

                        route_iface = PCAT_MAIN_IFACE_LAST;

                        if(percent > 0)
                        {
                            for(j=0;j<PCAT_MAIN_IFACE_LAST;j++)
                            {
                                if(g_strcmp0(iface,
                                    g_pcat_main_iface_names[j])==0)
                                {
                                    route_iface = j;

                                    ret = TRUE;

                                    break;
                                }
                            }
                        }

                        if(ret)
                        {
                            if(route_iface!=PCAT_MAIN_IFACE_LAST)
                            {
                                g_pcat_main_network_route_mode =
                                    g_pcat_main_iface_route_mode[
                                    route_iface];
                            }

                            break;
                        }
                    }
                }
            }

            json_object_put(root);
        }
        G_STMT_END;

        g_free(mwan3_stdout);

        if(!ret)
        {
            g_pcat_main_network_route_mode =
                PCAT_MANAGER_ROUTE_MODE_NONE;
        }

        if(TRUE)
        {
            if(g_get_monotonic_time() >
                mwan3_interface_check_timestamp +
                PCAT_MAIN_MWAN_STATUS_CHECK_TIMEOUT * 1000000L)
            {
                g_spawn_command_line_sync("mwan3 restart", NULL,
                    NULL, NULL, NULL);

                mwan3_interface_check_timestamp = g_get_monotonic_time();

                g_warning("MWAN3 status is not correct, try to restart!");
            }
            else
            {
                g_debug("MWAN3 status check OK!");
            }
        }

        for(i=0;i<50 && g_pcat_main_mwan_route_check_flag;i++)
        {
            g_usleep(100000);
        }
    }

    return NULL;
}

static gboolean pcat_main_status_check_timeout_func(gpointer user_data)
{
    if(g_pcat_main_net_status_led_applied_mode!=
           g_pcat_main_network_route_mode)
    {
        switch(g_pcat_main_network_route_mode)
        {
            case PCAT_MANAGER_ROUTE_MODE_WIRED:
            {
                if(g_pcat_main_net_status_led_work_mode)
                {
                    pcat_pmu_manager_net_status_led_setup(
                        50, 50, 0);
                }

                break;
            }
            case PCAT_MANAGER_ROUTE_MODE_MOBILE:
            {
                if(g_pcat_main_net_status_led_work_mode)
                {
                    pcat_pmu_manager_net_status_led_setup(
                        20, 380, 0);
                }

                break;
            }
            default:
            {
                if(g_pcat_main_net_status_led_work_mode)
                {
                    pcat_pmu_manager_net_status_led_setup(
                        0, 100, 0);
                }

                break;
            }
        }

        g_pcat_main_net_status_led_applied_mode =
            g_pcat_main_network_route_mode;
    }

    return TRUE;
}

static void pcat_main_log_handle_func(const gchar *log_domain,
    GLogLevelFlags log_level, const gchar *message, gpointer user_data)
{
    const char *level = "";
	GLogLevelFlags minlevel = G_LOG_LEVEL_INFO;
    gchar buffer[16384] = {0};
    gint outsize;
    GDateTime *dt;
    gchar *dtstr;

	switch(log_level)
    {
		case G_LOG_LEVEL_DEBUG:
        {
            level = "DEBUG";
            break;
        }
		case G_LOG_LEVEL_INFO:
        {
            level = "INFO";
            break;
        }
		case G_LOG_LEVEL_MESSAGE:
        {
            level = "MESSAGE";
            break;
        }
		case G_LOG_LEVEL_WARNING:
        {
            level = "WARNING";
            break;
        }
		case G_LOG_LEVEL_CRITICAL:
        {
            level = "CRITICAL";
            break;
        }
		case G_LOG_LEVEL_ERROR:
        {
            level = "ERROR";
            break;
        }
		default:
        {
            level = "UNKNOWN";
            break;
        }
	}

    if(log_domain==NULL)
    {
        log_domain = "";
    }

    dt = g_date_time_new_now_local();
    dtstr = g_date_time_format(dt, "%Y%m%d %H%M%S");
    g_date_time_unref(dt);

    outsize = g_snprintf(buffer, 16383, "[%s] %s-%s: %s\n", dtstr,
        log_domain, level, message);
    g_free(dtstr);

    if(g_pcat_main_debug_log_file_fp !=NULL && outsize > 0)
    {
        fwrite(buffer, 1, outsize, g_pcat_main_debug_log_file_fp);
        fflush(g_pcat_main_debug_log_file_fp);
    }

	if(log_level <= minlevel && outsize > 0)
    {
		fprintf(stderr, "%s", buffer);
	}
	if(log_level <= G_LOG_LEVEL_ERROR)
    {
        fprintf(stderr, "%s", buffer);
		abort();
    }
}


int main(int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;
    pthread_t mwan_policy_check_thread;

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

    if(g_pcat_main_config_data.debug_output_log)
    {
        g_pcat_main_debug_log_file_fp = fopen(PCAT_MAIN_LOG_FILE, "w+");

        g_log_set_handler(NULL, G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL |
            G_LOG_FLAG_RECURSION, pcat_main_log_handle_func, NULL);

    }

    if(!pcat_main_user_config_data_load())
    {
        g_warning("Failed to load user config data, use default one!");
    }

    if(g_pcat_main_cmd_daemonsize)
    {
        daemon(0, 0);
    }

    signal(SIGPIPE, SIG_IGN);
    g_unix_signal_add(SIGTERM, pcat_main_sigterm_func, NULL);
    g_unix_signal_add(SIGUSR1, pcat_main_sigusr1_func, NULL);

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
    if(!pcat_controller_init())
    {
        g_warning("Failed to initialize controller, may not be able to "
            "communicate with other processes.");
    }

    if(pthread_create(&mwan_policy_check_thread, NULL,
        pcat_main_mwan_policy_check_thread_func, NULL)!=0)
    {
        g_warning("Failed to create MWAN policy check thread, routing "
            "check will not work!");
    }
    else
    {
        pthread_detach(mwan_policy_check_thread);
    }

    g_pcat_main_status_check_timeout_id =
        g_timeout_add_seconds(2, pcat_main_status_check_timeout_func, NULL);

    g_main_loop_run(g_pcat_main_loop);

    if(g_pcat_main_status_check_timeout_id > 0)
    {
        g_source_remove(g_pcat_main_status_check_timeout_id);
        g_pcat_main_status_check_timeout_id = 0;
    }

    g_pcat_main_mwan_route_check_flag = FALSE;

    g_main_loop_unref(g_pcat_main_loop);
    g_pcat_main_loop = NULL;

    pcat_controller_uninit();
    pcat_modem_manager_uninit();
    pcat_pmu_manager_uninit();
    g_option_context_free(context);
    pcat_main_config_data_clear();

    if(g_pcat_main_debug_log_file_fp!=NULL)
    {
        fclose(g_pcat_main_debug_log_file_fp);
        g_pcat_main_debug_log_file_fp = NULL;
    }

    return 0;
}

PCatManagerMainConfigData *pcat_main_config_data_get()
{
    return &g_pcat_main_config_data;
}

PCatManagerUserConfigData *pcat_main_user_config_data_get()
{
    return &g_pcat_main_user_config_data;
}

void pcat_main_request_shutdown(gboolean send_pmu_request)
{
    g_pcat_main_request_shutdown = TRUE;
    g_pcat_main_request_shutdown_send_pmu_request = send_pmu_request;
    g_spawn_command_line_async("poweroff", NULL);
}

void pcat_main_user_config_data_sync()
{
    pcat_main_user_config_data_save();
}

PCatManagerRouteMode pcat_main_network_route_mode_get()
{
    return g_pcat_main_network_route_mode;
}

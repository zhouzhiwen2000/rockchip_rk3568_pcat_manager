#include <stdio.h>
#include <errno.h>
#include <termios.h>
#include <gpiod.h>
#include <libusb.h>
#include <gio/gio.h>
#include "modem-manager.h"
#include "common.h"

#define PCAT_MODEM_MANAGER_POWER_WAIT_TIME 10
#define PCAT_MODEM_MANAGER_POWER_READY_TIME 30
#define PCAT_MODEM_MANAGER_RESET_ON_TIME 10
#define PCAT_MODEM_MANAGER_RESET_WAIT_TIME 30

typedef enum
{
    PCAT_MODEM_MANAGER_STATE_NONE,
    PCAT_MODEM_MANAGER_STATE_READY
}PCatModemManagerState;

typedef enum
{
    PCAT_MODEM_MANAGER_DEVICE_ALL,
    PCAT_MODEM_MANAGER_DEVICE_LTE,
    PCAT_MODEM_MANAGER_DEVICE_5G
}PCatModemManagerDeviceType;

typedef enum
{
    PCAT_MODEM_MANAGER_MODE_NONE,
    PCAT_MODEM_MANAGER_MODE_2G,
    PCAT_MODEM_MANAGER_MODE_3G,
    PCAT_MODEM_MANAGER_MODE_LTE,
    PCAT_MODEM_MANAGER_MODE_5G
}PCatModemManagerMode;

typedef struct _PCatModemManagerUSBData
{
    PCatModemManagerDeviceType device_type;
    guint16 id_vendor;
    guint16 id_product;
    const gchar *external_control_exec;
    gboolean external_control_exec_is_daemon;
}PCatModemManagerUSBData;

typedef struct _PCatModemManagerData
{
    gboolean initialized;
    gboolean work_flag;
    GMutex mutex;
    PCatModemManagerState state;
    GThread *modem_work_thread;
    GHashTable *modem_mode_table;
    PCatModemManagerMode modem_mode;
    gint modem_signal_strength;

    libusb_context *usb_ctx;

    struct gpiod_chip *gpio_modem_power_chip;
    struct gpiod_chip *gpio_modem_rf_kill_chip;
    struct gpiod_chip *gpio_modem_reset_chip;
    struct gpiod_line *gpio_modem_power_line;
    struct gpiod_line *gpio_modem_rf_kill_line;
    struct gpiod_line *gpio_modem_reset_line;

    GSubprocess *external_control_exec_process;
    GInputStream *external_control_exec_stdout_stream;
    GSource *external_control_exec_stdout_read_source;
    GString *external_control_exec_stdout_buffer;
}PCatModemManagerData;

static PCatModemManagerUSBData g_pcat_modem_manager_supported_5g_list[] =
{
    {
        .device_type = PCAT_MODEM_MANAGER_DEVICE_ALL,
        .id_vendor = 0x2C7C,
        .id_product = 0,
        .external_control_exec = "quectel-cm",
        .external_control_exec_is_daemon = FALSE
    }
};

static PCatModemManagerData g_pcat_modem_manager_data = {0};

static inline gboolean pcat_modem_manager_modem_power_init(
    PCatModemManagerData *mm_data, PCatManagerMainConfigData *main_config_data)
{
    guint i;
    gint ret;

    g_message("Start Modem power initialization.");

    if(main_config_data->hw_gpio_modem_power_chip==NULL)
    {
        g_warning("Modem power GPIO chip not configured!");

        return FALSE;
    }

    if(main_config_data->hw_gpio_modem_rf_kill_chip==NULL)
    {
        g_warning("Modem RF kill GPIO chip not configured!");

        return FALSE;
    }

    if(main_config_data->hw_gpio_modem_reset_chip==NULL)
    {
        g_warning("Modem reset GPIO chip not configured!");

        return FALSE;
    }

    if(mm_data->gpio_modem_power_chip==NULL)
    {
        mm_data->gpio_modem_power_chip = gpiod_chip_open_by_name(
            main_config_data->hw_gpio_modem_power_chip);
        if(mm_data->gpio_modem_power_chip==NULL)
        {
            g_warning("Failed to open Modem power GPIO chip!");

            return FALSE;
        }
    }

    if(mm_data->gpio_modem_rf_kill_chip==NULL)
    {
        mm_data->gpio_modem_rf_kill_chip = gpiod_chip_open_by_name(
            main_config_data->hw_gpio_modem_rf_kill_chip);
        if(mm_data->gpio_modem_rf_kill_chip==NULL)
        {
            g_warning("Failed to open Modem power GPIO chip!");

            return FALSE;
        }
    }

    if(mm_data->gpio_modem_reset_chip==NULL)
    {
        mm_data->gpio_modem_reset_chip = gpiod_chip_open_by_name(
            main_config_data->hw_gpio_modem_reset_chip);
        if(mm_data->gpio_modem_reset_chip==NULL)
        {
            g_warning("Failed to open Modem reset GPIO chip!");

            return FALSE;
        }
    }

    if(mm_data->gpio_modem_power_line==NULL)
    {
        mm_data->gpio_modem_power_line = gpiod_chip_get_line(
            mm_data->gpio_modem_power_chip,
            main_config_data->hw_gpio_modem_power_line);
        if(mm_data->gpio_modem_power_line==NULL)
        {
            g_warning("Failed to open Modem power GPIO line!");

            return FALSE;
        }
    }
    if(!gpiod_line_is_requested(mm_data->gpio_modem_power_line))
    {
        ret = gpiod_line_request_output(mm_data->gpio_modem_power_line,
            "gpio-modem-power",
            main_config_data->hw_gpio_modem_power_active_low ? 1 : 0);
        if(ret!=0)
        {
            g_warning("Failed to request output on Modem power GPIO!");
        }
    }
    else
    {
        gpiod_line_set_value(mm_data->gpio_modem_power_line,
            main_config_data->hw_gpio_modem_power_active_low ? 1 : 0);
    }

    if(mm_data->gpio_modem_rf_kill_line==NULL)
    {
        mm_data->gpio_modem_rf_kill_line = gpiod_chip_get_line(
            mm_data->gpio_modem_rf_kill_chip,
            main_config_data->hw_gpio_modem_rf_kill_line);
        if(mm_data->gpio_modem_rf_kill_line==NULL)
        {
            g_warning("Failed to open Modem RF kill GPIO line!");

            return FALSE;
        }
    }
    if(!gpiod_line_is_requested(mm_data->gpio_modem_rf_kill_line))
    {
        ret = gpiod_line_request_output(mm_data->gpio_modem_rf_kill_line,
            "gpio-modem-rf-kill",
            main_config_data->hw_gpio_modem_rf_kill_active_low ? 0 : 1);
        if(ret!=0)
        {
            g_warning("Failed to request output on Modem RF kill GPIO!");
        }
    }
    else
    {
        gpiod_line_set_value(mm_data->gpio_modem_rf_kill_line,
            main_config_data->hw_gpio_modem_rf_kill_active_low ? 0 : 1);
    }

    if(mm_data->gpio_modem_reset_line==NULL)
    {
        mm_data->gpio_modem_reset_line = gpiod_chip_get_line(
            mm_data->gpio_modem_reset_chip,
            main_config_data->hw_gpio_modem_reset_line);
        if(mm_data->gpio_modem_reset_line==NULL)
        {
            g_warning("Failed to open Modem reset GPIO line!");

            return FALSE;
        }
    }
    if(!gpiod_line_is_requested(mm_data->gpio_modem_reset_line))
    {
        ret = gpiod_line_request_output(mm_data->gpio_modem_reset_line,
            "gpio-modem-reset",
            main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);
        if(ret!=0)
        {
            g_warning("Failed to request output on Modem reset GPIO!");
        }
    }
    else
    {
        gpiod_line_set_value(mm_data->gpio_modem_reset_line,
            main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);
    }

    for(i=0;i<PCAT_MODEM_MANAGER_POWER_WAIT_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    gpiod_line_set_value(mm_data->gpio_modem_power_line,
        main_config_data->hw_gpio_modem_power_active_low ? 0 : 1);
    gpiod_line_set_value(mm_data->gpio_modem_rf_kill_line,
        main_config_data->hw_gpio_modem_rf_kill_active_low ? 1 : 0);
    gpiod_line_set_value(mm_data->gpio_modem_reset_line,
        main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);

    for(i=0;i<PCAT_MODEM_MANAGER_POWER_READY_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    g_message("Modem power on successfully.");

    gpiod_line_set_value(mm_data->gpio_modem_reset_line,
        main_config_data->hw_gpio_modem_reset_active_low ? 0 : 1);

    for(i=0;i<PCAT_MODEM_MANAGER_RESET_ON_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    gpiod_line_set_value(mm_data->gpio_modem_reset_line,
        main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);

    for(i=0;i<PCAT_MODEM_MANAGER_RESET_WAIT_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    g_message("Modem power initialization completed.");

    return TRUE;
}

static inline void pcat_modem_manager_external_control_exec_line_parser(
    PCatModemManagerData *mm_data, const guint8 *buffer, gssize size)
{
    gsize i, j;
    GString *str = mm_data->external_control_exec_stdout_buffer;
    gsize used_size = 0;
    const gchar *start = str->str;
    gchar **fields, **values;
    GHashTable *table;
    const gchar *cmd, *smode, *signal_raw_str;
    gint signal_raw;
    gint signal_value;

    g_string_append_len(str, (const gchar *)buffer, size);

    if(str->len > 1048576)
    {
        str->len = 0;
    }

    g_message("STDOUT DATA: %s", str->str);

    for(i=0;i<str->len;i++)
    {
        if(str->str[i]=='\n')
        {
            str->str[i] = '\0';

            fields = g_strsplit(start, ",", -1);

            if(fields!=NULL)
            {
                table = g_hash_table_new_full(g_str_hash, g_str_equal,
                    g_free, g_free);

                for(j=0;fields[j]!=NULL;j++)
                {
                    values = g_strsplit(fields[j], "=", 2);
                    if(values!=NULL)
                    {
                        if(values[0]!=NULL && values[1]!=NULL)
                        {
                            g_hash_table_replace(table, g_strdup(values[0]),
                                g_strdup(values[1]));
                        }

                        g_strfreev(values);
                    }
                }
                g_strfreev(fields);

                cmd = g_hash_table_lookup(table, "CMD");

                if(g_strcmp0(cmd, "SIGNALINFO")==0)
                {
                    signal_value = 0;

                    smode = g_hash_table_lookup(table, "MODE");
                    mm_data->modem_mode = GPOINTER_TO_UINT(
                        g_hash_table_lookup(mm_data->modem_mode_table, smode));

                    G_STMT_START
                    {
                        signal_raw_str = g_hash_table_lookup(table, "RSSI");
                        if(signal_raw_str!=NULL)
                        {
                            if(sscanf(signal_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= 0)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -100)
                                {
                                    signal_value = signal_raw + 100;
                                }

                                break;
                            }
                        }

                        signal_raw_str = g_hash_table_lookup(table, "RSRQ");
                        if(signal_raw_str!=NULL)
                        {
                            if(sscanf(signal_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= -10)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -20)
                                {
                                    signal_value = (signal_raw + 20) * 10;
                                }

                                break;
                            }
                        }

                        signal_raw_str = g_hash_table_lookup(table, "RSRP");
                        if(signal_raw_str!=NULL)
                        {
                            if(sscanf(signal_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= -80)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -100)
                                {
                                    signal_value = (signal_raw + 100) * 5;
                                }

                                break;
                            }
                        }

                        signal_raw_str = g_hash_table_lookup(table, "RSCP");
                        if(signal_raw_str!=NULL)
                        {
                            if(sscanf(signal_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= -60)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -100)
                                {
                                    signal_value = (signal_raw + 100) * 5 / 2;
                                }

                                break;
                            }
                        }
                    }
                    G_STMT_END;

                    mm_data->modem_signal_strength = signal_value;
                    g_message("Modem signal strength: %d", signal_value);
                }

                g_hash_table_unref(table);
            }
            start = str->str + i + 1;
            used_size = i + 1;
        }
    }

    if(used_size > 0)
    {
        g_string_erase(str, 0, used_size);
    }
}

static gboolean pcat_modem_manager_external_control_exec_stdout_watch_func(
    GObject *object, gpointer user_data)
{
    PCatModemManagerData *mm_data = (PCatModemManagerData *)user_data;
    gssize rsize;
    guint8 buffer[4096];
    GError *error = NULL;

    while((rsize=g_pollable_input_stream_read_nonblocking(
        G_POLLABLE_INPUT_STREAM(object), buffer, 4096, NULL, &error))>0)
    {
        pcat_modem_manager_external_control_exec_line_parser(mm_data,
            buffer, rsize);
    }

    if(error!=NULL)
    {
        g_clear_error(&error);
    }

    return TRUE;
}

static void pcat_modem_manager_external_control_exec_wait_func(
    GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    PCatModemManagerData *mm_data = (PCatModemManagerData *)user_data;

    if(g_subprocess_wait_check_finish(G_SUBPROCESS(source_object), res,
        &error))
    {
        g_message("External control process exits normally.");
    }
    else
    {
        g_warning("External control process exits with error: %s",
            error->message!=NULL ? error->message : "Unknown");
        g_clear_error(&error);
    }

    if(mm_data->external_control_exec_stdout_read_source!=NULL)
    {
        g_source_destroy(mm_data->external_control_exec_stdout_read_source);
        mm_data->external_control_exec_stdout_read_source = NULL;
    }

    mm_data->external_control_exec_stdout_stream = NULL;

    g_object_unref(mm_data->external_control_exec_process);
    mm_data->external_control_exec_process = NULL;
}

static inline gboolean pcat_modem_manager_run_external_exec(
    PCatModemManagerData *mm_data, const PCatModemManagerUSBData *usb_data)
{
    GError *error = NULL;
    gboolean ret = TRUE;

    if(mm_data==NULL || usb_data==NULL ||
        usb_data->external_control_exec==NULL)
    {
        return FALSE;
    }

    if(!usb_data->external_control_exec_is_daemon)
    {
        G_STMT_START
        {
            if(mm_data->external_control_exec_process!=NULL)
            {
                break;
            }

            mm_data->external_control_exec_process = g_subprocess_new(
                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
                usb_data->external_control_exec, NULL);
            if(mm_data->external_control_exec_process==NULL)
            {
                g_warning("Failed to run external modem control "
                    "executable file %s: %s",
                    usb_data->external_control_exec,
                    error!=NULL ? error->message: "Unknown");
                g_clear_error(&error);
                ret = FALSE;

                break;
            }

            mm_data->external_control_exec_stdout_stream =
                g_subprocess_get_stdout_pipe(
                mm_data->external_control_exec_process);
            if(mm_data->external_control_exec_stdout_stream!=NULL)
            {
                mm_data->external_control_exec_stdout_read_source =
                    g_pollable_input_stream_create_source(
                    G_POLLABLE_INPUT_STREAM(
                    mm_data->external_control_exec_stdout_stream),
                    NULL);

                g_source_set_callback(
                    mm_data->external_control_exec_stdout_read_source,
                    G_SOURCE_FUNC(
                    pcat_modem_manager_external_control_exec_stdout_watch_func),
                    mm_data, NULL);
                g_source_attach(
                    mm_data->external_control_exec_stdout_read_source, NULL);
            }

            g_subprocess_wait_async(
                mm_data->external_control_exec_process, NULL,
                pcat_modem_manager_external_control_exec_wait_func,
                mm_data);
        }
        G_STMT_END;

    }
    else
    {
        /* TODO: Run external control exec as daemon. */
    }

    return ret;
}

static void pcat_modem_manager_scan_usb_devs(PCatModemManagerData *mm_data)
{
    libusb_device *dev;
    guint i;
    ssize_t cnt;
    struct libusb_device_descriptor desc;
    int r;
    libusb_device **devs = NULL;
    const PCatModemManagerUSBData *usb_data;
    guint uc;
    gboolean detected;

    cnt = libusb_get_device_list(NULL, &devs);
    if(cnt < 0)
    {
        return;
    }

    for(i=0;devs[i]!=NULL;i++)
    {
        detected = FALSE;
        dev = devs[i];

        r = libusb_get_device_descriptor(dev, &desc);
        if(r < 0)
        {
            g_warning("Failed to get USB device descriptor!");

            continue;
        }

        for(uc=0;uc < sizeof(g_pcat_modem_manager_supported_5g_list) /
            sizeof(PCatModemManagerUSBData);uc++)
        {
            usb_data = &(g_pcat_modem_manager_supported_5g_list[uc]);

            if(usb_data->id_vendor==desc.idVendor &&
               (usb_data->id_product==0 ||
                usb_data->id_product==desc.idProduct))
            {
                detected = TRUE;
                break;
            }
        }

        if(!detected)
        {
            continue;
        }

        switch(usb_data->device_type)
        {
            case PCAT_MODEM_MANAGER_DEVICE_5G:
            {
                break;
            }
            case PCAT_MODEM_MANAGER_DEVICE_LTE:
            {
                break;
            }
            case PCAT_MODEM_MANAGER_DEVICE_ALL:
            {
                break;
            }
            default:
            {
                break;
            }
        }

        pcat_modem_manager_run_external_exec(mm_data, usb_data);
    }

    libusb_free_device_list(devs, 1);
}

static gpointer pcat_modem_manager_modem_work_thread_func(
    gpointer user_data)
{
    PCatModemManagerData *mm_data = (PCatModemManagerData *)user_data;
    PCatManagerMainConfigData *main_config_data;

    main_config_data = pcat_manager_main_config_data_get();

    while(mm_data->work_flag)
    {
        switch(mm_data->state)
        {
            case PCAT_MODEM_MANAGER_STATE_NONE:
            {
                if(pcat_modem_manager_modem_power_init(
                    mm_data, main_config_data))
                {
                    mm_data->state = PCAT_MODEM_MANAGER_STATE_READY;
                }
                else
                {
                    if(!mm_data->work_flag)
                    {
                        break;
                    }

                    g_warning("Modem power initialization failed!");
                    g_usleep(2000000);
                }

                break;
            }

            case PCAT_MODEM_MANAGER_STATE_READY:
            {
                pcat_modem_manager_scan_usb_devs(mm_data);

                g_usleep(1000000); /* WIP */

                break;
            }

            default:
            {
                break;
            }
        }
    }

    if(mm_data->external_control_exec_process!=NULL)
    {
        g_subprocess_force_exit(mm_data->external_control_exec_process);
        g_subprocess_wait(mm_data->external_control_exec_process, NULL, NULL);
        g_object_unref(mm_data->external_control_exec_process);
        mm_data->external_control_exec_process = NULL;
    }

    if(mm_data->gpio_modem_reset_line!=NULL)
    {
        gpiod_line_release(mm_data->gpio_modem_reset_line);
        mm_data->gpio_modem_reset_line = NULL;
    }
    if(mm_data->gpio_modem_rf_kill_line!=NULL)
    {
        gpiod_line_release(mm_data->gpio_modem_rf_kill_line);
        mm_data->gpio_modem_rf_kill_line = NULL;
    }
    if(mm_data->gpio_modem_power_line!=NULL)
    {
        gpiod_line_release(mm_data->gpio_modem_power_line);
        mm_data->gpio_modem_power_line = NULL;
    }

    if(mm_data->gpio_modem_reset_chip!=NULL)
    {
        gpiod_chip_close(mm_data->gpio_modem_reset_chip);
        mm_data->gpio_modem_reset_chip = NULL;
    }
    if(mm_data->gpio_modem_rf_kill_chip!=NULL)
    {
        gpiod_chip_close(mm_data->gpio_modem_rf_kill_chip);
        mm_data->gpio_modem_rf_kill_chip = NULL;
    }
    if(mm_data->gpio_modem_power_chip!=NULL)
    {
        gpiod_chip_close(mm_data->gpio_modem_power_chip);
        mm_data->gpio_modem_power_chip = NULL;
    }

    return NULL;
}

gboolean pcat_modem_manager_init()
{
    int errcode;

    if(g_pcat_modem_manager_data.initialized)
    {
        g_message("Modem Manager is already initialized!");

        return TRUE;
    }

    g_pcat_modem_manager_data.work_flag = TRUE;
    g_mutex_init(&(g_pcat_modem_manager_data.mutex));

    g_pcat_modem_manager_data.modem_mode_table = g_hash_table_new_full(
        g_str_hash, g_str_equal, NULL, NULL);
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "NR5G-SA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_5G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "NR5G-NSA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_5G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "LTE", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_LTE));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "WCDMA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_3G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "TDSCDMA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_3G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "GSM", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_2G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "HDR", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_2G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "CDMA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_2G));

    errcode = libusb_init(&g_pcat_modem_manager_data.usb_ctx);
    if(errcode!=0)
    {
        g_warning("Failed to initialize libusb: %s, 5G modem may not work!",
            libusb_strerror(errcode));
    }

    g_pcat_modem_manager_data.external_control_exec_stdout_buffer =
        g_string_new(NULL);

    g_pcat_modem_manager_data.modem_work_thread = g_thread_new(
        "pcat-modem-manager-work-thread",
        pcat_modem_manager_modem_work_thread_func,
        &g_pcat_modem_manager_data);

    g_pcat_modem_manager_data.initialized = TRUE;

    return TRUE;
}

void pcat_modem_manager_uninit()
{
    g_pcat_modem_manager_data.work_flag = FALSE;

    if(g_pcat_modem_manager_data.modem_work_thread!=NULL)
    {
        g_thread_join(g_pcat_modem_manager_data.modem_work_thread);
        g_pcat_modem_manager_data.modem_work_thread = NULL;
    }

    g_mutex_clear(&(g_pcat_modem_manager_data.mutex));

    if(g_pcat_modem_manager_data.usb_ctx!=NULL)
    {
        libusb_exit(g_pcat_modem_manager_data.usb_ctx);
        g_pcat_modem_manager_data.usb_ctx = NULL;
    }

    if(g_pcat_modem_manager_data.external_control_exec_stdout_buffer!=NULL)
    {
        g_string_free(
            g_pcat_modem_manager_data.external_control_exec_stdout_buffer,
            TRUE);
        g_pcat_modem_manager_data.external_control_exec_stdout_buffer = NULL;
    }

    g_pcat_modem_manager_data.initialized = FALSE;
}

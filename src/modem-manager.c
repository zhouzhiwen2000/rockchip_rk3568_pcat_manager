#include <stdio.h>
#include <errno.h>
#include <gpiod.h>
#include <libusb.h>
#include "modem-manager.h"
#include "common.h"

#define PCAT_MODEM_MANAGER_POWER_WAIT_TIME 30
#define PCAT_MODEM_MANAGER_RESET_ON_TIME 10
#define PCAT_MODEM_MANAGER_RESET_WAIT_TIME 30

typedef enum
{
    PCAT_MODEM_MANAGER_STATE_NONE,
    PCAT_MODEM_MANAGER_STATE_READY
}PCatModemManagerState;

typedef enum
{
    PCAT_MODEM_MANAGER_DEVICE_LTE,
    PCAT_MODEM_MANAGER_DEVICE_5G
}PCatModemManagerDeviceType;

typedef struct _PCatModemManagerUSBID
{
    PCatModemManagerDeviceType device_type;
    guint id_vendor;
    guint id_product;
}PCatModemManagerUSBID;

typedef struct _PCatModemManagerData
{
    gboolean initialized;
    gboolean work_flag;
    GMutex mutex;
    PCatModemManagerState state;
    GThread *modem_work_thread;

    struct gpiod_chip *gpio_modem_power_chip;
    struct gpiod_chip *gpio_modem_rf_kill_chip;
    struct gpiod_chip *gpio_modem_reset_chip;
    struct gpiod_line *gpio_modem_power_line;
    struct gpiod_line *gpio_modem_rf_kill_line;
    struct gpiod_line *gpio_modem_reset_line;
}PCatModemManagerData;

static PCatModemManagerUSBID g_pcat_modem_manager_supported_5g_list[] =
{
    {
        .device_type = PCAT_MODEM_MANAGER_DEVICE_5G,
        .id_vendor = 0x2C7C,
        .id_product = 0x0900
    }
};

static PCatModemManagerData g_pcat_modem_manager_data = {0};

static inline gboolean pcat_modem_manager_modem_power_init(
    PCatModemManagerData *mm_data, PCatManagerMainConfigData *main_config_data)
{
    guint i;

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

    gpiod_line_request_output(mm_data->gpio_modem_power_line,
        "gpio-modem-power",
        main_config_data->hw_gpio_modem_power_active_low ? 0 : 1);
    gpiod_line_request_output(mm_data->gpio_modem_rf_kill_line,
        "gpio-modem-rf-kill",
        main_config_data->hw_gpio_modem_rf_kill_active_low ? 1 : 0);
    gpiod_line_request_output(mm_data->gpio_modem_reset_line,
        "gpio-modem-reset",
        main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);

    for(i=0;i<PCAT_MODEM_MANAGER_POWER_WAIT_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    gpiod_line_request_output(mm_data->gpio_modem_reset_line,
        "gpio-modem-reset",
        main_config_data->hw_gpio_modem_reset_active_low ? 0 : 1);

    for(i=0;i<PCAT_MODEM_MANAGER_RESET_ON_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    gpiod_line_request_output(mm_data->gpio_modem_reset_line,
        "gpio-modem-reset",
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
                g_usleep(1000000); /* WIP */

                break;
            }

            default:
            {
                break;
            }
        }
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
    if(g_pcat_modem_manager_data.initialized)
    {
        g_message("Modem Manager is already initialized!");

        return TRUE;
    }

    g_pcat_modem_manager_data.work_flag = TRUE;
    g_mutex_init(&(g_pcat_modem_manager_data.mutex));

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

    g_pcat_modem_manager_data.initialized = FALSE;
}

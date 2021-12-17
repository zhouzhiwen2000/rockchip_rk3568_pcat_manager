#ifndef HAVE_PCAT_COMMON_H
#define HAVE_PCAT_COMMON_H

#include <glib.h>

typedef struct _PCatManagerMainConfigData
{
    gboolean valid;

    gchar *hw_gpio_modem_power_chip;
    guint hw_gpio_modem_power_line;
    gboolean hw_gpio_modem_power_active_low;
    gchar *hw_gpio_modem_rf_kill_chip;
    guint hw_gpio_modem_rf_kill_line;
    gboolean hw_gpio_modem_rf_kill_active_low;
    gchar *hw_gpio_modem_reset_chip;
    guint hw_gpio_modem_reset_line;
    gboolean hw_gpio_modem_reset_active_low;

    gchar *pm_serial_device;
    guint pm_serial_baud;
}PCatManagerMainConfigData;

PCatManagerMainConfigData *pcat_manager_main_config_data_get();

#endif


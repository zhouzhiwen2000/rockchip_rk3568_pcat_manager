#ifndef HAVE_PCAT_PMU_MANAGER_H
#define HAVE_PCAT_PMU_MANAGER_H

#include <glib.h>

G_BEGIN_DECLS

gboolean pcat_pmu_manager_init();
void pcat_pmu_manager_uninit();
void pcat_pmu_manager_shutdown_request();
void pcat_pmu_manager_reboot_request();
gboolean pcat_pmu_manager_shutdown_completed();
gboolean pcat_pmu_manager_reboot_completed();
void pcat_pmu_manager_watchdog_timeout_set(guint timeout);
gboolean pcat_pmu_manager_pmu_status_get(guint *battery_voltage,
    guint *charger_voltage, gboolean *on_battery, guint *battery_percentage);
void pcat_pmu_manager_schedule_time_update();
void pcat_pmu_manager_charger_on_auto_start(gboolean state);
void pcat_pmu_manager_net_status_led_setup(guint on_time, guint down_time,
    guint repeat);

G_END_DECLS

#endif


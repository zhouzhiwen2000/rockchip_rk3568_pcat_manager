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

G_END_DECLS

#endif


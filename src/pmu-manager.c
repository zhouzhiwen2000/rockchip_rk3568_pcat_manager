#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <termios.h>

#include "pmu-manager.h"
#include "common.h"

typedef struct _PCatPMUManagerData
{
    gboolean initialized;

    int serial_fd;
    GIOChannel *serial_channel;
}PCatPMUManagerData;

static PCatPMUManagerData g_pcat_pmu_manager_data = {0};

gboolean pcat_pmu_manager_init()
{
    if(g_pcat_pmu_manager_data.initialized)
    {
        return TRUE;
    }

    g_pcat_pmu_manager_data.initialized = TRUE;

    return TRUE;
}

void pcat_pmu_manager_uninit()
{
    if(!g_pcat_pmu_manager_data.initialized)
    {
        return;
    }

    g_pcat_pmu_manager_data.initialized = FALSE;
}


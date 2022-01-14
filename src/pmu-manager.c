#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "pmu-manager.h"
#include "common.h"

#define PCAT_PMU_MANAGER_STATEFS_BATTERY_PATH "/run/state/namespaces/Battery"

typedef enum
{
    PCAT_PMU_MANAGER_COMMAND_HEARTBEAT = 0x1,
    PCAT_PMU_MANAGER_COMMAND_STATUS_REPORT = 0x7,
    PCAT_PMU_MANAGER_COMMAND_STATUS_REPORT_ACK = 0x8,
    PCAT_PMU_MANAGER_COMMAND_DATE_TIME_SYNC = 0x9,
    PCAT_PMU_MANAGER_COMMAND_DATE_TIME_SYNC_ACK = 0xA,
    PCAT_PMU_MANAGER_COMMAND_SCHEDULE_STARTUP_TIME_SET = 0xB,
    PCAT_PMU_MANAGER_COMMAND_SCHEDULE_STARTUP_TIME_SET_ACK = 0xC,
    PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_SHUTDOWN = 0xD,
    PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_SHUTDOWN_ACK = 0xE,
    PCAT_PMU_MANAGER_COMMAND_HOST_REQUEST_SHUTDOWN = 0xF,
    PCAT_PMU_MANAGER_COMMAND_HOST_REQUEST_SHUTDOWN_ACK = 0x10,
    PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_FACTORY_RESET = 0x11,
    PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_FACTORY_RESET_ACK = 0x12,
    PCAT_PMU_MANAGER_COMMAND_WATCHDOG_TIMEOUT_SET = 0x13,
    PCAT_PMU_MANAGER_COMMAND_WATCHDOG_TIMEOUT_SET_ACK = 0x14,
}PCatPMUManagerCommandType;

typedef struct _PCatPMUManagerData
{
    gboolean initialized;

    guint check_timeout_id;

    int serial_fd;
    GIOChannel *serial_channel;
    guint serial_read_source;
    guint serial_write_source;
    GByteArray *serial_read_buffer;
    GByteArray *serial_write_buffer;
    guint16 serial_write_frame_num;

    gboolean shutdown_request;
    gboolean reboot_request;
    gboolean shutdown_planned;
    gboolean shutdown_process_completed;
    gboolean reboot_process_completed;

    guint last_battery_voltage;
    guint last_charger_voltage;
    gboolean last_on_battery_state;
    guint last_battery_percentage;
}PCatPMUManagerData;

static PCatPMUManagerData g_pcat_pmu_manager_data = {0};

static inline guint16 pcat_pmu_serial_compute_crc16(const guint8 *data,
    gsize len)
{
    guint16 crc = 0xFFFF;
    gsize i;
    guint j;

    for(i=0;i<len;i++)
    {
        crc ^= data[i];
        for(j=0;j<8;j++)
        {
            if(crc & 1)
            {
                crc = (crc >> 1) ^ 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

static gboolean pcat_pmu_serial_write_watch_func(GIOChannel *source,
    GIOCondition condition, gpointer user_data)
{
    PCatPMUManagerData *pmu_data = (PCatPMUManagerData *)user_data;
    gssize wsize;
    gsize total_write_size = 0;
    guint remaining_size;
    gboolean ret = FALSE;

    do
    {
        if(pmu_data->serial_write_buffer->len <= total_write_size)
        {
            break;
        }

        remaining_size = pmu_data->serial_write_buffer->len -
            total_write_size;

        wsize = write(pmu_data->serial_fd,
            pmu_data->serial_write_buffer->data + total_write_size,
            remaining_size > 4096 ? 4096 : remaining_size);

        if(wsize > 0)
        {
            total_write_size+=wsize;
        }
    }
    while(wsize > 0);

    if(total_write_size > 0)
    {
        g_byte_array_remove_range(pmu_data->serial_write_buffer, 0,
            total_write_size);
    }

    if(wsize < 0)
    {
        if(errno==EAGAIN)
        {
            ret = TRUE;
        }
        else
        {
            g_warning("Serial port write error: %s", strerror(errno));
        }
    }

    if(!ret)
    {
        pmu_data->serial_write_source = 0;
    }

    return ret;
}

static void pcat_pmu_serial_write_data_request(
    PCatPMUManagerData *pmu_data, guint16 command, gboolean frame_num_set,
    guint16 frame_num, const guint8 *extra_data, guint16 extra_data_len,
    gboolean need_ack)
{
    GByteArray *ba;
    guint16 sv;
    guint16 dp_size;

    ba = g_byte_array_new();

    g_byte_array_append(ba, (const guint8 *)"\xA5\x01\x81", 3);
    if(frame_num_set)
    {
        sv = frame_num;
        sv = GUINT16_TO_LE(sv);
        g_byte_array_append(ba, (const guint8 *)&sv, 2);
    }
    else
    {
        sv = pmu_data->serial_write_frame_num;
        sv = GUINT16_TO_LE(sv);
        g_byte_array_append(ba, (const guint8 *)&sv, 2);
        pmu_data->serial_write_frame_num++;
    }

    if(extra_data!=NULL && extra_data_len > 0 && extra_data_len <= 65532)
    {
        sv = extra_data_len + 3;
        sv = GUINT16_TO_LE(sv);
        g_byte_array_append(ba, (const guint8 *)&sv, 2);

        sv = command;
        sv = GUINT16_TO_LE(sv);
        g_byte_array_append(ba, (const guint8 *)&sv, 2);

        g_byte_array_append(ba, extra_data, extra_data_len);

        dp_size = extra_data_len + 3;
    }
    else
    {
        sv = 3;
        sv = GUINT16_TO_LE(sv);

        g_byte_array_append(ba, (const guint8 *)&sv, 2);

        sv = command;
        sv = GUINT16_TO_LE(sv);
        g_byte_array_append(ba, (const guint8 *)&sv, 2);

        dp_size = 3;
    }

    g_byte_array_append(ba,
        need_ack ? (const guint8 *)"\x01" : (const guint8 *)"\x00", 1);

    sv = pcat_pmu_serial_compute_crc16(ba->data + 1, dp_size + 6);
    sv = GUINT16_TO_LE(sv);
    g_byte_array_append(ba, (const guint8 *)&sv, 2);

    g_byte_array_append(ba, (const guint8 *)"\x5A", 1);

    if(pmu_data->serial_write_buffer->len > 131072)
    {
        g_byte_array_remove_range(pmu_data->serial_write_buffer, 0, 65536);
    }

    g_byte_array_append(pmu_data->serial_write_buffer, ba->data, ba->len);

    g_byte_array_unref(ba);

    if(pmu_data->serial_write_source==0)
    {
        pmu_data->serial_write_source = g_io_add_watch(
            pmu_data->serial_channel, G_IO_OUT,
            pcat_pmu_serial_write_watch_func, pmu_data);
    }
}

static void pcat_pmu_manager_date_time_sync(PCatPMUManagerData *pmu_data)
{
    guint8 data[7];
    GDateTime *dt;
    gint y, m, d, h, min, sec;

    dt = g_date_time_new_now_utc();
    g_date_time_get_ymd(dt, &y, &m, &d);
    h = g_date_time_get_hour(dt);
    min = g_date_time_get_minute(dt);
    sec = g_date_time_get_second(dt);

    data[0] = y & 0xFF;
    data[1] = (y >> 8) & 0xFF;
    data[2] = m;
    data[3] = d;
    data[4] = h;
    data[5] = min;
    data[6] = sec;

    g_date_time_unref(dt);

    pcat_pmu_serial_write_data_request(pmu_data,
        PCAT_PMU_MANAGER_COMMAND_DATE_TIME_SYNC, FALSE, 0,
        data, 7, TRUE);
}

static void pcat_pmu_manager_schedule_time_update_internal(
    PCatPMUManagerData *pmu_data)
{
    guint i;
    const PCatManagerMainUserConfigData *uconfig_data;
    const PCatManagerPowerScheduleData *sdata;
    GByteArray *startup_setup_buffer;
    guint8 v;

    uconfig_data = pcat_manager_main_user_config_data_get();
    if(uconfig_data->power_schedule_data!=NULL)
    {
        startup_setup_buffer = g_byte_array_new();

        for(i=0;i<uconfig_data->power_schedule_data->len;i++)
        {
            sdata = g_ptr_array_index(uconfig_data->power_schedule_data, i);
            if(!sdata->enabled || !sdata->action)
            {
                continue;
            }

            v = sdata->year & 0xFF;
            g_byte_array_append(startup_setup_buffer, &v, 1);
            v = (sdata->year >> 8) & 0xFF;
            g_byte_array_append(startup_setup_buffer, &v, 1);

            v = sdata->month;
            g_byte_array_append(startup_setup_buffer, &v, 1);

            v = sdata->day;
            g_byte_array_append(startup_setup_buffer, &v, 1);

            v = sdata->hour;
            g_byte_array_append(startup_setup_buffer, &v, 1);

            v = sdata->minute;
            g_byte_array_append(startup_setup_buffer, &v, 1);

            v = sdata->dow_bits;
            g_byte_array_append(startup_setup_buffer, &v, 1);

            v = sdata->enable_bits;
            g_byte_array_append(startup_setup_buffer, &v, 1);

            if(startup_setup_buffer->len >= 48)
            {
                break;
            }
        }

        if(startup_setup_buffer->len > 0)
        {
            pcat_pmu_serial_write_data_request(pmu_data,
                PCAT_PMU_MANAGER_COMMAND_SCHEDULE_STARTUP_TIME_SET, FALSE, 0,
                startup_setup_buffer->data, startup_setup_buffer->len, TRUE);

            g_message("Updated PMU schedule startup data.");
        }

        g_byte_array_unref(startup_setup_buffer);
    }
}

static void pcat_pmu_serial_status_data_parse(PCatPMUManagerData *pmu_data,
    const guint8 *data, guint len)
{
    guint16 battery_voltage, charger_voltage;
    guint16 gpio_input, gpio_output;
    gint y, m, d, h, min, s;
    GDateTime *pmu_dt, *host_dt;
    gint64 pmu_unix_time, host_unix_time;
    FILE *fp;
    gdouble battery_percentage;
    gboolean on_battery;

    if(len < 16)
    {
        return;
    }

    battery_voltage = data[0] + ((guint16)data[1] << 8);
    charger_voltage = data[2] + ((guint16)data[3] << 8);
    gpio_input = data[4] + ((guint16)data[5] << 8);
    gpio_output = data[6] + ((guint16)data[7] << 8);
    y = data[8] + ((guint16)data[9] << 8);
    m = data[10];
    d = data[11];
    h = data[12];
    min = data[13];
    s = data[14];

    pmu_dt = g_date_time_new_utc(y, m, d, h, min, (gdouble)s);
    pmu_unix_time = g_date_time_to_unix(pmu_dt);
    g_date_time_unref(pmu_dt);

    host_dt = g_date_time_new_now_utc();
    host_unix_time = g_date_time_to_unix(host_dt);
    g_date_time_unref(host_dt);

    if(pmu_unix_time - host_unix_time > 60 ||
        host_unix_time - pmu_unix_time > 60)
    {
        g_message("PMU time out of sync, send time sync command.");

        pcat_pmu_manager_date_time_sync(pmu_data);
    }

    g_debug("PMU report battery voltage %u mV, charger voltage %u mV, "
        "GPIO input state %X, output state %X.", battery_voltage,
        charger_voltage, gpio_input, gpio_output);

    on_battery = (charger_voltage < 4200);

    if(on_battery)
    {
        if(battery_voltage >= 4200)
        {
            battery_percentage = 100.0f;
        }
        else if(battery_voltage >= 4050)
        {
            battery_percentage = ((gdouble)battery_voltage - 4050) * 100 / 150;
        }
        else
        {
            battery_percentage = 0.0f;
        }
    }
    else
    {
        if(battery_voltage >= 4200)
        {
            battery_percentage = 100.0f;
        }
        else if(battery_voltage >= 4060)
        {
            battery_percentage = 90.0f +
                ((gdouble)battery_voltage - 4060) * 10 / 140;
        }
        else if(battery_voltage >= 3980)
        {
            battery_percentage = 80.0f +
                ((gdouble)battery_voltage - 3980) * 10 / 80;
        }
        else if(battery_voltage >= 3920)
        {
            battery_percentage = 70.0f +
                ((gdouble)battery_voltage - 3920) * 10 / 60;
        }
        else if(battery_voltage >= 3870)
        {
            battery_percentage = 60.0f +
                ((gdouble)battery_voltage - 3870) * 10 / 50;
        }
        else if(battery_voltage >= 3820)
        {
            battery_percentage = 50.0f +
                ((gdouble)battery_voltage - 3820) * 10 / 50;
        }
        else if(battery_voltage >= 3790)
        {
            battery_percentage = 40.0f +
                ((gdouble)battery_voltage - 3790) * 10 / 30;
        }
        else if(battery_voltage >= 3770)
        {
            battery_percentage = 30.0f +
                ((gdouble)battery_voltage - 3770) * 10 / 20;
        }
        else if(battery_voltage >= 3740)
        {
            battery_percentage = 20.0f +
                ((gdouble)battery_voltage - 3740) * 10 / 30;
        }
        else if(battery_voltage >= 3680)
        {
            battery_percentage = 10.0f +
                ((gdouble)battery_voltage - 3680) * 10 / 60;
        }
        else if(battery_voltage >= 3450)
        {
            battery_percentage = ((gdouble)battery_voltage - 3450) * 10 / 230;
        }
        else
        {
            battery_percentage = 0.0f;
        }
    }

    pmu_data->last_battery_voltage = battery_voltage;
    pmu_data->last_charger_voltage = charger_voltage;
    pmu_data->last_on_battery_state = on_battery;
    pmu_data->last_battery_percentage = battery_percentage * 100;

    fp = fopen(PCAT_PMU_MANAGER_STATEFS_BATTERY_PATH"/ChargePercentage", "w");
    if(fp!=NULL)
    {
        fprintf(fp, "%lf\n", battery_percentage);
        fclose(fp);
    }

    fp = fopen(PCAT_PMU_MANAGER_STATEFS_BATTERY_PATH"/Voltage", "w");
    if(fp!=NULL)
    {
        fprintf(fp, "%u\n", battery_voltage * 1000);
        fclose(fp);
    }

    fp = fopen(PCAT_PMU_MANAGER_STATEFS_BATTERY_PATH"/OnBattery", "w");
    if(fp!=NULL)
    {
        fprintf(fp, "%u\n", on_battery ? 1 : 0);
        fclose(fp);
    }
}

static void pcat_pmu_serial_read_data_parse(PCatPMUManagerData *pmu_data)
{
    guint i;
    gsize used_size = 0, remaining_size;
    GByteArray *buffer = pmu_data->serial_read_buffer;
    guint16 expect_len, extra_data_len;
    const guint8 *p, *extra_data;
    guint16 checksum, rchecksum;
    guint16 command;
    guint8 src, dst;
    gboolean need_ack;
    guint16 frame_num;

    if(buffer->len < 13)
    {
        return;
    }

    for(i=0;i<buffer->len;i++)
    {
        if(buffer->data[i]==0xA5)
        {
            p = buffer->data + i;
            remaining_size = buffer->len - i;
            used_size = i;

            if(remaining_size < 13)
            {
                break;
            }

            expect_len = p[5] + ((guint16)p[6] << 8);
            if(expect_len < 3 || expect_len > 65532)
            {
                used_size = i;
                continue;
            }
            if(expect_len + 10 > remaining_size)
            {
                if(used_size > 0)
                {
                    g_byte_array_remove_range(
                        pmu_data->serial_read_buffer, 0, used_size);
                }

                return;
            }

            if(p[9+expect_len]!=0x5A)
            {
                used_size = i;
                continue;
            }

            checksum = p[7+expect_len] + ((guint16)p[8+expect_len] << 8);
            rchecksum = pcat_pmu_serial_compute_crc16(p+1, 6+expect_len);

            if(checksum!=rchecksum)
            {
                g_warning("Serial port got incorrect checksum %X, "
                    "should be %X!", checksum ,rchecksum);

                i += 9 + expect_len;
                used_size = i + 1;
                continue;
            }

            src = p[1];
            dst = p[2];
            frame_num = p[3] + ((guint16)p[4] << 8);

            command = p[7] + ((guint16)p[8] << 8);
            extra_data_len = expect_len - 3;
            if(expect_len > 3)
            {
                extra_data = p + 9;
            }
            else
            {
                extra_data = NULL;
            }
            need_ack = (p[6 + expect_len]!=0);

            g_debug("Got command %X from %X to %X.", command, src, dst);

            if(dst==0x1 || dst==0x80 || dst==0xFF)
            {
                switch(command)
                {
                    case PCAT_PMU_MANAGER_COMMAND_STATUS_REPORT:
                    {
                        if(extra_data_len < 16)
                        {
                            break;
                        }

                        pcat_pmu_serial_status_data_parse(pmu_data,
                            extra_data, extra_data_len);

                        if(need_ack)
                        {
                            pcat_pmu_serial_write_data_request(pmu_data,
                                command+1, TRUE, frame_num, NULL, 0, FALSE);
                        }

                        break;
                    }
                    case PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_SHUTDOWN:
                    {
                        pcat_manager_main_request_shutdown();

                        if(need_ack)
                        {
                            pcat_pmu_serial_write_data_request(pmu_data,
                                command+1, TRUE, frame_num, NULL, 0, FALSE);
                        }

                        break;
                    }
                    case PCAT_PMU_MANAGER_COMMAND_HOST_REQUEST_SHUTDOWN_ACK:
                    {
                        if(pmu_data->shutdown_request)
                        {
                            pmu_data->shutdown_process_completed = TRUE;
                        }

                        break;
                    }
                    case PCAT_PMU_MANAGER_COMMAND_WATCHDOG_TIMEOUT_SET_ACK:
                    {
                        if(pmu_data->reboot_request)
                        {
                            pmu_data->reboot_process_completed = TRUE;
                        }
                        break;
                    }
                    case PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_FACTORY_RESET:
                    {
                        guint8 state = 0;

                        g_spawn_command_line_async(
                            "pcat-factory-reset.sh", NULL);

                        if(need_ack)
                        {
                            pcat_pmu_serial_write_data_request(pmu_data,
                                command+1, TRUE, frame_num, &state, 1, FALSE);
                        }

                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
            }

            i += 9 + expect_len;
            used_size = i + 1;
        }
        else
        {
            used_size = i;
        }
    }

    if(used_size > 0)
    {
        g_byte_array_remove_range(pmu_data->serial_read_buffer, 0, used_size);
    }
}

static gboolean pcat_pmu_serial_read_watch_func(GIOChannel *source,
    GIOCondition condition, gpointer user_data)
{
    PCatPMUManagerData *pmu_data = (PCatPMUManagerData *)user_data;
    gssize rsize;
    guint8 buffer[4096];

    while((rsize=read(pmu_data->serial_fd, buffer, 4096))>0)
    {
        g_byte_array_append(pmu_data->serial_read_buffer, buffer, rsize);
        if(pmu_data->serial_read_buffer->len > 131072)
        {
            g_byte_array_remove_range(pmu_data->serial_read_buffer, 0,
                pmu_data->serial_read_buffer->len - 65536);
        }

        pcat_pmu_serial_read_data_parse(pmu_data);
    }

    return TRUE;
}

static gboolean pcat_pmu_serial_open(PCatPMUManagerData *pmu_data)
{
    PCatManagerMainConfigData *main_config_data;
    int fd;
    GIOChannel *channel;
    struct termios options;
    int rspeed = B115200;

    main_config_data = pcat_manager_main_config_data_get();

    fd = open(main_config_data->pm_serial_device,
        O_RDWR | O_NOCTTY | O_NDELAY);
    if(fd < 0)
    {
        g_warning("Failed to open serial port %s: %s",
            main_config_data->pm_serial_device, strerror(errno));

        return FALSE;
    }

    switch(main_config_data->pm_serial_baud)
    {
        case 4800:
        {
            rspeed = B4800;
            break;
        }
        case 9600:
        {
            rspeed = B9600;
            break;
        }
        case 19200:
        {
            rspeed = B19200;
            break;
        }
        case 38400:
        {
            rspeed = B38400;
            break;
        }
        case 57600:
        {
            rspeed = B57600;
            break;
        }
        case 115200:
        {
            rspeed = B115200;
            break;
        }
        default:
        {
            g_warning("Invalid serial speed, set to default speed at %u.",
                115200);
            break;
        }
    }

    tcgetattr(fd, &options);
    cfmakeraw(&options);
    cfsetispeed(&options, rspeed);
    cfsetospeed(&options, rspeed);
    options.c_cflag &= ~CSIZE;
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~PARODD;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CRTSCTS;
    options.c_cflag |= CS8;
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_iflag &= ~(IGNBRK | BRKINT | ICRNL |
        INLCR | PARMRK | INPCK | ISTRIP | IXON | IXOFF | IXANY);
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN);
    options.c_oflag &= ~OPOST;
    options.c_cc[VMIN] = 1;
    options.c_cc[VTIME] = 0;
    tcflush(fd, TCIOFLUSH);
    tcsetattr(fd, TCSANOW, &options);

    channel = g_io_channel_unix_new(fd);
    if(channel==NULL)
    {
        g_warning("Cannot open channel for serial port %s!",
            main_config_data->pm_serial_device);
        close(fd);

        return FALSE;
    }
    g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL);

    pmu_data->serial_fd = fd;
    pmu_data->serial_channel = channel;
    pmu_data->serial_read_buffer = g_byte_array_new();
    pmu_data->serial_write_buffer = g_byte_array_new();

    pmu_data->serial_read_source = g_io_add_watch(channel,
        G_IO_IN, pcat_pmu_serial_read_watch_func, pmu_data);

    g_message("Open PMU serial port %s successfully.",
        main_config_data->pm_serial_device);

    return TRUE;
}

static void pcat_pmu_serial_close(PCatPMUManagerData *pmu_data)
{
    if(pmu_data->serial_write_source > 0)
    {
        g_source_remove(pmu_data->serial_write_source);
        pmu_data->serial_write_source = 0;
    }

    if(pmu_data->serial_read_source > 0)
    {
        g_source_remove(pmu_data->serial_read_source);
        pmu_data->serial_read_source = 0;
    }

    if(pmu_data->serial_channel!=NULL)
    {
        g_io_channel_unref(pmu_data->serial_channel);
        pmu_data->serial_channel = NULL;
    }

    if(pmu_data->serial_fd > 0)
    {
        close(pmu_data->serial_fd);
        pmu_data->serial_fd = -1;
    }

    if(pmu_data->serial_write_buffer!=NULL)
    {
        g_byte_array_unref(pmu_data->serial_write_buffer);
        pmu_data->serial_write_buffer = NULL;
    }

    if(pmu_data->serial_read_buffer!=NULL)
    {
        g_byte_array_unref(pmu_data->serial_read_buffer);
        pmu_data->serial_read_buffer = NULL;
    }
}

static gboolean pcat_pmu_manager_check_timeout_func(gpointer user_data)
{
    PCatPMUManagerData *pmu_data = (PCatPMUManagerData *)user_data;
    const PCatManagerMainUserConfigData *uconfig_data;
    const PCatManagerPowerScheduleData *sdata;
    guint i;
    GDateTime *dt;
    gboolean need_action = FALSE;
    guint dow;

    if(pmu_data->serial_channel==NULL)
    {
        return TRUE;
    }

    if(!pmu_data->reboot_request && !pmu_data->shutdown_request)
    {
        pcat_pmu_serial_write_data_request(pmu_data,
            PCAT_PMU_MANAGER_COMMAND_HEARTBEAT, FALSE, 0, NULL, 0, FALSE);

        uconfig_data = pcat_manager_main_user_config_data_get();
        if(uconfig_data->power_schedule_data!=NULL &&
            !pmu_data->shutdown_planned)
        {
            dt = g_date_time_new_now_utc();

            for(i=0;i<uconfig_data->power_schedule_data->len;i++)
            {
                sdata = g_ptr_array_index(
                    uconfig_data->power_schedule_data, i);

                if(!sdata->enabled || sdata->action)
                {
                    continue;
                }

                if(!(sdata->enable_bits &
                    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_MINUTE))
                {
                    continue;
                }

                if(sdata->enable_bits &
                    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_YEAR)
                {
                    if(g_date_time_get_year(dt)==sdata->year &&
                       g_date_time_get_month(dt)==sdata->month &&
                       g_date_time_get_day_of_month(dt)==sdata->day &&
                       g_date_time_get_hour(dt)==sdata->hour &&
                       g_date_time_get_minute(dt)==sdata->minute)
                    {
                        need_action = TRUE;
                    }
                }
                else if(sdata->enable_bits &
                    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_MONTH)
                {
                    if(g_date_time_get_month(dt)==sdata->month &&
                       g_date_time_get_day_of_month(dt)==sdata->day &&
                       g_date_time_get_hour(dt)==sdata->hour &&
                       g_date_time_get_minute(dt)==sdata->minute)
                    {
                        need_action = TRUE;
                    }
                }
                else if(sdata->enable_bits &
                    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_DAY)
                {
                    if(g_date_time_get_day_of_month(dt)==sdata->day &&
                       g_date_time_get_hour(dt)==sdata->hour &&
                       g_date_time_get_minute(dt)==sdata->minute)
                    {
                        need_action = TRUE;
                    }
                }
                else if(sdata->enable_bits &
                    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_DOW)
                {
                    dow = g_date_time_get_day_of_week(dt) % 7;

                    if(((sdata->dow_bits >> dow) & 1) &&
                       g_date_time_get_hour(dt)==sdata->hour &&
                       g_date_time_get_minute(dt)==sdata->minute)
                    {
                        need_action = TRUE;
                    }
                }
                else if(sdata->enable_bits &
                    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_HOUR)
                {
                    if(g_date_time_get_hour(dt)==sdata->hour &&
                       g_date_time_get_minute(dt)==sdata->minute)
                    {
                        need_action = TRUE;
                    }
                }
                else
                {
                    if(g_date_time_get_minute(dt)==sdata->minute)
                    {
                        need_action = TRUE;
                    }
                }

                if(need_action)
                {
                    pcat_manager_main_request_shutdown();
                    pmu_data->shutdown_planned = TRUE;
                }
            }

            g_date_time_unref(dt);
        }
    }

    if(pmu_data->serial_write_buffer!=NULL &&
       pmu_data->serial_write_buffer->len > 0 &&
       pmu_data->serial_write_source==0)
    {
        pmu_data->serial_write_source = g_io_add_watch(
            pmu_data->serial_channel, G_IO_OUT,
            pcat_pmu_serial_write_watch_func, pmu_data);
    }

    return TRUE;
}

gboolean pcat_pmu_manager_init()
{


    if(g_pcat_pmu_manager_data.initialized)
    {
        return TRUE;
    }

    g_pcat_pmu_manager_data.shutdown_request = FALSE;
    g_pcat_pmu_manager_data.reboot_request = FALSE;
    g_pcat_pmu_manager_data.shutdown_process_completed = FALSE;
    g_pcat_pmu_manager_data.reboot_process_completed = FALSE;

    g_mkdir_with_parents(PCAT_PMU_MANAGER_STATEFS_BATTERY_PATH, 0755);

    if(!pcat_pmu_serial_open(&g_pcat_pmu_manager_data))
    {
        return FALSE;
    }

    g_pcat_pmu_manager_data.check_timeout_id = g_timeout_add_seconds(1,
        pcat_pmu_manager_check_timeout_func, &g_pcat_pmu_manager_data);

    g_pcat_pmu_manager_data.initialized = TRUE;

    pcat_pmu_manager_watchdog_timeout_set(5);
    pcat_pmu_manager_date_time_sync(&g_pcat_pmu_manager_data);
    pcat_pmu_manager_schedule_time_update_internal(&g_pcat_pmu_manager_data);

    return TRUE;
}

void pcat_pmu_manager_uninit()
{
    if(!g_pcat_pmu_manager_data.initialized)
    {
        return;
    }

    if(g_pcat_pmu_manager_data.check_timeout_id > 0)
    {
        g_source_remove(g_pcat_pmu_manager_data.check_timeout_id);
        g_pcat_pmu_manager_data.check_timeout_id = 0;
    }

    pcat_pmu_serial_close(&g_pcat_pmu_manager_data);

    g_pcat_pmu_manager_data.initialized = FALSE;
}

void pcat_pmu_manager_shutdown_request()
{
    pcat_pmu_serial_write_data_request(&g_pcat_pmu_manager_data,
        PCAT_PMU_MANAGER_COMMAND_HOST_REQUEST_SHUTDOWN,
        FALSE, 0, NULL, 0, TRUE);
    g_pcat_pmu_manager_data.shutdown_request = TRUE;
}

void pcat_pmu_manager_reboot_request()
{
    pcat_pmu_manager_watchdog_timeout_set(60);
    g_pcat_pmu_manager_data.reboot_request = TRUE;
}

gboolean pcat_pmu_manager_shutdown_completed()
{
    return g_pcat_pmu_manager_data.shutdown_process_completed;
}

gboolean pcat_pmu_manager_reboot_completed()
{
    return g_pcat_pmu_manager_data.reboot_process_completed;
}

void pcat_pmu_manager_watchdog_timeout_set(guint timeout)
{
    guint8 timeouts[3] = {60, 60, timeout};

    if(!g_pcat_pmu_manager_data.initialized)
    {
        return;
    }

    if(g_pcat_pmu_manager_data.serial_channel==NULL ||
        g_pcat_pmu_manager_data.serial_write_buffer==NULL)
    {
        return;
    }

    pcat_pmu_serial_write_data_request(&g_pcat_pmu_manager_data,
        PCAT_PMU_MANAGER_COMMAND_WATCHDOG_TIMEOUT_SET, FALSE, 0,
        timeouts, 3, TRUE);
}

gboolean pcat_pmu_manager_pmu_status_get(guint *battery_voltage,
    guint *charger_voltage, gboolean *on_battery, guint *battery_percentage)
{
    if(!g_pcat_pmu_manager_data.initialized)
    {
        return FALSE;
    }

    if(battery_voltage!=NULL)
    {
        *battery_voltage = g_pcat_pmu_manager_data.last_battery_voltage;
    }
    if(charger_voltage!=NULL)
    {
        *charger_voltage = g_pcat_pmu_manager_data.last_charger_voltage;
    }
    if(on_battery!=NULL)
    {
        *on_battery = g_pcat_pmu_manager_data.last_on_battery_state;
    }
    if(battery_percentage!=NULL)
    {
        *battery_percentage = g_pcat_pmu_manager_data.last_battery_percentage;
    }

    return TRUE;
}

void pcat_pmu_manager_schedule_time_update()
{
    if(!g_pcat_pmu_manager_data.initialized)
    {
        return;
    }

    pcat_pmu_manager_schedule_time_update_internal(&g_pcat_pmu_manager_data);
}

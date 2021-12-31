#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "pmu-manager.h"
#include "common.h"

typedef enum
{
    PCAT_PMU_MANAGER_COMMAND_HEARTBEAT = 0x1,
    PCAT_PMU_MANAGER_COMMAND_STATUS_REPORT = 0x7,
    PCAT_PMU_MANAGER_COMMAND_STATUS_REPORT_ACK = 0x8,
    PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_SHUTDOWN = 0xD,
    PCAT_PMU_MANAGER_COMMAND_PMU_REQUEST_SHUTDOWN_ACK = 0xE,
    PCAT_PMU_MANAGER_COMMAND_HOST_REQUEST_SHUTDOWN = 0xF,
    PCAT_PMU_MANAGER_COMMAND_HOST_REQUEST_SHUTDOWN_ACK = 0x10
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

    gboolean shutdown_process_completed;
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

static void pcat_pmu_serial_status_data_parse(PCatPMUManagerData *pmu_data,
    const guint8 *data, guint len)
{

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
                        g_spawn_command_line_async("poweroff", NULL);

                        if(need_ack)
                        {
                            pcat_pmu_serial_write_data_request(pmu_data,
                                command+1, TRUE, frame_num, NULL, 0, FALSE);
                        }

                        break;
                    }
                    case PCAT_PMU_MANAGER_COMMAND_HOST_REQUEST_SHUTDOWN_ACK:
                    {
                        pmu_data->shutdown_process_completed = TRUE;

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

    if(pmu_data->serial_channel==NULL)
    {
        return TRUE;
    }

    pcat_pmu_serial_write_data_request(pmu_data,
        PCAT_PMU_MANAGER_COMMAND_HEARTBEAT, FALSE, 0, NULL, 0, FALSE);

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

    if(!pcat_pmu_serial_open(&g_pcat_pmu_manager_data))
    {
        return FALSE;
    }

    g_pcat_pmu_manager_data.check_timeout_id = g_timeout_add_seconds(1,
        pcat_pmu_manager_check_timeout_func, &g_pcat_pmu_manager_data);

    g_pcat_pmu_manager_data.initialized = TRUE;

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
}

gboolean pcat_pmu_manager_shutdown_completed()
{
    return g_pcat_pmu_manager_data.shutdown_process_completed;
}

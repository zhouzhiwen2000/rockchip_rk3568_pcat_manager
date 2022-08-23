#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

static inline uint16_t compute_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    uint32_t j;

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

int main(int argc, char *argv[])
{
    int fd;
    struct termios options;
    int rspeed = B115200;
    int speed = 115200;
    const char *serial_device = "/dev/ttyS4";
    uint8_t buffer[13] = {0xA5, 0x01, 0x81, 0x0, 0x0, 0x3, 0x0,
        0x1, 0x0, 0x0, 0x0, 0x0, 0x5A};
    uint16_t frame_num = 0;
    uint16_t crc;

    if(argc > 1)
    {
        serial_device = argv[1];
    }
    if(argc > 2)
    {
        if(sscanf(argv[2], "%d", &speed) < 1)
        {
            speed = 115200;
        }
    }

    /* daemon(1, 1); */

    fd = open(serial_device, O_RDWR | O_NOCTTY);
    if(fd < 0)
    {
        fprintf(stderr, "Failed to open serial port %s: %s",
            serial_device, strerror(errno));

        return 1;
    }

    switch(speed)
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
            fprintf(stderr, "Invalid serial speed, "
                "set to default speed at %u.", 115200);
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

    while(1)
    {
        buffer[3] = frame_num & 0xFF;
        buffer[4] = (frame_num >> 8) & 0xFF;
        frame_num++;

        crc = compute_crc16(buffer + 1, 9);
        buffer[10] = crc & 0xFF;
        buffer[11] = (crc >> 8) & 0xFF;

        write(fd, buffer, 13);

        usleep(1000000);
    }

    close(fd);

    return 0;
}

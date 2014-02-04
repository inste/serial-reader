#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#define MODEM			"/dev/ttyUSB1"
#define SETF			"SNDF\r"
#define BUFFER			2048
#define SMALL			32
#define ITEMS_COUNT		180
#define BAUDRATE		B19200

#define OUTDATE_TIMEOUT		10	// Seconds
#define MICROSLEEP			20000 // 20 us

#define DATA_PORT		5000
#define LOG_PORT		4999


struct datum {
	char	datum[SMALL];
};

struct datum global_data[ITEMS_COUNT];
time_t	last_update;


int dev_exists(char * filename) {
	struct stat	buffer;
	return ((stat (filename, &buffer) == 0) && S_ISCHR(buffer.st_mode));
}

int open_device(char * device) {
    struct termios tio;
    int tty_fd;

    if (!dev_exists(device)) {
    	return -ENXIO;
    }

    memset(&tio, 0, sizeof(tio));
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_cflag = CS8 | CREAD | CLOCAL;
    tio.c_lflag = 0;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 5;

    if ((tty_fd = open(device, O_RDWR)) == -1) {
        return -1;
    }

    cfsetospeed(&tio, BAUDRATE);
    cfsetispeed(&tio, BAUDRATE);
    tcsetattr(tty_fd, TCSANOW, &tio);

    return tty_fd;
}

void init_data(struct datum * data) {
	int i;

	for (i = 0; i < ITEMS_COUNT; ++i) {
		memset(data[i].datum, 0, SMALL);
	}
}

void process_recv(struct datum * data, char * recv) {
	int i = 0, j, pos, idx = 5;
	char temp[SMALL];

/*	0x03 - STX, start transmission
 *  0x2c - comma, fields delimiter
 *  0x20 - space
 *  0x00 - NULL

*/
	while (0x02 != recv[i++])
		;;

	pos = i;

	while (1) {

		if (0x03 != recv[i] && 0x2c != recv[i] && 0x00 != recv[i] && i < BUFFER) {
			++i;
			continue;
		}

		if (0x2c == recv[i]) {
			memset(temp, 0, SMALL);
			memcpy(temp, recv + pos, i - pos);

			// Cutting down empty spaces from the end of fields
			for (j = i - pos - 1; j > -1; --j) {
				if (0x20 == temp[j])
					temp[j] = 0x00;
				else
					break;
			}

			strncpy(data[idx].datum, temp, SMALL);
			++idx;

			pos = ++i;
			continue;
		}

		if (0x03 == recv[i]) {
			last_update = time(NULL);
			break;
		}

		if (0x00 == recv[i]) {
			printf("Unexpected error: got NULL before ETX\n");
			break;
		}

		if (BUFFER == i) {
			printf("Unexpected error: buffer overrun\n");
			break;
		}

	}

}

int debug() {
    int count = 0;
    int fd;
    char data[BUFFER];
    char c;

    if ((fd = open("dump.bin", O_RDONLY)) != -1) {
    	while (read(fd, &c, 1) > 0) {
    		data[count++] = c;
    	}
    	close (fd);
    } else {
        printf("dump.bin not found\n");
        return -1;
    }

    data[count] = '\0';
    process_recv(global_data, data);
    return 0;
}

int get_max(int tty_fd, int listenfd, int ll_fd, int log_fd, int * sock_fd, int sock_fd_count) {
	int i;
	int max = 0;

	for (i = 0; i < sock_fd_count; ++i) {
		if (sock_fd[i] > max)
			max = sock_fd[i];
	}

	max = (ll_fd > max) ? ll_fd : max;
	max = (log_fd > max) ? log_fd : max;
	max = (tty_fd > max) ? tty_fd : max;
	return (listenfd > max) ? listenfd : max;
}

void send_log(int log_sock_fd, char * log) {
	if (log_sock_fd > 0)
		write(log_sock_fd, log, strlen(log));
}

int open_socket(int port) {
	int sock_fd;
    struct sockaddr_in serv_addr;

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    bind(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    return sock_fd;
}

int main(void) {
    int tty_fd, is_dev_opened;
    char data[BUFFER], temp[BUFFER], device_data[BUFFER], logtmp[BUFFER];
    int count, data_count;
    int i, j, idx;

    int listen_fd;
    int log_listen_fd;
    int log_sock_fd = -1;
    int sock_fd[BUFFER];
    int sock_fd_count = 0;

    int finished;

    time_t open_time;

    char * reqs;

    fd_set rfds;
    struct timeval tv;
    int retval;

    listen_fd = open_socket(DATA_PORT);
    listen(listen_fd, 25);

    log_listen_fd = open_socket(LOG_PORT);
    listen(log_listen_fd, 2);

    reqs = (char *)malloc(sizeof(char) * BUFFER * (SMALL + 1));

    init_data(global_data);

    is_dev_opened = 0;

    while (1) {

    	if (!is_dev_opened) {
    		tty_fd = open_device(MODEM);
    		++is_dev_opened;
    		open_time = time(NULL);

    		if (tty_fd > 0) {
    			snprintf(logtmp, BUFFER, "Device %s opened correctly\n", MODEM);
    			send_log(log_sock_fd, logtmp);
    			write(tty_fd, SETF, strlen(SETF));
    			usleep(MICROSLEEP);
    		}

    		if (-ENXIO == tty_fd) {
    			snprintf(logtmp, BUFFER, "Device file %s doesn't exist\n", MODEM);
    			send_log(log_sock_fd, logtmp);
    			is_dev_opened = 0;
    			usleep(MICROSLEEP);
    		}
    		if (!tty_fd) {
    			snprintf(logtmp, BUFFER, "Device file %s can't be opened\n", MODEM);
    			send_log(log_sock_fd, logtmp);
    			is_dev_opened = 0;
    			usleep(MICROSLEEP);
    		}
    	}

    	tv.tv_sec = 0;
    	tv.tv_usec = MICROSLEEP;

    	FD_ZERO(&rfds);
    	FD_SET(listen_fd, &rfds);
    	FD_SET(log_listen_fd, &rfds);

    	if (is_dev_opened)
    		FD_SET(tty_fd, &rfds);

    	if (log_sock_fd > 0)
    		FD_SET(log_sock_fd, &rfds);

    	for (i = 0; i < sock_fd_count; ++i)
    		FD_SET(sock_fd[i], &rfds);

    	retval = select(get_max(tty_fd, listen_fd, log_listen_fd, log_sock_fd, sock_fd, sock_fd_count) + 1, &rfds, NULL, NULL, &tv);

    	if (retval) {

    		if (is_dev_opened && FD_ISSET(tty_fd, &rfds)) {
				count = read(tty_fd, temp, BUFFER);

				finished = 0;
				for (i = 0; i < count; ++i) {
					if ('\r' == temp[i]) {
						++finished;
						break;
					}
				}

				memcpy(device_data + data_count, temp, count * sizeof(char));
				data_count += count;

				if (time(NULL) - open_time > OUTDATE_TIMEOUT) { // Device vanished during reading, 10 sec timeout
	    			data_count = 0;
	    			close(tty_fd);
	    			is_dev_opened = 0;

	    			snprintf(logtmp, BUFFER, "Device  %s vanished during reading, closing\n", MODEM);
	    			send_log(log_sock_fd, logtmp);
	    			continue;
				}

				if (finished) {
					device_data[data_count] = '\0';
	    		//	printf("%d '%s'\n", data_count, data);
	    			process_recv(global_data, device_data);
	    			data_count = 0;
	    			close(tty_fd);
	    			is_dev_opened = 0;

	    			snprintf(logtmp, BUFFER, "Closing device %s\n", MODEM);
	    			send_log(log_sock_fd, logtmp);
				}
    		}


    		if (FD_ISSET(listen_fd, &rfds)) {
    			sock_fd[sock_fd_count] = accept(listen_fd, (struct sockaddr*)NULL, NULL);
    			memset(reqs + sock_fd_count * (SMALL + 1), 0, sizeof(char) * (SMALL + 1));
    			++sock_fd_count;

    			snprintf(logtmp, BUFFER, "Accepted, has %d conns\n", sock_fd_count);
    			send_log(log_sock_fd, logtmp);
    			continue;
    		}

    		if (FD_ISSET(log_listen_fd, &rfds)) {
    			if (log_sock_fd > 0) { // Closing previous connection
    				close(log_sock_fd);
    			}

    			log_sock_fd = accept(log_listen_fd, (struct sockaddr*)NULL, NULL);
    			continue;
    		}

    		if (FD_ISSET(log_sock_fd, &rfds)) {
    			j = 0;
    			ioctl(log_sock_fd, FIONREAD, &j);
    			if (!j) { // Socket was closed on client
    				log_sock_fd = -1;
    			} else {// Data arrived to read should be ignored on this sock
    				read(log_sock_fd, temp, BUFFER);
    			}
    		}


    		for (i = 0; i < sock_fd_count; ++i) {
    			if (FD_ISSET(sock_fd[i], &rfds)) {
    				j = 0;
    				ioctl(sock_fd[i], FIONREAD, &j);

    				if (!j) { // Socket was closed, cleaning up
    					memmove(sock_fd + i, sock_fd + i + 1, sizeof(int) * sock_fd_count - i - 1);
    					--sock_fd_count;

    	    			snprintf(logtmp, BUFFER, "Closed, has %d conns\n", sock_fd_count);
    	    			send_log(log_sock_fd, logtmp);
    					continue;
    				} else {
    					count = read(sock_fd[i], data, SMALL);

    					if (count + *(reqs + i * (SMALL + 1)) <= SMALL) { // Unexpectedly big request?

    						memcpy(reqs + i * (SMALL + 1) + *(reqs + i * (SMALL + 1)) + 1, data, count);
    						*(reqs + i * (SMALL + 1)) += count;

    						finished = 0;
    						for (j = 0; j < *(reqs + i * (SMALL + 1)); ++j) {
    							if ('\r' == *(reqs + i * (SMALL + 1) + 1 + j)) {
    								++finished;
    								break;
    							}
    						}

    						if (finished) {
    							*(reqs + i * (SMALL + 1) + j + 1) = 0x00;
    							idx = strtol(reqs + i * (SMALL + 1) + 1, NULL, 10);

    							if (idx > 4 && idx < ITEMS_COUNT) {
    								if (time(NULL) - last_update < OUTDATE_TIMEOUT) { // Old data, don't send it?
    									count = snprintf(data, SMALL, "%s\r", global_data[idx].datum);
    									write(sock_fd[i], data, count);
    								} else {
    									count = snprintf(data, SMALL, "OUTDATED\r");
    									write(sock_fd[i], data, count);
    								}
    							} else {
    								count = snprintf(data, SMALL, "BAD_REQUEST\r");
    								write(sock_fd[i], data, count);
    							}

    							//printf("Got '%s'\n", reqs + i * (SMALL + 1) + 1);
    							*(reqs + i * (SMALL + 1)) = 0x00;
    						}
    					} else {
							count = snprintf(data, SMALL, "BIG_REQUEST\r");
							write(sock_fd[i], data, count);
    					}
    				}
    			}
    		}
    	}

    }

    free(reqs);

    return 0;
}


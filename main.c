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

#define SETF			"SNDF\r"
#define BUFFER			2048
#define SMALL			32
#define ITEMS_COUNT		180
#define BAUDRATE		B19200

#define OUTDATE_TIMEOUT		10	// Seconds
#define MICROSLEEP		20000 // 20 us

#define LOG_PORT		4999

#define __DEBUG__



struct datum {
	char	datum[SMALL];
};

struct datum	global_data[ITEMS_COUNT];
time_t		last_update = 0;


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

void write_log(int log_fd, int log_sock_fd, char * log) {
	size_t size;
	char buffer[BUFFER];

	if (log_fd > -1) {
		size = snprintf(buffer, BUFFER, "[%u] %s\r\n", (unsigned)time(NULL), log);
		write(log_fd, buffer, size);

		if (log_sock_fd > 0)
			write(log_sock_fd, buffer, size);
		fdatasync(log_fd);
	}
}

int process_recv(int log_fd, int log_sock_fd, struct datum * globaldata, char * recv) {
	int i = 0, j, pos, idx = 5;
	char temp[SMALL];
	int retval = 0;

	char * logtmp = (char *)malloc(sizeof(char) * BUFFER);
	struct datum * newdata = (struct datum *)malloc(sizeof(struct datum) * ITEMS_COUNT);

	memset(newdata, 0, sizeof(struct datum) * ITEMS_COUNT);

/*	0x03 - STX, start transmission
 *	0x2c - comma, fields delimiter
 *	0x20 - space
 *	0x00 - NULL

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
			if ((i - pos) < SMALL - 1) {
				memset(temp, 0, SMALL);
				memcpy(temp, recv + pos, i - pos);


				// Cutting down empty spaces from the end of fields
				for (j = i - pos - 1; j > -1; --j) {
					if (0x20 == temp[j])
						temp[j] = 0x00;
					else
						break;
				}

				strncpy(newdata[idx].datum, temp, SMALL);
				++idx;
			} else {
				snprintf(logtmp, BUFFER, "Possible data corruption, fields overflow");
				write_log(log_fd, log_sock_fd, logtmp);

				retval = -1;
				goto cleanup;
			}
			pos = ++i;
			continue;
		}

		if (0x03 == recv[i]) {
			break;
		}

		if (0x00 == recv[i]) {
			snprintf(logtmp, BUFFER, "Unexpected error: got NULL before ETX");
			write_log(log_fd, log_sock_fd, logtmp);

			retval = -1;
			goto cleanup;
		}

		if (BUFFER == i) {
			snprintf(logtmp, BUFFER, "Unexpected error: buffer overrun");
			write_log(log_fd, log_sock_fd, logtmp);

			retval = -1;
			goto cleanup;
		}
	}

	memcpy(globaldata, newdata, sizeof(struct datum) * ITEMS_COUNT);
	last_update = time(NULL);

cleanup:
	free(newdata);
	free(logtmp);
	return retval;
}

/*
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
} */

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

void process_native(char * request, int sock_fd) {
	int idx, count;
	char * data = (char *)malloc(sizeof(char) * BUFFER);

	idx = strtol(request, NULL, 10);

	if (idx > 4 && idx < ITEMS_COUNT) {
		if (time(NULL) - last_update < OUTDATE_TIMEOUT) { // Old data, don't send it?
			count = snprintf(data, SMALL, "%s\r", global_data[idx].datum);
			write(sock_fd, data, count);
		} else {
			count = snprintf(data, SMALL, "OUTDATED\r");
			write(sock_fd, data, count);
		}
	} else {
		count = snprintf(data, SMALL, "BAD_REQUEST\r");
		write(sock_fd, data, count);
	}
}


int event_loop(char * device_file, int port, int log_fd) {
	int tty_fd, is_dev_opened;
	char data[BUFFER], device_data[BUFFER];
	int count, data_count;
	int i, j;

	int listen_fd;
	int log_listen_fd;
	int log_sock_fd = -1;
	int sock_fd[BUFFER];
	int sock_fd_count = 0;

	struct sockaddr_in	sa = { 0 };
	socklen_t		sl = sizeof(sa);

	int finished;

	time_t open_time;

	char * reqs, * temp, * logtmp;

	fd_set rfds;
	struct timeval tv;
	int retval;

	listen_fd = open_socket(port);
	listen(listen_fd, 25);

	log_listen_fd = open_socket(LOG_PORT);
	listen(log_listen_fd, 2);

	reqs = (char *)malloc(sizeof(char) * BUFFER * (SMALL + 1));
	logtmp = (char *)malloc(sizeof(char) * BUFFER);

	for (i = 0; i < ITEMS_COUNT; ++i) {
		memset(global_data[i].datum, 0, SMALL);
	}

	is_dev_opened = 0;

	snprintf(logtmp, BUFFER, "Process started");
	write_log(log_fd, log_sock_fd, logtmp);

	while (1) {

		if (!is_dev_opened) {
			tty_fd = open_device(device_file);
			++is_dev_opened;
			open_time = time(NULL);

			if (tty_fd > 0) {
				//	snprintf(logtmp, BUFFER, "Device %s opened correctly", device_file);
				//	write_log(log_fd, log_sock_fd, logtmp);

				write(tty_fd, SETF, strlen(SETF));
				usleep(MICROSLEEP);
			}
			if (-ENXIO == tty_fd) {
				snprintf(logtmp, BUFFER, "Device file %s doesn't exist", device_file);
				write_log(log_fd, log_sock_fd, logtmp);
				is_dev_opened = 0;
				usleep(MICROSLEEP);
			}
			if (!tty_fd) {
				snprintf(logtmp, BUFFER, "Device file %s can't be opened", device_file);
				write_log(log_fd, log_sock_fd, logtmp);
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

		retval = select(
				get_max(tty_fd,
					listen_fd,
					log_listen_fd,
					log_sock_fd,
					sock_fd,
					sock_fd_count) + 1,
				&rfds,
				NULL,
				NULL,
				&tv);
		if (retval) {

			if (is_dev_opened && FD_ISSET(tty_fd, &rfds)) {
				temp = (char *)malloc(BUFFER * sizeof(char));
				count = read(tty_fd, temp, BUFFER);


				if (count + data_count > BUFFER) {
					free(temp);
					goto device_reading_cleanup;
				}

				finished = 0;
				for (i = 0; i < count; ++i) {
					if ('\r' == temp[i]) {
						++finished;
						break;
					}
				}

				memcpy(device_data + data_count, temp, count * sizeof(char));
				data_count += count;

				free(temp);

				if ((time(NULL) - open_time) > OUTDATE_TIMEOUT) { // Device vanished during reading, 10 sec timeout
					snprintf(logtmp, BUFFER, "Device  %s vanished during reading, closing", device_file);
					write_log(log_fd, log_sock_fd, logtmp);

					goto device_reading_cleanup;
				}

				if (finished) {
					device_data[data_count] = '\0';

					if (-1 == process_recv(log_fd, log_sock_fd, global_data, device_data)) {
						last_update = 0;
					}

					//snprintf(logtmp, BUFFER, "Closing device %s", device_file);
					//write_log(log_fd, log_sock_fd, logtmp);

					goto device_reading_cleanup;
				}

				goto device_reading_exit;

device_reading_cleanup:
				data_count = 0;
				close(tty_fd);
				is_dev_opened = 0;
				continue;
device_reading_exit:
				;;
			}

			if (FD_ISSET(listen_fd, &rfds)) {
				sock_fd[sock_fd_count] = accept(listen_fd, (struct sockaddr *)&sa, &sl);
				getpeername(sock_fd[sock_fd_count], (struct sockaddr *)&sa, &sl);
				memset(reqs + sock_fd_count * (SMALL + 1), 0, sizeof(char) * (SMALL + 1));
				++sock_fd_count;

				snprintf(logtmp, BUFFER,
						"Accepted data connection from %s, has %d conns",
						inet_ntoa(sa.sin_addr), sock_fd_count);
				write_log(log_fd, log_sock_fd, logtmp);
				continue;
			}
			if (FD_ISSET(log_listen_fd, &rfds)) {
				if (log_sock_fd > 0) { // Closing previous connection
					close(log_sock_fd);
				}

				log_sock_fd = accept(log_listen_fd, (struct sockaddr *)&sa, &sl);
				getpeername(log_sock_fd, (struct sockaddr *)&sa, &sl);
				snprintf(logtmp, BUFFER, "Accepted log connection from %s", inet_ntoa(sa.sin_addr));
				write_log(log_fd, log_sock_fd, logtmp);
				continue;
			}
			if (log_sock_fd > -1 && FD_ISSET(log_sock_fd, &rfds)) {
				j = 0;
				ioctl(log_sock_fd, FIONREAD, &j);
				if (!j) { // Socket was closed on client
					close(log_sock_fd);
					log_sock_fd = -1;

					snprintf(logtmp, BUFFER, "Log connection closed");
					write_log(log_fd, log_sock_fd, logtmp);
				} else {// Data arrived to read should be ignored on this sock
					temp = (char *)malloc(BUFFER * sizeof(char));
					read(log_sock_fd, temp, BUFFER);
					free(temp);
				}
			}
			for (i = 0; i < sock_fd_count; ++i) {
				if (FD_ISSET(sock_fd[i], &rfds)) {
					j = 0;
					ioctl(sock_fd[i], FIONREAD, &j);

					if (!j) { // Socket was closed, cleaning up
						close(sock_fd[i]);
						memmove(sock_fd + i, sock_fd + i + 1, sizeof(int) * sock_fd_count - i - 1);
						--sock_fd_count;

						snprintf(logtmp, BUFFER, "Closed, has %d conns", sock_fd_count);
						write_log(log_fd, log_sock_fd, logtmp);
						continue;
					} else {
						count = read(sock_fd[i], data, SMALL);

						if (count + *(reqs + i * (SMALL + 1)) <= SMALL) { // Unexpectedly big request?
							memcpy(reqs + i * (SMALL + 1) + *(reqs + i * (SMALL + 1)) + 1, data, count);
							*(reqs + i * (SMALL + 1)) += count;

							finished = 0;
							for (j = 0; j < *(reqs + i * (SMALL + 1)); ++j) {
								if ('\r' == *(reqs + i * (SMALL + 1) + 1 + j)) {
									finished = 1;  // Native protocol
									break;
								}
								if ('\n' == *(reqs + i * (SMALL + 1) + 1 + j)) {
									finished = 2;  // Zabbix protocol
									break;
								}

							}

							if (finished) {
								*(reqs + i * (SMALL + 1) + j + 1) = 0x00;
								switch (finished) {
								case 1 :
									process_native(reqs + i * (SMALL + 1) + 1, sock_fd[i]);
									break;
								}
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

		if (!retval) {
			if (is_dev_opened && time(NULL) - last_update > OUTDATE_TIMEOUT) {
				data_count = 0;
				close(tty_fd);
				is_dev_opened = 0;

				snprintf(logtmp, BUFFER, "Endpoint communication was lost during reading, reopen");
				write_log(log_fd, log_sock_fd, logtmp);
			}
		}
	}

	free(reqs);
	free(logtmp);

	return 0;
}

int main(int argc, char ** argv) {

	pid_t pid, sid;
	char device[SMALL], port[SMALL], logfile[SMALL];
	int log_fd, port_int = 0;

	if (argc > 1 && strlen(argv[1]) > 0)
		strncpy(device, argv[1], SMALL);
	else
		strcpy(device, "/dev/ttyUSB1");

	if (argc > 2 && strlen(argv[2]) > 0)
		strncpy(port, argv[2], SMALL);
	else
		strcpy(port, "5000");

	if (argc > 3 && strlen(argv[3]) > 0)
		strncpy(logfile, argv[3], SMALL);
	else
		strcpy(logfile, "serial-reader.log");

	umask(S_IWGRP | S_IWOTH);

	/* Trying to check availability of logfile */
	if (-1 == (log_fd = open(logfile, O_CREAT | O_WRONLY | O_APPEND))) {
		printf("Unable to open logfile %s\n", logfile);
		exit(EXIT_FAILURE);
	} else
		close(log_fd);

	/* Checking correctness of specified port */
	port_int = strtol(port, NULL, 10);

	if (port_int < 10 || port_int > 65535) {
		printf("Unexpected port: %s\n", port);
		exit(EXIT_FAILURE);
	}

#ifndef __DEBUG__
	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}

	if (pid > 0) {
	exit(EXIT_SUCCESS);
	}

	umask(S_IWGRP | S_IWOTH);

	sid = setsid();
	if (sid < 0) {
	exit(EXIT_FAILURE);
	}

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

#endif /* __DEBUG__ */

	log_fd = open(logfile, O_CREAT | O_WRONLY | O_APPEND);

	event_loop(device, port_int, log_fd);


	return 0;
}


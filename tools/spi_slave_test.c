// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/cm4_spi_slave"
#define DEFAULT_READ_SIZE 4096U

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage:\n"
		"  %s [-d DEVICE] read [-n BYTES] [-N] [-t MS]\n"
		"  %s [-d DEVICE] write-text [-t MS] TEXT\n"
		"  %s [-d DEVICE] write-file [-t MS] FILE\n"
		"  %s [-d DEVICE] poll [-e read|write|both] [-t MS]\n"
		"\n"
		"Options:\n"
		"  -d DEVICE  Character device (default %s)\n"
		"  -n BYTES   Maximum bytes for one read (default %u)\n"
		"  -N         Use nonblocking mode\n"
		"  -t MS      Poll timeout; -1 waits indefinitely\n"
		"  -e EVENTS  Events requested by poll (default both)\n"
		"\n"
		"read writes received bytes unchanged to stdout. FILE may be - for stdin.\n",
		program, program, program, program, DEFAULT_DEVICE,
		DEFAULT_READ_SIZE);
}

static long parse_long(const char *text, long min, long max, const char *name)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || *text == '\0' || *end != '\0' || value < min || value > max) {
		fprintf(stderr, "Invalid %s: %s\n", name, text);
		exit(EXIT_FAILURE);
	}
	return value;
}

static int open_device(const char *path, int flags)
{
	int fd = open(path, flags | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return fd;
}

static void write_all_stdout(const uint8_t *data, size_t size)
{
	size_t offset = 0;

	while (offset < size) {
		ssize_t written = write(STDOUT_FILENO, data + offset, size - offset);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "Cannot write read data to stdout: %s\n",
				strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (written == 0) {
			fprintf(stderr, "Writing read data to stdout made no progress\n");
			exit(EXIT_FAILURE);
		}
		offset += (size_t)written;
	}
}

static int wait_for_event(int fd, short events, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = events,
	};
	int ret;

	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		fprintf(stderr, "poll failed: %s\n", strerror(errno));
		return -1;
	}
	if (ret == 0) {
		fprintf(stderr, "poll timed out\n");
		return 0;
	}

	fprintf(stderr, "revents:");
	if (pfd.revents & POLLIN)
		fprintf(stderr, " POLLIN");
	if (pfd.revents & POLLOUT)
		fprintf(stderr, " POLLOUT");
	if (pfd.revents & POLLERR)
		fprintf(stderr, " POLLERR");
	if (pfd.revents & POLLHUP)
		fprintf(stderr, " POLLHUP");
	if (pfd.revents & POLLNVAL)
		fprintf(stderr, " POLLNVAL");
	fprintf(stderr, "\n");
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -1;
	return (pfd.revents & events) ? 1 : 0;
}

static int command_read(const char *device, int argc, char **argv)
{
	size_t count = DEFAULT_READ_SIZE;
	int timeout_ms = -1;
	int nonblock = 0;
	uint8_t *buffer;
	ssize_t received;
	int fd;
	int i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			count = (size_t)parse_long(argv[++i], 1, SSIZE_MAX, "byte count");
		} else if (!strcmp(argv[i], "-N")) {
			nonblock = 1;
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			timeout_ms = (int)parse_long(argv[++i], -1, INT_MAX,
						     "timeout");
		} else {
			fprintf(stderr, "Unknown read option: %s\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	buffer = malloc(count);
	if (!buffer) {
		fprintf(stderr, "Cannot allocate %zu-byte read buffer\n", count);
		return EXIT_FAILURE;
	}
	fd = open_device(device, O_RDONLY |
			 (nonblock || timeout_ms >= 0 ? O_NONBLOCK : 0));
	if (timeout_ms >= 0) {
		int ready = wait_for_event(fd, POLLIN, timeout_ms);

		if (ready <= 0) {
			close(fd);
			free(buffer);
			return ready == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}
	do {
		received = read(fd, buffer, count);
	} while (received < 0 && errno == EINTR);
	if (received < 0) {
		fprintf(stderr, "read from %s failed: %s\n", device,
			strerror(errno));
		close(fd);
		free(buffer);
		return EXIT_FAILURE;
	}

	write_all_stdout(buffer, (size_t)received);
	fprintf(stderr, "Read %zd byte%s from %s\n", received,
		received == 1 ? "" : "s", device);
	close(fd);
	free(buffer);
	return EXIT_SUCCESS;
}

static uint8_t *read_input_file(const char *path, size_t *size)
{
	size_t capacity = 4096;
	size_t used = 0;
	uint8_t *buffer = malloc(capacity);
	int fd;

	if (!buffer) {
		fprintf(stderr, "Cannot allocate input buffer\n");
		return NULL;
	}
	fd = !strcmp(path, "-") ? STDIN_FILENO : open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		free(buffer);
		return NULL;
	}

	for (;;) {
		ssize_t received;

		if (used == capacity) {
			size_t new_capacity;
			uint8_t *new_buffer;

			if (capacity > (size_t)SSIZE_MAX / 2) {
				fprintf(stderr, "Input is too large for one write\n");
				goto error;
			}
			new_capacity = capacity * 2;
			new_buffer = realloc(buffer, new_capacity);
			if (!new_buffer) {
				fprintf(stderr, "Cannot allocate %zu-byte input buffer\n",
					new_capacity);
				goto error;
			}
			buffer = new_buffer;
			capacity = new_capacity;
		}

		received = read(fd, buffer + used, capacity - used);
		if (received < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "Cannot read %s: %s\n", path, strerror(errno));
			goto error;
		}
		if (received == 0)
			break;
		used += (size_t)received;
	}

	if (fd != STDIN_FILENO)
		close(fd);
	*size = used;
	return buffer;

error:
	if (fd != STDIN_FILENO)
		close(fd);
	free(buffer);
	return NULL;
}

static int publish_response(const char *device, const uint8_t *data, size_t size,
			    int hold_ms)
{
	ssize_t written;
	int fd = open_device(device, O_WRONLY);

	do {
		written = write(fd, data, size);
	} while (written < 0 && errno == EINTR);
	if (written < 0) {
		fprintf(stderr, "write to %s failed: %s\n", device,
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}
	if ((size_t)written != size) {
		fprintf(stderr, "Short write to %s: %zd of %zu bytes\n",
			device, written, size);
		close(fd);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "Published %zu byte%s to %s\n", size,
		size == 1 ? "" : "s", device);
	if (hold_ms != 0) {
		int ret;

		if (hold_ms < 0)
			fprintf(stderr, "Response armed; press Ctrl-C after the master reads it.\n");
		else
			fprintf(stderr, "Keeping response armed for %d ms.\n", hold_ms);
		do {
			ret = poll(NULL, 0, hold_ms);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0) {
			fprintf(stderr, "Response hold failed: %s\n", strerror(errno));
			close(fd);
			return EXIT_FAILURE;
		}
	}
	close(fd);
	return EXIT_SUCCESS;
}

static int parse_write_args(int argc, char **argv, int *hold_ms,
			    const char **payload)
{
	int index = 0;

	*hold_ms = -1;
	if (index < argc && !strcmp(argv[index], "-t")) {
		if (index + 1 >= argc)
			return -1;
		*hold_ms = (int)parse_long(argv[index + 1], -1, INT_MAX,
					   "hold time");
		index += 2;
	}
	if (index + 1 != argc)
		return -1;
	*payload = argv[index];
	return 0;
}

static int command_poll(const char *device, int argc, char **argv)
{
	short events = POLLIN | POLLOUT;
	int timeout_ms = -1;
	int flags = O_RDWR;
	int fd;
	int i;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-e") && i + 1 < argc) {
			const char *name = argv[++i];

			if (!strcmp(name, "read")) {
				events = POLLIN;
				flags = O_RDONLY;
			} else if (!strcmp(name, "write")) {
				events = POLLOUT;
				flags = O_WRONLY;
			} else if (!strcmp(name, "both")) {
				events = POLLIN | POLLOUT;
				flags = O_RDWR;
			} else {
				fprintf(stderr, "Unknown poll event set: %s\n", name);
				return EXIT_FAILURE;
			}
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			timeout_ms = (int)parse_long(argv[++i], -1, INT_MAX,
						     "timeout");
		} else {
			fprintf(stderr, "Unknown poll option: %s\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	fd = open_device(device, flags);
	i = wait_for_event(fd, events, timeout_ms);
	close(fd);
	return i < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	const char *device = DEFAULT_DEVICE;
	const char *command;
	int index = 1;

	if (index < argc && !strcmp(argv[index], "-d")) {
		if (index + 1 >= argc) {
			usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
		device = argv[index + 1];
		index += 2;
	}
	if (index >= argc) {
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	command = argv[index++];
	if (!strcmp(command, "read"))
		return command_read(device, argc - index, argv + index);
	if (!strcmp(command, "poll"))
		return command_poll(device, argc - index, argv + index);
	if (!strcmp(command, "write-text")) {
		const char *text;
		int hold_ms;

		if (parse_write_args(argc - index, argv + index, &hold_ms, &text)) {
			usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
		return publish_response(device, (const uint8_t *)text, strlen(text),
					hold_ms);
	}
	if (!strcmp(command, "write-file")) {
		uint8_t *data;
		size_t size;
		const char *path;
		int hold_ms;
		int ret;

		if (parse_write_args(argc - index, argv + index, &hold_ms, &path)) {
			usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
		data = read_input_file(path, &size);
		if (!data)
			return EXIT_FAILURE;
		ret = publish_response(device, data, size, hold_ms);
		free(data);
		return ret;
	}

	fprintf(stderr, "Unknown command: %s\n", command);
	usage(stderr, argv[0]);
	return EXIT_FAILURE;
}

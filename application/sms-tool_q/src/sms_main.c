/*
 * 2017 - 2024 Cezary Jackiewicz <cezary@eko.one.pl>
 * 2014 lovewilliam <ztong@vt.edu>
 * sms tool for various of 3G/4G/5G modem
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <termios.h>

#include "pdu_lib/pdu.h"

static void usage()
{
	fprintf(stderr,
		"usage: [options] send phoneNumber message\n"
		"       [options] send_raw_pdu pdu\n"
		"       [options] recv\n"
		"       [options] delete msg_index | all\n"
		"       [options] status\n"
		"       [options] ussd code\n"
		"       [options] at command\n"
		"options:\n"
		"\t-b <baudrate> (default: 115200)\n"
		"\t-c coding scheme (for ussd, 0 - 7BIT, 2 - UCS2, default: detect)\n"
		"\t-t <seconds> AT command timeout (default: 5)\n"
		"\t-d <tty device> (default: /dev/ttyUSB0)\n"
		"\t-D debug (for ussd and at)\n"
		"\t-f <date/time format> (for sms/recv)\n"
		"\t-j json output (for sms/recv)\n"
		"\t-R use raw input (for ussd)\n"
		"\t-r use raw output (for ussd and sms/recv)\n"
		"\t-s <preferred storage> (for sms/recv/status)\n"
		);
	exit(2);
}

static struct termios save_tio;
static int port = -1;
static int termios_saved = 0;
static int mhi_transport = 0;
static const char* dev = "/dev/ttyUSB0";
static const char* storage = "";
static const char* dateformat = "%D %T";
static char sms_reference[64];
static char sms_error[128];

static void setserial(int baudrate)
{
	struct termios t;
	if (mhi_transport)
		return;
	if (tcgetattr(port, &t) < 0) {
		fprintf(stderr,"tcgetattr(%s): %s\n", dev, strerror(errno));
		return;
	}

	memmove(&save_tio, &t, sizeof(t));
	termios_saved = 1;

	cfmakeraw(&t);

	t.c_cflag |=CLOCAL;
	t.c_cflag |=CREAD;

// data bits
	t.c_cflag &=~CSIZE;
	t.c_cflag |= CS8;
// parity
	t.c_cflag &= ~PARENB;
// stop bits
	t.c_cflag &=~CSTOPB;
// flow control
	t.c_cflag &=~CRTSCTS;

	t.c_oflag &=~OPOST;
	t.c_cc[VMIN]=1;

	switch (baudrate)
	{
		case 0:
			break;
		case 4800:
			cfsetspeed(&t, B4800);
			break;
		case 9600:
			cfsetspeed(&t, B9600);
			break;
		case 19200:
			cfsetspeed(&t, B19200);
			break;
		case 38400:
			cfsetspeed(&t, B38400);
			break;
		case 57600:
			cfsetspeed(&t, B57600);
			break;
		case 115200:
			cfsetspeed(&t, B115200);
			break;
		default:
			fprintf(stderr,"Unsupported baudrate: %d\n", baudrate);
	}
	if (tcsetattr(port, TCSANOW, &t) < 0)
	{
		fprintf(stderr,"tcsetattr(%s)\n", dev);
	}
}

static void resetserial()
{
	if (port < 0)
		return;
	if (!mhi_transport && termios_saved) {
		if (tcsetattr(port, TCSANOW, &save_tio) < 0)
			fprintf(stderr, "failed tcsetattr(%s): %s\n", dev, strerror(errno));
		tcflush(port, TCIOFLUSH);
	}
	close(port);
	port = -1;
}

static void timeout(int sig __attribute__((unused)))
{
	fprintf(stderr,"No response from modem.\n");
	exit(2);
}

static int starts_with(const char* prefix, const char* str)
{
	while(*prefix)
	{
		if (*prefix++ != *str++)
		{
			return 0;
		}
	}
	return 1;
}

static int char_to_hex(char c)
{
	if (isdigit(c))
		return c - '0';
	if (islower(c))
		return 10 + c - 'a';
	if (isupper(c))
		return 10 + c - 'A';
	return -1;
}

static void print_json_escape_char(char c1, char c2)
{
	if (c1 == 0x0) {
		if(c2 == '"') printf("\\\"");
		else if(c2 == '\\') printf("\\\\");
		else if(c2 == '\b') printf("\\b");
		else if(c2 == '\n') printf("\\n");
		else if(c2 == '\f') printf("\\f");
		else if(c2 == '\r') printf("\\r");
		else if(c2 == '\t') printf("\\t");
		else if(c2 == '"') printf("\\\"");
		else if(c2 == '/') printf("\\/");
		else if(c2 < ' ') printf("\\u00%02x", c2);
		else printf("%c", c2);
	} else {
		printf("\\u%02x%02x", (unsigned char)c1, (unsigned char)c2);
	}
}

static int is_mhi_device(const char *path)
{
	return !strncmp(path, "/dev/mhi_", 9) || !strncmp(path, "/dev/wwan", 9);
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_all_deadline(const unsigned char *data, size_t length, int timeout_ms)
{
	size_t written = 0;
	int64_t deadline = monotonic_ms() + timeout_ms;

	while (written < length) {
		struct pollfd pfd = { .fd = port, .events = POLLOUT };
		int64_t remaining = deadline - monotonic_ms();
		int wait_ms = remaining > INT32_MAX ? INT32_MAX : (remaining > 0 ? (int)remaining : 0);
		int polled = poll(&pfd, 1, wait_ms);
		if (polled < 0 && errno == EINTR)
			continue;
		if (polled < 0)
			return -errno;
		if (polled == 0)
			return -ETIMEDOUT;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -ENODEV;
		if (pfd.revents & POLLOUT) {
			ssize_t count = write(port, data + written, length - written);
			if (count > 0) {
				written += (size_t)count;
				continue;
			}
			if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			return count < 0 ? -errno : -EIO;
		}
	}
	return 0;
}

static void drain_input(int timeout_ms)
{
	int64_t deadline = monotonic_ms() + timeout_ms;
	unsigned char buffer[1024];

	for (;;) {
		struct pollfd pfd = { .fd = port, .events = POLLIN };
		int64_t remaining = deadline - monotonic_ms();
		int wait_ms = remaining > 100 ? 100 : (remaining > 0 ? (int)remaining : 0);
		int polled = poll(&pfd, 1, wait_ms);
		if (polled <= 0)
			return;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return;
		if (pfd.revents & POLLIN) {
			ssize_t count = read(port, buffer, sizeof(buffer));
			if (count > 0)
				continue;
			if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			return;
		}
	}
}

static int response_has_line(const char *response, const char *token)
{
	const char *line = response;
	while (*line) {
		const char *end = strchr(line, '\n');
		const char *start = line;
		const char *stop = end ? end : line + strlen(line);
		while (start < stop && (*start == '\r' || *start == ' ' || *start == '\t'))
			start++;
		while (stop > start && (stop[-1] == '\r' || stop[-1] == ' ' || stop[-1] == '\t'))
			stop--;
		if ((size_t)(stop - start) == strlen(token) && !strncmp(start, token, strlen(token)))
			return 1;
		if (!end)
			break;
		line = end + 1;
	}
	return 0;
}

static int response_has_error(const char *response)
{
	const char *line = response;
	while (*line) {
		const char *end;
		while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t')
			line++;
		end = strchr(line, '\n');
		if (!strncmp(line, "ERROR", 5) || !strncmp(line, "+CMS ERROR:", 11) ||
			!strncmp(line, "+CME ERROR:", 11))
			return 1;
		if (!end)
			break;
		line = end + 1;
	}
	return 0;
}

static void remember_modem_error(const char *response)
{
	const char *line = response;
	sms_error[0] = '\0';
	while (*line) {
		const char *end;
		const char *start;
		size_t length;
		while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t')
			line++;
		end = strchr(line, '\n');
		if (!end)
			end = line + strlen(line);
		start = line;
		if (!strncmp(start, "ERROR", 5) || !strncmp(start, "+CMS ERROR:", 11) ||
			!strncmp(start, "+CME ERROR:", 11)) {
			while (end > start && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
				end--;
			length = (size_t)(end - start);
			if (length >= sizeof(sms_error))
				length = sizeof(sms_error) - 1;
			memcpy(sms_error, start, length);
			sms_error[length] = '\0';
			return;
		}
		if (!strchr(line, '\n'))
			break;
		line = end + 1;
	}
}

static int response_has_cmgs(const char *response)
{
	const char *line = response;
	while (*line) {
		while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t')
			line++;
		if (!strncmp(line, "+CMGS:", 6))
			return 1;
		line = strchr(line, '\n');
		if (!line)
			break;
	}
	return 0;
}

static void remember_cmgs_reference(const char *response)
{
	const char *line = response;
	sms_reference[0] = '\0';
	while (*line) {
		const char *end;
		const char *value;
		size_t length;
		while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t')
			line++;
		if (strncmp(line, "+CMGS:", 6)) {
			end = strchr(line, '\n');
			if (!end)
				break;
			line = end + 1;
			continue;
		}
		value = line + 6;
		while (*value == ' ' || *value == '\t')
			value++;
		end = strchr(value, '\n');
		if (!end)
			end = value + strlen(value);
		while (end > value && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
			end--;
		length = (size_t)(end - value);
		if (length >= sizeof(sms_reference))
			length = sizeof(sms_reference) - 1;
		memcpy(sms_reference, value, length);
		sms_reference[length] = '\0';
		return;
	}
}

static int read_response_until(int timeout_ms, int prompt, int cmgs, char *response, size_t response_size)
{
	size_t used = 0;
	int64_t deadline = monotonic_ms() + timeout_ms;
	unsigned char buffer[1024];

	if (!response_size)
		return -EINVAL;
	response[0] = '\0';
	for (;;) {
		struct pollfd pfd = { .fd = port, .events = POLLIN };
		int64_t remaining = deadline - monotonic_ms();
		int wait_ms = remaining > INT32_MAX ? INT32_MAX : (remaining > 0 ? (int)remaining : 0);
		int polled = poll(&pfd, 1, wait_ms);
		if (polled < 0 && errno == EINTR)
			continue;
		if (polled == 0)
			return -ETIMEDOUT;
		if (polled < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return -ENODEV;
		if (!(pfd.revents & POLLIN))
			continue;

		ssize_t count = read(port, buffer, sizeof(buffer));
		if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (count <= 0)
			return count == 0 ? -ENODEV : -errno;
		if (used >= response_size - 1)
			return -EOVERFLOW;
		if ((size_t)count >= response_size - used)
			count = (ssize_t)(response_size - used - 1);
		if (count > 0) {
			memcpy(response + used, buffer, (size_t)count);
			used += (size_t)count;
			response[used] = '\0';
		}

		if (response_has_error(response)) {
			remember_modem_error(response);
			return -EIO;
		}
		if (prompt && strchr(response, '>'))
			return 0;
		if (!prompt && response_has_line(response, "OK") && (!cmgs || response_has_cmgs(response)))
			return 0;
	}
}

static void cancel_sms_input(void)
{
	static const unsigned char escape = 0x1b;
	char response[512];
	if (write_all_deadline(&escape, 1, 500) == 0)
		read_response_until(1000, 0, 0, response, sizeof(response));
}

static int send_command_wait_ok(const char *command, int timeout_ms, char *response, size_t response_size)
{
	char line[256];
	int length = snprintf(line, sizeof(line), "%s\r\n", command);
	if (length < 0 || (size_t)length >= sizeof(line))
		return -EINVAL;
	if (write_all_deadline((const unsigned char *)line, (size_t)length, timeout_ms) < 0)
		return -EIO;
	return read_response_until(timeout_ms, 0, 0, response, response_size);
}

static int parse_hex_pdu(const char *text, unsigned char *pdu, size_t pdu_size, int *pdu_length)
{
	size_t length = strlen(text);
	if (!length || (length & 1) || length / 2 > pdu_size)
		return -EINVAL;
	for (size_t i = 0; i < length; i += 2) {
		int high = char_to_hex(text[i]);
		int low = char_to_hex(text[i + 1]);
		if (high < 0 || low < 0)
			return -EINVAL;
		pdu[i / 2] = (unsigned char)((high << 4) | low);
	}
	if (pdu[0] > length / 2 - 1)
		return -EINVAL;
	*pdu_length = (int)(length / 2);
	return 0;
}

static int send_sms_pdu(const unsigned char *pdu, int pdu_length)
{
	char response[8192];
	char command[64];
	char hex[2 * SMS_MAX_PDU_LENGTH + 1];
	int smsc_length;
	int tpdu_length;
	int length;
	sms_reference[0] = '\0';
	sms_error[0] = '\0';

	if (!pdu || pdu_length < 1 || pdu_length > SMS_MAX_PDU_LENGTH)
		return -EINVAL;
	smsc_length = pdu[0];
	tpdu_length = pdu_length - 1 - smsc_length;
	if (tpdu_length < 0)
		return -EINVAL;
	for (int i = 0; i < pdu_length; i++)
		sprintf(hex + 2 * i, "%02X", pdu[i]);
	hex[2 * pdu_length] = '\0';

	drain_input(500);
	if (send_command_wait_ok("AT", 5000, response, sizeof(response)) < 0)
		return -EIO;
	if (send_command_wait_ok("AT+CMGF=0", 5000, response, sizeof(response)) < 0)
		return -EIO;

	length = snprintf(command, sizeof(command), "AT+CMGS=%d\r\n", tpdu_length);
	if (length < 0 || (size_t)length >= sizeof(command))
		return -EINVAL;
	if (write_all_deadline((const unsigned char *)command, (size_t)length, 5000) < 0)
		return -EIO;
	{
		int prompt_result = read_response_until(10000, 1, 0, response, sizeof(response));
		if (prompt_result < 0) {
			if (prompt_result == -ETIMEDOUT)
				cancel_sms_input();
			return prompt_result;
		}
	}

	if (write_all_deadline((const unsigned char *)hex, strlen(hex), 5000) < 0) {
		cancel_sms_input();
		return -EIO;
	}
	{
		static const unsigned char ctrl_z = 0x1a;
		if (write_all_deadline(&ctrl_z, 1, 5000) < 0)
			return -EIO;
	}
	if (read_response_until(60000, 0, 1, response, sizeof(response)) < 0)
		return -EIO;
	if (!response_has_cmgs(response) || !response_has_line(response, "OK"))
		return -EIO;
	remember_cmgs_reference(response);
	return 0;
}

static int send_sms_text(const char *phone, const char *text)
{
	unsigned char pdu[SMS_MAX_PDU_LENGTH];
	int pdu_length = pdu_encode("", phone, text, pdu, sizeof(pdu));
	if (pdu_length < 0)
		return -EINVAL;
	return send_sms_pdu(pdu, pdu_length);
}

int main(int argc, char* argv[])
{
	int ch;
	int baudrate = 115200;
	int rawinput = 0;
	int rawoutput = 0;
	int jsonoutput = 0;
	int debug = 0;
	int dcs = -1;
	int user_set_timeout = 5;

	while ((ch = getopt(argc, argv, "b:c:t:d:Ds:f:jRr")) != -1){
		switch (ch) {
		case 'b': baudrate = atoi(optarg); break;
		case 'c': dcs = atoi(optarg); break;
		case 't': user_set_timeout = atoi(optarg); break;
		case 'd': dev = optarg; break;
		case 'D': debug = 1; break;
		case 's': storage = optarg; break;
		case 'f': dateformat = optarg; break;
		case 'j': jsonoutput = 1; break;
		case 'R': rawinput = 1; break;
		case 'r': rawoutput = 1; break;
		default:
			usage();
		}
	}

	argv += optind; argc -= optind;

	if (argc < 1)
		usage();
	if (!strcmp("send", argv[0]))
	{
		if(argc < 3)
			usage();
	}else if (!strcmp("send_raw_pdu", argv[0]))
	{
		if(argc < 2)
			usage();
	}else if (!strcmp("delete",argv[0]))
	{
		if(argc < 2)
			usage();
	}else if (!strcmp("recv", argv[0]))
	{
	}else if (!strcmp("status", argv[0]))
	{
	}else if (!strcmp("ussd", argv[0]))
	{
	}else if (!strcmp("at", argv[0]))
	{
		if(argc < 2)
			usage();
	}else
		usage();

	signal(SIGALRM,timeout);
	/* A removed MHI channel may report EPIPE while a transfer is queued. */
	signal(SIGPIPE, SIG_IGN);

	char cmdstr[100];
	char pdustr[2*SMS_MAX_PDU_LENGTH+4];
	unsigned char pdu[SMS_MAX_PDU_LENGTH];

	// Open once. MHI UCI nodes are raw character devices with partial termios
	// compatibility; USB ports retain the normal serial setup.
	mhi_transport = is_mhi_device(dev);
	port = open(dev, O_RDWR|O_NONBLOCK|O_NOCTTY);
	if (port < 0) {
		fprintf(stderr,"open(%s): %s\n", dev, strerror(errno));
		return 2;
	}
	setserial(baudrate);
	atexit(resetserial);

	if (!strcmp("send", argv[0])) {
		int result = send_sms_text(argv[1], argv[2]);
		if (result == 0) {
			if (sms_reference[0])
				printf("sms sent sucessfully: %s\n", sms_reference);
			else
				printf("sms sent sucessfully\n");
			return 0;
		}
		if (result == -EINVAL)
			fprintf(stderr,"error encoding SMS: invalid UTF-8, unsupported character, or message too long\n");
		else if (sms_error[0])
			fprintf(stderr,"sms not sent, modem response: %s\n", sms_error);
		else
			fprintf(stderr,"sms not sent, command or modem transaction failed\n");
		return 1;
	}
	if (!strcmp("send_raw_pdu", argv[0])) {
		unsigned char raw_pdu[SMS_MAX_PDU_LENGTH];
		int raw_length;
		int result = parse_hex_pdu(argv[1], raw_pdu, sizeof(raw_pdu), &raw_length);
		if (result == 0)
			result = send_sms_pdu(raw_pdu, raw_length);
		if (result == 0) {
			if (sms_reference[0])
				printf("sms sent sucessfully: %s\n", sms_reference);
			else
				printf("sms sent sucessfully\n");
			return 0;
		}
		if (result == -EINVAL)
			fprintf(stderr,"error encoding SMS PDU: invalid hexadecimal PDU or length\n");
		else if (sms_error[0])
			fprintf(stderr,"sms not sent, modem response: %s\n", sms_error);
		else
			fprintf(stderr,"sms not sent, command or modem transaction failed\n");
		return 1;
	}

	/* The legacy stdio reader expects a blocking descriptor.  Keep the
	 * descriptor non-blocking for the raw sender so its poll deadlines also
	 * cover a full MHI transmit ring. */
	{
		int flags = fcntl(port, F_GETFL, 0);
		if (flags >= 0)
			fcntl(port, F_SETFL, flags & ~O_NONBLOCK);
	}

	FILE* pf = fdopen(port, "w");
	FILE* pfi = fdopen(port, "r");
	if (!pf || ! pfi)
		fprintf(stderr,"open port failed\n");
	if(setvbuf(pf, NULL, _IOLBF, 0))
	{
		fprintf(stderr, "failed to make serial port linebuffered\n");
	}

	char buf[1024];

	if (!strcmp("recv", argv[0]))
	{
		alarm(10);
		if (strlen(storage) > 0) {
			fputs("AT+CPMS=\"", pf);
			fputs(storage, pf);
			fputs("\"\r\n", pf);
			while(fgets(buf, sizeof(buf), pfi)) {
				if(starts_with("OK", buf))
					break;
			}
		}
		fputs("AT+CMGF=0\r\n", pf);
		while(fgets(buf, sizeof(buf), pfi)) {
			if(starts_with("OK", buf))
				break;
		}
		fputs("AT+CMGL=4\r\n", pf);
		int idx[1024];
		int count  = 0;
		if(jsonoutput == 1) {
			printf("{\"msg\":[");
		}
		while(fgets(buf, sizeof buf, pfi))
		{
			if(starts_with("OK", buf))
				break;
			if(starts_with("+CMGL:", buf))
			{
				if(sscanf(buf, "+CMGL: %d,", &idx[count]) != 1)
				{
					fprintf(stderr, "unparsable CMGL response: %s\n", buf+7);
					continue;
				}
				if(!fgets(buf, sizeof buf, pfi))
					fprintf(stderr,"reading pdu %d\n", count);

				if(jsonoutput == 1) {
					if (count > 0) {
						printf(",");
					}
					printf("{\"index\":%d,",idx[count]);
				} else {
					printf("MSG: %d\n",idx[count]);
				}

				++count;

				if(rawoutput == 1)
				{
					if(jsonoutput == 1) {
						printf("\"content\":\"%s\"", buf);
					} else {
						printf("%s\n", buf);
					}
					continue;
				}

				int l = strlen(buf);
				int i;
				for(i = 0; i < l; i+=2)
					pdu[i/2] = 16*char_to_hex(buf[i]) + char_to_hex(buf[i+1]);

				time_t sms_time;
				char phone_str[40];
				char sms_txt[161];

				int tp_dcs_type;
				int ref_number;
				int total_parts;
				int part_number;
				int skip_bytes;

				int sms_len = pdu_decode(pdu, l/2, &sms_time, phone_str, sizeof(phone_str), sms_txt, sizeof(sms_txt),&tp_dcs_type,&ref_number,&total_parts,&part_number,&skip_bytes);
				if (sms_len <= 0) {
					fprintf(stderr, "error decoding pdu %d: %s\n", count-1, buf);
					if(jsonoutput == 1) {
						printf("\"error\":\"error decoding pdu\",\"sender\":\"\",\"timestamp\":\"\",\"content\":\"\"}");
					}
					continue;
				}

				if(jsonoutput == 1) {
					printf("\"sender\":\"%s\",",phone_str);
				} else {
					printf("From: %s\n",phone_str);
				}
				char time_data_str[64];
				strftime(time_data_str, 64, dateformat, gmtime(&sms_time));
				if(jsonoutput == 1) {
					printf("\"timestamp\":\"%s\",",time_data_str);
				} else {
					printf("Date/Time: %s\n",time_data_str);
				}

				if(total_parts > 0) {
					if(jsonoutput == 1) {
						printf("\"reference\":%d,\"part\":%d,\"total\":%d,", ref_number, part_number, total_parts);
					} else {
						printf("Reference number: %d\n", ref_number);
						printf("SMS segment %d of %d\n", part_number, total_parts);
					}
				}

				if(jsonoutput == 1) {
					printf("\"content\":\"");
				}
				switch((tp_dcs_type / 4) % 4)
				{
					case 0:
					{
						// GSM 7 bit
						int i = skip_bytes;
						if(skip_bytes > 0) i = (skip_bytes*8+6)/7;
						for(; i<sms_len; i++)
						{
							if(jsonoutput == 1) {
								print_json_escape_char(0x0, sms_txt[i]);
							} else {
								printf("%c", sms_txt[i]);
							}
						}
						break;
					}
					case 2:
					{
						// UCS2
						for(int i = skip_bytes;i<sms_len;i+=2)
						{
							if(jsonoutput == 1) {
								print_json_escape_char(sms_txt[i],sms_txt[i+1]);
							} else {
								int ucs2_char = 0x000000FF&sms_txt[i+1];
								ucs2_char|=(0x0000FF00&(sms_txt[i]<<8));
								unsigned char utf8_char[5];
								int len = ucs2_to_utf8(ucs2_char,utf8_char);
								int j;
								for(j=0;j<len;j++)
								{
									printf("%c", utf8_char[j]);
								}
							}
						}
						break;
					}
					default:
						break;
				}
				if(jsonoutput == 1) {
					printf("\"}");
				} else {
					printf("\n\n");
				}
			}
		}
		if(jsonoutput == 1) {
			printf("]}\n");
		}

	}

	if (!strcmp("delete",argv[0]))
	{
		int i = atoi(argv[1]);
		int j = i;
		if(!strcmp("all",argv[1]))
		{
			i = 0;
			j = 49;
		}
		printf("delete msg from %d to %d\n",i,j);
		for(;i<=j;i++)
		{
			fprintf(pf, "AT+CMGD=%d\r\n", i);
			while(fgets(buf, sizeof buf, pfi))
			{
				if(starts_with("OK", buf))
				{
					printf("Deleted message %d\n", i);
					break;
				}
				if(starts_with("+CMS ERROR:", buf))
				{
					printf("Error deleting message %d: %s\n", i, buf+12);
					break;
				}
			}
		}
	}

	if (!strcmp("status", argv[0]))
	{
		alarm(10);
		if (strlen(storage) > 0) {
			fputs("AT+CPMS=\"", pf);
			fputs(storage, pf);
			fputs("\"\r\n", pf);
			while(fgets(buf, sizeof(buf), pfi)) {
				if(starts_with("OK", buf))
					break;
			}
		}
		fputs("AT+CPMS?\r\n", pf);
		while(fgets(buf, sizeof buf, pfi))
		{
			if(starts_with("+CPMS:", buf))
			{
				char mem1[9];
				int mem1_used, mem1_total;
				if(sscanf(buf, "+CPMS: \"%2s\",%d,%d,", mem1, &mem1_used, &mem1_total) != 3)
				{
					fprintf(stderr, "unparsable CPMS response: %s\n", buf);
					break;
				}
				printf("Storage type: %s, used: %d, total: %d\n", mem1, mem1_used, mem1_total);
				break;
			}
			if(starts_with("OK", buf))
			{
				break;
			}
		}
	}

	if (!strcmp("ussd", argv[0]))
	{
		enum sms_charset {
			SMS_CHARSET_7BIT = 0,
			SMS_CHARSET_8BIT = 1,
			SMS_CHARSET_UCS2 = 2,
		};

		if (rawinput==1)
		{
			snprintf(cmdstr, sizeof(cmdstr), "AT+CUSD=1,\"%s\",15\r\n", argv[1]);
		}
		else
		{
			int pdu_len = EncodePDUMessage(argv[1], strlen(argv[1]), pdu, SMS_MAX_PDU_LENGTH);
			if (pdu_len > 0)
			{
				if (pdu[pdu_len - 1] == 0) {pdu[pdu_len - 1] = 0x1d;}
				for (int i = 0; i < pdu_len; ++i)
					sprintf(pdustr+2*i, "%02X", pdu[i]);
				snprintf(cmdstr, sizeof(cmdstr), "AT+CUSD=1,\"%s\",15\r\n", pdustr);
			}
			else
				fprintf(stderr, "error encoding to PDU: %s\n", argv[1]);
		}
		if (debug == 1)
			printf("debug: %s\n", cmdstr);

		fputs(cmdstr, pf);
		alarm(10);
		char ussd_buf[320];
		char ussd_txt[800];
		int rc, multiline = 0, tp_dcs_type = 0;
		while(fgets(buf, sizeof buf, pfi))
		{
			if(starts_with("OK", buf))
				continue;
			if(starts_with("+CME ERROR:", buf))
			{
				fprintf(stderr, "error: %s\n", buf+12);
				break;
			}
			if(starts_with("+CUSD:", buf))
			{
				if (debug == 1)
					printf("debug: %s\n", buf);

				char tmp[8];
				rc = sscanf(buf, "+CUSD:%7[^\"]\"%[^\"]\",%d", tmp, ussd_buf, &tp_dcs_type);
				if(rc == 2)
				{
					if(rawoutput == 1)
					{
						multiline = 1;
						rc = 3;
					}
				}

				if(rc != 3)
				{
					fprintf(stderr, "unparsable CUSD response: %s\n", buf);
					break;
				}

				if(rawoutput == 1)
				{
					printf("%s", ussd_buf);
					if (multiline == 1)
						continue;
					else
					{
						printf("\n");
						break;
					}
				}

				int l = strlen(ussd_buf);
				for(int i = 0; i < l; i+=2)
					pdu[i/2] = 16*char_to_hex(ussd_buf[i]) + char_to_hex(ussd_buf[i+1]);

				int upper = (tp_dcs_type & 0xf0) >> 4;
				int lower = tp_dcs_type & 0xf;
				int coding = -1;

				if (upper == 0x3 || upper == 0x8 || (upper >= 0xA && upper <= 0xE))
					coding = -1;

				switch (upper)
				{
					case 0:
						coding = SMS_CHARSET_7BIT;
						break;
					case 1:
						if (lower == 0)
							coding = SMS_CHARSET_7BIT;
						if (lower == 1)
							coding = SMS_CHARSET_UCS2;
						break;
					case 2:
						if (lower <= 4)
							coding = SMS_CHARSET_7BIT;
						break;
					case 4:
					case 5:
					case 6:
					case 7:
						if (((tp_dcs_type & 0x0c) >> 2) < 3)
							coding = (enum sms_charset) ((tp_dcs_type & 0x0c) >> 2);
						break;
					case 9:
						if (((tp_dcs_type & 0x0c) >> 2) < 3)
							coding = (enum sms_charset) ((tp_dcs_type & 0x0c) >> 2);
						break;
					case 15:
						if (lower & 0x4 == 0)
							coding = SMS_CHARSET_7BIT;
						break;
				};

				switch(dcs)
				{
					case SMS_CHARSET_7BIT:
					{
						coding = SMS_CHARSET_7BIT;
						break;
					}
					case SMS_CHARSET_UCS2:
					{
						coding = SMS_CHARSET_UCS2;
						break;
					}
				}

				switch(coding)
				{
					case SMS_CHARSET_7BIT:
					{
						// GSM 7 bit
						l = DecodePDUMessage_GSM_7bit(pdu, l/2, ussd_txt, sizeof(ussd_txt));
						if (l > 0) {
							if (l < sizeof(ussd_txt))
								ussd_txt[l] = 0;

							printf("%s\n", ussd_txt);
						} else {
							fprintf(stderr, "error decoding pdu: %s\n", ussd_buf);
						}

						break;
					}
					case SMS_CHARSET_UCS2:
					{
						// UCS2
						// FIXME: interaction with multiline, sample PDUs needed
						int utf_pos = 0;
						for(int i = 0;i+1<l/2;i+=2)
						{
							int ucs2_char = 0x000000FF&pdu[i+1];
							ucs2_char|=(0x0000FF00&(pdu[i]<<8));
							utf_pos += ucs2_to_utf8(ucs2_char,&ussd_txt[utf_pos]);
						}

						if (utf_pos > 0) {
							if (utf_pos < sizeof(ussd_txt))
								ussd_txt[utf_pos] = 0;

							printf("%s\n", ussd_txt);
						} else {
							fprintf(stderr, "error decoding pdu: %s\n", ussd_buf);
						}

						break;
					}
					default:
						fprintf(stderr, "unknown coding scheme: %d\n", tp_dcs_type);
						break;
				}

				break;
			}
			if (multiline == 1)
			{
				rc = sscanf(buf, "%[^\"]\",%d", ussd_buf, &tp_dcs_type);
				if (rc == 1)
				{
					printf("%s", ussd_buf);
				}
				if (rc == 2)
				{
					printf("%s\n", ussd_buf);
					multiline = 0;
					break;
				}
			}
		}
	}

	if (!strcmp("at", argv[0]))
	{
		alarm(user_set_timeout > 0 ? user_set_timeout : 5);
		fputs(argv[1], pf);
		fputs("\r\n", pf);

		while(fgets(buf, sizeof(buf), pfi)) {
			if(starts_with("OK", buf)) {
				if (debug == 1)
					printf("%s", buf);
				exit(0);
			}
			if(starts_with("ERROR", buf)) {
				if (debug == 1)
					printf("%s", buf);
				exit(1);
			}
			if(starts_with("COMMAND NOT SUPPORT", buf)) {
				if (debug == 1)
					printf("%s", buf);
				exit(1);
			}
			if(starts_with("+CME ERROR", buf)) {
				if (debug == 1)
					printf("%s", buf);
				exit(1);
			}
			printf("%s", buf);
		}
	}

	exit(0);
}

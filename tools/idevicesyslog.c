/*
 * idevicesyslog.c
 * Relay the syslog of a device to stdout
 *
 * Copyright (c) 2010-2020 Nikias Bassen, All Rights Reserved.
 * Copyright (c) 2009 Martin Szulecki All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define TOOL_NAME "idevicesyslog"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define sleep(x) Sleep(x*1000)
#endif

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/syslog_relay.h>
#include <libimobiledevice-glue/termcolors.h>
#include <libimobiledevice/ostrace.h>

static int quit_flag = 0;
static int exit_on_disconnect = 0;
static int show_device_name = 0;
static int force_syslog_relay = 0;

static char* udid = NULL;
static char** proc_filters = NULL;
static int num_proc_filters = 0;
static int proc_filter_excluding = 0;

static int* pid_filters = NULL;
static int num_pid_filters = 0;

static char** msg_filters = NULL;
static int num_msg_filters = 0;

static char** msg_reverse_filters = NULL;
static int num_msg_reverse_filters = 0;

static char** trigger_filters = NULL;
static int num_trigger_filters = 0;
static char** untrigger_filters = NULL;
static int num_untrigger_filters = 0;
static int triggered = 0;

static idevice_t device = NULL;
static syslog_relay_client_t syslog = NULL;
static ostrace_client_t ostrace = NULL;

static const char QUIET_FILTER[] = "CircleJoinRequested|CommCenter|HeuristicInterpreter|MobileMail|PowerUIAgent|ProtectedCloudKeySyncing|SpringBoard|UserEventAgent|WirelessRadioManagerd|accessoryd|accountsd|aggregated|analyticsd|appstored|apsd|assetsd|assistant_service|backboardd|biometrickitd|bluetoothd|calaccessd|callservicesd|cloudd|com.apple.Safari.SafeBrowsing.Service|contextstored|corecaptured|coreduetd|corespeechd|cdpd|dasd|dataaccessd|distnoted|dprivacyd|duetexpertd|findmydeviced|fmfd|fmflocatord|gpsd|healthd|homed|identityservicesd|imagent|itunescloudd|itunesstored|kernel|locationd|maild|mDNSResponder|mediaremoted|mediaserverd|mobileassetd|nanoregistryd|nanotimekitcompaniond|navd|nsurlsessiond|passd|pasted|photoanalysisd|powerd|powerlogHelperd|ptpd|rapportd|remindd|routined|runningboardd|searchd|sharingd|suggestd|symptomsd|timed|thermalmonitord|useractivityd|vmd|wifid|wirelessproxd";

static int use_network = 0;

static long long start_time = -1;
static long long size_limit = -1;
static long long age_limit = -1;

static char *line = NULL;
static int line_buffer_size = 0;
static int lp = 0;

static void add_filter(const char* filterstr)
{
	int filter_len = strlen(filterstr);
	const char* start = filterstr;
	const char* end = filterstr + filter_len;
	const char* p = start;
	while (p <= end) {
		if ((*p == '|') || (*p == '\0')) {
			if (p-start > 0) {
				char* procn = malloc(p-start+1);
				if (!procn) {
					fprintf(stderr, "ERROR: malloc() failed\n");
					exit(EXIT_FAILURE);
				}
				memcpy(procn, start, p-start);
				procn[p-start] = '\0';
				char* endp = NULL;
				int pid_value = (int)strtol(procn, &endp, 10);
				if (!endp || *endp == 0) {
					int *new_pid_filters = realloc(pid_filters, sizeof(int) * (num_pid_filters+1));
					if (!new_pid_filters) {
						fprintf(stderr, "ERROR: realloc() failed\n");
						exit(EXIT_FAILURE);
					}
					pid_filters = new_pid_filters;
					pid_filters[num_pid_filters] = pid_value;
					num_pid_filters++;
				} else {
					char **new_proc_filters = realloc(proc_filters, sizeof(char*) * (num_proc_filters+1));
					if (!new_proc_filters) {
						fprintf(stderr, "ERROR: realloc() failed\n");
						exit(EXIT_FAILURE);
					}
					proc_filters = new_proc_filters;
					proc_filters[num_proc_filters] = procn;
					num_proc_filters++;
				}
			}
			start = p+1;
		}
		p++;
	}
}

static int find_char(char c, char** p, const char* end)
{
	while ((**p != c) && (*p < end)) {
		(*p)++;
	}
	return (**p == c);
}

static void stop_logging(void);

static int message_filter_matching(const char* message)
{
	if (num_msg_filters > 0) {
		int found = 0;
		int i;
		for (i = 0; i < num_msg_filters; i++) {
			if (strstr(message, msg_filters[i])) {
				found = 1;
				break;
			}
		}
		if (!found) {
			return 0;
		}
	}
	if (num_msg_reverse_filters > 0) {
		int found = 0;
		int i;
		for (i = 0; i < num_msg_reverse_filters; i++) {
			if (strstr(message, msg_reverse_filters[i])) {
				found = 1;
				break;
			}
		}
		if (found) {
			return 0;
		}
	}
	return 1;
}

static int process_filter_matching(int pid, const char* process_name, int process_name_length)
{
	int proc_matched = 0;
	if (num_pid_filters > 0) {
		int found = proc_filter_excluding;
		int i = 0;
		for (i = 0; i < num_pid_filters; i++) {
			if (pid == pid_filters[i]) {
				found = !proc_filter_excluding;
				break;
			}
		}
		if (found) {
			proc_matched = 1;
		}
	}
	if (num_proc_filters > 0 && !proc_matched) {
		int found = proc_filter_excluding;
		int i = 0;
		for (i = 0; i < num_proc_filters; i++) {
			if (!proc_filters[i]) continue;
			if (strncmp(proc_filters[i], process_name, process_name_length) == 0) {
				found = !proc_filter_excluding;
				break;
			}
		}
		if (found) {
			proc_matched = 1;
		}
	}
	return proc_matched;
}

static void syslog_callback(char c, void *user_data)
{
	if (lp >= line_buffer_size-1) {
		line_buffer_size+=1024;
		char* _line = realloc(line, line_buffer_size);
		if (!_line) {
			fprintf(stderr, "ERROR: realloc failed\n");
			exit(EXIT_FAILURE);
		}
		line = _line;
	}
	line[lp++] = c;
	if (c == '\0') {
		int shall_print = 0;
		int trigger_off = 0;
		lp--;
		char* linep = &line[0];
		do {
			if (lp < 16) {
				shall_print = 1;
				cprintf(FG_WHITE);
				break;
			}

			if (line[3] == ' ' && line[6] == ' ' && line[15] == ' ') {
				char* end = &line[lp];
				char* p = &line[16];

				/* device name */
				char* device_name_start = p;
				char* device_name_end = p;
				if (!find_char(' ', &p, end)) break;
				device_name_end = p;
				p++;

				/* check if we have any triggers/untriggers */
				if (num_untrigger_filters > 0 && triggered) {
					int found = 0;
					int i;
					for (i = 0; i < num_untrigger_filters; i++) {
						if (strstr(device_name_end+1, untrigger_filters[i])) {
							found = 1;
							break;
						}
					}
					if (!found) {
						shall_print = 1;
					} else {
						shall_print = 1;
						trigger_off = 1;
					}
				} else if (num_trigger_filters > 0 && !triggered) {
					int found = 0;
					int i;
					for (i = 0; i < num_trigger_filters; i++) {
						if (strstr(device_name_end+1, trigger_filters[i])) {
							found = 1;
							break;
						}
					}
					if (!found) {
						shall_print = 0;
						break;
					}
					triggered = 1;
					shall_print = 1;
				} else if (num_trigger_filters == 0 && num_untrigger_filters > 0 && !triggered) {
					shall_print = 0;
					quit_flag++;
					break;
				}

				/* check message filters */
				shall_print = message_filter_matching(device_name_end+1);
				if (!shall_print) {
					break;
				}

				/* process name */
				char* proc_name_start = p;
				char* proc_name_end = p;
				if (!find_char('[', &p, end)) break;
				char* process_name_start = proc_name_start;
				char* process_name_end = p;
				char* pid_start = p+1;
				char* pp = process_name_start;
				if (find_char('(', &pp, p)) {
					process_name_end = pp;
				}
				if (!find_char(']', &p, end)) break;
				p++;
				if (*p != ' ') break;
				proc_name_end = p;
				p++;

				/* match pid / process name */
				char* endp = NULL;
				int pid_value = (int)strtol(pid_start, &endp, 10);
				if (process_filter_matching(pid_value, process_name_start, process_name_end-process_name_start)) {
					shall_print = 1;
				} else {
					if (num_pid_filters > 0 || num_proc_filters > 0) {
						shall_print = 0;
						break;
					}
				}

				/* log level */
				char* level_start = p;
				char* level_end = p;
				const char* level_color = NULL;
				if (!strncmp(p, "<Notice>:", 9)) {
					level_end += 9;
					level_color = FG_GREEN;
				} else if (!strncmp(p, "<Error>:", 8)) {
					level_end += 8;
					level_color = FG_RED;
				} else if (!strncmp(p, "<Warning>:", 10)) {
					level_end += 10;
					level_color = FG_YELLOW;
				} else if (!strncmp(p, "<Debug>:", 8)) {
					level_end += 8;
					level_color = FG_MAGENTA;
				} else {
					level_color = FG_WHITE;
				}

				/* write date and time */
				cprintf(FG_LIGHT_GRAY);
				fwrite(line, 1, 16, stdout);

				if (show_device_name) {
					/* write device name */
					cprintf(FG_DARK_YELLOW);
					fwrite(device_name_start, 1, device_name_end-device_name_start+1, stdout);
					cprintf(COLOR_RESET);
				}

				/* write process name */
				cprintf(FG_BRIGHT_CYAN);
				fwrite(process_name_start, 1, process_name_end-process_name_start, stdout);
				cprintf(FG_CYAN);
				fwrite(process_name_end, 1, proc_name_end-process_name_end+1, stdout);

				/* write log level */
				cprintf(level_color);
				if (level_end > level_start) {
					fwrite(level_start, 1, level_end-level_start, stdout);
					p = level_end;
				}

				lp -= p - linep;
				linep = p;

				cprintf(FG_WHITE);

			} else {
				shall_print = 1;
				cprintf(FG_WHITE);
			}
		} while (0);

		if ((num_msg_filters == 0 && num_msg_reverse_filters == 0 && num_proc_filters == 0 && num_pid_filters == 0 && num_trigger_filters == 0 && num_untrigger_filters == 0) || shall_print) {
			fwrite(linep, 1, lp, stdout);
			cprintf(COLOR_RESET);
			fflush(stdout);
			if (trigger_off) {
				triggered = 0;
			}
		}
		line[0] = '\0';
		lp = 0;
		return;
	}
}

static void ostrace_syslog_callback(const void* buf, size_t len, void* user_data)
{
	if (len < 0x81) {
		fprintf(stderr, "Error: not enough data in callback function?!\n");
		return;
	}

	struct ostrace_packet_header_t *trace_hdr = (struct ostrace_packet_header_t*)buf;

	if (trace_hdr->marker != 2 || (trace_hdr->type != 8 && trace_hdr->type != 2)) {
		fprintf(stderr, "unexpected packet data %02x %08x\n", trace_hdr->marker, trace_hdr->type);
	}

	const char* dataptr = (const char*)buf + trace_hdr->header_size;
	const char* process_name = dataptr;
	const char* image_name = (trace_hdr->imagepath_len > 0) ? dataptr + trace_hdr->procpath_len : NULL;
	const char* message = (trace_hdr->message_len > 0) ? dataptr + trace_hdr->procpath_len + trace_hdr->imagepath_len : NULL;
	//const char* subsystem = (trace_hdr->subsystem_len > 0) ? dataptr + trace_hdr->procpath_len + trace_hdr->imagepath_len + trace_hdr->message_len : NULL;
	//const char* category = (trace_hdr->category_len > 0) ? dataptr + trace_hdr->procpath_len + trace_hdr->imagepath_len + trace_hdr->message_len + trace_hdr->subsystem_len : NULL;

	int shall_print = 1;
	int trigger_off = 0;
	const char* process_name_short = (process_name) ? strrchr(process_name, '/') : "";
	process_name_short = (process_name_short) ? process_name_short+1 : process_name;
	const char* image_name_short = (image_name) ? strrchr(image_name, '/') : NULL;
	image_name_short = (image_name_short) ? image_name_short+1 : process_name;
	if (image_name_short && !strcmp(image_name_short, process_name_short)) {
		image_name_short = NULL;
	}

	do {
		/* check if we have any triggers/untriggers */
		if (num_untrigger_filters > 0 && triggered) {
			int found = 0;
			int i;
			for (i = 0; i < num_untrigger_filters; i++) {
				if (strstr(message, untrigger_filters[i])) {
					found = 1;
					break;
				}
			}
			if (!found) {
				shall_print = 1;
			} else {
				shall_print = 1;
				trigger_off = 1;
			}
		} else if (num_trigger_filters > 0 && !triggered) {
			int found = 0;
			int i;
			for (i = 0; i < num_trigger_filters; i++) {
				if (strstr(message, trigger_filters[i])) {
					found = 1;
					break;
				}
			}
			if (!found) {
				shall_print = 0;
				break;
			}
			triggered = 1;
			shall_print = 1;
		} else if (num_trigger_filters == 0 && num_untrigger_filters > 0 && !triggered) {
			shall_print = 0;
			quit_flag++;
			break;
		}
	
		/* check message filters */
		shall_print = message_filter_matching(message);
		if (!shall_print) {
			break;
		}

		/* check process filters */
		if (process_filter_matching(trace_hdr->pid, process_name_short, strlen(process_name_short))) {
			shall_print = 1;
		} else {
			if (num_pid_filters > 0 || num_proc_filters > 0) {
				shall_print = 0;
			}
		}
		if (!shall_print) {
			break;
		}
	} while (0);

	if (!shall_print) {
		return;
	}

	const char* level_str = "Unknown";
	const char* level_color = FG_YELLOW;
	switch (trace_hdr->level) {
		case 0:
			level_str = "Notice";
			level_color = FG_GREEN;
			break;
		case 0x01:
			level_str = "Info";
			level_color = FG_WHITE;
			break;
		case 0x02:
			level_str = "Debug";
			level_color = FG_MAGENTA;
			break;
		case 0x10:
			level_str = "Error";
			level_color = FG_RED;
			break;
		case 0x11:
			level_str = "Fault";
			level_color = FG_RED;
		default:
			break;
	}

	char datebuf[24];
	struct tm *tp;
	time_t time_sec = (time_t)trace_hdr->time_sec;
#ifdef HAVE_LOCALTIME_R
	struct tm tp_ = {0, };
	tp = localtime_r(&time_sec, &tp_);
#else
	tp = localtime(&time_sec);
#endif
#ifdef _WIN32
	strftime(datebuf, 16, "%b %#d %H:%M:%S", tp);
#else
	strftime(datebuf, 16, "%b %e %H:%M:%S", tp);
#endif
	snprintf(datebuf+15, 9, ".%06u", trace_hdr->time_usec);

	/* write date and time */
	cprintf(FG_LIGHT_GRAY "%s ", datebuf);

	if (show_device_name) {
		/* write device name TODO do we need this? */
		//cprintf(FG_DARK_YELLOW "%s ", device_name);
	}

	/* write process name */
	cprintf(FG_BRIGHT_CYAN "%s" FG_CYAN, process_name_short);
	if (image_name_short) {
		cprintf("(%s)", image_name_short);
	}
	cprintf("[%d]" COLOR_RESET " ", trace_hdr->pid);

	/* write log level */
	cprintf(level_color);
	cprintf("<%s>:" COLOR_RESET " ", level_str);

	/* write message */
	cprintf(FG_WHITE);
	cprintf("%s" COLOR_RESET "\n", message);
	fflush(stdout);	

	if (trigger_off) {
		triggered = 0;
	}
}

static plist_t get_pid_list()
{
	plist_t list = NULL;
	ostrace_client_t ostrace_tmp = NULL;
	ostrace_client_start_service(device, &ostrace_tmp, TOOL_NAME);
	if (ostrace_tmp) {
		ostrace_get_pid_list(ostrace_tmp, &list);
		ostrace_client_free(ostrace_tmp);
	}
	return list;
}

static int pid_valid(int pid)
{
	plist_t list = get_pid_list();
	if (!list) return 0;
	char valbuf[16];
	snprintf(valbuf, 16, "%d", pid);
	return (plist_dict_get_item(list, valbuf)) ? 1 : 0;
}

static int pid_for_proc(const char* procname)
{
	int result = -1;
	plist_t list = get_pid_list();
	if (!list) {
		return result;
	}
	plist_dict_iter iter = NULL;
	plist_dict_new_iter(list, &iter);
	if (iter) {
		plist_t node = NULL;
		do {
			char* key = NULL;
			node = NULL;
			plist_dict_next_item(list, iter, &key, &node);
			if (!key) {
				break;
			}
			if (PLIST_IS_DICT(node)) {
				plist_t pname = plist_dict_get_item(node, "ProcessName");
				if (PLIST_IS_STRING(pname)) {
					if (!strcmp(plist_get_string_ptr(pname, NULL), procname)) {
						result = (int)strtol(key, NULL, 10);
					}
				}
			}
			free(key);
		} while (node);
		plist_mem_free(iter);
	}
	plist_free(list);
	return result;
}

static int connect_service(int ostrace_required)
{
	if (!device) {
		idevice_error_t ret = idevice_new_with_options(&device, udid, (use_network) ? IDEVICE_LOOKUP_NETWORK : IDEVICE_LOOKUP_USBMUX);
		if (ret != IDEVICE_E_SUCCESS) {
			fprintf(stderr, "Device with udid %s not found!?\n", udid);
			return -1;
		}
	}

	lockdownd_client_t lockdown = NULL;
	lockdownd_error_t lerr = lockdownd_client_new_with_handshake(device, &lockdown, TOOL_NAME);
	if (lerr != LOCKDOWN_E_SUCCESS) {
		fprintf(stderr, "ERROR: Could not connect to lockdownd: %d\n", lerr);
		idevice_free(device);
		device = NULL;
		return -1;
	}
	lockdownd_service_descriptor_t svc = NULL;

	const char* service_name = OSTRACE_SERVICE_NAME;
	int use_ostrace = 1;
	if (idevice_get_device_version(device) < IDEVICE_DEVICE_VERSION(9,0,0) || force_syslog_relay) {
		service_name = SYSLOG_RELAY_SERVICE_NAME;
		use_ostrace = 0;
	}
	if (ostrace_required && !use_ostrace) {
		fprintf(stderr, "ERROR: This operation requires iOS 9 or later.\n");
		lockdownd_client_free(lockdown);
		idevice_free(device);
		device = NULL;
		return -1;
	}

	/* start syslog_relay/os_trace_relay service */
	lerr = lockdownd_start_service(lockdown, service_name, &svc);
	if (lerr == LOCKDOWN_E_PASSWORD_PROTECTED) {
		fprintf(stderr, "*** Device is passcode protected, enter passcode on the device to continue ***\n");
		while (!quit_flag) {
			lerr = lockdownd_start_service(lockdown, service_name, &svc);
			if (lerr != LOCKDOWN_E_PASSWORD_PROTECTED) {
				break;
			}
			sleep(1);
		}
	}
	if (lerr != LOCKDOWN_E_SUCCESS) {
		fprintf(stderr, "ERROR: Could not start %s service: %s (%d)\n", service_name, lockdownd_strerror(lerr), lerr);
		idevice_free(device);
		device = NULL;
		return -1;
	}
	lockdownd_client_free(lockdown);

	if (use_ostrace) {
		/* connect to os_trace_relay service */
		ostrace_error_t serr = OSTRACE_E_UNKNOWN_ERROR;
		serr = ostrace_client_new(device, svc, &ostrace);
		lockdownd_service_descriptor_free(svc);
		if (serr != OSTRACE_E_SUCCESS) {
			fprintf(stderr, "ERROR: Could not connect to %s service (%d)\n", service_name, serr);
			idevice_free(device);
			device = NULL;
			return -1;
		}
	} else {
		/* connect to syslog_relay service */
		syslog_relay_error_t serr = SYSLOG_RELAY_E_UNKNOWN_ERROR;
		serr = syslog_relay_client_new(device, svc, &syslog);
		lockdownd_service_descriptor_free(svc);
		if (serr != SYSLOG_RELAY_E_SUCCESS) {
			fprintf(stderr, "ERROR: Could not connect to %s service (%d)\n", service_name, serr);
			idevice_free(device);
			device = NULL;
			return -1;
		}
	}
	return 0;
}

static int start_logging(void)
{
	if (connect_service(0) < 0) {
		return -1;
	}

	/* start capturing syslog */
	if (ostrace) {
		plist_t options = plist_new_dict();
		if (num_proc_filters == 0 && num_pid_filters == 1 && !proc_filter_excluding) {
			if (pid_filters[0] > 0) {
				if (!pid_valid(pid_filters[0])) {
					fprintf(stderr, "NOTE: A process with pid doesn't exists!\n");
				}
			}
			plist_dict_set_item(options, "Pid", plist_new_int(pid_filters[0]));
		} else if (num_proc_filters == 1 && num_pid_filters == 0 && !proc_filter_excluding) {
			int pid = pid_for_proc(proc_filters[0]);
			if (!strcmp(proc_filters[0], "kernel")) {
				pid = 0;
			}
			if (pid >= 0) {
				plist_dict_set_item(options, "Pid", plist_new_int(pid));
			}
		}
		ostrace_error_t serr = ostrace_start_activity(ostrace, options, ostrace_syslog_callback, NULL);
		if (serr != OSTRACE_E_SUCCESS) {
			fprintf(stderr, "ERROR: Unable to start capturing syslog.\n");
			ostrace_client_free(ostrace);
			ostrace = NULL;
			idevice_free(device);
			device = NULL;
			return -1;
		}
	} else if (syslog) {
		syslog_relay_error_t serr = syslog_relay_start_capture_raw(syslog, syslog_callback, NULL);
		if (serr != SYSLOG_RELAY_E_SUCCESS) {
			fprintf(stderr, "ERROR: Unable to start capturing syslog.\n");
			syslog_relay_client_free(syslog);
			syslog = NULL;
			idevice_free(device);
			device = NULL;
			return -1;
		}
	} else {
		return -1;
	}

	fprintf(stdout, "[connected:%s]\n", udid);
	fflush(stdout);

	return 0;
}

static void stop_logging(void)
{
	fflush(stdout);

	if (syslog) {
		syslog_relay_client_free(syslog);
		syslog = NULL;
	}
	if (ostrace) {
		ostrace_stop_activity(ostrace);
		ostrace_client_free(ostrace);
		ostrace = NULL;
	}

	if (device) {
		idevice_free(device);
		device = NULL;
	}
}

static int write_callback(const void* buf, size_t len, void *user_data)
{
	FILE* f = (FILE*)user_data;
	ssize_t res = fwrite(buf, 1, len, f);
	if (res < 0) {
		return -1;
	}
	if (quit_flag > 0) {
		return -1;
	}
	return 0;
}

static void print_sorted_pidlist(plist_t list)
{
	struct listelem;
	struct listelem {
		int val;
		struct listelem *next;
	};
	struct listelem* sortedlist = NULL;

	plist_dict_iter iter = NULL;
	plist_dict_new_iter(list, &iter);
	if (iter) {
		plist_t node = NULL;
		do {
			char* key = NULL;
			node = NULL;
			plist_dict_next_item(list, iter, &key, &node);
			if (key) {
				int pidval = (int)strtol(key, NULL, 10);
				struct listelem* elem = (struct listelem*)malloc(sizeof(struct listelem));
				elem->val = pidval;
				elem->next = NULL;
				struct listelem* prev = NULL;
				struct listelem* curr = sortedlist;

				while (curr && pidval > curr->val) {
					prev = curr;
					curr = curr->next;
				}

				elem->next = curr;
				if (prev == NULL) {
					sortedlist = elem;
				} else {
					prev->next = elem;
				}
				free(key);
			}
		} while (node);
		plist_mem_free(iter);
	}
	struct listelem *listp = sortedlist;
	char pidstr[16];
	while (listp) {
		snprintf(pidstr, 16, "%d", listp->val);
		plist_t node = plist_dict_get_item(list, pidstr);
		if (PLIST_IS_DICT(node)) {
			plist_t pname = plist_dict_get_item(node, "ProcessName");
			if (PLIST_IS_STRING(pname)) {
				printf("%d %s\n", listp->val, plist_get_string_ptr(pname, NULL));
			}
		}
		struct listelem *curr = listp;
		listp = listp->next;
		free(curr);
	}
}

static void device_event_cb(const idevice_event_t* event, void* userdata)
{
	if (use_network && event->conn_type != CONNECTION_NETWORK) {
		return;
	}
	if (!use_network && event->conn_type != CONNECTION_USBMUXD) {
		return;
	}
	if (event->event == IDEVICE_DEVICE_ADD) {
		if (!syslog && !ostrace) {
			if (!udid) {
				udid = strdup(event->udid);
			}
			if (strcmp(udid, event->udid) == 0) {
				if (start_logging() != 0) {
					fprintf(stderr, "Could not start logger for udid %s\n", udid);
				}
			}
		}
	} else if (event->event == IDEVICE_DEVICE_REMOVE) {
		if ((syslog || ostrace) && (strcmp(udid, event->udid) == 0)) {
			stop_logging();
			fprintf(stdout, "[disconnected:%s]\n", udid);
			if (exit_on_disconnect) {
				quit_flag++;
			}
		}
	}
}

/**
 * signal handler function for cleaning up properly
 */
static void clean_exit(int sig)
{
	fprintf(stderr, "\nExiting...\n");
	quit_flag++;
}

static void print_usage(int argc, char **argv, int is_error)
{
	char *name = strrchr(argv[0], '/');
	fprintf(is_error ? stderr : stdout, "Usage: %s [OPTIONS]\n", (name ? name + 1: argv[0]));
	fprintf(is_error ? stderr : stdout,
		"\n"
		"Relay syslog of a connected device.\n"
		"\n"
		"OPTIONS:\n"
		"  -u, --udid UDID       target specific device by UDID\n"
		"  -n, --network         connect to network device\n"
		"  -x, --exit            exit when device disconnects\n"
		"  -h, --help            prints usage information\n"
		"  -d, --debug           enable communication debugging\n"
		"  -v, --version         prints version information\n"
		"  --no-colors           disable colored output\n"
		"  -o, --output FILE     write to FILE instead of stdout\n"
		"                        (existing FILE will be overwritten)\n"
		"  --colors              force writing colored output, e.g. for --output\n"
		"  --syslog_relay        force use of syslog_relay service\n"
		"\n"
		"COMMANDS:\n"
		"  pidlist               Print pid and name of all running processes.\n"
		"  archive PATH          Request a logarchive and write it to PATH.\n"
		"                        Output can be piped to another process using - as PATH.\n"
		"                        The file data will be in .tar format.\n"
		"    --start-time VALUE  start time of the log data as UNIX timestamp\n"
		"    --age-limit VALUE   maximum age of the log data\n"
		"    --size-limit VALUE  limit the size of the archive\n"
		"\n"
		"FILTER OPTIONS:\n"
		"  -m, --match STRING      only print messages that contain STRING\n"
		"  -M, --unmatch STRING    print messages that not contain STRING\n"
		"  -t, --trigger STRING    start logging when matching STRING\n"
		"  -T, --untrigger STRING  stop logging when matching STRING\n"
		"  -p, --process PROCESS   only print messages from matching process(es)\n"
		"  -e, --exclude PROCESS   print all messages except matching process(es)\n"
		"                          PROCESS is a process name or multiple process names\n"
		"                          separated by \"|\".\n"
		"  -q, --quiet             set a filter to exclude common noisy processes\n"
		"  --quiet-list            prints the list of processes for --quiet and exits\n"
		"  -k, --kernel            only print kernel messages\n"
		"  -K, --no-kernel         suppress kernel messages\n"
		"\n"
		"For filter examples consult idevicesyslog(1) man page.\n"
		"\n"
		"Homepage:    <" PACKAGE_URL ">\n"
		"Bug Reports: <" PACKAGE_BUGREPORT ">\n"
	);
}

int main(int argc, char *argv[])
{
	int include_filter = 0;
	int exclude_filter = 0;
	int include_kernel = 0;
	int exclude_kernel = 0;
	int force_colors = 0;
	int c = 0;
	const struct option longopts[] = {
		{ "debug", no_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ "udid", required_argument, NULL, 'u' },
		{ "network", no_argument, NULL, 'n' },
		{ "exit", no_argument, NULL, 'x' },
		{ "trigger", required_argument, NULL, 't' },
		{ "untrigger", required_argument, NULL, 'T' },
		{ "match", required_argument, NULL, 'm' },
		{ "process", required_argument, NULL, 'p' },
		{ "exclude", required_argument, NULL, 'e' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "kernel", no_argument, NULL, 'k' },
		{ "no-kernel", no_argument, NULL, 'K' },
		{ "quiet-list", no_argument, NULL, 1 },
		{ "no-colors", no_argument, NULL, 2 },
		{ "colors", no_argument, NULL, 3 },
		{ "syslog_relay", no_argument, NULL, 4 },
		{ "syslog-relay", no_argument, NULL, 4 },
		{ "legacy", no_argument, NULL, 4 },
		{ "start-time", required_argument, NULL, 5 },
		{ "size-limit", required_argument, NULL, 6 },
		{ "age-limit", required_argument, NULL, 7 },
		{ "output", required_argument, NULL, 'o' },
		{ "version", no_argument, NULL, 'v' },
		{ NULL, 0, NULL, 0}
	};

	signal(SIGINT, clean_exit);
	signal(SIGTERM, clean_exit);
#ifndef _WIN32
	signal(SIGQUIT, clean_exit);
	signal(SIGPIPE, SIG_IGN);
#endif

	while ((c = getopt_long(argc, argv, "dhu:nxt:T:m:M:e:p:qkKo:v", longopts, NULL)) != -1) {
		switch (c) {
		case 'd':
			idevice_set_debug_level(1);
			break;
		case 'u':
			if (!*optarg) {
				fprintf(stderr, "ERROR: UDID must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			}
			free(udid);
			udid = strdup(optarg);
			break;
		case 'n':
			use_network = 1;
			break;
		case 'q':
			exclude_filter++;
			add_filter(QUIET_FILTER);
			break;
		case 'p':
		case 'e':
			if (c == 'p') {
				include_filter++;
			} else if (c == 'e') {
				exclude_filter++;
			}
			if (!*optarg) {
				fprintf(stderr, "ERROR: filter string must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			}
			add_filter(optarg);
			break;
		case 'm':
			if (!*optarg) {
				fprintf(stderr, "ERROR: message filter string must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			} else {
				char **new_msg_filters = realloc(msg_filters, sizeof(char*) * (num_msg_filters+1));
				if (!new_msg_filters) {
					fprintf(stderr, "ERROR: realloc() failed\n");
					exit(EXIT_FAILURE);
				}
				msg_filters = new_msg_filters;
				msg_filters[num_msg_filters] = strdup(optarg);
				num_msg_filters++;
			}
			break;
		case 'M':
			if (!*optarg) {
				fprintf(stderr, "ERROR: reverse message filter string must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			} else {
				char **new_msg_filters = realloc(msg_reverse_filters, sizeof(char*) * (num_msg_reverse_filters+1));
				if (!new_msg_filters) {
					fprintf(stderr, "ERROR: realloc() failed\n");
					exit(EXIT_FAILURE);
				}
				msg_reverse_filters = new_msg_filters;
				msg_reverse_filters[num_msg_reverse_filters] = strdup(optarg);
				num_msg_reverse_filters++;
			}
			break;
		case 't':
			if (!*optarg) {
				fprintf(stderr, "ERROR: trigger filter string must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			} else {
				char **new_trigger_filters = realloc(trigger_filters, sizeof(char*) * (num_trigger_filters+1));
				if (!new_trigger_filters) {
					fprintf(stderr, "ERROR: realloc() failed\n");
					exit(EXIT_FAILURE);
				}
				trigger_filters = new_trigger_filters;
				trigger_filters[num_trigger_filters] = strdup(optarg);
				num_trigger_filters++;
			}
			break;
		case 'T':
			if (!*optarg) {
				fprintf(stderr, "ERROR: untrigger filter string must not be empty!\n");
				print_usage(argc, argv, 1);
				return 2;
			} else {
				char **new_untrigger_filters = realloc(untrigger_filters, sizeof(char*) * (num_untrigger_filters+1));
				if (!new_untrigger_filters) {
					fprintf(stderr, "ERROR: realloc() failed\n");
					exit(EXIT_FAILURE);
				}
				untrigger_filters = new_untrigger_filters;
				untrigger_filters[num_untrigger_filters] = strdup(optarg);
				num_untrigger_filters++;
			}
			break;
		case 'k':
			include_kernel++;
			break;
		case 'K':
			exclude_kernel++;
			break;
		case 'x':
			exit_on_disconnect = 1;
			break;
		case 'h':
			print_usage(argc, argv, 0);
			return 0;
		case 1:	{
			printf("%s\n", QUIET_FILTER);
			return 0;
		}
		case 2:
			term_colors_set_enabled(0);
			break;
		case 3:
			force_colors = 1;
			break;
		case 4:
			force_syslog_relay = 1;
			break;
		case 5:
			start_time = strtoll(optarg, NULL, 10);
			break;
		case 6:
			size_limit = strtoll(optarg, NULL, 10);
			break;
		case 7:
			age_limit = strtoll(optarg, NULL, 10);
			break;
		case 'o':
			if (!*optarg) {
				fprintf(stderr, "ERROR: --output option requires an argument!\n");
				print_usage(argc, argv, 1);
				return 2;
			} else {
				if (freopen(optarg, "w", stdout) == NULL) {
					fprintf(stderr, "ERROR: Failed to open output file '%s' for writing: %s\n", optarg, strerror(errno));
					return 1;
				}
				term_colors_set_enabled(0);
			}
			break;
		case 'v':
			printf("%s %s\n", TOOL_NAME, PACKAGE_VERSION);
			return 0;
		default:
			print_usage(argc, argv, 1);
			return 2;
		}
	}

	if (force_colors) {
		term_colors_set_enabled(1);
	}

	if (include_kernel > 0 && exclude_kernel > 0) {
		fprintf(stderr, "ERROR: -k and -K cannot be used together.\n");
		print_usage(argc, argv, 1);
		return 2;
	}

	if (include_filter > 0 && exclude_filter > 0) {
		fprintf(stderr, "ERROR: -p and -e/-q cannot be used together.\n");
		print_usage(argc, argv, 1);
		return 2;
	}
	if (include_filter > 0 && exclude_kernel > 0) {
		fprintf(stderr, "ERROR: -p and -K cannot be used together.\n");
		print_usage(argc, argv, 1);
		return 2;
	}

	if (exclude_filter > 0) {
		proc_filter_excluding = 1;
		if (include_kernel) {
			int i = 0;
			for (i = 0; i < num_proc_filters; i++) {
				if (!strcmp(proc_filters[i], "kernel")) {
					free(proc_filters[i]);
					proc_filters[i] = NULL;
				}
			}
		} else if (exclude_kernel) {
			add_filter("kernel");
		}
	} else {
		if (include_kernel) {
			add_filter("kernel");
		} else if (exclude_kernel) {
			proc_filter_excluding = 1;
			add_filter("kernel");
		}
	}

	if (num_untrigger_filters > 0 && num_trigger_filters == 0) {
		triggered = 1;
	}

	argc -= optind;
	argv += optind;

	if (argc > 0) {
		if (!strcmp(argv[0], "pidlist")) {
			if (connect_service(1) < 0) {
				return 1;
			}
			plist_t list = NULL;
			ostrace_get_pid_list(ostrace, &list);
			ostrace_client_free(ostrace);
			ostrace = NULL;
			idevice_free(device);
			device = NULL;
			if (!list) {
				return 1;
			}
			print_sorted_pidlist(list);
			plist_free(list);
			return 0;
		} else if (!strcmp(argv[0], "archive")) {
			if (force_syslog_relay) {
				force_syslog_relay = 0;
			}
			if (argc < 2) {
				fprintf(stderr, "Please specify an output filename.\n");
				return 1;
			}
			FILE* outf = NULL;
			if (!strcmp(argv[1], "-")) {
				if (isatty(1)) {
					fprintf(stderr, "Refusing to directly write to stdout. Pipe the output to another process.\n");
					return 1;
				}
				outf = stdout;
			} else {
				outf = fopen(argv[1], "w");
			}
			if (!outf) {
				fprintf(stderr, "Failed to open %s: %s\n", argv[1], strerror(errno));
				return 1;
			}
			if (connect_service(1) < 0) {
				if (outf != stdout) {
					fclose(outf);
				}
				return 1;
			}
			plist_t options = plist_new_dict();
			if (start_time > 0) {
				plist_dict_set_item(options, "StartTime", plist_new_int(start_time));
			}
			if (size_limit > 0) {
				plist_dict_set_item(options, "SizeLimit", plist_new_int(size_limit));
			}
			if (age_limit > 0) {
				plist_dict_set_item(options, "AgeLimit", plist_new_int(age_limit));
			}
			ostrace_create_archive(ostrace, options, write_callback, outf);
			ostrace_client_free(ostrace);
			ostrace = NULL;
			idevice_free(device);
			device = NULL;
			if (outf != stdout) {
				fclose(outf);
			}
			return 0;
		}
	}

	int num = 0;
	idevice_info_t *devices = NULL;
	idevice_get_device_list_extended(&devices, &num);
	idevice_device_list_extended_free(devices);
	if (num == 0) {
		if (!udid) {
			fprintf(stderr, "No device found. Plug in a device or pass UDID with -u to wait for device to be available.\n");
			return 1;
		}

		fprintf(stderr, "Waiting for device with UDID %s to become available...\n", udid);
	}

	line_buffer_size = 1024;
	line = malloc(line_buffer_size);

	idevice_subscription_context_t context = NULL;
	idevice_events_subscribe(&context, device_event_cb, NULL);

	while (!quit_flag) {
		sleep(1);
	}
	idevice_events_unsubscribe(context);
	stop_logging();

	if (num_proc_filters > 0) {
		int i;
		for (i = 0; i < num_proc_filters; i++) {
			free(proc_filters[i]);
		}
		free(proc_filters);
	}
	if (num_pid_filters > 0) {
		free(pid_filters);
	}
	if (num_msg_filters > 0) {
		int i;
		for (i = 0; i < num_msg_filters; i++) {
			free(msg_filters[i]);
		}
		free(msg_filters);
	}
	if (num_msg_reverse_filters > 0) {
		int i;
		for (i = 0; i < num_msg_reverse_filters; i++) {
			free(msg_reverse_filters[i]);
		}
		free(msg_reverse_filters);
	}
	if (num_trigger_filters > 0) {
		int i;
		for (i = 0; i < num_trigger_filters; i++) {
			free(trigger_filters[i]);
		}
		free(trigger_filters);
	}
	if (num_untrigger_filters > 0) {
		int i;
		for (i = 0; i < num_untrigger_filters; i++) {
			free(untrigger_filters[i]);
		}
		free(untrigger_filters);
	}

	free(line);

	free(udid);

	return 0;
}

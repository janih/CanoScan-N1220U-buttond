// canoscan_n1220u_buttond.c: standalone macOS button-poll daemon for the
// Canon CanoScan N1220U.
//
// The button-read protocol (the 4-byte query command and status bitmask)
// and the libusb interface/endpoint discovery pattern are ported from the
// "Plustek USB" backend of scanbd's bundled scanbuttond (plustek.c and
// libusbi.c), a GPLv2+ Linux scanner-button daemon:
// https://github.com/mdengler/scanbd
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.

#include <errno.h>
#include <libusb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VENDOR_ID 0x04a9
#define PRODUCT_ID 0x2207
#define POLL_INTERVAL_MS 500
#define BULK_TIMEOUT_MS 2000
#define REOPEN_RETRIES 10
#define REOPEN_RETRY_DELAY_MS 300

// The button-status bit is a self-clearing hardware latch: while the
// button is held down it can read as pressed on one poll and idle on the
// next (observed alternating every ~500ms on real hardware), so a single
// idle sample is not enough to consider the button released. Require this
// many consecutive idle samples before re-arming, to fire the action
// script once per physical press rather than once per poll.
#define RELEASE_DEBOUNCE_SAMPLES 3

// See plustek.c scanbtnd_get_button(): writing this 4-byte command and
// reading back 1 status byte returns a bitmask; bit 0x04 is the single
// button on 1-button Plustek-chipset devices such as the N1220U.
static const unsigned char BUTTON_QUERY_CMD[4] = {1, 2, 0, 1};
#define BUTTON_BIT 0x04

static volatile int keep_running = 1;

static void handle_stop_signal(int sig) {
	(void)sig;
	keep_running = 0;
}

static void log_msg(const char* fmt, ...) {
	time_t t = time(NULL);
	struct tm tmv;
	localtime_r(&t, &tmv);
	char timebuf[32];
	strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);
	fprintf(stderr, "[%s] ", timebuf);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

// Mirrors libusbi.c's libusb_search_interface()/in_endpoint()/out_endpoint():
// find the first interface exposing a bulk IN and a bulk OUT endpoint.
static int find_bulk_interface(libusb_device* dev, int* out_interface,
								unsigned char* out_ep, unsigned char* in_ep) {
	struct libusb_config_descriptor* cfg;
	if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return -1;

	int found = -1;
	for (int i = 0; i < cfg->bNumInterfaces && found < 0; i++) {
		const struct libusb_interface_descriptor* idesc = &cfg->interface[i].altsetting[0];
		unsigned char o = 0, in = 0;
		for (int e = 0; e < idesc->bNumEndpoints; e++) {
			const struct libusb_endpoint_descriptor* ep = &idesc->endpoint[e];
			if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
				continue;
			if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
				if (!in) in = ep->bEndpointAddress;
			} else {
				if (!o) o = ep->bEndpointAddress;
			}
		}
		if (o && in) {
			found = i;
			*out_ep = o;
			*in_ep = in;
		}
	}
	*out_interface = found;
	libusb_free_config_descriptor(cfg);
	return found >= 0 ? 0 : -1;
}

// Opens the scanner and claims its bulk interface. Used both at startup and
// to reclaim the device after an action script (which may itself open the
// scanner via SANE) has finished with it.
static int open_and_claim(libusb_context* ctx, libusb_device_handle** handle_out,
						   int* iface_out, unsigned char* out_ep, unsigned char* in_ep) {
	libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
	if (!handle) return -1;

	libusb_device* dev = libusb_get_device(handle);
	int iface;
	if (find_bulk_interface(dev, &iface, out_ep, in_ep) != 0) {
		libusb_close(handle);
		return -1;
	}

	int rc = libusb_claim_interface(handle, iface);
	if (rc != 0) {
		libusb_close(handle);
		return -1;
	}

	*handle_out = handle;
	*iface_out = iface;
	return 0;
}

// Runs the action script, passing the same style of env-vars scanbd itself
// uses (see conf/scanbd.conf: environment { device = "SCANBD_DEVICE";
// action = "SCANBD_ACTION" }).
//
// The button-poll interface is released and the device handle closed
// before the script runs, and reclaimed afterwards: the script commonly
// wants to actually scan via SANE, which needs to open+claim the same USB
// interface, and libusb only allows one claimant at a time. This blocks
// button polling for the duration of the script, which is the right
// tradeoff for a script that uses the scanner itself.
static void run_action_and_reclaim(libusb_context* ctx, libusb_device_handle** handle,
									int* iface, unsigned char* out_ep, unsigned char* in_ep,
									const char* script) {
	libusb_release_interface(*handle, *iface);
	libusb_close(*handle);
	*handle = NULL;

	pid_t pid = fork();
	if (pid < 0) {
		log_msg("fork failed: %s", strerror(errno));
	} else if (pid == 0) {
		setenv("SCANBD_DEVICE", "canoscan_n1220u", 1);
		setenv("SCANBD_ACTION", "scan", 1);
		execl(script, script, (char*)NULL);
		log_msg("exec of %s failed: %s", script, strerror(errno));
		_exit(127);
	} else {
		int status = 0;
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
			log_msg("action script exited with status %d", WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			log_msg("action script killed by signal %d", WTERMSIG(status));
	}

	for (int attempt = 0; attempt < REOPEN_RETRIES; attempt++) {
		if (open_and_claim(ctx, handle, iface, out_ep, in_ep) == 0) return;
		usleep(REOPEN_RETRY_DELAY_MS * 1000);
	}
	log_msg("failed to reopen scanner after running action script");
}

int main(int argc, char** argv) {
	const char* script = argc > 1 ? argv[1] : NULL;
	if (!script) {
		fprintf(stderr, "usage: %s /path/to/action.script\n", argv[0]);
		return 2;
	}

	signal(SIGINT, handle_stop_signal);
	signal(SIGTERM, handle_stop_signal);

	libusb_context* ctx;
	int rc = libusb_init(&ctx);
	if (rc != 0) {
		log_msg("libusb_init failed: %s", libusb_error_name(rc));
		return 1;
	}

	libusb_device_handle* handle = NULL;
	int iface;
	unsigned char out_ep, in_ep;
	if (open_and_claim(ctx, &handle, &iface, &out_ep, &in_ep) != 0) {
		log_msg("CanoScan N1220U (%04x:%04x) not found or busy", VENDOR_ID, PRODUCT_ID);
		libusb_exit(ctx);
		return 1;
	}

	log_msg("CanoScan N1220U ready: interface=%d out_ep=0x%02x in_ep=0x%02x, polling every %dms, script=%s",
			iface, out_ep, in_ep, POLL_INTERVAL_MS, script);

	// Seed with an unknown state so a stale/leftover "pressed" reading on
	// the very first poll (observed after SANE priming, and again after
	// reclaiming post-script) doesn't trigger a spurious action.
	int was_pressed = -1;
	int consecutive_idle = 0;
	while (keep_running) {
		if (!handle) {
			log_msg("no usable device handle, exiting");
			break;
		}

		unsigned char cmd[sizeof(BUTTON_QUERY_CMD)];
		memcpy(cmd, BUTTON_QUERY_CMD, sizeof(cmd));
		int transferred = 0;

		rc = libusb_bulk_transfer(handle, out_ep, cmd, sizeof(cmd), &transferred, BULK_TIMEOUT_MS);
		if (rc != 0 || transferred != (int)sizeof(cmd)) {
			log_msg("button query write failed (%s), transferred=%d", libusb_error_name(rc), transferred);
			libusb_clear_halt(handle, in_ep);
			usleep(POLL_INTERVAL_MS * 1000);
			continue;
		}

		unsigned char status = 0;
		rc = libusb_bulk_transfer(handle, in_ep, &status, 1, &transferred, BULK_TIMEOUT_MS);
		if (rc != 0 || transferred != 1) {
			log_msg("button query read failed (%s), transferred=%d", libusb_error_name(rc), transferred);
			libusb_clear_halt(handle, in_ep);
			usleep(POLL_INTERVAL_MS * 1000);
			continue;
		}

		if (getenv("BUTTOND_DEBUG_RAW")) log_msg("raw status=0x%02x", status);

		int pressed = (status & BUTTON_BIT) != 0;
		if (pressed) {
			consecutive_idle = 0;
			if (was_pressed == 0) {
				log_msg("button pressed (status=0x%02x) -> running %s", status, script);
				run_action_and_reclaim(ctx, &handle, &iface, &out_ep, &in_ep, script);
				was_pressed = -1; // re-seed: don't treat a residual reading as a new edge
				consecutive_idle = 0;
				continue;
			}
			was_pressed = 1;
		} else {
			consecutive_idle++;
			if (was_pressed == -1 || consecutive_idle >= RELEASE_DEBOUNCE_SAMPLES) was_pressed = 0;
		}

		usleep(POLL_INTERVAL_MS * 1000);
	}

	log_msg("shutting down");
	if (handle) {
		libusb_release_interface(handle, iface);
		libusb_close(handle);
	}
	libusb_exit(ctx);
	return 0;
}

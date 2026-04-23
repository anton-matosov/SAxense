// SixAxis Sense

/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <time.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/hidraw.h>
#include <linux/input.h>

#define REPORT_SIZE 141
#define REPORT_ID 0x32
#define SAMPLE_SIZE 64
#define SAMPLE_RATE 3000
#define NANOSECONDS_PER_SECOND 1000000000UL

enum {
	CONTROL_PACKET_ID = 0x11,
	AUDIO_PACKET_ID = 0x12,
	CONTROL_PAYLOAD_SIZE = 7,
	CONTROL_SEQUENCE_OFFSET = 6,
};

template <uint8_t PacketID, size_t PayloadSize>
struct __attribute__((packed)) packet_t {
	uint8_t pid :6 = PacketID;
	bool unk :1 = false;
	bool sized :1 = true;
	uint8_t length = PayloadSize;
	uint8_t data[PayloadSize];
};

using control_packet_t = packet_t<CONTROL_PACKET_ID, CONTROL_PAYLOAD_SIZE>;
using audio_packet_t = packet_t<AUDIO_PACKET_ID, SAMPLE_SIZE>;

struct __attribute__((packed)) report_payload {
	uint8_t tag :4;
	uint8_t seq :4;
	control_packet_t control;
	audio_packet_t audio;
};

struct __attribute__((packed)) report {
	uint8_t report_id;
	union {
		report_payload payload;
		uint8_t raw[REPORT_SIZE - sizeof(uint32_t) /*crc*/];
	};
	uint32_t crc;
};

static_assert(sizeof(report) == sizeof(report::report_id) + REPORT_SIZE, "unexpected report size");

static report *g_report;
static uint8_t *g_control_sequence;

static FILE *g_input_stream;
static FILE *g_output_stream;

static uint32_t crc32(const uint8_t *data, size_t size) {
	uint32_t crc = ~0xEADA2D49;  // 0xA2 seed

	while (size--) {
		crc ^= *data++;
		for (unsigned i = 0; i < 8; i++)
			crc = ((crc >> 1) ^ (0xEDB88320 & -(crc & 1)));
	}

	return ~crc;
}

static void fail(const char *message) {
	perror(message);
	exit(EXIT_FAILURE);
}

static bool detect_stdout_bus_type(unsigned *bus_type) {
	struct hidraw_devinfo device_info;

	if (ioctl(STDOUT_FILENO, HIDIOCGRAWINFO, &device_info) != 0)
		return false;

	*bus_type = device_info.bustype;
	return true;
}

static void validate_output_transport(void) {
	unsigned bus_type;

	if (!detect_stdout_bus_type(&bus_type))
		return;

	if (bus_type == BUS_USB) {
		fprintf(stderr,
			"SAxense only supports DualSense haptics over Bluetooth hidraw output.\n"
			"USB-connected controllers expose haptics through the controller audio interface,\n"
			"so writing this Bluetooth 0x32 report stream to a USB hidraw node causes undefined behavior.\n");
		exit(EXIT_FAILURE);
	}
}

static void update_report_crc(void) {
	g_report->crc = crc32((const uint8_t *)g_report,  sizeof(*g_report) - sizeof(g_report->crc));
}

static void handle_timer_tick(int signal_number) {
	(void)signal_number;
	fwrite_unlocked(g_report, sizeof(*g_report), 1, g_output_stream);

	uint8_t *sample_buffer = g_report->payload.audio.data;
	if (!fread_unlocked(sample_buffer, sizeof(*sample_buffer), SAMPLE_SIZE, g_input_stream))
		exit(0);

	(*g_control_sequence)++;
	update_report_crc();
}

static void initialize_report(void) {
	g_report = new report{};

	g_report->report_id = REPORT_ID;
	g_report->payload.tag = 0;
	g_report->payload.seq = 0;
	
	// has output haptics:
	// - 0b11111110
	// - 0b11111101
	// - 0b11111111
	// - 0b11111100
	//
	// no output
	// - 0b01111111
	// - 0b10111111
	// - 0b11011111
	// - 0b11101111
	// - 0b11110111
	// - 0b11111011
	g_report->payload.control.data[0] = 0b11111100;

	// These are some sort of flags. 
	// 0b00000001 - silent
	// 0b00000011 - silent
	// 0b00000111 - silent
	// 0b00001111 - normal
	// 0b00001100 - silent
	// 0b00001110 - silent
	// 0b00001000 - some sort of low pass filter or different encoding, high frequencies are gone. In this mode control sequence counter doesn't seem to be needed. Might be some sort of compatibility mode
	// Any bits in the top 4 seem to enable normal haptics.
	// 0b00010000 - normal
	// 0b00100000 - normal
	// 0b01000000 - normal
	// 0b10000000 - normal
	// 0b01010000 - normal
	// 0b11110000 - normal
	// 0b11110001 - normal
	// 0b11110011 - normal
	// 0b11110010 - normal
	// 0b11110100 - normal
	// 0b11111000 - normal
	g_report->payload.control.data[5] = 0b11110000;

	// Byte 6 in the control payload acts as the packet sequence counter.
	g_control_sequence = &g_report->payload.control.data[CONTROL_SEQUENCE_OFFSET];

	update_report_crc();
}

static timer_t start_sample_timer(void) {
	struct itimerspec timer_spec = {};
	timer_spec.it_interval.tv_nsec = NANOSECONDS_PER_SECOND * SAMPLE_SIZE / (SAMPLE_RATE * 2);
	timer_spec.it_value.tv_nsec = 1;

	timer_t timer_id;
	struct sigevent signal_event = {};
	signal_event.sigev_notify = SIGEV_SIGNAL;
	signal_event.sigev_signo = SIGRTMIN;
	signal_event.sigev_value.sival_ptr = &timer_id;

	if (signal(SIGRTMIN, handle_timer_tick) == SIG_ERR)
		fail("signal");
	if (timer_create(CLOCK_MONOTONIC, &signal_event, &timer_id) != 0)
		fail("timer_create");
	if (timer_settime(timer_id, 0, &timer_spec, NULL) != 0)
		fail("timer_settime");

	return timer_id;
}

int main(int argc, char *argv[]) {
	if (argc > 1) {
		g_input_stream = fopen(argv[1], "rb");
		if (!g_input_stream)
			fail("fopen input");
	} else {
		g_input_stream = stdin;
	}
	if (argc > 2) {
		g_output_stream = fopen(argv[2], "wb");
		if (!g_output_stream)
			fail("fopen output");
	} else {
		g_output_stream = stdout;
	}
	setbuf(g_input_stream, NULL);
	setbuf(g_output_stream, NULL);

	validate_output_transport();
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		fail("mlockall");

	initialize_report();
	(void)start_sample_timer();

	for (;;) sleep(3600);
}

// by Sdore, 2025
//  www.sdore.me

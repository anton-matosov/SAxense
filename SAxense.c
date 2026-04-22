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
#include <sys/mman.h>

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

typedef struct __attribute__((packed)) packet {
	uint8_t pid :6;
	bool unk :1,
	     sized :1;
	uint8_t length;
	uint8_t data[];
} packet_t;

typedef struct __attribute__((packed)) report {
	uint8_t report_id;
	union {
		struct __attribute__((packed)) {
			uint8_t tag :4,
			        seq :4;
			uint8_t data[];
		};
		struct __attribute__((packed)) {
			uint8_t payload[REPORT_SIZE - sizeof(uint32_t)];
			uint32_t crc;
		};
	};
} report_t;

static report_t *g_report;
static uint8_t *g_sample_buffer;
static uint8_t *g_control_sequence;

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

static void update_report_crc(void) {
	g_report->crc = crc32((const uint8_t *)g_report, 1 + sizeof(g_report->payload));
}

static void handle_timer_tick(int signal_number) {
	(void)signal_number;
	fwrite_unlocked(g_report, sizeof(*g_report), 1, stdout);

	if (!fread_unlocked(g_sample_buffer, sizeof(*g_sample_buffer), SAMPLE_SIZE, stdin))
		exit(0);

	(*g_control_sequence)++;
	update_report_crc();
}

static void initialize_report(void) {
	static const packet_t control_packet_template = {
		.pid = CONTROL_PACKET_ID,
		.sized = true,
		.length = CONTROL_PAYLOAD_SIZE,
		.data = {0b11111110, 0, 0, 0, 0, 0xFF, 0},
	};
	static const packet_t audio_packet_template = {
		.pid = AUDIO_PACKET_ID,
		.sized = true,
		.length = SAMPLE_SIZE,
		.data = {[0 ... SAMPLE_SIZE - 1] = 0},
	};

	g_report = malloc(sizeof(*g_report));
	if (!g_report)
		fail("malloc");

	g_report->report_id = REPORT_ID;
	g_report->tag = 0;

	packet_t *control_packet = (packet_t *)(g_report->data + 0);
	packet_t *audio_packet = (packet_t *)(g_report->data + sizeof(control_packet_template) + control_packet_template.length);

	memcpy(control_packet, &control_packet_template, sizeof(control_packet_template) + control_packet_template.length);
	memcpy(audio_packet, &audio_packet_template, sizeof(audio_packet_template));

	// Byte 6 in the control payload acts as the packet sequence counter.
	g_control_sequence = &control_packet->data[CONTROL_SEQUENCE_OFFSET];
	g_sample_buffer = audio_packet->data;
	update_report_crc();
}

static timer_t start_sample_timer(void) {
	struct itimerspec timer_spec = {0};
	timer_spec.it_interval.tv_nsec = NANOSECONDS_PER_SECOND * SAMPLE_SIZE / (SAMPLE_RATE * 2);
	timer_spec.it_value.tv_nsec = 1;

	timer_t timer_id;
	struct sigevent signal_event = {0};
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

int main(void) {
	setbuf(stdin, NULL);
	setbuf(stdout, NULL);
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		fail("mlockall");

	initialize_report();
	(void)start_sample_timer();

	for (;;) sleep(3600);
}

// by Sdore, 2025
//  www.sdore.me

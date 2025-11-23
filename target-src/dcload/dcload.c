/*
 * dcload, a Dreamcast ethernet loader
 *
 * Copyright (C) 2001 Andrew Kieschnick <andrewk@austin.rr.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

// Significantly overhauled by Moopthehedgehog, 2019-2020

/* uncomment following line to enable crappy screensaver (it just blanks) */
/* #define SCREENSAVER */

#include "display.h"
#include "packet.h"
#include "adapter.h"
#include "net.h"
#include "cdfs.h"
#include "maple.h"
#include "settings.h"

#include "perfctr.h"

// Volatile informs GCC not to cache the variable in a register.
// Most descriptions of volatile are that the data might be modified by an
// interrupt, is in a hardware register, or may be modified by processes outside
// this code (e.g. think a memory location accessed by different threads on
// multiple cores), so don't do any fancy optimizations with it. Internally,
// that means "don't optimize this variable that lives in memory by caching it
// in a register for later access."
//
// See https://blog.regehr.org/archives/28 for a great discussion on volatile
// variables.
//
// The reason I'm using it here is so that this program doesn't get wrecked by
// processes that are able to return to DCLOAD. The go.s file also wipes out all
// registers and the stack, so GCC needs to be told not to put long-term
// variables there. It does mean the memory footprint increases a tiny bit, but
// it's not huge.
volatile unsigned char booted = 0;
volatile unsigned char running = 0;

// Keep track of background color depending on the type of adapter installed
// volatile unsigned int global_bg_color = BBA_BG_COLOR;
volatile unsigned int installed_adapter = BBA_MODEL;

char *mac_string = "de:ad:be:ef:ba:be";

// For mac address string printing (mac address is in network byteorder)
static void uchar_to_string_hex(unsigned int foo, char *bar)
{
	char hexdigit[17] = "0123456789abcdef";

	bar[1] = hexdigit[foo & 0x0f];
	bar[0] = hexdigit[foo >> 4];
}

// The C language technically requires that all uninitialized global variables
// get initted to 0 via .bss, which is something I really don't like relying on.
// 'our_ip' is declared in commands.c, of all places, and is not given an
// explicit initializer (so I gave it one because it really does need one)...
// In commands.c it also looks like 'our_ip' gets overwritten by the destination
// IP of dcload-ip command packets routed to the Dreamcast. Thankfully, this is
// fine when that destination address will always match the DHCP address.
// It does also allow the "arp" trick to continue functioning if needed, but the
// IP address for arp is not allowed to be 0.x.x.x any more, as mentioned in
// various other comments in this file.
// --Moopthehedgehog

// This function looks to be the primary initializer of the our_ip variable...
static void set_ip_from_file(void)
{
	unsigned char i, c;
	unsigned char *ip = (unsigned char *)&our_ip;

	if(ip[3] != 0)
	{
		// We probably got back here from an exception or something. Original IP's still valid!
		return;
	}

	i = 0;
	c = 0;

	// Any IP will work now

	while(DREAMCAST_IP[c] != '\0')
	{
		if (DREAMCAST_IP[c] == '.')
		{
			i++;
			c++;
		}
		else
		{
			ip[i] *= 10;
			ip[i] += DREAMCAST_IP[c] - '0';
			c++;
		}
	}

	// IP is currently stored, for example, ip[0] 192-168-0-11 ip[3] (this is big endian data on a little endian system), which is what networking packets expect
	our_ip = ntohl(our_ip);
	// Now it's stored ip[0] 11-0-168-192 ip[3] (this is little endian data on a little endian system), which make_ip handles when building packets.
}

int main(void)
{
	unsigned char start;

	running = 0;

	/*    scif_init(115200); */

	if (adapter_detect() < 0)
		error_bb("NO ETHERNET ADAPTER DETECTED!");

	for(start = 0; start < 6; start++)
		uchar_to_string_hex(bb->mac[start], mac_string + start*3);

	set_ip_from_file();

	cdfs_redir_save(); /* will only save value once */
	cdfs_redir_disable();

	maple_init();

	if (!booted) {
		disp_info(bb->name, mac_string, our_ip);
		booted = 1;
	} else {
		booted = 0;
	}

	/*
		scif_puts(NAME);
		scif_puts("\n");
	*/

	// Enable PMCR if it's not enabled
	// (Don't worry, this won't run again if it's already enabled; that would accidentally reset the lease timer!)
#ifndef BUS_RATIO_COUNTER
	PMCR_Init(DCLOAD_PMCR, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);
#else
	PMCR_Init(DCLOAD_PMCR, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_RATIO_CYCLES);
#endif

	while (1) {

		if (booted) {
			disp_status("idle...");
		}

		bb->loop(1); // Identify that this bb->loop is the main loop for set_ip_dhcp()
	}
}

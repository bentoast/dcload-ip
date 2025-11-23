#include "adapter.h"
#include "display.h"
#include "dhcp.h"
#include "rtl8139.h"
#include "lan_adapter.h"
#include "net.h"
#include "perfctr.h"
#include "settings.h"

// Loop escape flag, used by all drivers.
volatile unsigned char escape_loop = 0;
int timeout_loop = 0;
int loop_secs_elapsed = 0;

// The currently configured driver.
adapter_t * bb;

// Packet receive buffer
__attribute__((aligned(32))) unsigned char raw_current_pkt[RAW_RX_PKT_BUF_SIZE]; // Here's a global array. Global packet receive buffer.
// Need to offset the packet by 2 for the command->data after headers to always be aligned to 8 bytes
// The performance gains are well worth the 2 wasted bytes.
__attribute__((aligned(2))) unsigned char * current_pkt = &(raw_current_pkt[2]);

// CPU clock cycles need doubles in 1 count = 1 cycle mode
#ifndef BUS_RATIO_COUNTER
__attribute__((aligned(8))) static const unsigned int const_perf_freq[2] = {PERFCOUNTER_SCALE_DOUBLE_HIGH, PERFCOUNTER_SCALE_DOUBLE_LOW};
__attribute__((aligned(8))) static const unsigned int const_32[2] = {0x41f00000, 0x00000000}; // 2^32 in SH4 double format
// PERFCOUNTER_SCALE is always 32-bit, but it's a constant stored in memory so using a double is OK
// When using doubles, the 32-bit halves need to be flipped since SH4 doesn't reoreder
// endianness for the 32-bit halves of a 64-bit double.
#endif

static volatile unsigned int current_counter_array[2] = {0};
static volatile unsigned int old_dhcp_lease_updater_array[2] = {0}; // To update lease time display
static volatile unsigned char dont_renew = 0;

static const char *dhcp_mode_string = " (DHCP Mode)"; // Indicator that DHCP is active
static const char *dhcp_timeout_string = " (DHCP Timed Out!)"; // DHCP timeout indicator

int adapter_detect() {
	// Try the BBA first.
	if (adapter_bba.detect() >= 0) {
		bb = &adapter_bba;
	} else if (adapter_la.detect() >= 0) {
		bb = &adapter_la;
	} else {
		return -1;
	}

	// Initialize the chosen adapter.
	if (bb->init() < 0)
		return -1;

	escape_loop = 0;

	return 0;
}

// Magic IP range to enable DHCP mode is 0.0.0.0/8, aka 0.x.x.x
// This range is reserved for source addresses only, per IETF.
// See https://en.wikipedia.org/wiki/IPv4#Special-use_addresses for details.
// DHCP clients have a source address of 0.0.0.0 and broadcast DHCP discover
// to 255.255.255.255 in order to initiate the DHCP process.

// This means, to use ARP to assign an IP address, use something like the
// 169.254.0.0/16 (a.k.a. 169.254.x.x) range. This range is specifically meant
// for that kind of direct link between two hosts.
void set_ip_dhcp(void)
{
	// Normally this would be if(!booted). However, this branch is not likely to
	// run frequently at all, so tell GCC to minimize its footprint in set_ip_dhcp()
	// with this macro. Ergo, we expect !booted to evaluate to 0.
	if(__builtin_expect(!booted, 0))
	{
		disp_info(bb->name, mac_string, our_ip);
		disp_status("idle...");
	}

	// Check renewal condition. Only matters if dhcp_lease_time has been set before.
	// The counter is 48-bit, so the max lease time allowed in 1 count = 1 cycle
	// mode is around 1410902 seconds. That's about 16.33 days.

	// SH4 dmulu.l is 32 bit * 32 bit --> 64 bit
	// GCC knows to use dmulu.l based on this code.
	// Multiplying is always better than dividing if possible.
	// By scaling up we can at least ensure that, internally, seconds comparisons will be accurate.
	// Displaying it to the user is not quite as straightforward.
	unsigned long long int long_dhcp_lease_time = (unsigned long long int)dhcp_lease_time * (unsigned long long int)(PERFCOUNTER_SCALE);
	PMCR_Read(DCLOAD_PMCR, current_counter_array);

	unsigned long long int *current_counter = (unsigned long long int*)current_counter_array;
	unsigned long long int *old_dhcp_lease_updater = (unsigned long long int*)old_dhcp_lease_updater_array;

	// Check if lease is still active, renewal threshold is at 50% lease time. '>> 1' is '/2'.
	// NOTE: GCC apparently can't handle the concept of dividing 64-bit numbers on SH4, even by a power of two.
	// It adds over 1kB of extra code at the mere sight of it, weirdly, so we do the shift manually here.
	if(__builtin_expect(dhcp_lease_time && (!dont_renew) && ((long_dhcp_lease_time >> 1) < (*current_counter)), 0))
	{
		dhcp_lease_time = 0; // This disables DHCP renewal unless it gets updated with a valid value. Its dual-purpose is a renewal code enabler.
		old_dhcp_lease_updater_array[0] = 0;
		old_dhcp_lease_updater_array[1] = 0;
		// Renewal needed
		dhcp_waiting_mode_display();

		disp_status("DHCP renewing...");
		int renew_result = dhcp_renew((unsigned int*)&our_ip); // So that GCC doesn't warn about volatile

		if(renew_result == -2)
		{
			// Not an error, it just means IP was no longer valid. Time to get a new one!
			// We're supposed to wait until 87.5% of the lease time has elapsed to do a new discover.
			dont_renew = 1;
		}
		else if(renew_result == -1)
		{
			// Errored out (ACK was invalid/didn't pass validation steps in
			// handle_dhcp_reply()), so IP is invalid. Therefore set it to
			// 255.255.255.255 so that the dhcp discover process won't occur.
			// With a lease time of 0 and an IP of 0xffffffff, DHCP renewal and
			// discovery will both be disabled, effectively disabling DHCP entirely.
			our_ip = 0xffffffff;

			// Update IP, change DHCP active indicator to timeout.
			update_ip_display(our_ip, dhcp_timeout_string);
			// Display lease time of 0
			update_lease_time_display(dhcp_lease_time);

			// At this point DHCP is disabled.
			// Don't need the counter anymore.
			PMCR_Disable(DCLOAD_PMCR);

			// Early return because there's no point in continuing.
			// dhcp_renew() runs dhcp_go() if it gets a NAK, and we probably got here
			// because of dhcp_nest_counter_maxed
			return;
		}
		else
		{
			// Not in waiting mode any more
			update_ip_display(our_ip, dhcp_mode_string);
			// Didn't time out! Display new lease time.
			update_lease_time_display(dhcp_lease_time);
		}

		disp_status("idle..."); // Staying consistent with DCLOAD conventions
	}
	// Only update if we need to. bb->loop could very easily be faster than 1 Hz.
	// Also: When int size is 32-bit, 'ULL' indicates a constant is actually
	// supposed to be stored in an unsigned long long int (64-bit, in SH4's case).
	else if( (long_dhcp_lease_time >= (*current_counter)) && ((*current_counter) > ((*old_dhcp_lease_updater) + ONSCREEN_REFRESH_SCALED - 1ULL)) )
	{
		old_dhcp_lease_updater_array[0] = current_counter_array[0];
		old_dhcp_lease_updater_array[1] = current_counter_array[1];

		// SH4 can only do 32 bit / 32 bit --> 32 bit for integers
		// Need to somehow divide the (long_dhcp_lease_time - current_counter) difference, which is 64-bit, without 64-bit divide...
		unsigned long long int difference = long_dhcp_lease_time - (*current_counter);

#ifndef BUS_RATIO_COUNTER
			// Well, there is a 64-bit divide. SH4 can do doubles.
			// In little endian mode the high half and low half of the double are
			// flipped, though.
			// Setting this up is also a little convoluted...
			unsigned int remaining_lease = 0;
			unsigned int old_fpscr = 0;

			// Save fpscr value so we can use it
			asm volatile ("sts fpscr,r1\n\t" // Store old fpscr into R1
										"mov r1,%[out_old_fpscr]\n\t" // Store old fpscr for safekeeping
				: [out_old_fpscr] "=r" (old_fpscr)
				: // no inputs
				: "r1"
			);
#ifndef UNDEFINED_DOUBLES
			// Right-way-of-doing-it method
			//
			// Unfortunately it appears that SZ = 1, PR = 1 is undefined on SH4, so we
			// gotta do some light gymnastics here.
			//

			// Switch to double-precision & single-move mode
			unsigned int double_tmp_fpscr = 0;
			double_tmp_fpscr = old_fpscr | (1 << 19); // Set FPSCR.PR to double-precision mode
			double_tmp_fpscr &= ~(1 << 20); // Clear FPSCR.SZ to single move mode

			asm volatile ("mov %[in_dbl_tmp],r1\n\t" // Load new fpscr into R1
				: // no outputs
				: [in_dbl_tmp] "r" (double_tmp_fpscr)
				: "r1"
			);
			// GCC has no clobber for fpscr, so this needs to be its own line
			asm volatile ("lds r1,fpscr\n\t"); // Load R1 into fpscr

			// Using paired moves would reduce this to 2 instructions and minimize memory accesses
			// Unpaired moves take 2 instructions (memory accesses) per double
			// Don't be fooled: I already flipped the two 32-bit halves of the doubles in memory
			// because I wanted to try using paired moves (via fmov.d). ;)
			asm volatile ("fmov.s @%[in_dblfreq]+,FR2\n\t" // Load SH4 double-encoded frequency directly into DR2
										"fmov.s @%[in_dblfreq],FR3\n\t"
										"fmov.s @%[in_dbl32]+,FR6\n\t" // Load SH4 double-encoded 2^32 directly into DR6
										"fmov.s @%[in_dbl32],FR7\n\t"
				: // no outputs
				: [in_dblfreq] "r" (const_perf_freq), [in_dbl32] "r" (const_32)
				: "fr2", "fr3", "fr6", "fr7"
			);

			// End right-way-of-doing-it setup method
#else
			//
			// Undefined behavior method
			//
			// I would guess the only thing undefined about it is that the 32-bit
			// halves of a double are flipped--technically that qualifies for
			// "undefined behavior." It wasn't until the SH4A that SZ=1, PR=1 actually
			// allowed for proper double-precision loads/stores and operations in
			// either endianness, and chances are that's why it's a reserved
			// combination on SH4 (i.e. Hitachi engineers were likely planning for it
			// and were not able to make it work until SH4A). Using SZ=1, PR=1 would
			// make code not interchangeable between SH4 and SH4A, and this kind of
			// incompatibility would definitely make the "undefined behavior"
			// designation make sense.
			//

			unsigned int undefined_tmp_fpscr = 0;
			undefined_tmp_fpscr = old_fpscr | (3 << 19); // Set FPSCR.PR to double-precision mode & FPSCR.SZ to pair-move mode

			asm volatile ("mov %[in_undef_tmp],r1\n\t" // Load new fpscr into R1
				: // no outputs
				: [in_undef_tmp] "r" (undefined_tmp_fpscr)
				: "r1"
			);
			// GCC has no clobber for fpscr, so this needs to be its own line
			asm volatile ("lds r1,fpscr\n\t"); // Load R1 into fpscr

			// Using paired moves to reduce this to 2 instructions
			// Unpaired moves would take 2 instructions per double
			asm volatile ("fmov.d @%[in_dblfreq],DR2\n\t" // Load SH4 double-encoded frequency directly into DR2
										"fmov.d @%[in_dbl32],DR6\n\t" // Load SH4 double-encoded 2^32 directly into DR6
				: // no outputs
				: [in_dblfreq] "r" (const_perf_freq), [in_dbl32] "r" (const_32)
				: "dr2", "dr6"
			);

			// End undefined behavior setup method
#endif
			// Manually convert 48-bit int to double
			asm volatile ("lds %[in_dbl0],FPUL\n\t" // Load low half
										"cmp/pl %[in_dbl0]\n\t" // Low half (signed) > 0 ? (T = 1) : (T = 0)
										"bt.s .skip_sign\n\t" // Skip sign conversion if > 0 (i.e. T == 1) (delayed)
										"float FPUL,DR0\n\t" // Convert low half to double (delay slot)
										"fadd DR6,DR0\n\t"// Handle sign conversion of low half by adding 2^32 to negatives
									".skip_sign:\n\t" // What's nice about doubles is that no information is lost in 32-bit sign conversion
										"lds %[in_dbl1],FPUL\n\t" // Load high half
										"float FPUL,DR4\n\t" // Convert high half to double
										"fmul DR6,DR4\n\t" // DR4 *= 2^32 to correct for the split
										"fadd DR4,DR0\n\t" // DR0 += DR4 to put Humpty Dumpty back together again
										// And now we have a 48-bit int converted to a double on an SH4.
										"fdiv DR2,DR0\n\t" // DR0 /= DR2; the whole point of all this
										"ftrc DR0,FPUL\n\t" // Convert result to 32-bit int in FPUL
										"sts FPUL,%[out_int]\n\t" // Get the answer out
				: [out_int] "=r" (remaining_lease)
				:	[in_dbl0] "r" (((unsigned int*)&difference)[0]), [in_dbl1] "r" (((unsigned int*)&difference)[1])
				: "t", "fpul", "dr0", "dr2", "dr4", "dr6" // "t-bit" gets clobbered by cmp/pl
			);
			// Constraints for SH can be found here:
			// https://github.com/gcc-mirror/gcc/blob/master/gcc/config/sh/constraints.md
			//
			// GCC doesn't always document arch-specific constraints in an easy-to-find
			// way. In such situations, best to check /gcc/config/<arch>/constraints.md
			// directly. Common constraints are in /gcc/common.md, but those are usually
			// well-documented on gcc's help pages.

			// Cleanup -- don't want to mess with programs that expect certain bits set by default.
			// Bad form on the program's part if so, but I don't want to be responsible for breaking things.
			asm volatile ("mov %[in_old_fpscr],r1\n\t" // Load old fpscr into R1
				: // no outputs
				: [in_old_fpscr] "r" (old_fpscr)
				: "r1"
			);
			// GCC has no clobber for fpscr, so this needs to be its own line
			asm volatile ("lds r1,fpscr\n\t"); // Load R1 into fpscr

			// This is what just happened:
	//		double div = ((double)difference) / ((double)PERFCOUNTER_SCALE);
	//		unsigned int remaining_lease = (unsigned int)div;

//		unsigned int remaining_lease = (unsigned int)((difference * 43ULL) >> 33); // <-- Assumes perfcounter values are supposed to be divided by 200,000,000 (well, 99750000 x 2, really)
#else
		// This is for CPU/bus ratio mode.
		// Assumes perfcounter values are to be divided by experimentally-derived
		// 2,393,976,245 (close enough to 99.75MHz * 24)
		unsigned int remaining_lease = (unsigned int)((difference * 14697ULL) >> 45);
		// This crazy number, if the perf counter is in bus ratio mode, is only off
		// by one second every 117575 seconds, which is the entire countable range
		// in that mode.
#endif
		// And, finally update the lease timer
		update_lease_time_display(remaining_lease);

		// MATH!
		//
		// The trick to the crazy 14697 number is finding a fraction very close to
		// 1/(bus clk * 24), but the denominator is a power of two and the
		// numerator is as close to an integer as possible. The maximum number that
		// can multiply the maximum 48-bit number and stay within 64-bits is 2^16
		// (64 - 48 = 16), or 65535. Note that I'm using 1/(99750000 * 24) as a
		// baseline, as I assume 99.75MHz is the target bus frequency.
		//
		// The upper bound for the denominator is 2^47 given those constraints, as
		// (2^47) * 1/(99750000 * 24) is the biggest number under 65535 we can get
		// without exceeding 65535. So the next step is to multiply 1/(bus clk * 24)
		// by every power of two that keeps the result >= 1, and use the numerator
		// that is closest to an integer to minimize error. For mine, this is 2^45,
		// which has a numerator of 14697.04... It's the closest numerator to an
		// integer of all the options, so it'll be the most accurate. For bus clocks
		// very close to 99.75MHz, 14697 is still the best number for this.
		//
		// Now what GCC does here is really neat: it multiplies the upper 32 bits of
		// 'difference' by the numerator, and then 32-bit x 32-bit --> 64-bit
		// multiplies the numerator by the lower 32 bits. Then, it adds the result
		// of the upper 32-bit multiply with the upper 32-bits of the 64-bit result
		// from the lower 32-bits multiply. Then it shifts that sum right by 13
		// instead of 45, and uses that register. This is one of my favorite tricks,
		// and I'm glad to see GCC doing it here. :)
		//
		// Note:
		// Finding error: first find the actual number of seconds the machine will
		// count to. In my case it's 117576. (2^48 - 1)/COUNTER_VALUE_PER_SEC = max
		// seconds, where for me COUNTER_VALUE_PER_SEC = 2393976245. Then,
		// ((14697/2^45)*2393976245)*117576 = max displayed seconds. For me, max
		// displayed seconds is 117575. That's one second lost per the entire
		// countable range in CPU/bus ratio mode, which is about 1 day, 8 hours, and
		// 40 minutes! In my actual calculation I kept as many decimal places as
		// possible, but they've been omitted here for clarity.
		//
		// The internal timer will not have any slowdown, as there is no approximate
		// division being done anywhere except for displaying the countdown on-
		// screen. Floats would solve this, but this is MUCH faster. I mean, just
		// compare it to the doubles shenanigans necessary to divide by 200MHz!
		// (Unfortunately there's no good approximation for 200MHz. The best I
		// found, 43/2^33, loses 0.8 seconds every 10 minutes.)
		//

#ifdef PERFCTR_DEBUG
		clear_lines(174, 24, global_bg_color);
		uint_to_string(current_counter_array[1], (unsigned char*)dhcp_lease_time_string);
		draw_string(30, 174, dhcp_lease_time_string, STR_COLOR);
		uint_to_string(current_counter_array[0], (unsigned char*)dhcp_lease_time_string);
		draw_string(126, 174, dhcp_lease_time_string, STR_COLOR);
	#if (DCLOAD_PMCR == 1)
		uint_to_string(*((volatile unsigned short*)PMCR1_CTRL_REG), (unsigned char*)dhcp_lease_time_string);
	#elif (DCLOAD_PMCR == 2)
		uint_to_string(*((volatile unsigned short*)PMCR2_CTRL_REG), (unsigned char*)dhcp_lease_time_string);
	#else
		uint_to_string(0xffffffff, (unsigned char*)dhcp_lease_time_string);
	#endif
		draw_string(230, 174, dhcp_lease_time_string, STR_COLOR);
#endif
	}

	// We're supposed to wait until 87.5% of the lease time has elapsed to do a new discover,
	// if we even need to do a new discover (due to getting a NAK during renewal).
	// GCC will almost certainly optimize this into the conditional. This is just way more readable.
	// Also: best variable name ever, lol.
	unsigned long long int eighty_seven_point_five = (long_dhcp_lease_time >> 1) + (long_dhcp_lease_time >> 2) + (long_dhcp_lease_time >> 3); // 0.5 + 0.25 + 0.125 = 0.875, or 87.5%
	// This checks if set_ip_from_file() gave us a DHCP-mode IP (0.x.x.x) or a static
	// IP of some sort. It also checks if the DHCP lease expired per above. DOUBLE WHAMMY!!
	// Only need to check the first octet of IP since it's a /8 range
	// Also check if renew was NAK'd, and if it was, are we at the 87.5% threshold for a new discover?
	if(__builtin_expect(((our_ip & 0xff000000) == 0) || (dont_renew && (eighty_seven_point_five < (*current_counter))), 0))
	{
		dont_renew = 0;
		dhcp_waiting_mode_display();

		// Wait until DHCP assigns an IP address. This should not take very long.
		// If it takes over 10 seconds, DHCP is probably not working.
		disp_status("Acquiring new IP address via DHCP...");
		int dhcp_result = dhcp_go((unsigned int*)&our_ip); // So that GCC doesn't warn about volatile
		if((dhcp_result == -1) || dhcp_nest_counter_maxed)
		{
			// IP will be 0.0.0.0. Use the old ARP method at this point.
			// Since dhcp_lease_time wasn't set, renewal won't happen; no worries there.

			// So set IP to 255.255.255.255, which means that an ACK was received but
			// it was invalid/didn't pass the validation checks in handle_dhcp_reply()
			our_ip = 0xffffffff;

			// Change DHCP active indicator to timeout.
			update_ip_display(our_ip, dhcp_timeout_string);

			// At this point DHCP is disabled.
			// Don't need the counter anymore.
			PMCR_Disable(DCLOAD_PMCR);
		}
		else
		{
 			// Else we didn't time out and probably got an address, hurray!
			// Overwrite displayed IP with DHCP-provided IP
			update_ip_display(our_ip, dhcp_mode_string);
			// Display time until IP lease ends.
			update_lease_time_display(dhcp_lease_time);
		}

		disp_status("idle..."); // Staying consistent with DCLOAD conventions
	}

	// dcload-ip can now keep on going as normal.
}

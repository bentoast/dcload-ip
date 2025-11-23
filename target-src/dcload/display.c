#include "display.h"
#include "packet.h"
#include "video.h"

volatile unsigned int global_bg_color = BBA_BG_COLOR;

static char *ip_string = "000.000.000.000"; // Reserve this much memory for max-size IP address

static const char *waiting_string = "Waiting for IP..."; // Waiting for IP indicator. 15 visible characters to match IP address string's visible 15 characters
static const char *dhcp_mode_string = " (DHCP Mode)"; // Indicator that DHCP is active
static const char *dhcp_lease_string = "DHCP Lease Time (sec): "; // DHCP lease time
static const char *dhcp_attempts_string = "DHCP Attempts: "; // DHCP attempts amount
static const char *dhcp_next_string = "Next attempt in... "; // DHCP attempt countdown
static char dhcp_lease_time_string[11] = {0}; // For converting lease time to seconds. 10 characters + null term. Max lease is theoretically 4294967295, but really is 1410902 due to perf counters.
static char dhcp_attempts_num[9] = {0};
static char dhcp_next_counter[9] = {0};

// For IP string printing (IP address is in host byteorder)
static void ip_to_string(unsigned int ip_addr, char *out_string)
{
	ip_addr = htonl(ip_addr); // Make life easier since strings are always stored MSB-first

	char decdigit[11] = "0123456789";
	int c = 0, i = 0;
	unsigned char* ip_octet = (unsigned char*)&ip_addr;
	unsigned char ones, tens, hundreds;

	while(i < 4)
	{
		ones = ip_octet[i] % 10;
		tens = (ip_octet[i] / 10) % 10;
		hundreds = ip_octet[i] / 100;

		if(hundreds)
		{
			out_string[c++] = decdigit[hundreds];
			out_string[c++] = decdigit[tens];
			out_string[c++] = decdigit[ones];
		}
		else if(tens)
		{
			out_string[c++] = decdigit[tens];
			out_string[c++] = decdigit[ones];
		}
		else
		{
			out_string[c++] = decdigit[ones];
		}

		if(i < 3)
		{
			out_string[c++] = '.';
		}

		i++;
	}

	out_string[c] = '\0';
}

void update_ip_display(unsigned int new_ip, const char *mode_string)
{
	clear_lines(126, 24, global_bg_color);
	ip_to_string(new_ip, ip_string);
	draw_string(30, 126, ip_string, STR_COLOR);
	draw_string(210, 126, mode_string, STR_COLOR);
}

void dhcp_waiting_mode_display(void)
{
	clear_lines(126, 24, global_bg_color);
	draw_string(30, 126, waiting_string, STR_COLOR);
	draw_string(234, 126, dhcp_mode_string, STR_COLOR);
}

void update_lease_time_display(unsigned int new_time)
{
	// Casting to char gets rid of GCC warning.
	uint_to_string_dec(new_time, dhcp_lease_time_string);
	clear_lines(424, 48, global_bg_color); // Clear 48 lines so DHCP retry attempts text is also cleared
	draw_string(30, 448, dhcp_lease_string, STR_COLOR);
	draw_string(306, 448, dhcp_lease_time_string, STR_COLOR);
}

/* converts expevt value to description, used by exception handler */
char * exception_code_to_string(unsigned int expevt)
{
	switch(expevt) {
	case 0x1e0:
		return "User break";
		break;
	case 0x0e0:
		return "Address error (read)";
		break;
	case 0x040:
		return "TLB miss exception (read)";
		break;
	case 0x0a0:
		return "TLB protection violation exception (read)";
		break;
	case 0x180:
		return "General illegal instruction exception";
		break;
	case 0x1a0:
		return "Slot illegal instruction exception";
		break;
	case 0x800:
		return "General FPU disable exception";
		break;
	case 0x820:
		return "Slot FPU disable exception";
		break;
	case 0x100:
		return "Address error (write)";
		break;
	case 0x060:
		return "TLB miss exception (write)";
		break;
	case 0x0c0:
		return "TLB protection violation exception (write)";
		break;
	case 0x120:
		return "FPU exception";
		break;
	case 0x080:
		return "Initial page write exception";
		break;
	case 0x160:
		return "Unconditional trap (TRAPA)";
		break;
	default:
		return "Unknown exception";
		break;
	}
}

// NOTE: this is hex, but I don't want to change this function name since it's
// hardcoded into various asm files
void uint_to_string(unsigned int foo, unsigned char *bar)
{
	char hexdigit[17] = "0123456789abcdef";
	int i;

	for(i=7; i>=0; i--) {
		bar[i] = hexdigit[(foo & 0x0f)];
		foo = foo >> 4;
	}
	bar[8] = 0;
}

// called by exception.S
void setup_video(unsigned int mode, unsigned int color)
{
	STARTUP_Init_Video(mode);
	clrscr(color);
}

void disp_info(const char *adapter_name, char *mac_string, unsigned int our_ip)
{
	setup_video(FB_RGB0555, global_bg_color);
	draw_string(30, 54, NAME, STR_COLOR);
	draw_string(30, 78, adapter_name, STR_COLOR);
	draw_string(30, 102, mac_string, STR_COLOR);

	ip_to_string(our_ip, ip_string);
	draw_string(30, 126, ip_string, STR_COLOR);
}

void disp_status(const char * status) {
	clear_lines(150, 24, global_bg_color);
	draw_string(30, 150, status, STR_COLOR);
}

void disp_dhcp_attempts_count(int dhcp_attempts)
{
	clear_lines(424, 24, global_bg_color);
	draw_string(30, 424, dhcp_attempts_string, STR_COLOR);
	uint_to_string_dec(dhcp_attempts, dhcp_attempts_num);
	draw_string(160, 424, dhcp_attempts_num, STR_COLOR);
}

void disp_dhcp_next_attempt(unsigned int time_left)
{
	clear_lines(448, 24, global_bg_color);
	draw_string(30, 448, dhcp_next_string, STR_COLOR);
	uint_to_string_dec(time_left, dhcp_next_counter);
	draw_string(160, 448, dhcp_next_counter, STR_COLOR);
}

void error_bb(char *msg)
{
	setup_video(FB_RGB0555, ERROR_BG_COLOR);
	draw_string(30, 54, NAME, STR_COLOR);
	draw_string(30, 78, msg, STR_COLOR);
	while(1)
	{
		asm volatile ("sleep"); // This way it doesn't actually halt and catch fire ;)
	}
}

/* set n lines starting at line y to value c */
void clear_lines(unsigned int y, unsigned int n, unsigned int c)
{
	unsigned short * vmem = (unsigned short *)(0xa5000000 + y*640*2);
	n = n * 640;
	while (n-- > 0)
		*vmem++ = c;
}

// 'bar' buffer is assumed to be large enough.
// The biggest decimal number is 4294967295, which is 10 characters‬ (excluding null term).
// So the buffer should be able to hold 11 characters.
void uint_to_string_dec(unsigned int foo, char *bar)
{
	char decdigit[11] = "0123456789";
	int i;

	for(i=9; i>=0; i--) {
		if((!foo) && (i < 9))
		{
			bar[i] = ' '; // pad with space
		}
		else
		{
			bar[i] = decdigit[(foo % 10)];
		}
		foo /= 10; // C functions are pass-by-value
	}
	bar[10] = 0; // Null term
}

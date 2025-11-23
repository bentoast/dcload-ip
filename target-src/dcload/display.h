#ifndef __DISPLAY_H__
#define __DISPLAY_H__

// Background color
// In RGB0555 format
// BBA default blue is 0x0010
// LAN default green is 0x0100
// Error default red is 0x2000
#define BBA_BG_COLOR 0x0010
#define LAN_BG_COLOR 0x0100
#define ERROR_BG_COLOR 0x2000

// This is stock dcload now, no more need to explicitly mention "with DHCP"
#define NAME "dcload-ip " DCLOAD_VERSION

// String color (0xffff = white)
#define STR_COLOR 0xffff

extern volatile unsigned int global_bg_color;

// Called by asm functions
char * exception_code_to_string(unsigned int expevt);
void uint_to_string(unsigned int foo, unsigned char *bar);
void setup_video(unsigned int mode, unsigned int color);

// Called by other parts of dcload
void disp_info(const char *adapter_name, char *mac_string, unsigned int our_ip);
void disp_status(const char * status);
void disp_dhcp_attempts_count(int dhcp_attempts);
void disp_dhcp_next_attempt(unsigned int);
void error_bb(char *msg);
void clear_lines(unsigned int y, unsigned int n, unsigned int c);
void uint_to_string_dec(unsigned int foo, char *bar);

void update_ip_display(unsigned int new_ip, const char *mode_string);
void dhcp_waiting_mode_display(void);
void update_lease_time_display(unsigned int new_time);

// These definitions correspond to 'fbuffer_color_mode' in STARTUP_Init_Video()
// dcload only supports 16-bit color modes
#define FB_RGB0555 0
#define FB_RGB565 1

// To set video modes via startup_support.c
// dcload only supports 640x480 modes
void STARTUP_Init_Video(unsigned char fbuffer_color_mode);
void STARTUP_Set_Video(unsigned char fbuffer_color_mode);

#endif

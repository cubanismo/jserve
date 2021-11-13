// NOTE: Only tested in debug builds. Release builds don't work
// This is really, really bare-bones code and is not production quality.
// If you use it, you can probably improve it. :)

/* jserve.cpp : Remote GDB stub for Jaguar

Design:
	jserve listens on a local TCP port for GDB connections
	It parses GDB commands and translates them into binary form
	Binary data is sent, via USB, to EZ scratchpad memory
	The 68K GDB stub exchanges register and Jag storage with EZ scratchpad memory

Scratchpad format:
	$3800:	EZ-HOST buffer base address (set by jserve)
	$3802:	Words to transfer OR single step flag for 'run' (set by jserve)
	$3804:	68K buffer base address OR new PC for 'run' OR 0 for last PC (set by jserve)
	$3808:	Current state (set by stub and jserve)
			-2:		Running
			-1:		Trap, waiting for command
			0:		Ping (do nothing)
			4:		Write memory
			8:		Read memory
			C:		Exit trap (return to current)
	$380A:	Exception vector address (set by stub following trap)

	Registers can be found at $2C00 in Jag memory during trap
	There is no double buffering

Usage:
	set target localhost:4567
	load
	run
*/

#ifdef _WIN32
#include "stdafx.h"
#include <winsock.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef const struct sockaddr* LPSOCKADDR;
//#define IPPROTO_TCP 0
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define Sleep(a) usleep((a)*1000)
#define closesocket close
#endif
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "usb.h"

#define DEFAULT_PORT	4567

#define	EZBUF_BASE		0x3900
#define EZBUF_LEN		0x400
#define EZ_OPCODE		0x3808
#define EZ_CMD			0x3800

#define JAG_REGS		0x2C00
#define REG_BYTES		180

#define	ENBIGEND(_x) (((_x)[0] << 24) + ((_x)[1] << 16) + ((_x)[2] << 8) + (_x)[3])
#define	HALFBIGEND(_x) (((_x)[0] << 8) + (_x)[1])

#define hexc(_x) ((_x) > '9' ? ((_x)-'a')+10 : (_x) - '0')
#define uchar unsigned char

#define debug printf

SOCKET openGDB();
void openJag();
void bye(const char* msg);
char get(SOCKET s);
void put(SOCKET s, char);
void jcp(const char* file);

int jping();
void jcheckup();
void jreset();
void jwrite(int addr, char* buf, int len);
void jread(int addr, char* buf, int len);
void jwritehex(int addr, char* buf, int len);
int jreadhex(int addr, char* buf, int len);

int computeSignal(int exceptionVector);

const char *hex = "0123456789abcdef";
usb_dev_handle* udev = 0;
FILE* flog;
int logdir = -1;
char bpoint[0x200000*2];	// Shadow data for software breakpoints (in hex)
unsigned short defregs[REG_BYTES/2];

int main(int argc, char *argv[])
{
	bool ValidCmd=false;
	while(1) {
		// Connect to Jaguar, then to GDB
		openJag();
		// put the Skunkboard into a safe state, then if the user disconnects reset will work

		SOCKET sock = openGDB();		// this waits...
		openJag();

		// Initialize the registers to something meaningful
		memset(bpoint, '0', 2048*1024);
		memset(defregs, 0, sizeof(defregs));
		defregs[30] = 0x1f00;	// Stack pointer (1f fff8)
		defregs[31] = 0xf8ff;
		defregs[33] = 0x20;		// Supervisor mode
		defregs[35] = 0x40;		// Startup program counter
		jwrite(JAG_REGS, (char*)defregs, REG_BYTES);

		// Parse GDB commands as they are sent, replying to them (with Jaguar's help)
		int chatting = 1, running = 0, sigval = 5;
		char buf[65536], trap[2] = {0x4e, 0x41};

		while (chatting) {
			char *dptr = buf;
			int ck = 0, addr, len, skip = 0, off = 0;
			unsigned short runcmd[5] = {0, 0, 0, 0, 0xc};

			// While not running, just wait for commands from GDB
			if (!running) {
				memset(buf, 0, sizeof(buf));
				while ('$' != get(sock))	;					// Wait for start of command
				for (char *ptr = buf; *ptr = get(sock); ptr++)	// Fetch command
					if ('#' == *ptr || '$' == *ptr)
						break;	
				get(sock);	get(sock);							// Discard checksum
				put(sock, '+');									// Acknowledge request

				if ((buf[0]=='$') && (!ValidCmd)) {
					Sleep(10);
					continue;
				}
				debug("Got command %s\n", buf);
			}
			ValidCmd=true;

			// Wait for the Jaguar to tell us what's up
			if (running) {
				debug("Wait while Running....\n");
			}
			while (running) {
				short status[2] = {0, 0};
				assert(4 == usb_control_msg(udev, 0xc0, 0xff, 4, EZ_OPCODE, (char*)status, 4, 1000));
				if (status[0] == -1) {
					running = 0;
					buf[0] = '?';	// Dump sigval
					sigval = computeSignal(status[1]);
				} else
					Sleep(100);		// Don't hog the CPU
			}

			// Act on command
			switch(buf[0]) {
				// Return current signal state
				case '?':
					debug("Signal: 0x%02X\n", sigval&0xff);

					*dptr++ = 'S';
					*dptr++ = hex[(sigval >> 4) & 15];
					*dptr++ = hex[sigval & 15];
					break;
				// Return current 68K registers
				case 'g':
					debug("Read Registers\n");
					dptr += jreadhex(JAG_REGS, dptr, REG_BYTES);
					break;
				// Write new 68K registers
				case 'G':
					debug("Write Registers\n");
					jwritehex(JAG_REGS, buf+1, REG_BYTES);
					*dptr++ = 'O';	*dptr++ = 'K';
					break;
				// Write memory contents
				case 'M':
					sscanf(buf+1, "%x,%x", &addr, &len);
					
					debug("Write memory 0x%x, %d bytes\n", addr, len);

					for (skip = 0; buf[skip] != ':'; skip++)	;
					if (len < 1) {
						debug("Error - invalid length passed from GDB\n");
						*dptr++ = 'E';	*dptr++ = '0'; *dptr++ = '3';
					} else if (addr+len > 0x200000 && addr+len < 0x800000) {
						debug("Error - can't write cart memory\n");
						*dptr++ = 'E';	*dptr++ = '0'; *dptr++ = '3';
					} else {
						// Backup code for breakpoints (note that the buffer is in hex form)
						if (addr >= 0 && addr+len <= 0x200000)
							memcpy(bpoint+addr*2, buf+1+skip, len*2);
						// Use backup to handle odd address and odd length requests
						if (0 != (addr & 1))	{	addr--;	len++;	}
						if (0 != (len & 1))		{	len++;	}
						memcpy(buf+1+skip, bpoint+addr*2, len*2);
						jwritehex(addr, buf+1+skip, len);
						*dptr++ = 'O';	*dptr++ = 'K';
					}
					break;
				// Insert breakpoint
				case 'Z':
					sscanf(buf+1, "%x,%x,%x", &skip, &addr, &len);
					debug("Add a breakpoint 0x%x, size %d\n", addr, len);

					if (addr >= 0 && addr < 0x200000) {
						jwrite(addr, trap, 2);
						*dptr++ = 'O';	*dptr++ = 'K';
					} else {
						debug("Error - not in RAM space\n");
						*dptr++ = 'E';	*dptr++ = '0'; *dptr++ = '3';
					}
					break;
				// Remove breakpoint
				case 'z':
					sscanf(buf+1, "%x,%x,%x", &skip, &addr, &len);
					debug("Remove a breakpoint 0x%x, size %d\n", addr, len);

					if (addr >= 0 && addr < 0x200000) {
						memcpy(buf, bpoint+addr*2, 4);	// Restore old code from bpoint buffer
						buf[4] = 0;
						debug("Removed break by setting %x to %s\n", addr, buf);
						jwritehex(addr, buf, 2);		// Must copy to avoid jwritehex scramble
						*dptr++ = 'O';	*dptr++ = 'K';
					} else {
						debug("Error - not in RAM space\n");
						*dptr++ = 'E';	*dptr++ = '0'; *dptr++ = '3';
					}
					break;
				// Read memory contents
				case 'm':
					sscanf(buf+1, "%x,%x", &addr, &len);
					debug("Read memory 0x%x, %d bytes\n", addr, len);
					dptr += len*2;	// Transfer data by rounding up the copy size
					jreadhex((addr)&(~1), buf, (len+1)&(~1));
					if (addr & 1)	// Shift left if we rounded off the address
						off = 2;
					break;
				// Single step
				case 's':
					runcmd[1] = 0x8000;		// Single step
					debug("Going to step...\n");
				// Continue (and single step)
				case 'c':
					if ('#' != buf[1]) {	// Have a new PC -- set it
						sscanf(buf+1, "%x", &addr);
						runcmd[2] = addr >> 16;
						runcmd[3] = addr & 65535;
						debug("New PC = 0x%x\n", addr);
					}
					assert(usb_control_msg(udev, 0x40, 0xfe, 10, EZ_CMD, (char*)runcmd, 10, 1000) == 10);
					//*dptr++ = 'O';	*dptr++ = 'K';
					running = 1;
					break;

				// Shut down!
				case '$':
					debug("Empty string shutdown.\n");
					chatting = 0;
					break;

				case 'k':
					debug("Kill NOP\n");
					*dptr++ = 'O';	*dptr++ = 'K';
					break;

				default:
					debug("Unknown command '%c' (0x%02X)\n", buf[0], buf[0]);
			}

			// Return reply
			if ((chatting)&&(!running)) {
				put(sock, '$');
				debug("Reply: ");
				for (char* ptr = buf+off; ptr < (dptr+off); ptr++) {
					put(sock, *ptr);
					debug("%c", *ptr);
					ck += *ptr;
				}
				debug("\n");
				put(sock, '#');
				put(sock, hex[(ck >> 4) & 15]);
				put(sock, hex[ck & 15]);
			}
		}
		printf("\nAborted.\n");
		closesocket(sock);
		fclose(flog);
		ValidCmd=false;
	}

	// Clean up
	usb_close(udev);
}

void bye(const char* msg) {
	printf("%s", msg);
	exit(1);
}

void openJag() {	
	struct usb_bus *bus;

	usb_init();
	usb_set_debug(0);

	usb_find_busses();
	usb_find_devices();

	if (udev != 0)
		usb_close(udev);
	udev = 0;

	struct usb_device *dev = 0;

	for (bus = usb_get_busses(); bus; bus = bus->next)
		for (dev = bus->devices; dev; dev = dev->next)
			if (0x4b4 == dev->descriptor.idVendor && 0x7200 == dev->descriptor.idProduct) {
				udev = usb_open(dev);
				if (!udev)
					bye("Found, but can't access, Jaguar.  Is it in use?\n");

				int len;
				char cmd[1024];
				FILE* fp = fopen("turbow.bin", "rb");

				if (NULL == fp || (len = fread(cmd, 1, 1024, fp)) < 1)
					bye("Could not read turbow.bin\n");
				fclose(fp);

				// Install turbow.bin
				int ret = usb_control_msg(udev, 0x40, 0xff, 0, 0x304c, cmd, len, 1000);
				printf("Installed EZ-HOST stub: %d scan codes sent\n", ret);
				jcheckup();
				return;
			}

	bye("Can't find any Jaguars on USB.\n");
}

// Make sure the Jaguar still works after the latest escapade
void jcheckup() {
	Sleep(100);
	if (jping())
		printf("Connected to existing Jaguar stub...\n");
	if (!jping()) {
		// Wait for JCP to come online
		short poll = 0;
		if (usb_control_msg(udev, 0xC0, 0xff, 4, 0x2800+0xFEA, 
				(char*)&poll, 2, 1000) == 2 && -1 == poll) {
			// Try to install jagdb.cof
			Sleep(100);
			jcp("jdb.cof");
			printf("Establishing contact with Jaguar stub...");
			for (int i = 0; i < 50 && !jping(); i++);
		} 
		if (jping())
			printf("Connected!\n");
		else {
			printf("Failed to contact Jaguar, resetting...\n");
			jreset();
			usb_close(udev);
			udev = 0;
			Sleep(7000);
			openJag();
		}
	}
}

// Ping the Jaguar -- return true on success, else false
int jping() {
	short status = 0;
	if(usb_control_msg(udev, 0x40, 0xfe, 2, EZ_OPCODE, (char*)&status, 2, 1000) != 2)
		return 0;
	Sleep(100);		// Plenty of time!
	assert(usb_control_msg(udev, 0xC0, 0xff, 2, EZ_OPCODE, (char*)&status, 2, 1000) == 2);
	return status;
}

// Reset the Jaguar
void jreset() {
	// We have to use scan mode to access registers (TurboWrite uses DMA engine)
	// Reset is 0xc028=2, 0xc028=0
	unsigned char cmd[10] = {0xB6, 0xC3, 0x04, 0x00, 0x00, 0x28, 0xC0, 0x02, 0x00, 0x00};

	assert(usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmd, 10, 1000) == 10);
	cmd[7] = 0;
	assert(usb_control_msg(udev, 0x40, 0xff, 10, 0x304C, (char*)cmd, 10, 1000) == 10);
}

// Open a socket on localhost for GDB
SOCKET openGDB() {
#ifdef _WIN32
	WORD wVersionRequested = MAKEWORD(1,1);
	WSADATA wsaData;
	WSAStartup(wVersionRequested, &wsaData);
#endif

	int port = DEFAULT_PORT;
	SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	assert(listenSocket != INVALID_SOCKET);
	SOCKADDR_IN saServer;
	saServer.sin_family = AF_INET;
	saServer.sin_addr.s_addr = INADDR_ANY;
	saServer.sin_port = htons(port);
	int ret = bind(listenSocket, (LPSOCKADDR)&saServer, sizeof(struct sockaddr));
	assert(ret != SOCKET_ERROR);
	ret = listen(listenSocket, SOMAXCONN);
	assert(ret != SOCKET_ERROR);

	printf("Waiting on localhost:%d...", port);

	SOCKET sock = accept(listenSocket, NULL, NULL);
	assert(sock != INVALID_SOCKET);

	assert(NULL != (flog = fopen("jdb.log", "w")));
	logdir = 0;
	printf("Connected!  Logging to jdb.log.\n");
	closesocket(listenSocket);
	return sock;
}

// Return one byte from the socket
char get(SOCKET s) {
	char c;
	if (recv(s, &c, 1, 0) != 1)
		c = '$';
	if (logdir != 'g')
		fputs("\n> ", flog);
	logdir = 'g';
	fputc(c, flog);
	return c;
}

// Send one byte to the socket
void put(SOCKET s, char c) {
	send(s, &c, 1, 0);
	if (logdir != 'p')
		fputs("\n  ", flog);
	logdir = 'p';
	fputc(c, flog);
}

// Write len bytes from 68K memory at addr from buf
// Do not call unless you know the stub is idle
void jwrite(int addr, char* buf, int blen) {
	debug("Write to Jaguar address 0x%x, %d bytes\n", addr, blen);

	assert(0 == (blen&1) && 0 == (addr & 1));
	while (blen > 0) {	// Do it in chunks
		int len = blen > EZBUF_LEN ? EZBUF_LEN : blen;

		// 'Fix' the byte order in this block
		for (int i = 0; i < len; i += 2) {
			int swap = buf[i+1];
			buf[i+1] = buf[i];
			buf[i] = swap;
		}

		short cmd[5], status = 0;
		cmd[0] = EZBUF_BASE;	// EZ base address
		cmd[1] = len/2-1;		// Length of this block in words-1
		cmd[2] = addr >> 16;	// 68K base address
		cmd[3] = addr & 65535;
		cmd[4] = 0x4;			// JDB opcode
	
		// Send data, command, then poll for completion
		assert(usb_control_msg(udev, 0x40, 0xfe, len, EZBUF_BASE, buf, len, 1000) == len);
		assert(usb_control_msg(udev, 0x40, 0xfe, 10, EZ_CMD, (char*)cmd, 10, 1000) == 10);
		while (-1 != status)
			assert(usb_control_msg(udev, 0xC0, 0xff, 2, EZ_OPCODE, (char*)&status, 2, 1000) == 2);
			
		buf += len;
		addr += len;
		blen -= len;
	}
}

// Same as jwrite only the buffer is in hex format
// Note that this scrambles the hex buffer during operation
void jwritehex(int addr, char* buf, int len) {
	for(int i = 0; i < len; i++)
		buf[i] = (hexc(buf[i*2]) << 4) + hexc(buf[i*2+1]);
	jwrite(addr, buf, len);
}

// Read len bytes from 68K memory at addr into buf
// Do not call unless you know the stub is idle
void jread(int addr, char* buf, int blen) {
	assert(0 == (blen&1) && 0 == (addr & 1));
	while (blen > 0) {	// Do it in chunks
		int len = blen > EZBUF_LEN ? EZBUF_LEN : blen;

		short cmd[5], status = 0;
		cmd[0] = EZBUF_BASE;	// EZ base address
		cmd[1] = len/2-1;		// Length of this block in words-1
		cmd[2] = addr >> 16;	// 68K base address
		cmd[3] = addr & 65535;
		cmd[4] = 0x8;			// JDB opcode
	
		// Send command and poll for completion
		assert(usb_control_msg(udev, 0x40, 0xfe, 10, EZ_CMD, (char*)cmd, 10, 1000) == 10);
		while (-1 != status) {
			assert(usb_control_msg(udev, 0xC0, 0xff, 2, EZ_OPCODE, (char*)&status, 2, 1000) == 2);
			Sleep(1);
		}
			
		// Read the result into the buffer
		assert(usb_control_msg(udev, 0xC0, 0xff, len, EZBUF_BASE, buf, len, 1000) == len);

		// 'Fix' the byte order in this block
		for (int i = 0; i < len; i += 2) {
			int swap = buf[i+1];
			buf[i+1] = buf[i];
			buf[i] = swap;
		}

		buf += len;
		addr += len;
		blen -= len;
	}
}

// Same as jread only it converts binary into hex
// Note that buf must be twice len to hold the hex output
int jreadhex(int addr, char* buf, int len) {
	int oldlen = len;
	jread(addr, buf, len);
	while(--len >= 0) {
		buf[len*2+1] = hex[buf[len] & 15];
		buf[len*2] = hex[(buf[len] >> 4) & 15];
	}
	return oldlen*2;
}

// Send a boot file to the Jaguar via JCP (such as jdbug.cof)
void jcp(const char* file) {
	// Pull the whole file into memory
	uchar *fdata = (uchar*)malloc(4200000);	// 4MB + header
	memset(fdata, 0, 4200000);
	int base = 0x4000, flen = 0, skip = 0;	// Defaults

	FILE *fp = fopen(file, "rb");
	if (NULL == fp || (flen = fread(fdata, 1, 4200000, fp)) < 256)
		bye("Couldn't read file\n");
	fclose(fp);

	// Check file header
	if (0x802000 == ENBIGEND(fdata+0x404)) {
		printf("Cart ROM:  ");
		base = 0x802000;
		skip = 0x2000;
	} else if (0x802000 == ENBIGEND(fdata+0x604)) {
		printf("Cart ROM + 512:  ");
		base = 0x802000;
		skip = 0x2200;
	} else if ((fdata[0] == 0x01) && (fdata[1] == 0x50)) {
		printf("COFF File:  ");
		base = ENBIGEND(fdata+56);
		skip = ENBIGEND(fdata+68);
	} else if ((fdata[0] == 0x7f) && (fdata[1] == 'E') && (fdata[2] == 'L') && (fdata[3] == 'F')) {
		printf("ELF File:  ");
		if ((fdata[5] != 0x2) || (0x20004 != ENBIGEND(fdata+0x10)))
			bye("Not 68K executable.\n");

		skip = base = ENBIGEND(fdata+0x18);
		flen = 0;
	
		// Map all the sections into a new memory image.
		int secs = HALFBIGEND(fdata+0x30), seclen = HALFBIGEND(fdata+0x2e);
		uchar *img = (uchar*)malloc(2048*1024), *secptr = fdata+ENBIGEND(fdata+0x20);
		memset(img, 0, 2048*1024);
		while (secs-- >= 0) {
			int sadr = ENBIGEND(secptr+0xc), slen = ENBIGEND(secptr+0x14);
			uchar* fptr = fdata+ENBIGEND(secptr+0x10);
			if (0 != sadr) {		// 0 is debug info, so ignore it
				if (sadr < base)
					bye("Section has base address below entry point.  See readelf for details.");
				if (sadr+slen > flen)
					flen = sadr+slen;
				if (flen >= 2048*1024 || base < 0)
					bye("Section falls outside Jaguar memory.  See readelf for details.");
				if (1 == ENBIGEND(secptr+0x4))	// Progbits, so copy them
					memcpy(img+sadr, fptr, slen);
			}
			secptr+=seclen;
		}
		free(fdata);	// Point to the newly created memory image
		fdata = img;
	}

	flen -= skip;
	uchar *fptr = fdata + skip;

	if (flen > 4096) {
		flen = 4096;
		printf("WARNING WARNING WARNING:  TRUNCATED STUB TO 4096 BYTES!\n");
	}

	printf("Skip %d bytes, base addr is %x, sending %d bytes...", skip, base, flen);

	// Send the data to the Jaguar, 4064 bytes at a time
	uchar block[4080];
	memset(block, 0, 4080);
	int nextez = 0x1800, curbase = base, dotty=0;

	while (flen > 0) {
		// 'Fix' the byte order for the next block of file data
		for (int i = 0; i < 4064; i += 2) {
			block[i+1] = *fptr++;
			block[i] = *fptr++;
		}

		// Set up block trailer
		block[0xFE2] = curbase & 255;
		block[0xFE3] = (curbase >> 8) & 255;
		block[0xFE0] = (curbase >> 16) & 255;
		block[0xFE1] = (curbase >> 24) & 255;
		curbase += 4064;

		int start = (flen <= 4064) ? base : -1;
		block[0xFE6] = start & 255;
		block[0xFE7] = (start >> 8) & 255;
		block[0xFE4] = (start >> 16) & 255;
		block[0xFE5] = (start >> 24) & 255;

		block[0xFE8] = 0;
		block[0xFE9] = nextez >> 8;
		nextez = (0x1800 == nextez) ? 0x2800 : 0x1800;

		int len = ((flen <= 4064) ? (flen+3)>>2 : 4064>>2) - 1;
		block[0xFEA] = len & 255;
		block[0xFEB] = (len >> 8) & 255;
		flen -= 4064;

		// Wait for the block to come free (handshake with 68K).
		short poll=5;
		do {
			assert(usb_control_msg(udev, 0xC0, 0xff, 4, nextez+0xFEA, 
				(char*)&poll, 2, 1000) == 2);
		} while (-1 != poll);

		printf("Send a block\n");
		// Send off the finished block.
		assert(usb_control_msg(udev, 0x40, 0xfe, 4080, nextez, 
			(char*)block, 4080, 1000) == 4080);
	}

	printf("Installed!\n");

	// Clean up
	free(fdata);
}

/* this function takes the 68000 exception number and attempts to
   translate this number into a unix compatible signal value */
int computeSignal (int exceptionVector)
{
  int sigval;
  switch (exceptionVector)
    {
    case 2:
      sigval = 10;
      break;			/* bus error           */
    case 3:
      sigval = 10;
      break;			/* address error       */
    case 4:
      sigval = 4;
      break;			/* illegal instruction */
    case 5:
      sigval = 8;
      break;			/* zero divide         */
    case 6:
      sigval = 8;
      break;			/* chk instruction     */
    case 7:
      sigval = 8;
      break;			/* trapv instruction   */
    case 8:
      sigval = 11;
      break;			/* privilege violation */
    case 9:
      sigval = 5;
      break;			/* trace trap          */
    case 10:
      sigval = 4;
      break;			/* line 1010 emulator  */
    case 11:
      sigval = 4;
      break;			/* line 1111 emulator  */

      /* Coprocessor protocol violation.  Using a standard MMU or FPU
         this cannot be triggered by software.  Call it a SIGBUS.  */
    case 13:
      sigval = 10;
      break;

    case 1:
      sigval = 2;
      break;			/* interrupt           */
    case 0:
      sigval = 5;
      break;			/* breakpoint          */

      /* This is a trap #8 instruction.  Apparently it is someone's software
         convention for some sort of SIGFPE condition.  Whose?  How many
         people are being screwed by having this code the way it is?
         Is there a clean solution?  */
    case 40:
      sigval = 8;
      break;			/* floating point err  */

    case 48:
      sigval = 8;
      break;			/* floating point err  */
    case 49:
      sigval = 8;
      break;			/* floating point err  */
    case 50:
      sigval = 8;
      break;			/* zero divide         */
    case 51:
      sigval = 8;
      break;			/* underflow           */
    case 52:
      sigval = 8;
      break;			/* operand error       */
    case 53:
      sigval = 8;
      break;			/* overflow            */
    case 54:
      sigval = 8;
      break;			/* NAN                 */
    default:
      sigval = 7;		/* "software generated" */
    }
  return (sigval);
}

/* Daikin conditioner simulator for S21 protocol testing */

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <ctype.h>
#include <popt.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#include "main/daikin_s21.h"

#ifdef WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

int debug = 0; // Dump commands and responses (short form)
int dump  = 0; // Raw dump

// Simulated A/C state. Defaults are chosen to be distinct; can be changed via
// command line.
int   power       = 0;    // Power on
int   mode        = 3;    // Mode
float temp        = 22.5; // Set point
int   fan         = 3;    // Fan speed
int   swing       = 0;    // Swing direction
int   powerful    = 0;    // Powerful mode
int   eco         = 0;    // Eco mode
int   home        = 245;  // Reported temparatures (multiplied by 10 here)
int   outside     = 205;
int   inlet       = 185;
int   fanrpm      = 52;   // Fan RPM (divided by 10 here)
int   comprpm     = 42;   // Compressor RPM
int   protocol    = 2; // Protocol version
const char *model = "135D"; // Reported A/C model code. Default taken from FTXF20D5V1B

static void hexdump_raw(const unsigned char *buf, unsigned int len)
{
   for (int i = 0; i < len; i++)
      printf(" %02X", buf[i]);
   printf("\n");
}

static void hexdump(const char *header, const unsigned char *buf, unsigned int len)
{
   if (dump) {
      printf("%s:", header);
      hexdump_raw(buf, len);
   }
}

static void serial_write(int p, const unsigned char *response, unsigned int pkt_len)
{
   int l;

   hexdump("Tx", response, pkt_len);

   l = write(p, response, pkt_len);

   if (l < 0) {
	  perror("Serial write failed");
	  exit(255);
   }
   if (l != pkt_len) {
	  fprintf(stderr, "Serial write failed; %d bytes instead of %d\n", l, pkt_len);
	  exit(255);
   }
}

static void s21_nak(int p, unsigned char *buf)
{
   static unsigned char response = NAK;

   printf(" -> Unknown command %c%c, sending NAK\n", buf[S21_CMD0_OFFSET], buf[S21_CMD1_OFFSET]);
   serial_write(p, &response, 1);
   
   buf[0] = 0; // Clear read buffer
}

static void s21_ack(int p)
{
   static unsigned char response = ACK;

   serial_write(p, &response, 1);
}

static void s21_nonstd_reply(int p, unsigned char *response, int body_len)
{
   int pkt_len = S21_FRAMING_LEN + body_len;
   int l;

   s21_ack(p); // Send ACK before the reply

   // Make a proper framing
   response[S21_STX_OFFSET] = STX;
   response[S21_CMD0_OFFSET + body_len] = s21_checksum(response, pkt_len);
   response[S21_CMD0_OFFSET + body_len + 1] = ETX;

   serial_write(p, response, pkt_len);
}

static void s21_reply(int p, unsigned char *response, const unsigned char *cmd, int payload_len)
{
	response[S21_CMD0_OFFSET] = cmd[S21_CMD0_OFFSET] + 1;
    response[S21_CMD1_OFFSET] = cmd[S21_CMD1_OFFSET];

	s21_nonstd_reply(p, response, 2 + payload_len); // Body is two cmd bytes plus payload
}

// A wrapper for unknown command. Useful because we're adding them in bulk
static void unknown_cmd(int p, unsigned char *response, const unsigned char *cmd,
                        unsigned char r0, unsigned char r1, unsigned char r2, unsigned char r3)
{
   if (debug)
      printf(" -> unknown ('%c%c') = 0x%02X 0x%02X 0x%02X 0x%02X\n",
	         cmd[S21_CMD0_OFFSET], cmd[S21_CMD1_OFFSET], r0, r1, r2, r3);
   response[3] = r0;
   response[4] = r1;
   response[5] = r2;
   response[6] = r3;

   s21_reply(p, response, cmd, S21_PAYLOAD_LEN);
}

static void send_temp(int p, unsigned char *response, const unsigned char *cmd, int value, const char *name)
{
	char buf[5];
	
	snprintf(buf, sizeof(buf), "%+d", value);
	if (debug)
	   printf(" -> %s = %s\n", name, buf);

    // A decimal value from sensor is sent as ASCII value with sign,
	// spelled backwards for some reason. One decimal place is assumed.
	response[S21_PAYLOAD_OFFSET + 0] = buf[3];
	response[S21_PAYLOAD_OFFSET + 1] = buf[2];
	response[S21_PAYLOAD_OFFSET + 2] = buf[1];
	response[S21_PAYLOAD_OFFSET + 3] = buf[0];
	
	s21_reply(p, response, cmd, S21_PAYLOAD_LEN);
}

static void send_int(int p, unsigned char *response, const unsigned char *cmd, int value, const char *name)
{
	char buf[4];

	snprintf(buf, sizeof(buf), "%03d", value);
	if (debug)
	   	printf(" -> %s = %s\n", name, buf);

	// Order inverted, the same as in send_temp()
    response[S21_PAYLOAD_OFFSET + 0] = buf[2];
	response[S21_PAYLOAD_OFFSET + 1] = buf[1];
	response[S21_PAYLOAD_OFFSET + 2] = buf[0];
			
	s21_reply(p, response, buf, 3); // Nontypical response, 3 bytes, not 4!
}

int
main(int argc, const char *argv[])
{
   const char     *port = NULL;
   poptContext     optCon;
   const struct poptOption optionsTable[] = {
	  {"port", 'p', POPT_ARG_STRING, &port, 0, "Port", "/dev/cu.usbserial..."},
	  {"debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug"},
	  {"on", 0, POPT_ARG_NONE, &power, 0, "Power on"},
	  {"mode", 0, POPT_ARG_INT, &mode, 0, "Mode", "0=F,1=H,2=C,3=A,7=D"},
	  {"fan", 0, POPT_ARG_INT, &fan, 0, "Fan", "0 = auto, 1-5 = set speed, 6 = quiet"},
	  {"temp", 0, POPT_ARG_FLOAT, &temp, 0, "Temp", "C"},
	  {"comprpm", 0, POPT_ARG_INT, &fanrpm, 0, "Fan rpm (divided by 10)"},
	  {"comprpm", 0, POPT_ARG_INT, &comprpm, 0, "Compressor rpm"},
	  {"powerful", 0, POPT_ARG_NONE, &powerful, 0, "Debug"},
	  {"dump", 'V', POPT_ARG_NONE, &dump, 0, "Dump"},
	  {"protocol", 0, POPT_ARG_INT, &protocol, 0, "Reported protocol version"},
	  {"model", 0, POPT_ARG_STRING, &model, 0, "Reported model code"},
	  POPT_AUTOHELP {}
   };

   optCon = poptGetContext(NULL, argc, argv, optionsTable, 0);
   //poptSetOtherOptionHelp(optCon, "");

   int             c;
   if ((c = poptGetNextOpt(optCon)) < -1) {
      fprintf(stderr, "%s: %s\n", poptBadOption(optCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
      exit(255);
   }

   if (poptPeekArg(optCon) || !port)
   {
      poptPrintUsage(optCon, stderr, 0);
      return -1;
   }
   poptFreeContext(optCon);

   if (!model || strlen(model) < 4) {
	  fprintf(stderr, "Invalid --model code given, 4 characters required");
	  return -1;
   }

   int p = open(port, O_RDWR);

   if (p < 0) {
      fprintf(stderr, "Cannot open %s: %s", port, strerror(errno));
	  exit(255);
   }
#ifdef WIN32
   DCB dcb = {0};
   
   dcb.DCBlength = sizeof(dcb);
   dcb.BaudRate  = CBR_2400;
   dcb.fBinary   = TRUE;
   dcb.fParity   = TRUE;
   dcb.ByteSize  = 8;
   dcb.Parity    = EVENPARITY;
   dcb.StopBits  = TWOSTOPBITS;
   
   if (!SetCommState((HANDLE)_get_osfhandle(p), &dcb)) {
      fprintf(stderr, "Failed to set port parameters\n");
	  exit(255);
   }
#else
   struct termios  t;
   if (tcgetattr(p, &t) < 0)
      err(1, "Cannot get termios");
   cfsetspeed(&t, 2400);
   t.c_cflag = CREAD | CS8 | PARENB | CSTOPB;
   if (tcsetattr(p, TCSANOW, &t) < 0)
      err(1, "Cannot set termios");
   usleep(100000);
   tcflush(p, TCIOFLUSH);
#endif

   unsigned char buf[256];
   unsigned char response[256];
   unsigned char chksum;

   buf[0] = 0;

   while (1)
   {
	  // Carry over STX from the previous iteration
	  int len = buf[0] == STX ? 1 : 0;

      while (len < sizeof(buf))
      {
         int l = read(p, buf + len, 1);

		 if (l < 0) {
		    perror("Error reading from serial port");
			exit(255);
		 }
		 if (l == 0)
		    continue;
		 if (len == 0 && *buf != STX) {
			printf("Garbage byte received: 0x%02X\n", *buf);
			continue;
		 }
         len += l;
		 if (buf[len - 1] == ETX)
			break;
      }
      if (!len)
         continue;
	 
	  hexdump("Rx", buf, len);

      chksum = s21_checksum(buf, len);
      if (chksum != buf[len - 2]) {
		 printf("Bad checksum: 0x%02X vs 0x%02X\n", chksum, buf[len - 2]);
		 buf[0] = 0; // Just silently drop the packet. My FTXF20D does this.
		 continue;
	  }

      if (debug)
         printf("Got command: %c%c\n", buf[1], buf[2]);

	  if (buf[1] == 'D') {
		 // Set value. No response expected, just ACK.
		 s21_ack(p);

		 switch (buf[2]) {
	     case '1':
		    power = buf[S21_PAYLOAD_OFFSET + 0] - '0'; // ASCII char
			mode  = buf[S21_PAYLOAD_OFFSET + 1] - '0'; // See AC_MODE_*
			temp  = s21_decode_target_temp(buf[S21_PAYLOAD_OFFSET + 2]);
			fan   = s21_decode_fan(buf[S21_PAYLOAD_OFFSET + 3]);

			printf(" Set power %d mode %d temp %.1f fan %d\n", power, mode, temp, fan);
			break;
		 case '5':
		    swing = buf[S21_PAYLOAD_OFFSET + 0] - '0'; // ASCII char
			// Payload offset 1 equals to '?' for "on" and '0' for "off
			// Payload offset 2 and 3 are always '0', seem unused

			printf(" Set swing %d spare bytes", swing);
			hexdump_raw(&buf[S21_PAYLOAD_OFFSET + 1], S21_PAYLOAD_LEN - 1);
			break;
		 case '6':
		    powerful = buf[S21_PAYLOAD_OFFSET + 0] == '2'; // '2' or '0'
			// My Daichi controller always sends 'D6 0 0 0 0' for 'Eco',
			// both on and off. Bug or feature ?

			printf(" Set powerful %d spare bytes", powerful);
			hexdump_raw(&buf[S21_PAYLOAD_OFFSET + 1], S21_PAYLOAD_LEN - 1);
			break;
		 default:
            printf(" Set unknown:");
		    hexdump_raw(buf, len);
		    break;
		 }

	     buf[0] = 0;
		 continue;
	  }

      if (buf[1] == 'F') {
		 // Query control settings
		 switch (buf[2]) {
	     case '1':
		    if (debug)
		       printf(" -> power %d mode %d temp %.1f\n", power, mode, temp);
		    response[3] = power + '0'; // sent as ASCII
			response[4] = mode + '0';
			// 18.0 + 0.5 * (signed) (payload[2] - '@')
			response[5] = s21_encode_target_temp(temp);
			response[6] = s21_encode_fan(fan);

			s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		 case '2':
		    // BRP069B41 sends this as first command. If NAK is received, it keeps retrying
			// and doesn't send anything else. Suggestion - query AC features
			// The response values here are kindly provided by a user in reverse engineering
			// thread: https://github.com/revk/ESP32-Faikin/issues/408#issuecomment-2278296452
			// Correspond to A/C models CTXM60RVMA, CTXM35RVMA
			// It was experimentally found that with different values, given by FTXF20D, the
			// controller falls into error 252 and refuses to accept A/C commands over HTTP.
			// FTXF20D: 34 3A 00 80
			unknown_cmd(p, response, buf, 0x3D, 0x3B, 0x00, 0x80);
			break;
		 case '3':
		    if (debug)
		       printf(" -> powerful ('F3') %d\n", powerful);
			response[3] = 0x30; // No idea what this is, taken from my FTXF20D
			response[4] = 0xFE;
			response[5] = 0xFE;
			response[6] = powerful ? 2 : 0;

		    s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		 case '4':
		    // Also taken from CTXM60RVMA, CTXM35RVMA, and also error 252 if wrong
			// FTXF20D: 30 00 A0 30
			unknown_cmd(p, response, buf, 0x30, 0x00, 0x80, 0x30);
			break;
		 case '5':
		    if (debug)
		       printf(" -> swing %d\n", swing);
		    response[3] = swing;
			response[4] = 0;
			response[5] = 0;
			response[6] = 0;

			s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		 case '6':
		    if (debug)
		       printf(" -> powerful ('F6') %d\n", powerful);
		    response[3] = powerful ? 2 : 0;
			response[4] = 0;
			response[5] = 0;
			response[6] = 0;

			s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		 case '7':
		    if (debug)
		       printf(" -> eco %d\n", eco);
		    response[3] = 0;
			response[4] = eco ? '2' : '0';
			response[5] = 0;
			response[6] = 0;

			s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		case '8':
		    if (debug)
		       printf(" -> Protocol version = %d\n", protocol);
			// 'F8' - this is found out to be protocol version.
			// My FTXF20D replies with '0020' (assuming reading in reverse like everything else).
			// If we say that, BRP069B41 then asks for F9 (we know it's different form of home/outside sensor)
			// then proceeds requiring more commands, majority of english alphabet. I got tired implementing
			// all of them and tried to downgrade the response to '0000'. This caused the controller sending
			// 'MM' command (see below), and then it goes online with our emulated A/C.
			// '0010' gives the same results
		    response[3] = '0';
			response[4] = '0' + protocol;
			response[5] = '0';

			s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		 case '9':
			// In debug log temperature values will appear multiplied by 2
		    response[3] = home / 5 + 0x80;
			response[4] = outside / 5 + 0x80; // This is from Faikin sources, but FTXF20D returnx 0xFF here
			response[5] = 0xFF; // Copied from FTFX20D
			response[6] = 0x30; // Copied from FTFX20D

		    if (debug)
		       printf(" -> home = 0x%02X (%.1f) outside = 0x%02X (%.1f)\n",
			          response[3], home / 10.0, response[4], outside / 10.0);

			s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		 case 'C':
		    // Protocol v2 - model code. Reported as "model=" in aircon/get_model_info.
			// One of few commands, which is only sent by controller once after bootup.
			// Even if communication is broken, then recovered (sim restarted), it won't
			// be sent again. Controller reboot would be required to accept the new value.
		 	if (debug)
		       printf(" -> model = %s\n", model);

		    response[3] = model[3];
			response[4] = model[2];
			response[5] = model[1];
			response[6] = model[0];

			s21_reply(p, response, buf, S21_PAYLOAD_LEN);
			break;
		 // All unknown_cmd's below are queried by BRP069B41 for protocol version 2.
		 // They are all mandatory; if we respond NAK, the controller keeps retrying
		 // this command and doesn't proceed.
		 // All response values are taken from FTXF20D
		 case 'B':
			unknown_cmd(p, response, buf, 0x30, 0x33, 0x36, 0x30); // 0630
			break;
		 case 'G':
			unknown_cmd(p, response, buf, 0x30, 0x34, 0x30, 0x30); // 0040
			break;
		 case 'K':
			unknown_cmd(p, response, buf, 0x71, 0x73, 0x35, 0x31); //15sq
			break;
		 case 'M':
			unknown_cmd(p, response, buf, 0x33, 0x42, 0x30, 0x30); //00B3
			break;
		 case 'N':
			unknown_cmd(p, response, buf, 0x30, 0x30, 0x30, 0x30); //0000
			break;
		 case 'P':
			unknown_cmd(p, response, buf, 0x37, 0x33, 0x30, 0x30); // 0037
			break;
		 case 'Q':
			unknown_cmd(p, response, buf, 0x45, 0x33, 0x30, 0x30); //003E
			break;
		 case 'R':
			unknown_cmd(p, response, buf, 0x30, 0x30, 0x30, 0x30); // 0000
			break;
		 case 'S':
			unknown_cmd(p, response, buf, 0x30, 0x30, 0x30, 0x30); // 0000
			break;
		 case 'T':
			unknown_cmd(p, response, buf, 0x31, 0x30, 0x30, 0x30); // 0001
			break;
		 case 'V':
		 	// This one is not sent by BRP069B41, but i quickly got tired of adding these
			// one by one and simply ran all the alphabet up to FZZ on my FTXF20D, so here it is.
			unknown_cmd(p, response, buf, 0x33, 0x37, 0x83, 0x30);
			break;
		 // BRP069B41 also sends 'FY' command, but accepts NAK and stops doing so.
		 // Therefore the command is optional. My FTXF20D also doesn't recognize it.
		 default:
		    // Respond NAK to an unknown command. My FTXF20D does the same.
		    s21_nak(p, buf);
		    continue;
		 }
	  } else if (buf[S21_CMD0_OFFSET] == 'M') {
		if (debug)
		    printf(" -> unknown ('MM')\n");
		// This is sent by BRP069B41 and response is mandatory. The controller
		// loops forever if NAK is received.
		// I experimentally found out that this command doesn't have a second
		// byte, and the A/C always responds with this. Note non-standard
		// response form.
		response[S21_CMD0_OFFSET] = 'M';
		response[2] = 'F';
		response[3] = 'F';
		response[4] = 'F';
		response[5] = 'F';

		s21_nonstd_reply(p, response, 5);
	  } else if (buf[S21_CMD0_OFFSET] == 'R') {
		 // Query sensors
		 switch (buf[S21_CMD1_OFFSET]) {
	     case 'H':
		    send_temp(p, response, buf, home, "home");
		    break;
	     case 'I':
		    send_temp(p, response, buf, inlet, "inlet");
		    break;
	     case 'a':
		    send_temp(p, response, buf, outside, "outside");
		    break;
	     case 'L':
		 	send_int(p, response, buf, fanrpm, "fanrpm");
		    break;
		 case 'd':
		 	send_int(p, response, buf, comprpm, "compressor rpm");
			break;
	     case 'N':
		 	// These two are queried by BRP069B41, at least for protocol version 1, but we have no idea
			// what they mean. Not found anywhere in controller's http responses. We're replying with
			// some distinct values for possible identification in case if they pop up somewhere.
			// The following is what my FTX20D returns, also with known commands from above, for comparison:
			// {"protocol":"S21","dump":"0253483035322B5D03","SH":"052+"} - home
			// {"protocol":"S21","dump":"0253493535322B6303","SI":"552+"} - inlet
			// {"protocol":"S21","dump":"0253613035312B7503","Sa":"051+"} - outside
			// {"protocol":"S21","dump":"02534E3532312B6403","SN":"521+"} - ???
			// {"protocol":"S21","dump":"0253583033322B6B03","SX":"032+"} - ???
		    send_temp(p, response, buf, 235, "unknown ('RN')");
		    break;
	     case 'X':
		    send_temp(p, response, buf, 215, "unknown ('RX')");
		    break;
		 default:
		    s21_nak(p, buf);
		    continue;
		 }
	  } else {
		  s21_nak(p, buf);
		  continue;
	  }

      // We are here if we just have sent a reply. The controller must ACK it.

	  do {
         len = read(p, buf, 1);

         if (len < 0) {
		    perror("Error reading from serial port");
		    exit(255);
	     }
	  } while (len != 1);

      hexdump("Rx", buf, 1);

	  if (debug && buf[0] != ACK) {
		 printf("Protocol error: expected ACK, got 0x%02X\n", buf[0]);
	  }
	  // My Daichi cloud controller doesn't send this ACK.
	  // After a small delay it simply sends a next packet
	  if (buf[0] == STX) {
		 if (debug)
		    printf("The controller didn't ACK our response, next frame started!\n");
	  } else {
		 buf[0] = 0;
	  }
   }
   return 0;
}

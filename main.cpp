#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <hamlib/rig.h>
#include "INIReader.cpp"

// DEFINES GO HERE
#define VERSION "0.1"			// program version for messages, etc
#define PACKET_DEST "APMGT1"	// packet tocall

using namespace std;

// GLOBAL VARS GO HERE
string mycall;					// callsign we're operating under, excluding ssid
char myssid;					// ssid of this station (stored as a number, not ascii)
bool verbose = false;			// did the user ask for verbose mode?
bool gps_debug = false;			// did the user ask for gps debug info?
bool tnc_debug = false;			// did the user ask for tnc debug info?
int kiss_iface = -1;			// tnc serial port
int gps_iface = -1;				// gps serial port
bool beacon_ok = false;			// should we be sending beacons?
float pos_lat;					// current latitude - positive N, negative S
float pos_long;					// current longitude - positive W, negative E
struct tm * gps_time = new tm;	// last time received from the gps (if enabled)
int static_beacon_rate;			// how often (in seconds) to send a beacon
vector<string> path_calls;		// path callsigns
vector<char> path_ssids;		// path ssids

// BEGIN FUNCTIONS
void find_and_replace(string& subject, const string& search, const string& replace) {	// find and replace in a string, thanks Czarek Tomczak
	size_t pos = 0;
	while((pos = subject.find(search, pos)) != string::npos) {
		subject.replace(pos, search.length(), replace);
		pos += replace.length();
	}
}

int get_baud(int baudint) {		// return a baudrate code from the baudrate int
	switch (baudint) {
	case 0:
		return B0;
	case 50:
		return B50;
	case 75:
		return B75;
	case 100:
		return B110;
	case 134:
		return B134;
	case 150:
		return B150;
	case 200:
		return B200;
	case 300:
		return B300;
	case 600:
		return B600;
	case 1200:
		return B1200;
	case 1800:
		return B1800;
	case 2400:
		return B2400;
	case 4800:
		return B4800;
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
	default:
		return -1;
	}
}	// END OF 'get_baud'

int open_port(string port, int baud_code) {					// open a serial port
	struct termios options;
	int iface = open(port.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);	// open the port
	if (iface == -1) return -1;								// failed to open port
	tcgetattr(iface, &options);								// get current control options for the port
	cfsetispeed(&options, baud_code);						// set in baud rate
	cfsetospeed(&options, baud_code);						// set out baud rate
	options.c_cflag &= ~PARENB;								// no parity
	options.c_cflag &= ~CSTOPB;								// 1 stop bit
	options.c_cflag &= ~CSIZE;								// turn off 'csize'
	options.c_cflag |= CS8;									// 8 bit data
	options.c_cflag |= (CLOCAL | CREAD);					// enable the receiver and set local mode
	options.c_lflag = ICANON;								// canonical mode
	options.c_oflag &= ~OPOST;								// raw output
	tcsetattr(iface, TCSANOW, &options);					// set the new options for the port
	fcntl(iface, F_SETFL, 0);								// set port to blocking reads
	return iface;											// return the file number
}	// END OF 'open_port'

void init(int argc, char* argv[]) {		// read config, set up serial ports, etc
	string configfile = "/etc/aprstoolkit.conf";

// COMMAND LINE ARGUMENT PARSING
	
	if (argc > 1) {		// user entered command line arguments
		int c;
		opterr = 0;
		while ((c = getopt (argc, argv, "c:vz:")) != -1)		// loop through all command line args
			switch (c) {
			case 'c':		// user specified a config file
				configfile = optarg;
				break;
			case 'v':
				verbose = true;	// user wants to hear about what's going on
				break;
			case 'z':		// user wants debug info	TODO: should this be documented?
				if (strcmp(optarg, "gps") == 0) gps_debug = true;
				if (strcmp(optarg, "tnc") == 0) tnc_debug = true;
				break;
			case '?':		// can't understand what the user wants from us, let's set them straight
				fprintf(stderr, "Usage: aprstoolkit [-v] [-c CONFIGFILE]\n\n");
				fprintf(stderr, "Options:\n -v\tbe verbose\n -c\tspecify config file\n -?\tshow this help\n");
				exit (EXIT_FAILURE);
				break;
			}
	}

	if (verbose) printf("APRS Toolkit %s\n\n", VERSION);

// CONFIG FILE PARSING

	INIReader readconfig(configfile);	// read the config ini

	if (readconfig.ParseError() < 0) {	// if we couldn't parse the config
		fprintf(stderr, "Error loading %s\n", configfile.c_str());
		exit (EXIT_FAILURE);
	}

	if (verbose) printf("Using config file %s\n", configfile.c_str());

	string call = readconfig.Get("station", "mycall", "N0CALL");	// parse the mycall config paramater
	int index = call.find_first_of("-");
	if (index == -1) {	// no ssid specified
		mycall = call;
		myssid = 0;
	} else {			// ssid was specified
		mycall = call.substr(0,index);	// left of the dash
		myssid = atoi(call.substr(index+1,2).c_str());	// right of the dash
	}
	if (mycall.length() > 6) {
		fprintf(stderr,"MYCALL: Station callsign must be 6 characters or less.\n");
		exit (EXIT_FAILURE);
	}
	if (myssid < 0 or myssid > 15) {
		fprintf(stderr,"MYCALL: Station SSID must be between 0 and 15.\n");
		exit (EXIT_FAILURE);
	}
	if (verbose) printf("Operating as %s-%i\n", mycall.c_str(), myssid);		// whew, we made it through all the tests

	bool gps_enable = readconfig.GetBoolean("gps", "enable", false);
	string kiss_port = readconfig.Get("tnc", "port", "/dev/ttyS0");
	int kiss_baud = readconfig.GetInteger("tnc", "baud", 9600);
	string gps_port  = readconfig.Get("gps", "port", "/dev/ttyS1");
	int gps_baud = readconfig.GetInteger("gps", "baud", 4800);
	static_beacon_rate = readconfig.GetInteger("beacon", "static_rate", 900);

	string beacon_via_str = readconfig.Get("beacon", "via", "");		// now we get to parse the via paramater
	int current;
	int next = -1;
	string this_call;
	int this_ssid;
	do {
		current = next + 1;
		next = beacon_via_str.find_first_of(",", current);
		call = beacon_via_str.substr(current, next-current);
		index = call.find_first_of("-");
			if (index == -1) {	// no ssid specified
				this_call = call;
				this_ssid = 0;
			} else {			// ssid was specified
				this_call = call.substr(0,index);	// left of the dash
				this_ssid = atoi(call.substr(index+1,2).c_str());	// right of the dash
			}
			if (this_call.length() > 6) {
				fprintf(stderr,"VIA: Station callsign must be 6 characters or less.\n");
				exit (EXIT_FAILURE);
			}
			if (this_ssid < 0 or myssid > 15) {
				fprintf(stderr,"VIA: Station SSID must be between 0 and 15.\n");
				exit (EXIT_FAILURE);
			}
			path_calls.push_back(this_call);		// input validation ok, add this to the via vectors
			path_ssids.push_back(this_ssid);
	} while (next != -1);
	if (path_calls.size() > 8) {
		fprintf(stderr,"VIA: Cannot have more than 8 digis in the path.\n");
		exit (EXIT_FAILURE);
	}
// OPEN KISS INTERFACE

	// no 'if' here, since this would be pointless without a TNC

	int baud_code = get_baud(kiss_baud);

	if (baud_code == -1) {		// get_baud says that's an invalid baud rate
		fprintf(stderr, "Invalid KISS baud rate %i\n", kiss_baud);
		exit (EXIT_FAILURE);
	}

	kiss_iface = open_port(kiss_port, baud_code);

	if (kiss_iface == -1) {		// couldn't open the serial port...
		fprintf(stderr, "Could not open KISS port %s\n", kiss_port.c_str());
		exit (EXIT_FAILURE);
	}

	if (verbose) printf("Successfully opened KISS port %s at %i baud\n", kiss_port.c_str(), kiss_baud);

// OPEN GPS INTERFACE

	if (gps_enable) {
		baud_code = get_baud(gps_baud);

		if (baud_code == -1) {
			fprintf(stderr, "Invalid GPS baud rate %i\n", gps_baud);
			exit (EXIT_FAILURE);
		}

		gps_iface = open_port(gps_port, baud_code);

		if (gps_iface == -1) {
			fprintf(stderr, "Could not open GPS port %s\n", gps_port.c_str());
			exit (EXIT_FAILURE);
		}

		if (verbose) printf("Successfully opened GPS port %s at %i baud\n", gps_port.c_str(), gps_baud);
	}
	if (verbose) printf("Init finished!\n\n");
}	// END OF 'init'

string ax25_callsign(const char* callsign) {		// pad a callsign with spaces to 6 chars and shift chars to the left
	char paddedcallsign[] = {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00};	// start with a string of spaces
	memcpy(paddedcallsign, callsign, strlen(callsign));		// copy the shifted callsign into our padded container
	for (int i=0;i<6;i++) {
			paddedcallsign[i] <<= 1;		// shift all the chars in the input callsign
		}
	return paddedcallsign;
}	// END OF 'ax25_callsign'

char ax25_ssid(char ssid, bool hbit, bool last) {
	ssid <<= 1;			// shift ssid
	if (hbit) {			// set h and c bits
		ssid |= 0xE0;	// 11100000
	} else {
		ssid |= 0x60;	// 01100000
	}
	ssid |= last;		// set address end bit
	return ssid;
}

void send_kiss_frame(char* source, int source_ssid, char* destination, int destination_ssid, vector<string> via, vector<int>via_ssids, vector<bool>via_hbits, string payload) {		// send a KISS packet to the TNC
	// we'll build the ax25 frame before adding the kiss encapsulation
	string buff = ax25_callsign(destination);					// add destination address
	buff.append(1, ax25_ssid(destination_ssid, false, false));	// add destination ssid
	buff.append(ax25_callsign(source));							// add source address
	if (via.size() == 0) {
		buff.append(1, ax25_ssid(source_ssid, false, true));	// no path, add source ssid and end the address field
	} else {
		buff.append(1, ax25_ssid(source_ssid, false, false));	// path to follow, don't end the address field just yet
		for (int i=0;i<(via.size()-1);i++) {					// loop thru all via calls except the last
			buff.append(ax25_callsign(via[i].c_str()));			// add this via callsign
			buff.append(1, ax25_ssid(via_ssids[i], via_hbits[i], false)); // add this via ssid
		}
		buff.append(ax25_callsign(via[via.size()-1].c_str()));	// finally made it to the last via call
		buff.append(1,ax25_ssid(via_ssids[via_ssids.size()-1], via_hbits[via_hbits.size()-1], true));	// end the address field
	}
	buff.append("\x03\xF0");									// add control and pid bytes (ui frame)
	buff.append(payload);										// add the actual data
	// now we can escape any FENDs and FESCs that appear in the ax25 frame and add kiss encapsulation
	find_and_replace(buff,"\xC0","\xDB\xDC");					// replace any FENDs with FESC,TFEND
	find_and_replace(buff,"\xDB","\xDB\xDD");					// replace any FESCs with FESC,TFESC
	buff.insert(0,"\xC0\x00");									// add kiss header
	buff.append(1,0xC0);										// add kiss footer
	write(kiss_iface,buff.c_str(),buff.length());				// spit this out the kiss interface
	if (tnc_debug) printf("%s-%i to %s-%i via %i digis: %s\n", source, source_ssid, destination, destination_ssid, via.size(), payload.c_str());
}	// END OF 'send_kiss_frame'

void* gps_thread(void*) {		// thread to listen to the incoming NMEA stream and update our position and time
	string buff = "";
	char * data = new char[1];

	while (true) {
		read(gps_iface, data, 1);
		if (data[0] == '\n') {			// NMEA data is terminated with a newline.
			if (buff.length() > 6) {	// but let's not bother if this was an incomplete line
				if (gps_debug) printf("GPS_IN: %s\n", buff.c_str());
				if (buff.compare(0, 6, "$GPRMC") == 0) {	// GPRMC has most of the info we care about
					string params[12];
					int current = 7;
					int next = 0;
					for (int a=0; a<12; a++) {		// here we'll split the fields of the GPRMC sentence
						next = buff.find_first_of(',', current);
						params[a] = buff.substr(current, next - current);
						current = next + 1;
					}
					if (params[1].compare("A") == 0) {
						beacon_ok = true;
						gps_time->tm_hour = atoi(params[0].substr(0,2).c_str());
						gps_time->tm_min = atoi(params[0].substr(2,2).c_str());
						gps_time->tm_sec = atoi(params[0].substr(4,2).c_str());
						gps_time->tm_mday = atoi(params[8].substr(0,2).c_str());
						gps_time->tm_mon = atoi(params[8].substr(2,2).c_str()) - 1;		// tm_mon is 0-11
						gps_time->tm_year = atoi(params[8].substr(4,2).c_str()) + 100; 	// tm_year is "years since 1900"
						pos_lat = atof(params[2].c_str());
						if (params[3].compare("S") == 0) pos_lat = pos_lat * -1;
						pos_long = atof(params[4].c_str());
						if (params[5].compare("E") == 0) pos_long = pos_long * -1;
						if (gps_debug) printf("GPS_DEBUG: Lat: %f Long: %f Time: %s", pos_lat, pos_long, asctime(gps_time));
					} else {
						beacon_ok = false;
						if (gps_debug) printf("GPS_DEBUG: data invalid.\n");
					}
				}
			}
			buff = "";		// clear the buffer
		} else {
			buff.append(1, data[0]);	// this wasn't a newline so just add it to the buffer.
		}
	}
	return 0;
} // END 'gps_thread'

void* beacon_thread(void*) {	// send a beacon periodically	TODO: this.
	while (true) {
		sleep(1);
	}
	return 0;
} // END 'beacon_thread'

int main(int argc, char* argv[]) {

	init(argc, argv);	// get everything ready to go

	if (gps_iface > 0) {	// start the gps interface thread if the gps interface was opened
		pthread_t gps_t;
		pthread_create(&gps_t, NULL, &gps_thread, NULL);
	}

	while(true) {	// let the threads do their thing
		sleep(600);
	}

	if (verbose) printf("Closing TNC interface\n");		// clean up
	close(kiss_iface);
	if (verbose) printf("Closing GPS interface\n");
	close(gps_iface);

	return 0;

}	// END OF 'main'

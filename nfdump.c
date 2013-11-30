/*
 *  nfdump : Reads netflow data from files, saved by nfcapd
 *  		 Data can be view, filtered and saved to 
 *  		 files.
 *
 *  Copyright (c) 2004, SWITCH - Teleinformatikdienste fuer Lehre und Forschung
 *  All rights reserved.
 *  
 *  Redistribution and use in source and binary forms, with or without 
 *  modification, are permitted provided that the following conditions are met:
 *  
 *   * Redistributions of source code must retain the above copyright notice, 
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice, 
 *     this list of conditions and the following disclaimer in the documentation 
 *     and/or other materials provided with the distribution.
 *   * Neither the name of SWITCH nor the names of its contributors may be 
 *     used to endorse or promote products derived from this software without 
 *     specific prior written permission.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 *  POSSIBILITY OF SUCH DAMAGE.
 *  
 *  $Author: peter $
 *
 *  $Id: nfdump.c 92 2007-08-24 12:10:24Z peter $
 *
 *  $LastChangedRevision: 92 $
 *	
 *
 */

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#include "nffile.h"
#include "nfnet.h"
#include "nf_common.h"
#include "netflow_v5_v7.h"
#include "rbtree.h"
#include "nftree.h"
#include "nfprof.h"
#include "nfdump.h"
#include "nfstat.h"
#include "version.h"
#include "launch.h"
#include "util.h"
#include "flist.h"
#include "panonymizer.h"

/* hash parameters */
#define HashBits 20
#define NumPrealloc 128000

#define AGGR_SIZE 7


/* Global Variables */
FilterEngine_data_t	*Engine;

/* Local Variables */
static char const *rcsid 		  = "$Id: nfdump.c 92 2007-08-24 12:10:24Z peter $";
static uint64_t total_bytes;
static uint32_t total_flows;
static uint32_t skipped_flows;
static time_t t_first_flow, t_last_flow;

extern const uint16_t MAGIC;
extern const uint16_t VERSION;

/*
 * Output formats:
 * User defined output formats can be compiled into nfdump, for easy access
 * The format has the same syntax as describe in nfdump(1) -o fmt:<format>
 *
 * A format description consists of a single line containing arbitrary strings
 * and format specifier as described below:
 *
 * 	%ts		// Start Time - first seen
 * 	%te		// End Time	- last seen
 * 	%td		// Duration
 * 	%pr		// Protocol
 * 	%sa		// Source Address
 * 	%da		// Destination Address
 * 	%sap	// Source Address:Port
 * 	%dap	// Destination Address:Port
 * 	%sp		// Source Port
 * 	%dp		// Destination Port
 * 	%sas	// Source AS
 * 	%das	// Destination AS
 * 	%in		// Input Interface num
 * 	%out	// Output Interface num
 * 	%pkt	// Packets
 * 	%byt	// Bytes
 * 	%fl		// Flows
 * 	%pkt	// Packets
 * 	%flg	// TCP Flags
 * 	%tos	// Tos
 * 	%bps	// bps - bits per second
 * 	%pps	// pps - packets per second
 * 	%bpp	// bps - Bytes perl package
 *
 * The nfdump standard output formats line, long and extended are defined as follows:
 */

#define FORMAT_line "%ts %td %pr %sap -> %dap %pkt %byt %fl"

#define FORMAT_long "%ts %td %pr %sap -> %dap %flg %tos %pkt %byt %fl"

#define FORMAT_extended "%ts %td %pr %sap -> %dap %flg %tos %pkt %byt %pps %bps %bpp %fl"

/* The appropriate header line is compiled automatically.
 *
 * For each defined output format a v6 long format automatically exists as well e.g.
 * line -> line6, long -> long6, extended -> extended6
 * v6 long formats need more space to print IP addresses, as IPv6 addresses are printed in full length,
 * where as in standard output format IPv6 addresses are condensed for better readability.
 * 
 * Define your own output format and compile it into nfdumnp:
 * 1. Define your output format string.
 * 2. Test the format using standard syntax -o "fmt:<your format>"
 * 3. Create a #define statement for your output format, similar than the standard output formats above.
 * 4. Add another line into the printmap[] struct below BEFORE the last NULL line for you format:
 *    { "formatname", format_special, FORMAT_definition, NULL },
 *   The first parameter is the name of your format as recognized on the command line as -o <formatname>
 *   The second parameter is always 'format_special' - the printing function.
 *   The third parameter is your format definition as defined in #define.
 *   The forth parameter is always NULL for user defined formats.
 * 5. Recompile nfdump
 */

// Assign print functions for all output options -o
// Teminated with a NULL record
struct printmap_s {
	char		*printmode;		// name of the output format
	printer_t	func;			// name of the function, which prints the record
	char		*Format;		// output format definition
	char		*HeaderLine;	// Header line for static output formats if needed. NULL otherwise
} printmap[] = {
	{ "raw",		format_file_block_record,  	NULL, 				NULL },
	{ "line", 		format_special,      		FORMAT_line, 		NULL },
	{ "long", 		format_special, 			FORMAT_long, 		NULL },
	{ "extended",	format_special, 			FORMAT_extended,	NULL },
	{ "pipe", 		flow_record_to_pipe,      	NULL, 				NULL },
// add your formats here

// This is always the last line
	{ NULL,			NULL,                       NULL,				NULL }
};

#define DefaultMode "line"

// compare at most 16 chars
#define MAXMODELEN	16	

// all records should be version 5
#define FLOW_VERSION 5

/* exported fuctions */
void LogError(char *format, ...);

/* Function Prototypes */
static void usage(char *name);

static int ParseAggregateMask( char *arg, uint64_t *AggregateMasks, uint16_t *Aggregate_bits );

static int ParseCryptoPAnKey ( char *s, char *key );

static void PrintSummary(stat_record_t *stat_record, int plain_numbers);

static stat_record_t process_data(char *wfile, int element_stat, int flow_stat, int sort_flows,
	printer_t print_header, printer_t print_record, time_t twin_start, time_t twin_end, 
	uint64_t limitflows, uint64_t *AggregateMasks, int anon, int tag, int zero_flows);

/* Functions */
static void usage(char *name) {
		printf("usage %s [options] [\"filter\"]\n"
					"-h\t\tthis text you see right here\n"
					"-V\t\tPrint version and exit.\n"
					"-a\t\tAggregate netflow data.\n"
					"-A <expr>[/net]\tHow to aggregate: ',' sep list of 'srcip dstip srcport dstport'\n"
					"\t\tor subnet aggregation: srcip4/24, srcip6/64.\n"
					"-r\t\tread input from file\n"
					"-w\t\twrite output to file\n"
					"-f\t\tread netflow filter from file\n"
					"-n\t\tDefine number of top N. \n"
					"-c\t\tLimit number of records to display\n"
					"-S\t\tGenerate netflow statistics info.\n"
					"-s <expr>[/<order>]\tGenerate statistics for <expr>: \n"
					"-N\t\tPrint plain numbers in summary line\n"
					"\t\tsrcip, dstip, ip, srcport, dstport, port, srcas, dstas, as, inif, outif, proto\n"
					"\t\tand ordered by <order>: packets, bytes, flows, bps pps and bpp.\n"
					"-q\t\tQuiet: Do not print the header and bottom stat lines.\n"
					"-z\t\tZero flows - dumpfile contains only statistics record.\n"
					"-l <expr>\tSet limit on packets for line and packed output format.\n"
					"-K <key>\tAnonymize IP addressses using CryptoPAn with key <key>.\n"
					"\t\tkey: 32 character string or 64 digit hex string starting with 0x.\n"
					"-L <expr>\tSet limit on bytes for line and packed output format.\n"
					"-M <expr>\tRead input from multiple directories.\n"
					"-I \t\tPrint netflow summary statistics info from file, specified by -r.\n"
					"\t\t/dir/dir1:dir2:dir3 Read the same files from '/dir/dir1' '/dir/dir2' and '/dir/dir3'.\n"
					"\t\treqquests either -r filename or -R firstfile:lastfile without pathnames\n"
					"-m\t\tPrint netflow data date sorted. Only useful with -M\n"
					"-R <expr>\tRead input from sequence of files.\n"
					"\t\t/any/dir  Read all files in that directory.\n"
					"\t\t/dir/file Read all files beginning with 'file'.\n"
					"\t\t/dir/file1:file2: Read all files from 'file1' to file2.\n"
					"-o <mode>\tUse <mode> to print out netflow records:\n"
					"\t\t raw      Raw record dump.\n"
					"\t\t line     Standard output line format.\n"
					"\t\t long     Standard output line format with additional fields.\n"
					"\t\t extended Even more information.\n"
					"\t\t pipe     '|' separated, machine parseable output format.\n"
					"\t\t\tmode may be extended by '6' for full IPv6 listing. e.g.long6, extended6.\n"
					"-X\t\tDump Filtertable and exit (debug option).\n"
					"-Z\t\tCheck filter syntax and exit.\n"
					"-t <time>\ttime window for filtering packets\n"
					"\t\tyyyy/MM/dd.hh:mm:ss[-yyyy/MM/dd.hh:mm:ss]\n", name);
} /* usage */

/* 
 * some modules are needed for daemon code as well as normal stdio code 
 * therefore a generic LogError is defined, which maps in this case
 * to stderr
 */
void LogError(char *format, ...) {
va_list var_args;

	va_start(var_args, format);
	vfprintf(stderr, format, var_args);
	va_end(var_args);

} // End of LogError


static int ParseAggregateMask( char *arg, uint64_t *AggregateMasks, uint16_t *Aggregate_bits ) {
char *p, *q;
uint32_t	subnet;

	// preset Aggregate Mask array, for those fields not affected
	AggregateMasks[0] = ~( MaskSrcPort | MaskDstPort );
	AggregateMasks[5] = ~( MaskSrcAS | MaskDstAS );
	AggregateMasks[5] = ~( MaskSrcAS | MaskDstAS );
	AggregateMasks[6] = ~MaskProto;
	*Aggregate_bits 	  = 0;

	subnet = 0;
	p = strtok(arg, ",");
	while ( p ) {
		q = strchr(p, '/');
		if ( q ) {
			*q = 0;
			subnet = atoi(q+1);
		} 
		if ( strcasecmp(p, "srcip4" ) == 0 ) {
			AggregateMasks[1] = 0xffffffffffffffffLL;
			AggregateMasks[2] = 0xffffffffffffffffLL;
			if ( q ) {
				if ( subnet < 1 || subnet > 32 ) {
					fprintf(stderr, "Subnet specifier '%s' out of range for IPv4\n", q+1);
					return 0;
				}
				AggregateMasks[2] = 0xffffffffffffffffLL << ( 32 - subnet );
			}
			*Aggregate_bits |= Aggregate_SRCIP;
		} else if ( strcasecmp(p, "dstip4" ) == 0 ) {
			AggregateMasks[3] = 0xffffffffffffffffLL;
			AggregateMasks[4] = 0xffffffffffffffffLL;
			if ( q ) {
				if ( subnet < 1 || subnet > 32 ) {
					fprintf(stderr, "Subnet specifier '%s' out of range for IPv4\n", q+1);
					return 0;
				}
				AggregateMasks[4] = 0xffffffffffffffffLL << ( 32 - subnet );
			}
			*Aggregate_bits |= Aggregate_DSTIP;
		} else if ( strcasecmp(p, "srcip6" ) == 0 ) {
			AggregateMasks[1] = 0xffffffffffffffffLL;
			AggregateMasks[2] = 0xffffffffffffffffLL;
			if ( q ) {
				if ( subnet < 1 || subnet > 128 ) {
					fprintf(stderr, "Subnet specifier '%s' out of range for IPv6\n", q+1);
					return 0;
				}
				if ( subnet > 64 ) {
					AggregateMasks[2] = 0xffffffffffffffffLL << ( 128 - subnet );
				} else {
					AggregateMasks[1] = 0xffffffffffffffffLL << ( 64 - subnet );
					AggregateMasks[2] = 0;
				}
			}
			*Aggregate_bits |= Aggregate_SRCIP;
		} else if ( strcasecmp(p, "dstip6" ) == 0 ) {
			AggregateMasks[3] = 0xffffffffffffffffLL;
			AggregateMasks[4] = 0xffffffffffffffffLL;
			if ( q ) {
				if ( subnet < 1 || subnet > 128 ) {
					fprintf(stderr, "Subnet specifier '%s' out of range for IPv6\n", q+1);
					return 0;
				}
				if ( subnet > 64 ) {
					AggregateMasks[4] = 0xffffffffffffffffLL << ( 128 - subnet );
				} else {
					AggregateMasks[3] = 0xffffffffffffffffLL << ( 64 - subnet );
					AggregateMasks[4] = 0;
				}
			}
			*Aggregate_bits |= Aggregate_DSTIP;
		} else if ( q ) {
			*q = '/';
			fprintf(stderr, "Subnet specifier not allowed here: '%s'\n", p);
			return 0;
		} else if ( strcasecmp(p, "srcip" ) == 0 ) {
			AggregateMasks[1] = 0xffffffffffffffffLL;
			AggregateMasks[2] = 0xffffffffffffffffLL;
			*Aggregate_bits |= Aggregate_SRCIP;
		} else if ( strcasecmp(p, "dstip" ) == 0 ) {
			AggregateMasks[3] = 0xffffffffffffffffLL;
			AggregateMasks[4] = 0xffffffffffffffffLL;
			*Aggregate_bits |= Aggregate_DSTIP;
		} else if (  strcasecmp(p, "srcport" ) == 0 ) {
			AggregateMasks[0] |= MaskSrcPort;
			*Aggregate_bits |= Aggregate_SRCPORT;
		} else if ( strcasecmp(p, "dstport" ) == 0 ) {
			AggregateMasks[0] |= MaskDstPort;
			*Aggregate_bits |= Aggregate_DSTPORT;
		} else if (  strcasecmp(p, "srcas" ) == 0 ) {
			AggregateMasks[5] |= MaskSrcAS;
			*Aggregate_bits |= Aggregate_SRCAS;
		} else if ( strcasecmp(p, "dstas" ) == 0 ) {
			AggregateMasks[5] |= MaskDstAS;
			*Aggregate_bits |= Aggregate_DSTAS;
		} else if ( strcasecmp(p, "proto" ) == 0 ) {
			AggregateMasks[6] |= MaskProto;
			*Aggregate_bits |= Aggregate_PROTO;


		} else {
			fprintf(stderr, "Unknown aggregate field: '%s'\n", p);
			return 0;
		}
		p = strtok(NULL, ",");
	}

	// if we aggregate srcip and dstip, we retain the AS info and do not mask them out
	// as this info may be important
	if ( (*Aggregate_bits & ( Aggregate_SRCIP + Aggregate_DSTIP )) == ( Aggregate_SRCIP + Aggregate_DSTIP ) ) {
		AggregateMasks[5] = MaskSrcAS | MaskDstAS;
		*Aggregate_bits |= Aggregate_SRCAS;
		*Aggregate_bits |= Aggregate_DSTAS;
	}
	return 1;

} /* End of ParseAggregateMask */

static int ParseCryptoPAnKey ( char *s, char *key ) {
int i, j;
char numstr[3];

	if ( strlen(s) == 32 ) {
		// Key is a string
		strncpy(key, s, 32);
		return 1;
	}

	tolower(s[1]);
	numstr[2] = 0;
	if ( strlen(s) == 66 && s[0] == '0' && s[1] == 'x' ) {
		j = 2;
		for ( i=0; i<32; i++ ) {
			if ( !isxdigit((int)s[j]) || !isxdigit((int)s[j+1]) )
				return 0;
			numstr[0] = s[j++];
			numstr[1] = s[j++];
			key[i] = strtol(numstr, NULL, 16);
		}
		return 1;
	}

	// It's an invalid key
	return 0;

} // End of ParseCryptoPAnKey

static void PrintSummary(stat_record_t *stat_record, int plain_numbers) {
static double	duration;
uint64_t	bps, pps, bpp;
char 		byte_str[32], packet_str[32], bps_str[32], pps_str[32], bpp_str[32];

	bps = pps = bpp = 0;
	duration = stat_record->last_seen - stat_record->first_seen;
	duration += ((double)stat_record->msec_last - (double)stat_record->msec_first) / 1000.0;
	if ( duration > 0 && stat_record->last_seen > 0 ) {
		bps = ( stat_record->numbytes << 3 ) / duration;	// bits per second. ( >> 3 ) -> * 8 to convert octets into bits
		pps = stat_record->numpackets / duration;			// packets per second
		bpp = stat_record->numbytes / stat_record->numpackets;	// Bytes per Packet
	}
	if ( plain_numbers ) {
		printf("Summary: total flows: %llu, total bytes: %llu, total packets: %llu, avg bps: %llu, avg pps: %llu, avg bpp: %llu\n",
			stat_record->numflows, stat_record->numbytes, stat_record->numpackets, bps, pps, bpp );
	} else {
		format_number(stat_record->numbytes, byte_str, VAR_LENGTH);
		format_number(stat_record->numpackets, packet_str, VAR_LENGTH);
		format_number(bps, bps_str, VAR_LENGTH);
		format_number(pps, pps_str, VAR_LENGTH);
		format_number(bpp, bpp_str, VAR_LENGTH);
		printf("Summary: total flows: %llu, total bytes: %s, total packets: %s, avg bps: %s, avg pps: %s, avg bpp: %s\n",
		(unsigned long long)stat_record->numflows, byte_str, packet_str, bps_str, pps_str, bpp_str );
	}

} // End of PrintSummary

stat_record_t process_data(char *wfile, int element_stat, int flow_stat, int sort_flows,
	printer_t print_header, printer_t print_record, time_t twin_start, time_t twin_end, 
	uint64_t limitflows, uint64_t *AggregateMasks, int anon, int tag, int zero_flows) {
data_block_header_t in_flow_header, *out_flow_header;					
common_record_t 	*flow_record, *in_buff, *out_buff;
master_record_t		master_record;
void				*writeto;
stat_record_t 		stat_record;
uint32_t	NumReadRecords, file_blocks, buffer_size;
int 		i, rfd, wfd, done, ret, do_stat, write_file, has_aggregate_mask;
char 		*string;

#ifdef COMPAT14
extern int	Format14;
#endif

	out_flow_header = NULL;
	out_buff 		= NULL;
	writeto			= NULL;

	// if we write to a file other than stdout
	write_file = wfile && ( strcmp(wfile, "-") != 0 );

	// print flows later, when all records are processed and sorted
	if ( sort_flows ) {
		print_record = NULL;
		limitflows = 0;
	}

	memset((void *)&stat_record, 0, sizeof(stat_record_t));

	// time window of all matched flows
	stat_record.first_seen = 0x7fffffff;
	stat_record.msec_first = 999;

	// time window of all processed flows
	t_first_flow = 0x7fffffff;
	t_last_flow  = 0;

	file_blocks  = 0;

	do_stat = element_stat || flow_stat;

	// check for a special aggregate mask
	has_aggregate_mask = AggregateMasks != NULL;

	// Get the first file handle
	rfd = GetNextFile(0, twin_start, twin_end, NULL);
	if ( rfd < 0 ) {
		if ( rfd == FILE_ERROR )
			perror("Can't open input file for reading");
		return stat_record;
	}

#ifdef COMPAT14
	if ( Format14 ) 
		fprintf(stderr, "Reading nfdump <= v1.4 old style file format!\n");
#endif

	if ( wfile ) {
		wfd = strcmp(wfile, "-") == 0 ? STDOUT_FILENO : OpenNewFile(wfile, &string);
		if ( strcmp(wfile, "-") == 0 ) { // output to stdout
			file_header_t	*file_header;
			size_t			len;

			wfd = STDOUT_FILENO;

			len = sizeof(file_header_t) + sizeof(stat_record_t);
			file_header = (file_header_t *)malloc(len);
			memset((void *)file_header, 0, len);
			file_header->magic 		= MAGIC;
			file_header->version 	= VERSION;
			strncpy(file_header->ident, "none", IDENT_SIZE);
			write(STDOUT_FILENO, (void *)file_header, len) ;

		} else {
			wfd = OpenNewFile(wfile, &string);

		}
		if ( wfd < 0 ) {
			if ( string != NULL )
				fprintf(stderr, "%s\n", string);
			if ( rfd ) 
				close(rfd);
			return stat_record;
		}
		out_buff = malloc(OUTPUT_BUFF_SIZE);
		if ( !out_buff ) {
			fprintf(stderr, "Buffer allocation error: %s", strerror(errno));
			return stat_record;
		}
		out_flow_header 			= (data_block_header_t *)out_buff;
		out_flow_header->size 		= 0;
		out_flow_header->NumBlocks 	= 0;
		out_flow_header->id			= DATA_BLOCK_TYPE_1;
		out_flow_header->pad		= 0;
		writeto = (void *)((pointer_addr_t)out_flow_header + sizeof(data_block_header_t));
	} else 
		wfd = 0;

	// allocate buffer suitable for netflow version
	buffer_size = BUFFSIZE;
	in_buff = (common_record_t *) malloc(buffer_size);

	if ( !in_buff ) {
		perror("Memory allocation error");
		close(rfd);
		if ( write_file ) {
			/* Write stat info and close file */
			CloseUpdateFile(wfd, &stat_record, file_blocks, GetIdent(), &string );
			if ( string != NULL )
				fprintf(stderr, "%s\n", string);
		} 
		return stat_record;
	}

	// setup Filter Engine to point to master_record, as any record read from file
	// is expanded into this record
	Engine->nfrecord = (uint64_t *)&master_record;

	done = 0;
	while ( !done ) {

#ifdef COMPAT14
		if ( Format14 ) 
			ret = Compat14_ReadHeader(rfd, &in_flow_header);
		else
			ret = read(rfd, &in_flow_header, sizeof(data_block_header_t));
#else
		ret = read(rfd, &in_flow_header, sizeof(data_block_header_t));
#endif

		if ( ret == 0 ) {
			// EOF of rfd
			rfd = GetNextFile(rfd, twin_start, twin_end, NULL);
			if ( rfd < 0 ) {
				if ( rfd == FILE_ERROR )
					fprintf(stderr, "Can't read from file '%s': %s\n",GetCurrentFilename(), strerror(errno) );
				done = 1;
			} 
			continue;
		} else if ( ret == -1 ) {
			fprintf(stderr, "Can't read from file '%s': %s\n",GetCurrentFilename(), strerror(errno) );
			break;
		}
		total_bytes += ret;

		if ( in_flow_header.id != DATA_BLOCK_TYPE_1 ) {
			fprintf(stderr, "Can't process block type %u\n", in_flow_header.id);
			skipped_flows++;
			continue;
		}

		NumReadRecords = in_flow_header.NumBlocks;

		if ( in_flow_header.size > buffer_size ) {
			void *tmp;
			// Actually, this should never happen, but catch it anyway
			if ( in_flow_header.size > MAX_BUFFER_SIZE ) {
				// this is most likely corrupt
				fprintf(stderr, "Corrupt data file: Requested buffer size %u exceeds max. buffer size.\n", in_flow_header.size);
				break;
			}
			// make it at least the requested size
			buffer_size = in_flow_header.size;
			tmp = realloc((void *)in_buff, buffer_size);
			if ( !tmp ) {
				fprintf(stderr, "Can't reallocate buffer to %u bytes: %s\n", buffer_size, strerror(errno));
				break;
			}
			in_buff = (common_record_t *)tmp;
		}

#ifdef COMPAT14
		if ( Format14 ) 
			ret = Compat14_ReadRecords(rfd, in_buff, &in_flow_header);
		else
			ret = read(rfd, in_buff, in_flow_header.size);
#else
		ret = read(rfd, in_buff, in_flow_header.size);
#endif
		if ( ret == 0 ) {
			done = 1;
			break;
		} else if ( ret == -1 ) {
			perror("Error reading data");
			close(rfd);
			if ( write_file ) {
				/* Write stat info and close file */
				CloseUpdateFile(wfd, &stat_record, file_blocks, GetIdent(), &string );
				if ( string != NULL )
					fprintf(stderr, "%s\n", string);
			} 
			break;
		}
		if ( in_flow_header.size != ret ) {
			// Ups - this was a short read - most likely reading from the stdin pipe
			// loop until we have requested size
			size_t	request_size, total_size;
			void *read_ptr;

			total_size 	 = ret;
			request_size = in_flow_header.size - total_size;
			read_ptr 	 = (void *)((pointer_addr_t)in_buff + total_size);
			do {
				ret = read(rfd, read_ptr, request_size);
				if ( ret == 0 ) {
					break;
				} else if ( ret < 0 ) {
					perror("Error reading data");
					break;
				}
				total_size += ret;
				if ( total_size < in_flow_header.size ) {
					request_size = in_flow_header.size - ret;
					read_ptr 	 = (void *)((pointer_addr_t)in_buff + total_size);
					request_size = in_flow_header.size - total_size;
				}
			} while ( ret > 0 && ( total_size < in_flow_header.size ));
			
			if ( total_size != in_flow_header.size ) {
				// still unsuccessful
#ifdef HAVE_SIZE_T_Z_FORMAT
				fprintf(stderr, "Short read for netflow records: Expected %i, got %zu bytes!\n",
					in_flow_header.size, total_size );
#else
				fprintf(stderr, "Short read for netflow records: Expected %i, got %lu bytes!\n",
					in_flow_header.size, (unsigned long)total_size );
#endif
				continue;
			} else {
				// continue
				ret = in_flow_header.size;
			}
		}
		total_bytes += ret;

		flow_record = in_buff;
		for ( i=0; i < NumReadRecords; i++ ) {
			total_flows++;
			ExpandRecord( flow_record, &master_record);

			// Update global time span window
			if ( master_record.first < t_first_flow )
				t_first_flow = master_record.first;
			if ( master_record.last > t_last_flow ) 
				t_last_flow = master_record.last;

			// Time based filter
			// if no time filter is given, the result is always true
			flow_record->mark  = twin_start && (master_record.first < twin_start || master_record.last > twin_end) ? 0 : 1;
			flow_record->mark &= limitflows ? stat_record.numflows < limitflows : 1;

			// filter netflow record with user supplied filter
			if ( flow_record->mark ) 
				flow_record->mark = (*Engine->FilterEngine)(Engine);

			if ( flow_record->mark == 0 ) { // record failed to pass all filters
				// increment pointer by number of bytes for netflow record
				flow_record = (common_record_t *)((pointer_addr_t)flow_record + flow_record->size);	
				// go to next record
				continue;
			}

			// Records passed filter -> continue record processing
			flow_record->mark = 0;

			// Update statistics
			switch (master_record.prot) {
				case 1:
					stat_record.numflows_icmp++;
					stat_record.numpackets_icmp += master_record.dPkts;
					stat_record.numbytes_icmp   += master_record.dOctets;
					break;
				case 6:
					stat_record.numflows_tcp++;
					stat_record.numpackets_tcp += master_record.dPkts;
					stat_record.numbytes_tcp   += master_record.dOctets;
					break;
				case 17:
					stat_record.numflows_udp++;
					stat_record.numpackets_udp += master_record.dPkts;
					stat_record.numbytes_udp   += master_record.dOctets;
					break;
				default:
					stat_record.numflows_other++;
					stat_record.numpackets_other += master_record.dPkts;
					stat_record.numbytes_other   += master_record.dOctets;
			}
			stat_record.numflows++;
			stat_record.numpackets 	+= master_record.dPkts;
			stat_record.numbytes 	+= master_record.dOctets;

			if ( master_record.first < stat_record.first_seen ) {
				stat_record.first_seen = master_record.first;
				stat_record.msec_first = master_record.msec_first;
			}
			if ( master_record.first == stat_record.first_seen && 
				 master_record.msec_first < stat_record.msec_first ) 
					stat_record.msec_first = master_record.msec_first;

			if ( master_record.last > stat_record.last_seen ) {
				stat_record.last_seen = master_record.last;
				stat_record.msec_last = master_record.msec_last;
			}
			if ( master_record.last == stat_record.last_seen && 
				 master_record.msec_last > stat_record.msec_last ) 
					stat_record.msec_last = master_record.msec_last;


			if ( wfd != 0 ) {
				if ( (out_flow_header->size + flow_record->size) < OUTPUT_BUFF_SIZE && !zero_flows) {
					memcpy(writeto, (void *)flow_record, flow_record->size);

					if ( anon ) {
						pointer_addr_t size = sizeof(common_record_t) - sizeof(uint8_t[4]);
						if ( (flow_record->flags & FLAG_IPV6_ADDR ) == 0 ) {
							uint32_t	*ip = (uint32_t *)((pointer_addr_t)writeto + size);
							ip[0] = anonymize(ip[0]);
							ip[1] = anonymize(ip[1]);
						} else {
							ipv6_block_t *ip = (ipv6_block_t *)((pointer_addr_t)writeto + size);
							uint64_t	anon_ip[2];
							anonymize_v6(ip->srcaddr, anon_ip);
							ip->srcaddr[0] = anon_ip[0];
							ip->srcaddr[1] = anon_ip[1];

							anonymize_v6(ip->dstaddr, anon_ip);
							ip->dstaddr[0] = anon_ip[0];
							ip->dstaddr[1] = anon_ip[1];
						}
					} 
	
					out_flow_header->NumBlocks++;
					out_flow_header->size += flow_record->size;
					writeto = (void *)((pointer_addr_t)writeto + flow_record->size);

					// flush current buffer to disc
					if ( out_flow_header->size > OUTPUT_FLUSH_LIMIT ) {
						if ( write(wfd, out_buff, sizeof(data_block_header_t) + out_flow_header->size) <= 0 ) {
							fprintf(stderr, "Failed to write output buffer to disk: '%s'" , strerror(errno));
						} else {
							out_flow_header->size 		= 0;
							out_flow_header->NumBlocks 	= 0;
							writeto = (void *)((pointer_addr_t)out_flow_header + sizeof(data_block_header_t) );
							file_blocks++;
						}
					}
				}

			} else if ( do_stat ) {
					// we need to add this record to the stat hash
					AddStat(&in_flow_header, &master_record, flow_stat, element_stat, has_aggregate_mask, AggregateMasks);
			} else {
					// if we need tp print out this record
					if ( print_record ) {
						print_record(&master_record, 1, &string, anon, tag);
						if ( string ) {
							if ( limitflows ) {
								if ( (stat_record.numflows <= limitflows) )
									printf("%s\n", string);
							} else 
								printf("%s\n", string);
						}
					}

					// if we need to sort the flows first -> insert into hash table
					// they get 
					if ( sort_flows ) 
						InsertFlow(&master_record);
			}

			// Advance pointer by number of bytes for netflow record
			flow_record = (common_record_t *)((pointer_addr_t)flow_record + flow_record->size);	

		} // for all records

		// check if we are done, due to -c option 
		if ( limitflows ) 
			done = stat_record.numflows >= limitflows;

	} // while

	if ( rfd > 0 ) 
		close(rfd);

	// flush output file
	if ( wfd ) {
		// flush current buffer to disc
		if ( out_flow_header->NumBlocks && !zero_flows) {
			if ( write(wfd, out_buff, sizeof(data_block_header_t) + out_flow_header->size) <= 0 ) {
				fprintf(stderr, "Failed to write output buffer to disk: '%s'" , strerror(errno));
			} else {
				file_blocks++;
			}
		}

		/* Stat info */
		if ( write_file ) {
			/* Write stat info and close file */
			CloseUpdateFile(wfd, &stat_record, file_blocks, GetIdent(), &string );
			if ( string != NULL )
				fprintf(stderr, "%s\n", string);
		} // else stdout
	}	 

	free((void *)in_buff);
	return stat_record;

} // End of process_data


int main( int argc, char **argv ) {
struct stat stat_buff;
stat_record_t	sum_stat, *sr;
printer_t 	print_header, print_record;
nfprof_t 	profile_data;
char 		c, *rfile, *Rfile, *Mdirs, *wfile, *ffile, *filter, *tstring, *stat_type;
char		*byte_limit_string, *packet_limit_string, *print_mode, *record_header;
char		*order_by, CryptoPAnKey[32];
int 		ffd, ret, element_stat, fdump;
int 		i, user_format, quiet, flow_stat, topN, aggregate, aggregate_mask;
int 		print_stat, syntax_only, date_sorted, do_anonymize, do_tag, zero_flows;
int			plain_numbers, pipe_output;
time_t 		t_start, t_end;
uint16_t	Aggregate_Bits;
uint32_t	limitflows;
uint64_t	AggregateMasks[AGGR_SIZE];
char 		Ident[IdentLen];

	rfile = Rfile = Mdirs = wfile = ffile = filter = tstring = stat_type = NULL;
	byte_limit_string = packet_limit_string = NULL;
	fdump = aggregate = 0;
	aggregate_mask	= 0;
	t_start = t_end = 0;
	syntax_only	    = 0;
	topN	        = 10;
	flow_stat       = 0;
	print_stat      = 0;
	element_stat  	= 0;
	limitflows		= 0;
	date_sorted		= 0;
	total_bytes		= 0;
	total_flows		= 0;
	skipped_flows	= 0;
	do_anonymize	= 0;
	do_tag			= 0;
	quiet			= 0;
	user_format		= 0;
	zero_flows		= 0;
	plain_numbers   = 0;
	pipe_output		= 0;

	print_mode      = NULL;
	print_header 	= NULL;
	print_record  	= NULL;
	record_header 	= "";
	Aggregate_Bits	= 0xFFFF;	// set all bits

	Ident[0] = '\0';

	SetStat_DefaultOrder("flows");

	for ( i=0; i<AGGR_SIZE; AggregateMasks[i++] = 0 ) ;

	while ((c = getopt(argc, argv, "6aA:c:Ss:hn:i:f:qzr:w:K:M:NImO:R:XZt:TVv:l:L:o:")) != EOF) {
		switch (c) {
			case 'h':
				usage(argv[0]);
				exit(0);
				break;
			case 'a':
				aggregate = 1;
				break;
			case 'A':
				if ( !ParseAggregateMask(optarg, AggregateMasks, &Aggregate_Bits) ) {
					fprintf(stderr, "Option -A requires a ',' separated list out of 'srcip dstip srcport dstport srcip4[/netbits], dstip4[/netbits], srcip6[/netbits], dstip6[/netbits] '\n");
					exit(255);
				}
				aggregate_mask = 1;
				break;
			case 'X':
				fdump = 1;
				break;
			case 'Z':
				syntax_only = 1;
				break;
			case 'q':
				quiet = 1;
				break;
			case 'z':
				zero_flows = 1;
				break;
			case 'c':	
				limitflows = atoi(optarg);
				if ( !limitflows ) {
					fprintf(stderr, "Option -c needs a number > 0\n");
					exit(255);
				}
				break;
			case 's':
				stat_type = optarg;
                if ( !SetStat(stat_type, &element_stat, &flow_stat) ) {
                    exit(255);
                } 
				break;
			case 'V':
				printf("%s: Version: %s %s\n%s\n",argv[0], nfdump_version, nfdump_date, rcsid);
				exit(0);
				break;
			case 'l':
				packet_limit_string = optarg;
				break;
			case 'K':
				if ( !ParseCryptoPAnKey(optarg, CryptoPAnKey) ) {
					fprintf(stderr, "Invalid key '%s' for CryptoPAn!\n", optarg);
					exit(255);
				}
				do_anonymize = 1;
				break;
			case 'L':
				byte_limit_string = optarg;
				break;
			case 'N':
				plain_numbers = 1;
				break;
			case 'f':
				ffile = optarg;
				break;
			case 't':
				tstring = optarg;
				break;
			case 'r':
				rfile = optarg;
				if ( strcmp(rfile, "-") == 0 )
					rfile = NULL;
				break;
			case 'm':
				date_sorted = 1;
				break;
			case 'M':
				Mdirs = optarg;
				break;
			case 'I':
				print_stat++;
				break;
			case 'o':	// output mode
				print_mode = optarg;
				break;
			case 'O':	// stat order by
				order_by = optarg;
				if ( !SetStat_DefaultOrder(order_by) ) {
					fprintf(stderr, "Order '%s' unknown!\n", order_by);
					exit(255);
				}
				break;
			case 'R':
				Rfile = optarg;
				break;
			case 'v':
				fprintf(stderr, "Option no longer supported.\n");
				break;
			case 'w':
				wfile = optarg;
				break;
			case 'n':
				topN = atoi(optarg);
				if ( topN < 0 ) {
					fprintf(stderr, "TopnN number %i out of range\n", topN);
					exit(255);
				}
				break;
			case 'S':	// Compatibility with pre 1.4 -S option
				stat_type = "record/packets/bytes";
				fprintf(stderr, "WARNING: -S depricated! use -s record/packets/bytes instead. Option will get removed.\n");
                if ( !SetStat(stat_type, &element_stat, &flow_stat) ) {
                    // Should never happen
                    exit(255);
                } 
				break;
			case 'T':
				do_tag = 1;
				break;
			case 'i':
				strncpy(Ident, optarg, IDENT_SIZE);
				Ident[IDENT_SIZE - 1] = 0;
				if ( strchr(Ident, ' ') ) {
					fprintf(stderr,"Ident must not contain spaces\n");
					exit(255);
				}
				break;
			case '6':	// print long IPv6 addr
				Setv6Mode(1);
				break;
			default:
				usage(argv[0]);
				exit(0);
		}
	}
	if (argc - optind > 1) {
		usage(argv[0]);
		exit(255);
	} else {
		/* user specified a pcap filter */
		filter = argv[optind];
	}
	
	// Change Ident only
	if ( rfile && strlen(Ident) > 0 ) {
		char *err;
		ChangeIdent(rfile, Ident, &err);
		exit(0);
	}

	if ( (element_stat && !flow_stat) && aggregate_mask ) {
		fprintf(stderr, "Warning: Aggregation ignored for element statistics\n");
		aggregate_mask = 0;
	}

	if ( aggregate_mask && stat_type ) {
		char *stat_conflict = VerifyStat(Aggregate_Bits);
		if ( stat_conflict ) {
			fprintf(stderr, "Selected aggregation masks record field '%s'!\n", stat_conflict);
			exit(255);
		}
	}

	if ( rfile && Rfile ) {
		fprintf(stderr, "-r and -R are mutually exclusive. Plase specify either -r or -R\n");
		exit(255);
	}
	if ( Mdirs && !(rfile || Rfile) ) {
		fprintf(stderr, "-M needs either -r or -R to specify the file or file list. Add '-R .' for all files in the directories.\n");
		exit(255);
	}

	SetupInputFileSequence(Mdirs, rfile, Rfile);

	if ( print_stat ) {
		if ( !rfile && !Rfile && !Mdirs) {
			fprintf(stderr, "Expect data file(s).\n");
			exit(255);
		}

		memset((void *)&sum_stat, 0, sizeof(stat_record_t));
		sum_stat.first_seen = 0x7fffffff;
		sum_stat.msec_first = 999;
		ffd = GetNextFile(0, 0, 0, &sr);
		if ( ffd <= 0 ) {
			if ( ffd == FILE_ERROR )
				fprintf(stderr, "Error open file: %s\n", strerror(errno));
			exit(250);
		}
		while ( ffd > 0 ) {
			SumStatRecords(&sum_stat, sr);
			ffd = GetNextFile(ffd, 0, 0, &sr);
		}
		PrintStat(&sum_stat);
		exit(0);
	}

	// handle print mode
	if ( !print_mode )
		print_mode = DefaultMode;


	if ( strncasecmp(print_mode, "fmt:", 4) == 0 ) {
		// special user defined output format
		char *format = &print_mode[4];
		if ( strlen(format) ) {
			if ( !ParseOutputFormat(format) )
				exit(255);
			print_record  = format_special;
			record_header = format_special_header();
			user_format	  = 1;
		} else {
			fprintf(stderr, "Missing format description for user defined output format!\n");
			exit(255);
		}
	} else {
		// predefined output format

		// Check for long_v6 mode
		i = strlen(print_mode);
		if ( i > 2 ) {
			if ( print_mode[i-1] == '6' ) {
				Setv6Mode(1);
				print_mode[i-1] = '\0';
			} else 
				Setv6Mode(0);
		}

		i = 0;
		while ( printmap[i].printmode ) {
			if ( strncasecmp(print_mode, printmap[i].printmode, MAXMODELEN) == 0 ) {
				if ( printmap[i].Format ) {
					if ( !ParseOutputFormat(printmap[i].Format) )
						exit(255);
					// predefined custom format
					print_record  = printmap[i].func;
					record_header = format_special_header();
					user_format	  = 1;
				} else {
					// To support the pipe output format for element stats - check for pipe, and remember this
					if ( strncasecmp(print_mode, "pipe", MAXMODELEN) == 0 ) {
						pipe_output = 1;
					}
					// predefined static format
					print_record  = printmap[i].func;
					record_header = printmap[i].HeaderLine;
					user_format	  = 0;
				}
				break;
			}
			i++;
		}
	}

	if ( !print_record ) {
		fprintf(stderr, "Unknown output mode '%s'\n", print_mode);
		exit(255);
	}

	// this is the only case, where headers are printed.
	if ( strncasecmp(print_mode, "raw", 16) == 0 )
		print_header = format_file_block_header;
	
	if ( aggregate && (flow_stat || element_stat) ) {
		aggregate = 0;
		fprintf(stderr, "Command line switch -s or -S overwrites -a\n");
	}

	if ( !filter && ffile ) {
		if ( stat(ffile, &stat_buff) ) {
			fprintf(stderr, "Can't stat filter file '%s': %s\n", ffile, strerror(errno));
			exit(255);
		}
		filter = (char *)malloc(stat_buff.st_size+1);
		if ( !filter ) {
			perror("Memory allocation error");
			exit(255);
		}
		ffd = open(ffile, O_RDONLY);
		if ( ffd < 0 ) {
			fprintf(stderr, "Can't open filter file '%s': %s\n", ffile, strerror(errno));
			exit(255);
		}
		ret = read(ffd, (void *)filter, stat_buff.st_size);
		if ( ret < 0   ) {
			perror("Error reading filter file");
			close(ffd);
			exit(255);
		}
		total_bytes += ret;
		filter[stat_buff.st_size] = 0;
		close(ffd);
	}

	// if no filter is given, set the default ip filter which passes through every flow
	if ( !filter  || strlen(filter) == 0 ) 
		filter = "any";

	Engine = CompileFilter(filter);
	if ( !Engine ) 
		exit(254);

	if ( fdump ) {
		printf("StartNode: %i Engine: %s\n", Engine->StartNode, Engine->Extended ? "Extended" : "Fast");
		DumpList(Engine);
		exit(0);
	}

	if ( syntax_only )
		exit(0);

	if ((aggregate || flow_stat)  && ( topN > 1000 || topN == 0) ) {
		printf("TopN for record statistic: 0 < topN < 1000 only allowed for IP statistics\n");
		exit(255);
	}

	if ((aggregate || flow_stat || date_sorted)  && !Init_FlowTable(HashBits, NumPrealloc) )
			exit(250);

	if (element_stat && !Init_StatTable(HashBits, NumPrealloc) )
			exit(250);

	SetLimits(element_stat || aggregate || flow_stat, packet_limit_string, byte_limit_string);

	if ( tstring ) {
		if ( !ScanTimeFrame(tstring, &t_start, &t_end) )
			exit(255);
	}


	if ( !(flow_stat || element_stat || wfile || quiet ) && record_header ) {
		if ( user_format ) {
			printf("%s\n", record_header);
		} else {
			// static format - no static format with header any more, but keep code anyway
			if ( Getv6Mode() ) {
				printf("%s\n", record_header);
			} else
				printf("%s\n", record_header);
		}
	}

	if (do_anonymize)
		PAnonymizer_Init((uint8_t *)CryptoPAnKey);

	nfprof_start(&profile_data);
	sum_stat = process_data(wfile, element_stat, aggregate || flow_stat, date_sorted,
						print_header, print_record, t_start, t_end, 
						limitflows, aggregate_mask ? AggregateMasks : NULL, do_anonymize, do_tag, zero_flows);
	nfprof_end(&profile_data, total_flows);

	if ( total_bytes == 0 )
		exit(0);

	if (aggregate) {
		ReportAggregated(print_record, limitflows, date_sorted, do_anonymize, do_tag);
		Dispose_Tables(1, 0); // Free FlowTable
	}

	if (flow_stat || element_stat) {
		ReportStat(record_header, print_record, topN, flow_stat, element_stat, do_anonymize, do_tag, pipe_output);
		Dispose_Tables(flow_stat, element_stat);
	} 

	if ( date_sorted && !(aggregate || flow_stat || element_stat) ) {
		PrintSortedFlows(print_record, limitflows, do_anonymize, do_tag);
		Dispose_Tables(1, 0);	// Free FlowTable
	}

	if ( !wfile && !quiet ) {
		if (do_anonymize)
			printf("IP addresses anonymized\n");
		PrintSummary(&sum_stat, plain_numbers);
 		printf("Time window: %s\n", TimeString(t_first_flow, t_last_flow));
		printf("Total flows processed: %u, skipped: %u, Bytes read: %llu\n", 
			total_flows, skipped_flows, (unsigned long long)total_bytes);
		nfprof_print(&profile_data, stdout);
	}
	return 0;
}

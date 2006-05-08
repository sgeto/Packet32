/*============================================================================*
 * FILE: winpcap_stress.c
 *============================================================================*
 *
 * COPYRIGHT (C) 2006 BY
 *          CACE TECHNOLOGIES, INC., DAVIS, CALIFORNIA
 *          ALL RIGHTS RESERVED.
 *
 *          THIS SOFTWARE IS FURNISHED UNDER A LICENSE AND MAY BE USED AND
 *          COPIED ONLY IN ACCORDANCE WITH THE TERMS OF SUCH LICENSE AND WITH
 *          THE INCLUSION OF THE ABOVE COPYRIGHT NOTICE.  THIS SOFTWARE OR ANY
 *          OTHER COPIES THEREOF MAY NOT BE PROVIDED OR OTHERWISE MADE
 *          AVAILABLE TO ANY OTHER PERSON.  NO TITLE TO AND OWNERSHIP OF THE
 *          SOFTWARE IS HEREBY TRANSFERRED.
 *
 *          THE INFORMATION IN THIS SOFTWARE IS SUBJECT TO CHANGE WITHOUT
 *          NOTICE AND SHOULD NOT BE CONSTRUED AS A COMMITMENT BY CACE TECNOLOGIES
 *
 *===========================================================================*
 *
 * This program is a generic "stress test" for winpcap. It creates several threads
 * each of which opens an adapter, captures some packets and then closes it.
 * The user can specify:
 *
 *  - the number of threads
 *  - the number of read operations that every thread performs before exiting
 *
 * The program prints statistics before exiting.
 *
 *===========================================================================*/


#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <pcap.h>

#define LINE_LEN 16

#define NUM_THREADS 16
#define MAX_NUM_READS 500

u_int n_iterations = 0;
u_int n_packets = 0;
u_int n_timeouts = 0;
u_int n_open_errors = 0;
u_int n_read_errors = 0;
u_int n_findalldevs_errors = 0;

CRITICAL_SECTION print_cs;


/////////////////////////////////////////////////////////////////////

void usage()
{
	printf("winpcap_stress: utility that stresses winpcap by opening and capturing from multiple adapters at the same time.\n");
	printf("   Usage: winpcap_stress <nthreads> <adapter_substring_to_match>\n\n"
		"   Examples:\n"
		"      winpcap_stress\n"
		"      winpcap_stress 10\n\n"
		"      winpcap_stress 10 \\Device\\NPF_{ \n");	
}

/////////////////////////////////////////////////////////////////////

void sigh(int val)
{
	EnterCriticalSection(&print_cs);

	printf("\nNumber of iterations:\t\t%u\n", n_iterations);
	printf("Number of packets captured:\t\t%u\n", n_packets);
	printf("Number of read timeouts:\t\t%u\n", n_timeouts);
	printf("Number of open errors:\t\t%u\n", n_open_errors);
	printf("Number of read errors:\t\t%u\n", n_read_errors);

	//
	// Note: we don't release the critical section on purpose, so the user doesn't 
	// get crappy input when he presses CTRL+C 
	//
	exit(0);
}

/////////////////////////////////////////////////////////////////////

DWORD WINAPI pcap_thread(LPVOID arg)
{
	pcap_t *fp;
	char* AdName = (char*)arg;
	char errbuf[PCAP_ERRBUF_SIZE];
	int res;
	struct pcap_pkthdr *header;
	const u_char *pkt_data;
	u_int i;
	u_int n_reads;

	//
	// Open the adapter
	//
	if((fp = pcap_open_live(AdName,
		65535,
		1,								// promiscuous mode
		1,								// read timeout
		errbuf)) == NULL)
	{
		EnterCriticalSection(&print_cs);
		fprintf(stderr,"\nError opening adapter (%s)\n", errbuf);
		LeaveCriticalSection(&print_cs);
		n_open_errors++;
		return -1;
	}
	
	//
	// Read the packets
	//
	if(MAX_NUM_READS)
	{
		n_reads = rand() % MAX_NUM_READS;
	}
	else
	{
		n_reads = 0;
	}

	for(i = 0; i < n_reads; i++)
	{
		res = pcap_next_ex(fp, &header, &pkt_data);
		
		if(res < 0)
		{
			break;
		}

		if(res == 0)
		{
			// Timeout elapsed
			n_timeouts++;
			continue;
		}

		// print pkt timestamp and pkt len
		n_packets++;
	}

	if(res == -1)
	{
		EnterCriticalSection(&print_cs);
		printf("Read error: %s\n", pcap_geterr(fp));
		LeaveCriticalSection(&print_cs);
		n_read_errors++;
	}

	pcap_close(fp);

	return 1;
}

/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{	
	pcap_if_t *alldevs, *d;
	u_int i;
	char errbuf[PCAP_ERRBUF_SIZE];
	u_int n_threads;
	HANDLE* hThreads;
	char* string_to_match;
	DWORD WaitRes;

	if(argc == 1)
	{
		n_threads = NUM_THREADS;
		string_to_match = NULL;
	}
	else
	if(argc == 2)
	{
		n_threads = atoi(argv[1]);
		string_to_match = NULL;
	}
	else
	if(argc == 3)
	{
		n_threads = atoi(argv[1]);
		string_to_match = argv[2];
	}
	else
	{
		usage();
		return 1;
	}

	hThreads = (HANDLE*)malloc(n_threads * sizeof(HANDLE));
	if(!hThreads)
	{
		printf("Memeory allocation failure\n");
		return -1;
	}
	
	memset(hThreads, n_threads * sizeof(HANDLE), 0);

	signal(SIGINT, sigh);
	InitializeCriticalSection(&print_cs);

	// 
	// Scan the device list
	//
	if(pcap_findalldevs(&alldevs, errbuf) == -1)
	{
		EnterCriticalSection(&print_cs);
		fprintf(stderr,"Error in pcap_findalldevs_ex: %s\n", errbuf);
		LeaveCriticalSection(&print_cs);
		n_findalldevs_errors++;
//		continue;
		return -1;
	}
	
	//
	// Jump to the selected adapter
	//
	for(i = 0;;)
	{		
		//
		// Go through the list, feeding a thread with each adapter that contains our substring
		//
		for(d = alldevs; d; d = d->next)
		{
			if(string_to_match)
			{
				if(!strstr(d->name, string_to_match))
				{
					continue;
				}
			}

			if(i == n_threads)
			{
				i= 0;
			}
			
			//
			// Check if the thread is done
			//
			WaitRes = WaitForSingleObject(hThreads[i], 1);

			if(WaitRes == WAIT_TIMEOUT)
			{
				//
				// In case of timeout, we switch to the following thread
				//
				i++;
				continue;
			}
			
			//
			// Create the child thread
			//
			EnterCriticalSection(&print_cs);
			printf("Thread %u, %s 0x%x\n", i, d->name, WaitRes);
			LeaveCriticalSection(&print_cs);
			
			//
			// Close the thread handle 
			//
			if(hThreads[i])
			{
				CloseHandle(hThreads[i]);
			}

			hThreads[i] = CreateThread(NULL, 0, pcap_thread, d->name, 0, NULL);
			
			if(hThreads[i] == NULL)
			{
				printf("error creating thread. Quitting\n");
				sigh(42);
			}
			
			n_iterations++;
			i++;
		}
	}
	
	free(hThreads);
	
	return 0;
}
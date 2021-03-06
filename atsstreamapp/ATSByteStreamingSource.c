/*
Copyright (c) 2015, Cable Television Laboratories, Inc.(“CableLabs”)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of CableLabs nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL CABLELABS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "log.h"

#define TS_SIZE 188

unsigned int g_exit = 0;

static struct option long_options[] = { 
    { "help",  no_argument, NULL, 'h' }, 
//    { "packetsPerSecond",  required_argument, NULL, 'p' }, 
    { 0,  0, 0, 0 }
}; 

static char options[] = 
"\t-h, --help\n";
//"\t-p, --packetsPerSecond\n"; 

static void usage() 
{ 
    fprintf(stderr, "\nATSStreamApp\n"); 
    fprintf(stderr, "\nUsage: \nATSStreamApp [options] <inputfile1>,<destinationIP1>:<destinationPort1>,<TSPacketsPerSec1> <inputfile2>,<destinationIP2>:<destinationPort2>,<TSPacketsPerSec2> ... <inputfileN>,<destinationIPN>:<destinationPortN>,<TSPacketsPerSecN>\n\nOptions:\n%s\n", options);
}

typedef struct
{
   int threadNum;
   char *filePath;
   unsigned long destIPAddr;
   unsigned short destPort;
   int packetsPerSecond;

} ebp_file_stream_thread_params_t;

int parseInputArg (char *inputArg, char **ppFilePath, unsigned long *pDestIP, unsigned short *pDestPort,
                   int *pPacketsPerSec)
{
   // input args are of the form "filepath,IPAddr:port"

   printf ("Inside parseInputArg\n");

   *ppFilePath = strtok (inputArg, ",");
   char *temp = strtok (NULL, ",");
   char *packetsPerSecondString = strtok (NULL, ",");
   *pPacketsPerSec = atoi(packetsPerSecondString);

   char *ipString = strtok (temp, ":");
   char *portString = strtok (NULL, "");

   printf ("filePath = %s\n", *ppFilePath);
   printf ("ipString = %s\n", ipString);
   printf ("portString = %s\n", portString);
   printf ("packetsPerSecondString = %s\n", packetsPerSecondString);

   char *tempStr1 = strtok (ipString, ".");
   char *tempStr2 = strtok (NULL, ".");
   char *tempStr3 = strtok (NULL, ".");
   char *tempStr4 = strtok (NULL, ".");

   unsigned char temp1 = (unsigned char) strtoul (tempStr1, NULL, 10);
   unsigned char temp2 = (unsigned char) strtoul (tempStr2, NULL, 10);
   unsigned char temp3 = (unsigned char) strtoul (tempStr3, NULL, 10);
   unsigned char temp4 = (unsigned char) strtoul (tempStr4, NULL, 10);

   *pDestIP = temp1;
   *pDestIP = (*pDestIP)<<8;
   *pDestIP = (*pDestIP) | temp2;
   *pDestIP = (*pDestIP)<<8;
   *pDestIP = (*pDestIP) | temp3;
   *pDestIP = (*pDestIP)<<8;
   *pDestIP = (*pDestIP) | temp4;

   *pDestPort = (unsigned short) strtoul (portString, NULL, 10);

//   printf ("DestIP = 0x%x\n", (unsigned int)(*pDestIP));
//   printf ("DestPort = %u\n", (unsigned int)(*pDestPort));

   return 0;
}

int sendBytes (uint8_t *buf, int bufSz, int socketHandle, struct sockaddr_in *destAddr)
{   
   /* send a message to the server */ 
   int returnCode = sendto(socketHandle, buf, bufSz, 0, (struct sockaddr *)destAddr, sizeof(*destAddr));
   if (returnCode < 0)
   { 
      // fatal error here
      return -1; 
   }

   // check if all bytes were sent
   if (returnCode != bufSz)
   {
      return -2;
   }

   return returnCode;
}


void *EBPFileStreamThreadProc(void *threadParams)
{
   int returnCode = 0;

   ebp_file_stream_thread_params_t * ebpFileStreamThreadParams = (ebp_file_stream_thread_params_t *)threadParams;
   LOG_INFO_ARGS("EBPFileStreamThread %d starting...", ebpFileStreamThreadParams->threadNum);


   FILE *infile = NULL;
   if ((infile = fopen(ebpFileStreamThreadParams->filePath, "rb")) == NULL)
   {
      LOG_ERROR_ARGS("EBPFileStreamThread %d: FAIL: Cannot open file %s - %s", 
         ebpFileStreamThreadParams->threadNum, ebpFileStreamThreadParams->filePath, strerror(errno));

      free (ebpFileStreamThreadParams);
   }

	struct sockaddr_in myAddr;
   struct sockaddr_in destAddr;

   // create socket
   int mySocket = socket(AF_INET, SOCK_DGRAM, 0);
   if (mySocket < 0)
   {
      LOG_ERROR_ARGS("EBPFileStreamThread %d: Error creating socket: %s", 
         ebpFileStreamThreadParams->threadNum, strerror(errno));
      return NULL;
   }

   // bind to loacal addr
   memset((void *)&myAddr, 0, sizeof(myAddr));
	myAddr.sin_family = AF_INET;
	myAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	myAddr.sin_port = 0;

	returnCode = bind(mySocket, (struct sockaddr *)&myAddr, sizeof(myAddr));
   if (returnCode < 0)
   {
      LOG_ERROR_ARGS("EBPFileStreamThread %d: Error binding socket: %s", 
         ebpFileStreamThreadParams->threadNum, strerror(errno));
      return NULL;
	}

   // destination address
   memset((char*)&destAddr, 0, sizeof(destAddr)); 
   destAddr.sin_family = AF_INET; 
   destAddr.sin_port = htons(ebpFileStreamThreadParams->destPort); 
	destAddr.sin_addr.s_addr = htonl(ebpFileStreamThreadParams->destIPAddr);  // 192.168.0.17


   int num_packets = 5;  // GORP
   int bufSz = TS_SIZE * num_packets;
   uint8_t *buf = malloc(bufSz);

   int total_packets = 0;

   // read file and send out UDP packets -- be sure that each packet has
   // integral number of transport stream packets
   int i=0;
   while ((num_packets = fread(buf, TS_SIZE, num_packets, infile)) > 0)
   {
      total_packets += num_packets;

 /*     if (i%1000 == 0)
      {
         LOG_INFO_ARGS ("total_packets = %d", total_packets);
      }
      */
      i++;

      returnCode = sendBytes (buf, num_packets * TS_SIZE, mySocket, &destAddr);
      if (returnCode < 0)
      {
         LOG_ERROR_ARGS("EBPFileStreamThread %d: Error sending bytes: %s", 
            ebpFileStreamThreadParams->threadNum, strerror(errno));
      }

      if (ebpFileStreamThreadParams->packetsPerSecond != 0 && total_packets%ebpFileStreamThreadParams->packetsPerSecond == 0)
      {
         LOG_INFO_ARGS ("total_packets = %d, packetsPerSecond = %d...sleeping...", total_packets, ebpFileStreamThreadParams->packetsPerSecond);
         sleep (1);
      }

      if (g_exit)
      {
         break;
      }
   }

   LOG_INFO_ARGS ("DONE: total_packets = %d", total_packets);
   close (mySocket);

   return NULL;
}

static int startThreads(int numFiles, char **inputArgs, pthread_t ***fileStreamThreads, pthread_attr_t *threadAttr)
{
   LOG_INFO ("Main:startThreads: entering");

   char *pFilePath;
   unsigned long destIP;
   unsigned short destPort;
   int packetsPerSecond;

   int returnCode = 0;

   // one worker thread per file
   // num worker thread = numFiles

   // filenames are of the form "filepath,IPAddr:port,packetsPerSec"

   pthread_attr_init(threadAttr);
   pthread_attr_setdetachstate(threadAttr, PTHREAD_CREATE_JOINABLE);

   // start the file stream threads
   *fileStreamThreads = (pthread_t **) calloc (numFiles, sizeof(pthread_t*));
   for (int threadIndex = 0; threadIndex < numFiles; threadIndex++)
   {
      returnCode = parseInputArg (inputArgs[threadIndex], &pFilePath, &destIP, &destPort, &packetsPerSecond);
      if (returnCode < 0)
      {
         // GORP: fatal error here
         continue;
      }

      ebp_file_stream_thread_params_t *ebpFileStreamThreadParams = (ebp_file_stream_thread_params_t *)malloc (sizeof(ebp_file_stream_thread_params_t));
      ebpFileStreamThreadParams->threadNum = threadIndex;  // same as file index
      ebpFileStreamThreadParams->filePath = pFilePath;
      ebpFileStreamThreadParams->destIPAddr = destIP;
      ebpFileStreamThreadParams->destPort = destPort;
      ebpFileStreamThreadParams->packetsPerSecond = packetsPerSecond;

      (*fileStreamThreads)[threadIndex] = (pthread_t *)malloc (sizeof (pthread_t));
      pthread_t *fileStreamThread = (*fileStreamThreads)[threadIndex];
      LOG_INFO_ARGS("Main:startThreads: creating fileStream thread %d", threadIndex);
      returnCode = pthread_create(fileStreamThread, threadAttr, EBPFileStreamThreadProc, (void *)ebpFileStreamThreadParams);
      if (returnCode)
      {
         LOG_ERROR_ARGS("Main:startThreads: FAIL: error %d creating fileStream thread %d", 
            returnCode, threadIndex);
          return -1;
      }
  }

   LOG_INFO("Main:startThreads: exiting");
   return 0;
}
  
static int waitForThreadsToExit(int numFiles, pthread_t **fileStreamThreads, pthread_attr_t *threadAttr)
{
   LOG_INFO("Main:waitForThreadsToExit: entering");
   void *status;
   int returnCode = 0;

   for (int threadIndex = 0; threadIndex < numFiles; threadIndex++)
   {
      returnCode = pthread_join(*(fileStreamThreads[threadIndex]), &status);
      if (returnCode) 
      {
         LOG_ERROR_ARGS ("Main:waitForThreadsToExit: error %d from pthread_join() for fileStream thread %d",
            returnCode, threadIndex);
         returnCode = -1;
      }

      LOG_INFO_ARGS("Main:waitForThreadsToExit: completed join with fileStream thread %d: status = %ld", 
         threadIndex, (long)status);
   }

   pthread_attr_destroy(threadAttr);
        
   LOG_INFO ("Main:waitForThreadsToExit: exiting");
   return returnCode;
}

int main(int argc, char** argv) 
{
   if (argc < 2)
   {
      usage();
      return 1;
   }
    
   int c;
   int long_options_index; 

   int peekFlag = 0;

   while ((c = getopt_long(argc, argv, "h", long_options, &long_options_index)) != -1) 
   {
       switch (c) 
       {
         case 'h':
         default:
            usage(); 
            return 1;
       }
   }

   LOG_INFO_ARGS ("Main: entering: optind = %d", optind);
   int numFiles = argc-optind;
   LOG_INFO_ARGS ("Main: entering: numFiles = %d", numFiles);
   for (int i=optind; i<argc; i++)
   {
      LOG_INFO_ARGS ("Main: FilePath %d = %s", i, argv[i]); 
   }

   pthread_t **fileStreamThreads;
   pthread_attr_t threadAttr;

   int returnCode = startThreads(numFiles, &argv[optind], &fileStreamThreads, &threadAttr);
   if (returnCode != 0)
   {
      LOG_ERROR ("Main: FATAL ERROR during startThreads: exiting"); 
      exit (-1);
   }

   printf ("Press return to exit...\n\n");
   int myChar = getchar();
   printf ("Exiting...\n");
   g_exit = 1;

  
   returnCode = waitForThreadsToExit(numFiles, fileStreamThreads, &threadAttr);
   if (returnCode != 0)
   {
      LOG_ERROR ("Main: FATAL ERROR during waitForThreadsToExit: exiting"); 
      exit (-1);
   }

   LOG_INFO ("Main: exiting");
   return 0;
}

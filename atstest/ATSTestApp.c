/*
** Copyright (C) 2014  Cable Television Laboratories, Inc.
** Contact: http://www.cablelabs.com/
 

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <errno.h>
#include <mpeg2ts_demux.h>
#include <libts_common.h>
#include <tpes.h>
#include <ebp.h>
#include <pthread.h>
#include <getopt.h>

#include "EBPCommon.h"
#include "EBPFileIngestThread.h"
#include "EBPSegmentAnalysisThread.h"

#include "ATSTestApp.h"


static int g_bPATFound = 0;
static int g_bPMTFound = 0;
static int g_bEBPSearchEnded = 0;
static vqarray_t *g_unfinishedPIDs = NULL;
static vqarray_t *g_ebpStructs = NULL;
static int64_t g_streamStartTimeMsecs;

static int pmt_processor(mpeg2ts_program_t *m2p, void *arg);
static int pat_processor(mpeg2ts_stream_t *m2s, void *arg);

static struct option long_options[] = { 
    { "peek",	   no_argument,        NULL, 'p' }, 
    { "help",       no_argument,        NULL, 'h' }, 
}; 

static char options[] = 
"\t-p, --peek\n" 
"\t-h, --help\n"; 

static void usage() 
{ 
    fprintf(stderr, "\nATSTestApp\n"); 
    fprintf(stderr, "\nUsage: \nnATSTestApp [options] <input file 1> <input file 2> ... <input file N>\n\nOptions:\n%s\n", options);
}

void printBoundaryInfoArray(ebp_boundary_info_t *boundaryInfoArray)
{
   LOG_INFO ("      EBP Boundary Info:");

   for (int i=0; i<EBP_NUM_PARTITIONS; i++)
   {
      if (boundaryInfoArray[i].isBoundary)
      {
         if (boundaryInfoArray[i].isImplicit)
         {
            LOG_INFO_ARGS ("         PARTITION %d: IMPLICIT, PID = %d, FileIndex = %d", i, boundaryInfoArray[i].implicitPID, 
               boundaryInfoArray[i].implicitFileIndex);
         }
         else
         {
            LOG_INFO_ARGS ("         PARTITION %d: EXPLICIT", i);
         }
      }
   }
}

int modBoundaryInfoArray (ebp_descriptor_t * ebpDescriptor, ebp_t *ebp, ebp_boundary_info_t *boundaryInfoArray,
   int currentFileIndex, int currentStreamIndex, program_stream_info_t *programStreamInfo, int numFiles,
   ebp_stream_info_t *videoStreamInfo)
{
   if (ebpDescriptor != NULL)
   {
      printf ("modBoundaryInfoArray: ebpDescriptor!= NULL\n");
      for (int i=0; i<ebpDescriptor->num_partitions; i++)
      {
         ebp_partition_data_t *partition = (ebp_partition_data_t *)vqarray_get(ebpDescriptor->partition_data, i);
         if (partition->partition_id > 9)
         {
            LOG_ERROR("Main:modBoundaryInfoArray: FAIL: PartitionID > 9 detected %s");
            return -1;
         }

         boundaryInfoArray[partition->partition_id].isBoundary = partition->boundary_flag;
         boundaryInfoArray[partition->partition_id].isImplicit = !(partition->ebp_data_explicit_flag);
         boundaryInfoArray[partition->partition_id].implicitPID = partition->ebp_pid;

         // next we need to find which file has the video PID referenced
         if (boundaryInfoArray[partition->partition_id].isImplicit)
         {
            // first check the same file that has the audio stream to see if this PID exists
            int streamIndexTemp;
            int returnCode = getStreamIndex(&(programStreamInfo[currentFileIndex]), partition->ebp_pid, &streamIndexTemp);
            if (returnCode == 0)
            {
               boundaryInfoArray[partition->partition_id].implicitFileIndex = currentFileIndex;
            }
            else
            {
               // PID is not present in same file -- check other files, and pick the first that has the PID
               LOG_INFO_ARGS ("Main:modBoundaryInfoArray: for file %d -- checking other files for implicit PID %d", 
                  currentFileIndex, boundaryInfoArray[partition->partition_id].implicitPID);

               int fileIndexWithPID;
               returnCode = getFileWithPID(programStreamInfo, numFiles, partition->ebp_pid, &fileIndexWithPID);
               if (returnCode != 0)
               {
                  LOG_ERROR_ARGS("Main:modBoundaryInfoArray: FAIL: Cannot find file with video PID %d for implicit EBP for file %d",
                     boundaryInfoArray[partition->partition_id].implicitPID, currentFileIndex);
                  return -1;
               }
               boundaryInfoArray[partition->partition_id].implicitFileIndex = fileIndexWithPID;
            }
         }
      }
   }
   else if (ebp != NULL)
   {
      printf ("modBoundaryInfoArray: ebp_descriptor NULL, ebp != NULL\n");

      if (ebp->ebp_fragment_flag)
      {
         boundaryInfoArray[EBP_PARTITION_FRAGMENT].isBoundary = 1;
         boundaryInfoArray[EBP_PARTITION_FRAGMENT].isImplicit = 0;
         boundaryInfoArray[EBP_PARTITION_FRAGMENT].implicitPID = 0;
      }
      if (ebp->ebp_segment_flag)
      {
         boundaryInfoArray[EBP_PARTITION_SEGMENT].isBoundary = 1;
         boundaryInfoArray[EBP_PARTITION_SEGMENT].isImplicit = 0;
         boundaryInfoArray[EBP_PARTITION_SEGMENT].implicitPID = 0;
      }
      if (ebp->ebp_ext_partition_flag)
      {
         // GORP: different boundaries could be signalled in different EBPs -- all boundaries may
         // not be in one EBP!!
         uint8_t ebp_ext_partitions_temp = ebp->ebp_ext_partitions;
         ebp_ext_partitions_temp = ebp_ext_partitions_temp >> 1; // skip partition1d 0

         // partiton 1 and 2 are not included in the extended partition mask, so skip them
         for (int i=3; i<EBP_NUM_PARTITIONS; i++)
         {
            if (ebp_ext_partitions_temp & 0x1)
            {
               boundaryInfoArray[i].isBoundary = 1;
               boundaryInfoArray[i].isImplicit = 0;
               boundaryInfoArray[i].implicitPID = 0;
            }

            ebp_ext_partitions_temp = ebp_ext_partitions_temp >> 1;
         }
      }
   }
   else
   {
      printf ("modBoundaryInfoArray: both ebp_descriptor and ebp NULL\n");

      if (IS_VIDEO_STREAM((programStreamInfo->stream_types)[currentStreamIndex]))
      {
         LOG_ERROR_ARGS("Main:modBoundaryInfoArray: FAIL: video stream has no EBP descriptor and no EBP in stream for file %d",
            currentFileIndex);
         return -1;
      }
      else  if (IS_AUDIO_STREAM((programStreamInfo->stream_types)[currentStreamIndex]))
      {
         // if audio, set implicit on video -- same partitions as video

         // get video PID
         uint32_t videoPID;
         uint32_t streamTypeTemp;
         int returnCode = getVideoPID(programStreamInfo, &videoPID, &streamTypeTemp);
         if (returnCode != 0)
         {
            // no video stream -- nothing to determine EBP segment boundaries
            LOG_INFO_ARGS ("Main:setupQueues: no video stream for file %d: no EBP info for file", currentFileIndex);
            return -1;
         }

         // copy EBP infro from video to audio, but make each implicit
         for (int i=0; i<EBP_NUM_PARTITIONS; i++)
         {
            boundaryInfoArray[i].isBoundary = videoStreamInfo->ebpBoundaryInfo[i].isBoundary;
            boundaryInfoArray[i].isImplicit = 1;
            boundaryInfoArray[i].implicitFileIndex = currentFileIndex;
            boundaryInfoArray[i].implicitPID = videoPID;
         }
      }
      else
      {
         LOG_ERROR_ARGS("Main:modBoundaryInfoArray: FAIL: stream is not video or audio for file %d",
            currentFileIndex);
         return -1;
      }
   }

   return 0;
}

static int handle_ts_packet(ts_packet_t *ts, elementary_stream_info_t *es_info, void *arg)
{
   return 1;
}

static int handle_pes_packet(pes_packet_t *pes, elementary_stream_info_t *esi, vqarray_t *ts_queue, void *arg)
{
//   printf ("PREREAD: handle_pes_packet\n");
   // Get the first TS packet and check it for EBP
   ts_packet_t *ts = (ts_packet_t*)vqarray_get(ts_queue,0);
   if (ts == NULL)
   {
      // GORP: should this be an error?
      pes_free(pes);
      return 1; // Don't care about this packet
   }
         
   // Read N seconds of data 
   // if we get an ebp struct in each stream before that, then exit.


   // get time and exit if greater that limit
   int64_t currentTimeMsecs = (pes->header.PTS * 1000) / 90000;
   if (g_streamStartTimeMsecs == -1)
   {
      g_streamStartTimeMsecs = currentTimeMsecs;
   }
   else
   {
      if ((currentTimeMsecs - g_streamStartTimeMsecs) > PREREAD_EBP_SEARCH_TIME_MSECS)
      {
         LOG_INFO ("EBP search timed out\n");
         g_bEBPSearchEnded = 1;
      }
   }

   ebp_stream_info_t streamInfo;  // this isnt really used for anything -- just a placeholder in the following
   streamInfo.PID = esi->elementary_PID;
   ebp_t* ebp = getEBP(ts, &streamInfo, -1 /* threadNum */);
         
   // GORP: testing -- removes ebp from audio to test implicit triggering
/*   if (esi->elementary_PID != 481)
   {
      ebp = NULL;
   }
   */

   if (ebp != NULL)
   {         
      int index = -1;
      for (int i=0; i<vqarray_length(g_unfinishedPIDs); i++)
      {
         uint32_t PIDTemp = (uint32_t) vqarray_get(g_unfinishedPIDs, i);

         if (PIDTemp == esi->elementary_PID)
         {
            index = i;
            break;
         }
      }

      if (index >= 0)
      {
         LOG_INFO_ARGS ("PID %d EBP detection complete", esi->elementary_PID);
         vqarray_remove(g_unfinishedPIDs, index);
      }

      // store EBP struct in programStreamInfo (by PID) -- this is used for later EBP boundary analysis
      program_stream_info_t *programStreamInfo = (program_stream_info_t *)arg;
      for (int i=0; i<programStreamInfo->numStreams; i++)
      {
         if ((programStreamInfo->PIDs)[i] == esi->elementary_PID)
         {
            (programStreamInfo->ebps)[i] = ebp_copy(ebp);
         }
      }

     if (vqarray_length(g_unfinishedPIDs) == 0)
      {
         LOG_INFO ("EBP detection complete");
         g_bEBPSearchEnded = 1;
      }

      ebp_free(ebp);
   }

   pes_free(pes);
   return 1;
}

static int pmt_processor(mpeg2ts_program_t *m2p, void *arg)
{
   LOG_DEBUG ("pmt_processor");
   g_bPMTFound = 1;

   if (m2p == NULL || m2p->pmt == NULL) // if we don't have any PSI, there's nothing we can do
      return 0;

   pid_info_t *pi = NULL;
   g_unfinishedPIDs = vqarray_new();
   g_ebpStructs = vqarray_new();

   program_stream_info_t *programStreamInfo = (program_stream_info_t *)arg;
   populateProgramStreamInfo(programStreamInfo, m2p);
            
   for (int i = 0; i < vqarray_length(m2p->pids); i++) // TODO replace linear search w/ hashtable lookup in the future
   {
      if ((pi = vqarray_get(m2p->pids, i)) != NULL)
      {
         int handle_pid = 0;

         descriptor_t *desc = NULL;
         if (IS_VIDEO_STREAM(pi->es_info->stream_type))
         {
            // Look for the SCTE128 descriptor in the ES loop
            for (int d = 0; d < vqarray_length(pi->es_info->descriptors); d++)
            {
               if ((desc = vqarray_get(pi->es_info->descriptors, d)) != NULL)
               {
                  if (desc->tag == 0x97)
                  {
                     mpeg2ts_program_enable_scte128(m2p);
                  }
               }
            }
            handle_pid = 1;
         }
         else if (IS_AUDIO_STREAM(pi->es_info->stream_type))
         {
            handle_pid = 1;
         }
            
         if (handle_pid)
         {
            ebp_descriptor_t* ebpDescriptor = getEBPDescriptor (pi->es_info);
            // GORP: testing -- removes ebpDescriptor to test triggering without descriptor present
//            ebpDescriptor = NULL;
            if (ebpDescriptor == NULL)
            {
               LOG_INFO_ARGS ("Starting EBP detection for PID: %d\n", pi->es_info->elementary_PID);
               vqarray_add(g_unfinishedPIDs, (vqarray_elem_t *)pi->es_info->elementary_PID);
            }
            else
            {
               LOG_INFO_ARGS ("EBP Descriptor present -- no EBP detection necessary for PID: %d\n", pi->es_info->elementary_PID);
            }

            pes_demux_t *pd = pes_demux_new(handle_pes_packet);
            pd->pes_arg = arg;
            pd->pes_arg_destructor = NULL;

            // hook PES demuxer to the PID processor
            demux_pid_handler_t *demux_handler = calloc(1, sizeof(demux_pid_handler_t));
            demux_handler->process_ts_packet = pes_demux_process_ts_packet;
            demux_handler->arg = pd;
            demux_handler->arg_destructor = (arg_destructor_t)pes_demux_free;

            // hook PES demuxer to the PID processor
            demux_pid_handler_t *demux_validator = calloc(1, sizeof(demux_pid_handler_t));
            demux_validator->process_ts_packet = handle_ts_packet;
            demux_validator->arg = arg;
            demux_validator->arg_destructor = NULL;

            // hook PID processor to PID
            mpeg2ts_program_register_pid_processor(m2p, pi->es_info->elementary_PID, demux_handler, demux_validator);
         }
      }
   }

   return 1;
}

static int pat_processor(mpeg2ts_stream_t *m2s, void *arg)
{
   LOG_DEBUG ("pat_processor");
   g_bPATFound = 1;

   for (int i = 0; i < vqarray_length(m2s->programs); i++)
   {
      mpeg2ts_program_t *m2p = vqarray_get(m2s->programs, i);

      if (m2p == NULL) continue;
      m2p->pmt_processor =  (pmt_processor_t)pmt_processor;
      m2p->arg = arg;
   }
   return 1;
}

int prereadFiles(int numFiles, char **fileNames, program_stream_info_t *programStreamInfo)
{
   LOG_INFO ("prereadFiles: entering");

   int num_packets = 4096;
   uint8_t *ts_buf = malloc(TS_SIZE * 4096);

   for (int i=0; i<numFiles; i++)
   {
      printf ("\n");
      LOG_INFO_ARGS ("Main:prereadFiles: FilePath %d = %s", i, fileNames[i]); 

      // reset PAT/PMT read flags
      g_bPATFound = 0;
      g_bPMTFound = 0;
      g_bEBPSearchEnded = 0;
      g_streamStartTimeMsecs = -1;

      FILE *infile = NULL;
      if ((infile = fopen(fileNames[i], "rb")) == NULL)
      {
         LOG_ERROR_ARGS("Main:prereadFiles: Cannot open file %s - %s", fileNames[i], strerror(errno));
         return -1;
      }

      mpeg2ts_stream_t *m2s = NULL;

      if (NULL == (m2s = mpeg2ts_stream_new()))
      {
         LOG_ERROR_ARGS("Main:prereadFiles: Error creating MPEG-2 STREAM object for file %s", fileNames[i]);
         return -1;
      }

       // Register EBP descriptor parser
      // GORP: testing -- comment the following out to removes ebpDescriptor to test triggering without descriptor present
      descriptor_table_entry_t *desc = calloc(1, sizeof(descriptor_table_entry_t));
      desc->tag = EBP_DESCRIPTOR;
      desc->free_descriptor = ebp_descriptor_free;
      desc->print_descriptor = ebp_descriptor_print;
      desc->read_descriptor = ebp_descriptor_read;
      if (!register_descriptor(desc))
      {
         LOG_ERROR_ARGS("Main:prereadFiles: FAIL: Could not register EBP descriptor parser for file %s", fileNames[i]);
         return -1;
      }
      

      m2s->pat_processor = (pat_processor_t)pat_processor;
      m2s->arg = &(programStreamInfo[i]);
      m2s->arg_destructor = NULL;

      while (!(g_bPATFound && g_bPMTFound && g_bEBPSearchEnded) && 
         (num_packets = fread(ts_buf, TS_SIZE, 4096, infile)) > 0)
      {
         for (int i = 0; i < num_packets; i++)
         {
            ts_packet_t *ts = ts_new();
            ts_read(ts, ts_buf + i * TS_SIZE, TS_SIZE);
            mpeg2ts_stream_read_ts_packet(m2s, ts);

            // check if PAT/PMT read -- if so, break out
            if (g_bPATFound && g_bPMTFound && g_bEBPSearchEnded)
            {
               LOG_DEBUG ("Main:prereadFiles: PAT/PMT found");
               break;
            }
         }
      }

      mpeg2ts_stream_free(m2s);

      fclose(infile);


   }

   free (ts_buf);
   LOG_INFO ("Main:prereadFiles: exiting");
   return 0;
}

int teardownQueues(int numFiles, int numStreams, ebp_stream_info_t **streamInfoArray)
{
   LOG_INFO ("Main:teardownQueues: entering");
   int returnCode = 0;

   for (int fileIndex=0; fileIndex < numFiles; fileIndex++)
   {
      for (int streamIndex=0; streamIndex < numStreams; streamIndex++)
      {
         int arrayIndex = get2DArrayIndex (fileIndex, streamIndex, numStreams);
         ebp_stream_info_t *streamInfo = streamInfoArray[arrayIndex];

         LOG_INFO_ARGS ("Main:teardownQueues: fifo (file_index = %d, streamIndex = %d, PID = %d) push_counter = %d, pop_counter = %d", 
            fileIndex, streamIndex, streamInfo->PID, streamInfo->fifo->push_counter, streamInfo->fifo->pop_counter);
         LOG_DEBUG_ARGS ("Main:teardownQueues: calling fifo_destroy for file %d stream %d", 
               fileIndex, streamIndex);
         returnCode = fifo_destroy (streamInfo->fifo);
         if (returnCode != 0)
         {
            LOG_ERROR_ARGS ("Main:teardownQueues: FAIL: error destroying queue for file %d stream %d",
               fileIndex, streamIndex);
            returnCode = -1;
         }

         free (streamInfo->fifo);
         free (streamInfo);
      }
   }

   free (streamInfoArray);

   LOG_INFO ("Main:teardownQueues: exiting");
   return returnCode;
}

int getStreamIndex(program_stream_info_t *programStreamInfo, uint32_t PID, int *streamIndexOut)
{
   for (int streamIndex = 0; streamIndex < programStreamInfo->numStreams; streamIndex++)
   {
      if ((programStreamInfo->PIDs)[streamIndex] == PID)
      {
         *streamIndexOut = streamIndex;
         return 0;
      }
   }

   return -1;
}

int getVideoPID(program_stream_info_t *programStreamInfo, uint32_t *PIDOut, uint32_t *streamType)
{
   int videoFound = 0;
   for (int streamIndex = 0; streamIndex < programStreamInfo->numStreams; streamIndex++)
   {
      if (IS_VIDEO_STREAM((programStreamInfo->stream_types)[streamIndex]))
      {
         if (videoFound)
         {
            LOG_ERROR ("Main:getVideoPID: FAIL: more than one video stream found");
            return -1;
         }

         *PIDOut = (programStreamInfo->PIDs)[streamIndex];
         *streamType = (programStreamInfo->stream_types)[streamIndex];
         videoFound = 1;
      }
   }

   if (videoFound)
   {
      return 0;
   }
   else
   {
      LOG_ERROR ("Main:getVideoPID: FAIL: no video stream found");
      return -1;
   }
}

int getAudioPID(program_stream_info_t *programStreamInfo, char *languageIn, uint32_t PIDIn, uint32_t *PIDOut, uint32_t *streamType)
{
   for (int streamIndex = 0; streamIndex < programStreamInfo->numStreams; streamIndex++)
   {
      if (IS_AUDIO_STREAM((programStreamInfo->stream_types)[streamIndex]))
      {
         if (languageIn != NULL)
         {
            printf ("Using language %s for match\n", languageIn);
            printf ("Using languageComparing language %s to %s\n", languageIn, (programStreamInfo->language)[streamIndex]);
            // use language for match
            if (strcmp(languageIn, (programStreamInfo->language)[streamIndex]) == 0)
            {
               *PIDOut = (programStreamInfo->PIDs)[streamIndex];
               printf ("Match! PIDOut = %d\n", *PIDOut);
               *streamType = (programStreamInfo->stream_types)[streamIndex];
               return 0;
            }
            else
            {
               printf ("No match\n");
            }
         }
         else
         {
            // use PID for match
            if (PIDIn == (programStreamInfo->PIDs)[streamIndex])
            {
               *PIDOut = (programStreamInfo->PIDs)[streamIndex];
               *streamType = (programStreamInfo->stream_types)[streamIndex];
               return 0;
            }
         }
      }
   }

   return -1;
}

varray_t *getUniqueAudioIDArray(int numFiles, program_stream_info_t *programStreamInfo, int *useLanguageAsID)
{
   LOG_INFO ("Main:getUniqueAudioIDArray: entering");

   // we will either use language string or PID as the unique audio stream ID.
   // to decide which to use, check if each stream has unique audio language strings (these are a 
   // concatenation of the language descriptor, the component name descriptor, and the AC3 descriptor)
   *useLanguageAsID = 1;
   for (int fileIndex=0; fileIndex < numFiles; fileIndex++)
   {
      varray_t *audioStreamLanguageArray = varray_new();
      for (int streamIndex = 0; streamIndex < programStreamInfo[fileIndex].numStreams; streamIndex++)
      {
         if (IS_AUDIO_STREAM((programStreamInfo[fileIndex].stream_types)[streamIndex]))
         {
            int foundLanguage = 0;
            for (int i=0; i<varray_length(audioStreamLanguageArray); i++)
            {
               varray_elem_t* element = varray_get(audioStreamLanguageArray, i);
               char *language = *((char **)element);
               printf ("comparing %s to %s\n", language, (programStreamInfo[fileIndex].language)[streamIndex]);
               if (strcmp(language, (programStreamInfo[fileIndex].language)[streamIndex]) == 0)
               {
                  foundLanguage = 1;
                  break;
               }
            }

            if (!foundLanguage)
            {
                LOG_INFO_ARGS ("Main:getUniqueAudioIDArray: (%d, %d), adding language %s", 
                   fileIndex, streamIndex, (programStreamInfo[fileIndex].language)[streamIndex]);

                // add language to array
                varray_add (audioStreamLanguageArray, &((programStreamInfo[fileIndex].language)[streamIndex]));
            }
            else
            {
                LOG_INFO_ARGS ("Main:getUniqueAudioIDArray: (%d, %d), Non-unique language found: %s", 
                  fileIndex, streamIndex, (programStreamInfo[fileIndex].language)[streamIndex]);

                *useLanguageAsID = 0;
            }
         }
      }
      varray_free (audioStreamLanguageArray);
   }

   if (*useLanguageAsID)
   {
      LOG_INFO ("Main:getUniqueAudioIDArray: Constructing audio strea list: using language for ID");
   }
   else
   {
      LOG_INFO ("Main:getUniqueAudioIDArray: Constructing audio strea list: using PID for ID");
   }


  // count how many unique audio streams there are
   varray_t *audioStreamIDArray = varray_new();
   for (int fileIndex=0; fileIndex < numFiles; fileIndex++)
   {
      for (int streamIndex = 0; streamIndex < programStreamInfo[fileIndex].numStreams; streamIndex++)
      {
         if (IS_AUDIO_STREAM((programStreamInfo[fileIndex].stream_types)[streamIndex]))
         {
            int foundID = 0;
            for (int i=0; i<varray_length(audioStreamIDArray); i++)
            {
               varray_elem_t* element = varray_get(audioStreamIDArray, i);
               if (*useLanguageAsID)
               {
                  char *language = *((char **)element);
                  if (strcmp(language, (programStreamInfo[fileIndex].language)[streamIndex]) == 0)
                  {
                     foundID = 1;
                     break;
                  }
               }
               else
               {
                  uint32_t PID = *((uint32_t *)element);
                  if (PID == (programStreamInfo[fileIndex].PIDs)[streamIndex])
                  {
                     foundID = 1;
                     break;
                  }
               }
            }

            if (!foundID)
            {
               if (*useLanguageAsID)
               {
                  LOG_INFO_ARGS ("Main:getUniqueAudioIDArray: (%d, %d), adding Language %s", 
                     fileIndex, streamIndex, (programStreamInfo[fileIndex].language)[streamIndex]);

                  // add language here
                  varray_add (audioStreamIDArray, &((programStreamInfo[fileIndex].language)[streamIndex]));
               }
               else
               {
                  LOG_INFO_ARGS ("Main:getUniqueAudioIDArray: (%d, %d), adding PID %d", 
                     fileIndex, streamIndex, (programStreamInfo[fileIndex].PIDs)[streamIndex]);

                  // add PID here
                  varray_add (audioStreamIDArray, &((programStreamInfo[fileIndex].PIDs)[streamIndex]));
               }
            }
            else
            {
               if (*useLanguageAsID)
               {
                   LOG_INFO_ARGS ("Main:getUniqueAudioIDArray: (%d, %d), NOT adding language %s", 
                     fileIndex, streamIndex, (programStreamInfo[fileIndex].language)[streamIndex]);
               }
               else
               {
                   LOG_INFO_ARGS ("Main:getUniqueAudioIDArray: (%d, %d), NOT adding PID %d", 
                     fileIndex, streamIndex, (programStreamInfo[fileIndex].PIDs)[streamIndex]);
               }
            }
         }
      }
   }
                
   LOG_INFO ("Main:getUniqueAudioIDArray: exting");
   return audioStreamIDArray;
}

int getFileWithPID(program_stream_info_t *programStreamInfo, int numFiles, uint32_t PID, 
                        int *fileIndexWithPID)
{
   // returns first file index with a PID matching the input PID
   LOG_INFO ("Main:getFileWithVideoPID: entering");

   int returnCode;
   int streamIndexTemp;

   for (int fileIndex=0; fileIndex < numFiles; fileIndex++)
   {
      returnCode = getStreamIndex(&(programStreamInfo[fileIndex]), PID, &streamIndexTemp);
      if (returnCode == 0)
      {
         // DONE
         *fileIndexWithPID = fileIndex;
         return 0;
      }
   }

   return -1;
}

int setupQueues(int numFiles, char **fileNames, program_stream_info_t *programStreamInfo,
                ebp_stream_info_t ***streamInfoArray, int *numStreamsPerFile)
{
   LOG_INFO ("Main:setupQueues: entering");

   int returnCode;

   // we need to set up a 2D array of queueus.  The array size is numFiles x numStreams
   // where numStreams is the total of all possible streams (video + all possible audio).
   // so if one file contains an audio stream and another file contains two other DIFFERENT
   // audio files, then the total number of streams is 4 (video + 3 audio).

   // Distinguish audio streams by either language or PID.  First check if the audio streams in
   // each file have unique languages -- if so, use the language as the means to match audio streams
   // across files.  If languages are NOT unique, use PID to match audio streams across files.
   // Here, the language is a concatenation of the language descriptor, the component name descriptor 
   // and the AC3 descriptor.
   

   // count how many unique audio streams there are
   int useLanguageAsID;
   varray_t *audioStreamIDArray = getUniqueAudioIDArray(numFiles, programStreamInfo, &useLanguageAsID);
   int numStreams = varray_length(audioStreamIDArray) + 1 /*video stream */;
   LOG_INFO_ARGS ("Main:setupQueues: numStreams = %d", numStreams);

   *numStreamsPerFile = numStreams;


   // *streamInfoArray is an array of stream_info pointers, not stream_infos.  if a particular stream_info is not
   // applicable for a file, that stream_info pointer can be left null.  In what follows we allocate stream info
   // objets for each strem within a file.  The first stream info is always the video, and then next ones are audio -- we
   // use the list of unique audio streams in the fileset obtained above to order the audio stream info list.
   // if a file does not contain an audio stream, the stream info object for that stream is left NULL -- this
   // is how the analysis threads know which stream info objects apply to a file.
   *streamInfoArray = (ebp_stream_info_t **) calloc (numFiles * numStreams, sizeof (ebp_stream_info_t*));

   for (int fileIndex=0; fileIndex < numFiles; fileIndex++)
   {
      ebp_stream_info_t *videoStreamInfo = NULL;
      for (int streamIndex=0; streamIndex < numStreams; streamIndex++)
      {
         int arrayIndex = get2DArrayIndex (fileIndex, streamIndex, numStreams);

         uint32_t PID;
         uint32_t streamType;
         if (streamIndex == 0)
         {
            returnCode = getVideoPID(&(programStreamInfo[fileIndex]), &PID, &streamType);
            if (returnCode != 0)
            {
               // no video stream -- this is legal
               LOG_INFO_ARGS ("Main:setupQueues: no video stream for file %d", fileIndex);
               continue;
            }
         }
         else 
         {
            char *language = NULL;
            uint32_t PIDTemp = 0;  
            if (useLanguageAsID)
            {
               language = *((char **)varray_get (audioStreamIDArray, streamIndex-1));
            }
            else
            {
               PIDTemp = *((uint32_t *)varray_get (audioStreamIDArray, streamIndex-1));
            }

            PID = 0;
            returnCode = getAudioPID(&(programStreamInfo[fileIndex]), language, PIDTemp, &PID, &streamType);
            if (returnCode != 0)
            {
               // this audio stream doesn't exist, so leave stream info NULL
               LOG_INFO_ARGS ("Main:setupQueues: (%d, %d) skipping fifo creation for PID %d", 
                  fileIndex, streamIndex, PID);
               continue;
            }         
         }

         LOG_INFO_ARGS ("Main:setupQueues: (%d, %d) creating fifo for PID %d", 
                  fileIndex, streamIndex, PID);

        (*streamInfoArray)[arrayIndex] = (ebp_stream_info_t *)calloc (1, sizeof (ebp_stream_info_t));
         ebp_stream_info_t *streamInfo = (*streamInfoArray)[arrayIndex];
         if (streamIndex == 0)
         {
            videoStreamInfo = streamInfo;
         }

         streamInfo->fifo = (thread_safe_fifo_t *)calloc (1, sizeof (thread_safe_fifo_t));

         LOG_DEBUG_ARGS ("Main:setupQueues: calling fifo_create for file %d, stream %d", 
               fileIndex, streamIndex);
         returnCode = fifo_create (streamInfo->fifo, fileIndex);
         if (returnCode != 0)
         {
            LOG_ERROR_ARGS ("Main:setupQueues: error creating fifo for file %d, stream %d", 
               fileIndex, streamIndex);
            return -1;
         }

         streamInfo->PID = PID;
         streamInfo->isVideo = IS_VIDEO_STREAM(streamType);

         streamInfo->lastVideoChunkPTS = 0;
         streamInfo->lastVideoChunkPTSValid = 0;
         streamInfo->streamPassFail = 1;

         streamInfo->ebpBoundaryInfo = setupDefaultBoundaryInfoArray();
         returnCode = modBoundaryInfoArray (programStreamInfo[fileIndex].ebpDescriptors[streamIndex], 
            programStreamInfo[fileIndex].ebps[streamIndex],
            streamInfo->ebpBoundaryInfo, fileIndex, streamIndex, programStreamInfo, numFiles,
            videoStreamInfo);
         if (returnCode != 0)
         {
            LOG_ERROR_ARGS ("Main:setupQueues: error modifying boundary info for file %d, stream %d", 
               fileIndex, streamIndex);
            return -1;
         }
      }
   }

   varray_free(audioStreamIDArray);
   LOG_INFO ("Main:setupQueues: exiting");
   return 0;
}

int startThreads(int numFiles, int totalNumStreams, ebp_stream_info_t **streamInfoArray, char **fileNames,
   int *filePassFails, pthread_t ***fileIngestThreads, pthread_t ***analysisThreads, pthread_attr_t *threadAttr)
{
   LOG_INFO ("Main:startThreads: entering");

   int returnCode = 0;

   // one worker thread per file, one analysis thread per streamtype
   // num worker thread = numFiles
   // num analysis threads = totalNumStreams

   pthread_attr_init(threadAttr);
   pthread_attr_setdetachstate(threadAttr, PTHREAD_CREATE_JOINABLE);


   // start analyzer threads
   *analysisThreads = (pthread_t **) calloc (totalNumStreams, sizeof(pthread_t*));
   for (int threadIndex = 0; threadIndex < totalNumStreams; threadIndex++)
   {
      // this is freed by the analysis thread when it exits
      ebp_stream_info_t **streamInfos = (ebp_stream_info_t **)calloc(numFiles, sizeof (ebp_stream_info_t*));
      for (int i=0; i<numFiles; i++)
      {
         int arrayIndex = get2DArrayIndex (i, threadIndex, totalNumStreams);

         streamInfos[i] = streamInfoArray[arrayIndex];
      }
      
      ebp_segment_analysis_thread_params_t *ebpSegmentAnalysisThreadParams = (ebp_segment_analysis_thread_params_t *)malloc (sizeof(ebp_segment_analysis_thread_params_t));
      ebpSegmentAnalysisThreadParams->threadID = 100 + threadIndex;
      ebpSegmentAnalysisThreadParams->numFiles = numFiles;
      ebpSegmentAnalysisThreadParams->streamInfos = streamInfos;

      (*analysisThreads)[threadIndex] = (pthread_t *)malloc (sizeof (pthread_t));
      pthread_t *analyzerThread = (*analysisThreads)[threadIndex];
      LOG_INFO_ARGS("Main:startThreads: creating analyzer thread %d", threadIndex);
      returnCode = pthread_create(analyzerThread, threadAttr, EBPSegmentAnalysisThreadProc, (void *)ebpSegmentAnalysisThreadParams);
      if (returnCode)
      {
         LOG_ERROR_ARGS("Main:startThreads: FAIL: error %d creating analyzer thread %d", 
            returnCode, threadIndex);
          return -1;
      }
   }


   // start the file ingest threads
   *fileIngestThreads = (pthread_t **) calloc (numFiles, sizeof(pthread_t*));
   for (int threadIndex = 0; threadIndex < numFiles; threadIndex++)
   {
      int arrayIndex = get2DArrayIndex (threadIndex, 0, totalNumStreams);
      ebp_stream_info_t **streamInfos = &(streamInfoArray[arrayIndex]);
             
      // pass ALL stream infos so that threads can do implicit triggering on each other
      ebp_file_ingest_thread_params_t *ebpFileIngestThreadParams = (ebp_file_ingest_thread_params_t *)malloc (sizeof(ebp_file_ingest_thread_params_t));
      ebpFileIngestThreadParams->threadNum = threadIndex;  // same as file index
      ebpFileIngestThreadParams->numStreams = totalNumStreams;
      ebpFileIngestThreadParams->numFiles = numFiles;
      ebpFileIngestThreadParams->allStreamInfos = streamInfoArray;
      ebpFileIngestThreadParams->filePath = fileNames[threadIndex];
      ebpFileIngestThreadParams->filePassFail = &(filePassFails[threadIndex]);

      (*fileIngestThreads)[threadIndex] = (pthread_t *)malloc (sizeof (pthread_t));
      pthread_t *fileIngestThread = (*fileIngestThreads)[threadIndex];
      LOG_INFO_ARGS("Main:startThreads: creating fileIngest thread %d", threadIndex);
      returnCode = pthread_create(fileIngestThread, threadAttr, EBPFileIngestThreadProc, (void *)ebpFileIngestThreadParams);
      if (returnCode)
      {
         LOG_ERROR_ARGS("Main:startThreads: FAIL: error %d creating fileIngest thread %d", 
            returnCode, threadIndex);
          return -1;
      }
  }

   LOG_INFO("Main:startThreads: exiting");
   return 0;
}
  
int waitForThreadsToExit(int numFiles, int totalNumStreams,
   pthread_t **fileIngestThreads, pthread_t **analysisThreads, pthread_attr_t *threadAttr)
{
   LOG_INFO("Main:waitForThreadsToExit: entering");
   void *status;
   int returnCode = 0;

   for (int threadIndex = 0; threadIndex < totalNumStreams; threadIndex++)
   {
      returnCode = pthread_join(*(analysisThreads[threadIndex]), &status);
      if (returnCode) 
      {
         LOG_ERROR_ARGS ("Main:waitForThreadsToExit: error %d from pthread_join() for analysis thread %d",
            returnCode, threadIndex);
         returnCode = -1;
      }

      LOG_INFO_ARGS("Main:waitForThreadsToExit: completed join with analysis thread %d: status = %ld", 
         threadIndex, (long)status);
   }

   for (int threadIndex = 0; threadIndex < numFiles; threadIndex++)
   {
      returnCode = pthread_join(*(fileIngestThreads[threadIndex]), &status);
      if (returnCode) 
      {
         LOG_ERROR_ARGS ("Main:waitForThreadsToExit: error %d from pthread_join() for fileIngest thread %d",
            returnCode, threadIndex);
         returnCode = -1;
      }

      LOG_INFO_ARGS("Main:waitForThreadsToExit: completed join with fileIngest thread %d: status = %ld", 
         threadIndex, (long)status);
   }

   pthread_attr_destroy(threadAttr);
        
   LOG_INFO ("Main:waitForThreadsToExit: exiting");
   return returnCode;
}

void analyzeResults(int numFiles, int numStreams, ebp_stream_info_t **streamInfoArray, char **fileNames,
   int *filePassFails)
{
   LOG_INFO ("");
   LOG_INFO ("");
   LOG_INFO ("TEST RESULTS");
   LOG_INFO ("");

   for (int i=0; i<numFiles; i++)
   {
      LOG_INFO_ARGS ("File %s", fileNames[i]);
      int overallPassFail = filePassFails[i];
      for (int j=0; j<numStreams; j++)
      {
         int arrayIndex = get2DArrayIndex (i, j, numStreams);
         ebp_stream_info_t *streamInfo = streamInfoArray[arrayIndex];
         overallPassFail &= streamInfo->streamPassFail;
      }

      LOG_INFO_ARGS ("   File PassFail Result: %s", (overallPassFail?"PASS":"FAIL"));
      LOG_INFO ("   Stream PassFail Results:");
      for (int j=0; j<numStreams; j++)
      {
         int arrayIndex = get2DArrayIndex (i, j, numStreams);
         ebp_stream_info_t *streamInfo = streamInfoArray[arrayIndex];

         if (streamInfo == NULL)
         {
            // if stream is absent from this file
            continue;
         }

         LOG_INFO_ARGS ("      PID %d (%s): %s", streamInfo->PID, (streamInfo->isVideo?"VIDEO":"AUDIO"),
            (streamInfo->streamPassFail?"PASS":"FAIL"));

         printBoundaryInfoArray(streamInfo->ebpBoundaryInfo);
      }
      LOG_INFO ("");

   }

   LOG_INFO ("TEST RESULTS END");
   LOG_INFO ("");
   LOG_INFO ("");
}

void printStreamInfo(int numFiles, int numStreams, ebp_stream_info_t **streamInfoArray, char **fileNames)
{
   LOG_INFO ("");
   LOG_INFO ("");
   LOG_INFO ("STREAM INFO");
   LOG_INFO ("");

   for (int i=0; i<numFiles; i++)
   {
      LOG_INFO_ARGS ("File %s", fileNames[i]);
      LOG_INFO ("   Stream:");
      for (int j=0; j<numStreams; j++)
      {
         int arrayIndex = get2DArrayIndex (i, j, numStreams);
         ebp_stream_info_t *streamInfo = streamInfoArray[arrayIndex];

         if (streamInfo == NULL)
         {
            // if stream is absent from this file
            continue;
         }

         LOG_INFO_ARGS ("      PID %d (%s): %s", streamInfo->PID, (streamInfo->isVideo?"VIDEO":"AUDIO"),
            (streamInfo->streamPassFail?"PASS":"FAIL"));

         printBoundaryInfoArray(streamInfo->ebpBoundaryInfo);
      }
      LOG_INFO ("");

   }

   LOG_INFO ("TEST RESULTS END");
   LOG_INFO ("");
   LOG_INFO ("");
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

   while ((c = getopt_long(argc, argv, "ph", long_options, &long_options_index)) != -1) 
   {
       switch (c) 
       {
         case 'p':
            peekFlag = 1; 
            break;         
         case 'h':
         default:
            usage(); 
            return 1;
       }
   }


   LOG_INFO_ARGS ("Main: entering: optind = %d", optind);
   int numFiles = argc-optind;
   for (int i=optind; i<argc; i++)
   {
      LOG_INFO_ARGS ("Main: FilePath %d = %s", i, argv[i]); 
   }

   program_stream_info_t *programStreamInfo = (program_stream_info_t *)calloc (numFiles, 
      sizeof (program_stream_info_t));
   int *filePassFails = calloc(numFiles, sizeof(int));
   for (int i=0; i<numFiles; i++)
   {
      filePassFails[i] = 1;
   }

   int returnCode = prereadFiles(numFiles, &argv[optind], programStreamInfo);
   if (returnCode != 0)
   {
      LOG_ERROR ("Main: FATAL ERROR during prereadFiles: exiting"); 
      exit (-1);
   }

   // array of fifo pointers
   ebp_stream_info_t **streamInfoArray = NULL;
   int numStreamsPerFile;
   returnCode = setupQueues(numFiles, &argv[optind], programStreamInfo, &streamInfoArray, &numStreamsPerFile);
   if (returnCode != 0)
   {
      LOG_ERROR ("Main: FATAL ERROR during setupQueues: exiting"); 
      exit (-1);
   }

   if (peekFlag)
   {
      printStreamInfo(numFiles, numStreamsPerFile, streamInfoArray, &argv[optind]);
      return 0;
   }

   pthread_t **fileIngestThreads;
   pthread_t **analysisThreads;
   pthread_attr_t threadAttr;

   returnCode = startThreads(numFiles, numStreamsPerFile, streamInfoArray, &argv[optind], filePassFails,
      &fileIngestThreads, &analysisThreads, &threadAttr);
   if (returnCode != 0)
   {
      LOG_ERROR ("Main: FATAL ERROR during startThreads: exiting"); 
      exit (-1);
   }


   returnCode = waitForThreadsToExit(numFiles, numStreamsPerFile, fileIngestThreads, analysisThreads, &threadAttr);
   if (returnCode != 0)
   {
      LOG_ERROR ("Main: FATAL ERROR during waitForThreadsToExit: exiting"); 
      exit (-1);
   }

   // analyze the pass fail results
   analyzeResults(numFiles, numStreamsPerFile, streamInfoArray, &argv[optind], filePassFails);


   returnCode = teardownQueues(numFiles, numStreamsPerFile, streamInfoArray);
   if (returnCode != 0)
   {
      LOG_ERROR ("Main: FATAL ERROR during teardownQueues: exiting"); 
      exit (-1);
   }

   freeProgramStreamInfo (programStreamInfo);
   free (filePassFails);

   pthread_exit(NULL);

   LOG_INFO ("Main: exiting");
   return 0;
}


void testsParseNTPTimestamp()
{
   uint32_t numSeconds;
   float fractionalSecond;

   uint64_t ntpTime = 0x0000000180000000;

   parseNTPTimestamp(ntpTime, &numSeconds, &fractionalSecond);

   printf ("numSeconds = %u, fractionalSecond = %f\n", numSeconds, fractionalSecond);
}

ebp_boundary_info_t *setupDefaultBoundaryInfoArray()
{
   // this sets all partitions to no-boundary -- the correct values are filled in later
   ebp_boundary_info_t * boundaryInfoArray = (ebp_boundary_info_t *) calloc (EBP_NUM_PARTITIONS, sizeof(ebp_boundary_info_t));

   for (int i=0; i<EBP_NUM_PARTITIONS; i++)
   {
      boundaryInfoArray[i].queueLastImplicitPTS = varray_new();
   }

   return boundaryInfoArray;
}

void aphabetizeStringArray(char **stringArray, int stringArraySz)
{
   // bubble sort
   if (stringArraySz <= 1)
   {
      return;
   }

   int done = 0;
   while (!done)
   {
      done = 1;
      for (int i=1; i<stringArraySz; i++)
      {
         if (strcmp(stringArray[i-1], stringArray[i]) > 0)
         {
            char *temp = stringArray[i-1];
            stringArray[i-1] = stringArray[i];
            stringArray[i] = temp;

            done = 0;
         }
      }
   }
}

void aphabetizeLanguageDescriptorLanguages (language_descriptor_t* languageDescriptor)
{
   // bubble sort
   if (languageDescriptor->num_languages <= 1)
   {
      return;
   }

   int done = 0;
   while (!done)
   {
      done = 1;
      for (int i=1; i<languageDescriptor->num_languages; i++)
      {
         if (strcmp(languageDescriptor->languages[i-1].ISO_639_language_code, 
            languageDescriptor->languages[i].ISO_639_language_code) > 0)
         {
            iso639_lang_t temp = languageDescriptor->languages[i-1];
            languageDescriptor->languages[i-1] = languageDescriptor->languages[i];
            languageDescriptor->languages[i] = temp;

            done = 0;
         }
      }
   }
}

void freeProgramStreamInfo(program_stream_info_t *programStreamInfo)
{
   for (int i=0; i<programStreamInfo->numStreams; i++)
   {
      free ((programStreamInfo->language)[i]);
      ebp_free (programStreamInfo->ebps[i]);
      ebp_descriptor_free ((descriptor_t *)((programStreamInfo->ebpDescriptors)[i]));
   }

   free (programStreamInfo->stream_types);
   free (programStreamInfo->PIDs);
   free (programStreamInfo->ebpDescriptors);
   free (programStreamInfo->ebps);
   free (programStreamInfo->language);

   free (programStreamInfo);
}

void populateProgramStreamInfo(program_stream_info_t *programStreamInfo, mpeg2ts_program_t *m2p)
{
   programStreamInfo->numStreams = vqarray_length(m2p->pids);

   programStreamInfo->stream_types = (uint32_t*) calloc (programStreamInfo->numStreams, sizeof (uint32_t));
   programStreamInfo->PIDs = (uint32_t*) calloc (programStreamInfo->numStreams, sizeof (uint32_t));
   programStreamInfo->ebpDescriptors = (ebp_descriptor_t**) calloc (programStreamInfo->numStreams, sizeof (ebp_descriptor_t *));
   programStreamInfo->ebps = (ebp_t**) calloc (programStreamInfo->numStreams, sizeof (ebp_t *));
   programStreamInfo->language = (char**) calloc (programStreamInfo->numStreams, sizeof (char *));

   pid_info_t *pi;

   for (int j = 0; j < vqarray_length(m2p->pids); j++)
   {
      if ((pi = vqarray_get(m2p->pids, j)) != NULL)
      {
         LOG_INFO_ARGS ("Main:prereadFiles: stream %d: stream_type = %u, elementary_PID = %u, ES_info_length = %u",
            j, pi->es_info->stream_type, pi->es_info->elementary_PID, pi->es_info->ES_info_length);

         programStreamInfo->stream_types[j] = pi->es_info->stream_type;
         programStreamInfo->PIDs[j] = pi->es_info->elementary_PID;

         ebp_descriptor_t* ebpDescriptor = getEBPDescriptor (pi->es_info);
         if (ebpDescriptor != NULL)
         {
            ebp_descriptor_print_stdout (ebpDescriptor);
            programStreamInfo->ebpDescriptors[j] = ebp_descriptor_copy(ebpDescriptor);
         }
         else
         {
            LOG_INFO ("NULL ebp_descriptor");
         }

         language_descriptor_t* languageDescriptor = getLanguageDescriptor (pi->es_info);
         component_name_descriptor_t* componentNameDescriptor = getComponentNameDescriptor (pi->es_info);
         ac3_descriptor_t* ac3Descriptor = getAC3Descriptor (pi->es_info);
         int languageStringSz = 3; // commas + NULL terminator

         if (languageDescriptor != NULL)
         {
            for (int ii=0; ii<languageDescriptor->num_languages; ii++)
            {
               languageStringSz += 4; // 3 chars plus a : to separate languages
            }
         }
         if (componentNameDescriptor != NULL && componentNameDescriptor->num_names)
         {
//            void sortStringArray(componentNameDescriptor->names, componentNameDescriptor->numNames);
            for (int ii=0; ii<componentNameDescriptor->num_names; ii++)
            {
               languageStringSz += strlen(componentNameDescriptor->names[ii]) + 1;  // strlen plus a : to separate names
            }
         }
         if (ac3Descriptor != NULL)
         {
            languageStringSz += 3;
         }
            
         programStreamInfo->language[j] = (char *)calloc (languageStringSz, 1);

         if (languageDescriptor != NULL)
         {
            // alphabetize multiple language strings in case they are in a different
            // order in different streams
            aphabetizeLanguageDescriptorLanguages (languageDescriptor);
            for (int ii=0; ii<languageDescriptor->num_languages; ii++)
            {
               strcat (programStreamInfo->language[j], languageDescriptor->languages[ii].ISO_639_language_code);
               strcat (programStreamInfo->language[j], ":");
            }

            LOG_INFO_ARGS ("Num Languages = %d, Language = %s", languageDescriptor->num_languages, programStreamInfo->language[j]);
         }
         strcat (programStreamInfo->language[j], ",");

         if (componentNameDescriptor != NULL && componentNameDescriptor->num_names)
         {
            aphabetizeStringArray(componentNameDescriptor->names, componentNameDescriptor->num_names);
            for (int ii=0; ii<componentNameDescriptor->num_names; ii++)
            {
               strcat (programStreamInfo->language[j], componentNameDescriptor->names[ii]);               
               strcat (programStreamInfo->language[j], ":");
            }
         }
/*         else
         {
            // GORP:testing: tests discrimination by language by making a unique language per stream
            char temp[10];
            sprintf (temp, "%d", pi->es_info->elementary_PID);
            strcat (programStreamInfo[i].language[j], temp);               
         }
         */
         strcat (programStreamInfo->language[j], ",");

         if (ac3Descriptor != NULL)
         {
            strcat (programStreamInfo->language[j], ac3Descriptor->language);               
         }
               
         LOG_INFO_ARGS ("Language = %s", programStreamInfo->language[j]);
      }
   }
}
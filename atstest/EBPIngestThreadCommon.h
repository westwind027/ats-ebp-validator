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

#ifndef __H_EBP_INGEST_THREAD_COMMON_
#define __H_EBP_INGEST_THREAD_COMMON_

#include "ThreadSafeFIFO.h"
#include <tpes.h>
#include <hashtable.h>
#include "scte35.h"

typedef struct 
{
    int threadNum;
    int numStreams;
    int numIngests;

    ebp_stream_info_t **allStreamInfos;

    int *ingestPassFail;

    uint64_t currentVideoPTS;
    hashtable_t *mapOldSCTE35SpliceInserts;  //hash map mapping eventIDs to latest PTS

    psi_table_buffer_t scte35TableBuffer;  // used for SCTE35 tabels that span mult TS packets

} ebp_ingest_thread_params_t;

int postToFIFO (uint64_t PTS, uint32_t sapType, ebp_t *ebp, ebp_descriptor_t *ebpDescriptor, uint32_t PID, 
                 uint8_t partitionId, int threadNum, int numStreams, ebp_stream_info_t **allStreamInfos);

uint64_t adjustPTSForTests (uint64_t PTSIn, int fileIndex, ebp_stream_info_t * streamInfo);
int ingest_pat_processor(mpeg2ts_stream_t *m2s, void *arg);
int ingest_pmt_processor(mpeg2ts_program_t *m2p, void *arg);

ebp_descriptor_t* getEBPDescriptor (elementary_stream_info_t *esi);
component_name_descriptor_t* getComponentNameDescriptor (elementary_stream_info_t *esi);
language_descriptor_t* getLanguageDescriptor (elementary_stream_info_t *esi);
ac3_descriptor_t* getAC3Descriptor (elementary_stream_info_t *esi);

void findFIFO (uint32_t PID, ebp_stream_info_t **streamInfos, int numStreams,
   thread_safe_fifo_t**fifoOut, int *fifoIndex);
ebp_t* getEBP(ts_packet_t *ts, ebp_stream_info_t * streamInfo, int threadNum);
int detectBoundary(int threadNum, ebp_t* ebp, ebp_stream_info_t *streamInfo, uint64_t PTS, int *isBoundary,
                   hashtable_t *mapOldSCTE35SpliceInserts);
void triggerImplicitBoundaries (int threadNum, ebp_stream_info_t **streamInfoArray, int numStreams, int numFiles,
   int currentStreamInfoIndex, uint64_t PTS, uint8_t partitionId, int fileIndex);

uint32_t getSAPType(pes_packet_t *pes, ts_packet_t *first_ts,  uint32_t streamType);
uint32_t getSAPType_AVC(pes_packet_t *pes, ts_packet_t *first_ts);
uint32_t getSAPType_MPEG2_AAC(pes_packet_t *pes, ts_packet_t *first_ts);
uint32_t getSAPType_MPEG4_AAC(pes_packet_t *pes, ts_packet_t *first_ts);
uint32_t getSAPType_AC3(pes_packet_t *pes, ts_packet_t *first_ts);
uint32_t getSAPType_MPEG2_VIDEO(pes_packet_t *pes, ts_packet_t *first_ts);

void addSCTE35Point_AllBoundaries (int threadNum, ebp_stream_info_t *streamInfo, scte35_splice_info_section *scte35InfoSection);
void checkPTSAgainstSCTE35Points_AllBoundaries (int threadNum, ebp_stream_info_t *streamInfo, uint64_t PTS,
                                                hashtable_t *mapOldSCTE35SpliceInserts);
uint64_t getSCTE35PTS (scte35_splice_info_section *scte35InfoSection, uint32_t PID);

void addSCTE35Point (varray_t* scte35List, scte35_splice_info_section *scte35InfoSection, int threadNum, int partitionID, uint32_t PID);
int checkPTSAgainstSCTE35Points (varray_t* scte35List, uint64_t PTS, uint64_t deltaSCTE35PTS, int threadNum,
                                  int partitionID, uint32_t PID, hashtable_t *mapOldSCTE35SpliceInserts);
int checkEBPAgainstSCTE35Points (varray_t* scte35List, uint64_t PTS, uint64_t deltaSCTE35PTS, int threadNum,
                                  int partitionID, uint32_t PID, hashtable_t *mapOldSCTE35SpliceInserts);
void pruneOldSCTE35Map(hashtable_t *mapOldSCTE35SpliceInserts, uint64_t currentPTS);



#endif  // __H_EBP_INGEST_THREAD_COMMON_

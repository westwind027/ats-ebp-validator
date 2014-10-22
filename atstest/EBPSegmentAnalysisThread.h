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

#ifndef __H_EBPSEGMENTANALYSISTHREAD_67511FLKKJF
#define __H_EBPSEGMENTANALYSISTHREAD_67511FLKKJF

#include "EBPCommon.h"

typedef struct 
{  
   int threadID;  // GORP: put these in debug statements
   int numStreamInfos;
//   thread_safe_fifo_t **fifos;
   ebp_stream_info_t **streamInfos;

} ebp_segment_analysis_thread_params_t;

typedef struct
{
   int64_t PTS;
   uint32_t SAPType;

   ebp_t *EBP;
   ebp_descriptor_t *latestEBPDescriptor;  // GORP: may need to copy this else ingest code frees it?

} ebp_segment_info_t;


void cleanupEBPSegmentInfo (ebp_segment_info_t *ebpSegmentInfo);
void *EBPSegmentAnalysisThreadProc(void *threadParams);
int syncIncomingStreams (int threadID, int numStreamInfos, ebp_stream_info_t **streamInfos, int *fifoNotActive);


#endif  // __H_EBPSEGMENTANALYSISTHREAD_67511FLKKJF
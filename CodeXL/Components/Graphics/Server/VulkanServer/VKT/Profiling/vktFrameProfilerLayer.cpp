//==============================================================================
/// Copyright (c) 2016 Advanced Micro Devices, Inc. All rights reserved.
/// \author AMD Developer Tools Team
/// \file   vktFrameProfilerLayer.cpp
/// \brief  The Vulkan-specific Frame Profiler layer implementation.
//==============================================================================

#include "vktFrameProfilerLayer.h"
#include "../vktLayerManager.h"
#include "../Tracing/vktTraceAnalyzerLayer.h"
#include "../Objects/vktObjectDatabaseProcessor.h"
#include "../Objects/Wrappers/vktWrappedCmdBuf.h"
#include "../Objects/Wrappers/vktWrappedQueue.h"
#include "../vktInterceptManager.h"
#include "../Profiling/vktTimestampedCmdBuf.h"
#include "../vktDefines.h"

//-----------------------------------------------------------------------------
/// Default constructor for VktFrameProfilerLayer.
//-----------------------------------------------------------------------------
VktFrameProfilerLayer::VktFrameProfilerLayer()
    : ModernAPIFrameProfilerLayer()
{
    SetProfiledFuncs();
}

//-----------------------------------------------------------------------------
/// Retrieve a pointer to the parent LayerManager used by this tool.
/// \returns A pointer to the parent LayerManager used by this tool.
//-----------------------------------------------------------------------------
ModernAPILayerManager* VktFrameProfilerLayer::GetParentLayerManager()
{
    return VktLayerManager::GetLayerManager();
}

//-----------------------------------------------------------------------------
/// Clear out all of the ProfilerResults collected by a VktCmdBufProfiler.
/// IMPORTANT NOTE: All worker threads accessing profile results data
/// must have terminated and results must have been gathered and sent back
/// to the client before calling this function
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::ClearProfilingResults()
{
    ProfilerResultsMap::iterator itemIter;
    ProfilerResultsMap::iterator beginIter = mEntriesWithProfilingResults.begin();
    ProfilerResultsMap::iterator endIter = mEntriesWithProfilingResults.end();

    for (itemIter = beginIter; itemIter != endIter; ++itemIter)
    {
        QueueWrapperToProfilingResultsMap threadMap = itemIter->second;
        QueueWrapperToProfilingResultsMap::iterator threadIter;
        QueueWrapperToProfilingResultsMap::iterator threadBegin = threadMap.begin();
        QueueWrapperToProfilingResultsMap::iterator threadEnd = threadMap.end();

        // Kill the map associated with each Queue type.
        for (threadIter = threadBegin; threadIter != threadEnd; ++threadIter)
        {
            SampleIdToProfilerResultMap* pResultMap = threadIter->second;

            SampleIdToProfilerResultMap::iterator resultIter;
            SampleIdToProfilerResultMap::iterator resultBegin = pResultMap->begin();
            SampleIdToProfilerResultMap::iterator resultEnd = pResultMap->end();

            for (resultIter = resultBegin; resultIter != resultEnd; ++resultIter)
            {
                SAFE_DELETE(resultIter->second);
            }

            SAFE_DELETE(pResultMap);
        }

        threadMap.clear();
    }

    // It should be safe to kill this now that all of the entries are destroyed.
    mEntriesWithProfilingResults.clear();

    mSampleIdToEntry.clear();
}

//-----------------------------------------------------------------------------
/// Validate the contents of a profiler result.
/// \param result The result to validate.
/// \returns True if the profiler result looks good.
//-----------------------------------------------------------------------------
bool VktFrameProfilerLayer::ValidateProfilerResult(const ProfilerResult& result)
{
    // @TODO - pull it up
    bool validResult = true;

    // Verify that the raw clock timestamps are non-zero.
    if ((result.timestampResult.rawClocks.preStart == 0ULL) ||
        (result.timestampResult.rawClocks.start == 0ULL) ||
        (result.timestampResult.rawClocks.end == 0ULL))
    {
        validResult = false;

        Log(logERROR, "Detected empty timestamp. PreStartRawClock: %llu || StartRawClock: %llu || EndRawClock: %llu || SampleID: %d || Frame: %d || Cmd: %s || CmdBuf: 0x%p || CmdBufMeasurementNum: %d || CmdBufMeasurementCount: %d\n",
            result.timestampResult.rawClocks.preStart,
            result.timestampResult.rawClocks.start,
            result.timestampResult.rawClocks.end,
            result.measurementInfo.idInfo.sampleId,
            result.measurementInfo.idInfo.frame,
            VktTraceAnalyzerLayer::Instance()->GetFunctionNameFromId((FuncId)result.measurementInfo.idInfo.funcId),
            result.measurementInfo.idInfo.pWrappedCmdBuf,
            result.measurementInfo.measurementNum,
            static_cast<VktWrappedCmdBuf*>(result.measurementInfo.idInfo.pWrappedCmdBuf)->GetProfiledCallCount());
    }

    // Verify that start < end.
    if ((result.timestampResult.rawClocks.preStart > result.timestampResult.rawClocks.start) ||
        (result.timestampResult.rawClocks.preStart > result.timestampResult.rawClocks.end) ||
        (result.timestampResult.rawClocks.start > result.timestampResult.rawClocks.end))
    {
        validResult = false;

        Log(logERROR, "Detected (Start>End) timestamp. PreStartRawClock: %llu || StartRawClock: %llu || EndRawClock: %llu || SampleID: %d || Frame: %d || Cmd: %s || CmdBuf: 0x%p || CmdBufMeasurementNum: %d || CmdBufMeasurementCount: %d\n",
            result.timestampResult.rawClocks.preStart,
            result.timestampResult.rawClocks.start,
            result.timestampResult.rawClocks.end,
            result.measurementInfo.idInfo.sampleId,
            result.measurementInfo.idInfo.frame,
            VktTraceAnalyzerLayer::Instance()->GetFunctionNameFromId((FuncId)result.measurementInfo.idInfo.funcId),
            result.measurementInfo.idInfo.pWrappedCmdBuf,
            result.measurementInfo.measurementNum,
            static_cast<VktWrappedCmdBuf*>(result.measurementInfo.idInfo.pWrappedCmdBuf)->GetProfiledCallCount());
    }

    return validResult;
}

//-----------------------------------------------------------------------------
/// Verify, align, and store the new profiler results.
/// \param pQueue The Queue used to collect the results to verify.
/// \param results The vector of profiler results to verify.
/// \param pTimestampPair A pair of calibration timestamps used to align CPU and GPU timestamps.
/// \param threadID The ThreadId that the results are collected with.
/// \param frameStartTime The start time of the frame as collected on the CPU.
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::VerifyAlignAndStoreResults(
    VktWrappedQueue*             pQueue,
    std::vector<ProfilerResult>& results,
    CalibrationTimestampPair*    pTimestampPair,
    UINT32                       threadID,
    GPS_TIMESTAMP                frameStartTime)
{
#if MANUAL_TIMESTAMP_CALIBRATION == 0
    UNREFERENCED_PARAMETER(pTimestampPair);
    UNREFERENCED_PARAMETER(frameStartTime);
#endif

    SampleIdToProfilerResultMap* pResultMap = FindOrCreateProfilerResultsMap(pQueue, threadID);
    PsAssert(pResultMap != nullptr);

    if (pResultMap != nullptr)
    {
        for (size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex)
        {
            ProfilerResult& currentResult = results[resultIndex];

            const UINT64 sampleId = currentResult.measurementInfo.idInfo.sampleId;

            // Verify that the timestamps retrieved from the profiler appear to be valid.
            if (ValidateProfilerResult(currentResult) == true)
            {
#if MANUAL_TIMESTAMP_CALIBRATION
                // Now attempt to align the profiled GPU timestamps with the traced API calls on the CPU.
                bool bAlignedSuccessfully = AlignProfilerResultWithCPUTimeline(currentResult, pTimestampPair, frameStartTime);
#else
                bool bAlignedSuccessfully = true;
#endif

                // @TODO - determine if this is what we want to do
                // Make zero-duration case equal to 1 clock cycle
                if (currentResult.timestampResult.rawClocks.start == currentResult.timestampResult.rawClocks.end)
                {
                    currentResult.timestampResult.rawClocks.end++;
                }

                if (bAlignedSuccessfully)
                {
                    // Store the final adjusted profiler results if they're valid.
                    ProfilerResult* pNewResult = new ProfilerResult;
                    CopyProfilerResult(pNewResult, &currentResult);
                    (*pResultMap)[sampleId] = pNewResult;
                }
                else
                {
                    Log(logERROR, "Command with SampleId %d failed to align with CPU timeline.\n", sampleId);
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
/// Set the internal flag that determines if GPU command profiling is enabled.
/// \param inbProfilingEnabled The flag used to enable or disable profiling.
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::SetProfilingEnabled(bool inbProfilingEnabled)
{
    ModernAPIFrameProfilerLayer::SetProfilingEnabled(inbProfilingEnabled);
}

//-----------------------------------------------------------------------------
/// Insert an APIEntry into the list of entries with profiler results.
/// \param pEntry An APIEntry with a sampled GPU time.
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::StoreProfilerResult(VktAPIEntry* pEntry)
{
    // Need to lock here to control access into our profiling results map
    ScopeLock profilerResultsLock(&mProfilingResultsMutex);

    UINT32 threadId = osGetCurrentThreadId();

    if (mSampleIdToEntry.find(threadId) == mSampleIdToEntry.end())
    {
        // create a new entry
        SampleIdToAPIEntryMap newMap;
        newMap[pEntry->m_sampleId] = pEntry;

        mSampleIdToEntry[threadId] = newMap;
    }
    else
    {
        SampleIdToAPIEntryMap& mapEntry = mSampleIdToEntry[threadId];
        mapEntry[pEntry->m_sampleId] = pEntry;
    }
}

//-----------------------------------------------------------------------------
/// Given a SampleId, find the VktAPIEntry that was logged while API tracing.
/// \param inSampleId The SampleId to search with.
/// \return The buffered VktAPIEntry with a matching SampleId, or nullptr if it doesn't exist for some reason.
//-----------------------------------------------------------------------------
VktAPIEntry* VktFrameProfilerLayer::FindInvocationBySampleId(UINT64 inSampleId)
{
    VktAPIEntry* apiEntry = nullptr;

    ThreadSafeSampleIdToAPIEntryMap::iterator threadIter;

    for (threadIter = mSampleIdToEntry.begin(); threadIter != mSampleIdToEntry.end(); ++threadIter)
    {
        SampleIdToAPIEntryMap& entryMap = threadIter->second;

        SampleIdToAPIEntryMap::iterator sampleIdIter = entryMap.find(inSampleId);

        if (sampleIdIter != entryMap.end())
        {
            apiEntry = sampleIdIter->second;
            break;
        }
    }

    return apiEntry;
}

//-----------------------------------------------------------------------------
/// Handles operations that need to occur before profiling an API call.
/// \param funcId The FuncId corresponding to the API call being traced.
/// \param pWrappedCmdBuf The interface pointer used to invoke the API call.
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::PreCall(FuncId funcId, VktWrappedCmdBuf* pWrappedCmdBuf)
{
    if (pWrappedCmdBuf->IsProfilingEnabled() && ShouldProfileFunction(funcId))
    {
        osThreadId threadId = osGetCurrentThreadId();
        SampleInfo* pSampleInfo = GetSampleInfoForThread(threadId);

        if (pSampleInfo != nullptr)
        {
            UINT64 nextSampleId = SetNextSampleId(pSampleInfo);

            ProfilerMeasurementId measurementId = {};
            VktUtil::ConstructMeasurementInfo(funcId, nextSampleId, pWrappedCmdBuf, VktLayerManager::GetLayerManager()->GetFrameCount(), pWrappedCmdBuf->FillCount(), measurementId);

            ProfilerResultCode beginResult = pWrappedCmdBuf->BeginCmdMeasurement(&measurementId);

            if (beginResult == PROFILER_SUCCESS)
            {
                pSampleInfo->mSampleId = measurementId.sampleId;
                pSampleInfo->mbBeginSampleSuccessful = true;
            }
            else
            {
                Log(logERROR, "Failed BeginCmdMeasurement. CmdBuf='0x%p' SampleId='%d'\n", pWrappedCmdBuf->AppHandle(), measurementId.sampleId);
            }
        }
        else
        {
            Log(logERROR, "Failed to find or create SampleInfo instance for Thread %d\n", threadId);
        }
    }
}

//-----------------------------------------------------------------------------
/// Handler used after the real runtime implementation of an API call has been invoked.
/// \param pNewAPIEntry The new APIEntry created for the API call invocation.
/// \param pWrappedCmdBuf The interface pointer used to invoke the API call.
/// \param inFuncId The FuncId corresponding to the API call being traced.
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::PostCall(VktAPIEntry* pNewAPIEntry, FuncId inFuncId, VktWrappedCmdBuf* pWrappedCmdBuf)
{
    // Wait and gather results
    if (pWrappedCmdBuf->IsProfilingEnabled() && ShouldProfileFunction(inFuncId))
    {
        osThreadId threadId = osGetCurrentThreadId();
        SampleInfo* pSampleInfo = GetSampleInfoForThread(threadId);

        if (pSampleInfo != nullptr)
        {
            if (pSampleInfo->mbBeginSampleSuccessful == true)
            {
                ProfilerResultCode endResult = pWrappedCmdBuf->EndCmdMeasurement();

                if (endResult == PROFILER_SUCCESS)
                {
                    pNewAPIEntry->m_sampleId = pSampleInfo->mSampleId;
                    StoreProfilerResult(pNewAPIEntry);
                }
                else
                {
                    Log(logERROR, "Failed EndCmdMeasurement. CmdBuf='0x%p' SampleId='%d'\n", pWrappedCmdBuf->AppHandle(), pSampleInfo->mSampleId);
                }
            }
            else
            {
                Log(logERROR, "Didn't call EndMeasurement because BeginMeasurement wasn't successful.\n");
            }
        }
        else
        {
            Log(logERROR, "Didn't call EndSample because there was no SampleInfo for Thread %d\n", threadId);
        }
    }
}

//-----------------------------------------------------------------------------
/// Initialize the list of functions that we want to profile for GPU time.
/// Functions set to 'true' within this list be profiled and presented within the GPU trace.
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::SetProfiledFuncs()
{
    m_profiledFuncs[FuncId_vkCmdBindPipeline] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetViewport] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetScissor] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetLineWidth] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetDepthBias] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetBlendConstants] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetDepthBounds] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetStencilCompareMask] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetStencilWriteMask] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdSetStencilReference] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdBindDescriptorSets] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdBindIndexBuffer] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdBindVertexBuffers] = VK_FALSE;

    m_profiledFuncs[FuncId_vkCmdDraw] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdDrawIndexed] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdDrawIndirect] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdDrawIndexedIndirect] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdDispatch] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdDispatchIndirect] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdCopyBuffer] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdCopyImage] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdBlitImage] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdCopyBufferToImage] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdCopyImageToBuffer] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdUpdateBuffer] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdFillBuffer] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdClearColorImage] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdClearDepthStencilImage] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdClearAttachments] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdResolveImage] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdPipelineBarrier] = VK_TRUE;

    m_profiledFuncs[FuncId_vkCmdWaitEvents] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdSetEvent] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdResetEvent] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdBeginQuery] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdEndQuery] = VK_FALSE;
    m_profiledFuncs[FuncId_vkCmdWriteTimestamp] = VK_FALSE;

    m_profiledFuncs[FuncId_vkCmdPushConstants] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdResetQueryPool] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdCopyQueryPoolResults] = VK_TRUE;

    m_profiledFuncs[FuncId_vkCmdBeginRenderPass] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdNextSubpass] = VK_TRUE;
    m_profiledFuncs[FuncId_vkCmdEndRenderPass] = VK_TRUE;

    m_profiledFuncs[FuncId_vkCmdExecuteCommands] = VK_TRUE;
}

//-----------------------------------------------------------------------------
/// Take one result from VktCmdBufProfiler and scale the timestamp based on the incoming calibration timestamps.
/// \param ioResult The ProfilerResult instance to align with the CPU timeline.
/// \param pTimestamps A calibration timestamp structure used to align GPU events along the CPU timeline.
/// \param inFrameStartTime The starting time for the frame, measured by the CPU.
/// \returns True or false, indicating the success of the alignment operation.
//-----------------------------------------------------------------------------
bool VktFrameProfilerLayer::AlignProfilerResultWithCPUTimeline(ProfilerResult& ioResult, const CalibrationTimestampPair* pTimestamps, GPS_TIMESTAMP inFrameStartTime)
{
    // @TODO - pull it up
    bool bAlignmentResult = false;

    if (pTimestamps != nullptr)
    {
        // The "DeltaStartTime" represents
        double cpuClockFrequency = (double)pTimestamps->cpuFrequency.QuadPart;

        double cpuStartMillisecond = (double)(static_cast<double>(pTimestamps->mBeforeExecutionCPUTimestamp) * 1000.0) / cpuClockFrequency;
        double gpuMillisecondAtBeforeExecution = (static_cast<double>(pTimestamps->mBeforeExecutionGPUTimestamp) * 1000.0) / static_cast<double>(pTimestamps->mQueueFrequency);

        // Extract the raw clock cycles from the profiler result, and convert them into GPU Milliseconds.
        double gpuMillisecondAtResultStart = (static_cast<double>(ioResult.timestampResult.rawClocks.start)    * 1000.0) / static_cast<double>(pTimestamps->mQueueFrequency);
        double gpuMillisecondAtResultEnd = (static_cast<double>(ioResult.timestampResult.rawClocks.end)      * 1000.0) / static_cast<double>(pTimestamps->mQueueFrequency);

        // Now compute the GPU timeline's delta between the "Before Execution GPU Timestamp" and the Start and End millisecond in the GPU timeline.
        double gpuMillisecondAtDeltaStart = (gpuMillisecondAtResultStart - gpuMillisecondAtBeforeExecution);
        double gpuMillisecondAtDeltaEnd = (gpuMillisecondAtResultEnd - gpuMillisecondAtBeforeExecution);

        // Compute the final profiled command's Start and End time by adding the item duration to the "Before CPU Execution" start time.
        double alignedStart = gpuMillisecondAtDeltaStart + cpuStartMillisecond;
        double alignedEnd = gpuMillisecondAtDeltaEnd + cpuStartMillisecond;

        // Take the frame start time into account
        double frameStartOffset = (inFrameStartTime.QuadPart * 1000.0) / cpuClockFrequency;

        alignedStart -= frameStartOffset;
        alignedEnd -= frameStartOffset;

        // Verify that the timestamps are larger than zero.
        if (alignedStart >= 0.0 && alignedEnd >= 0.0)
        {
            ioResult.timestampResult.alignedMillisecondTimestamps.start = alignedStart;
            ioResult.timestampResult.alignedMillisecondTimestamps.end = alignedEnd;

            bAlignmentResult = true;
        }
    }

    return bAlignmentResult;
}

//-----------------------------------------------------------------------------
/// Collect and store calibration timestamps from the CPU and GPU to align execution results in a single timeline.
/// \param pWrappedQueue The Queue responsible for work submission.
/// \param pTimestamps The timestamps structure used to hold timestamps occurring before and after workload execution.
//-----------------------------------------------------------------------------
VkResult VktFrameProfilerLayer::CollectCalibrationTimestamps(VktWrappedQueue* pWrappedQueue, CalibrationTimestampPair* pTimestamps)
{
    VkResult result = VK_INCOMPLETE;

#if MANUAL_TIMESTAMP_CALIBRATION
    if ((pWrappedQueue != nullptr) && (pTimestamps != nullptr))
    {
        VkQueue queue = pWrappedQueue->AppHandle();
        VkDevice device = pWrappedQueue->ParentDevice();

        TimestampedCmdBufConfig config = {};
        config.device           = device;
        config.physicalDevice   = pWrappedQueue->PhysicalDevice();
        config.mapTimestampMem  = false;
        config.pipelineLoc      = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        config.queueFamilyIndex = pWrappedQueue->GetQueueFamilyIndex();

        VktTimestampedCmdBuf* pTimestampedCmdBuf = VktTimestampedCmdBuf::Create(config);

        if (pTimestampedCmdBuf != nullptr)
        {
            const VkCommandBuffer cmdBufs[] = { pTimestampedCmdBuf->CmdBufHandle() };

            VkSubmitInfo submitInfo = {};
            submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.pNext                = nullptr;
            submitInfo.waitSemaphoreCount   = 0;
            submitInfo.pWaitSemaphores      = nullptr;
            submitInfo.pWaitDstStageMask    = nullptr;
            submitInfo.commandBufferCount   = 1;
            submitInfo.pCommandBuffers      = cmdBufs;
            submitInfo.signalSemaphoreCount = 0;
            submitInfo.pSignalSemaphores    = nullptr;

            VkFence fence = VK_NULL_HANDLE;
            VkFenceCreateInfo fenceCreateInfo = {};
            result = device_dispatch_table(queue)->CreateFence(device, &fenceCreateInfo, nullptr, &fence);

            if (result == VK_SUCCESS)
            {
                LARGE_INTEGER largeInt = {};

                result = pWrappedQueue->QueueSubmit_ICD(queue, 1, &submitInfo, fence);
                VKT_ASSERT(result == VK_SUCCESS);

                VkResult waitResult = VK_TIMEOUT;
                do
                {
                    waitResult = device_dispatch_table(device)->WaitForFences(device, 1, &fence, VK_TRUE, GPU_FENCE_TIMEOUT_TIME);
                } while (waitResult == VK_TIMEOUT);

                // Fetch the GPU counter
                pTimestampedCmdBuf->GetTimestampResult(&pTimestamps->mBeforeExecutionGPUTimestamp);

#ifdef WIN32
                // Immediately after, fetch the CPU counter
                QueryPerformanceCounter(&largeInt);

                pTimestamps->mBeforeExecutionCPUTimestamp = largeInt.QuadPart;
#endif

                pTimestamps->mQueueFrequency = (UINT64)pWrappedQueue->GetTimestampFrequency();

                device_dispatch_table(device)->DestroyFence(device, fence, nullptr);
            }

            delete pTimestampedCmdBuf;
            pTimestampedCmdBuf = nullptr;
        }
    }
#else
    UNREFERENCED_PARAMETER(pWrappedQueue);
    UNREFERENCED_PARAMETER(pTimestamps);
#endif

    return result;
}

//-----------------------------------------------------------------------------
/// Copy a profiler result from one chunk of memory to another.
/// \param pDst The destination ProfilerResults memory to copy to.
/// \param pSrc The source memory to copy from.
//-----------------------------------------------------------------------------
void VktFrameProfilerLayer::CopyProfilerResult(ProfilerResult* pDst, const ProfilerResult* pSrc)
{
    // Copy the measurement info.
    memcpy(&pDst->measurementInfo, &pSrc->measurementInfo, sizeof(ProfilerMeasurementInfo));
    memcpy(&pDst->timestampResult, &pSrc->timestampResult, sizeof(ProfilerTimestampResult));
}

//-----------------------------------------------------------------------------
/// Create a new map to associate a LinkId with profiling results.
/// \param pWrappedQueue The Queue that was submitted in order to retrieve profiling results.
/// \param inThreadId The thread being used to create a results map.
/// \returns A SampleIdToProfilerResultMap instance used to store profiler results.
//-----------------------------------------------------------------------------
SampleIdToProfilerResultMap* VktFrameProfilerLayer::FindOrCreateProfilerResultsMap(VktWrappedQueue* pWrappedQueue, UINT32 inThreadId)
{
    SampleIdToProfilerResultMap* pResultMap = nullptr;

    if (mEntriesWithProfilingResults.count(inThreadId) == 0)
    {
        // make the new map data for this thread
        QueueWrapperToProfilingResultsMap queueMap;
        pResultMap = new SampleIdToProfilerResultMap();
        queueMap.insert(make_pair(pWrappedQueue, pResultMap));
        {
            // Lock before we insert something new into this map.
            ScopeLock profilerResultsLock(&mProfilingResultsMutex);
            mEntriesWithProfilingResults[inThreadId] = queueMap;
        }
    }
    else
    {
        QueueWrapperToProfilingResultsMap& queueMap = mEntriesWithProfilingResults[inThreadId];

        QueueWrapperToProfilingResultsMap::const_iterator queueIt = queueMap.find(pWrappedQueue);

        if (queueIt != queueMap.end())
        {
            return queueIt->second;
        }
        else
        {
            pResultMap = new SampleIdToProfilerResultMap();
            queueMap.insert(make_pair(pWrappedQueue, pResultMap));
        }
    }

    return pResultMap;
}

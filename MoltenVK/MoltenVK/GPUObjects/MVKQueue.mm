/*
 * MVKQueue.mm
 *
 * Copyright (c) 2015-2026 The Brenwill Workshop Ltd. (http://www.brenwill.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MVKInstance.h"
#include "MVKQueue.h"
#include "MVKSurface.h"
#include "MVKSwapchain.h"
#include "MVKSync.h"
#include "MVKFoundation.h"
#include "MVKOSExtensions.h"
#include "MVKGPUCapture.h"
#include "MVKBuffer.h"
#include "MVKDescriptorSet.h"
#include "MVKImage.h"
#include "MVKPipeline.h"
#include "MVKQueryPool.h"
#include "MVKFramebuffer.h"
#include "MVKRenderPass.h"
#include "MVKCommandEncodingPool.h"
#include "mvk_datatypes.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace std;


#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
static constexpr uint32_t kMetal4CommandAllocatorDefaultCount = 4;
static constexpr uint32_t kMetal4CommandAllocatorMaxCount = 16;
static constexpr NSUInteger kMetal4CommandResidencyInitialCapacity = 256;
static constexpr double kMetal4CommandValidationDefaultTimeoutMs = 5000.0;
static constexpr double kMetal4CommandValidationMinimumTimeoutMs = 100.0;
static constexpr double kMetal4CommandValidationMaximumTimeoutMs = 30000.0;
static constexpr size_t kMetal4UnsupportedCommandCapacity = 64;
static_assert(kMVKMetal4MaxColorAttachmentCount == kMVKMaxColorAttachmentCount,
			  "Metal 4 clear attachment transport must match MoltenVK render limits");

enum class MVKMetal4FallbackReason : uint8_t {
	UnsupportedSemaphore = 0,
	UnsupportedCommandBuffer,
	PrepareFailed,
	ResidencyAcquireFailed,
	AllocatorUnavailable,
	CommandObjectUnavailable,
	EncodingReplayableException,
	CommandBufferNotEnded,
	PrecommitReplayableException,
	Count,
};

static const char* mvkMetal4FallbackReasonName(MVKMetal4FallbackReason reason) {
	switch (reason) {
		case MVKMetal4FallbackReason::UnsupportedSemaphore: return "unsupported_semaphore";
		case MVKMetal4FallbackReason::UnsupportedCommandBuffer: return "unsupported_command_buffer";
		case MVKMetal4FallbackReason::PrepareFailed: return "prepare_failed";
		case MVKMetal4FallbackReason::ResidencyAcquireFailed: return "residency_acquire_failed";
		case MVKMetal4FallbackReason::AllocatorUnavailable: return "allocator_unavailable";
		case MVKMetal4FallbackReason::CommandObjectUnavailable: return "command_object_unavailable";
		case MVKMetal4FallbackReason::EncodingReplayableException: return "encoding_replayable_exception";
		case MVKMetal4FallbackReason::CommandBufferNotEnded: return "command_buffer_not_ended";
		case MVKMetal4FallbackReason::PrecommitReplayableException: return "precommit_replayable_exception";
		case MVKMetal4FallbackReason::Count: break;
	}
	return "unknown";
}

struct MVKMetal4FallbackTelemetry {
	uint64_t totalCount = 0;
	uint64_t reasonCount = 0;
	const char* unsupportedCommand = "none";
	uint64_t unsupportedCommandCount = 0;
};

struct MVKMetal4CompletedQuery {
	MVKQueryPool* queryPool = nullptr;
	uint32_t query = 0;
};

/**
 * Queue-independent ownership shared by MTL4 feedback and event callbacks.
 * The state owns the allocator arena, ordering event, and command-backend
 * residency set, but never dereferences MVKQueue or MVKDevice from a late
 * callback.
 */
struct MVKMetal4CommandQueueState {
	struct UnsupportedCommandEntry {
		const char* name = nullptr;
		uint64_t count = 0;
	};

	struct AllocatorSlot {
		id<MTL4CommandAllocator> allocator = nil;
		uint32_t inFlightCount = 0;
		bool encoding = false;
		bool resetPending = false;
		bool retired = false;
	};

	struct ResidencyEntry {
		id<MTLAllocation> allocation = nil;
		uint32_t inFlightCount = 0;
	};

	mutex lock;
	mutex unsupportedCommandLock;
	condition_variable probeReady;
	vector<AllocatorSlot> allocators;
	size_t nextAllocatorIndex = 0;
	unordered_map<uintptr_t, ResidencyEntry> residentAllocations;
	id<MTLResidencySet> residencySet = nil;
	id<MTLSharedEvent> orderingEvent = nil;
	MTLSharedEventListener* orderingListener = nil;
	atomic<bool> shuttingDown = false;
	atomic<bool> probeSubmitted = false;
	atomic<bool> probeCompleted = false;
	atomic<bool> probeSucceeded = false;
	atomic<bool> probeAllocatorCompleted = false;
	atomic<bool> probeMayBeInFlight = false;
	atomic<uint64_t> nextSequence = 1;
	atomic<uint64_t> submittedCount = 0;
	atomic<uint64_t> completedCount = 0;
	atomic<uint64_t> attemptedSubmissionCount = 0;
	atomic<uint64_t> realSubmissionCount = 0;
	atomic<uint64_t> fallbackCount = 0;
	array<atomic<uint64_t>, static_cast<size_t>(MVKMetal4FallbackReason::Count)> fallbackReasonCounts;
	array<UnsupportedCommandEntry, kMetal4UnsupportedCommandCapacity> unsupportedCommands;
	size_t unsupportedCommandTypeCount = 0;
	uint64_t unsupportedCommandOverflowCount = 0;
	atomic<uint64_t> failureCount = 0;
	atomic<uint64_t> bufferCopyCount = 0;
	atomic<uint64_t> bufferFillCount = 0;
	atomic<uint64_t> bufferUpdateCount = 0;
	atomic<uint64_t> imageCopyCount = 0;
	atomic<uint64_t> computeDispatchCount = 0;
	atomic<uint64_t> renderSubmissionCount = 0;
	atomic<uint64_t> renderPassCount = 0;
	atomic<uint64_t> drawCount = 0;
	atomic<uint64_t> barrierCount = 0;
	atomic<uint64_t> queryResetCount = 0;
	atomic<uint64_t> queryCopyCount = 0;
	atomic<uint64_t> visibilityQueryCount = 0;
	string lastError;
	string lastReplayableException;

	MVKMetal4CommandQueueState() {
		for (auto& counter : fallbackReasonCounts) { counter.store(0, memory_order_relaxed); }
	}

	~MVKMetal4CommandQueueState() {
		for (auto& item : residentAllocations) { [item.second.allocation release]; }
		for (auto& slot : allocators) { [slot.allocator release]; }
		[orderingListener release];
		[orderingEvent release];
		[residencySet release];
	}

	bool initialize(id<MTLDevice> mtlDevice,
					uint32_t allocatorCount,
					NSString* label,
					string* failureReason) {
		lock_guard<mutex> guard(lock);

		MTLResidencySetDescriptor* residencyDescriptor = [MTLResidencySetDescriptor new];
		residencyDescriptor.label = [label stringByAppendingString:@" Residency"];
		residencyDescriptor.initialCapacity = kMetal4CommandResidencyInitialCapacity;
		NSError* error = nil;
		residencySet = [mtlDevice newResidencySetWithDescriptor:residencyDescriptor error:&error];
		[residencyDescriptor release];
		if (!residencySet) {
			if (failureReason) {
				*failureReason = error.localizedDescription.UTF8String ?:
					"could not create Metal 4 command residency set";
			}
			return false;
		}
		[residencySet commit];

		orderingEvent = [mtlDevice newSharedEvent];
		orderingListener = [MTLSharedEventListener new];
		if (!orderingEvent || !orderingListener) {
			if (failureReason) { *failureReason = "could not create Metal 4 queue ordering event"; }
			return false;
		}
		orderingEvent.label = [label stringByAppendingString:@" Ordering"];

		allocators.resize(allocatorCount);
		for (size_t idx = 0; idx < allocatorCount; idx++) {
			MTL4CommandAllocatorDescriptor* descriptor = [MTL4CommandAllocatorDescriptor new];
			descriptor.label = [NSString stringWithFormat:@"%@ Allocator %zu", label, idx];
			error = nil;
			allocators[idx].allocator =
				[mtlDevice newCommandAllocatorWithDescriptor:descriptor error:&error];
			[descriptor release];
			if (!allocators[idx].allocator) {
				if (failureReason) {
					*failureReason = error.localizedDescription.UTF8String ?:
						"could not create Metal 4 command allocator";
				}
				return false;
			}
		}
		return true;
	}

	id<MTLResidencySet> copyResidencySet() {
		lock_guard<mutex> guard(lock);
		return [residencySet retain];
	}

	id<MTLSharedEvent> copyOrderingEvent() {
		lock_guard<mutex> guard(lock);
		return [orderingEvent retain];
	}

	MTLSharedEventListener* copyOrderingListener() {
		lock_guard<mutex> guard(lock);
		return [orderingListener retain];
	}

	uint64_t reserveSequence() {
		return nextSequence.fetch_add(1, memory_order_relaxed);
	}

	uint64_t lastSequence() const {
		uint64_t value = nextSequence.load(memory_order_acquire);
		return value > 0 ? value - 1 : 0;
	}

	bool acquireAllocator(size_t* slotIndex, id<MTL4CommandAllocator>* allocator) {
		lock_guard<mutex> guard(lock);
		if (shuttingDown.load(memory_order_acquire) || allocators.empty()) { return false; }

		for (size_t offset = 0; offset < allocators.size(); offset++) {
			size_t idx = (nextAllocatorIndex + offset) % allocators.size();
			auto& slot = allocators[idx];
			if (slot.retired || slot.encoding || slot.inFlightCount != 0) { continue; }
			if (slot.resetPending && slot.inFlightCount == 0) {
				[slot.allocator reset];
				slot.resetPending = false;
			}
			slot.encoding = true;
			nextAllocatorIndex = (idx + 1) % allocators.size();
			*slotIndex = idx;
			*allocator = [slot.allocator retain];
			return true;
		}
		return false;
	}

	void finishEncoding(size_t slotIndex, bool submitted) {
		lock_guard<mutex> guard(lock);
		if (slotIndex >= allocators.size()) { return; }
		auto& slot = allocators[slotIndex];
		slot.encoding = false;
		if (submitted) {
			slot.inFlightCount++;
			submittedCount.fetch_add(1, memory_order_relaxed);
			return;
		}
		if (!submitted && slot.inFlightCount == 0) {
			[slot.allocator reset];
			slot.resetPending = false;
		}
	}

	void completeAllocator(size_t slotIndex) {
		lock_guard<mutex> guard(lock);
		if (slotIndex >= allocators.size()) { return; }
		auto& slot = allocators[slotIndex];
		if (slot.inFlightCount > 0) { slot.inFlightCount--; }
		if (slot.inFlightCount == 0) {
			if (slot.encoding) {
				slot.resetPending = true;
			} else {
				[slot.allocator reset];
				slot.resetPending = false;
			}
		}
		completedCount.fetch_add(1, memory_order_relaxed);
	}

	void retireAllocator(size_t slotIndex) {
		lock_guard<mutex> guard(lock);
		if (slotIndex >= allocators.size()) { return; }
		auto& slot = allocators[slotIndex];
		slot.retired = true;
		slot.encoding = false;
		slot.resetPending = false;
	}

	bool acquireResidency(const vector<id<MTLAllocation>>& allocations) {
		lock_guard<mutex> guard(lock);
		if (shuttingDown.load(memory_order_acquire)) { return false; }

		unordered_set<uintptr_t> seen;
		bool changed = false;
		for (id<MTLAllocation> allocation : allocations) {
			if (!allocation) { return false; }
			uintptr_t key = (uintptr_t)allocation;
			if (!seen.insert(key).second) { continue; }
			auto it = residentAllocations.find(key);
			if (it == residentAllocations.end()) {
				[residencySet addAllocation:allocation];
				residentAllocations.emplace(key, ResidencyEntry{[allocation retain], 1});
				changed = true;
			} else {
				it->second.inFlightCount++;
			}
		}
		if (changed) { [residencySet commit]; }
		return true;
	}

	void releaseResidency(const vector<id<MTLAllocation>>& allocations) {
		lock_guard<mutex> guard(lock);
		unordered_set<uintptr_t> seen;
		bool changed = false;
		for (id<MTLAllocation> allocation : allocations) {
			uintptr_t key = (uintptr_t)allocation;
			if (!seen.insert(key).second) { continue; }
			auto it = residentAllocations.find(key);
			if (it == residentAllocations.end()) { continue; }
			if (it->second.inFlightCount > 0) { it->second.inFlightCount--; }
			if (it->second.inFlightCount == 0) {
				[residencySet removeAllocation:it->second.allocation];
				[it->second.allocation release];
				residentAllocations.erase(it);
				changed = true;
			}
		}
		if (changed) { [residencySet commit]; }
	}

	bool markProbeStatus(NSError* error, NSString* fallbackReason) {
		{
			lock_guard<mutex> guard(lock);
			if (probeCompleted.load(memory_order_acquire)) { return false; }
			bool succeeded = error == nil && fallbackReason == nil;
			probeSucceeded.store(succeeded, memory_order_release);
			probeCompleted.store(true, memory_order_release);
			if (!succeeded) {
				failureCount.fetch_add(1, memory_order_relaxed);
				if (error) {
					lastError = error.localizedDescription.UTF8String ?: "unknown Metal 4 commit error";
				} else {
					lastError = fallbackReason.UTF8String ?: "unknown Metal 4 probe failure";
				}
			}
		}
		probeReady.notify_all();
		return true;
	}

	void completeProbeAllocator(size_t slotIndex) {
		if (!probeAllocatorCompleted.exchange(true, memory_order_acq_rel)) {
			completeAllocator(slotIndex);
		}
	}

	void completeProbe(size_t slotIndex, NSError* error) {
		probeMayBeInFlight.store(false, memory_order_release);
		markProbeStatus(error, nil);
		completeProbeAllocator(slotIndex);
	}

	void failProbeBeforeCommit(size_t slotIndex, NSString* reason) {
		markProbeStatus(nil, reason);
		completeProbeAllocator(slotIndex);
	}

	void failProbeInFlight(NSString* reason) {
		// The queue may have accepted the command buffer. Keep its allocator alive
		// until real Metal feedback arrives; a bounded leak after device loss is
		// safer than resetting memory that the GPU may still be reading.
		markProbeStatus(nil, reason);
	}

	bool waitForProbe(uint64_t timeoutNs) {
		unique_lock<mutex> guard(lock);
		bool completed = probeReady.wait_for(
			guard,
			chrono::nanoseconds(timeoutNs),
			[this] { return probeCompleted.load(memory_order_acquire); });
		guard.unlock();
		if (!completed) {
			failProbeInFlight(@"Metal 4 empty submission probe timed out");
		}
		return probeSucceeded.load(memory_order_acquire);
	}

	uint64_t recordSubmissionAttempt() {
		return attemptedSubmissionCount.fetch_add(1, memory_order_relaxed) + 1;
	}
	void recordRealSubmission() { realSubmissionCount.fetch_add(1, memory_order_relaxed); }
	MVKMetal4FallbackTelemetry recordUnsupportedCommand(const char* commandName) {
		const char* stableName = commandName && commandName[0] ? commandName : "unknown_command";
		lock_guard<mutex> guard(unsupportedCommandLock);
		for (size_t idx = 0; idx < unsupportedCommandTypeCount; idx++) {
			auto& entry = unsupportedCommands[idx];
			if (entry.name == stableName || (entry.name && !strcmp(entry.name, stableName))) {
				entry.count++;
				return { 0, 0, entry.name, entry.count };
			}
		}
		if (unsupportedCommandTypeCount < unsupportedCommands.size()) {
			auto& entry = unsupportedCommands[unsupportedCommandTypeCount++];
			entry.name = stableName;
			entry.count = 1;
			return { 0, 0, entry.name, entry.count };
		}
		unsupportedCommandOverflowCount++;
		return { 0, 0, "other_unsupported_command", unsupportedCommandOverflowCount };
	}

	string unsupportedCommandSummary() {
		lock_guard<mutex> guard(unsupportedCommandLock);
		vector<UnsupportedCommandEntry> entries(
			unsupportedCommands.begin(),
			unsupportedCommands.begin() + unsupportedCommandTypeCount);
		sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
			return left.count > right.count;
		});
		string summary;
		size_t emitted = 0;
		for (const auto& entry : entries) {
			if (!entry.name || emitted == 8) { break; }
			if (!summary.empty()) { summary += ","; }
			summary += entry.name;
			summary += ":";
			summary += to_string(entry.count);
			emitted++;
		}
		if (unsupportedCommandOverflowCount) {
			if (!summary.empty()) { summary += ","; }
			summary += "other:" + to_string(unsupportedCommandOverflowCount);
		}
		return summary.empty() ? "none" : summary;
	}

	MVKMetal4FallbackTelemetry recordFallback(MVKMetal4FallbackReason reason,
											 const char* unsupportedCommand = nullptr) {
		auto index = static_cast<size_t>(reason);
		MVKMetal4FallbackTelemetry telemetry = unsupportedCommand
			? recordUnsupportedCommand(unsupportedCommand)
			: MVKMetal4FallbackTelemetry{};
		telemetry.totalCount = fallbackCount.fetch_add(1, memory_order_relaxed) + 1;
		telemetry.reasonCount = fallbackReasonCounts[index].fetch_add(1, memory_order_relaxed) + 1;
		return telemetry;
	}

	bool recordReplayableException(const char* phase,
								 NSException* exception,
								 string* summary) {
		string exceptionName = exception.name.UTF8String ?: "unknown";
		string exceptionReason = exception.reason.UTF8String ?: "unknown";
		string nextSummary = "phase=" + string(phase ?: "unknown") +
			", name=" + exceptionName +
			", reason=" + exceptionReason;
		lock_guard<mutex> guard(lock);
		bool changed = nextSummary != lastReplayableException;
		lastReplayableException = nextSummary;
		if (summary) { *summary = nextSummary; }
		return changed;
	}
	void recordBufferCopy(uint64_t count = 1) { bufferCopyCount.fetch_add(count, memory_order_relaxed); }
	void recordBufferFill(uint64_t count = 1) { bufferFillCount.fetch_add(count, memory_order_relaxed); }
	void recordBufferUpdate(uint64_t count = 1) { bufferUpdateCount.fetch_add(count, memory_order_relaxed); }
	void recordImageCopy(uint64_t count = 1) { imageCopyCount.fetch_add(count, memory_order_relaxed); }
	void recordComputeDispatch(uint64_t count = 1) { computeDispatchCount.fetch_add(count, memory_order_relaxed); }
	void recordRenderSubmission() { renderSubmissionCount.fetch_add(1, memory_order_relaxed); }
	void recordRenderPass(uint64_t count = 1) { renderPassCount.fetch_add(count, memory_order_relaxed); }
	void recordDraw(uint64_t count = 1) { drawCount.fetch_add(count, memory_order_relaxed); }
	void recordBarrier(uint64_t count = 1) { barrierCount.fetch_add(count, memory_order_relaxed); }
	void recordQueryReset(uint64_t count = 1) { queryResetCount.fetch_add(count, memory_order_relaxed); }
	void recordQueryCopy(uint64_t count = 1) { queryCopyCount.fetch_add(count, memory_order_relaxed); }
	void recordVisibilityQuery(uint64_t count = 1) { visibilityQueryCount.fetch_add(count, memory_order_relaxed); }

	void recordFailure(NSString* reason) {
		lock_guard<mutex> guard(lock);
		failureCount.fetch_add(1, memory_order_relaxed);
		lastError = reason.UTF8String ?: "unknown Metal 4 submission error";
	}

	void hostSignalOrdering(uint64_t sequence) {
		id<MTLSharedEvent> event = copyOrderingEvent();
		if (event && event.signaledValue < sequence) { event.signaledValue = sequence; }
		[event release];
	}

	void shutdown() {
		shuttingDown.store(true, memory_order_release);
		probeReady.notify_all();
	}
};

static MTLStages mvkMetal4StagesFromVkPipelineStages(VkPipelineStageFlags2 stages) {
	MTLStages mtlStages = 0;
	if (mvkIsAnyFlagEnabled(stages,
						  VK_PIPELINE_STAGE_2_TRANSFER_BIT |
						  VK_PIPELINE_STAGE_2_COPY_BIT |
						  VK_PIPELINE_STAGE_2_BLIT_BIT |
						  VK_PIPELINE_STAGE_2_CLEAR_BIT |
						  VK_PIPELINE_STAGE_2_RESOLVE_BIT)) {
		mtlStages |= MTLStageBlit;
	}
	if (mvkIsAnyFlagEnabled(stages, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)) {
		mtlStages |= MTLStageDispatch;
	}
	if (mvkIsAnyFlagEnabled(stages,
						  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
						  VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
						  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
						  VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)) {
		mtlStages |= MTLStageVertex;
	}
	if (mvkIsAnyFlagEnabled(stages,
						  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
						  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
						  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
						  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)) {
		mtlStages |= MTLStageFragment;
	}
	if (!mtlStages && stages) {
		// Conservatively cover every encoder type currently materialized rather
		// than dropping ordering for a broader Vulkan stage mask.
		mtlStages = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit;
	}
	return mtlStages;
}

/** Concrete compute/transfer/render command materializer for the first usable backend slice. */
class MVKMetal4TransferCommandEncoder final : public MVKMetal4CommandEncoder {

public:
	MVKMetal4TransferCommandEncoder(shared_ptr<MVKMetal4CommandQueueState> state,
								 MVKDevice* device,
								 id<MTLDevice> mtlDevice,
								 MVKPixelFormats* pixelFormats) :
		_state(std::move(state)), _device(device), _pixelFormats(pixelFormats),
		_mtlDevice(mtlDevice) {}

	struct BufferBinding {
		id<MTLBuffer> buffer = nil;
		NSUInteger offset = 0;
	};
	struct ImageBinding {
		vector<id<MTLTexture>> textures;
	};
	struct PendingBarrier {
		MTLStages afterQueueStages = 0;
		MTLStages beforeStages = 0;
		MTL4VisibilityOptions visibilityOptions = MTL4VisibilityOptionNone;
	};
	struct ImageViewBinding {
		id<MTLTexture> texture = nil;
		NSUInteger level = 0;
		NSUInteger slice = 0;
		NSUInteger depthPlane = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;
		NSUInteger width = 0;
		NSUInteger height = 0;
	};
	struct CommandCounters {
		uint64_t bufferCopies = 0;
		uint64_t bufferFills = 0;
		uint64_t bufferUpdates = 0;
		uint64_t imageCopies = 0;
		uint64_t computeDispatches = 0;
		uint64_t renderPasses = 0;
		uint64_t draws = 0;
		uint64_t barriers = 0;
		uint64_t queryResets = 0;
		uint64_t queryCopies = 0;
		uint64_t visibilityQueries = 0;
	};
	struct QueryPoolBinding {
		id<MTLBuffer> resetBuffer = nil;
		id<MTLBuffer> resultBuffer = nil;
	};
	struct PendingQueryReset {
		MVKQueryPool* queryPool = nullptr;
		uint32_t firstQuery = 0;
		uint32_t queryCount = 0;
	};
	struct UpdateDataBinding {
		id<MTLBuffer> buffer = nil;
		NSUInteger size = 0;
	};
	struct BoundDescriptorSet {
		MVKDescriptorSetLayout* layout = nullptr;
		id<MTLBuffer> buffer = nil;
		NSUInteger offset = 0;
	};
	struct BoundVertexBuffer {
		id<MTLBuffer> buffer = nil;
		NSUInteger offset = 0;
		NSUInteger size = 0;
		NSUInteger stride = 0;
	};
	struct BoundIndexBuffer {
		id<MTLBuffer> buffer = nil;
		NSUInteger offset = 0;
		NSUInteger size = 0;
		MTLIndexType type = MTLIndexTypeUInt16;
	};
	struct DepthStencilStateBinding {
		MVKStencilReference compareMask = {};
		MVKStencilReference writeMask = {};
		id<MTLDepthStencilState> state = nil;
	};
	struct GraphicsPipelineBinding {
		id<MTLRenderPipelineState> pipelineState = nil;
		vector<DepthStencilStateBinding> depthStencilStates;
	};
	struct ClearAttachmentsBinding {
		MVKRPSKeyClearAtt pipelineKey = {};
		id<MTLRenderPipelineState> pipelineState = nil;
		id<MTLDepthStencilState> depthStencilState = nil;
		id<MTLBuffer> clearColors = nil;
		id<MTLBuffer> vertices = nil;
		vector<VkClearRect> rects;
		NSUInteger vertexCount = 0;
		uint32_t stencilReference = 0;
	};

	~MVKMetal4TransferCommandEncoder() override {
		endEncoding();
		for (auto& item : _buffers) { [item.second.buffer release]; }
		for (auto& item : _images) {
			for (id<MTLTexture> texture : item.second.textures) { [texture release]; }
		}
		for (auto& item : _imageViews) { [item.second.texture release]; }
		for (auto& item : _queryPools) {
			[item.second.resetBuffer release];
			[item.second.resultBuffer release];
		}
		for (auto& item : _updateData) { [item.second.buffer release]; }
		for (auto& item : _computePipelines) { [item.second release]; }
		for (auto& item : _graphicsPipelines) {
			[item.second.pipelineState release];
			for (auto& depthStencil : item.second.depthStencilStates) {
				[depthStencil.state release];
			}
		}
		for (auto& item : _clearAttachments) {
			[item.second.pipelineState release];
			[item.second.depthStencilState release];
			[item.second.clearColors release];
			[item.second.vertices release];
		}
		for (id<MTLAllocation> allocation : _descriptorAllocations) { [allocation release]; }
		[_argumentTable release];
	}

	bool useBuffer(MVKBuffer* buffer) override {
		if (!buffer) { return false; }
		if (_buffers.count(buffer)) { return true; }
		id<MTLBuffer> mtlBuffer = buffer->getMTLBuffer();
		if (!mtlBuffer) { return false; }
		_buffers.emplace(buffer, BufferBinding{[mtlBuffer retain], buffer->getMTLBufferOffset()});
		_allocations.push_back((id<MTLAllocation>)mtlBuffer);
		return true;
	}

	bool useImage(MVKImage* image) override {
		if (!image) { return false; }
		if (_images.count(image)) { return true; }
		ImageBinding binding;
		binding.textures.reserve(image->getPlaneCount());
		for (uint8_t plane = 0; plane < image->getPlaneCount(); plane++) {
			id<MTLTexture> texture = image->getMTLTexture(plane);
			if (!texture) {
				for (id<MTLTexture> retained : binding.textures) { [retained release]; }
				return false;
			}
			binding.textures.push_back([texture retain]);
			_allocations.push_back((id<MTLAllocation>)texture);
		}
		_images.emplace(image, std::move(binding));
		return true;
	}

	bool useImageView(MVKImageView* imageView) override {
		if (!imageView) { return false; }
		if (_imageViews.count(imageView)) { return true; }
		if (!useImage(imageView->getImage())) { return false; }

		MTLRenderPassColorAttachmentDescriptor* descriptor =
			[MTLRenderPassColorAttachmentDescriptor new];
		imageView->populateMTLRenderPassAttachmentDescriptor(descriptor);
		id<MTLTexture> texture = descriptor.texture;
		if (!texture) {
			[descriptor release];
			return false;
		}
		ImageViewBinding binding;
		binding.texture = [texture retain];
		binding.level = descriptor.level;
		binding.slice = descriptor.slice;
		binding.depthPlane = descriptor.depthPlane;
		binding.format = imageView->getVkFormat();
		binding.width = texture.width;
		binding.height = texture.height;
		[descriptor release];
		_imageViews.emplace(imageView, binding);
		_allocations.push_back((id<MTLAllocation>)texture);
		return true;
	}

	bool useQueryPool(MVKQueryPool* queryPool) override {
		if (!queryPool) { return false; }
		if (_queryPools.count(queryPool)) { return true; }
		id<MTLBuffer> resetBuffer = queryPool->getMetal4ResetMTLBuffer();
		NSUInteger resultOffset = 0;
		id<MTLBuffer> resultBuffer =
			queryPool->getMetal4ResultMTLBuffer(0, 1, resultOffset);
		_queryPools.emplace(queryPool, QueryPoolBinding{
			[resetBuffer retain], [resultBuffer retain]});
		if (resetBuffer) { _allocations.push_back((id<MTLAllocation>)resetBuffer); }
		if (resultBuffer && resultBuffer != resetBuffer) {
			_allocations.push_back((id<MTLAllocation>)resultBuffer);
		}
		return true;
	}

	bool useQueryResultPool(MVKQueryPool* queryPool) override {
		if (!useQueryPool(queryPool)) { return false; }
		auto binding = _queryPools.find(queryPool);
		return binding != _queryPools.end() && binding->second.resultBuffer;
	}

	bool useVisibilityQueryPool(MVKQueryPool* queryPool) override {
		if (!queryPool || !queryPool->supportsMetal4VisibilityQueries() ||
			(_visibilityQueryPool && _visibilityQueryPool != queryPool) ||
			!useQueryPool(queryPool)) {
			return false;
		}
		auto binding = _queryPools.find(queryPool);
		if (binding == _queryPools.end() || !binding->second.resetBuffer) { return false; }
		_visibilityQueryPool = queryPool;
		return true;
	}

	bool useUpdateBufferData(const void* data, size_t size) override {
		if (!_mtlDevice || !data || !size || size > 65536) { return false; }
		auto existing = _updateData.find(data);
		if (existing != _updateData.end()) { return existing->second.size == size; }
		id<MTLBuffer> buffer = [_mtlDevice newBufferWithBytes:data
												 length:size
												options:MTLResourceStorageModeShared |
														MTLResourceCPUCacheModeWriteCombined];
		if (!buffer) { return false; }
		_updateData.emplace(data, UpdateDataBinding{buffer, size});
		_allocations.push_back((id<MTLAllocation>)buffer);
		return true;
	}

	bool useComputePipeline(MVKComputePipeline* pipeline) override {
		if (!pipeline || !pipeline->supportsMetal4Execution() ||
			(pipeline->requiresMetal4ArgumentTable() && !ensureArgumentTable())) {
			return false;
		}
		_preparedComputePipeline = pipeline;
		if (_computePipelines.count(pipeline)) { return true; }
		id<MTLComputePipelineState> pipelineState = pipeline->getPipelineState();
		if (!pipelineState) { return false; }
		_computePipelines.emplace(pipeline, [pipelineState retain]);
		_allocations.push_back((id<MTLAllocation>)pipelineState);
		return true;
	}

	bool useGraphicsPipeline(MVKGraphicsPipeline* pipeline) override {
		if (!pipeline || !pipeline->supportsMetal4RenderExecution() ||
			(pipeline->requiresMetal4ArgumentTable() && !ensureArgumentTable())) {
			return false;
		}
		_preparedGraphicsPipeline = pipeline;
		if (_graphicsPipelines.count(pipeline)) { return true; }
		id<MTLRenderPipelineState> pipelineState = pipeline->getMainPipelineState();
		if (!pipelineState) { return false; }
		const auto& stateData = pipeline->getStaticStateData();
		MVKStencilReference compareMask {
			stateData.depthStencil.frontFaceStencilData.readMask,
			stateData.depthStencil.backFaceStencilData.readMask,
		};
		MVKStencilReference writeMask {
			stateData.depthStencil.frontFaceStencilData.writeMask,
			stateData.depthStencil.backFaceStencilData.writeMask,
		};
		id<MTLDepthStencilState> depthStencilState =
			newDepthStencilState(pipeline, compareMask, writeMask);
		if (!depthStencilState) { return false; }
		GraphicsPipelineBinding binding;
		binding.pipelineState = [pipelineState retain];
		binding.depthStencilStates.push_back(
			DepthStencilStateBinding{compareMask, writeMask, depthStencilState});
		_graphicsPipelines.emplace(pipeline, std::move(binding));
		_allocations.push_back((id<MTLAllocation>)pipelineState);
		return true;
	}

	void resetPrepareState() override {
		_preparedComputePipeline = nullptr;
		_preparedGraphicsPipeline = nullptr;
		_preparedComputeDescriptorSets.fill(nullptr);
		_preparedGraphicsDescriptorSets.fill(nullptr);
	}

	void recordMetal4EncodingFailure(const char* commandType) override {
		if (!_encodingFailureCommand) {
			_encodingFailureCommand = commandType ?: "MVKCommand";
		}
	}

	const char* getMetal4EncodingFailureCommand() const {
		return _encodingFailureCommand ?: "MVKCommand";
	}

	bool useDescriptorSet(VkPipelineBindPoint bindPoint,
					  MVKDescriptorSet* descriptorSet,
					  uint32_t setIndex) override {
		if (bindPoint != VK_PIPELINE_BIND_POINT_COMPUTE &&
			bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) {
			return false;
		}
		auto& preparedSets = bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE
			? _preparedComputeDescriptorSets
			: _preparedGraphicsDescriptorSets;
		if (!descriptorSet || !descriptorSet->supportsMetal4ArgumentTable() ||
			setIndex >= preparedSets.size() || !ensureArgumentTable()) {
			return false;
		}
		if (!retainDescriptorAllocation((id<MTLAllocation>)descriptorSet->gpuBufferObject)) {
			return false;
		}
		preparedSets[setIndex] = descriptorSet;
		return true;
	}

	bool prepareComputeDispatch() override {
		if (!_preparedComputePipeline) { return false; }
		if (!_preparedComputePipeline->supportsMetal4ArgumentTableExecution()) {
			return _preparedComputePipeline->supportsMetal4DescriptorlessExecution();
		}
		MVKPipelineLayout* pipelineLayout = _preparedComputePipeline->getLayout();
		if (!pipelineLayout) { return false; }
		vector<id<MTLResource>> resources;
		const auto& uses =
			_preparedComputePipeline->getStageResources().bindScript.descriptorBindings;
		for (const auto& use : uses) {
			if (use.set >= _preparedComputeDescriptorSets.size() ||
				use.set >= pipelineLayout->getDescriptorSetCount()) {
				return false;
			}
			MVKDescriptorSet* descriptorSet = _preparedComputeDescriptorSets[use.set];
			if (!descriptorSet ||
				descriptorSet->layout != pipelineLayout->getDescriptorSetLayout(use.set)) {
				return false;
			}
			resources.clear();
			descriptorSet->collectMetal4BindingResources(use.bindingIdx, resources);
			for (id<MTLResource> resource : resources) {
				if (!retainDescriptorAllocation((id<MTLAllocation>)resource)) {
					return false;
				}
			}
		}
		return true;
	}

	bool prepareGraphicsDraw() override {
		if (!_preparedGraphicsPipeline) { return false; }
		if (!_preparedGraphicsPipeline->supportsMetal4ArgumentTableRenderExecution()) {
			return _preparedGraphicsPipeline->supportsMetal4DescriptorlessRenderExecution();
		}
		MVKPipelineLayout* pipelineLayout = _preparedGraphicsPipeline->getLayout();
		if (!pipelineLayout) { return false; }
		vector<id<MTLResource>> resources;
		for (MVKShaderStage stage : {kMVKShaderStageVertex, kMVKShaderStageFragment}) {
			const auto& uses =
				_preparedGraphicsPipeline->getStageResources(stage).bindScript.descriptorBindings;
			for (const auto& use : uses) {
				if (use.set >= _preparedGraphicsDescriptorSets.size() ||
					use.set >= pipelineLayout->getDescriptorSetCount()) {
					return false;
				}
				MVKDescriptorSet* descriptorSet = _preparedGraphicsDescriptorSets[use.set];
				if (!descriptorSet ||
					descriptorSet->layout != pipelineLayout->getDescriptorSetLayout(use.set)) {
					return false;
				}
				resources.clear();
				descriptorSet->collectMetal4BindingResources(use.bindingIdx, resources);
				for (id<MTLResource> resource : resources) {
					if (!retainDescriptorAllocation((id<MTLAllocation>)resource)) {
						return false;
					}
				}
			}
		}
		return true;
	}

	bool beginEncoding(id<MTL4CommandBuffer> commandBuffer, id<MTLResidencySet> residencySet) {
		if (!commandBuffer || _commandBuffer) { return false; }
		_commandBuffer = commandBuffer;
		[commandBuffer useResidencySet:residencySet];
		return true;
	}

	bool copyImage(MVKImage* srcImage,
				   uint8_t srcPlane,
				   const VkImageCopy2& region,
				   MVKImage* dstImage,
				   uint8_t dstPlane) override {
		auto src = _images.find(srcImage);
		auto dst = _images.find(dstImage);
		if (!ensureComputeEncoder() || src == _images.end() || dst == _images.end() ||
			srcPlane >= src->second.textures.size() || dstPlane >= dst->second.textures.size()) {
			return false;
		}

		id<MTLTexture> srcTexture = src->second.textures[srcPlane];
		id<MTLTexture> dstTexture = dst->second.textures[dstPlane];
		uint32_t srcLevel = region.srcSubresource.mipLevel;
		uint32_t dstLevel = region.dstSubresource.mipLevel;
		uint32_t srcBaseLayer = region.srcSubresource.baseArrayLayer;
		uint32_t dstBaseLayer = region.dstSubresource.baseArrayLayer;
		uint32_t layerCount = region.srcSubresource.layerCount == VK_REMAINING_ARRAY_LAYERS ?
			srcImage->getLayerCount() - srcBaseLayer : region.srcSubresource.layerCount;

		VkExtent3D srcExtent = srcImage->getExtent3D(srcPlane, srcLevel);
		VkExtent3D dstExtent = dstImage->getExtent3D(dstPlane, dstLevel);
		bool src3D = srcImage->getMTLTextureType() == MTLTextureType3D;
		bool dst3D = dstImage->getMTLTextureType() == MTLTextureType3D;
		if (mvkVkExtent3DsAreEqual(srcExtent, region.extent) &&
			mvkVkExtent3DsAreEqual(dstExtent, region.extent) && src3D == dst3D) {
			[_computeEncoder copyFromTexture:srcTexture
							 sourceSlice:srcBaseLayer
							 sourceLevel:srcLevel
							   toTexture:dstTexture
					  destinationSlice:dstBaseLayer
					  destinationLevel:dstLevel
							 sliceCount:layerCount
							 levelCount:1];
			_counters.imageCopies++;
			return true;
		}

		MTLOrigin srcOrigin = mvkMTLOriginFromVkOffset3D(region.srcOffset);
		MTLOrigin dstOrigin = mvkMTLOriginFromVkOffset3D(region.dstOffset);
		MTLSize copySize = mvkMTLSizeFromVkExtent3D(region.extent);
		uint32_t iterationCount = layerCount;
		if (src3D != dst3D) {
			iterationCount = region.extent.depth;
			copySize.depth = 1;
		}

		for (uint32_t layer = 0; layer < iterationCount; layer++) {
			MTLOrigin layerSrcOrigin = srcOrigin;
			MTLOrigin layerDstOrigin = dstOrigin;
			NSUInteger srcSlice = srcBaseLayer + layer;
			NSUInteger dstSlice = dstBaseLayer + layer;
			if (src3D && !dst3D) {
				layerSrcOrigin.z += layer;
				srcSlice = srcBaseLayer;
			} else if (!src3D && dst3D) {
				layerDstOrigin.z += layer;
				dstSlice = dstBaseLayer;
			}
			[_computeEncoder copyFromTexture:srcTexture
							 sourceSlice:srcSlice
							 sourceLevel:srcLevel
							sourceOrigin:layerSrcOrigin
							  sourceSize:copySize
							   toTexture:dstTexture
					  destinationSlice:dstSlice
					  destinationLevel:dstLevel
					 destinationOrigin:layerDstOrigin];
			_counters.imageCopies++;
		}
		return true;
	}

	bool copyBufferImage(MVKBuffer* buffer,
						 MVKImage* image,
						 const VkBufferImageCopy2& region,
						 bool toImage) override {
		auto bufferIt = _buffers.find(buffer);
		auto imageIt = _images.find(image);
		uint8_t plane = MVKImage::getPlaneFromVkImageAspectFlags(
			region.imageSubresource.aspectMask);
		if (!ensureComputeEncoder() || bufferIt == _buffers.end() ||
			imageIt == _images.end() || plane >= imageIt->second.textures.size()) {
			return false;
		}

		id<MTLBuffer> mtlBuffer = bufferIt->second.buffer;
		id<MTLTexture> mtlTexture = imageIt->second.textures[plane];
		MTLPixelFormat pixelFormat = image->getMTLPixelFormat(plane);
		MVKPixelFormats* pixelFormats = image->getPixelFormats();
		uint32_t bufferWidth = region.bufferRowLength ?
			region.bufferRowLength : region.imageExtent.width;
		uint32_t bufferHeight = region.bufferImageHeight ?
			region.bufferImageHeight : region.imageExtent.height;
		NSUInteger bytesPerRow = pixelFormats->getBytesPerRow(pixelFormat, bufferWidth);
		NSUInteger activeRowBytes = pixelFormats->getBytesPerRow(
			pixelFormat, region.imageExtent.width);
		NSUInteger bytesPerImage =
			pixelFormats->getBytesPerLayer(pixelFormat, bytesPerRow, bufferHeight);
		MTLBlitOption options = MTLBlitOptionNone;
		if (pixelFormats->isDepthFormat(pixelFormat) &&
			pixelFormats->isStencilFormat(pixelFormat)) {
			bool copyDepth = mvkAreAllFlagsEnabled(
				region.imageSubresource.aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT);
			if (copyDepth) {
				if (pixelFormats->getBytesPerTexel(pixelFormat) != 4) {
					NSUInteger rowReduction = bufferWidth;
					NSUInteger activeReduction = region.imageExtent.width;
					NSUInteger imageReduction =
						static_cast<NSUInteger>(bufferWidth) * bufferHeight;
					if (bytesPerRow < rowReduction ||
						activeRowBytes < activeReduction ||
						bytesPerImage < imageReduction) {
						return false;
					}
					bytesPerRow -= rowReduction;
					activeRowBytes -= activeReduction;
					bytesPerImage -= imageReduction;
				}
				options = MTLBlitOptionDepthFromDepthStencil;
			} else {
				bytesPerRow = bufferWidth;
				activeRowBytes = region.imageExtent.width;
				bytesPerImage =
					static_cast<NSUInteger>(bufferWidth) * bufferHeight;
				options = MTLBlitOptionStencilFromDepthStencil;
			}
		}
		NSUInteger bufferOffset = bufferIt->second.offset + (NSUInteger)region.bufferOffset;
		NSUInteger rowCount = region.imageExtent.height;
		NSUInteger maxValue = std::numeric_limits<NSUInteger>::max();
		bool is3D = image->getMTLTextureType() == MTLTextureType3D;
		uint32_t layerCount =
			region.imageSubresource.layerCount == VK_REMAINING_ARRAY_LAYERS
				? image->getLayerCount() - region.imageSubresource.baseArrayLayer
				: region.imageSubresource.layerCount;
		NSUInteger imageCount = is3D ? region.imageExtent.depth : layerCount;
		if (!bytesPerRow || !activeRowBytes || bufferOffset > mtlBuffer.length ||
			!bytesPerImage || !imageCount ||
			(imageCount > 1 && bytesPerImage >
				(maxValue - bufferOffset) / (imageCount - 1))) {
			return false;
		}
		NSUInteger lastImageOffset = bufferOffset + bytesPerImage * (imageCount - 1);
		if (rowCount > 1 && bytesPerRow >
			(maxValue - lastImageOffset) / (rowCount - 1)) {
			return false;
		}
		NSUInteger lastRowOffset = lastImageOffset + bytesPerRow * (rowCount - 1);
		if (lastRowOffset > mtlBuffer.length ||
			activeRowBytes > mtlBuffer.length - lastRowOffset) {
			return false;
		}

		MTLOrigin textureOrigin = mvkMTLOriginFromVkOffset3D(region.imageOffset);
		MTLSize textureSize = mvkMTLSizeFromVkExtent3D(region.imageExtent);
		NSUInteger level = region.imageSubresource.mipLevel;
		if (!is3D) { textureSize.depth = 1; }
		NSUInteger iterationCount = is3D ? 1 : layerCount;
		for (NSUInteger iteration = 0; iteration < iterationCount; iteration++) {
			NSUInteger copyBufferOffset = bufferOffset + bytesPerImage * iteration;
			NSUInteger slice = region.imageSubresource.baseArrayLayer + iteration;
			NSUInteger metalBytesPerImage = is3D ? bytesPerImage : 0;
			if (toImage) {
				[_computeEncoder copyFromBuffer:mtlBuffer
								 sourceOffset:copyBufferOffset
							sourceBytesPerRow:bytesPerRow
						  sourceBytesPerImage:metalBytesPerImage
								  sourceSize:textureSize
								   toTexture:mtlTexture
							 destinationSlice:slice
							 destinationLevel:level
							destinationOrigin:textureOrigin
								 options:options];
			} else {
				[_computeEncoder copyFromTexture:mtlTexture
								  sourceSlice:slice
								  sourceLevel:level
								 sourceOrigin:textureOrigin
								   sourceSize:textureSize
									 toBuffer:mtlBuffer
							 destinationOffset:copyBufferOffset
						destinationBytesPerRow:bytesPerRow
					  destinationBytesPerImage:metalBytesPerImage
									 options:options];
			}
			_counters.imageCopies++;
		}
		return true;
	}

	bool bindComputePipeline(MVKComputePipeline* pipeline) override {
		auto it = _computePipelines.find(pipeline);
		if (!ensureComputeEncoder() || it == _computePipelines.end()) { return false; }
		[_computeEncoder setComputePipelineState:it->second];
		_boundComputePipeline = pipeline;
		_computeResourcesBoundForEncoder = false;
		return true;
	}

	bool dispatchThreadgroups(uint32_t groupCountX,
							 uint32_t groupCountY,
							 uint32_t groupCountZ) override {
		if (!ensureComputeEncoder() || !_boundComputePipeline) { return false; }
		if (!_computeResourcesBoundForEncoder && !applyComputeResources()) { return false; }
		[_computeEncoder dispatchThreadgroups:MTLSizeMake(groupCountX, groupCountY, groupCountZ)
					  threadsPerThreadgroup:_boundComputePipeline->getThreadgroupSize()];
		_counters.computeDispatches++;
		return true;
	}

	bool pipelineBarrier(VkPipelineStageFlags2 srcStages,
						 VkAccessFlags2 srcAccess,
						 VkPipelineStageFlags2 dstStages,
						 VkAccessFlags2 dstAccess) override {
		// The strict render slice does not claim barriers from inside an active render pass.
		// Ending that encoder here would silently terminate Vulkan rendering, while an
		// intra-pass MTL4 barrier cannot legally name transfer/dispatch stages.
		if (_renderEncoder) { return false; }

		MTLStages after = mvkMetal4StagesFromVkPipelineStages(srcStages);
		MTLStages before = mvkMetal4StagesFromVkPipelineStages(dstStages);
		if (!after) { after = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit; }
		if (!before) { before = MTLStageVertex | MTLStageFragment | MTLStageDispatch | MTLStageBlit; }
		MTL4VisibilityOptions visibility = MTL4VisibilityOptionNone;
		VkAccessFlags2 writes = VK_ACCESS_2_MEMORY_WRITE_BIT |
			VK_ACCESS_2_SHADER_WRITE_BIT |
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_2_TRANSFER_WRITE_BIT |
			VK_ACCESS_2_HOST_WRITE_BIT;
		if (mvkIsAnyFlagEnabled(srcAccess | dstAccess, writes)) {
			visibility |= MTL4VisibilityOptionDevice;
		}

		// A Vulkan barrier may cross Metal encoder classes. Defer it until the next
		// consuming encoder exists, then issue a consumer queue-stage barrier with a
		// before-stage mask legal for that encoder. This preserves render<->compute/
		// transfer ordering without passing render stages to a compute encoder or blit
		// stages to a render encoder.
		endComputeEncoding();
		_pendingBarriers.push_back(PendingBarrier{after, before, visibility});
		_counters.barriers++;
		return true;
	}

	bool trackImageBarrier(const MVKPipelineBarrier& barrier) override {
		if (barrier.type != MVKPipelineBarrier::Image || !barrier.mvkImage ||
			_images.find(barrier.mvkImage) == _images.end()) {
			return false;
		}
		_pendingImageBarriers.push_back(barrier);
		return true;
	}

	bool copyBuffer(MVKBuffer* srcBuffer,
					VkDeviceSize srcOffset,
					MVKBuffer* dstBuffer,
					VkDeviceSize dstOffset,
					VkDeviceSize size) override {
		auto src = _buffers.find(srcBuffer);
		auto dst = _buffers.find(dstBuffer);
		if (!ensureComputeEncoder() || src == _buffers.end() || dst == _buffers.end() || !size) { return false; }
		[_computeEncoder copyFromBuffer:src->second.buffer
						 sourceOffset:(src->second.offset + (NSUInteger)srcOffset)
							 toBuffer:dst->second.buffer
					destinationOffset:(dst->second.offset + (NSUInteger)dstOffset)
								 size:(NSUInteger)size];
		_counters.bufferCopies++;
		return true;
	}

	bool fillBuffer(MVKBuffer* dstBuffer,
					VkDeviceSize dstOffset,
					VkDeviceSize size,
					uint8_t value) override {
		auto dst = _buffers.find(dstBuffer);
		if (!ensureComputeEncoder() || dst == _buffers.end() || !size) { return false; }
		[_computeEncoder fillBuffer:dst->second.buffer
						  range:NSMakeRange(dst->second.offset + (NSUInteger)dstOffset,
											 (NSUInteger)size)
						  value:value];
		_counters.bufferFills++;
		return true;
	}

	bool resetQueryPool(MVKQueryPool* queryPool,
						uint32_t firstQuery,
						uint32_t queryCount) override {
		auto binding = _queryPools.find(queryPool);
		if (binding == _queryPools.end() || !queryCount) { return false; }
		if (binding->second.resetBuffer) {
			NSRange range = queryPool->getMetal4ResetRange(firstQuery, queryCount);
			if (!range.length || range.location > binding->second.resetBuffer.length ||
				range.length > binding->second.resetBuffer.length - range.location ||
				!ensureComputeEncoder()) {
				return false;
			}
			[_computeEncoder fillBuffer:binding->second.resetBuffer range:range value:0];
		}
		_pendingQueryResets.push_back(PendingQueryReset{queryPool, firstQuery, queryCount});
		_counters.queryResets++;
		return true;
	}

	bool copyQueryPoolResults(MVKQueryPool* queryPool,
								 uint32_t firstQuery,
								 uint32_t queryCount,
								 MVKBuffer* dstBuffer,
								 VkDeviceSize dstOffset,
								 VkDeviceSize dstStride,
								 VkQueryResultFlags flags) override {
		auto queryBinding = _queryPools.find(queryPool);
		auto dst = _buffers.find(dstBuffer);
		if (queryBinding == _queryPools.end() || dst == _buffers.end() ||
			!queryCount || mvkIsAnyFlagEnabled(flags,
				VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT)) {
			return false;
		}

		NSUInteger srcOffset = 0;
		id<MTLBuffer> srcBuffer =
			queryPool->getMetal4ResultMTLBuffer(firstQuery, queryCount, srcOffset);
		if (!srcBuffer || srcBuffer != queryBinding->second.resultBuffer) { return false; }

		for (uint32_t queryIndex = 0; queryIndex < queryCount; queryIndex++) {
			uint32_t query = firstQuery + queryIndex;
			bool completed = std::any_of(
				_completedQueries.begin(), _completedQueries.end(),
				[queryPool, query](const MVKMetal4CompletedQuery& item) {
					return item.queryPool == queryPool && item.query == query;
				});
			if (!completed) { return false; }
		}

		NSUInteger elementSize = mvkIsAnyFlagEnabled(flags, VK_QUERY_RESULT_64_BIT) ?
			sizeof(uint64_t) : sizeof(uint32_t);
		NSUInteger dstBaseOffset = dst->second.offset + (NSUInteger)dstOffset;
		if (dstStride < elementSize || srcOffset > srcBuffer.length ||
			queryCount > (srcBuffer.length - srcOffset) / kMVKQuerySlotSizeInBytes ||
			dstBaseOffset > dst->second.buffer.length ||
			elementSize > dst->second.buffer.length ||
			(queryCount > 1 && dstStride >
				(std::numeric_limits<NSUInteger>::max() - dstBaseOffset) / (queryCount - 1)) ||
			dstBaseOffset + (queryCount - 1) * (NSUInteger)dstStride >
				dst->second.buffer.length - elementSize || !ensureComputeEncoder()) {
			return false;
		}
		[_computeEncoder barrierAfterQueueStages:MTLStageFragment
							 beforeStages:MTLStageBlit
						visibilityOptions:MTL4VisibilityOptionDevice];

		for (uint32_t query = 0; query < queryCount; query++) {
			[_computeEncoder copyFromBuffer:srcBuffer
							 sourceOffset:srcOffset + query * kMVKQuerySlotSizeInBytes
								 toBuffer:dst->second.buffer
						destinationOffset:dstBaseOffset + query * (NSUInteger)dstStride
									 size:elementSize];
		}
		_counters.queryCopies++;
		return true;
	}

	bool beginVisibilityQuery(MVKQueryPool* queryPool,
							  uint32_t query,
							  VkQueryControlFlags flags) override {
		if (!_renderEncoder || queryPool != _visibilityQueryPool || _activeQueryPool) {
			return false;
		}
		MTLVisibilityResultMode mode = mvkAreAllFlagsEnabled(
			flags, VK_QUERY_CONTROL_PRECISE_BIT)
			? MTLVisibilityResultModeCounting
			: MTLVisibilityResultModeBoolean;
		[_renderEncoder setVisibilityResultMode:mode
								 offset:MVKOcclusionQueryPool::getVisibilityResultOffset(query)];
		_activeQueryPool = queryPool;
		_activeQuery = query;
		return true;
	}

	bool endVisibilityQuery(MVKQueryPool* queryPool, uint32_t query) override {
		if (!_renderEncoder || _activeQueryPool != queryPool || _activeQuery != query) {
			return false;
		}
		[_renderEncoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		_completedQueries.push_back(MVKMetal4CompletedQuery{queryPool, query});
		_activeQueryPool = nullptr;
		_activeQuery = 0;
		_counters.visibilityQueries++;
		return true;
	}

	bool updateBuffer(MVKBuffer* dstBuffer,
					  VkDeviceSize dstOffset,
					  const void* data,
					  size_t size) override {
		auto dst = _buffers.find(dstBuffer);
		auto src = _updateData.find(data);
		if (dst == _buffers.end() || src == _updateData.end() || src->second.size != size ||
			!size || !ensureComputeEncoder()) {
			return false;
		}
		NSUInteger destinationOffset = dst->second.offset + static_cast<NSUInteger>(dstOffset);
		if (destinationOffset > dst->second.buffer.length ||
			size > dst->second.buffer.length - destinationOffset) {
			return false;
		}
		[_computeEncoder copyFromBuffer:src->second.buffer
						 sourceOffset:0
							 toBuffer:dst->second.buffer
					destinationOffset:destinationOffset
								 size:size];
		_counters.bufferUpdates++;
		return true;
	}

	bool beginRendering(const VkRenderingInfo& renderingInfo) override {
		if (!_commandBuffer || _renderEncoder || renderingInfo.colorAttachmentCount == 0 ||
			renderingInfo.colorAttachmentCount > kMVKMaxColorAttachmentCount ||
			!renderingInfo.pColorAttachments) {
			return false;
		}
		const ImageViewBinding* firstColorBinding = nullptr;
		vector<id<MTLTexture>> renderAttachments;
		for (uint32_t colorIndex = 0;
			 colorIndex < renderingInfo.colorAttachmentCount;
			 colorIndex++) {
			auto viewIt = _imageViews.find(
				(MVKImageView*)renderingInfo.pColorAttachments[colorIndex].imageView);
			if (viewIt == _imageViews.end()) { return false; }
			renderAttachments.push_back(viewIt->second.texture);
			if (!firstColorBinding) {
				firstColorBinding = &viewIt->second;
			} else if (viewIt->second.width != firstColorBinding->width ||
					   viewIt->second.height != firstColorBinding->height) {
				return false;
			}
		}
		auto depthIt = _imageViews.end();
		if (renderingInfo.pDepthAttachment) {
			depthIt = _imageViews.find(
				(MVKImageView*)renderingInfo.pDepthAttachment->imageView);
			if (depthIt == _imageViews.end()) { return false; }
			renderAttachments.push_back(depthIt->second.texture);
		}
		auto stencilIt = _imageViews.end();
		if (renderingInfo.pStencilAttachment) {
			stencilIt = _imageViews.find(
				(MVKImageView*)renderingInfo.pStencilAttachment->imageView);
			if (stencilIt == _imageViews.end()) { return false; }
			renderAttachments.push_back(stencilIt->second.texture);
		}
		endComputeEncoding();

		MTL4RenderPassDescriptor* descriptor = [MTL4RenderPassDescriptor new];
		for (uint32_t colorIndex = 0;
			 colorIndex < renderingInfo.colorAttachmentCount;
			 colorIndex++) {
			const VkRenderingAttachmentInfo& color =
				renderingInfo.pColorAttachments[colorIndex];
			const ImageViewBinding& binding =
				_imageViews.find((MVKImageView*)color.imageView)->second;
			MTLRenderPassColorAttachmentDescriptor* colorDescriptor =
				descriptor.colorAttachments[colorIndex];
			colorDescriptor.texture = binding.texture;
			colorDescriptor.level = binding.level;
			colorDescriptor.slice = binding.slice;
			colorDescriptor.depthPlane = binding.depthPlane;
			colorDescriptor.loadAction =
				mvkMTLLoadActionFromVkAttachmentLoadOpInObj(color.loadOp, nullptr);
			colorDescriptor.storeAction =
				mvkMTLStoreActionFromVkAttachmentStoreOpInObj(
					color.storeOp, false, true, nullptr);
			colorDescriptor.clearColor = MTLClearColorMake(
				color.clearValue.color.float32[0],
				color.clearValue.color.float32[1],
				color.clearValue.color.float32[2],
				color.clearValue.color.float32[3]);
		}
		if (renderingInfo.pDepthAttachment) {
			const VkRenderingAttachmentInfo& depth = *renderingInfo.pDepthAttachment;
			const ImageViewBinding& depthBinding = depthIt->second;
			MTLRenderPassDepthAttachmentDescriptor* depthDescriptor =
				descriptor.depthAttachment;
			depthDescriptor.texture = depthBinding.texture;
			depthDescriptor.level = depthBinding.level;
			depthDescriptor.slice = depthBinding.slice;
			depthDescriptor.depthPlane = depthBinding.depthPlane;
			depthDescriptor.loadAction =
				mvkMTLLoadActionFromVkAttachmentLoadOpInObj(depth.loadOp, nullptr);
			depthDescriptor.storeAction =
				mvkMTLStoreActionFromVkAttachmentStoreOpInObj(
					depth.storeOp, false, true, nullptr);
			depthDescriptor.clearDepth = depth.clearValue.depthStencil.depth;
		}
		if (renderingInfo.pStencilAttachment) {
			const VkRenderingAttachmentInfo& stencil = *renderingInfo.pStencilAttachment;
			const ImageViewBinding& stencilBinding = stencilIt->second;
			MTLRenderPassStencilAttachmentDescriptor* stencilDescriptor =
				descriptor.stencilAttachment;
			stencilDescriptor.texture = stencilBinding.texture;
			stencilDescriptor.level = stencilBinding.level;
			stencilDescriptor.slice = stencilBinding.slice;
			stencilDescriptor.depthPlane = stencilBinding.depthPlane;
			stencilDescriptor.loadAction =
				mvkMTLLoadActionFromVkAttachmentLoadOpInObj(stencil.loadOp, nullptr);
			stencilDescriptor.storeAction =
				mvkMTLStoreActionFromVkAttachmentStoreOpInObj(
					stencil.storeOp, false, true, nullptr);
			stencilDescriptor.clearStencil = stencil.clearValue.depthStencil.stencil;
		}
		descriptor.renderTargetWidth = renderingInfo.renderArea.extent.width;
		descriptor.renderTargetHeight = renderingInfo.renderArea.extent.height;
		descriptor.renderTargetArrayLength = renderingInfo.layerCount;
		if (_visibilityQueryPool) {
			descriptor.visibilityResultBuffer =
				_queryPools.find(_visibilityQueryPool)->second.resetBuffer;
			descriptor.visibilityResultType = MTLVisibilityResultTypeAccumulate;
		}
		_renderEncoder = [[_commandBuffer renderCommandEncoderWithDescriptor:descriptor] retain];
		[descriptor release];
		if (!_renderEncoder) { return false; }
		applyRenderAttachmentBarrier(_renderEncoder, renderAttachments);
		applyPendingBarriers(_renderEncoder, MTLStageVertex | MTLStageFragment);

		_currentColorAttachmentCount = renderingInfo.colorAttachmentCount;
		for (uint32_t colorIndex = 0;
			 colorIndex < _currentColorAttachmentCount;
			 colorIndex++) {
			_currentColorAttachmentFormats[colorIndex] =
				_imageViews.find((MVKImageView*)renderingInfo.pColorAttachments[colorIndex].imageView)
					->second.format;
		}
		_currentDepthFormat = renderingInfo.pDepthAttachment
			? depthIt->second.format
			: VK_FORMAT_UNDEFINED;
		_currentStencilFormat = renderingInfo.pStencilAttachment
			? stencilIt->second.format
			: VK_FORMAT_UNDEFINED;
		_currentRenderWidth = firstColorBinding->width;
		_currentRenderHeight = firstColorBinding->height;
		_currentRenderLayerCount = renderingInfo.layerCount;
		_graphicsPipelineBoundForEncoder = false;
		_graphicsResourcesBoundForEncoder = false;
		_graphicsViewportScissorAppliedForEncoder = false;
		_graphicsBlendConstantsAppliedForEncoder = false;
		_counters.renderPasses++;
		return !_boundGraphicsPipeline || applyGraphicsPipeline(true);
	}

	bool beginRenderPass(MVKRenderPass* renderPass,
						 MVKFramebuffer* framebuffer,
						 const VkRect2D& renderArea,
						 const VkClearValue* clearValues,
						 size_t clearValueCount,
						 MVKImageView*const* attachments,
						 size_t attachmentCount) override {
		if (!_commandBuffer || _renderEncoder || !renderPass || !framebuffer ||
			renderPass->getSubpassCount() != 1 || !attachments || !attachmentCount) {
			return false;
		}
		MVKRenderSubpass* subpass = renderPass->getSubpass(0);
		if (!subpass ||
			subpass->getColorAttachmentCount() > kMVKMaxColorAttachmentCount) {
			return false;
		}
		bool hasUsedColorAttachment = false;
		for (uint32_t colorIndex = 0;
			 colorIndex < subpass->getColorAttachmentCount();
			 colorIndex++) {
			hasUsedColorAttachment |= subpass->isColorAttachmentUsed(colorIndex);
		}
		if (!hasUsedColorAttachment && !subpass->isDepthAttachmentUsed() &&
			!subpass->isStencilAttachmentUsed()) {
			return false;
		}
		vector<id<MTLTexture>> renderAttachments;
		renderAttachments.reserve(attachmentCount);
		for (size_t attachmentIndex = 0; attachmentIndex < attachmentCount;
			 attachmentIndex++) {
			auto viewIt = _imageViews.find(attachments[attachmentIndex]);
			if (viewIt == _imageViews.end()) { return false; }
			renderAttachments.push_back(viewIt->second.texture);
		}
		endComputeEncoding();

		MTLRenderPassDescriptor* legacyDescriptor =
			[MTLRenderPassDescriptor renderPassDescriptor];
		subpass->populateMTLRenderPassDescriptor(
			legacyDescriptor,
			0,
			framebuffer,
			MVKArrayRef<MVKImageView*const>(attachments, attachmentCount),
			MVKArrayRef<const VkClearValue>(clearValues, clearValueCount),
			true,
			false,
			false);

		MTL4RenderPassDescriptor* descriptor = [MTL4RenderPassDescriptor new];
		for (uint32_t colorIndex = 0;
			 colorIndex < subpass->getColorAttachmentCount();
			 colorIndex++) {
			descriptor.colorAttachments[colorIndex] =
				legacyDescriptor.colorAttachments[colorIndex];
		}
		if (subpass->isDepthAttachmentUsed()) {
			descriptor.depthAttachment = legacyDescriptor.depthAttachment;
		}
		if (subpass->isStencilAttachmentUsed()) {
			descriptor.stencilAttachment = legacyDescriptor.stencilAttachment;
		}
		descriptor.renderTargetWidth = renderArea.extent.width;
		descriptor.renderTargetHeight = renderArea.extent.height;
		descriptor.renderTargetArrayLength = framebuffer->getLayerCount();
		if (_visibilityQueryPool) {
			descriptor.visibilityResultBuffer =
				_queryPools.find(_visibilityQueryPool)->second.resetBuffer;
			descriptor.visibilityResultType = MTLVisibilityResultTypeAccumulate;
		}
		_renderEncoder = [[_commandBuffer renderCommandEncoderWithDescriptor:descriptor]
			retain];
		[descriptor release];
		if (!_renderEncoder) { return false; }
		applyRenderAttachmentBarrier(_renderEncoder, renderAttachments);
		applyPendingBarriers(_renderEncoder, MTLStageVertex | MTLStageFragment);

		VkExtent2D extent = framebuffer->getExtent2D();
		_currentColorAttachmentCount = subpass->getColorAttachmentCount();
		for (uint32_t colorIndex = 0;
			 colorIndex < _currentColorAttachmentCount;
			 colorIndex++) {
			_currentColorAttachmentFormats[colorIndex] =
				subpass->getColorAttachmentFormat(colorIndex);
		}
		_currentDepthFormat = subpass->isDepthAttachmentUsed()
			? subpass->getDepthFormat()
			: VK_FORMAT_UNDEFINED;
		_currentStencilFormat = subpass->isStencilAttachmentUsed()
			? subpass->getStencilFormat()
			: VK_FORMAT_UNDEFINED;
		_currentRenderWidth = extent.width;
		_currentRenderHeight = extent.height;
		_currentRenderLayerCount = framebuffer->getLayerCount();
		_graphicsPipelineBoundForEncoder = false;
		_graphicsResourcesBoundForEncoder = false;
		_graphicsViewportScissorAppliedForEncoder = false;
		_graphicsBlendConstantsAppliedForEncoder = false;
		_counters.renderPasses++;
		return !_boundGraphicsPipeline || applyGraphicsPipeline(true);
	}

	bool endRendering() override {
		if (!_renderEncoder || _activeQueryPool) { return false; }
		endRenderEncoding();
		return true;
	}

	bool bindGraphicsPipeline(MVKGraphicsPipeline* pipeline) override {
		if (_graphicsPipelines.find(pipeline) == _graphicsPipelines.end()) { return false; }
		_boundGraphicsPipeline = pipeline;
		_graphicsResourcesBoundForEncoder = false;
		_graphicsViewportScissorAppliedForEncoder = false;
		_graphicsBlendConstantsAppliedForEncoder = false;
		return !_renderEncoder || applyGraphicsPipeline(true);
	}

	bool setViewports(uint32_t firstViewport,
					  uint32_t viewportCount,
					  const VkViewport* viewports) override {
		if (firstViewport != 0 || viewportCount == 0 ||
			viewportCount > kMVKMaxViewportScissorCount || !viewports) {
			return false;
		}
		for (uint32_t viewportIndex = 0; viewportIndex < viewportCount; viewportIndex++) {
			_dynamicViewports[viewportIndex] = viewports[viewportIndex];
		}
		_dynamicViewportCount = viewportCount;
		_graphicsViewportScissorAppliedForEncoder = false;
		return true;
	}

	bool setScissors(uint32_t firstScissor,
					 uint32_t scissorCount,
					 const VkRect2D* scissors) override {
		if (firstScissor != 0 || scissorCount == 0 ||
			scissorCount > kMVKMaxViewportScissorCount || !scissors) {
			return false;
		}
		for (uint32_t scissorIndex = 0; scissorIndex < scissorCount; scissorIndex++) {
			_dynamicScissors[scissorIndex] = scissors[scissorIndex];
		}
		_dynamicScissorCount = scissorCount;
		_graphicsViewportScissorAppliedForEncoder = false;
		return true;
	}

	bool setStencilCompareMask(VkStencilFaceFlags faceMask,
							   uint32_t stencilCompareMask) override {
		if (!isValidStencilFaceMask(faceMask)) { return false; }
		if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
			_dynamicStencilCompareMask.frontFaceValue = stencilCompareMask;
		}
		if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
			_dynamicStencilCompareMask.backFaceValue = stencilCompareMask;
		}
		_graphicsPipelineBoundForEncoder = false;
		return true;
	}

	bool setStencilWriteMask(VkStencilFaceFlags faceMask,
							 uint32_t stencilWriteMask) override {
		if (!isValidStencilFaceMask(faceMask)) { return false; }
		if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
			_dynamicStencilWriteMask.frontFaceValue = stencilWriteMask;
		}
		if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
			_dynamicStencilWriteMask.backFaceValue = stencilWriteMask;
		}
		_graphicsPipelineBoundForEncoder = false;
		return true;
	}

	bool setStencilReference(VkStencilFaceFlags faceMask,
							 uint32_t stencilReference) override {
		if (!isValidStencilFaceMask(faceMask)) { return false; }
		if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
			_dynamicStencilReference.frontFaceValue = stencilReference;
		}
		if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
			_dynamicStencilReference.backFaceValue = stencilReference;
		}
		_graphicsPipelineBoundForEncoder = false;
		return true;
	}

	bool setDepthBias(float constantFactor,
					  float clamp,
					  float slopeFactor) override {
		_dynamicDepthBias.depthBiasConstantFactor = constantFactor;
		_dynamicDepthBias.depthBiasClamp = clamp;
		_dynamicDepthBias.depthBiasSlopeFactor = slopeFactor;
		_hasDynamicDepthBias = true;
		_graphicsPipelineBoundForEncoder = false;
		return true;
	}

	bool setBlendConstants(const float* blendConstants) override {
		if (!blendConstants) { return false; }
		mvkCopy(_dynamicBlendConstants.float32, blendConstants, 4);
		_hasDynamicBlendConstants = true;
		_graphicsBlendConstantsAppliedForEncoder = false;
		return true;
	}

	bool bindVertexBuffers(uint32_t firstBinding,
						   uint32_t bindingCount,
						   const MVKVertexMTLBufferBinding* bindings) override {
		if (!bindingCount || !bindings ||
			firstBinding + bindingCount > kMVKMaxBufferCount) {
			return false;
		}
		for (uint32_t bindingOffset = 0; bindingOffset < bindingCount; bindingOffset++) {
			const MVKVertexMTLBufferBinding& binding = bindings[bindingOffset];
			NSUInteger bufferLength = binding.mtlBuffer.length;
			if (!binding.mtlBuffer || binding.offset > bufferLength ||
				(binding.size && binding.size > bufferLength - binding.offset)) {
				return false;
			}
			_graphicsVertexBuffers[firstBinding + bindingOffset] = BoundVertexBuffer{
				binding.mtlBuffer,
				static_cast<NSUInteger>(binding.offset),
				binding.size ? binding.size : bufferLength - binding.offset,
				binding.stride,
			};
		}
		_graphicsResourcesBoundForEncoder = false;
		return true;
	}

	bool bindIndexBuffer(MVKBuffer* buffer,
						 VkDeviceSize offset,
						 VkDeviceSize size,
						 VkIndexType indexType) override {
		auto binding = _buffers.find(buffer);
		if (!buffer || binding == _buffers.end() ||
			(indexType != VK_INDEX_TYPE_UINT16 && indexType != VK_INDEX_TYPE_UINT32) ||
			offset > buffer->getByteCount() || size > buffer->getByteCount() - offset) {
			return false;
		}
		MTLIndexType mtlIndexType = indexType == VK_INDEX_TYPE_UINT16
			? MTLIndexTypeUInt16
			: MTLIndexTypeUInt32;
		NSUInteger indexSize = indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
		NSUInteger mtlOffset = binding->second.offset + offset;
		if (mtlOffset % indexSize != 0 ||
			mtlOffset > binding->second.buffer.length ||
			size > binding->second.buffer.length - mtlOffset) {
			return false;
		}
		_boundIndexBuffer = BoundIndexBuffer{
			binding->second.buffer,
			mtlOffset,
			static_cast<NSUInteger>(size),
			mtlIndexType,
		};
		return true;
	}

	bool bindDescriptorSets(VkPipelineBindPoint bindPoint,
							MVKPipelineLayout* layout,
							uint32_t firstSet,
							uint32_t setCount,
							MVKDescriptorSet*const* descriptorSets) override {
		if ((bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS &&
			 bindPoint != VK_PIPELINE_BIND_POINT_COMPUTE) || !layout || !setCount ||
			!descriptorSets || firstSet + setCount > kMVKMaxDescriptorSetCount ||
			firstSet + setCount > layout->getDescriptorSetCount()) {
			return false;
		}
		auto& boundSets = bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE
			? _computeDescriptorSets
			: _graphicsDescriptorSets;
		for (uint32_t setOffset = 0; setOffset < setCount; setOffset++) {
			MVKDescriptorSet* descriptorSet = descriptorSets[setOffset];
			MVKDescriptorSetLayout* descriptorLayout =
				layout->getDescriptorSetLayout(firstSet + setOffset);
			if (!descriptorSet || !descriptorSet->supportsMetal4ArgumentTable() ||
				descriptorSet->layout != descriptorLayout ||
				(descriptorLayout->gpuSize() && !descriptorSet->gpuBufferObject)) {
				return false;
			}
			boundSets[firstSet + setOffset] = BoundDescriptorSet{
				descriptorLayout,
				descriptorSet->gpuBufferObject,
				descriptorSet->gpuBufferOffset,
			};
		}
		if (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
			_computeResourcesBoundForEncoder = false;
		} else {
			_graphicsResourcesBoundForEncoder = false;
		}
		return true;
	}

	bool draw(uint32_t firstVertex,
			  uint32_t vertexCount,
			  uint32_t firstInstance,
			  uint32_t instanceCount) override {
		if (!_renderEncoder || !_boundGraphicsPipeline || !vertexCount || !instanceCount) {
			return false;
		}
		if (!_graphicsPipelineBoundForEncoder && !applyGraphicsPipeline()) { return false; }
		if (!_boundGraphicsPipeline->isRasterizationDisabled() &&
			!_graphicsViewportScissorAppliedForEncoder &&
			!applyViewportScissorState()) {
			return false;
		}
		if (!_boundGraphicsPipeline->isRasterizationDisabled() &&
			!_graphicsBlendConstantsAppliedForEncoder &&
			!applyBlendConstantsState()) {
			return false;
		}
		if (!_graphicsResourcesBoundForEncoder && !applyGraphicsResources()) { return false; }
		const auto& stateData = _boundGraphicsPipeline->getStaticStateData();
		[_renderEncoder drawPrimitives:(MTLPrimitiveType)stateData.primitiveType
						 vertexStart:firstVertex
						 vertexCount:vertexCount
					   instanceCount:instanceCount
						baseInstance:firstInstance];
		_counters.draws++;
		_renderWork = true;
		return true;
	}

	bool drawIndexed(uint32_t firstIndex,
						 uint32_t indexCount,
						 int32_t vertexOffset,
						 uint32_t firstInstance,
						 uint32_t instanceCount) override {
		if (!_renderEncoder || !_boundGraphicsPipeline || !_boundIndexBuffer.buffer ||
			!indexCount || !instanceCount) {
			return false;
		}
		NSUInteger indexSize = _boundIndexBuffer.type == MTLIndexTypeUInt16 ? 2 : 4;
		uint64_t firstIndexOffset = uint64_t(firstIndex) * indexSize;
		uint64_t indexBytes = uint64_t(indexCount) * indexSize;
		if (firstIndexOffset > _boundIndexBuffer.size ||
			indexBytes > _boundIndexBuffer.size - firstIndexOffset ||
			firstIndexOffset > NSUIntegerMax - _boundIndexBuffer.offset) {
			return false;
		}
		NSUInteger indexBufferOffset =
			_boundIndexBuffer.offset + static_cast<NSUInteger>(firstIndexOffset);
		NSUInteger indexBufferLength = static_cast<NSUInteger>(indexBytes);
		if (!_graphicsPipelineBoundForEncoder && !applyGraphicsPipeline()) { return false; }
		if (!_boundGraphicsPipeline->isRasterizationDisabled() &&
			!_graphicsViewportScissorAppliedForEncoder &&
			!applyViewportScissorState()) {
			return false;
		}
		if (!_boundGraphicsPipeline->isRasterizationDisabled() &&
			!_graphicsBlendConstantsAppliedForEncoder &&
			!applyBlendConstantsState()) {
			return false;
		}
		if (!_graphicsResourcesBoundForEncoder && !applyGraphicsResources()) { return false; }
		const auto& stateData = _boundGraphicsPipeline->getStaticStateData();
		[_renderEncoder drawIndexedPrimitives:(MTLPrimitiveType)stateData.primitiveType
								 indexCount:indexCount
								  indexType:_boundIndexBuffer.type
								indexBuffer:_boundIndexBuffer.buffer.gpuAddress + indexBufferOffset
						  indexBufferLength:indexBufferLength
							  instanceCount:instanceCount
								 baseVertex:vertexOffset
							   baseInstance:firstInstance];
		_counters.draws++;
		_renderWork = true;
		return true;
	}

	bool useClearAttachments(const MVKMetal4ClearAttachmentsInfo& info) override {
		if (!_device || !_mtlDevice || !info.commandKey || !info.encodingPool ||
			!info.rects || !info.rectCount ||
			info.colorAttachmentCount > kMVKMaxColorAttachmentCount ||
			!info.framebufferLayerCount ||
			!ensureArgumentTable()) {
			return false;
		}
		if (_clearAttachments.count(info.commandKey)) { return true; }

		ClearAttachmentsBinding binding;
		binding.rects.assign(info.rects, info.rects + info.rectCount);
		for (const VkClearRect& rect : binding.rects) {
			if (!rect.layerCount || rect.baseArrayLayer >= info.framebufferLayerCount ||
				rect.layerCount > info.framebufferLayerCount - rect.baseArrayLayer ||
				rect.layerCount > (NSUIntegerMax - binding.vertexCount) / 6) {
				return false;
			}
			binding.vertexCount += static_cast<NSUInteger>(rect.layerCount) * 6;
		}
		binding.stencilReference = info.depthStencilValue.stencil;
		binding.pipelineKey.mtlSampleCount = 1;
		if (info.framebufferLayerCount > 1) {
			binding.pipelineKey.enableLayeredRendering();
		}
		simd::float4 clearColors[kMVKClearAttachmentCount] = {};
		MVKPixelFormats* pixelFormats = _pixelFormats;
		if (!pixelFormats) { return false; }
		for (uint32_t colorIndex = 0;
			 colorIndex < info.colorAttachmentCount;
			 colorIndex++) {
			VkFormat format = info.colorAttachmentFormats[colorIndex];
			binding.pipelineKey.attachmentMTLPixelFormats[colorIndex] =
				pixelFormats->getMTLPixelFormat(format);
			if (info.clearColors[colorIndex]) {
				binding.pipelineKey.enableAttachment(colorIndex);
				MTLClearColor color =
					pixelFormats->getMTLClearColor(info.colorValues[colorIndex], format);
				clearColors[colorIndex] = {
					(float)color.red, (float)color.green,
					(float)color.blue, (float)color.alpha,
				};
			}
		}
		binding.pipelineKey.attachmentMTLPixelFormats[kMVKClearAttachmentDepthIndex] =
			pixelFormats->getMTLPixelFormat(info.depthFormat);
		binding.pipelineKey.attachmentMTLPixelFormats[kMVKClearAttachmentStencilIndex] =
			pixelFormats->getMTLPixelFormat(info.stencilFormat);
		if (info.clearDepth) {
			binding.pipelineKey.enableAttachment(kMVKClearAttachmentDepthIndex);
			float depth = info.depthStencilValue.depth;
			clearColors[kMVKClearAttachmentDepthIndex] = { depth, depth, depth, depth };
		}
		if (info.clearStencil) {
			binding.pipelineKey.enableAttachment(kMVKClearAttachmentStencilIndex);
		}

		if (binding.pipelineKey.isAnyAttachmentEnabled()) {
			id<MTLRenderPipelineState> pipelineState =
				info.encodingPool->getCmdClearMTLRenderPipelineState(binding.pipelineKey);
			id<MTLDepthStencilState> depthStencilState =
				info.encodingPool->getMTLDepthStencilState(info.clearDepth, info.clearStencil);
			if (!pipelineState || !depthStencilState) { return false; }
			binding.pipelineState = [pipelineState retain];
			binding.depthStencilState = [depthStencilState retain];
			binding.clearColors = [_mtlDevice newBufferWithBytes:clearColors
													  length:sizeof(clearColors)
													 options:MTLResourceStorageModeShared |
															 MTLResourceCPUCacheModeWriteCombined];
			binding.vertices = [_mtlDevice newBufferWithLength:
				binding.vertexCount * sizeof(simd::float4)
												 options:MTLResourceStorageModeShared |
															 MTLResourceCPUCacheModeWriteCombined];
			if (!binding.clearColors || !binding.vertices) {
				[binding.pipelineState release];
				[binding.depthStencilState release];
				[binding.clearColors release];
				[binding.vertices release];
				return false;
			}
			_allocations.push_back((id<MTLAllocation>)binding.pipelineState);
			_allocations.push_back((id<MTLAllocation>)binding.clearColors);
			_allocations.push_back((id<MTLAllocation>)binding.vertices);
		}
		_clearAttachments.emplace(info.commandKey, std::move(binding));
		return true;
	}

	bool clearAttachments(const void* commandKey) override {
		auto item = _clearAttachments.find(commandKey);
		if (!_renderEncoder || item == _clearAttachments.end() || _activeQueryPool) {
			return false;
		}
		ClearAttachmentsBinding& binding = item->second;
		if (!binding.pipelineKey.isAnyAttachmentEnabled()) { return true; }
		if (!binding.pipelineState || !binding.depthStencilState ||
			!binding.clearColors || !binding.vertices ||
			!_currentRenderWidth || !_currentRenderHeight) {
			return false;
		}

		auto* vertices = static_cast<simd::float4*>(binding.vertices.contents);
		NSUInteger vertexIndex = 0;
		for (const VkClearRect& clearRect : binding.rects) {
			uint64_t right = uint64_t(clearRect.rect.offset.x) + clearRect.rect.extent.width;
			uint64_t bottom = uint64_t(clearRect.rect.offset.y) + clearRect.rect.extent.height;
			if (clearRect.rect.offset.x < 0 || clearRect.rect.offset.y < 0 ||
				!clearRect.layerCount ||
				clearRect.baseArrayLayer >= _currentRenderLayerCount ||
				clearRect.layerCount >
					_currentRenderLayerCount - clearRect.baseArrayLayer ||
				right > _currentRenderWidth || bottom > _currentRenderHeight) {
				return false;
			}
			float leftPos = (float)clearRect.rect.offset.x / _currentRenderWidth;
			float rightPos = (float)right / _currentRenderWidth;
			float bottomPos = (float)clearRect.rect.offset.y / _currentRenderHeight;
			float topPos = (float)bottom / _currentRenderHeight;
			leftPos = leftPos * 2.0f - 1.0f;
			rightPos = rightPos * 2.0f - 1.0f;
			bottomPos = bottomPos * 2.0f - 1.0f;
			topPos = topPos * 2.0f - 1.0f;
			uint32_t endLayer = clearRect.baseArrayLayer + clearRect.layerCount;
			for (uint32_t layer = clearRect.baseArrayLayer; layer < endLayer; layer++) {
				float layerValue = static_cast<float>(layer);
				vertices[vertexIndex++] = { leftPos, topPos, 0.0f, layerValue };
				vertices[vertexIndex++] = { leftPos, bottomPos, 0.0f, layerValue };
				vertices[vertexIndex++] = { rightPos, bottomPos, 0.0f, layerValue };
				vertices[vertexIndex++] = { rightPos, bottomPos, 0.0f, layerValue };
				vertices[vertexIndex++] = { rightPos, topPos, 0.0f, layerValue };
				vertices[vertexIndex++] = { leftPos, topPos, 0.0f, layerValue };
			}
		}

		[_renderEncoder setRenderPipelineState:binding.pipelineState];
		[_renderEncoder setDepthStencilState:binding.depthStencilState];
		[_renderEncoder setCullMode:MTLCullModeNone];
		[_renderEncoder setFrontFacingWinding:MTLWindingCounterClockwise];
		[_renderEncoder setTriangleFillMode:MTLTriangleFillModeFill];
		[_renderEncoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
		[_renderEncoder setStencilReferenceValue:binding.stencilReference];
		MTLViewport viewport = {
			0.0, 0.0, (double)_currentRenderWidth, (double)_currentRenderHeight, 0.0, 1.0,
		};
		MTLScissorRect scissor = { 0, 0, _currentRenderWidth, _currentRenderHeight };
		[_renderEncoder setViewport:viewport];
		[_renderEncoder setScissorRect:scissor];
		[_argumentTable setAddress:binding.clearColors.gpuAddress atIndex:0];
		uint32_t vertexBufferIndex =
			_device->getMetalBufferIndexForVertexAttributeBinding(kMVKVertexContentBufferIndex);
		[_argumentTable setAddress:binding.vertices.gpuAddress
				 attributeStride:sizeof(simd::float4)
						 atIndex:vertexBufferIndex];
		[_renderEncoder setArgumentTable:_argumentTable
						 atStages:MTLRenderStageVertex | MTLRenderStageFragment];
		[_renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle
					 vertexStart:0
					 vertexCount:binding.vertexCount];
		_graphicsPipelineBoundForEncoder = false;
		_graphicsResourcesBoundForEncoder = false;
		_graphicsViewportScissorAppliedForEncoder = false;
		_graphicsBlendConstantsAppliedForEncoder = false;
		_counters.draws++;
		_renderWork = true;
		return true;
	}

	void endEncoding() {
		endRenderEncoding();
		endComputeEncoding();
	}

	void abandonEncoding() {
		[_renderEncoder release];
		_renderEncoder = nil;
		[_computeEncoder release];
		_computeEncoder = nil;
		_commandBuffer = nil;
		_boundComputePipeline = nullptr;
		_currentColorAttachmentCount = 0;
		_currentDepthFormat = VK_FORMAT_UNDEFINED;
		_currentStencilFormat = VK_FORMAT_UNDEFINED;
		_currentRenderWidth = 0;
		_currentRenderHeight = 0;
		_currentRenderLayerCount = 0;
		_graphicsPipelineBoundForEncoder = false;
		_graphicsResourcesBoundForEncoder = false;
		_graphicsViewportScissorAppliedForEncoder = false;
		_graphicsBlendConstantsAppliedForEncoder = false;
	}

	const vector<id<MTLAllocation>>& getAllocations() const { return _allocations; }
	bool hasRenderWork() const { return _renderWork; }
	void publishCommittedCounters() {
		_state->recordBufferCopy(_counters.bufferCopies);
		_state->recordBufferFill(_counters.bufferFills);
		_state->recordBufferUpdate(_counters.bufferUpdates);
		_state->recordImageCopy(_counters.imageCopies);
		_state->recordComputeDispatch(_counters.computeDispatches);
		_state->recordRenderPass(_counters.renderPasses);
		_state->recordDraw(_counters.draws);
		_state->recordBarrier(_counters.barriers);
		_state->recordQueryReset(_counters.queryResets);
		_state->recordQueryCopy(_counters.queryCopies);
		_state->recordVisibilityQuery(_counters.visibilityQueries);
	}

	void publishCommittedState() {
		for (const auto& barrier : _pendingImageBarriers) {
			barrier.mvkImage->applyMetal4ImageLayoutTransition(barrier);
		}
		for (const auto& reset : _pendingQueryResets) {
			reset.queryPool->applyMetal4Reset(reset.firstQuery, reset.queryCount);
		}
		for (const auto& query : _completedQueries) {
			query.queryPool->applyMetal4End(query.query);
		}
	}

	const vector<MVKMetal4CompletedQuery>& getCompletedQueries() const {
		return _completedQueries;
	}

private:
	bool ensureComputeEncoder() {
		if (!_commandBuffer || _renderEncoder) { return false; }
		if (_computeEncoder) { return true; }
		_computeEncoder = [[_commandBuffer computeCommandEncoder] retain];
		if (!_computeEncoder) { return false; }
		applyPendingBarriers(_computeEncoder, MTLStageDispatch | MTLStageBlit);
		return true;
	}

	void applyPendingBarriers(id<MTL4CommandEncoder> encoder, MTLStages supportedStages) {
		for (auto it = _pendingBarriers.begin(); it != _pendingBarriers.end();) {
			MTLStages before = it->beforeStages & supportedStages;
			if (!before) {
				++it;
				continue;
			}
			[encoder barrierAfterQueueStages:it->afterQueueStages
							 beforeStages:before
						visibilityOptions:it->visibilityOptions];
			it->beforeStages &= ~before;
			if (it->beforeStages) {
				++it;
			} else {
				it = _pendingBarriers.erase(it);
			}
		}
	}

	void applyRenderAttachmentBarrier(
		id<MTL4RenderCommandEncoder> encoder,
		const vector<id<MTLTexture>>& renderAttachments) {
		bool overlapsPreviousPass = false;
		for (id<MTLTexture> attachment : renderAttachments) {
			if (find(_previousRenderAttachments.begin(),
					 _previousRenderAttachments.end(),
					 attachment) != _previousRenderAttachments.end()) {
				overlapsPreviousPass = true;
				break;
			}
		}
		if (overlapsPreviousPass) {
			[encoder barrierAfterQueueStages:MTLStageVertex | MTLStageFragment
						 beforeStages:MTLStageVertex | MTLStageFragment
					visibilityOptions:MTL4VisibilityOptionDevice];
			_counters.barriers++;
		}
		_previousRenderAttachments = renderAttachments;
	}

	void endComputeEncoding() {
		if (!_computeEncoder) { return; }
		[_computeEncoder endEncoding];
		[_computeEncoder release];
		_computeEncoder = nil;
		_boundComputePipeline = nullptr;
		_computeResourcesBoundForEncoder = false;
	}

	void endRenderEncoding() {
		if (!_renderEncoder) { return; }
		[_renderEncoder endEncoding];
		[_renderEncoder release];
		_renderEncoder = nil;
		_currentColorAttachmentCount = 0;
		_currentDepthFormat = VK_FORMAT_UNDEFINED;
		_currentStencilFormat = VK_FORMAT_UNDEFINED;
		_currentRenderWidth = 0;
		_currentRenderHeight = 0;
		_currentRenderLayerCount = 0;
		_graphicsPipelineBoundForEncoder = false;
		_graphicsResourcesBoundForEncoder = false;
		_graphicsViewportScissorAppliedForEncoder = false;
		_graphicsBlendConstantsAppliedForEncoder = false;
	}

	static bool isValidStencilFaceMask(VkStencilFaceFlags faceMask) {
		constexpr VkStencilFaceFlags valid = VK_STENCIL_FACE_FRONT_AND_BACK;
		return (faceMask & valid) != 0 && (faceMask & ~valid) == 0;
	}

	bool ensureArgumentTable() {
		if (_argumentTable) { return true; }
		if (!_mtlDevice) { return false; }
		MTL4ArgumentTableDescriptor* descriptor = [MTL4ArgumentTableDescriptor new];
		descriptor.maxBufferBindCount = kMVKMaxBufferCount;
		descriptor.maxTextureBindCount = kMVKMaxTextureCount;
		descriptor.maxSamplerStateBindCount = kMVKMaxSamplerCount;
		descriptor.initializeBindings = YES;
		descriptor.supportAttributeStrides = YES;
		NSError* error = nil;
		_argumentTable = [_mtlDevice newArgumentTableWithDescriptor:descriptor error:&error];
		[descriptor release];
		return _argumentTable != nil;
	}

	bool retainDescriptorAllocation(id<MTLAllocation> allocation) {
		if (!allocation) { return true; }
		if (_descriptorAllocationSet.insert((const void*)allocation).second) {
			[allocation retain];
			_descriptorAllocations.push_back(allocation);
			_allocations.push_back(allocation);
		}
		return true;
	}

	id<MTLDepthStencilState> newDepthStencilState(
		MVKGraphicsPipeline* pipeline,
		const MVKStencilReference& compareMask,
		const MVKStencilReference& writeMask) {
		if (!_mtlDevice || !pipeline) { return nil; }
		const auto& stateData = pipeline->getStaticStateData();
		MTLDepthStencilDescriptor* depthStencilDescriptor =
			[MTLDepthStencilDescriptor new];
		bool depthTestEnabled =
			stateData.enable.has(MVKRenderStateEnableFlag::DepthTest);
		depthStencilDescriptor.depthCompareFunction =
			depthTestEnabled
				? (MTLCompareFunction)stateData.depthStencil.depthCompareFunction
				: MTLCompareFunctionAlways;
		depthStencilDescriptor.depthWriteEnabled =
			depthTestEnabled && stateData.depthStencil.depthWriteEnabled;
		bool stencilTestEnabled = stateData.depthStencil.stencilTestEnabled;
		auto newStencilDescriptor = [](const MVKMTLStencilDescriptorData& stencilData,
									  uint32_t readMask,
									  uint32_t writeMaskValue) {
			MTLStencilDescriptor* descriptor = [MTLStencilDescriptor new];
			descriptor.stencilCompareFunction =
				(MTLCompareFunction)stencilData.op.stencilCompareFunction;
			descriptor.stencilFailureOperation =
				(MTLStencilOperation)stencilData.op.stencilFailureOperation;
			descriptor.depthFailureOperation =
				(MTLStencilOperation)stencilData.op.depthFailureOperation;
			descriptor.depthStencilPassOperation =
				(MTLStencilOperation)stencilData.op.depthStencilPassOperation;
			descriptor.readMask = readMask;
			descriptor.writeMask = writeMaskValue;
			return descriptor;
		};
		MTLStencilDescriptor* frontFaceStencil = stencilTestEnabled
			? newStencilDescriptor(stateData.depthStencil.frontFaceStencilData,
							   compareMask.frontFaceValue,
							   writeMask.frontFaceValue)
			: nil;
		MTLStencilDescriptor* backFaceStencil = stencilTestEnabled
			? newStencilDescriptor(stateData.depthStencil.backFaceStencilData,
							   compareMask.backFaceValue,
							   writeMask.backFaceValue)
			: nil;
		depthStencilDescriptor.frontFaceStencil = frontFaceStencil;
		depthStencilDescriptor.backFaceStencil = backFaceStencil;
		id<MTLDepthStencilState> state =
			[_mtlDevice newDepthStencilStateWithDescriptor:depthStencilDescriptor];
		[frontFaceStencil release];
		[backFaceStencil release];
		[depthStencilDescriptor release];
		return state;
	}

	bool applyGraphicsPipeline(bool allowIncompleteDynamicState = false) {
		if (!_renderEncoder || !_boundGraphicsPipeline) { return false; }
		auto it = _graphicsPipelines.find(_boundGraphicsPipeline);
		if (it == _graphicsPipelines.end() ||
			_boundGraphicsPipeline->getMetal4ColorAttachmentCount() !=
				_currentColorAttachmentCount ||
			_boundGraphicsPipeline->getMetal4DepthAttachmentFormat() != _currentDepthFormat ||
			_boundGraphicsPipeline->getMetal4StencilAttachmentFormat() != _currentStencilFormat) {
			return false;
		}
		for (uint32_t colorIndex = 0;
			 colorIndex < _currentColorAttachmentCount;
			 colorIndex++) {
			if (_boundGraphicsPipeline->getMetal4ColorAttachmentFormat(colorIndex) !=
				_currentColorAttachmentFormats[colorIndex]) {
				return false;
			}
		}
		const auto& stateData = _boundGraphicsPipeline->getStaticStateData();
		MVKStencilReference compareMask {
			stateData.depthStencil.frontFaceStencilData.readMask,
			stateData.depthStencil.backFaceStencilData.readMask,
		};
		MVKStencilReference writeMask {
			stateData.depthStencil.frontFaceStencilData.writeMask,
			stateData.depthStencil.backFaceStencilData.writeMask,
		};
		if (_boundGraphicsPipeline->usesMetal4DynamicStencilCompareMask()) {
			compareMask = _dynamicStencilCompareMask;
		}
		if (_boundGraphicsPipeline->usesMetal4DynamicStencilWriteMask()) {
			writeMask = _dynamicStencilWriteMask;
		}
		id<MTLDepthStencilState> depthStencilState = nil;
		for (auto& depthStencil : it->second.depthStencilStates) {
			if (depthStencil.compareMask.frontFaceValue == compareMask.frontFaceValue &&
				depthStencil.compareMask.backFaceValue == compareMask.backFaceValue &&
				depthStencil.writeMask.frontFaceValue == writeMask.frontFaceValue &&
				depthStencil.writeMask.backFaceValue == writeMask.backFaceValue) {
				depthStencilState = depthStencil.state;
				break;
			}
		}
		if (!depthStencilState) {
			depthStencilState = newDepthStencilState(
				_boundGraphicsPipeline, compareMask, writeMask);
			if (!depthStencilState) { return false; }
			it->second.depthStencilStates.push_back(
				DepthStencilStateBinding{compareMask, writeMask, depthStencilState});
		}
		[_renderEncoder setRenderPipelineState:it->second.pipelineState];
		[_renderEncoder setDepthStencilState:depthStencilState];
		[_renderEncoder setCullMode:(MTLCullMode)stateData.cullMode];
		[_renderEncoder setFrontFacingWinding:(MTLWinding)stateData.frontFace];
		[_renderEncoder setTriangleFillMode:(MTLTriangleFillMode)stateData.polygonMode];
		const MVKDepthBias* depthBias = &stateData.depthBias;
		if (_boundGraphicsPipeline->usesMetal4DynamicDepthBias()) {
			if (!_hasDynamicDepthBias) { return allowIncompleteDynamicState; }
			depthBias = &_dynamicDepthBias;
		}
		if (stateData.enable.has(MVKRenderStateEnableFlag::DepthBias)) {
			[_renderEncoder setDepthBias:depthBias->depthBiasConstantFactor
						 slopeScale:depthBias->depthBiasSlopeFactor
							  clamp:depthBias->depthBiasClamp];
		} else {
			[_renderEncoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
		}
		if (stateData.depthStencil.stencilTestEnabled) {
			uint32_t frontReference = stateData.stencilReference.frontFaceValue;
			uint32_t backReference = stateData.stencilReference.backFaceValue;
			if (_boundGraphicsPipeline->usesMetal4DynamicStencilReference()) {
				frontReference = _dynamicStencilReference.frontFaceValue;
				backReference = _dynamicStencilReference.backFaceValue;
			}
			if (frontReference == backReference) {
				[_renderEncoder setStencilReferenceValue:frontReference];
			} else {
				[_renderEncoder setStencilFrontReferenceValue:frontReference
									 backReferenceValue:backReference];
			}
		}
		_graphicsPipelineBoundForEncoder = true;
		return true;
	}

	bool applyViewportScissorState() {
		if (!_renderEncoder || !_boundGraphicsPipeline) { return false; }
		const auto& stateData = _boundGraphicsPipeline->getStaticStateData();
		const VkViewport* viewport = nullptr;
		const VkRect2D* scissor = nullptr;
		uint32_t viewportCount = 0;
		uint32_t scissorCount = 0;
		if (_boundGraphicsPipeline->usesMetal4DynamicViewport()) {
			if (_dynamicViewportCount == 0) { return false; }
			viewport = _dynamicViewports.data();
			viewportCount = _dynamicViewportCount;
		} else {
			if (stateData.numViewports != 1) { return false; }
			viewport = _boundGraphicsPipeline->getViewports();
			viewportCount = 1;
		}
		if (_boundGraphicsPipeline->usesMetal4DynamicScissor()) {
			if (_dynamicScissorCount == 0) { return false; }
			scissor = _dynamicScissors.data();
			scissorCount = _dynamicScissorCount;
		} else {
			if (stateData.numScissors != 1) { return false; }
			scissor = _boundGraphicsPipeline->getScissors();
			scissorCount = 1;
		}
		if (!viewport || !scissor || viewportCount == 0 || scissorCount == 0 ||
			viewportCount > kMVKMaxViewportScissorCount ||
			scissorCount > kMVKMaxViewportScissorCount) {
			return false;
		}
		MTLViewport mtlViewports[kMVKMaxViewportScissorCount];
		for (uint32_t viewportIndex = 0; viewportIndex < viewportCount; viewportIndex++) {
			mtlViewports[viewportIndex] = mvkMTLViewportFromVkViewport(viewport[viewportIndex]);
		}
		MTLScissorRect mtlScissors[kMVKMaxViewportScissorCount];
		for (uint32_t scissorIndex = 0; scissorIndex < scissorCount; scissorIndex++) {
			const VkRect2D& vkScissor = scissor[scissorIndex];
			if (vkScissor.offset.x < 0 || vkScissor.offset.y < 0 ||
				(uint64_t)vkScissor.offset.x + vkScissor.extent.width > _currentRenderWidth ||
				(uint64_t)vkScissor.offset.y + vkScissor.extent.height > _currentRenderHeight) {
				return false;
			}
			mtlScissors[scissorIndex] = mvkMTLScissorRectFromVkRect2D(vkScissor);
		}
		[_renderEncoder setViewports:mtlViewports count:viewportCount];
		[_renderEncoder setScissorRects:mtlScissors count:scissorCount];
		_graphicsViewportScissorAppliedForEncoder = true;
		return true;
	}

	bool applyBlendConstantsState() {
		if (!_renderEncoder || !_boundGraphicsPipeline) { return false; }
		if (!_boundGraphicsPipeline->usesMetal4BlendConstants()) {
			_graphicsBlendConstantsAppliedForEncoder = true;
			return true;
		}
		const MVKColor32* blendConstants = nullptr;
		if (_boundGraphicsPipeline->usesMetal4DynamicBlendConstants()) {
			if (!_hasDynamicBlendConstants) { return false; }
			blendConstants = &_dynamicBlendConstants;
		} else {
			blendConstants = &_boundGraphicsPipeline->getStaticStateData().blendConstants;
		}
		const float* color = blendConstants->float32;
		[_renderEncoder setBlendColorRed:color[0]
							 green:color[1]
							  blue:color[2]
							 alpha:color[3]];
		_graphicsBlendConstantsAppliedForEncoder = true;
		return true;
	}

	bool applyGraphicsResources() {
		if (!_renderEncoder || !_boundGraphicsPipeline) { return false; }
		if (_boundGraphicsPipeline->requiresMetal4ArgumentTable() && !ensureArgumentTable()) {
			return false;
		}
		MVKPipelineLayout* pipelineLayout = _boundGraphicsPipeline->getLayout();
		MTLRenderStages stages = 0;
		for (size_t vkBinding : _boundGraphicsPipeline->getVkVertexBuffers()) {
			const BoundVertexBuffer& vertexBuffer = _graphicsVertexBuffers[vkBinding];
			if (!vertexBuffer.buffer || vertexBuffer.offset > vertexBuffer.buffer.length) {
				return false;
			}
			uint32_t metalBinding =
				_boundGraphicsPipeline->getMetalBufferIndexForVertexAttributeBinding(
					static_cast<uint32_t>(vkBinding));
			if (_boundGraphicsPipeline->usesMetal4DynamicVertexStride()) {
				[_argumentTable setAddress:vertexBuffer.buffer.gpuAddress + vertexBuffer.offset
						 attributeStride:vertexBuffer.stride
								 atIndex:metalBinding];
			} else {
				[_argumentTable setAddress:vertexBuffer.buffer.gpuAddress + vertexBuffer.offset
								 atIndex:metalBinding];
			}
			stages |= MTLRenderStageVertex;
		}
		if (_boundGraphicsPipeline->supportsMetal4ArgumentTableRenderExecution()) {
			for (MVKShaderStage stage : {kMVKShaderStageVertex, kMVKShaderStageFragment}) {
				const auto& resources = _boundGraphicsPipeline->getStageResources(stage);
				for (size_t setIndex : resources.resources.descriptorSetData) {
					if (setIndex >= pipelineLayout->getDescriptorSetCount()) { return false; }
					const BoundDescriptorSet& descriptorSet = _graphicsDescriptorSets[setIndex];
					if (!descriptorSet.buffer ||
						descriptorSet.layout != pipelineLayout->getDescriptorSetLayout(setIndex)) {
						return false;
					}
					[_argumentTable setAddress:descriptorSet.buffer.gpuAddress + descriptorSet.offset
								 atIndex:setIndex];
				}
				if (resources.resources.descriptorSetData.areAnyBitsSet()) {
					stages |= stage == kMVKShaderStageVertex
						? MTLRenderStageVertex
						: MTLRenderStageFragment;
				}
			}
		} else if (!_boundGraphicsPipeline->supportsMetal4DescriptorlessRenderExecution()) {
			return false;
		}
		if (stages) { [_renderEncoder setArgumentTable:_argumentTable atStages:stages]; }
		_graphicsResourcesBoundForEncoder = true;
		return true;
	}

	bool applyComputeResources() {
		if (!_computeEncoder || !_boundComputePipeline) { return false; }
		if (_boundComputePipeline->supportsMetal4ArgumentTableExecution()) {
			if (!ensureArgumentTable()) { return false; }
			MVKPipelineLayout* pipelineLayout = _boundComputePipeline->getLayout();
			if (!pipelineLayout) { return false; }
			const auto& resources = _boundComputePipeline->getStageResources();
			for (size_t setIndex : resources.resources.descriptorSetData) {
				if (setIndex >= pipelineLayout->getDescriptorSetCount()) { return false; }
				const BoundDescriptorSet& descriptorSet = _computeDescriptorSets[setIndex];
				if (!descriptorSet.buffer ||
					descriptorSet.layout != pipelineLayout->getDescriptorSetLayout(setIndex)) {
					return false;
				}
				[_argumentTable setAddress:descriptorSet.buffer.gpuAddress + descriptorSet.offset
							 atIndex:setIndex];
			}
			[_computeEncoder setArgumentTable:_argumentTable];
		} else if (!_boundComputePipeline->supportsMetal4DescriptorlessExecution()) {
			return false;
		}
		_computeResourcesBoundForEncoder = true;
		return true;
	}

	shared_ptr<MVKMetal4CommandQueueState> _state;
	unordered_map<MVKBuffer*, BufferBinding> _buffers;
	unordered_map<MVKImage*, ImageBinding> _images;
	unordered_map<MVKImageView*, ImageViewBinding> _imageViews;
	unordered_map<MVKQueryPool*, QueryPoolBinding> _queryPools;
	unordered_map<const void*, UpdateDataBinding> _updateData;
	unordered_map<MVKComputePipeline*, id<MTLComputePipelineState>> _computePipelines;
	unordered_map<MVKGraphicsPipeline*, GraphicsPipelineBinding> _graphicsPipelines;
	unordered_map<const void*, ClearAttachmentsBinding> _clearAttachments;
	array<BoundVertexBuffer, kMVKMaxBufferCount> _graphicsVertexBuffers = {};
	BoundIndexBuffer _boundIndexBuffer = {};
	array<BoundDescriptorSet, kMVKMaxDescriptorSetCount> _computeDescriptorSets = {};
	array<BoundDescriptorSet, kMVKMaxDescriptorSetCount> _graphicsDescriptorSets = {};
	array<MVKDescriptorSet*, kMVKMaxDescriptorSetCount>
		_preparedComputeDescriptorSets = {};
	array<MVKDescriptorSet*, kMVKMaxDescriptorSetCount>
		_preparedGraphicsDescriptorSets = {};
	unordered_set<const void*> _descriptorAllocationSet;
	vector<id<MTLAllocation>> _descriptorAllocations;
	vector<PendingBarrier> _pendingBarriers;
	vector<id<MTLTexture>> _previousRenderAttachments;
	vector<MVKPipelineBarrier> _pendingImageBarriers;
	vector<PendingQueryReset> _pendingQueryResets;
	vector<MVKMetal4CompletedQuery> _completedQueries;
	vector<id<MTLAllocation>> _allocations;
	MVKDevice* _device = nullptr;
	MVKPixelFormats* _pixelFormats = nullptr;
	id<MTLDevice> _mtlDevice = nil;
	id<MTL4CommandBuffer> _commandBuffer = nil;
	id<MTL4ComputeCommandEncoder> _computeEncoder = nil;
	id<MTL4RenderCommandEncoder> _renderEncoder = nil;
	id<MTL4ArgumentTable> _argumentTable = nil;
	MVKComputePipeline* _boundComputePipeline = nullptr;
	MVKGraphicsPipeline* _boundGraphicsPipeline = nullptr;
	MVKComputePipeline* _preparedComputePipeline = nullptr;
	MVKGraphicsPipeline* _preparedGraphicsPipeline = nullptr;
	MVKQueryPool* _visibilityQueryPool = nullptr;
	MVKQueryPool* _activeQueryPool = nullptr;
	uint32_t _activeQuery = 0;
	VkFormat _currentColorAttachmentFormats[kMVKMaxColorAttachmentCount] = {};
	uint32_t _currentColorAttachmentCount = 0;
	bool _computeResourcesBoundForEncoder = false;
	VkFormat _currentDepthFormat = VK_FORMAT_UNDEFINED;
	VkFormat _currentStencilFormat = VK_FORMAT_UNDEFINED;
	NSUInteger _currentRenderWidth = 0;
	NSUInteger _currentRenderHeight = 0;
	uint32_t _currentRenderLayerCount = 0;
	array<VkViewport, kMVKMaxViewportScissorCount> _dynamicViewports = {};
	array<VkRect2D, kMVKMaxViewportScissorCount> _dynamicScissors = {};
	uint32_t _dynamicViewportCount = 0;
	uint32_t _dynamicScissorCount = 0;
	MVKColor32 _dynamicBlendConstants = {};
	MVKDepthBias _dynamicDepthBias = {};
	MVKStencilReference _dynamicStencilCompareMask = {};
	MVKStencilReference _dynamicStencilWriteMask = {};
	MVKStencilReference _dynamicStencilReference = {};
	bool _graphicsPipelineBoundForEncoder = false;
	bool _graphicsResourcesBoundForEncoder = false;
	bool _graphicsViewportScissorAppliedForEncoder = false;
	bool _graphicsBlendConstantsAppliedForEncoder = false;
	bool _hasDynamicBlendConstants = false;
	bool _hasDynamicDepthBias = false;
	bool _renderWork = false;
	CommandCounters _counters;
	const char* _encodingFailureCommand = nullptr;
};

/** Idempotent owner shared by MTL4 commit feedback and queue-order completion. */
struct MVKMetal4SubmissionCompletion {
	mutex lock;
	MVKQueueCommandBufferSubmission* submission = nullptr;
	MVKQueue* queue = nullptr;
	shared_ptr<MVKMetal4CommandQueueState> state;
	vector<id<MTLAllocation>> allocations;
	vector<MVKMetal4CompletedQuery> completedQueries;
	size_t allocatorIndex = 0;
	uint64_t sequence = 0;
	uint64_t startTime = 0;
	NSError* feedbackError = nil;
	bool schedulingComplete = false;
	bool completionRequested = false;
	bool completed = false;

	MVKMetal4SubmissionCompletion(MVKQueueCommandBufferSubmission* mvkSubmission,
								  MVKQueue* mvkQueue,
								  shared_ptr<MVKMetal4CommandQueueState> sharedState,
								  const vector<id<MTLAllocation>>& residentAllocations,
								  const vector<MVKMetal4CompletedQuery>& submissionQueries,
								  size_t slotIndex,
								  uint64_t orderValue,
								  uint64_t gpuStartTime) :
		submission(mvkSubmission),
		queue(mvkQueue),
		state(std::move(sharedState)),
		allocations(residentAllocations),
		completedQueries(submissionQueries),
		allocatorIndex(slotIndex),
		sequence(orderValue),
		startTime(gpuStartTime) {
		queue->retain();
	}

	~MVKMetal4SubmissionCompletion() {
		[feedbackError release];
		queue->release();
	}

	void receiveFeedback(id<MTL4CommitFeedback> feedback) {
		NSError* error = [feedback.error retain];
		{
			lock_guard<mutex> guard(lock);
			[feedbackError release];
			feedbackError = error;
		}
		if (error) {
			state->recordFailure(error.localizedDescription);
			queue->reportResult(VK_ERROR_DEVICE_LOST,
							MVK_CONFIG_LOG_LEVEL_ERROR,
							"Metal 4 Vulkan submission failed: %s",
							error.localizedDescription.UTF8String ?: "unknown error");
			queue->getDevice()->markLost(false);
		}

		// Apple invokes commit feedback after the workload completes. It is the
		// authoritative lifetime boundary for allocators, residency, and Vulkan
		// fence/semaphore completion. Host-signaling here also repairs ordering if
		// an exception occurred after commit but before queue signalEvent().
		state->hostSignalOrdering(sequence);
		complete();
	}

	void finalize(MVKQueueCommandBufferSubmission* completedSubmission) {
		for (const auto& query : completedQueries) {
			query.queryPool->finishMetal4Query(query.query);
		}
		state->completeAllocator(allocatorIndex);
		state->releaseResidency(allocations);
		queue->addPerformanceInterval(queue->getPerformanceStats().queue.mtlCommandBufferExecution, startTime);
		completedSubmission->finish();
	}

	void complete() {
		MVKQueueCommandBufferSubmission* completedSubmission = nullptr;
		{
			lock_guard<mutex> guard(lock);
			completionRequested = true;
			if (!schedulingComplete || completed) { return; }
			completed = true;
			completedSubmission = submission;
			submission = nullptr;
		}
		finalize(completedSubmission);
	}

	void markSchedulingComplete() {
		MVKQueueCommandBufferSubmission* completedSubmission = nullptr;
		{
			lock_guard<mutex> guard(lock);
			schedulingComplete = true;
			if (!completionRequested || completed) { return; }
			completed = true;
			completedSubmission = submission;
			submission = nullptr;
		}
		finalize(completedSubmission);
	}
};
#endif


#pragma mark -
#pragma mark MVKQueueFamily

// MTLCommandQueues are cached in MVKQueueFamily/MVKPhysicalDevice because they are very
// limited in number. An app that creates multiple VkDevices over time (such as a test suite)
// will soon find 15 second delays when creating subsequent MTLCommandQueues.
id<MTLCommandQueue> MVKQueueFamily::getMTLCommandQueue(uint32_t queueIndex) {
	lock_guard<mutex> lock(_qLock);
	id<MTLCommandQueue> mtlQ = _mtlQueues[queueIndex];
	if ( !mtlQ ) {
		@autoreleasepool {		// Catch any autoreleased objects created during MTLCommandQueue creation
			uint32_t maxCmdBuffs = getMVKConfig().maxActiveMetalCommandBuffersPerQueue;
			mtlQ = [_physicalDevice->getMTLDevice() newCommandQueueWithMaxCommandBufferCount: maxCmdBuffs];		// retained
			_mtlQueues[queueIndex] = mtlQ;
		}
	}
	return mtlQ;
}

MVKQueueFamily::MVKQueueFamily(MVKPhysicalDevice* physicalDevice, uint32_t queueFamilyIndex, const VkQueueFamilyProperties* pProperties) {
	_physicalDevice = physicalDevice;
	_queueFamilyIndex = queueFamilyIndex;
	_properties = *pProperties;
	_mtlQueues.assign(_properties.queueCount, nil);
}

MVKQueueFamily::~MVKQueueFamily() {
	mvkReleaseContainerContents(_mtlQueues);
}


#pragma mark -
#pragma mark MVKQueue

void MVKQueue::propagateDebugName() {
	setMetalObjectLabel(_mtlQueue, _debugName);
	// MTL4CommandQueue.label is fixed by MTL4CommandQueueDescriptor at creation.
}


#pragma mark Queue submissions

// Execute the queue submission under an autoreleasepool to ensure transient Metal objects are autoreleased.
// This is critical for apps that don't use standard OS autoreleasing runloop threading.
static inline VkResult execute(MVKQueueSubmission* qSubmit) { @autoreleasepool { return qSubmit->execute(); } }

// Executes the submmission, either immediately, or by dispatching to an execution queue.
// Submissions to the execution queue are wrapped in a dedicated autoreleasepool.
// Relying on the dispatch queue to find time to drain the autoreleasepool can
// result in significant memory creep under heavy workloads.
VkResult MVKQueue::submit(MVKQueueSubmission* qSubmit) {
	if (_device->getConfigurationResult() != VK_SUCCESS) { return _device->getConfigurationResult(); }

	if ( !qSubmit ) { return VK_SUCCESS; }     // Ignore nils

	// Extract result before submission to avoid race condition with early destruction
	// Submit regardless of config result, to ensure submission semaphores and fences are signalled.
	// The submissions will ensure a misconfiguration will be safe to execute.
	VkResult rslt = qSubmit->getConfigurationResult();
	if (_execQueue) {
		std::unique_lock lock(_execQueueMutex);
		_execQueueJobCount++;

		dispatch_async(_execQueue, ^{
			execute(qSubmit);

			std::unique_lock execLock(_execQueueMutex);
			if (!--_execQueueJobCount)
				_execQueueConditionVariable.notify_all();
		} );
	} else {
		rslt = execute(qSubmit);
	}
	return rslt;
}

static inline uint32_t getCommandBufferCount(const VkSubmitInfo2* pSubmitInfo) { return pSubmitInfo->commandBufferInfoCount; }
static inline uint32_t getCommandBufferCount(const VkSubmitInfo* pSubmitInfo) { return pSubmitInfo->commandBufferCount; }

template <typename S>
VkResult MVKQueue::submit(uint32_t submitCount, const S* pSubmits, VkFence fence, MVKCommandUse cmdUse) {

    // Fence-only submission
    if (submitCount == 0 && fence) {
        return submit(new MVKQueueCommandBufferSubmission(this, (S*)nullptr, fence, cmdUse));
    }

    VkResult rslt = VK_SUCCESS;
    for (uint32_t sIdx = 0; sIdx < submitCount; sIdx++) {
        VkFence fenceOrNil = (sIdx == (submitCount - 1)) ? fence : VK_NULL_HANDLE; // last one gets the fence

		const S* pVkSub = &pSubmits[sIdx];
		MVKQueueCommandBufferSubmission* mvkSub;
		uint32_t cbCnt = getCommandBufferCount(pVkSub);
		if (cbCnt <= 1) {
			mvkSub = new MVKQueueFullCommandBufferSubmission<1>(this, pVkSub, fenceOrNil, cmdUse);
		} else if (cbCnt <= 16) {
			mvkSub = new MVKQueueFullCommandBufferSubmission<16>(this, pVkSub, fenceOrNil, cmdUse);
		} else if (cbCnt <= 32) {
			mvkSub = new MVKQueueFullCommandBufferSubmission<32>(this, pVkSub, fenceOrNil, cmdUse);
		} else if (cbCnt <= 64) {
			mvkSub = new MVKQueueFullCommandBufferSubmission<64>(this, pVkSub, fenceOrNil, cmdUse);
		} else if (cbCnt <= 128) {
			mvkSub = new MVKQueueFullCommandBufferSubmission<128>(this, pVkSub, fenceOrNil, cmdUse);
		} else if (cbCnt <= 256) {
			mvkSub = new MVKQueueFullCommandBufferSubmission<256>(this, pVkSub, fenceOrNil, cmdUse);
		} else {
			mvkSub = new MVKQueueFullCommandBufferSubmission<512>(this, pVkSub, fenceOrNil, cmdUse);
		}

        VkResult subRslt = submit(mvkSub);
        if (rslt == VK_SUCCESS) { rslt = subRslt; }
    }
    return rslt;
}

// Concrete implementations of templated MVKQueue::submit().
template VkResult MVKQueue::submit(uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence, MVKCommandUse cmdUse);
template VkResult MVKQueue::submit(uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence, MVKCommandUse cmdUse);

VkResult MVKQueue::submit(const VkPresentInfoKHR* pPresentInfo) {
	return submit(new MVKQueuePresentSurfaceSubmission(this, pPresentInfo));
}

VkResult MVKQueue::waitIdle(MVKCommandUse cmdUse) {
	if (_execQueue) {
		std::unique_lock lock(_execQueueMutex);
		while (_execQueueJobCount)
			_execQueueConditionVariable.wait(lock);
	}
	@autoreleasepool {
		auto* mtlCmdBuff = getMTLCommandBuffer(cmdUse);
		encodeMetal4OrderingWait(mtlCmdBuff, getLastMetal4SubmissionSequence() + 1);
		[mtlCmdBuff commit];
		[mtlCmdBuff waitUntilCompleted];
	}
	return _device->getConfigurationResult();
}

id<MTLCommandBuffer> MVKQueue::getMTLCommandBuffer(MVKCommandUse cmdUse, bool retainRefs) {
	id<MTLCommandBuffer> mtlCmdBuff = nil;
	uint64_t startTime = getPerformanceTimestamp();

	MTLCommandBufferDescriptor* mtlCmdBuffDesc = [MTLCommandBufferDescriptor new];	// temp retain
	mtlCmdBuffDesc.retainedReferences = retainRefs;
	if (getMVKConfig().debugMode) {
		mtlCmdBuffDesc.errorOptions |= MTLCommandBufferErrorOptionEncoderExecutionStatus;
	}
	mtlCmdBuff = [_mtlQueue commandBufferWithDescriptor: mtlCmdBuffDesc];
	[mtlCmdBuffDesc release];														// temp release

	addPerformanceInterval(getPerformanceStats().queue.retrieveMTLCommandBuffer, startTime);
	NSString* mtlCmdBuffLabel = getMTLCommandBufferLabel(cmdUse);
	setMetalObjectLabel(mtlCmdBuff, mtlCmdBuffLabel);
	[mtlCmdBuff addCompletedHandler: ^(id<MTLCommandBuffer> mtlCB) { handleMTLCommandBufferError(mtlCB); }];

	if ( !mtlCmdBuff ) { reportError(VK_ERROR_OUT_OF_POOL_MEMORY, "%s could not be acquired.", mtlCmdBuffLabel.UTF8String); }
	return mtlCmdBuff;
}

bool MVKQueue::isMetal4CommandSubmissionReady() const {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	auto state = _metal4CommandState;
	return state && state->probeCompleted.load(memory_order_acquire) &&
		state->probeSucceeded.load(memory_order_acquire);
#else
	return false;
#endif
}

uint64_t MVKQueue::reserveMetal4SubmissionSequence() {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	auto state = _metal4CommandState;
	return state ? state->reserveSequence() : 0;
#else
	return 0;
#endif
}

uint64_t MVKQueue::getLastMetal4SubmissionSequence() const {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	auto state = _metal4CommandState;
	return state ? state->lastSequence() : 0;
#else
	return 0;
#endif
}

void MVKQueue::encodeMetal4OrderingWait(id<MTLCommandBuffer> commandBuffer, uint64_t sequence) {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	auto state = _metal4CommandState;
	if (!state || !commandBuffer || sequence <= 1) { return; }
	id<MTLSharedEvent> event = state->copyOrderingEvent();
	[commandBuffer encodeWaitForEvent:event value:sequence - 1];
	[event release];
#else
	(void)commandBuffer;
	(void)sequence;
#endif
}

void MVKQueue::encodeMetal4OrderingSignal(id<MTLCommandBuffer> commandBuffer, uint64_t sequence) {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	auto state = _metal4CommandState;
	if (!state || !commandBuffer || !sequence) { return; }
	id<MTLSharedEvent> event = state->copyOrderingEvent();
	[commandBuffer encodeSignalEvent:event value:sequence];
	[event release];
#else
	(void)commandBuffer;
	(void)sequence;
#endif
}

NSString* MVKQueue::getMTLCommandBufferLabel(MVKCommandUse cmdUse) {
#define CASE_GET_LABEL(cu)  \
	case kMVKCommandUse ##cu:  \
		if ( !_mtlCmdBuffLabel ##cu ) { _mtlCmdBuffLabel ##cu = [[NSString stringWithFormat: @"%s MTLCommandBuffer on Queue %d-%d", mvkVkCommandName(kMVKCommandUse ##cu), _queueFamily->getIndex(), _index] retain]; }  \
		return _mtlCmdBuffLabel ##cu

	switch (cmdUse) {
		CASE_GET_LABEL(BeginCommandBuffer);
		CASE_GET_LABEL(QueueSubmit);
		CASE_GET_LABEL(QueuePresent);
		CASE_GET_LABEL(QueueWaitIdle);
		CASE_GET_LABEL(DeviceWaitIdle);
		CASE_GET_LABEL(AcquireNextImage);
		CASE_GET_LABEL(InvalidateMappedMemoryRanges);
		CASE_GET_LABEL(CopyImageToMemory);
		default:
			MVKAssert(false, "Uncached MTLCommandBuffer label for command use %s.", mvkVkCommandName(cmdUse));
			return [NSString stringWithFormat: @"%s MTLCommandBuffer on Queue %d-%d", mvkVkCommandName(cmdUse), _queueFamily->getIndex(), _index];
	}
#undef CASE_GET_LABEL
}

static const char* mvkStringFromMTLCommandEncoderErrorState(MTLCommandEncoderErrorState errState) {
	switch (errState) {
		case MTLCommandEncoderErrorStateUnknown:   return "unknown";
		case MTLCommandEncoderErrorStateAffected:  return "affected";
		case MTLCommandEncoderErrorStateCompleted: return "completed";
		case MTLCommandEncoderErrorStateFaulted:   return "faulted";
		case MTLCommandEncoderErrorStatePending:   return "pending";
	}
	return "unknown";
}

void MVKQueue::handleMTLCommandBufferError(id<MTLCommandBuffer> mtlCmdBuff) {
	if (mtlCmdBuff.status != MTLCommandBufferStatusError) { return; }

	// If a command buffer error has occurred, report the error. If the error affects
	// the physical device, always mark both the device and physical device as lost.
	// If the error is local to this command buffer, optionally mark the device (but not the
	// physical device) as lost, depending on the value of MVKConfiguration::resumeLostDevice.
	VkResult vkErr = VK_ERROR_UNKNOWN;
	bool markDeviceLoss = !getMVKConfig().resumeLostDevice;
	bool markPhysicalDeviceLoss = false;
	switch (mtlCmdBuff.error.code) {
		case MTLCommandBufferErrorBlacklisted:
		case MTLCommandBufferErrorNotPermitted:	// May also be used for command buffers executed in the background without the right entitlement.
#if MVK_MACOS && !MVK_MACCAT
		case MTLCommandBufferErrorDeviceRemoved:
#endif
			vkErr = VK_ERROR_DEVICE_LOST;
			markDeviceLoss = true;
			markPhysicalDeviceLoss = true;
			break;
		case MTLCommandBufferErrorTimeout:
			vkErr = VK_TIMEOUT;
			break;
		case MTLCommandBufferErrorStackOverflow:
		case MTLCommandBufferErrorPageFault:
		case MTLCommandBufferErrorOutOfMemory:
		default:
			vkErr = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			break;
	}
	if (markDeviceLoss) {
		getDevice()->stopAutoGPUCapture(MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE_DEVICE);
		getDevice()->markLost(markPhysicalDeviceLoss);
	}
	reportResult(vkErr, (markDeviceLoss ? MVK_CONFIG_LOG_LEVEL_ERROR : MVK_CONFIG_LOG_LEVEL_WARNING),
				 "%s VkDevice after MTLCommandBuffer \"%s\" execution failed (code %li): %s",
				 (markDeviceLoss ? "Lost" : "Resumed"),
				 (mtlCmdBuff.label ? mtlCmdBuff.label.UTF8String : ""),
				 mtlCmdBuff.error.code, mtlCmdBuff.error.localizedDescription.UTF8String);

	if (NSArray<id<MTLCommandBufferEncoderInfo>>* mtlEncInfo = mtlCmdBuff.error.userInfo[MTLCommandBufferEncoderInfoErrorKey]) {
		MVKLogInfo("Encoders for %p \"%s\":", mtlCmdBuff, mtlCmdBuff.label ? mtlCmdBuff.label.UTF8String : "");
		for (id<MTLCommandBufferEncoderInfo> enc in mtlEncInfo) {
			MVKLogInfo(" - %s: %s", enc.label.UTF8String, mvkStringFromMTLCommandEncoderErrorState(enc.errorState));
			if (enc.debugSignposts.count > 0) {
				MVKLogInfo("   Debug signposts:");
				for (NSString* signpost in enc.debugSignposts) {
					MVKLogInfo("    - %s", signpost.UTF8String);
				}
			}
		}
	}

	bool isFirstMsg = true;
	for (id<MTLFunctionLog> log in mtlCmdBuff.logs) {
		if (isFirstMsg) {
			MVKLogInfo("Shader log messages:");
			isFirstMsg = false;
		}
		MVKLogInfo("%s", log.description.UTF8String);
	}
}

#pragma mark Construction

MVKQueue::MVKQueue(MVKDevice* device, MVKQueueFamily* queueFamily, uint32_t index, float priority, VkQueueGlobalPriority globalPriority) : MVKDeviceTrackingMixin(device) {
	_queueFamily = queueFamily;
	_index = index;
	_priority = priority;
	_globalPriority = globalPriority;

	initName();
	initExecQueue();
	initMTLCommandQueue();
	initMTL4CommandQueue();
}

void MVKQueue::initName() {
	const char* fmt = "MoltenVKQueue-%d-%d-%.1f";
	char name[256];
	snprintf(name, sizeof(name)/sizeof(char), fmt, _queueFamily->getIndex(), _index, _priority);
	_name = name;
}

void MVKQueue::initExecQueue() {
	_execQueue = nil;
	if ( !getMVKConfig().synchronousQueueSubmits ) {
		// Determine the dispatch queue priority
		dispatch_qos_class_t dqQOS;
		switch (_globalPriority) {
			case VK_QUEUE_GLOBAL_PRIORITY_LOW:
				dqQOS = QOS_CLASS_UTILITY;
				break;
			case VK_QUEUE_GLOBAL_PRIORITY_HIGH:
				dqQOS = QOS_CLASS_USER_INTERACTIVE;
				break;
			case VK_QUEUE_GLOBAL_PRIORITY_MEDIUM:
			default: // Fall back to default (medium)
				dqQOS = QOS_CLASS_USER_INITIATED;
				break;
		}
		int dqPriority = (1.0 - _priority) * QOS_MIN_RELATIVE_PRIORITY;
		dispatch_queue_attr_t dqAttr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, dqQOS, dqPriority);

		// Create the dispatch queue
		_execQueue = dispatch_queue_create((getName() + "-Dispatch").c_str(), dqAttr);		// retained
	}
}

// Retrieves and initializes the Metal command queue and Xcode GPU capture scopes
void MVKQueue::initMTLCommandQueue() {
	_mtlQueue = _queueFamily->getMTLCommandQueue(_index);	// not retained (cached in queue family)
	_device->addResidencySet(_mtlQueue);

	_submissionCaptureScope = new MVKGPUCaptureScope(this);
	if (_queueFamily->getIndex() == getMVKConfig().defaultGPUCaptureScopeQueueFamilyIndex &&
		_index == getMVKConfig().defaultGPUCaptureScopeQueueIndex) {
		getDevice()->startAutoGPUCapture(MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE_FRAME, _mtlQueue);
		_submissionCaptureScope->makeDefault();
	}
	_submissionCaptureScope->beginScope();	// Allow Xcode to capture the first frame if desired.
}

bool MVKQueue::validateMTL4CommandObjects() {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	if (@available(macOS 26.0, iOS 26.0, *)) {
		id<MTLDevice> mtlDevice = getMTLDevice();
		MTL4CommandAllocatorDescriptor* descriptor = [MTL4CommandAllocatorDescriptor new];
		descriptor.label = [NSString stringWithFormat:@"%s Metal 4 object probe allocator", getName().c_str()];
		NSError* error = nil;
		id<MTL4CommandAllocator> allocator =
			[mtlDevice newCommandAllocatorWithDescriptor:descriptor error:&error];
		[descriptor release];
		id<MTL4CommandBuffer> commandBuffer = [mtlDevice newCommandBuffer];
		bool valid = allocator && commandBuffer;
		if (valid) {
			@try {
				commandBuffer.label = [NSString stringWithFormat:@"%s Metal 4 command object probe", getName().c_str()];
				[commandBuffer beginCommandBufferWithAllocator:allocator];
				[commandBuffer endCommandBuffer];
			} @catch (NSException*) {
				valid = false;
			}
		}
		[commandBuffer release];
		[allocator release];
		return valid;
	}
#endif
	return false;
}

bool MVKQueue::startMTL4CommandSubmissionProbe() {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	if (!_mtl4Queue || !_metal4CommandState) { return false; }
	if (@available(macOS 26.0, iOS 26.0, *)) {
		auto state = _metal4CommandState;
		size_t slotIndex = 0;
		id<MTL4CommandAllocator> allocator = nil;
		if (!state->acquireAllocator(&slotIndex, &allocator)) { return false; }

		id<MTL4CommandBuffer> commandBuffer = [getMTLDevice() newCommandBuffer];
		if (!commandBuffer) {
			state->finishEncoding(slotIndex, false);
			state->failProbeBeforeCommit(slotIndex, @"MTL4CommandBuffer creation returned nil");
			[allocator release];
			return false;
		}

		bool commandBufferBeginAttempted = false;
		bool commandBufferBegun = false;
		bool commandBufferEndAttempted = false;
		bool commandBufferEnded = false;
		@try {
			commandBuffer.label = [NSString stringWithFormat:@"%s Metal 4 empty submission probe", getName().c_str()];
			commandBufferBeginAttempted = true;
			[commandBuffer beginCommandBufferWithAllocator:allocator];
			commandBufferBegun = true;
			commandBufferEndAttempted = true;
			[commandBuffer endCommandBuffer];
			commandBufferEnded = true;
		} @catch (NSException* exception) {
			NSException* failure = exception;
			if (commandBufferBegun && !commandBufferEndAttempted) {
				@try {
					commandBufferEndAttempted = true;
					[commandBuffer endCommandBuffer];
					commandBufferEnded = true;
				} @catch (NSException* cleanupException) {
					failure = cleanupException;
				}
			}
			if (commandBufferBeginAttempted && !commandBufferEnded) {
				state->retireAllocator(slotIndex);
				state->markProbeStatus(nil, failure.reason);
			} else {
				state->finishEncoding(slotIndex, false);
				state->failProbeBeforeCommit(slotIndex, failure.reason);
			}
			[commandBuffer release];
			[allocator release];
			return false;
		}

		state->finishEncoding(slotIndex, true);
		state->probeSubmitted.store(true, memory_order_release);

		MTL4CommitOptions* options = [MTL4CommitOptions new];
		if (!options) {
			state->failProbeBeforeCommit(slotIndex, @"Could not create MTL4CommitOptions");
			[commandBuffer release];
			[allocator release];
			return false;
		}

		[options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
			state->completeProbe(slotIndex, feedback.error);
		}];

		id<MTL4CommandBuffer> commandBuffers[] = { commandBuffer };
		@try {
			state->probeMayBeInFlight.store(true, memory_order_release);
			[_mtl4Queue commit:commandBuffers count:1 options:options];
		} @catch (NSException* exception) {
			state->failProbeInFlight(exception.reason);
			[options release];
			[commandBuffer release];
			[allocator release];
			return false;
		}

		double timeoutMs = mvkGetEnvVarNumber(
			"MVK_CONFIG_METAL4_COMMAND_VALIDATION_TIMEOUT_MS",
			kMetal4CommandValidationDefaultTimeoutMs);
		if (!isfinite(timeoutMs)) { timeoutMs = kMetal4CommandValidationDefaultTimeoutMs; }
		timeoutMs = max(kMetal4CommandValidationMinimumTimeoutMs,
						min(kMetal4CommandValidationMaximumTimeoutMs, timeoutMs));
		bool succeeded = state->waitForProbe((uint64_t)(timeoutMs * 1000000.0));

		[options release];
		[commandBuffer release];
		[allocator release];
		return succeeded;
	}
#endif
	return false;
}

// Creates the independent Metal 4 queue, command-backend residency set, allocator arena,
// total-order event, and bounded empty commit validation. Real Vulkan copy/fill submissions
// are selected only after this function returns successfully.
void MVKQueue::initMTL4CommandQueue() {
	_metal4CommandBackendRequested = mvkGetEnvVarNumber("MVK_CONFIG_METAL4_COMMAND_BACKEND", 0.0) != 0.0;
	if (!_metal4CommandBackendRequested) { return; }

#if MVK_USE_METAL_PRIVATE_API
	_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
						  "Metal 4 command backend disabled in Metal private-API builds.");
	return;
#endif

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	if (getMVKConfig().useMetalPrivateAPI) {
		_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
						  "Metal 4 command backend disabled because Metal private APIs are active.");
		return;
	}
	bool supportsMetal4Family = getPhysicalDevice()->getMTLDeviceCapabilities().supportsMetal4;
	bool validationOverride =
		mvkGetEnvVarNumber("MVK_CONFIG_METAL4_COMMAND_VALIDATION", 0.0) != 0.0;
	if ((!supportsMetal4Family && !validationOverride) || !mvkOSVersionIsAtLeast(26.0)) {
		_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
						  "Metal 4 command backend requested but unavailable on this OS or GPU.");
		return;
	}
	if (!supportsMetal4Family) {
		_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
						  "Metal 4 command validation override bypassed only the GPU-family advertisement; public factories and the bounded commit probe must still pass.");
	}
	if (@available(macOS 26.0, iOS 26.0, *)) {
		id<MTLDevice> mtlDevice = getMTLDevice();
		if (![mtlDevice respondsToSelector:@selector(newMTL4CommandQueueWithDescriptor:error:)] ||
			![mtlDevice respondsToSelector:@selector(newCommandAllocatorWithDescriptor:error:)] ||
			![mtlDevice respondsToSelector:@selector(newCommandBuffer)] ||
			![mtlDevice respondsToSelector:@selector(newResidencySetWithDescriptor:error:)]) {
			_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
							  "Metal 4 command backend requested but required command or residency factories are unavailable.");
			return;
		}

		NSString* queueLabel = [NSString stringWithFormat:@"%s MTL4CommandQueue", getName().c_str()];
		MTL4CommandQueueDescriptor* queueDescriptor = [MTL4CommandQueueDescriptor new];
		queueDescriptor.label = queueLabel;
		NSError* error = nil;
		_mtl4Queue = [mtlDevice newMTL4CommandQueueWithDescriptor:queueDescriptor error:&error];
		[queueDescriptor release];
		if (!_mtl4Queue) {
			_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
							  "Could not create the Metal 4 command queue for Vulkan queue %u-%u: %s",
							  _queueFamily->getIndex(), _index,
							  error.localizedDescription.UTF8String ?: "unknown error");
			return;
		}

		if (!validateMTL4CommandObjects()) {
			_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
							  "Metal 4 command object validation failed for Vulkan queue %u-%u; retaining the legacy backend.",
							  _queueFamily->getIndex(), _index);
			[_mtl4Queue release];
			_mtl4Queue = nil;
			return;
		}

		double configuredAllocatorCount = mvkGetEnvVarNumber(
			"MVK_CONFIG_METAL4_COMMAND_ALLOCATOR_COUNT",
			(double)kMetal4CommandAllocatorDefaultCount);
		uint32_t allocatorCount = kMetal4CommandAllocatorDefaultCount;
		if (isfinite(configuredAllocatorCount) && configuredAllocatorCount >= 1.0 &&
			configuredAllocatorCount <= (double)kMetal4CommandAllocatorMaxCount) {
			allocatorCount = (uint32_t)configuredAllocatorCount;
		}

		_metal4CommandState = make_shared<MVKMetal4CommandQueueState>();
		string failureReason;
		if (!_metal4CommandState->initialize(mtlDevice, allocatorCount, queueLabel, &failureReason)) {
			_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
							  "Could not create the Metal 4 queue state for Vulkan queue %u-%u; retaining the legacy backend: %s",
							  _queueFamily->getIndex(), _index, failureReason.c_str());
			_metal4CommandState.reset();
			[_mtl4Queue release];
			_mtl4Queue = nil;
			return;
		}

		id<MTLResidencySet> residencySet = _metal4CommandState->copyResidencySet();
		[_mtl4Queue addResidencySet:residencySet];
		[residencySet release];

		if (!startMTL4CommandSubmissionProbe()) {
			string reason;
			{
				lock_guard<mutex> guard(_metal4CommandState->lock);
				reason = _metal4CommandState->lastError;
			}
			_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
							  "Metal 4 command queue validation failed for Vulkan queue %u-%u; retaining the legacy backend: %s",
							  _queueFamily->getIndex(), _index, reason.c_str());
			if (_metal4CommandState->probeMayBeInFlight.load(memory_order_acquire)) {
				_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
							  "Metal 4 validation commit may still be in flight; retaining its Metal 4 sidecar until feedback or queue teardown.");
				return;
			}
			id<MTLResidencySet> failedResidencySet = _metal4CommandState->copyResidencySet();
			[_mtl4Queue removeResidencySet:failedResidencySet];
			[failedResidencySet release];
			_metal4CommandState->shutdown();
			[_mtl4Queue release];
			_mtl4Queue = nil;
			_metal4CommandState.reset();
			return;
		}

		_metal4CommandBackendReady = true;
		_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
						  "Metal 4 Vulkan transfer backend ready for queue %u-%u with %u allocators; unsupported submissions fall back before encoding.",
						  _queueFamily->getIndex(), _index, allocatorCount);
		return;
	}
#endif

	_device->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
					  "Metal 4 command backend requested but excluded from this MoltenVK target.");
}

MVKQueue::~MVKQueue() {
	destroyExecQueue();
	_submissionCaptureScope->destroy();
	_device->removeResidencySet(_mtlQueue);
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	if (_metal4CommandState) {
		string unsupportedCommandSummary = _metal4CommandState->unsupportedCommandSummary();
		_device->reportMessage(
			MVK_CONFIG_LOG_LEVEL_INFO,
			"Metal 4 command backend summary: attempts=%llu, real_submissions=%llu, render_submissions=%llu, fallbacks=%llu, failures=%llu, buffer_copies=%llu, buffer_fills=%llu, buffer_updates=%llu, image_copies=%llu, compute_dispatches=%llu, render_passes=%llu, draws=%llu, barriers=%llu, query_resets=%llu, query_copies=%llu, visibility_queries=%llu, unsupported_commands=%s.",
			(unsigned long long)_metal4CommandState->attemptedSubmissionCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->realSubmissionCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->renderSubmissionCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->fallbackCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->failureCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->bufferCopyCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->bufferFillCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->bufferUpdateCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->imageCopyCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->computeDispatchCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->renderPassCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->drawCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->barrierCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->queryResetCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->queryCopyCount.load(memory_order_relaxed),
			(unsigned long long)_metal4CommandState->visibilityQueryCount.load(memory_order_relaxed),
			unsupportedCommandSummary.c_str());
		_metal4CommandState->shutdown();
		id<MTLResidencySet> residencySet = _metal4CommandState->copyResidencySet();
		if (_mtl4Queue && residencySet) { [_mtl4Queue removeResidencySet:residencySet]; }
		[residencySet release];
	}
	[_mtl4Queue release];
	_metal4CommandState.reset();
#endif

	[_mtlCmdBuffLabelBeginCommandBuffer release];
	[_mtlCmdBuffLabelQueueSubmit release];
	[_mtlCmdBuffLabelQueuePresent release];
	[_mtlCmdBuffLabelDeviceWaitIdle release];
	[_mtlCmdBuffLabelQueueWaitIdle release];
	[_mtlCmdBuffLabelAcquireNextImage release];
	[_mtlCmdBuffLabelInvalidateMappedMemoryRanges release];
}

// Destroys the execution dispatch queue.
void MVKQueue::destroyExecQueue() {
	if (_execQueue) {
		dispatch_release(_execQueue);
		_execQueue = nullptr;
	}
}


#pragma mark -
#pragma mark MVKQueueSubmission

void MVKSemaphoreSubmitInfo::encodeWait(id<MTLCommandBuffer> mtlCmdBuff) {
	if (_semaphore) { _semaphore->encodeWait(mtlCmdBuff, value); }
}

void MVKSemaphoreSubmitInfo::encodeSignal(id<MTLCommandBuffer> mtlCmdBuff) {
	if (_semaphore) { _semaphore->encodeSignal(mtlCmdBuff, value); }
}

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
bool MVKSemaphoreSubmitInfo::supportsMetal4QueueEncoding() const {
	return !_semaphore || _semaphore->supportsMetal4QueueEncoding();
}

void MVKSemaphoreSubmitInfo::encodeMetal4Wait(id<MTL4CommandQueue> queue) {
	if (_semaphore) { _semaphore->encodeMetal4Wait(queue, value); }
}

void MVKSemaphoreSubmitInfo::encodeMetal4Signal(id<MTL4CommandQueue> queue) {
	if (_semaphore) { _semaphore->encodeMetal4Signal(queue, value); }
}
#endif

MVKSemaphoreSubmitInfo::MVKSemaphoreSubmitInfo(const VkSemaphoreSubmitInfo& semaphoreSubmitInfo) :
	_semaphore((MVKSemaphore*)semaphoreSubmitInfo.semaphore),
	value(semaphoreSubmitInfo.value),
	stageMask(semaphoreSubmitInfo.stageMask),
	deviceIndex(semaphoreSubmitInfo.deviceIndex) {
		if (_semaphore) { _semaphore->retain(); }
}

MVKSemaphoreSubmitInfo::MVKSemaphoreSubmitInfo(const VkSemaphore semaphore,
											   VkPipelineStageFlags stageMask) :
	_semaphore((MVKSemaphore*)semaphore),
	value(0),
	stageMask(stageMask),
	deviceIndex(0) {
		if (_semaphore) { _semaphore->retain(); }
}

MVKSemaphoreSubmitInfo::MVKSemaphoreSubmitInfo(const MVKSemaphoreSubmitInfo& other) :
	_semaphore(other._semaphore),
	value(other.value),
	stageMask(other.stageMask),
	deviceIndex(other.deviceIndex) {
		if (_semaphore) { _semaphore->retain(); }
}

MVKSemaphoreSubmitInfo& MVKSemaphoreSubmitInfo::operator=(const MVKSemaphoreSubmitInfo& other) {
	// Retain new object first in case it's the same object
	if (other._semaphore) {other._semaphore->retain(); }
	if (_semaphore) { _semaphore->release(); }
	_semaphore = other._semaphore;

	value = other.value;
	stageMask = other.stageMask;
	deviceIndex = other.deviceIndex;
	return *this;
}

MVKSemaphoreSubmitInfo::~MVKSemaphoreSubmitInfo() {
	if (_semaphore) { _semaphore->release(); }
}

MVKCommandBufferSubmitInfo::MVKCommandBufferSubmitInfo(const VkCommandBufferSubmitInfo& commandBufferInfo) :
	commandBuffer(MVKCommandBuffer::getMVKCommandBuffer(commandBufferInfo.commandBuffer)),
	deviceMask(commandBufferInfo.deviceMask) {}

MVKCommandBufferSubmitInfo::MVKCommandBufferSubmitInfo(VkCommandBuffer commandBuffer) :
	commandBuffer(MVKCommandBuffer::getMVKCommandBuffer(commandBuffer)),
	deviceMask(0) {}

MVKQueueSubmission::MVKQueueSubmission(MVKQueue* queue,
									   uint32_t waitSemaphoreInfoCount,
									   const VkSemaphoreSubmitInfo* pWaitSemaphoreSubmitInfos) : 
	MVKBaseDeviceObject(queue->getDevice()),
	_queue(queue) {

	_queue->retain();	// Retain here and release in destructor. See note for MVKQueueCommandBufferSubmission::finish().
	_creationTime = getPerformanceTimestamp();

	_waitSemaphores.reserve(waitSemaphoreInfoCount);
	for (uint32_t i = 0; i < waitSemaphoreInfoCount; i++) {
		_waitSemaphores.emplace_back(pWaitSemaphoreSubmitInfos[i]);
	}
}

MVKQueueSubmission::MVKQueueSubmission(MVKQueue* queue,
									   uint32_t waitSemaphoreCount,
									   const VkSemaphore* pWaitSemaphores,
									   const VkPipelineStageFlags* pWaitDstStageMask) :
	MVKBaseDeviceObject(queue->getDevice()),
	_queue(queue) {

	_queue->retain();	// Retain here and release in destructor. See note for MVKQueueCommandBufferSubmission::finish().
	_creationTime = getPerformanceTimestamp();

	_waitSemaphores.reserve(waitSemaphoreCount);
	for (uint32_t i = 0; i < waitSemaphoreCount; i++) {
		_waitSemaphores.emplace_back(pWaitSemaphores[i], pWaitDstStageMask ? pWaitDstStageMask[i] : 0);
	}
}

MVKQueueSubmission::~MVKQueueSubmission() {
	_queue->release();
}


#pragma mark -
#pragma mark MVKQueueCommandBufferSubmission

VkResult MVKQueueCommandBufferSubmission::execute() {

	_queue->_submissionCaptureScope->beginScope();
	_submissionSequence = _queue->reserveMetal4SubmissionSequence();

	bool handledByMetal4 = false;
	VkResult metal4Result = executeMetal4(&handledByMetal4);
	if (handledByMetal4) { return metal4Result; }

	// If using encoded semaphore waiting, do so now.
	for (auto& ws : _waitSemaphores) { ws.encodeWait(getActiveMTLCommandBuffer()); }

	// Wait time from an async vkQueueSubmit() call to starting submit and encoding of the command buffers
	addPerformanceInterval(_queue->getPerformanceStats().queue.waitSubmitCommandBuffers, _creationTime);

	// Submit each command buffer.
	submitCommandBuffers();

	// If using encoded semaphore signaling, do so now.
	for (auto& ss : _signalSemaphores) { ss.encodeSignal(getActiveMTLCommandBuffer()); }

	// Commit the last MTLCommandBuffer.
	// Nothing after this because callback might destroy this instance before this function ends.
	return commitActiveMTLCommandBuffer(true);
}

bool MVKQueueCommandBufferSubmission::supportsMetal4Semaphores() const {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	for (const auto& wait : _waitSemaphores) {
		if (!wait.supportsMetal4QueueEncoding()) { return false; }
	}
	for (const auto& signal : _signalSemaphores) {
		if (!signal.supportsMetal4QueueEncoding()) { return false; }
	}
	return true;
#else
	return false;
#endif
}

VkResult MVKQueueCommandBufferSubmission::executeMetal4(bool* handled) {
	if (handled) { *handled = false; }
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	if (!handled ||
		_commandUse != kMVKCommandUseQueueSubmit ||
		!_queue->isMetal4CommandBackendReady() ||
		!_queue->isMetal4CommandSubmissionReady()) {
		return VK_SUCCESS;
	}

	auto state = _queue->_metal4CommandState;
	id<MTL4CommandQueue> commandQueue = _queue->_mtl4Queue;
	if (!state || !commandQueue) {
		_queue->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
						  "Metal 4 command backend became unavailable after initialization; retaining the legacy backend.");
		return VK_SUCCESS;
	}

	state->recordSubmissionAttempt();
	auto recordFallback = [&](MVKMetal4FallbackReason reason,
								 const char* unsupportedCommand = nullptr) {
		MVKMetal4FallbackTelemetry fallback = state->recordFallback(reason, unsupportedCommand);
		bool isPowerOfTwo =
			fallback.totalCount != 0 &&
			(fallback.totalCount & (fallback.totalCount - 1)) == 0;
		bool isFirstUnsupportedCommand = fallback.unsupportedCommandCount == 1;
		if (fallback.reasonCount == 1 || isPowerOfTwo || isFirstUnsupportedCommand) {
			string unsupportedCommands = state->unsupportedCommandSummary();
			_queue->reportMessage(
				MVK_CONFIG_LOG_LEVEL_INFO,
				"Metal 4 command backend live: attempts=%llu, real_submissions=%llu, fallbacks=%llu, failures=%llu, latest_fallback=%s, latest_reason_count=%llu, latest_unsupported_command=%s, latest_unsupported_command_count=%llu, unsupported_commands=%s.",
				(unsigned long long)state->attemptedSubmissionCount.load(memory_order_relaxed),
				(unsigned long long)state->realSubmissionCount.load(memory_order_relaxed),
				(unsigned long long)fallback.totalCount,
				(unsigned long long)state->failureCount.load(memory_order_relaxed),
				mvkMetal4FallbackReasonName(reason),
				(unsigned long long)fallback.reasonCount,
				fallback.unsupportedCommand,
				(unsigned long long)fallback.unsupportedCommandCount,
				unsupportedCommands.c_str());
		}
	};

	if (!supportsMetal4Semaphores()) {
		recordFallback(MVKMetal4FallbackReason::UnsupportedSemaphore);
		return VK_SUCCESS;
	}
	const char* firstUnsupportedCommand = nullptr;
	if (!supportsMetal4CommandBuffers(&firstUnsupportedCommand)) {
		recordFallback(MVKMetal4FallbackReason::UnsupportedCommandBuffer,
					   firstUnsupportedCommand);
		return VK_SUCCESS;
	}

	MVKMetal4TransferCommandEncoder encoder(
		state, getDevice(), getMTLDevice(), getPixelFormats());
	if (!prepareMetal4CommandBuffers(&encoder)) {
		recordFallback(MVKMetal4FallbackReason::PrepareFailed);
		return VK_SUCCESS;
	}

	const auto& allocations = encoder.getAllocations();
	if (!state->acquireResidency(allocations)) {
		recordFallback(MVKMetal4FallbackReason::ResidencyAcquireFailed);
		return VK_SUCCESS;
	}

	size_t allocatorIndex = 0;
	id<MTL4CommandAllocator> allocator = nil;
	if (!state->acquireAllocator(&allocatorIndex, &allocator)) {
		state->releaseResidency(allocations);
		recordFallback(MVKMetal4FallbackReason::AllocatorUnavailable);
		return VK_SUCCESS;
	}

	id<MTL4CommandBuffer> commandBuffer = [getMTLDevice() newCommandBuffer];
	id<MTLResidencySet> residencySet = state->copyResidencySet();
	MTL4CommitOptions* options = [MTL4CommitOptions new];
	if (!commandBuffer || !residencySet || !options) {
		state->finishEncoding(allocatorIndex, false);
		state->releaseResidency(allocations);
		[options release];
		[residencySet release];
		[commandBuffer release];
		[allocator release];
		recordFallback(MVKMetal4FallbackReason::CommandObjectUnavailable);
		return VK_SUCCESS;
	}

	bool claimedCommandBuffers = false;
	bool commandBufferBeginAttempted = false;
	bool commandBufferBegun = false;
	bool encoderEndAttempted = false;
	bool commandBufferEndAttempted = false;
	bool commandBufferEnded = false;
	const char* encodingPhase = "label_command_buffer";
	@try {
		commandBuffer.label = [NSString stringWithFormat:@"%s Metal 4 Vulkan transfer submission %llu",
												 _queue->getName().c_str(),
												 (unsigned long long)_submissionSequence];
		encodingPhase = "begin_command_buffer";
		commandBufferBeginAttempted = true;
		[commandBuffer beginCommandBufferWithAllocator:allocator];
		commandBufferBegun = true;
		encodingPhase = "begin_encoder";
		if (!encoder.beginEncoding(commandBuffer, residencySet)) {
			@throw [NSException exceptionWithName:@"MVKMetal4EncoderCreation"
									 reason:@"Could not create MTL4ComputeCommandEncoder"
								   userInfo:nil];
		}
		encodingPhase = "claim_vulkan_command_buffers";
		if (!beginMetal4CommandBuffers()) {
			@throw [NSException exceptionWithName:@"MVKMetal4CommandClaim"
									 reason:@"Could not claim every Vulkan command buffer"
								   userInfo:nil];
		}
		claimedCommandBuffers = true;
		encodingPhase = "encode_vulkan_commands";
		if (!encodeMetal4CommandBuffers(&encoder)) {
			const char* failedCommand = encoder.getMetal4EncodingFailureCommand();
			@throw [NSException exceptionWithName:@"MVKMetal4CommandEncoding"
									 reason:[NSString stringWithFormat:
										 @"Metal 4 command materialization failed for %s",
										 failedCommand]
								   userInfo:nil];
		}
		encodingPhase = "end_encoder";
		encoderEndAttempted = true;
		encoder.endEncoding();
		encodingPhase = "end_command_buffer";
		commandBufferEndAttempted = true;
		[commandBuffer endCommandBuffer];
		commandBufferEnded = true;
	} @catch (NSException* exception) {
		bool closedForReplay = !commandBufferBeginAttempted;
		NSException* cleanupException = nil;
		if (commandBufferBegun && !encoderEndAttempted && !commandBufferEndAttempted) {
			@try {
				encoderEndAttempted = true;
				encoder.endEncoding();
				commandBufferEndAttempted = true;
				[commandBuffer endCommandBuffer];
				commandBufferEnded = true;
				closedForReplay = true;
			} @catch (NSException* caughtCleanupException) {
				cleanupException = caughtCleanupException;
			}
		}
		if (claimedCommandBuffers) { endMetal4CommandBuffers(false); }
		if (!closedForReplay) {
			encoder.abandonEncoding();
			state->retireAllocator(allocatorIndex);
			state->releaseResidency(allocations);
			NSException* failure = cleanupException ?: exception;
			state->recordFailure(failure.reason);
			setConfigurationResult(VK_ERROR_DEVICE_LOST);
			_queue->reportResult(VK_ERROR_DEVICE_LOST,
							 MVK_CONFIG_LOG_LEVEL_ERROR,
							 "Metal 4 command encoding could not be safely closed: %s",
							 failure.reason.UTF8String ?: "unknown exception");
			_queue->getDevice()->markLost(false);
			[options release];
			[residencySet release];
			[commandBuffer release];
			[allocator release];
			*handled = true;
			finish();
			return VK_ERROR_DEVICE_LOST;
		}
		state->finishEncoding(allocatorIndex, false);
		state->releaseResidency(allocations);
		[options release];
		[residencySet release];
		[commandBuffer release];
		[allocator release];
		string exceptionSummary;
		if (state->recordReplayableException(encodingPhase, exception, &exceptionSummary)) {
			_queue->reportMessage(
				MVK_CONFIG_LOG_LEVEL_INFO,
				"Metal 4 replayable encoding exception: %s.",
				exceptionSummary.c_str());
		}
		recordFallback(MVKMetal4FallbackReason::EncodingReplayableException);
		return VK_SUCCESS;
	}

	if (!commandBufferEnded) {
		if (claimedCommandBuffers) { endMetal4CommandBuffers(false); }
		state->finishEncoding(allocatorIndex, false);
		state->releaseResidency(allocations);
		[options release];
		[residencySet release];
		[commandBuffer release];
		[allocator release];
		recordFallback(MVKMetal4FallbackReason::CommandBufferNotEnded);
		return VK_SUCCESS;
	}

	uint64_t startTime = getPerformanceTimestamp();
	auto completion = make_shared<MVKMetal4SubmissionCompletion>(
		this,
		_queue,
		state,
		allocations,
		encoder.getCompletedQueries(),
		allocatorIndex,
		_submissionSequence,
		startTime);

	[options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
		completion->receiveFeedback(feedback);
	}];

	id<MTLSharedEvent> orderingEvent = state->copyOrderingEvent();
	bool allocatorInFlight = false;
	bool commitAttempted = false;
	bool queueSideEffectsStarted = false;
	VkResult result = getConfigurationResult();
	@try {
		if (_submissionSequence > 1) {
			queueSideEffectsStarted = true;
			[commandQueue waitForEvent:orderingEvent value:_submissionSequence - 1];
		}
		for (auto& wait : _waitSemaphores) {
			queueSideEffectsStarted = true;
			wait.encodeMetal4Wait(commandQueue);
		}

		id<MTL4CommandBuffer> commandBuffers[] = { commandBuffer };
		state->finishEncoding(allocatorIndex, true);
		allocatorInFlight = true;
		commitAttempted = true;
		[commandQueue commit:commandBuffers count:1 options:options];
		endMetal4CommandBuffers(true);
		claimedCommandBuffers = false;
		encoder.publishCommittedState();
		encoder.publishCommittedCounters();
		state->recordRealSubmission();
		if (encoder.hasRenderWork()) { state->recordRenderSubmission(); }

		for (auto& signal : _signalSemaphores) { signal.encodeMetal4Signal(commandQueue); }
		[commandQueue signalEvent:orderingEvent value:_submissionSequence];

		if (state->realSubmissionCount.load(memory_order_relaxed) == 1) {
			_queue->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
							  "Executed first Vulkan submission on the Metal 4 transfer backend (queue %u-%u, sequence %llu, attempts=%llu).",
							  _queue->_queueFamily->getIndex(), _queue->_index,
							  (unsigned long long)_submissionSequence,
							  (unsigned long long)state->attemptedSubmissionCount.load(memory_order_relaxed));
		}
	} @catch (NSException* exception) {
		if (!queueSideEffectsStarted && !commitAttempted) {
			if (claimedCommandBuffers) {
				endMetal4CommandBuffers(false);
				claimedCommandBuffers = false;
			}
			if (allocatorInFlight) {
				state->completeAllocator(allocatorIndex);
			} else {
				state->finishEncoding(allocatorIndex, false);
			}
			state->releaseResidency(allocations);
			recordFallback(MVKMetal4FallbackReason::PrecommitReplayableException);
			[orderingEvent release];
			[options release];
			[residencySet release];
			[commandBuffer release];
			[allocator release];
			*handled = false;
			return VK_SUCCESS;
		}

		if (!commitAttempted) {
			if (claimedCommandBuffers) {
				endMetal4CommandBuffers(false);
				claimedCommandBuffers = false;
			}
			if (allocatorInFlight) {
				state->completeAllocator(allocatorIndex);
			} else {
				state->finishEncoding(allocatorIndex, false);
			}
			state->releaseResidency(allocations);
			state->recordFailure(exception.reason);
			setConfigurationResult(VK_ERROR_DEVICE_LOST);
			_queue->reportResult(VK_ERROR_DEVICE_LOST,
							 MVK_CONFIG_LOG_LEVEL_ERROR,
							 "Metal 4 queue wait produced a non-replayable side effect: %s",
							 exception.reason.UTF8String ?: "unknown exception");
			_queue->getDevice()->markLost(false);
			[orderingEvent release];
			[options release];
			[residencySet release];
			[commandBuffer release];
			[allocator release];
			*handled = true;
			finish();
			return VK_ERROR_DEVICE_LOST;
		}

		if (claimedCommandBuffers) {
			endMetal4CommandBuffers(true);
			claimedCommandBuffers = false;
		}
		state->recordFailure(exception.reason);
		setConfigurationResult(VK_ERROR_DEVICE_LOST);
		result = VK_ERROR_DEVICE_LOST;
		_queue->reportResult(VK_ERROR_DEVICE_LOST,
							 MVK_CONFIG_LOG_LEVEL_ERROR,
							 "Metal 4 Vulkan queue commit became ambiguous: %s",
							 exception.reason.UTF8String ?: "unknown exception");
		_queue->getDevice()->markLost(false);
		// Do not host-signal ordering or release allocator/residency here. The
		// command queue may have accepted the workload before throwing. Real
		// commit feedback is the only safe release boundary.
	}

	[orderingEvent release];
	[options release];
	[residencySet release];
	[commandBuffer release];
	[allocator release];

	*handled = true;
	completion->markSchedulingComplete();
	return result;
#else
	return VK_SUCCESS;
#endif
}

// Returns the active MTLCommandBuffer, lazily retrieving it from the queue if needed.
id<MTLCommandBuffer> MVKQueueCommandBufferSubmission::getActiveMTLCommandBuffer() {
	if ( !_activeMTLCommandBuffer ) {
		bool needsRetain = false;
		if (!_device->hasResidencySet() && (getEnabledDescriptorIndexingFeatures().descriptorBindingPartiallyBound || getMVKConfig().liveCheckAllResources)) {
			// Partially bound descriptors will get bound by us even if they're not used at runtime by the shader.
			// The application is free to destroy them even if they're not used at runtime even if we bound them.
			// Metal will be very unhappy if we destroy something we bound, even if it isn't used at runtime.
			needsRetain = true;
		}
		setActiveMTLCommandBuffer(_queue->getMTLCommandBuffer(_commandUse, needsRetain));
	}
	return _activeMTLCommandBuffer;
}

// Commits the current active MTLCommandBuffer, if it exists, and sets a new active MTLCommandBuffer.
void MVKQueueCommandBufferSubmission::setActiveMTLCommandBuffer(id<MTLCommandBuffer> mtlCmdBuff, bool isPrefilled) {

	if (_activeMTLCommandBuffer) { commitActiveMTLCommandBuffer(); }

	// A prefilled command buffer already contains encoders, so a wait inserted now
	// would be too late. Commit an empty bridge on the legacy queue first; implicit
	// queue order then places all prefilled work after the hybrid-backend wait.
	if (isPrefilled && _submissionSequence && !_legacyOrderingWaitEncoded) {
		id<MTLCommandBuffer> bridge = _queue->getMTLCommandBuffer(_commandUse);
		_queue->encodeMetal4OrderingWait(bridge, _submissionSequence);
		[bridge commit];
		_legacyOrderingWaitEncoded = true;
	}

	_activeMTLCommandBuffer = [mtlCmdBuff retain];		// retained to handle prefilled
	[_activeMTLCommandBuffer enqueue];
	if (!isPrefilled && _submissionSequence && !_legacyOrderingWaitEncoded) {
		_queue->encodeMetal4OrderingWait(_activeMTLCommandBuffer, _submissionSequence);
		_legacyOrderingWaitEncoded = true;
	}
}

// Commits and releases the currently active MTLCommandBuffer, optionally signalling
// when the MTLCommandBuffer is done. The first time this is called, it will wait on
// any semaphores. We have delayed signalling the semaphores as long as possible to
// allow as much filling of the MTLCommandBuffer as possible before forcing a wait.
VkResult MVKQueueCommandBufferSubmission::commitActiveMTLCommandBuffer(bool signalCompletion) {

	// If using inline semaphore waiting, do so now.
	// When prefilled command buffers are used, multiple commits will happen because native semaphore
	// waits need to be committed before the prefilled command buffer is committed. Since semaphores
	// will reset their internal signal flag on wait, we need to make sure that we only wait once, otherwise we will freeze.
	// Another option to wait on emulated semaphores once is to do it in the execute function, but doing it here
	// should be more performant when prefilled command buffers aren't used, because we spend time encoding commands
	// first, thus giving the command buffer signalling these semaphores more time to complete.
	if ( !_emulatedWaitDone ) {
		for (auto& ws : _waitSemaphores) { ws.encodeWait(nil); }
		_emulatedWaitDone = true;
	}

	// The visibility result buffer will be returned to its pool when the active MTLCommandBuffer
	// finishes executing, and therefore cannot be used beyond the active MTLCommandBuffer.
	// By now, it's been submitted to the MTLCommandBuffer, so remove it from the encoding context,
	// to ensure a fresh one will be used by commands executing on any subsequent MTLCommandBuffers.
	if (_encodingContext.visibilityResultBuffer.buffer())
		_device->returnVisibilityBuffer(std::move(_encodingContext.visibilityResultBuffer));

	// If this is the last command buffer in the submission, we're losing the context and need synchronize
	// current barrier fences to the ones at index 0, which will be what the next submision starts with.
	if (isUsingMetalArgumentBuffers() && signalCompletion) {
		_encodingContext.syncFences(getDevice(), _activeMTLCommandBuffer);
	}

	// If we need to signal completion, use getActiveMTLCommandBuffer() to ensure at least
	// one MTLCommandBuffer is used, otherwise if this instance has no content, it will not
	// finish(), signal the fence and semaphores, and be destroyed.
	// Use temp var for MTLCommandBuffer commit and release because completion callback
	// may destroy this instance before this function ends.
	id<MTLCommandBuffer> mtlCmdBuff = signalCompletion ? getActiveMTLCommandBuffer() : _activeMTLCommandBuffer;
	_activeMTLCommandBuffer = nil;

	if (signalCompletion) {
		_queue->encodeMetal4OrderingSignal(mtlCmdBuff, _submissionSequence);
	}

	uint64_t startTime = getPerformanceTimestamp();
	[mtlCmdBuff addCompletedHandler: ^(id<MTLCommandBuffer> mtlCB) {
		addPerformanceInterval(getPerformanceStats().queue.mtlCommandBufferExecution, startTime);
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
		if (mtlCB.status == MTLCommandBufferStatusError && _queue->_metal4CommandState) {
			_queue->_metal4CommandState->hostSignalOrdering(_submissionSequence);
		}
#endif
		if (signalCompletion) { this->finish(); }	// Must be the last thing the completetion callback does.
	}];

	// Retrieve the result before committing MTLCommandBuffer, because finish() will destroy this instance.
	VkResult rslt = mtlCmdBuff ? getConfigurationResult() : VK_ERROR_OUT_OF_POOL_MEMORY;
	[mtlCmdBuff commit];
	[mtlCmdBuff release];		// retained

	// If we need to signal completion, but an error occurred and the MTLCommandBuffer
	// was not created, call the finish() function directly.
	if (signalCompletion && !mtlCmdBuff) {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
		if (_queue->_metal4CommandState) {
			_queue->_metal4CommandState->hostSignalOrdering(_submissionSequence);
		}
#endif
		finish();
	}

	return rslt;
}

// Be sure to retain() any API objects referenced in this function, and release() them in the
// destructor (or superclass destructor). It is possible for rare race conditions to result
// in the app destroying API objects before this function completes execution. For example,
// this may occur if a GPU semaphore here triggers another submission that triggers a fence,
// and the app immediately destroys objects. Rare, but it has been encountered.
void MVKQueueCommandBufferSubmission::finish() {

	// Performed here instead of as part of execute() for rare case where app destroys queue
	// immediately after a waitIdle() is cleared by fence below, taking the capture scope with it.
	_queue->_submissionCaptureScope->endScope();

	// If using inline semaphore signaling, do so now.
	for (auto& ss : _signalSemaphores) { ss.encodeSignal(nil); }

	// If a fence exists, signal it.
	if (_fence) { _fence->signal(); }

	this->destroy();
}

// On device loss, the fence and signal semaphores may be signalled early, and they might then
// be destroyed on the waiting thread before this submission is done with them. We therefore
// retain() each here to ensure they live long enough for this submission to finish using them.
MVKQueueCommandBufferSubmission::MVKQueueCommandBufferSubmission(MVKQueue* queue,
																 const VkSubmitInfo2* pSubmit,
																 VkFence fence,
																 MVKCommandUse cmdUse) :
	MVKQueueSubmission(queue,
					   pSubmit ? pSubmit->waitSemaphoreInfoCount : 0,
					   pSubmit ? pSubmit->pWaitSemaphoreInfos : nullptr),
	_fence((MVKFence*)fence),
	_commandUse(cmdUse) {
	
	if (_fence) { _fence->retain(); }

	// pSubmit can be null if just tracking the fence alone
	if (pSubmit) {
		uint32_t ssCnt = pSubmit->signalSemaphoreInfoCount;
		_signalSemaphores.reserve(ssCnt);
		for (uint32_t i = 0; i < ssCnt; i++) {
			_signalSemaphores.emplace_back(pSubmit->pSignalSemaphoreInfos[i]);
		}
	}
}

// On device loss, the fence and signal semaphores may be signalled early, and they might then
// be destroyed on the waiting thread before this submission is done with them. We therefore
// retain() each here to ensure they live long enough for this submission to finish using them.
MVKQueueCommandBufferSubmission::MVKQueueCommandBufferSubmission(MVKQueue* queue,
																 const VkSubmitInfo* pSubmit,
																 VkFence fence,
																 MVKCommandUse cmdUse)
	: MVKQueueSubmission(queue,
						 pSubmit ? pSubmit->waitSemaphoreCount : 0,
						 pSubmit ? pSubmit->pWaitSemaphores : nullptr,
						 pSubmit ? pSubmit->pWaitDstStageMask : nullptr),

	_fence((MVKFence*)fence),
	_commandUse(cmdUse) {
	
	if (_fence) { _fence->retain(); }

    // pSubmit can be null if just tracking the fence alone
    if (pSubmit) {
		uint32_t ssCnt = pSubmit->signalSemaphoreCount;
		_signalSemaphores.reserve(ssCnt);
		for (uint32_t i = 0; i < ssCnt; i++) {
			_signalSemaphores.emplace_back(pSubmit->pSignalSemaphores[i], 0);
		}

		VkTimelineSemaphoreSubmitInfo* pTimelineSubmit = nullptr;
        for (const auto* next = (const VkBaseInStructure*)pSubmit->pNext; next; next = next->pNext) {
            switch (next->sType) {
                case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
                    pTimelineSubmit = (VkTimelineSemaphoreSubmitInfo*)next;
                    break;
                default:
                    break;
            }
        }
        if (pTimelineSubmit) {
            uint32_t wsvCnt = pTimelineSubmit->waitSemaphoreValueCount;
            for (uint32_t i = 0; i < wsvCnt; i++) {
                _waitSemaphores[i].value = pTimelineSubmit->pWaitSemaphoreValues[i];
            }

			uint32_t ssvCnt = pTimelineSubmit->signalSemaphoreValueCount;
			for (uint32_t i = 0; i < ssvCnt; i++) {
				_signalSemaphores[i].value = pTimelineSubmit->pSignalSemaphoreValues[i];
			}
        }
    }
}

MVKQueueCommandBufferSubmission::~MVKQueueCommandBufferSubmission() {
	if (_fence) { _fence->release(); }
}


template <size_t N>
void MVKQueueFullCommandBufferSubmission<N>::submitCommandBuffers() {
	uint64_t startTime = getPerformanceTimestamp();

	for (auto& cbInfo : _cmdBuffers) { cbInfo.commandBuffer->submit(this, &_encodingContext); }

	addPerformanceInterval(getPerformanceStats().queue.submitCommandBuffers, startTime);
}


template <size_t N>
bool MVKQueueFullCommandBufferSubmission<N>::supportsMetal4CommandBuffers(
	const char** firstUnsupportedCommand) const {
	if (firstUnsupportedCommand) { *firstUnsupportedCommand = nullptr; }
	for (const auto& cbInfo : _cmdBuffers) {
		if (!cbInfo.commandBuffer) {
			if (firstUnsupportedCommand) { *firstUnsupportedCommand = "null_command_buffer"; }
			return false;
		}
		if (!cbInfo.commandBuffer->supportsMetal4Encoding(firstUnsupportedCommand)) { return false; }
	}
	return true;
}

template <size_t N>
bool MVKQueueFullCommandBufferSubmission<N>::prepareMetal4CommandBuffers(MVKMetal4CommandEncoder* encoder) {
	for (auto& cbInfo : _cmdBuffers) {
		encoder->resetPrepareState();
		if (!cbInfo.commandBuffer->prepareMetal4Encoding(encoder)) { return false; }
	}
	return true;
}

template <size_t N>
bool MVKQueueFullCommandBufferSubmission<N>::beginMetal4CommandBuffers() {
	_metal4PreviousExecutionState.clear();
	_metal4PreviousExecutionState.reserve(_cmdBuffers.size());
	for (auto& cbInfo : _cmdBuffers) {
		bool previousWasExecuted = false;
		if (!cbInfo.commandBuffer->beginMetal4Execution(&previousWasExecuted)) {
			for (size_t idx = 0; idx < _metal4PreviousExecutionState.size(); idx++) {
				_cmdBuffers[idx].commandBuffer->endMetal4Execution(
					_metal4PreviousExecutionState[idx] != 0,
					false);
			}
			_metal4PreviousExecutionState.clear();
			return false;
		}
		_metal4PreviousExecutionState.push_back(previousWasExecuted ? 1 : 0);
	}
	return true;
}

template <size_t N>
void MVKQueueFullCommandBufferSubmission<N>::endMetal4CommandBuffers(bool committed) {
	for (size_t idx = 0; idx < _metal4PreviousExecutionState.size(); idx++) {
		_cmdBuffers[idx].commandBuffer->endMetal4Execution(
			_metal4PreviousExecutionState[idx] != 0,
			committed);
	}
	_metal4PreviousExecutionState.clear();
}

template <size_t N>
bool MVKQueueFullCommandBufferSubmission<N>::encodeMetal4CommandBuffers(MVKMetal4CommandEncoder* encoder) {
	for (auto& cbInfo : _cmdBuffers) {
		if (!cbInfo.commandBuffer->encodeMetal4(encoder)) { return false; }
	}
	return true;
}

template <size_t N>
MVKQueueFullCommandBufferSubmission<N>::MVKQueueFullCommandBufferSubmission(MVKQueue* queue,
																			const VkSubmitInfo2* pSubmit,
																			VkFence fence,
																			MVKCommandUse cmdUse)
	: MVKQueueCommandBufferSubmission(queue, pSubmit, fence, cmdUse) {

	if (pSubmit) {
		uint32_t cbCnt = pSubmit->commandBufferInfoCount;
		_cmdBuffers.reserve(cbCnt);
		for (uint32_t i = 0; i < cbCnt; i++) {
			_cmdBuffers.emplace_back(pSubmit->pCommandBufferInfos[i]);
			setConfigurationResult(_cmdBuffers.back().commandBuffer->getConfigurationResult());
		}
	}
}

template <size_t N>
MVKQueueFullCommandBufferSubmission<N>::MVKQueueFullCommandBufferSubmission(MVKQueue* queue,
																			const VkSubmitInfo* pSubmit,
																			VkFence fence,
																			MVKCommandUse cmdUse)
	: MVKQueueCommandBufferSubmission(queue, pSubmit, fence, cmdUse) {

	if (pSubmit) {
		uint32_t cbCnt = pSubmit->commandBufferCount;
		_cmdBuffers.reserve(cbCnt);
		for (uint32_t i = 0; i < cbCnt; i++) {
			_cmdBuffers.emplace_back(pSubmit->pCommandBuffers[i]);
			setConfigurationResult(_cmdBuffers.back().commandBuffer->getConfigurationResult());
		}
	}
}


#pragma mark -
#pragma mark MVKQueuePresentSurfaceSubmission

// If the semaphores are encodable, wait on them by encoding them on the MTLCommandBuffer before presenting.
// If the semaphores are not encodable, wait on them inline after presenting.
// The semaphores know what to do.
VkResult MVKQueuePresentSurfaceSubmission::execute() {
	_submissionSequence = _queue->reserveMetal4SubmissionSequence();

	// MTLCommandBuffer retain references to avoid rare case where objects are destroyed too early.
	// Although testing could not determine which objects were being lost, queue present MTLCommandBuffers
	// are used only once per frame, and retain so few objects, that blanket retention is still performant.
	id<MTLCommandBuffer> mtlCmdBuff = _queue->getMTLCommandBuffer(kMVKCommandUseQueuePresent, true);
	_queue->encodeMetal4OrderingWait(mtlCmdBuff, _submissionSequence);

	for (auto& ws : _waitSemaphores) {
		ws.encodeWait(mtlCmdBuff);	// Encoded semaphore waits
		ws.encodeWait(nil);			// Inline semaphore waits
	}

	// Wait time from an async vkQueuePresentKHR() call to starting presentation of the swapchains
	addPerformanceInterval(getPerformanceStats().queue.waitPresentSwapchains, _creationTime);

	for (int i = 0; i < _presentInfo.size(); i++ ) {
		setConfigurationResult(_presentInfo[i].presentableImage->presentCAMetalDrawable(mtlCmdBuff, _presentInfo[i]));
	}

	if (_queue->_queueFamily->getIndex() == getMVKConfig().defaultGPUCaptureScopeQueueFamilyIndex &&
		_queue->_index == getMVKConfig().defaultGPUCaptureScopeQueueIndex) {
		getDevice()->stopAutoGPUCapture(MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE_ON_DEMAND);
		getDevice()->startAutoGPUCapture(MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE_ON_DEMAND, _queue->getMTLCommandQueue());
	}

	if ( !mtlCmdBuff ) { setConfigurationResult(VK_ERROR_OUT_OF_POOL_MEMORY); }	// Check after images may set error.
	_queue->encodeMetal4OrderingSignal(mtlCmdBuff, _submissionSequence);

	// Add completion callback to the MTLCommandBuffer to call finish(), 
	// or if the MTLCommandBuffer could not be created, call finish() directly.
	// Retrieve the result first, because finish() will destroy this instance.
	VkResult rslt = getConfigurationResult();
	if (mtlCmdBuff) {
		[mtlCmdBuff addCompletedHandler: ^(id<MTLCommandBuffer> mtlCB) {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
			if (mtlCB.status == MTLCommandBufferStatusError && _queue->_metal4CommandState) {
				_queue->_metal4CommandState->hostSignalOrdering(_submissionSequence);
			}
#endif
			this->finish();
		}];
		[mtlCmdBuff commit];
	} else {
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
		if (_queue->_metal4CommandState) {
			_queue->_metal4CommandState->hostSignalOrdering(_submissionSequence);
		}
#endif
		finish();
	}
	return rslt;
}

void MVKQueuePresentSurfaceSubmission::finish() {

	// Let Xcode know the current frame is done, then start a new frame,
	// and if auto GPU capture is active, and it's time to stop it, do so.
	auto cs = _queue->_submissionCaptureScope;
	cs->endScope();
	cs->beginScope();
	if (_queue->_queueFamily->getIndex() == getMVKConfig().defaultGPUCaptureScopeQueueFamilyIndex &&
		_queue->_index == getMVKConfig().defaultGPUCaptureScopeQueueIndex) {
		getDevice()->stopAutoGPUCapture(MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE_FRAME);
	}

	this->destroy();
}

MVKQueuePresentSurfaceSubmission::MVKQueuePresentSurfaceSubmission(MVKQueue* queue,
																   const VkPresentInfoKHR* pPresentInfo)
	: MVKQueueSubmission(queue, pPresentInfo->waitSemaphoreCount, pPresentInfo->pWaitSemaphores, nullptr) {

	const VkPresentTimesInfoGOOGLE* pPresentTimesInfo = nullptr;
	const VkSwapchainPresentFenceInfoKHR* pPresentFenceInfo = nullptr;
	const VkSwapchainPresentModeInfoKHR* pPresentModeInfo = nullptr;
	const VkPresentRegionsKHR* pPresentRegions = nullptr;
	const VkPresentIdKHR* pPresentId = nullptr;
	const VkPresentId2KHR* pPresentId2 = nullptr;
	for (auto* next = (const VkBaseInStructure*)pPresentInfo->pNext; next; next = next->pNext) {
		switch (next->sType) {
			case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
				pPresentRegions = (const VkPresentRegionsKHR*) next;
				break;
			case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
				pPresentId = (const VkPresentIdKHR*) next;
				break;
			case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
				pPresentId2 = (const VkPresentId2KHR*) next;
				break;
			case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR:
				pPresentFenceInfo = (const VkSwapchainPresentFenceInfoKHR*) next;
				break;
			case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR:
				pPresentModeInfo = (const VkSwapchainPresentModeInfoKHR*) next;
				break;
			case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE:
				pPresentTimesInfo = (const VkPresentTimesInfoGOOGLE*) next;
				break;
			default:
				break;
		}
	}

	// Populate the array of swapchain images, testing each one for status
	uint32_t scCnt = pPresentInfo->swapchainCount;
	const VkPresentTimeGOOGLE* pPresentTimes = nullptr;
	if (pPresentTimesInfo) {
		pPresentTimes = pPresentTimesInfo->pTimes;
		MVKAssert(pPresentTimesInfo->swapchainCount == scCnt, "VkPresentTimesInfoGOOGLE swapchainCount must match VkPresentInfo swapchainCount.");
	}
	const VkPresentModeKHR* pPresentModes = nullptr;
	if (pPresentModeInfo) {
		pPresentModes = pPresentModeInfo->pPresentModes;
		MVKAssert(pPresentModeInfo->swapchainCount == scCnt, "VkSwapchainPresentModeInfoKHR swapchainCount must match VkPresentInfo swapchainCount.");
	}
	const VkFence* pFences = nullptr;
	if (pPresentFenceInfo) {
		pFences = pPresentFenceInfo->pFences;
		MVKAssert(pPresentFenceInfo->swapchainCount == scCnt, "VkSwapchainPresentFenceInfoKHR swapchainCount must match VkPresentInfo swapchainCount.");
	}
	const VkPresentRegionKHR* pRegions = nullptr;
	if (pPresentRegions) {
		pRegions = pPresentRegions->pRegions;
	}
	const uint64_t* pPresentIds = nullptr;
	if (pPresentId2) {
		pPresentIds = pPresentId2->pPresentIds;
	} else if (pPresentId) {
		pPresentIds = pPresentId->pPresentIds;
	}

	VkResult* pSCRslts = pPresentInfo->pResults;
	_presentInfo.reserve(scCnt);
	for (uint32_t scIdx = 0; scIdx < scCnt; scIdx++) {
		MVKSwapchain* mvkSC = (MVKSwapchain*)pPresentInfo->pSwapchains[scIdx];
		MVKImagePresentInfo presentInfo = {};	// Start with everything zeroed
		presentInfo.queue = _queue;
		presentInfo.presentableImage = mvkSC->getPresentableImage(pPresentInfo->pImageIndices[scIdx]);
		presentInfo.presentMode = pPresentModes ? pPresentModes[scIdx] : VK_PRESENT_MODE_MAX_ENUM_KHR;
		presentInfo.fence = pFences ? (MVKFence*)pFences[scIdx] : nullptr;
		presentInfo.presentId = pPresentIds ? pPresentIds[scIdx] : 0;
		if (pPresentTimes) {
			presentInfo.presentIDGoogle = pPresentTimes[scIdx].presentID;
			presentInfo.desiredPresentTime = pPresentTimes[scIdx].desiredPresentTime;
		}
		mvkSC->setLayerNeedsDisplay(pRegions ? &pRegions[scIdx] : nullptr);
		_presentInfo.push_back(presentInfo);
		VkResult scRslt = mvkSC->getSurfaceStatus();
		if (pSCRslts) { pSCRslts[scIdx] = scRslt; }
		setConfigurationResult(scRslt);
	}
}

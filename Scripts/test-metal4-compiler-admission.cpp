#include "MoltenVK/MoltenVK/GPUObjects/MVKMetal4CompilerAdmission.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using mvk::Metal4AdmissionQueue;
using mvk::Metal4AdmissionScope;
using mvk::Metal4AdmissionUrgency;
using mvk::Metal4AdmissionWaiter;

static void testStrictUrgencyOrder() {
	Metal4AdmissionQueue queue;
	Metal4AdmissionScope lifecycle{1, Metal4AdmissionUrgency::Lifecycle, 1};
	Metal4AdmissionScope demanded{2, Metal4AdmissionUrgency::Demanded, 20};
	Metal4AdmissionScope blocking{3, Metal4AdmissionUrgency::Blocking, 30};
	Metal4AdmissionWaiter first{&lifecycle, 1};
	Metal4AdmissionWaiter second{&demanded, 2};
	Metal4AdmissionWaiter third{&blocking, 3};

	assert(queue.enqueue(first));
	assert(queue.enqueue(second));
	assert(queue.enqueue(third));
	assert(queue.best() == &third);
	queue.erase(third);
	assert(queue.best() == &second);
	queue.erase(second);
	assert(queue.best() == &first);
}

static void testRootSequenceThenNativeFifo() {
	Metal4AdmissionQueue queue;
	Metal4AdmissionScope laterRoot{1, Metal4AdmissionUrgency::Blocking, 40};
	Metal4AdmissionScope earlierRoot{2, Metal4AdmissionUrgency::Blocking, 10};
	Metal4AdmissionScope sameRoot{3, Metal4AdmissionUrgency::Blocking, 10};
	Metal4AdmissionWaiter first{&laterRoot, 1};
	Metal4AdmissionWaiter second{&earlierRoot, 2};
	Metal4AdmissionWaiter third{&sameRoot, 3};

	queue.enqueue(first);
	queue.enqueue(second);
	queue.enqueue(third);
	assert(queue.best() == &second);
	queue.erase(second);
	assert(queue.best() == &third);
	queue.erase(third);
	assert(queue.best() == &first);
}

static void testUnknownRootFallsBackToNativeFifo() {
	Metal4AdmissionQueue queue;
	Metal4AdmissionScope unknownFirst{1, Metal4AdmissionUrgency::Demanded, 0};
	Metal4AdmissionScope unknownSecond{2, Metal4AdmissionUrgency::Demanded, 0};
	Metal4AdmissionWaiter first{&unknownFirst, 7};
	Metal4AdmissionWaiter second{&unknownSecond, 8};

	queue.enqueue(first);
	queue.enqueue(second);
	assert(queue.best() == &first);
}

static void testLivePromotionReordersQueuedWork() {
	Metal4AdmissionQueue queue;
	Metal4AdmissionScope firstScope{1, Metal4AdmissionUrgency::Demanded, 30};
	Metal4AdmissionScope promotedScope{2, Metal4AdmissionUrgency::Demanded, 40};
	Metal4AdmissionWaiter first{&firstScope, 1};
	Metal4AdmissionWaiter promoted{&promotedScope, 2};

	queue.enqueue(first);
	queue.enqueue(promoted);
	assert(queue.best() == &first);
	assert(mvk::promoteMetal4AdmissionScope(
		promotedScope, Metal4AdmissionUrgency::Blocking, 5));
	assert(queue.best() == &promoted);
	assert(!mvk::promoteMetal4AdmissionScope(
		promotedScope, Metal4AdmissionUrgency::Demanded, 50));
	assert(promotedScope.urgency == Metal4AdmissionUrgency::Blocking);
	assert(promotedScope.orderingSequence == 5);
}

static void testQueueMembershipAndCapacity() {
	Metal4AdmissionQueue queue;
	Metal4AdmissionScope scope{1, Metal4AdmissionUrgency::Demanded, 1};
	Metal4AdmissionWaiter waiter{&scope, 1};

	assert(queue.enqueue(waiter));
	assert(!queue.enqueue(waiter));
	assert(queue.size() == 1);
	assert(!queue.canAdmit(waiter, 1, 1));
	assert(queue.canAdmit(waiter, 0, 1));
	assert(queue.canAdmit(waiter, 1, 2));
	assert(!queue.canAdmit(waiter, 0, 0));
	assert(queue.erase(waiter));
	assert(!queue.erase(waiter));
	assert(queue.empty());
}

static std::vector<uint64_t> admitOneCapacityWave(size_t taskMaximum) {
	Metal4AdmissionQueue queue;
	Metal4AdmissionScope scopes[] = {
		{1, Metal4AdmissionUrgency::Lifecycle, 1},
		{2, Metal4AdmissionUrgency::Demanded, 20},
		{3, Metal4AdmissionUrgency::Blocking, 30},
		{4, Metal4AdmissionUrgency::Demanded, 10},
	};
	Metal4AdmissionWaiter waiters[] = {
		{&scopes[0], 1},
		{&scopes[1], 2},
		{&scopes[2], 3},
		{&scopes[3], 4},
	};
	for (auto& waiter : waiters) { queue.enqueue(waiter); }

	std::vector<uint64_t> admitted;
	size_t tasksInFlight = 0;
	while (!queue.empty() && tasksInFlight < taskMaximum) {
		Metal4AdmissionWaiter* winner = queue.best();
		assert(winner);
		assert(queue.canAdmit(*winner, tasksInFlight, taskMaximum));
		admitted.push_back(winner->scope->scopeId);
		queue.erase(*winner);
		tasksInFlight++;
	}
	if (!queue.empty()) {
		assert(!queue.canAdmit(*queue.best(), tasksInFlight, taskMaximum));
	}
	return admitted;
}

static void testCapacityNeverExpandsPastConfiguredLimit() {
	assert((admitOneCapacityWave(1) == std::vector<uint64_t>{3}));
	assert((admitOneCapacityWave(2) == std::vector<uint64_t>{3, 4}));
	assert((admitOneCapacityWave(3) == std::vector<uint64_t>{3, 4, 2}));
}

static void testLaneDoesNotSplitTheSharedOrder() {
	Metal4AdmissionQueue queue;
	Metal4AdmissionScope libraryScope{1, Metal4AdmissionUrgency::Demanded, 20};
	Metal4AdmissionScope renderScope{2, Metal4AdmissionUrgency::Blocking, 30};
	Metal4AdmissionScope computeScope{3, Metal4AdmissionUrgency::Demanded, 10};
	Metal4AdmissionWaiter library{&libraryScope, 1};
	Metal4AdmissionWaiter render{&renderScope, 2};
	Metal4AdmissionWaiter compute{&computeScope, 3};

	queue.enqueue(library);
	queue.enqueue(render);
	queue.enqueue(compute);
	assert(queue.best() == &render);
	queue.erase(render);
	assert(queue.best() == &compute);
	queue.erase(compute);
	assert(queue.best() == &library);
}

int main() {
	testStrictUrgencyOrder();
	testRootSequenceThenNativeFifo();
	testUnknownRootFallsBackToNativeFifo();
	testLivePromotionReordersQueuedWork();
	testQueueMembershipAndCapacity();
	testCapacityNeverExpandsPastConfiguredLimit();
	testLaneDoesNotSplitTheSharedOrder();
	std::cout << "Metal 4 compiler admission policy tests passed.\n";
	return 0;
}

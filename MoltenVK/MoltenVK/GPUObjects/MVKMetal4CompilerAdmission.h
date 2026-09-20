/*
 * MVKMetal4CompilerAdmission.h
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace mvk {

enum class Metal4AdmissionUrgency : uint8_t {
	Lifecycle = 0,
	Demanded = 1,
	Blocking = 2,
};

/** Mutable priority shared by every native compiler task in one managed work scope. */
struct Metal4AdmissionScope {
	uint64_t scopeId = 0;
	Metal4AdmissionUrgency urgency = Metal4AdmissionUrgency::Demanded;
	uint64_t orderingSequence = 0;
};

/**
 * Improves a scope in place. Priority never moves backwards; a newly discovered
 * earlier root may refine the stable order without restarting queued work.
 */
inline bool promoteMetal4AdmissionScope(Metal4AdmissionScope& scope,
										Metal4AdmissionUrgency urgency,
										uint64_t orderingSequence) {
	bool changed = false;
	if (urgency > scope.urgency) {
		scope.urgency = urgency;
		changed = true;
	}
	if (orderingSequence != 0 &&
		(scope.orderingSequence == 0 || orderingSequence < scope.orderingSequence)) {
		scope.orderingSequence = orderingSequence;
		changed = true;
	}
	return changed;
}

/** Intrusive waiter owned by the blocked compiler thread. */
struct Metal4AdmissionWaiter {
	Metal4AdmissionScope* scope = nullptr;
	uint64_t enqueueSequence = 0;
	Metal4AdmissionWaiter* next = nullptr;
	bool queued = false;
};

/**
 * Allocation-free ordered admission for the small, bounded Metal compiler pool.
 * Callers provide synchronization. The queue scans its short waiter list so a
 * live scope promotion immediately changes the next winner without relinking.
 */
class Metal4AdmissionQueue {

public:
	bool enqueue(Metal4AdmissionWaiter& waiter) {
		if (waiter.queued) { return false; }
		waiter.next = nullptr;
		waiter.queued = true;
		if (!_head) {
			_head = &waiter;
		} else {
			Metal4AdmissionWaiter* tail = _head;
			while (tail->next) { tail = tail->next; }
			tail->next = &waiter;
		}
		_size++;
		return true;
	}

	bool erase(Metal4AdmissionWaiter& waiter) {
		Metal4AdmissionWaiter** current = &_head;
		while (*current && *current != &waiter) { current = &(*current)->next; }
		if (!*current) { return false; }
		*current = waiter.next;
		waiter.next = nullptr;
		waiter.queued = false;
		_size--;
		return true;
	}

	Metal4AdmissionWaiter* best() const {
		Metal4AdmissionWaiter* winner = _head;
		for (Metal4AdmissionWaiter* candidate = _head ? _head->next : nullptr;
			 candidate;
			 candidate = candidate->next) {
			if (precedes(*candidate, *winner)) { winner = candidate; }
		}
		return winner;
	}

	bool canAdmit(const Metal4AdmissionWaiter& waiter,
				  size_t tasksInFlight,
				  size_t taskMaximum) const {
		return taskMaximum > 0 && tasksInFlight < taskMaximum && best() == &waiter;
	}

	bool empty() const { return _head == nullptr; }
	size_t size() const { return _size; }

private:
	static Metal4AdmissionUrgency urgency(const Metal4AdmissionWaiter& waiter) {
		return waiter.scope ? waiter.scope->urgency : Metal4AdmissionUrgency::Demanded;
	}

	static uint64_t orderingSequence(const Metal4AdmissionWaiter& waiter) {
		if (!waiter.scope || waiter.scope->orderingSequence == 0) {
			return std::numeric_limits<uint64_t>::max();
		}
		return waiter.scope->orderingSequence;
	}

	static bool precedes(const Metal4AdmissionWaiter& left,
						 const Metal4AdmissionWaiter& right) {
		if (urgency(left) != urgency(right)) { return urgency(left) > urgency(right); }
		if (orderingSequence(left) != orderingSequence(right)) {
			return orderingSequence(left) < orderingSequence(right);
		}
		return left.enqueueSequence < right.enqueueSequence;
	}

	Metal4AdmissionWaiter* _head = nullptr;
	size_t _size = 0;
};

} // namespace mvk

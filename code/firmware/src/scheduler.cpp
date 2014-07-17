#include "scheduler.h"

#include <pthread.h> //for pthread_setschedparam
#include <time.h> //for clock_nanosleep
#include <signal.h> //for sigaction signal handlers
#include <cstdlib> //for atexit
#include <algorithm> //for push_heap
#include "logging.h"
#include "timeutil.h"

std::array<std::vector<void(*)()>, SCHED_NUM_EXIT_HANDLER_LEVELS> Scheduler::exitHandlers;
std::atomic<bool> Scheduler::isExiting(false);


void ctrlCOrZHandler(int s){
   printf("Caught signal %d\n",s);
   exit(1); 
}

void segfaultHandler(int /*signal*/, siginfo_t *si, void */*arg*/) {
    printf("Caught segfault at address %p\n", si->si_addr);
    exit(1);
}

void Scheduler::callExitHandlers() {
	if (!isExiting.exchange(true)) { //try setting isExiting to true. If it was previously false, then call the exit handlers:
		LOG("Exiting\n");
		for (const std::vector<void(*)()>& level : exitHandlers) {
			for (void(*handler)() : level) {
				(*handler)();
			}
		}
	}
}


void Scheduler::configureExitHandlers() {
	std::atexit((void(*)())&Scheduler::callExitHandlers);
	//listen for ctrl+c, ctrl+z and segfaults. Then try to properly unmount any I/Os (crucial for disabling the heated nozzle)
	struct sigaction sigIntHandler;
	sigIntHandler.sa_handler = ctrlCOrZHandler;
	sigemptyset(&sigIntHandler.sa_mask);
	sigIntHandler.sa_flags = 0;
	sigaction(SIGINT, &sigIntHandler, NULL); //register ctrl+c
	sigaction(SIGTSTP, &sigIntHandler, NULL); //register ctrl+z
	sigaction(SIGABRT, &sigIntHandler, NULL); //register SIGABRT, which is triggered for critical errors (eg glibc detects double-free)
	
	struct sigaction sa;
    //memset(&sa, 0, sizeof(sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = segfaultHandler;
    sa.sa_flags   = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); //register segfault listener
}

void Scheduler::registerExitHandler(void (*handler)(), unsigned level) {
	if (level > exitHandlers.size()) {
		throw std::runtime_error("Tried to register an exit handler at too high of a level");
	}
	exitHandlers[level].push_back(handler);
	//std::atexit(handler);
}


Scheduler::Scheduler() : _lockPushes(mutex, std::defer_lock), _arePushesLocked(false), bufferSize(SCHED_CAPACITY) {
	//clock_gettime(CLOCK_MONOTONIC, &(this->lastEventHandledTime)); //initialize to current time.
}


void Scheduler::queue(const Event& evt) {
	//LOGV("Scheduler::queue\n");
	std::unique_lock<std::mutex> lock(this->mutex);
	/*if (this->eventQueue.empty()) { //if no event is before this, then the relative time held by this event must be associated with the current time:
		clock_gettime(CLOCK_MONOTONIC, &(this->lastEventHandledTime));
	}*/
	this->orderedInsert(evt);
	this->nonemptyCond.notify_one(); //notify the consumer thread that a new event is ready.
}

void Scheduler::orderedInsert(const Event &evt) {
	this->eventQueue.push_back(evt);
	if (timespecLt(evt.time(), this->eventQueue.back().time())) { //If not already ordered, we must order it.
		std::push_heap(this->eventQueue.begin(), this->eventQueue.end());
	}
}

void Scheduler::schedPwm(AxisIdType idx, const PwmInfo &p) {
	LOGV("Scheduler::schedPwm: %i, %u, %u. Current: %u, %u\n", idx, p.nsHigh, p.nsLow, pwmInfo[idx].nsHigh, pwmInfo[idx].nsLow);
	if (pwmInfo[idx].nsHigh != 0 && pwmInfo[idx].nsLow != 0) { //already scheduled and running. Just update times.
		pwmInfo[idx] = p;
	} else { //have to schedule:
		LOGV("Scheduler::schedPwm: queueing\n");
		pwmInfo[idx] = p;
		Event evt(timespecNow(), idx, StepForward);
		this->queue(evt);
	}
}


Event Scheduler::nextEvent() {
	Event evt;
	if (!this->_arePushesLocked) { //Lock other threads from pushing to queue, if not done already.
		_lockPushes.lock();
	}
	while (this->eventQueue.empty()) { //wait for an event to be pushed.
		this->nonemptyCond.wait(_lockPushes); //condition_variable.wait() can produce spurious wakeups; need the while loop.
	}
	evt = this->eventQueue.front();
	this->eventQueue.pop_front();
	//check if event is PWM-based:
	if (evt.direction() == StepForward) {
		if (pwmInfo[evt.stepperId()].nsHigh != 0) { //do we have a defined high-time? If not, then PWM is over.
			Event nextPwm(evt.time(), evt.stepperId(), StepBackward);
			nextPwm.offsetNano(pwmInfo[evt.stepperId()].nsHigh);
			this->orderedInsert(nextPwm); //to do: ordered insert
		}
	} else {
		if (pwmInfo[evt.stepperId()].nsLow != 0) { //do we have a defined low-time? If not, then PWM is over.
			Event nextPwm(evt.time(), evt.stepperId(), StepForward);
			nextPwm.offsetNano(pwmInfo[evt.stepperId()].nsLow);
			this->orderedInsert(nextPwm);
		}
	}
	if (this->eventQueue.size() < bufferSize) { //queue is underfilled; release the lock
		_lockPushes.unlock();
		this->_arePushesLocked = false;
	} else { //queue is filled; do not release the lock.
		this->_arePushesLocked = true;
	}
	
	struct timespec sleepUntil = evt.time();
	struct timespec curTime;
	clock_gettime(CLOCK_MONOTONIC, &curTime);
	//LOGV("Scheduler::nextEvent sleep from %lu.%lu until %lu.%lu\n", curTime.tv_sec, curTime.tv_nsec, sleepUntil.tv_sec, sleepUntil.tv_nsec);
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleepUntil, NULL); //sleep to event time.
	//clock_gettime(CLOCK_MONOTONIC, &(this->lastEventHandledTime)); //in case we fall behind, preserve the relative time between events.
	//this->lastEventHandledTime = sleepUntil;
	return evt;
}

void Scheduler::initSchedThread() {
	struct sched_param sp; 
	sp.sched_priority=SCHED_PRIORITY; 
	if (int ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) {
		LOGW("Warning: pthread_setschedparam (increase thread priority) at scheduler.cpp returned non-zero: %i\n", ret);
	}
}

struct timespec Scheduler::lastSchedTime() const {
	Event evt;
	{
		std::unique_lock<std::mutex> lock(this->mutex);
		if (this->eventQueue.size()) {
			evt = this->eventQueue.back();
		} else {
			lock.unlock();
			timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			return ts;
		}
	}
	return evt.time();
}

void Scheduler::setBufferSize(unsigned size) {
	this->bufferSize = size;
}
unsigned Scheduler::getBufferSize() const {
	return this->bufferSize;
}

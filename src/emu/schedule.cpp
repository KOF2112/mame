// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    schedule.cpp

    Core device execution and scheduling engine.

---

	Still to do:
	- Test out save states
	- Clean up timer devices
	- Clean up more timers in devices/drivers
	- Attotime tweaks:

	   - Make ATTOTIME_MAX_SECONDS = (1 << 29)
	   - Remove check at start of add
	   - Is never could be (seconds >> 29) == 1
	   - Is zero should check attoseconds first

inline attotime operator+(const attotime &left, const attotime &right) noexcept
{
	attotime result;

	// add the seconds and attoseconds
	attoseconds_t attos = left.m_attoseconds + right.m_attoseconds;
	seconds_t secs = left.m_seconds + right.m_seconds;

	// normalize
	attoseconds_t over = attos - ATTOSECONDS_PER_SECOND;
	if (over >= 0)
	{
		attos = over;
		secs += 1;
	}

	// overflow
	if (secs >= ATTOTIME_MAX_SECONDS)
		return attotime::never;

	return attotime(secs, attos);
}

***************************************************************************/

#include "emu.h"
#include "debugger.h"
#include "hashing.h"


//**************************************************************************
//  DEBUGGING
//**************************************************************************

#define VERBOSE 0

#define LOG(...)  do { if (VERBOSE) machine().logerror(__VA_ARGS__); } while (0)
#define PRECISION 18



//**************************************************************************
//  EMU TIMER CB
//**************************************************************************

//-------------------------------------------------
//  timer_callback - constructor
//-------------------------------------------------

timer_callback::timer_callback(persistent_timer *persistent) :
	m_ptr(nullptr),
	m_scheduler(nullptr),
	m_next_registered(nullptr),
	m_persistent(persistent),
	m_device(nullptr),
	m_unique_hash(0),
	m_save_index(0)
{
}


//-------------------------------------------------
//  ~timer_callback - destructor
//-------------------------------------------------

timer_callback::~timer_callback()
{
	if (m_scheduler != nullptr)
		m_scheduler->deregister_callback(*this);
}


//-------------------------------------------------
//  timer_callback - copy constructor
//-------------------------------------------------

timer_callback::timer_callback(timer_callback const &src) :
	m_delegate(src.m_delegate),
	m_ptr(src.m_ptr),
	m_scheduler(src.m_scheduler),
	m_next_registered(src.m_next_registered),
	m_persistent(nullptr),
	m_device(src.m_device),
	m_unique_hash(src.m_unique_hash),
	m_save_index(src.m_save_index),
	m_unique_id(src.m_unique_id)
{
}


//-------------------------------------------------
//  operator= - copy assignment
//-------------------------------------------------

timer_callback &timer_callback::operator=(timer_callback const &src)
{
	if (&src != this)
	{
		m_delegate = src.m_delegate;
		m_ptr = src.m_ptr;
		m_device = src.m_device;
		m_scheduler = src.m_scheduler;
		m_next_registered = src.m_next_registered;
		// deliberately do not touch m_persistent since it is
		// an allocation-only property
		m_unique_hash = src.m_unique_hash;
		m_unique_id = src.m_unique_id;
	}
	return *this;
}


//-------------------------------------------------
//  set_ptr - set the callback's pointer value;
//  only valid at initialization
//-------------------------------------------------

timer_callback &timer_callback::set_ptr(void *ptr)
{
	// only allowed to set pointers prior to execution; use the save state
	// registration_allowed() as a proxy for this
	if (!m_scheduler->machine().save().registration_allowed())
		throw emu_fatalerror("Timer pointers must remain constant after creation.");
	m_ptr = ptr;
	return *this;
}


//-------------------------------------------------
//  set_device - set the callback's associated
//  device; only valid at initialization
//-------------------------------------------------

timer_callback &timer_callback::set_device(device_t &device)
{
	// only allowed to set pointers prior to execution; use the save state
	// registration_allowed() as a proxy for this
	if (!m_scheduler->machine().save().registration_allowed())
		throw emu_fatalerror("Timer devices must remain constant after creation.");
	m_device = &device;
	return *this;
}


//-------------------------------------------------
//  init_base - register a callback
//-------------------------------------------------

timer_callback &timer_callback::init_base(device_scheduler &scheduler, timer_expired_delegate const &delegate, char const *unique, const char *unique2)
{
	// build the full name, appending the unique identifier(s) if present
	std::string fullid = delegate.name();
	if (unique != nullptr)
	{
		fullid += "/";
		fullid += unique;
	}
	if (unique2 != nullptr)
	{
		fullid += "/";
		fullid += unique2;
	}

	// if not already registered, just pass through
	if (m_next_registered == nullptr)
	{
		m_delegate = delegate;
		m_scheduler = &scheduler;
		m_unique_id = fullid;
		m_unique_hash = util::crc32_creator::simple(fullid.c_str(), fullid.length());
		m_save_index = m_scheduler->register_callback(*this);
printf("Registered: %d %08X %s\n", m_save_index, m_unique_hash, m_unique_id.c_str());
	}

	// otherwise, make sure we match
	else
	{
		if (m_delegate != delegate)
			throw emu_fatalerror("timer_callback::init called multiple times on the same object with different callbacks.");
		if (m_unique_id != fullid)
			throw emu_fatalerror("timer_callback::init called multiple times on the same object with different ids (%s vs. %s).", m_unique_id.c_str(), fullid.c_str());
	}
	return *this;
}


//-------------------------------------------------
//  init_device - register this callback,
//  associated with a device
//-------------------------------------------------

timer_callback &timer_callback::init_device(device_t &device, timer_expired_delegate const &delegate, char const *unique)
{
	return init(device.machine().scheduler(), delegate, device.tag(), unique).set_device(device);
}


//-------------------------------------------------
//  init_clone - initialize as a clone of another
//  callback, but with a different delegate
//-------------------------------------------------

timer_callback &timer_callback::init_clone(timer_callback const &src, timer_expired_delegate const &delegate)
{
	// start with a direct copy
	*this = src;

	// replace the delegate and clear the registration
	m_delegate = delegate;
	m_next_registered = nullptr;
	return *this;
}



//**************************************************************************
//  TIMER INSTANCE
//**************************************************************************

//-------------------------------------------------
//  timer_instance - constructor
//-------------------------------------------------

timer_instance::timer_instance() :
	m_next(nullptr),
	m_prev(nullptr),
	m_start(attotime::zero),
	m_expire(attotime::never),
	m_callback(nullptr),
	m_param{ 0, 0, 0 },
	m_active(false)
{
}


//-------------------------------------------------
//  ~timer_instance - destructor
//-------------------------------------------------

timer_instance::~timer_instance()
{
}


//-------------------------------------------------
//  init_persistent - initialize a persistent
//  system or device timer; persistent timers can
//  be saved and start off in a disabled state
//-------------------------------------------------

timer_instance &timer_instance::init_persistent(timer_callback &callback)
{
	assert(callback.persistent() != nullptr);
	m_callback = &callback;

	// everything else has been initialized by the constructor;
	// unlike transient timers, we are embedded in the persistent_timer
	// object, and so don't need to worry about re-use

	return *this;
}


//-------------------------------------------------
//  init_transient - initialize a transient
//  system timer; transient timers have a parameter
//  and expiration time from the outset
//-------------------------------------------------

timer_instance &timer_instance::init_transient(timer_callback &callback, attotime const &duration)
{
	assert(callback.persistent() == nullptr);
	m_callback = &callback;

	// ensure the entire timer state is clean, since we re-use these
	// instances for fast allocation
	m_param[0] = m_param[1] = m_param[2] = 0;
	m_active = false;

	// add immediately to the active queue
	attotime start = callback.scheduler().time();
	return insert(start, start + duration);
}


//-------------------------------------------------
//  elapsed - return the amount of time since the
//  timer was started
//-------------------------------------------------

attotime timer_instance::elapsed() const noexcept
{
	return scheduler().time() - m_start;
}


//-------------------------------------------------
//  remaining - return the amount of time
//  remaining until the timer expires
//-------------------------------------------------

attotime timer_instance::remaining() const noexcept
{
	attotime curtime = scheduler().time();
	if (curtime >= m_expire)
		return attotime::zero;
	return m_expire - curtime;
}


//-------------------------------------------------
//  save - save our state to the given save data
//  structure
//-------------------------------------------------

timer_instance &timer_instance::save(timer_instance_save &dst)
{
	dst.start = m_start;
	dst.expire = m_expire;
	dst.param[0] = m_param[0];
	dst.param[1] = m_param[1];
	dst.param[2] = m_param[2];
	dst.hash = m_callback->unique_hash();
	dst.save_index = m_callback->save_index();
	return *this;
}


//-------------------------------------------------
//  restore - restore our state from the given
//  save data structure
//-------------------------------------------------

timer_instance &timer_instance::restore(timer_instance_save const &src, timer_callback &callback, bool enabled)
{
	m_callback = &callback;
	m_param[0] = src.param[0];
	m_param[1] = src.param[1];
	m_param[2] = src.param[2];
	m_active = false;
	m_start = src.start;
	m_expire = src.expire;
	return enabled ? insert(src.start, src.expire) : *this;
}


//-------------------------------------------------
//  insert - insert us into the scheduler's
//  active timer queue
//-------------------------------------------------

timer_instance &timer_instance::insert(attotime const &start, attotime const &expire)
{
	m_start = start;
	m_expire = expire;
	m_active = true;
	return m_callback->scheduler().instance_insert(*this);
}


//-------------------------------------------------
//  remove - remove us from the scheduler's
//  active timer queue
//-------------------------------------------------

timer_instance &timer_instance::remove()
{
	m_active = false;
	return m_callback->scheduler().instance_remove(*this);
}


//-------------------------------------------------
//  dump - dump internal state to a single output
//  line in the error log
//-------------------------------------------------

void timer_instance::dump() const
{
	persistent_timer *persistent = m_callback->persistent();

	running_machine &machine = scheduler().machine();
	machine.logerror("%p: %s exp=%15s start=%15s ptr=%p param=%lld/%lld/%lld",
		this,
		(m_callback->persistent() != nullptr) ? "P" : "T",
		m_expire.as_string(PRECISION),
		m_start.as_string(PRECISION),
		m_callback->ptr(),
		m_param[0], m_param[1], m_param[2]);

	if (persistent != nullptr)
		machine.logerror(" per=%15s", persistent->period().as_string(PRECISION));

	if (m_callback->device() != nullptr)
		machine.logerror(" dev=%s id=%d\n", m_callback->device()->tag(), int(param(2)));
	else
		machine.logerror(" cb=%s\n", m_callback->name());
}



//**************************************************************************
//  PERSISTENT_TIMER
//**************************************************************************

//-------------------------------------------------
//  persistent_timer - constructor
//-------------------------------------------------

persistent_timer::persistent_timer() :
	m_modified(0),
	m_callback(this),
	m_periodic_callback(this)
{
}


//-------------------------------------------------
//  ~persistent_timer - destructor
//-------------------------------------------------

persistent_timer::~persistent_timer()
{
}


//-------------------------------------------------
//  enable - enable a timer, returning the
//  previous state
//-------------------------------------------------

bool persistent_timer::enable(bool enable)
{
	// fetch the previous state and set the new one
	bool old = enabled();
	m_enabled = enable;

	// if nothing changed, leave it alone
	if (old == enable)
		return old;

	// remove if previously active
	if (m_instance.active())
		m_instance.remove();

	// only re-insert if enabled
	if (enable)
		m_instance.insert(m_instance.start(), m_instance.expire());

	// mark as modified
	m_modified = true;
	return old;
}


//-------------------------------------------------
//  adjust - change the timer's start time,
//  parameter, or period
//-------------------------------------------------

persistent_timer &persistent_timer::adjust(attotime const &start_delay, s32 param, attotime const &period)
{
	// set the parameters first
	m_instance.set_param(param);

	// adjust implicitly enables the timer
	m_enabled = true;

	// set the period and adjust the callback appropriately
	m_period = period.is_zero() ? attotime::never : period;
	if (periodic())
		m_instance.m_callback = &m_periodic_callback;
	else
		m_instance.m_callback = &m_callback;

	// compute start/expire times
	attotime start = m_callback.scheduler().time();
	attotime expire = start;
	if (start_delay.seconds() >= 0)
		expire += start_delay;

	// then insert into the active list, removing first if previously active
	if (m_instance.active())
		m_instance.remove();
	m_instance.insert(start, expire);

	// mark as modified
	m_modified = true;
	return *this;
}


//-------------------------------------------------
//  init_common - handle common initialization
//  tasks
//-------------------------------------------------

persistent_timer &persistent_timer::init_common()
{
	// initialize the timer instance
	m_instance.init_persistent(m_callback);

	// create the periodic callback by cloning (but not registering) our periodic
	// front-end callback
	m_periodic_callback.init_clone(m_callback, timer_expired_delegate(FUNC(persistent_timer::periodic_callback), this));

	return *this;
}


//-------------------------------------------------
//  save - save persistent timer data to the given
//  save data structure
//-------------------------------------------------

persistent_timer &persistent_timer::save(timer_instance_save &dst)
{
	m_instance.save(dst);

	// overwrite the hash/save_index from the instance becuase it could be pointing
	// to our periodic callback and we want the real underlying callback
	dst.hash = m_callback.unique_hash();
	dst.save_index = m_callback.save_index();
	dst.period = m_period;
	dst.enabled = enabled();
	return *this;
}


//-------------------------------------------------
//  restore - restore persistent timer data from
//  the given save data structure
//-------------------------------------------------

persistent_timer &persistent_timer::restore(timer_instance_save const &src, timer_callback &callback)
{
	m_period = src.period;
	m_enabled = src.enabled;
	m_instance.restore(src, periodic() ? m_periodic_callback : m_callback, m_enabled);
	return *this;
}


//-------------------------------------------------
//  periodic_callback - callback to handle
//  periodic timers
//-------------------------------------------------

void persistent_timer::periodic_callback(timer_instance const &timer)
{
	// clear the modified state
	m_modified = false;

	// call the real callback
	m_callback(timer);

	// if the timer wasn't modified during the callback, advance by one period
	if (!m_modified)
		m_instance.insert(m_instance.m_expire, m_instance.m_expire + m_period);
}



//**************************************************************************
//  TRANSIENT TIMER FACTORY
//**************************************************************************

//-------------------------------------------------
//  transient_timer_factory - constructor
//-------------------------------------------------

transient_timer_factory::transient_timer_factory()
{
}



//**************************************************************************
//  BASETIME-RELATIVE
//**************************************************************************

//-------------------------------------------------
//  basetime_relative - constructor
//-------------------------------------------------

device_scheduler::basetime_relative::basetime_relative() :
	m_relative(0),
	m_absolute(attotime::zero),
	m_absolute_dirty(false),
	m_base_seconds(0)
{
}


//-------------------------------------------------
//  set - set an absolute time, updating the
//  base-relative time as well
//-------------------------------------------------

void device_scheduler::basetime_relative::set(attotime const &src)
{
	m_absolute = src;
	m_absolute_dirty = false;
	update_relative();
}


//-------------------------------------------------
//  add - add attoseconds to the base-relative
//  time, marking the absolute time dirty for
//  later conversion if needed
//-------------------------------------------------

void device_scheduler::basetime_relative::add(attoseconds_t src)
{
	m_relative += src;
	m_absolute_dirty = true;
}


//-------------------------------------------------
//  set_base_seconds - set the base seconds value
//-------------------------------------------------

void device_scheduler::basetime_relative::set_base_seconds(seconds_t base)
{
	// update the absolute time if dirty first
	if (m_absolute_dirty)
		update_absolute();

	// then set and recompute the relative from the absolute time
	m_base_seconds = base;
	update_relative();
}


//-------------------------------------------------
//  update_relative - update the relative time
//  from the absolute time
//-------------------------------------------------

void device_scheduler::basetime_relative::update_relative()
{
	seconds_t delta = m_absolute.seconds() - m_base_seconds;

	// if the seconds match, then the relative time is fine as-is
	m_relative = m_absolute.attoseconds();
	if (delta == 0)
		return;

	// if the absolute time is ahead/behind, we need to add/subtract
	// ATTOSECONDS_PER_SECOND; but only do it once
	if (delta > 0)
	{
		if (delta == 1)
			m_relative += ATTOSECONDS_PER_SECOND;
		else
			m_relative = MAX_RELATIVE;
	}
	else
	{
		if (delta == -1)
			m_relative -= ATTOSECONDS_PER_SECOND;
		else
			m_relative = MIN_RELATIVE;
	}
}


//-------------------------------------------------
//  update_absolute - update the absolute time
//  from the relative time
//-------------------------------------------------

void device_scheduler::basetime_relative::update_absolute()
{
	seconds_t secs = m_base_seconds;
	attoseconds_t attos = m_relative;

	// if relative is outside of range, adjust the seconds
	if (attos >= ATTOSECONDS_PER_SECOND)
	{
		attos -= ATTOSECONDS_PER_SECOND;
		secs++;
	}
	else if (attos < 0)
	{
		attos += ATTOSECONDS_PER_SECOND;
		secs--;
	}

	// set the new value and clear any dirtiness
	m_absolute.set_seconds(secs);
	m_absolute.set_attoseconds(attos);
	m_absolute_dirty = false;
}



//**************************************************************************
//  DEVICE SCHEDULER
//**************************************************************************

//-------------------------------------------------
//  device_scheduler - constructor
//-------------------------------------------------

device_scheduler::device_scheduler(running_machine &machine) :
	m_machine(machine),
	m_executing_device(nullptr),
	m_execute_list(nullptr),
	m_basetime(attotime::zero),
	m_active_timers_head(&m_active_timers_tail),
	m_free_timers(nullptr),
	m_registered_callbacks(nullptr),
	m_callback_timer(nullptr),
	m_callback_timer_expire_time(attotime::zero),
	m_suspend_changes_pending(true),
	m_quantum_minimum(ATTOSECONDS_IN_NSEC(1) / 1000)
{
	// register global states
	auto &save = machine.save();
	save.save_item(NAME(m_basetime));

	// we could use STRUCT_MEMBER here if it worked on attotimes, but it doesn't
	// currently, so do it the manual way
	for (int index = 0; index < MAX_SAVE_INSTANCES; index++)
	{
		save.save_item(NAME(m_timer_save[index].start), index);
		save.save_item(NAME(m_timer_save[index].expire), index);
		save.save_item(NAME(m_timer_save[index].param), index);
		save.save_item(NAME(m_timer_save[index].hash), index);
		save.save_item(NAME(m_timer_save[index].save_index), index);
		save.save_item(NAME(m_timer_save[index].enabled), index);
		save.save_item(NAME(m_timer_save[index].period), index);
	}

	// create a factory for empty timers
	m_empty_timer.init(*this, *this, FUNC(device_scheduler::empty_timer));

	// create a factory for trigger timers
	m_timed_trigger.init(*this, *this, FUNC(device_scheduler::timed_trigger));
}


//-------------------------------------------------
//  device_scheduler - destructor
//-------------------------------------------------

device_scheduler::~device_scheduler()
{
}


//-------------------------------------------------
//  time - return the current time
//-------------------------------------------------

attotime device_scheduler::time() const noexcept
{
	// if we're currently in a callback, use the timer's expiration time as a base
	if (m_callback_timer != nullptr)
		return m_callback_timer_expire_time;

	// if we're executing as a particular CPU, use its local time as a base
	// otherwise, return the global base time
	return (m_executing_device != nullptr) ? m_executing_device->local_time() : m_basetime;
}


//-------------------------------------------------
//  can_save - return true if it's safe to save
//  (i.e., no transient timers outstanding)
//-------------------------------------------------

bool device_scheduler::can_save() const
{
	// count the total number of active timers
	int index = 0;
	for (timer_instance *timer = m_active_timers_head; timer != &m_active_timers_tail; timer = timer->next())
		index++;

	// also count the number of inactive persistent timers
	for (timer_callback *cb = m_registered_callbacks; cb != nullptr && index < MAX_SAVE_INSTANCES; cb = cb->m_next_registered)
		if (cb->persistent() != nullptr && !cb->persistent()->instance().active())
			index++;

	return (index <= MAX_SAVE_INSTANCES);
}


//-------------------------------------------------
//  timeslice - execute all devices for a single
//  timeslice
//-------------------------------------------------

template<bool Debugging>
void device_scheduler::timeslice()
{
	LOG("------------------\n");

	// build the execution list if we don't have one yet
	if (UNEXPECTED(m_execute_list == nullptr))
		rebuild_execute_list();

	// if the current quantum has expired, find a new one
	while (m_basetime >= m_quantum_list.first()->m_expire)
		m_quantum_allocator.reclaim(m_quantum_list.detach_head());

	// loop until we hit the next timer
	attoseconds_t basetime = m_basetime.attoseconds();
	while (basetime < m_first_timer_expire.relative())
	{
		// by default, assume our target is the end of the next quantum
		attoseconds_t target = basetime + m_quantum_list.first()->m_actual;
		assert(target < basetime_relative::MAX_RELATIVE);

		// however, if the next timer is going to fire before then, override
		if (m_first_timer_expire.relative() < target)
			target = m_first_timer_expire.relative();

		LOG("timeslice: target = %18lldas\n", target);

		// do we have pending suspension changes?
		if (m_suspend_changes_pending)
			apply_suspend_changes();

		// loop over all executing devices
		for (device_execute_interface *exec = m_execute_list; exec != nullptr; exec = exec->m_nextexec)
		{
			// compute how many attoseconds to execute this device
			attoseconds_t delta = target - exec->m_localtime.relative() - 1;
			assert(delta < basetime_relative::MAX_RELATIVE);

			// if we're already ahead, do nothing; in theory we should do this 0 as
			// well, but it's a rare case and the compiler tends to be able to
			// optimize a strict < 0 check better than <= 0
			if (delta < 0)
				continue;

			// if not suspended, execute normally
			if (EXPECTED(exec->m_suspend == 0))
			{
				// precache the CPU timing information
				u64 attoseconds_per_cycle = exec->m_attoseconds_per_cycle;

				// compute how many cycles we want to execute, rounding up
				u32 ran = u64(delta) / attoseconds_per_cycle + 1;
				LOG("  %12.12s: t=%018lldas %018lldas = %dc; ", exec->device().tag(), exec->m_localtime.relative(), delta, ran);

				g_profiler.start(exec->m_profiler);

				// store the number of cycles we've requested in the executing
				// device
				// TODO: do we need to do this?
				exec->m_cycles_running = ran;

				// set the device's icount value to the number of cycles we want
				// the fact that we have a direct point to this is an artifact of
				// the original MAME design
				auto *icountptr = exec->m_icountptr;
				*icountptr = ran;

				// clear m_cycles_stolen, which gets updated if the timeslice
				// is aborted (due to synchronization or setting a new timer to
				// expire before the original timeslice end)
				exec->m_cycles_stolen = 0;

				// store a pointer to the executing device so that we know the
				// relevant active context
				m_executing_device = exec;

				// tell the debugger we're going to start executing; the funny math
				// below uses the relative target as attoseconds, but it may be
				// larger than the the max attoseconds allowed, so we add zero to
				// it, which forces a normalization
				if (Debugging)
					exec->debugger_start_cpu_hook(attotime(m_basetime.seconds(), target) + attotime::zero);

				// now run the device for the number of cycles
				exec->run();

				// tell the debugger we're done executing
				if (Debugging)
					exec->debugger_stop_cpu_hook();

				// now let's see how many cycles we actually ran; if the device's
				// icount is negative, then we ran more than requested (this is both
				// allowed and expected), so the subtract here typically will
				// increase ran
				assert(ran >= *icountptr);
				ran -= *icountptr;

				// if cycles were stolen (i.e., icount was artificially decremented)
				// then ran isn't actually correct, so remove the number of cycles
				// that we did that for
				assert(ran >= exec->m_cycles_stolen);
				ran -= exec->m_cycles_stolen;

				// time should never go backwards, nor should we ever attempt to
				// execute more than a full second (minimum quantum prevents that)
				assert(ran >= 0 && ran < exec->m_cycles_per_second);

				g_profiler.stop();

				// update the device's count of total cycles executed with the
				// true number of cycles
				exec->m_totalcycles += ran;

				// update the local time for the device so that it represents an
				// integral number of cycles
				attoseconds_t deltatime = attoseconds_per_cycle * ran;
				exec->m_localtime.add(deltatime);

				LOG(" ran %dc, %dc total", ran, s32(exec->m_totalcycles));

				// if the new local device time is less than our target, move the
				// target up, but not before the base
				if (exec->m_localtime.relative() < target)
				{
					target = std::max(exec->m_localtime.relative(), basetime);
					LOG(" (new target)");
				}
				LOG("\n");
			}

			// if suspended, eat cycles efficiently
			else if (exec->m_eatcycles)
			{
				// this is just the minimal logic from above to consume the cycles;
				// we don't check to see if the new local time is less than the
				// target because the calculation below guarantees it won't happen
				u32 ran = u64(delta) / u64(exec->m_attoseconds_per_cycle) + 1;
				exec->m_totalcycles += ran;
				exec->m_localtime.add(exec->m_attoseconds_per_cycle * ran);
			}
		}

		// set the executing device to null, which indicates that there is
		// no active context; this is used by machine.time() to return the
		// context-appropriate value
		m_executing_device = nullptr;

		// our final target becomes our new base time
		basetime = target;
	}

	// if basetime remained within the current second, we just have to
	// update the attoseconds part; however, it if did overflow, we need to
	// update all the basetime_relative structures in the system
	if (basetime < ATTOSECONDS_PER_SECOND)
		m_basetime.set_attoseconds(basetime);
	else
	{
		basetime -= ATTOSECONDS_PER_SECOND;
		assert(basetime < ATTOSECONDS_PER_SECOND);
		m_basetime.set_attoseconds(basetime);
		m_basetime.set_seconds(m_basetime.seconds() + 1);
		update_basetime();
	}

	// now that we've reached the expiration time of the first timer in the
	// queue, execute pending ones
	execute_timers();
}

// explicitly instatiate both versions of the scheduler
template void device_scheduler::timeslice<true>();
template void device_scheduler::timeslice<false>();


//-------------------------------------------------
//  abort_timeslice - abort execution for the
//  current timeslice
//-------------------------------------------------

void device_scheduler::abort_timeslice()
{
	if (m_executing_device != nullptr)
		m_executing_device->abort_timeslice();
}


//-------------------------------------------------
//  trigger - generate a global trigger
//-------------------------------------------------

void device_scheduler::trigger(int trigid, attotime const &after)
{
	// ensure we have a list of executing devices
	if (m_execute_list == nullptr)
		rebuild_execute_list();

	// if we have a non-zero time, schedule a timer
	if (after != attotime::zero)
		m_timed_trigger.call_after(after, trigid);

	// send the trigger to everyone who cares
	else
		for (device_execute_interface *exec = m_execute_list; exec != nullptr; exec = exec->m_nextexec)
			exec->trigger(trigid);
}


//-------------------------------------------------
//  boost_interleave - temporarily boosts the
//  interleave factor
//-------------------------------------------------

void device_scheduler::boost_interleave(attotime const &timeslice_time, attotime const &boost_duration)
{
	// ignore timeslices > 1 second
	if (timeslice_time.seconds() > 0)
		return;
	add_scheduling_quantum(timeslice_time, boost_duration);
}


//-------------------------------------------------
//  register_callback - register a timer
//  expired callback
//-------------------------------------------------

u32 device_scheduler::register_callback(timer_callback &callback)
{
	// look for duplicates and compute a unique id
	u32 index = 0;
	for (timer_callback *scan = m_registered_callbacks; scan != nullptr; scan = scan->m_next_registered)
		if (scan->unique_hash() == callback.unique_hash())
			index++;

	// now hook us in
	callback.m_next_registered = m_registered_callbacks;
	m_registered_callbacks = &callback;
	return index;
}


//-------------------------------------------------
//  deregister_callback - deregister a timer
//  expired callback
//-------------------------------------------------

void device_scheduler::deregister_callback(timer_callback &callback)
{
	for (timer_callback **nextptr = &m_registered_callbacks; *nextptr != nullptr; nextptr = &(*nextptr)->m_next_registered)
		if (*nextptr == &callback)
		{
			*nextptr = callback.m_next_registered;
			return;
		}
}


//-------------------------------------------------
//  timer_alloc - allocate a persistent timer
//  and return a pointer
//-------------------------------------------------

persistent_timer *device_scheduler::timer_alloc(timer_expired_delegate const &callback, void *ptr)
{
	// allocate a new persistent timer and save it in a vector
	m_allocated_persistents.push_back(std::make_unique<persistent_timer>());
	persistent_timer &timer = *m_allocated_persistents.back().get();

	// initialize the timer instance
	return &timer.init(*this, callback).set_ptr(ptr);
}


//-------------------------------------------------
//  timer_alloc - allocate a persistent device
//  timer and return a pointer
//-------------------------------------------------

persistent_timer *device_scheduler::timer_alloc(device_t &device, device_timer_id id, void *ptr)
{
	// allocate a new persistent timer and save it in a vector
	m_allocated_persistents.push_back(std::make_unique<device_persistent_timer>());
	device_persistent_timer &timer = static_cast<device_persistent_timer &>(*m_allocated_persistents.back().get());

	// initialize the timer instance
	return &timer.init(device, id, ptr);
}


//-------------------------------------------------
//  eat_all_cycles - eat a ton of cycles on all
//  CPUs to force a quick exit
//-------------------------------------------------

void device_scheduler::eat_all_cycles()
{
	for (device_execute_interface *exec = m_execute_list; exec != nullptr; exec = exec->m_nextexec)
		exec->eat_cycles(1000000000);
}


//-------------------------------------------------
//  presave - before creating a save state
//-------------------------------------------------

void device_scheduler::presave()
{
	int index = 0;

#if VERBOSE
	dump_timers();
#endif

	// copy in all the timer instance data to the save area
	for (timer_instance *timer = m_active_timers_head; timer != &m_active_timers_tail && index < MAX_SAVE_INSTANCES; timer = timer->next())
	{
		auto *persistent = timer->m_callback->persistent();
		if (persistent != nullptr)
			persistent->save(m_timer_save[index++]);
		else
			timer->save(m_timer_save[index++]);
	}

	// then copy in inactive persistent timers
	for (timer_callback *cb = m_registered_callbacks; cb != nullptr && index < MAX_SAVE_INSTANCES; cb = cb->m_next_registered)
		if (cb->persistent() != nullptr && !cb->persistent()->instance().active())
			cb->persistent()->save(m_timer_save[index++]);

	// zero out the remainder
	for ( ; index < MAX_SAVE_INSTANCES; index++)
	{
		auto &dest = m_timer_save[index];
		dest.start = attotime::zero;
		dest.expire = attotime::never;
		dest.period = attotime::never;
		dest.param[0] = dest.param[1] = dest.param[2] = 0;
		dest.hash = 0;
		dest.enabled = false;
	}

	// report the timer state after a log
	LOG("Prior to saving state:\n");
}


//-------------------------------------------------
//  postload - after loading a save state
//-------------------------------------------------

void device_scheduler::postload()
{
	// first discard or capture active timers
	while (m_active_timers_head != &m_active_timers_tail)
	{
		auto &prevhead = *m_active_timers_head;
		m_active_timers_head = prevhead.m_next;
		instance_reclaim(prevhead);
	}

	// now go through the restored save area and recreate all the timers
	for (int index = 0; index < MAX_SAVE_INSTANCES; index++)
	{
		// scan until we find a never-expiring timer
		auto &dest = m_timer_save[index];
		if (dest.expire.is_never())
			break;

		// first find a matching callback
		timer_callback *cb;
		for (cb = m_registered_callbacks; cb != nullptr; cb = cb->m_next_registered)
			if (cb->unique_hash() == dest.hash && cb->save_index() == dest.save_index)
				break;

		// if we can't find the timer, that's a concern (probably fatal)
		if (cb == nullptr)
		{
			osd_printf_warning("Unable to find matching callback for %08X\n", dest.hash);
			continue;
		}

		// if the callback is persistent, just configure the persistent timer
		auto *persistent = cb->persistent();
		if (persistent != nullptr)
			persistent->restore(dest, *cb);
		else
			instance_alloc().restore(dest, *cb);
	}

	// force a refresh of things that are lazily updated
	update_first_timer_expire();
	m_suspend_changes_pending = true;
	rebuild_execute_list();

	// report the timer state after a log
	LOG("After resetting/reordering timers:\n");
#if VERBOSE
	dump_timers();
#endif
}


//-------------------------------------------------
//  execute_timers - execute timers that are due
//-------------------------------------------------

inline void device_scheduler::execute_timers()
{
	LOG("execute_timers: new=%s head->expire=%s\n", m_basetime.as_string(PRECISION), m_first_timer_expire.absolute().as_string(PRECISION));

	// now process any timers that are overdue; due to our never-expiring dummy
	// instance, we don't need to check for nullptr on the head
	while (m_active_timers_head->m_expire <= m_basetime)
	{
		// if this is a one-shot timer, disable it now
		timer_instance &timer = *m_active_timers_head;
		m_active_timers_head = timer.m_next;
		timer.m_active = false;

		// set the global state of which callback we're in
		m_callback_timer = &timer;
		m_callback_timer_expire_time = timer.m_expire;

		// call the callback
		g_profiler.start(PROFILER_TIMER_CALLBACK);
		{
			if (timer.m_callback->device() != nullptr)
				LOG("execute_timers: timer device %s timer %d\n", timer.m_callback->device()->tag(), int(timer.param(2)));
			else
				LOG("execute_timers: timer callback %s\n", timer.m_callback->name());

			(*timer.m_callback)(timer);
		}
		g_profiler.stop();

		// reclaim the timer now that we're done with it
		instance_reclaim(timer);
	}

	// update the expiration time of the first timer
	update_first_timer_expire();

	// clear the callback timer global
	m_callback_timer = nullptr;
}


//-------------------------------------------------
//  update_basetime - update all the
//  basetime_relative times now that the basetime
//  has ticked over another second
//-------------------------------------------------

void device_scheduler::update_basetime()
{
	seconds_t base_seconds = m_basetime.seconds();

	// update execute devices
	for (device_execute_interface &exec : execute_interface_enumerator(machine().root_device()))
		exec.m_localtime.set_base_seconds(base_seconds);

	// move timers from future list into current list
	m_first_timer_expire.set_base_seconds(base_seconds);
}


//-------------------------------------------------
//  compute_perfect_interleave - compute the
//  "perfect" interleave interval
//-------------------------------------------------

void device_scheduler::compute_perfect_interleave()
{
	// ensure we have a list of executing devices
	if (m_execute_list == nullptr)
		rebuild_execute_list();

	// start with the first one
	device_execute_interface *first = m_execute_list;
	if (first != nullptr)
	{
		// start with a huge time factor and find the 2nd smallest cycle time
		attoseconds_t smallest = first->minimum_quantum();
		attoseconds_t perfect = ATTOSECONDS_PER_SECOND - 1;
		for (device_execute_interface *exec = first->m_nextexec; exec != nullptr; exec = exec->m_nextexec)
		{
			// find the 2nd smallest cycle interval
			attoseconds_t curquantum = exec->minimum_quantum();
			if (curquantum < smallest)
			{
				perfect = smallest;
				smallest = curquantum;
			}
			else if (curquantum < perfect)
				perfect = curquantum;
		}

		// if this is a new minimum quantum, apply it
		if (m_quantum_minimum != perfect)
		{
			// adjust all the actuals; this doesn't affect the current
			m_quantum_minimum = perfect;
			for (quantum_slot &quant : m_quantum_list)
				quant.m_actual = std::max(quant.m_requested, m_quantum_minimum);
		}
	}
}


//-------------------------------------------------
//  rebuild_execute_list - rebuild the list of
//  executing CPUs, moving suspended CPUs to the
//  end
//-------------------------------------------------

void device_scheduler::rebuild_execute_list()
{
	// if we haven't yet set a scheduling quantum, do it now
	if (m_quantum_list.empty())
	{
		// set the core scheduling quantum, ensuring it's no longer than 60Hz
		attotime min_quantum = machine().config().maximum_quantum(attotime::from_hz(60));

		// if the configuration specifies a device to make perfect, pick that as the minimum
		device_execute_interface *const exec(machine().config().perfect_quantum_device());
		if (exec)
			min_quantum = (std::min)(attotime(0, exec->minimum_quantum()), min_quantum);

		// inform the timer system of our decision
		add_scheduling_quantum(min_quantum, attotime::never);
	}

	// start with an empty list
	device_execute_interface **active_tailptr = &m_execute_list;
	*active_tailptr = nullptr;

	// also make an empty list of suspended devices
	device_execute_interface *suspend_list = nullptr;
	device_execute_interface **suspend_tailptr = &suspend_list;

	// iterate over all devices
	for (device_execute_interface &exec : execute_interface_enumerator(machine().root_device()))
	{
		// append to the appropriate list
		exec.m_nextexec = nullptr;
		if (exec.m_suspend == 0)
		{
			*active_tailptr = &exec;
			active_tailptr = &exec.m_nextexec;
		}
		else
		{
			*suspend_tailptr = &exec;
			suspend_tailptr = &exec.m_nextexec;
		}
	}

	// append the suspend list to the end of the active list
	*active_tailptr = suspend_list;
}


//-------------------------------------------------
//  apply_suspend_changes - applies suspend/resume
//  changes to all device_execute_interfaces
//-------------------------------------------------

inline void device_scheduler::apply_suspend_changes()
{
	u32 suspendchanged = 0;
	for (device_execute_interface *exec = m_execute_list; exec != nullptr; exec = exec->m_nextexec)
	{
		suspendchanged |= exec->m_suspend ^ exec->m_nextsuspend;
		exec->m_suspend = exec->m_nextsuspend;
		exec->m_nextsuspend &= ~SUSPEND_REASON_TIMESLICE;
		exec->m_eatcycles = exec->m_nexteatcycles;
	}

	// recompute the execute list if any CPUs changed their suspension state
	if (suspendchanged != 0)
		rebuild_execute_list();
	else
		m_suspend_changes_pending = false;
}


//-------------------------------------------------
//  add_scheduling_quantum - add a scheduling
//  quantum; the smallest active one is the one
//  that is in use
//-------------------------------------------------

void device_scheduler::add_scheduling_quantum(attotime const &quantum, attotime const &duration)
{
	assert(quantum.seconds() == 0);

	attotime curtime = time();
	attotime expire = curtime + duration;
	const attoseconds_t quantum_attos = quantum.attoseconds();

	// figure out where to insert ourselves, expiring any quanta that are out-of-date
	quantum_slot *insert_after = nullptr;
	quantum_slot *next;
	for (quantum_slot *quant = m_quantum_list.first(); quant != nullptr; quant = next)
	{
		// if this quantum is expired, nuke it
		next = quant->next();
		if (curtime >= quant->m_expire)
			m_quantum_allocator.reclaim(m_quantum_list.detach(*quant));

		// if this quantum is shorter than us, we need to be inserted afterwards
		else if (quant->m_requested <= quantum_attos)
			insert_after = quant;
	}

	// if we found an exact match, just take the maximum expiry time
	if (insert_after != nullptr && insert_after->m_requested == quantum_attos)
		insert_after->m_expire = std::max(insert_after->m_expire, expire);

	// otherwise, allocate a new quantum and insert it after the one we picked
	else
	{
		quantum_slot &quant = *m_quantum_allocator.alloc();
		quant.m_requested = quantum_attos;
		quant.m_actual = std::max(quantum_attos, m_quantum_minimum);
		quant.m_expire = expire;
		m_quantum_list.insert_after(quant, insert_after);
	}
}


//-------------------------------------------------
//  instance_alloc - allocate memory for a new
//  timer instance, either by reclaiming a
//  freed one, or allocating memory for a new one
//-------------------------------------------------

timer_instance &device_scheduler::instance_alloc()
{
	// attempt to rescue one off the free list
	timer_instance *instance = m_free_timers;
	if (instance != nullptr)
	{
		m_free_timers = instance->m_next;
		return *instance;
	}

	// if none, allocate a new one
	m_allocated_instances.push_back(std::make_unique<timer_instance>());
	return *m_allocated_instances.back().get();
}


//-------------------------------------------------
//  instance_reclaim - reclaim memory for a
//  timer instance by adding it to the free list
//-------------------------------------------------

inline void device_scheduler::instance_reclaim(timer_instance &timer)
{
	// don't reclaim persistent instances because they are part of the
	// persistent_timer object
	if (timer.m_callback->persistent() != nullptr)
		return;

	// reclaimed instances go back on the free list
	timer.m_next = m_free_timers;
	m_free_timers = &timer;
}


//-------------------------------------------------
//  instance_insert - insert a timer instance at
//  the the appropriate spot in the active
//  timer queue
//-------------------------------------------------

inline timer_instance &device_scheduler::instance_insert(timer_instance &timer)
{
	// special case insert at start; we always have at least a dummy never-
	// expiring timer in the list, so no need to check for nullptr
	if (timer.m_expire < m_active_timers_head->m_expire)
	{
		// no previous, next is the head
		timer.m_prev = nullptr;
		timer.m_next = m_active_timers_head;

		// link the old head as the previous and make us head
		m_active_timers_head = m_active_timers_head->m_prev = &timer;

		// since the head changed, the time of the first expirations changed
		update_first_timer_expire();
		abort_timeslice();
		return timer;
	}

	// special case insert at end; since we're not the head, we can be sure
	// that there's at least one timer before the permanent tail
	if (timer.m_expire >= m_active_timers_tail.m_prev->m_expire)
	{
		// hook us up in front of the tail
		timer.m_prev = m_active_timers_tail.m_prev;
		timer.m_next = &m_active_timers_tail;
		m_active_timers_tail.m_prev = m_active_timers_tail.m_prev->m_next = &timer;

		// no need to recompute if changing a later timer
		return timer;
	}

	// scan to find out where we go
	for (timer_instance *scan = m_active_timers_head->m_next; scan != nullptr; scan = scan->m_next)
		if (timer.m_expire < scan->m_expire)
		{
			timer.m_prev = scan->m_prev;
			timer.m_next = scan;
			scan->m_prev = scan->m_prev->m_next = &timer;

			// no need to recompute if changing a later timer
			return timer;
		}

	// should never get here
	return timer;
}


//-------------------------------------------------
//  instance_remove - remove a timer from the
//  active timer queue
//-------------------------------------------------

inline timer_instance &device_scheduler::instance_remove(timer_instance &timer)
{
	// link the previous to us; if no previous, we're the head
	if (timer.m_prev != nullptr)
		timer.m_prev->m_next = timer.m_next;
	else
	{
		m_active_timers_head = timer.m_next;
		update_first_timer_expire();
	}

	// link the next to us; we can't be the tail, so presume next is non-null
	timer.m_next->m_prev = timer.m_prev;

	// return the timer back for chaining
	return timer;
}


//-------------------------------------------------
//  empty_timer - empty callback stub when
//  timers provide nothing
//-------------------------------------------------

void device_scheduler::empty_timer(timer_instance const &timer)
{
}


//-------------------------------------------------
//  timed_trigger - generate a trigger after a
//  given amount of time
//-------------------------------------------------

void device_scheduler::timed_trigger(timer_instance const &timer)
{
	trigger(timer.param());
}


//-------------------------------------------------
//  dump_timers - dump the current timer state
//-------------------------------------------------

void device_scheduler::dump_timers() const
{
	machine().logerror("=============================================\n");
	machine().logerror("Timer Dump: Time = %15s\n", time().as_string(PRECISION));
	for (timer_instance *timer = m_active_timers_head; timer != nullptr; timer = timer->next())
		timer->dump();
	machine().logerror("=============================================\n");
}

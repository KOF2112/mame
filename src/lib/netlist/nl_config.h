// license:GPL-2.0+
// copyright-holders:Couriersud
/*!
 *
 * \file nl_config.h
 *
 */

#ifndef NLCONFIG_H_
#define NLCONFIG_H_

#include "plib/pconfig.h"

//============================================================
//  SETUP
//============================================================


//============================================================
//  GENERAL
//============================================================

/*! Make use of a memory pool for performance related objects.
 *
 * Set to 1 to compile netlist with memory allocations from a
 * linear memory pool. This is based of the assumption that
 * due to enhanced locality there will be less cache misses.
 * Your mileage may vary.
 *
 */
#ifndef NL_USE_MEMPOOL
#define NL_USE_MEMPOOL               (1)
#endif

/*! Enable queue statistics.
 *
 * Queue statistics come at a performance cost. Although
 * the cost is low, we disable them here since they are
 * only needed during development.
 *
 */
#ifndef NL_USE_QUEUE_STATS
#define NL_USE_QUEUE_STATS             (0)
#endif

/*! Store input values in logic_terminal_t.
 *
 * Set to 1 to store values in logic_terminal_t instead of
 * accessing them indirectly by pointer from logic_net_t.
 * This approach is stricter and should identify bugs in
 * the netlist core faster.
 * By default it is disabled since it is not as fast as
 * the default approach. It is up to 10% slower.
 *
 */
#ifndef NL_USE_COPY_INSTEAD_OF_REFERENCE
#define NL_USE_COPY_INSTEAD_OF_REFERENCE (0)
#endif

/*
 * FIXME: Using truthtable is a lot slower than the explicit device
 *        in breakout. Performance drops by 20%. This can be fixed by
 *        setting param USE_DEACTIVATE for the device.
 */

#ifndef NL_USE_TRUTHTABLE_7448
#define NL_USE_TRUTHTABLE_7448 (0)
#endif

/*
 * FIXME: The truthtable implementation of 74107 (JK-Flipflop)
 *        is included for educational purposes to demonstrate how
 *        to implement state holding devices as truthtables.
 *        It will completely nuke performance for pong.
 */

#ifndef NL_USE_TRUTHTABLE_74107
#define NL_USE_TRUTHTABLE_74107 (0)
#endif

//============================================================
//  DEBUGGING
//============================================================

#ifndef NL_DEBUG
#define NL_DEBUG                    (false)
//#define NL_DEBUG                    (true)
#endif

//============================================================
// Time resolution
//============================================================

// Use nano-second resolution - Sufficient for now

static constexpr const auto NETLIST_INTERNAL_RES = 1000000000;
static constexpr const auto NETLIST_CLOCK = NETLIST_INTERNAL_RES;

//============================================================
// Floating point types used
//
// Don't change this. Simple analog circuits like pong
// work with float. Kidniki just doesn't work at all
// due to numeric issues
//============================================================

using nl_fptype = double;

namespace netlist
{
	/*! Specific constants depending on floating type
	 *
	 *  @tparam FT floating point type: double/float
	 */
	template <typename FT>
	struct fp_constants
	{ };

	/*! Specific constants for long double floating point type
	 */
	template <>
	struct fp_constants<long double>
	{
		static inline constexpr long double DIODE_MAXDIFF() noexcept { return  1e100L; }
		static inline constexpr long double DIODE_MAXVOLT() noexcept { return  300.0L; }

		static inline constexpr long double TIMESTEP_MAXDIFF() noexcept { return  1e100L; }
		static inline constexpr long double TIMESTEP_MINDIV() noexcept { return  1e-60L; }

		static inline constexpr const char * name() noexcept { return "long double"; }
		static inline constexpr const char * suffix() noexcept { return "L"; }
	};

	/*! Specific constants for double floating point type
	 */
	template <>
	struct fp_constants<double>
	{
		static inline constexpr double DIODE_MAXDIFF() noexcept { return  1e100; }
		static inline constexpr double DIODE_MAXVOLT() noexcept { return  300.0; }

		static inline constexpr double TIMESTEP_MAXDIFF() noexcept { return  1e100; }
		static inline constexpr double TIMESTEP_MINDIV() noexcept { return  1e-60; }

		static inline constexpr const char * name() noexcept { return "double"; }
		static inline constexpr const char * suffix() noexcept { return ""; }
	};

	/*! Specific constants for float floating point type
	 */
	template <>
	struct fp_constants<float>
	{
		static inline constexpr float DIODE_MAXDIFF() noexcept { return  1e2; }
		static inline constexpr float DIODE_MAXVOLT() noexcept { return  20.0; }

		static inline constexpr float TIMESTEP_MAXDIFF() noexcept { return  1e30f; }
		static inline constexpr float TIMESTEP_MINDIV() noexcept { return  1e-8f; }

		static inline constexpr const char * name() noexcept { return "float"; }
		static inline constexpr const char * suffix() noexcept { return "f"; }
	};
} // namespace netlist

#endif /* NLCONFIG_H_ */

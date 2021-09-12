// license:BSD-3-Clause
// copyright-holders:Vas Crabb
#ifndef MAME_MACHINE_KEYBOARD_IPP
#define MAME_MACHINE_KEYBOARD_IPP

#pragma once

// Work around for MSVC warning that identifies some of these as unused
#ifdef _MSC_VER
#pragma warning ( disable : 4505 )
#endif

#include "keyboard.h"

#include <numeric>


template <uint8_t ROW_COUNT>
template <typename... T>
device_matrix_keyboard_interface<ROW_COUNT>::device_matrix_keyboard_interface(machine_config const &mconfig, device_t &device, T &&... tags)
	: device_interface(device, "matrix_keyboard")
	, m_key_rows{ { device, std::forward<T>(tags) }... }
	, m_next_row(0U)
	, m_processing(0U)
	, m_typematic_row(0U)
	, m_typematic_column(0U)
{
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::interface_pre_start()
{
	m_scan_timer.init(*this, FUNC(device_matrix_keyboard_interface<ROW_COUNT>::scan_row));
	m_typematic_timer.init(*this, FUNC(device_matrix_keyboard_interface<ROW_COUNT>::typematic));
	reset_key_state();
	typematic_stop();
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::interface_post_start()
{
	device().save_item(NAME(m_key_states));
	device().save_item(NAME(m_next_row));
	device().save_item(NAME(m_processing));
	device().save_item(NAME(m_typematic_row));
	device().save_item(NAME(m_typematic_column));
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::start_processing(const attotime &period)
{
	m_processing = 1U;
	m_scan_timer.adjust_periodic(period);
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::stop_processing()
{
	m_scan_timer.reset();
	m_processing = 0U;
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::reset_key_state()
{
	std::fill(std::begin(m_key_states), std::end(m_key_states), ioport_value(0U));
	m_next_row = 0U;
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::typematic_start(uint8_t row, uint8_t column, attotime const &delay, attotime const &interval)
{
	m_typematic_row = row;
	m_typematic_column = column;
	m_typematic_timer.adjust(delay, 0, interval);
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::typematic_restart(attotime const &delay, attotime const &interval)
{
	if ((m_typematic_row != uint8_t(~0U)) || (m_typematic_column != uint8_t(~0U)))
		m_typematic_timer.adjust(delay, 0, interval);
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::typematic_stop()
{
	m_typematic_row = uint8_t(~0U);
	m_typematic_column = uint8_t(~0U);
	m_typematic_timer.reset();
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::scan_row(timer_instance const &timer)
{
	assert(m_next_row < std::size(m_key_rows));
	assert(m_next_row < std::size(m_key_states));

	will_scan_row(m_next_row);

	ioport_value &state(m_key_states[m_next_row]);
	ioport_value const keys(m_key_rows[m_next_row]->read());
	ioport_value const change(state ^ keys);

	ioport_value mask(1U);
	for (uint8_t column = 0U; m_processing && (state != keys); ++column, mask <<= 1)
	{
		if (change & mask)
		{
			state ^= mask;
			if (keys & mask)
				key_make(m_next_row, column);
			else
				key_break(m_next_row, column);
		}
	}

	m_next_row = (m_next_row + 1) % std::size(m_key_rows);
	if (m_next_row == 0)
		scan_complete();
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::typematic(timer_instance const &timer)
{
	assert((m_typematic_row != uint8_t(~0U)) || (m_typematic_column != uint8_t(~0U)));
	key_repeat(m_typematic_row, m_typematic_column);
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::key_repeat(uint8_t row, uint8_t column)
{
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::key_break(uint8_t row, uint8_t column)
{
	if (typematic_is(row, column))
		typematic_stop();
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::will_scan_row(uint8_t row)
{
}


template <uint8_t ROW_COUNT>
void device_matrix_keyboard_interface<ROW_COUNT>::scan_complete()
{
}


template <uint8_t ROW_COUNT>
bool device_matrix_keyboard_interface<ROW_COUNT>::are_all_keys_up()
{
	return 0U == std::accumulate(
			std::begin(m_key_rows),
			std::end(m_key_rows),
			ioport_value(0),
			[] (ioport_value a, auto const &b) { return a | b->read(); });
}

#endif // MAME_MACHINE_KEYBOARD_IPP

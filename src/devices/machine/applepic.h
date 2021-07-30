// license:BSD-3-Clause
// copyright-holders:AJR

#ifndef MAME_DEVICES_MACHINE_APPLEPIC_H
#define MAME_DEVICES_MACHINE_APPLEPIC_H

#pragma once

#include "cpu/m6502/r65c02.h"

//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// ======================> applepic_device

class applepic_device : public device_t
{
public:
	// construction/destruction
	applepic_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	// callback configuration
	auto prd_callback() { return m_prd_callback.bind(); }
	auto pwr_callback() { return m_pwr_callback.bind(); }
	auto hint_callback() { return m_hint_callback.bind(); }
	auto gpout0_callback() { return m_gpout_callback[0].bind(); }
	auto gpout1_callback() { return m_gpout_callback[1].bind(); }

	// host interface
	u8 host_r(offs_t offset);
	void host_w(offs_t offset, u8 data);

	// peripheral device requests
	DECLARE_WRITE_LINE_MEMBER(pint_w);
	DECLARE_WRITE_LINE_MEMBER(reqa_w);
	DECLARE_WRITE_LINE_MEMBER(reqb_w);

protected:
	// device-level overrides
	virtual void device_add_mconfig(machine_config &config) override;
	virtual void device_resolve_objects() override;
	virtual void device_start() override;
	virtual void device_reset() override;

private:
	static const std::string_view s_interrupt_names[8];

	struct dma_channel
	{
		u8 control;
		u16 map;
		u16 tc;
	};

	u8 timer_r(offs_t offset);
	void timer_w(offs_t offset, u8 data);
	void reset_timer_interrupt();
	u8 dma_channel_r(offs_t offset);
	void dma_channel_w(offs_t offset, u8 data);
	u8 scc_control_r();
	void scc_control_w(u8 data);
	u8 io_control_r();
	void io_control_w(u8 data);
	u8 timer_dpll_control_r();
	void timer_dpll_control_w(u8 data);
	u8 int_mask_r();
	void int_mask_w(u8 data);
	u8 int_reg_r();
	void int_reg_w(u8 data);
	void set_interrupt(int which);
	void reset_interrupt(int which);
	u8 host_reg_r();
	void host_reg_w(u8 data);
	u8 device_reg_r(offs_t offset);
	void device_reg_w(offs_t offset, u8 data);

	void internal_map(address_map &map);

	// internal CPU
	required_device<r65c02_device> m_iopcpu;

	// callbacks for peripheral and host
	devcb_read8 m_prd_callback;
	devcb_write8 m_pwr_callback;
	devcb_write_line m_hint_callback;
	devcb_write_line::array<2> m_gpout_callback;

	// internal state
	u16 m_ram_address;
	u8 m_status_reg;
	u16 m_timer_counter;
	u16 m_timer_latch;
	dma_channel m_dma_channel[2];
	u8 m_scc_control;
	u8 m_io_control;
	u8 m_timer_dpll_control;
	u8 m_int_mask;
	u8 m_int_reg;
};

// device type declaration
DECLARE_DEVICE_TYPE(APPLEPIC, applepic_device)

#endif // MAME_DEVICES_MACHINE_APPLEPIC_H

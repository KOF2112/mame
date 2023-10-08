// license:BSD-3-Clause
// copyright-holders:Miodrag Milanovic
#ifndef MAME_FORMATS_TD0_DSK_H
#define MAME_FORMATS_TD0_DSK_H

#pragma once

#include "flopimg.h"


class td0_format : public floppy_image_format_t
{
public:
	td0_format();

	virtual int identify(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants) const override;
	virtual bool load(util::random_read &io, uint32_t form_factor, const std::vector<uint32_t> &variants, floppy_image &image) const override;

	virtual const char *name() const noexcept override;
	virtual const char *description() const noexcept override;
	virtual const char *extensions() const noexcept override;
	virtual bool supports_save() const noexcept override;
};

extern const td0_format FLOPPY_TD0_FORMAT;

#endif // MAME_FORMATS_TD0_DSK_H

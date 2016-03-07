/*
 * Copyright (C) 2016 Free Electrons
 * Copyright (C) 2016 NextThing Co
 *
 * Boris Brezillon <boris.brezillon@free-electrons.com>
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef _SUN4I_TV_H_
#define _SUN4I_TV_H_

struct sun4i_tv_color_gains {
	u16	cb;
	u16	cr;
};

struct sun4i_tv_burst_levels {
	u16	cb;
	u16	cr;
};

struct sun4i_tv_video_levels {
	u16	black;
	u16	blank;
};

struct sun4i_tv_resync_parameters {
	bool	field;
	u16	line;
	u16	pixel;
};

struct sun4i_tv_mode {
	char		*name;

	u32		mode;
	u32		chroma_freq;
	u16		back_porch;
	u16		front_porch;
	u16		line_number;
	u16		vblank_level;

	u32		hdisplay;
	u16		hfront_porch;
	u16		hsync_len;
	u16		hback_porch;

	u32		vdisplay;
	u16		vfront_porch;
	u16		vsync_len;
	u16		vback_porch;

	bool		yc_en;
	bool		dac3_en;
	bool		dac_bit25_en;

	struct sun4i_tv_color_gains		*color_gains;
	struct sun4i_tv_burst_levels		*burst_levels;
	struct sun4i_tv_video_levels		*video_levels;
	struct sun4i_tv_resync_parameters	*resync_params;
};


#endif /* _SUN4I_TV_H_ */

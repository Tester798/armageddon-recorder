/* Armageddon Recorder - Capture code
 * Copyright (C) 2012 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef AREC_CAPTURE_HPP
#define AREC_CAPTURE_HPP

#include <windows.h>
#include <string>

#include "audio.hpp"

#define FRAME_PREFIX "arec_"
#define SYNC_FRAMES 25

#define WM_WAEXIT WM_USER+1
#define WM_PUSHLOG WM_USER+2

struct wa_capture {
	std::string replay_path;
	std::string capture_path;
	
	unsigned int frame_rate;
	
	HANDLE worker_thread;
	
	char *worms_cmdline;
	HANDLE worms_process;
	
	bool using_rec_a;
	audio_recorder *audio_rec_a, *audio_rec_b;
	HANDLE audio_event;
	
	wav_writer *wav_out;
	unsigned int next_sync;
	
	HANDLE capture_monitor;
	HANDLE force_exit;
	
	wa_capture(const std::string &replay, unsigned int width, unsigned int height, unsigned int fps, const std::string &start, const std::string &end);
	~wa_capture();
	
	void worker_main();
	
	bool frame_exists(unsigned int frame);
	unsigned int count_frames();
	
	void flush_audio(audio_recorder *rec);
};

#endif /* !AREC_CAPTURE_HPP */

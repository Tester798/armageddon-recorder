/* Armageddon Recorder - Encoder stuff
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

#include <windows.h>
#include <assert.h>

#include "encode.hpp"
#include "capture.hpp"
#include "ui.hpp"

const ffmpeg_format video_formats[] = {
	{ "None", NULL, NULL },
	{ "H264", "libx264", "-crf 21 -preset veryfast" },
	{ NULL }
};

const ffmpeg_format audio_formats[] = {
	{ "AAC", "aac", "-b:a 128k" },
	{ NULL }
};

const container_format container_formats[] = {
	{
		"mp4",
		"MP4",
		(const char*[]){ "libx264", NULL },
		(const char*[]){ "aac", NULL }
	},
	{ NULL }
};

/* Get the index of a named format within an ffmpeg_format array.
 * Returns -1 if the named format was not found.
*/
int get_ffmpeg_index(const ffmpeg_format formats[], const std::string &name)
{
	for(int i = 0; formats[i].name; i++)
	{
		if(name == formats[i].name)
		{
			return i;
		}
	}
	
	return -1;
}

static bool test_codec(const char *codecs[], const char *codec)
{
	for(int i = 0; codecs[i]; i++)
	{
		if(strcmp(codecs[i], codec) == 0)
		{
			return true;
		}
	}
	
	return false;
}

std::vector<int> get_valid_containers(int video_format, int audio_format)
{
	std::vector<int> ret;
	
	const char *video_codec = video_formats[video_format].codec;
	const char *audio_codec = audio_formats[audio_format].codec;
	
	for(int i = 0; container_formats[i].ext; i++)
	{
		const char **vc_ok = container_formats[i].video_formats;
		const char **ac_ok = container_formats[i].audio_formats;
		
		if(test_codec(vc_ok, video_codec) && test_codec(ac_ok, audio_codec))
		{
			ret.push_back(i);
		}
	}
	
	return ret;
}

static void append_codec(std::string &cmdline, const char *ct, const ffmpeg_format &format)
{
	cmdline.append(ct);
	cmdline.append(format.codec);
	
	if(format.extra)
	{
		cmdline.append(" ");
		cmdline.append(format.extra);
	}
}

std::string ffmpeg_cmdline()
{
	std::string frames_in = escape_filename(config.capture_dir + "\\" + FRAME_PREFIX + "%06d.png");
	std::string audio_in  = escape_filename(config.capture_dir + "\\" + FRAME_PREFIX + "audio.wav");
	std::string video_out = escape_filename(config.video_file);
	
	std::string cmdline = "ffmpeg.exe -threads " + to_string(config.max_enc_threads) + " -y -r " + to_string(config.frame_rate) + " -i \"" + frames_in + "\"";
	
	cmdline.append(std::string(" -i \"") + audio_in + "\"");
	
	append_codec(cmdline, " -c:v ", video_formats[config.video_format]);
	append_codec(cmdline, " -c:a ", audio_formats[config.audio_format]);
	
	cmdline.append(std::string(" -pix_fmt yuv420p \"") + video_out + "\"");
	
	return cmdline;
}

static char *ffmpeg_cmdline_buf = NULL;

static HANDLE ffmpeg_proc    = NULL;
static HANDLE ffmpeg_watcher = NULL;

/* Wait for the ffmpeg process to exit and post WM_ENC_EXIT to the progress
 * dialog when it does.
*/
static WINAPI DWORD ffmpeg_watcher_main(LPVOID lpParameter)
{
	WaitForSingleObject(ffmpeg_proc, INFINITE);
	
	DWORD exit_code;
	GetExitCodeProcess(ffmpeg_proc, &exit_code);
	
	PostMessage(progress_dialog, WM_ENC_EXIT, (WPARAM)(exit_code), 0);
	
	return 0;
}

/* Cleanup from any previous ffmpeg_run() call and then run ffmpeg. */
bool ffmpeg_run()
{
	ffmpeg_cleanup();
	
	std::string cmdline = ffmpeg_cmdline();
	
	ffmpeg_cmdline_buf = new char[cmdline.length() + 1];
	strcpy(ffmpeg_cmdline_buf, cmdline.c_str());
	
	STARTUPINFO sinfo;
	memset(&sinfo, 0, sizeof(sinfo));
	sinfo.cb = sizeof(sinfo);
	
	PROCESS_INFORMATION pinfo;
	
	if(!CreateProcess(NULL, ffmpeg_cmdline_buf, NULL, NULL, FALSE, 0, NULL, NULL, &sinfo, &pinfo))
	{
		log_push(std::string("Cannot run ffmpeg.exe: ") + w32_error(GetLastError()) + "\r\n");
		return false;
	}
	
	ffmpeg_proc = pinfo.hProcess;
	CloseHandle(pinfo.hThread);
	
	assert((ffmpeg_watcher = CreateThread(NULL, 0, &ffmpeg_watcher_main, NULL, 0, NULL)));
	
	return true;
}

/* Terminate any running ffmpeg and release any associated memory. */
void ffmpeg_cleanup()
{
	if(ffmpeg_watcher)
	{
		TerminateThread(ffmpeg_watcher, 1);
		
		CloseHandle(ffmpeg_watcher);
		ffmpeg_watcher = NULL;
	}
	
	if(ffmpeg_proc)
	{
		TerminateProcess(ffmpeg_proc, 1);
		
		CloseHandle(ffmpeg_proc);
		ffmpeg_proc = NULL;
	}
	
	delete ffmpeg_cmdline_buf;
	ffmpeg_cmdline_buf = NULL;
}

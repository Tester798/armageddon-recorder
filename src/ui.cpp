/* Armageddon Recorder - UI code
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
#include <windowsx.h>
#include <commctrl.h>
#include <assert.h>

#include "main.hpp"
#include "ui.hpp"
#include "capture.hpp"
#include "resource.h"
#include "encode.hpp"
#include "audio.hpp"

const std::string currentTime() {
	time_t now = time(0);
	struct tm tstruct;
	char buf[100];
	tstruct = *localtime(&now);
	strftime(buf, sizeof(buf), "%X", &tstruct);
	return buf;
}

HWND progress_dialog = NULL;

std::string get_window_string(HWND hwnd)
{
	int len = GetWindowTextLength(hwnd);
	
	if(len > 0)
	{
		char *buf = new char[len + 1];
		
		GetWindowText(hwnd, buf, len + 1);
		
		std::string text = buf;
		
		delete buf;
		
		return text;
	}
	else{
		return "";
	}
}

/* Read an integer value from a window.
 * Throws bad_input if the window contains an invalid or out-of-range integer.
*/
int get_window_int(HWND window, int min, int max)
{
	std::string s = get_window_string(window);
	
	char *endptr;
	int value = strtol(s.c_str(), &endptr, 10);
	
	if(s.empty() || endptr[0] != '\0' || value < min || value > max)
	{
		throw bad_input();
	}
	
	return value;
}

double get_window_double(HWND window) {
	std::string s = get_window_string(window);
	
	if(s.empty() || strspn(s.c_str(), "1234567890.") != s.length()) {
		return -1;
	}
	
	return atof(s.c_str());
}

bool checkbox_get(HWND hwnd)
{
	return (Button_GetCheck(hwnd) == BST_CHECKED);
}

void checkbox_set(HWND hwnd, bool checked)
{
	Button_SetCheck(hwnd, checked ? BST_CHECKED : BST_UNCHECKED);
}

/* Set the enabled state of a menu item. */
void menu_item_enable(HMENU menu, UINT item, bool enable)
{
	MENUITEMINFO info;
	
	info.cbSize = sizeof(info);
	info.fMask  = MIIM_STATE;
	
	GetMenuItemInfo(menu, item, FALSE, &info);
	
	if(enable)
	{
		info.fState &= ~MFS_DISABLED;
	}
	else{
		info.fState |= MFS_DISABLED;
	}
	
	SetMenuItemInfo(menu, item, FALSE, &info);
}

/* Returns the checked state of a menu item. */
bool menu_item_get(HMENU menu, UINT item)
{
	MENUITEMINFO info;
	
	info.cbSize = sizeof(info);
	info.fMask  = MIIM_STATE;
	
	GetMenuItemInfo(menu, item, FALSE, &info);
	
	return !!(info.fState & MFS_CHECKED);
}

/* Set the checked state of a menu item. */
void menu_item_set(HMENU menu, UINT item, bool state)
{
	MENUITEMINFO info;
	
	info.cbSize = sizeof(info);
	info.fMask  = MIIM_STATE;
	
	GetMenuItemInfo(menu, item, FALSE, &info);
	
	if(state)
	{
		info.fState |= MFS_CHECKED;
	}
	else{
		info.fState &= ~MFS_CHECKED;
	}
	
	SetMenuItemInfo(menu, item, FALSE, &info);
}

/* Invert the checked state of a menu item and return the new state. */
bool menu_item_toggle(HMENU menu, UINT item)
{
	MENUITEMINFO info;
	
	info.cbSize = sizeof(info);
	info.fMask  = MIIM_STATE;
	
	GetMenuItemInfo(menu, item, FALSE, &info);
	
	info.fState ^= MFS_CHECKED;
	
	SetMenuItemInfo(menu, item, FALSE, &info);
	
	return !!(info.fState & MFS_CHECKED);
}

/* Initialise a volume control pair.
 * Value should be 0 to 100.
*/
void volume_init(HWND slider, HWND edit, int value)
{
	SendMessage(slider, TBM_SETRANGE, (WPARAM)(FALSE), MAKELPARAM(0, 100));
	SendMessage(slider, TBM_SETTICFREQ, (WPARAM)(5), (LPARAM)(0));
	
	SendMessage(slider, TBM_SETPOS, (WPARAM)(TRUE), (LPARAM)(value));
	
	volume_on_slider(slider, edit);
}

/* Update the edit part of a volume control pair.
 * Call whenever the slider changes.
*/
void volume_on_slider(HWND slider, HWND edit)
{
	int pos = SendMessage(slider, TBM_GETPOS, (WPARAM)(0), (LPARAM)(0));
	
	SetWindowText(edit, std::string(to_string(pos) + "%").c_str());
}

static DWORD WINAPI audio_gen_thread(LPVOID lpParameter)
{
	if(make_output_wav())
	{
		PostMessage(progress_dialog, WM_AUDIO_DONE, 0, 0);
	}
	else{
		PostMessage(progress_dialog, WM_ABORTED, 0, 0);
	}
	
	return 0;
}

enum capture_state
{
	s_init,
	s_capture,
	s_audio_gen,
	s_encode,
	s_done
};

INT_PTR CALLBACK prog_dproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	static int return_code;
	
	static capture_state state = s_init;
	
	switch(msg) {
		case WM_INITDIALOG:
		{
			progress_dialog = hwnd;
			return_code = 0;
			
			SendMessage(hwnd, WM_SETICON, 0, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(ICON16)));
			SendMessage(hwnd, WM_SETICON, 1, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(ICON32)));
			
			PostMessage(hwnd, WM_BEGIN, 0, 0);
			
			return TRUE;
		}
		
		case WM_CLOSE:
		{
			if(state == s_done)
			{
				ffmpeg_cleanup();
				
				progress_dialog = NULL;
				
				EndDialog(hwnd, return_code);
			}
			
			return TRUE;
		}
		
		case WM_BEGIN:
		{
			if(start_capture())
			{
				state = s_capture;
			}
			else{
				PostMessage(hwnd, WM_ABORTED, 0, 0);
			}
			
			return TRUE;
		}
		
		case WM_WAEXIT:
		{
			log_push("WA exited with status " + to_string((DWORD)(wp)) + "\r\n");
			
			finish_capture();
			
			if(config.audio_format > 0)
			{
				log_push("Creating audio file...\r\n");
				
				HANDLE at = CreateThread(NULL, 0, &audio_gen_thread, NULL, 0, NULL);
				assert(at);
				
				CloseHandle(at);
			
				state = s_audio_gen;
			}
			else{
				PostMessage(hwnd, WM_AUDIO_DONE, 0, 0);
			}
			
			return TRUE;
		}
		
		case WM_AUDIO_DONE:
		{
			if(config.video_format > 0 || config.audio_format > 0)
			{
				log_push("Starting encoder...\r\n");
				
				if(ffmpeg_run())
				{
					state = s_encode;
				}
				else{
					PostMessage(hwnd, WM_ABORTED, 0, 0);
				}
			}
			else{
				PostMessage(hwnd, WM_ENC_EXIT, 0, 0);
			}
			
			return TRUE;
		}
		
		case WM_ENC_EXIT:
		{
			/* The encoder process has exited */
			
			ffmpeg_cleanup();
			
			if(config.do_cleanup)
			{
				log_push("Cleaning up...\r\n");
				delete_capture();
			}
			
			log_push("Complete!\r\n");
			
			EnableWindow(GetDlgItem(hwnd, IDOK), TRUE);
			
			state = s_done;
			
			return TRUE;
		}
		
		case WM_COMMAND:
		{
			if(HIWORD(wp) == BN_CLICKED && LOWORD(wp) == IDOK) {
				return_code = 1;
				PostMessage(hwnd, WM_CLOSE, 0, 0);
			}
			
			return TRUE;
		}
		
		case WM_PUSHLOG:
		{
			/* Append text to the log window
			 * WPARAM: const std::string* containing text to append
			*/
			
			HWND log = GetDlgItem(hwnd, LOG_EDIT);
			
			SetWindowText(log, std::string(get_window_string(log) + *(const std::string*)(wp)).c_str());
			
			Edit_Scroll(log, 500, -500);
			
			return TRUE;
		}
		
		case WM_ABORTED:
		{
			finish_capture();
			ffmpeg_cleanup();
			
			MessageBox(hwnd, "Capture aborted due to error.\nCheck log window for details.", NULL, MB_OK | MB_ICONERROR);
			
			state = s_done;
			
			return TRUE;
		}
		
		default:
			break;
	}
	
	return FALSE;
}

/* Append text to the progress dialog log window */
void log_push(const std::string &msg) {
	const std::string logmsg = currentTime() + " " + msg;
	SendMessage(progress_dialog, WM_PUSHLOG, (WPARAM)&logmsg, 0);
}

void show_error(const std::string &msg)
{
	if(progress_dialog)
	{
		log_push(msg + "\r\n");
	}
	else{
		MessageBox(NULL, msg.c_str(), NULL, MB_OK | MB_ICONERROR | MB_TASKMODAL);
	}
}

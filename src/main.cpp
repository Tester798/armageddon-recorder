/* Armageddon Recorder
 * Copyright (C) 2012-2014 Daniel Collins <solemnwarning@solemnwarning.net>
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
#include <objbase.h>
#include <shlobj.h>
#include <vfw.h>
#include <list>
#include <string>
#include <iostream>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <memory>
#include <ctype.h>

#include "resource.h"
#include "audio.hpp"
#include "reg.hpp"
#include "encode.hpp"
#include "capture.hpp"
#include "main.hpp"
#include "ui.hpp"

/* I thought we were past missing things, MinGW... */
#define BIF_NONEWFOLDERBUTTON 0x00000200
typedef LPITEMIDLIST PIDLIST_ABSOLUTE;

const char *detail_levels[] = {
	"0 - No background",
	"1 - Extra waves",
	"2 - Gradient background, more waves",
	"3 - Smoother gradient background",
	"4 - Flying debris in background",
	"5 - Images in background",
	NULL
};

const char *chat_levels[] = {
	"Show nothing",
	"Show telephone",
	"Show messages",
	NULL
};

arec_config config;

std::string wa_path, wa_exe_name, wa_exe_path;
bool wormkit_exe;
uint64_t wa_version;

bool com_init = false;		/* COM has been initialized in the main thread */

reg_handle wa_options(HKEY_CURRENT_USER, "Software\\Team17SoftwareLTD\\WormsArmageddon\\Options", KEY_QUERY_VALUE | KEY_SET_VALUE, false);

INT_PTR CALLBACK options_dproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* Test if a start/end time is in valid format. An empty string is valid */
bool validate_time(const std::string &time) {
	int stage = 0;
	
	for(size_t i = 0; i < time.length();) {
		if((stage == 1 || stage == 3) && time[i] == '.') {
			stage = 5;
		}
		
		switch(stage) {
			case 0:
			case 2:
			case 4:
			case 6:
				if(isdigit(time[i++])) {
					while(i < time.length() && isdigit(time[i])) {
						i++;
					}
					
					stage++;
				}else{
					return false;
				}
				
				break;
				
			case 1:
			case 3:
				if(time[i++] == ':' && i < time.length() && isdigit(time[i])) {
					stage++;
				}else{
					return false;
				}
				
				break;
				
			case 5:
				if(time[i++] == '.' && i < time.length() && isdigit(time[i])) {
					stage++;
				}else{
					return false;
				}
				
				break;
				
			default:
				return false;
		}
	}
	
	return true;
}

/* TODO: Default path */
std::string choose_dir(HWND parent, const std::string &title, const std::string &test_file) {
	if(!com_init) {
		HRESULT err = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
		if(err != S_OK) {
			/* TODO: Error message */
			MessageBox(parent, std::string("CoInitializeEx: " + to_string(err)).c_str(), NULL, MB_OK | MB_ICONERROR);
			return std::string();
		}
		
		com_init = true;
	}
	
	std::string ret;
	
	while(ret.empty()) {
		BROWSEINFO binfo;
		memset(&binfo, 0, sizeof(binfo));
		
		binfo.hwndOwner = parent;
		binfo.lpszTitle = title.c_str();
		binfo.ulFlags = BIF_NONEWFOLDERBUTTON | BIF_RETURNONLYFSDIRS;
		
		PIDLIST_ABSOLUTE pidl = SHBrowseForFolder(&binfo);
		if(pidl) {
			char path[MAX_PATH];
			std::string test_path;
			
			if(!SHGetPathFromIDList(pidl, path)) {
				goto NOT_FOUND;
			}
			
			CoTaskMemFree(pidl);
			
			if(path[strlen(path) - 1] == '\\') {
				path[strlen(path) - 1] = '\0';
			}
			
			test_path = std::string(path) + "\\" + test_file;
			
			if(GetFileAttributes(test_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
				ret = path;
				break;
			}
			
			NOT_FOUND:
			MessageBox(parent, std::string("Selected directory does not contain " + test_file).c_str(), NULL, MB_OK | MB_ICONERROR);
		}else{
			break;
		}
	}
	
	return ret;
}

void set_combo_height(HWND combo) {
	RECT rect;
	
	GetWindowRect(combo, &rect);
	SetWindowPos(combo, 0, 0, 0, rect.right - rect.left, LIST_HEIGHT, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
}

/* ugh. */
static bool strcaseeq(const char *s1, const char *s2)
{
	while(*s1 != '\0' && *s2 != '\0' && tolower(*s1) == tolower(*s2))
	{
		++s1;
		++s2;
	}
	
	return (*s1 == '\0' && *s2 == '\0');
}

static void repopulate_exe_menu(HWND window)
{
	/* Build a list of things named like WA executables. */
	
	std::vector<std::string> exes;
	exes.push_back("WA.exe");
	
	WIN32_FIND_DATA find_data;
	HANDLE find_handle = FindFirstFile(std::string(wa_path + "\\WA-*.exe").c_str(), &find_data);
	
	if(find_handle != INVALID_HANDLE_VALUE)
	{
		do {
			exes.push_back(find_data.cFileName);
		} while(FindNextFile(find_handle, &find_data));
		
		FindClose(find_handle);
	}
	
	HMENU menu = GetSubMenu(GetSubMenu(GetMenu(window), 0), 1);
	
	/* Clear and repopulate the menu. */
	
	while(RemoveMenu(menu, 0, MF_BYPOSITION)) {}
	
	for(auto exe = exes.begin(); exe != exes.end(); ++exe)
	{
		MENUITEMINFO mii;
		memset(&mii, 0, sizeof(mii));
		
		mii.cbSize     = sizeof(mii);
		mii.fMask      = MIIM_STRING | MIIM_STATE | MIIM_ID;
		mii.fType      = MFT_STRING;
		mii.fState     = strcaseeq(exe->c_str(), wa_exe_name.c_str()) ? MFS_CHECKED : 0;
		mii.wID        = SELECT_WA_EXE;
		mii.dwTypeData = (CHAR*)exe->c_str();
		mii.cch        = exe->length() + 1;
		
		InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &mii);
	}
}

static void update_wa_info()
{
	/* Update paths */
	wa_exe_path = wa_path + "\\" + wa_exe_name;
	
	/* Check if wormkit (HookLib.dll) is installed */
	wormkit_exe = (
		GetFileAttributes(std::string(wa_path + "\\HookLib.dll").c_str()) != INVALID_FILE_ATTRIBUTES
	);
	
	/* Get the version number out of WA.exe */
	
	DWORD fvi_size = GetFileVersionInfoSize(wa_exe_path.c_str(), NULL);
	
	std::unique_ptr<unsigned char[]> fvi(new unsigned char[fvi_size]);
	
	if(!fvi_size || !GetFileVersionInfo(wa_exe_path.c_str(), 0, fvi_size, fvi.get()))
	{
		MessageBox(
			NULL,
			std::string(std::string("Could not get WA version information: ") + w32_error(GetLastError())).c_str(),
			NULL,
			MB_ICONERROR | MB_OK | MB_TASKMODAL);
		
		return;
	}
	
	VS_FIXEDFILEINFO *rb;
	UINT rb_size;
	
	if(!VerQueryValue(fvi.get(), "\\", (void**)(&rb), &rb_size))
	{
		MessageBox(
			NULL,
			"Could not get WA version information",
			NULL,
			MB_ICONERROR | MB_OK | MB_TASKMODAL);
		return;
	}
	
	wa_version = ((uint64_t)(rb->dwFileVersionMS) << 32) | rb->dwFileVersionLS;
}

static void toggle_clipping(HWND hwnd)
{
	bool enabled = checkbox_get(GetDlgItem(hwnd, FIX_CLIPPING));

	config.fix_clipping = enabled;

	EnableWindow(GetDlgItem(hwnd, MIN_VOL_SLIDER), enabled);
	EnableWindow(GetDlgItem(hwnd, MIN_VOL_EDIT), enabled);
}

INT_PTR CALLBACK main_dproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch(msg)
	{
		case WM_INITDIALOG:
		{
			/* Set the window icon... */
			
			SendMessage(hwnd, WM_SETICON, 0, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(ICON16)));
			SendMessage(hwnd, WM_SETICON, 1, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(ICON32)));
			
			HMENU menu = GetMenu(hwnd);

			{
				/* Set the MNS_NOTIFYBYPOS style on the window
				 * menu bar, clicking any items will result in a
				 * WM_MENUCOMMAND message.
				*/

				MENUINFO mi;
				memset(&mi, 0, sizeof(mi));

				mi.cbSize = sizeof(mi);
				mi.fMask  = MIM_STYLE;
				
				GetMenuInfo(menu, &mi);
				
				mi.dwStyle |= MNS_NOTIFYBYPOS;
				
				SetMenuInfo(menu, &mi);
			}
			
			/* Initialise menu items... */
			
			menu_item_enable(GetMenu(hwnd), LOAD_WORMKIT_DLLS, wormkit_exe);
			menu_item_set(GetMenu(hwnd), LOAD_WORMKIT_DLLS, wormkit_exe && config.load_wormkit_dlls);
			
			menu_item_set(GetMenu(hwnd), WA_LOCK_CAMERA, config.wa_lock_camera);
			menu_item_set(GetMenu(hwnd), WA_BIGGER_FONT, config.wa_bigger_fonts);
			menu_item_set(GetMenu(hwnd), WA_TRANSPARENT_LABELS, config.wa_transparent_labels);
			
			HMENU exe_menu = CreatePopupMenu();
			ModifyMenu(
				GetSubMenu(GetMenu(hwnd), 0),
				SELECT_WA_EXE,
				MF_BYCOMMAND | MF_POPUP | MF_STRING,
				(UINT_PTR)(exe_menu),
				"Select Worms Armageddon executable");
			
			repopulate_exe_menu(hwnd);
			
			/* Capture settings... */
			
			SetWindowText(GetDlgItem(hwnd, RES_X), to_string(config.width).c_str());
			SetWindowText(GetDlgItem(hwnd, RES_Y), to_string(config.height).c_str());
			
			SetWindowText(GetDlgItem(hwnd, FRAMES_SEC), to_string(config.frame_rate).c_str());
			
			HWND detail_list = GetDlgItem(hwnd, WA_DETAIL);
			
			for(unsigned int i = 0; detail_levels[i]; i++) {
				ComboBox_AddString(detail_list, detail_levels[i]);
			}
			
			ComboBox_SetCurSel(detail_list, config.wa_detail_level);
			set_combo_height(detail_list);
			
			HWND chat_list = GetDlgItem(hwnd, WA_CHAT);
			
			for(unsigned int i = 0; chat_levels[i]; i++) {
				ComboBox_AddString(chat_list, chat_levels[i]);
			}
			
			ComboBox_SetCurSel(chat_list, config.wa_chat_behaviour);
			set_combo_height(chat_list);
			
			checkbox_set(GetDlgItem(hwnd, DO_CLEANUP), config.do_cleanup);
			
			/* Audio settings... */
			
			volume_init(GetDlgItem(hwnd, INIT_VOL_SLIDER), GetDlgItem(hwnd, INIT_VOL_EDIT), config.init_vol);
			
			checkbox_set(GetDlgItem(hwnd, FIX_CLIPPING), config.fix_clipping);
			toggle_clipping(hwnd);
			
			volume_init(GetDlgItem(hwnd, MIN_VOL_SLIDER), GetDlgItem(hwnd, MIN_VOL_EDIT), config.min_vol);
			
			/* Video settings... */
			
			HWND fmt_list = GetDlgItem(hwnd, VIDEO_FORMAT);
			
			for(unsigned int i = 0; video_formats[i].name; i++)
			{
				ComboBox_AddString(fmt_list, video_formats[i].name);
			}
			
			ComboBox_SetCurSel(fmt_list, config.video_format);
			set_combo_height(fmt_list);
			
			HWND audio_fmt_list = GetDlgItem(hwnd, AUDIO_FORMAT_MENU);
			
			for(unsigned int i = 0; audio_formats[i].name; i++)
			{
				ComboBox_AddString(audio_fmt_list, audio_formats[i].name);
			}
			
			ComboBox_SetCurSel(audio_fmt_list, config.audio_format);
			set_combo_height(audio_fmt_list);
			
			goto VIDEO_ENABLE;
		}
		
		case WM_CLOSE:
		{
			EndDialog(hwnd, 0);
			return TRUE;
		}
		
		case WM_COMMAND:
		{
			if(HIWORD(wp) == BN_CLICKED)
			{
				switch(LOWORD(wp))
				{
					case IDOK:
					{
						/* Capture settings... */
						
						config.replay_file = get_window_string(GetDlgItem(hwnd, REPLAY_PATH));
						
						try {
							config.width  = get_window_int(GetDlgItem(hwnd, RES_X), 0);
							config.height = get_window_int(GetDlgItem(hwnd, RES_Y), 0);
						}
						catch(const bad_input &e)
						{
							MessageBox(hwnd, "Invalid resolution", NULL, MB_OK | MB_ICONERROR);
							break;
						}
						
						try {
							unsigned int max_rate = wa_version >= make_version(3, 7, 2, 40)
								? 25600
								: 50;
							
							config.frame_rate = get_window_int(GetDlgItem(hwnd, FRAMES_SEC), 1, max_rate);
						}
						catch(const bad_input &e)
						{
							MessageBox(hwnd, "Frame rate must be an integer in the range 1-50", NULL, MB_OK | MB_ICONERROR);
							break;
						}
						
						config.start_time = get_window_string(GetDlgItem(hwnd, TIME_START));
						config.end_time   = get_window_string(GetDlgItem(hwnd, TIME_END));
						
						if(!validate_time(config.start_time))
						{
							MessageBox(hwnd, "Invalid start time", NULL, MB_OK | MB_ICONERROR);
							break;
						}
						
						if(!validate_time(config.end_time))
						{
							MessageBox(hwnd, "Invalid end time", NULL, MB_OK | MB_ICONERROR);
							break;
						}
						
						config.wa_detail_level = ComboBox_GetCurSel(GetDlgItem(hwnd, WA_DETAIL));
						config.wa_chat_behaviour = ComboBox_GetCurSel(GetDlgItem(hwnd, WA_CHAT));
						
						config.do_cleanup = checkbox_get(GetDlgItem(hwnd, DO_CLEANUP));
						
						/* Audio settings... */
						
						config.init_vol = SendMessage(GetDlgItem(hwnd, INIT_VOL_SLIDER), TBM_GETPOS, (WPARAM)(0), (LPARAM)(0));
						
						config.fix_clipping = checkbox_get(GetDlgItem(hwnd, FIX_CLIPPING));
						
						config.min_vol  = SendMessage(GetDlgItem(hwnd, MIN_VOL_SLIDER), TBM_GETPOS, (WPARAM)(0), (LPARAM)(0));
						
						/* Video settings... */
						
						config.video_file = get_window_string(GetDlgItem(hwnd, AVI_PATH));
						
						if(config.video_format == 0 && config.audio_format == 0 && config.do_cleanup)
						{
							MessageBox(hwnd, "You've chosen to not create a file and delete frames/audio when finished. You probably don't want this.", NULL, MB_OK | MB_ICONWARNING);
							break;
						}
						
						if((config.video_format > 0 || config.audio_format > 0) && config.video_file.empty())
						{
							MessageBox(hwnd, "Output filename is required", NULL, MB_OK | MB_ICONERROR);
							break;
						}
						
						/* Fill in convenience variables */
						
						config.replay_name = config.replay_file;
						
						size_t last_slash = config.replay_name.find_last_of('\\');
						if(last_slash != std::string::npos)
						{
							config.replay_name.erase(0, last_slash + 1);
						}
						
						if(config.replay_name.find_last_of('\\') != std::string::npos)
						{
							config.replay_name.erase(0, config.replay_name.find_last_of('\\') + 1);
						}
						
						if(config.replay_name.find_last_of('.') != std::string::npos)
						{
							config.replay_name.erase(config.replay_name.find_last_of('.'));
						}
						
						config.capture_dir = wa_path + "\\User\\Capture\\" + config.replay_name;
						
						EndDialog(hwnd, 1);
						break;
					}
					
					case IDCANCEL:
					{
						EndDialog(hwnd, 0);
						break;
					}
					
					case REPLAY_BROWSE:
					{
						char filename[512] = "";
						
						OPENFILENAME openfile;
						memset(&openfile, 0, sizeof(openfile));
						
						openfile.lStructSize = sizeof(openfile);
						openfile.hwndOwner = hwnd;
						openfile.lpstrFilter = "Worms Armageddon replay (*.WAgame)\0*.WAgame\0All Files\0*\0";
						openfile.lpstrFile = filename;
						openfile.nMaxFile = sizeof(filename);
						openfile.lpstrInitialDir = (config.replay_dir.length() ? config.replay_dir.c_str() : (wa_path + "\\User\\Games").c_str());
						openfile.lpstrTitle = "Select replay";
						openfile.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
						
						if(GetOpenFileName(&openfile))
						{
							config.replay_file = filename;
							SetWindowText(GetDlgItem(hwnd, REPLAY_PATH), config.replay_file.c_str());

							if(config.video_dir.length())
							{
								std::string dir = config.video_dir + "\\";
								std::string file = config.replay_file;
								file.erase(0, file.find_last_of('\\') + 1);
								config.video_file = dir + file + ".mp4";
							}else{
								config.video_file = config.replay_file + ".mp4";
							}
							SetWindowText(GetDlgItem(hwnd, AVI_PATH), config.video_file.c_str());
							
							config.replay_dir = config.replay_file;
							config.replay_dir.erase(config.replay_dir.find_last_of('\\'));
						}
						else if(CommDlgExtendedError())
						{
							MessageBox(hwnd, std::string("GetOpenFileName: " + to_string(CommDlgExtendedError())).c_str(), NULL, MB_OK | MB_ICONERROR);
						}
						
						break;
					}
					
					case DO_CLEANUP:
					{
						config.do_cleanup = checkbox_get(GetDlgItem(hwnd, DO_CLEANUP));
						break;
					}

					case FIX_CLIPPING:
					{
						toggle_clipping(hwnd);
						break;
					}
					
					case AVI_BROWSE:
					{
						std::string filter;
						
						std::vector<int> containers = get_valid_containers(config.video_format, config.audio_format);
						
						for(size_t i = 0; i < containers.size(); i++)
						{
							filter.append(container_formats[containers[i]].name);
							filter.append(1, '\0');
							
							filter.append("*.");
							filter.append(container_formats[containers[i]].ext);
							filter.append(1, '\0');
						}
						
						filter.append(1, '\0');
						
						char filename[512] = "";
						
						strncpy(filename, config.video_file.c_str(), sizeof(filename));
						filename[sizeof(filename) - 1] = '\0';
						
						OPENFILENAME openfile;
						memset(&openfile, 0, sizeof(openfile));
						
						openfile.lStructSize     = sizeof(openfile);
						openfile.hwndOwner       = hwnd;
						openfile.lpstrFilter     = filter.data();
						openfile.lpstrFile       = filename;
						openfile.nMaxFile        = sizeof(filename);
						openfile.lpstrInitialDir = config.video_dir.length() ? config.video_dir.c_str() : NULL;
						openfile.lpstrTitle      = "Save video as...";
						openfile.Flags           = OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
						openfile.lpstrDefExt     = "";
						
						if(GetSaveFileName(&openfile))
						{
							config.video_file = filename;
							SetWindowText(GetDlgItem(hwnd, AVI_PATH), config.video_file.c_str());
							
							config.video_dir = config.video_file;
							config.video_dir.erase(config.video_dir.find_last_of('\\'));
						}
						else if(CommDlgExtendedError())
						{
							MessageBox(hwnd, std::string("GetSaveFileName: " + to_string(CommDlgExtendedError())).c_str(), NULL, MB_OK | MB_ICONERROR);
						}
						
						break;
					}
				}
				
				return TRUE;
			}
			else if(HIWORD(wp) == CBN_SELCHANGE)
			{
				if(LOWORD(wp) == VIDEO_FORMAT)
				{
					config.video_format = ComboBox_GetCurSel(GetDlgItem(hwnd, VIDEO_FORMAT));
					
					VIDEO_ENABLE:
					
					EnableWindow(GetDlgItem(hwnd, AVI_PATH), config.video_format > 0 || config.audio_format > 0);
					EnableWindow(GetDlgItem(hwnd, AVI_BROWSE), config.video_format > 0 || config.audio_format > 0);
					
					return TRUE;
				}
				else if(LOWORD(wp) == AUDIO_FORMAT_MENU)
				{
					config.audio_format = ComboBox_GetCurSel(GetDlgItem(hwnd, AUDIO_FORMAT_MENU));

					goto VIDEO_ENABLE;
				}
			}
		}
		
		case WM_MENUCOMMAND:
		{
			MENUITEMINFO mii;
			memset(&mii, 0, sizeof(mii));
			mii.cbSize = sizeof(mii);
			mii.fMask  = MIIM_STRING | MIIM_ID | MIIM_SUBMENU;
			
			GetMenuItemInfo((HMENU)(lp), (UINT)(wp), TRUE, &mii);
			
			std::unique_ptr<char[]> name(new char[++mii.cch]);
			mii.dwTypeData = name.get();
			
			GetMenuItemInfo((HMENU)(lp), (UINT)(wp), TRUE, &mii);
			
			if(mii.wID == SELECT_WA_DIR)
			{
				std::string dir = choose_dir(hwnd, "Select Worms Armageddon directory:", "wa.exe");
				if(!dir.empty())
				{
					wa_exe_name = "WA.exe";
					wa_path = dir;
					
					update_wa_info();
					repopulate_exe_menu(hwnd);
					
					EnableMenuItem(GetMenu(hwnd), LOAD_WORMKIT_DLLS, wormkit_exe ? MF_ENABLED : MF_GRAYED);
					CheckMenuItem(GetMenu(hwnd), LOAD_WORMKIT_DLLS, (wormkit_exe && config.load_wormkit_dlls) ? MF_CHECKED : MF_UNCHECKED);
				}
			}
			else if(mii.wID == SELECT_WA_EXE && mii.hSubMenu == NULL)
			{
				wa_exe_name = name.get();

				update_wa_info();
				repopulate_exe_menu(hwnd);
			}
			else if(mii.wID == LOAD_WORMKIT_DLLS)
			{
				config.load_wormkit_dlls = menu_item_toggle(GetMenu(hwnd), LOAD_WORMKIT_DLLS);
			}
			else if(mii.wID == ADV_OPTIONS)
			{
				DialogBox(GetModuleHandle(NULL), MAKEINTRESOURCE(DLG_OPTIONS), hwnd, &options_dproc);
			}
			else if(mii.wID == WA_LOCK_CAMERA)
			{
				config.wa_lock_camera = menu_item_toggle(GetMenu(hwnd), WA_LOCK_CAMERA);
			}
			else if(mii.wID == WA_BIGGER_FONT)
			{
				config.wa_bigger_fonts = menu_item_toggle(GetMenu(hwnd), WA_BIGGER_FONT);
			}
			else if(mii.wID == WA_TRANSPARENT_LABELS)
			{
				config.wa_transparent_labels = menu_item_toggle(GetMenu(hwnd), WA_TRANSPARENT_LABELS);
			}
		}
		
		case WM_HSCROLL:
		{
			if((HWND)(lp) == NULL)
			{
				break;
			}
			
			int scroll_id = GetWindowLong((HWND)(lp), GWL_ID);
			
			switch(scroll_id)
			{
				case INIT_VOL_SLIDER:
				{
					volume_on_slider((HWND)(lp), GetDlgItem(hwnd, INIT_VOL_EDIT));
					config.init_vol = SendMessage(GetDlgItem(hwnd, INIT_VOL_SLIDER), TBM_GETPOS, (WPARAM)(0), (LPARAM)(0));
					break;
				}
				
				case MIN_VOL_SLIDER:
				{
					volume_on_slider((HWND)(lp), GetDlgItem(hwnd, MIN_VOL_EDIT));
					config.min_vol = SendMessage(GetDlgItem(hwnd, MIN_VOL_SLIDER), TBM_GETPOS, (WPARAM)(0), (LPARAM)(0));
					break;
				}
			}
			
			break;
		}
		
		default:
			break;
	}
	
	return FALSE;
}

std::string escape_filename(std::string name) {
	for(size_t i = 0; i < name.length(); i++) {
		if(name[i] == '\\') {
			name.insert(i++, "\\");
		}
	}
	
	return name;
}

INT_PTR CALLBACK options_dproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch(msg)
	{
		case WM_INITDIALOG:
		{
			SetWindowText(GetDlgItem(hwnd, MAX_ENC_THREADS), to_string(config.max_enc_threads).c_str());
			return TRUE;
		}
		
		case WM_COMMAND:
		{
			if(HIWORD(wp) == BN_CLICKED)
			{
				if(LOWORD(wp) == IDOK)
				{
					try {
						config.max_enc_threads = get_window_int(GetDlgItem(hwnd, MAX_ENC_THREADS), 0);
					}
					catch(const bad_input &e)
					{
						MessageBox(hwnd, "Max threads must be an integer", NULL, MB_OK | MB_ICONERROR);
						break;
					}
					
					EndDialog(hwnd, 1);
				}
				else if(LOWORD(wp) == IDCANCEL)
				{
					PostMessage(hwnd, WM_CLOSE, 0, 0);
				}
			}
			
			return TRUE;
		}
		
		case WM_CLOSE:
		{
			EndDialog(hwnd, 0);
			return TRUE;
		}
		
		default:
		{
			break;
		}
	}
	
	return FALSE;
}

int main(int argc, char **argv)
{
	SetErrorMode(SEM_FAILCRITICALERRORS);
	
	InitCommonControls();
	
	reg_handle reg(HKEY_CURRENT_USER, "Software\\Armageddon Recorder", KEY_QUERY_VALUE | KEY_SET_VALUE, true);
	
	wa_path     = reg.get_string("wa_path");
	wa_exe_name = reg.get_string("wa_exe_name", "WA.exe");
	if(wa_path.empty())
	{
		reg_handle wa_reg(HKEY_CURRENT_USER, "Software\\Team17SoftwareLTD\\WormsArmageddon", KEY_QUERY_VALUE, false);
		wa_path = wa_reg.get_string("PATH");
		
		if(wa_path.empty())
		{
			wa_path = choose_dir(NULL, "Select Worms Armageddon directory:", "wa.exe");
		}
		
		if(wa_path.empty())
		{
			MessageBox(NULL, "Worms Armageddon must be installed.", NULL, MB_OK | MB_ICONERROR);
			return 1;
		}
		
		wa_exe_name = "WA.exe";
	}
	
	config.load_wormkit_dlls = reg.get_dword("load_wormkit_dlls", false);
	update_wa_info();
	
	config.video_format = std::max(get_ffmpeg_index(video_formats, reg.get_string("selected_encoder", "H264")), 0);
	config.audio_format = std::max(get_ffmpeg_index(audio_formats, reg.get_string("audio_format", "AAC")), 0);
	
	config.width = reg.get_dword("res_x", 640);
	config.height = reg.get_dword("res_y", 480);
	
	config.frame_rate = reg.get_dword("frame_rate", 50);
	
	config.max_enc_threads = reg.get_dword("max_enc_threads", 0);
	
	config.wa_detail_level = reg.get_dword("wa_detail_level", 0);
	config.wa_chat_behaviour = reg.get_dword("wa_chat_behaviour", 0);
	config.wa_lock_camera = reg.get_dword("wa_lock_camera", true);
	config.wa_bigger_fonts = reg.get_dword("wa_bigger_fonts", false);
	config.wa_transparent_labels = reg.get_dword("wa_transparent_labels", true);
	
	config.do_cleanup = reg.get_dword("do_cleanup", true);
	
	config.init_vol     = reg.get_dword("init_vol", 100);
	
	config.fix_clipping = reg.get_dword("fix_clipping", true);
	config.min_vol      = reg.get_dword("min_vol", 40);
	
	config.replay_dir = reg.get_string("replay_dir");
	config.video_dir = reg.get_string("video_dir");
	
	while(true)
	{
		auto res = DialogBox(GetModuleHandle(NULL), MAKEINTRESOURCE(DLG_MAIN), NULL, &main_dproc);

		reg.set_string("selected_encoder", video_formats[config.video_format].name);
		reg.set_string("audio_format", audio_formats[config.audio_format].name);
		
		reg.set_dword("res_x", config.width);
		reg.set_dword("res_y", config.height);
		
		reg.set_dword("frame_rate", config.frame_rate);
		
		reg.set_dword("max_enc_threads", config.max_enc_threads);
		
		reg.set_dword("wa_detail_level", config.wa_detail_level);
		reg.set_dword("wa_chat_behaviour", config.wa_chat_behaviour);
		reg.set_dword("wa_lock_camera", config.wa_lock_camera);
		reg.set_dword("wa_bigger_fonts", config.wa_bigger_fonts);
		reg.set_dword("wa_transparent_labels", config.wa_transparent_labels);
		
		reg.set_dword("do_cleanup", config.do_cleanup);
		
		reg.set_dword("init_vol", config.init_vol);
		
		reg.set_dword("fix_clipping", config.fix_clipping);
		reg.set_dword("min_vol", config.min_vol);
		
		reg.set_string("replay_dir", config.replay_dir);
		reg.set_string("video_dir", config.video_dir);
		
		reg.set_string("wa_path", wa_path);
		reg.set_string("wa_exe_name", wa_exe_name);
		reg.set_dword("load_wormkit_dlls", config.load_wormkit_dlls);

		if(!res) {
			break;
		}

		if(!DialogBox(GetModuleHandle(NULL), MAKEINTRESOURCE(DLG_PROGRESS), NULL, &prog_dproc)) {
			break;
		}
	}

	if(com_init) {
		CoUninitialize();
	}
	
	return 0;
}

/* Convert a windows error number to an error message */
const char *w32_error(DWORD errnum) {
	static char buf[1024] = {'\0'};
	
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, errnum, 0, buf, 1023, NULL);
	buf[strcspn(buf, "\r\n")] = '\0';
	return buf;	
}

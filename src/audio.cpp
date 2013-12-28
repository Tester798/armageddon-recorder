/* Armageddon Recorder - Audio functions
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

#define __STDC_LIMIT_MACROS

#include <windows.h>
#include <string>
#include <stdio.h>
#include <assert.h>
#include <sndfile.h>
#include <tr1/memory>
#include <map>

#include "audio.hpp"
#include "ds-capture.h"
#include "ui.hpp"
#include "capture.hpp"

struct audio_buffer
{
	unsigned char *buf;
	size_t size;
	
	unsigned int sample_rate;
	unsigned int sample_bits;
	unsigned int channels;
	
	bool playing;
	bool looping;
	size_t position;
	double gain;
	
	audio_buffer(size_t new_size, unsigned int new_rate, unsigned int new_bits, unsigned int new_channels, double new_gain)
	{
		buf  = new unsigned char[new_size];
		size = new_size;
		
		if(new_bits == 8)
		{
			memset(buf, 128, size);
		}
		else{
			memset(buf, 0, size);
		}
		
		sample_rate = new_rate;
		sample_bits = new_bits;
		channels    = new_channels;
		
		playing  = false;
		looping  = false;
		position = 0;
		gain     = new_gain;
	}
	
	audio_buffer(const audio_buffer &src)
	{
		buf  = new unsigned char[src.size];
		size = src.size;
		
		memcpy(buf, src.buf, size);
		
		sample_rate = src.sample_rate;
		sample_bits = src.sample_bits;
		channels    = src.channels;
		
		playing  = src.playing;
		looping  = src.looping;
		position = src.position;
		gain     = src.gain;
	}
	
	~audio_buffer()
	{
		delete buf;
	}
	
	std::vector<int16_t> read_frame()
	{
		/* Step 1: Populate resample_in with samples in the source
		 * format.
		*/
		
		size_t input_frame_size    = (sample_bits / 8) * channels;    /* Size of one audio frame in the buffer */
		size_t input_frames_needed = sample_rate / config.frame_rate; /* Number of those frames needed */
		
		size_t ri_size             = input_frame_size * input_frames_needed;
		unsigned char *resample_in = new unsigned char[ri_size];
		
		if(sample_bits == 8)
		{
			memset(resample_in, 128, ri_size);
		}
		else{
			memset(resample_in, 0, ri_size);
		}
		
		for(size_t f = 0; f < input_frames_needed && playing; ++f)
		{
			if(looping && position + input_frame_size >= size)
			{
				position = 0;
			}
			
			if(position + input_frame_size < size)
			{
				memcpy(resample_in + (input_frame_size * f), buf + position, input_frame_size);
				position += input_frame_size;
			}
		}
		
		/* Step 2: Resample to the output format. */
		
		size_t ro_size              = pcm_resample_bufsize(input_frames_needed, channels, sample_rate, SAMPLE_RATE, SAMPLE_BITS);
		unsigned char *resample_out = new unsigned char[ro_size];
		
		memset(resample_out, 0, ro_size);
		
		pcm_resample(resample_in, resample_out, input_frames_needed, channels, sample_rate, SAMPLE_RATE, sample_bits, SAMPLE_BITS);
		
		/* Step 3: Extract the samples from the PCM data and drop or
		 * duplicate channels as necessary.
		*/
		
		std::vector<int16_t> ret;
		
		for(size_t f = 0; f < (SAMPLE_RATE / config.frame_rate); ++f)
		{
			size_t so = f * channels * 2;
			
			if(so + 1 < ro_size)
			{
				ret.push_back(*(int16_t*)(resample_out + so) * gain);
				so += 2;
			}
			else{
				ret.push_back(0);
			}
			
			if(channels >= 2 && so + 1 < ro_size)
			{
				ret.push_back(*(int16_t*)(resample_out + so) * gain);
				so += 2;
			}
			else{
				ret.push_back(ret.back());
			}
		}
		
		delete resample_out;
		delete resample_in;
		
		return ret;
	}
};

typedef std::map<unsigned int, audio_buffer>::iterator buffer_iter;

bool make_output_wav()
{
	std::string log_path = config.capture_dir + "\\" FRAME_PREFIX "audio.dat";
	std::string wav_path = config.capture_dir + "\\" FRAME_PREFIX "audio.wav";
	
	FILE *log = fopen(log_path.c_str(), "rb");
	if(!log)
	{
		log_push(std::string("Could not open " FRAME_PREFIX "audio.dat: ") + w32_error(GetLastError()) + "\r\n");
		return false;
	}
	
	/* Background audio is held in a streaming buffer and properly
	 * synchronising the play/write pointers after the fact is difficult,
	 * so we make a first pass over the log, concatenating each blob of PCM
	 * together before resampling it to the output format.
	*/
	
	std::vector<int16_t> background_samples;
	size_t background_offset = 0;
	
	{
		unsigned int background_buffer = 0;
		std::vector<unsigned char> background_raw;
		
		unsigned int background_rate;
		unsigned int background_bits;
		unsigned int background_channels;
		
		struct audio_event event;
		while(fread(&event, sizeof(event), 1, log))
		{
			if(event.check != 0x12345678)
			{
				log_push("Encountered record with invalid check\r\n");
				break;
			}
			
			if(event.op == AUDIO_OP_INIT)
			{
				/* Assume the last created (not cloned) buffer
				 * is the one used for background audio.
				*/
				
				background_buffer = event.e.init.buf_id;
				background_raw.clear();
				
				background_rate     = event.e.init.sample_rate;
				background_bits     = event.e.init.sample_bits;
				background_channels = event.e.init.channels;
			}
			else if(event.op == AUDIO_OP_LOAD && event.e.load.buf_id == background_buffer)
			{
				size_t base = background_raw.size();
				background_raw.resize(base + event.e.load.size);
				
				if(fread(&(background_raw[base]), 1, event.e.load.size, log) != event.e.load.size)
				{
					log_push("Unexpected end of log!\r\n");
					fclose(log);
					return false;
				}
			}
		}
		
		assert(fseek(log, 0, SEEK_SET) == 0);
		
		/* Resample the raw PCM to the output format. */
		
		size_t raw_frames = background_raw.size() / ((background_bits / 8) * background_channels);
		size_t ro_size    = pcm_resample_bufsize(raw_frames, background_channels, background_rate, SAMPLE_RATE, SAMPLE_BITS);
		
		unsigned char *resample_out = new unsigned char[ro_size];
		memset(resample_out, 0, ro_size);
		
		pcm_resample(&(background_raw[0]), resample_out, raw_frames, background_channels, background_rate, SAMPLE_RATE, background_bits, SAMPLE_BITS);
		
		/* Populate the background_samples buffer with samples that can
		 * be directly copied into m_samples, duplicating or skipping
		 * channels as necessary.
		*/
		
		size_t ro_frames = pcm_resample_frames(raw_frames, background_rate, SAMPLE_RATE);
		int16_t *ro_ptr  = (int16_t*)(resample_out);
		
		background_samples.reserve(ro_frames * CHANNELS);
		
		for(size_t f = 0; f < ro_frames; ++f)
		{
			for(unsigned int c = 0; c < CHANNELS; ++c)
			{
				if(c < background_channels)
				{
					background_samples.push_back(*(ro_ptr++));
				}
				else{
					background_samples.push_back(background_samples.front());
				}
			}
			
			if(background_channels > CHANNELS)
			{
				ro_ptr += (background_channels - CHANNELS);
			}
		}
	}
	
	SF_INFO wav_fmt;
	wav_fmt.samplerate = SAMPLE_RATE;
	wav_fmt.channels   = CHANNELS;
	wav_fmt.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	
	SNDFILE *wav = sf_open(wav_path.c_str(), SFM_WRITE, &wav_fmt);
	if(!wav)
	{
		log_push(std::string("Could not open ") + wav_path + ": " + sf_strerror(NULL) + "\r\n");
		return false;
	}
	
	int volume = config.init_vol;
	
	std::map<unsigned int, audio_buffer> buffers;
	unsigned int background_buffer = 0;
	
	struct audio_event event;
	
	unsigned int frame_num = 0;
	
	RESTART:
	
	while(fread(&event, sizeof(event), 1, log))
	{
		assert(event.frame >= frame_num);
		
		if(event.check != 0x12345678)
		{
			log_push("Encountered record with invalid check\r\n");
			break;
		}
		
		/* Mix audio for any frames before this one. */
		
		while(frame_num < event.frame)
		{
			++frame_num;
			
			/* Get samples from each buffer, combine them all. */
			
			std::vector<int64_t> m_samples((SAMPLE_RATE / config.frame_rate) * CHANNELS);
			
			for(buffer_iter b = buffers.begin(); b != buffers.end(); ++b)
			{
				if(b->first == background_buffer)
				{
					continue;
				}
				
				std::vector<int16_t> b_samples = b->second.read_frame();
				
				assert(m_samples.size() == b_samples.size());
				
				for(size_t i = 0; i < m_samples.size() && i < b_samples.size(); ++i)
				{
					m_samples[i] += b_samples[i];
				}
			}
			
			for(size_t i = 0; i < m_samples.size() && (background_samples.size() - background_offset) >= CHANNELS;)
			{
				for(int c = 0; c < CHANNELS; ++c)
				{
					m_samples[i++] += (background_samples[background_offset++] * ((double)(volume) / 100));
				}
			}
			
			/* Search for any samples which will be clipped. */
			
			if(config.fix_clipping && volume > config.min_vol)
			{
				for(size_t i = 0; i < m_samples.size(); ++i)
				{
					if(m_samples[i] < INT16_MIN || m_samples[i] > INT16_MAX)
					{
						log_push("Clipping detected on frame " + to_string(frame_num) + " at " + to_string(volume) + "% volume\r\n");
						
						/* TODO: Reduce volume based on
						 * clipping rather than stepping.
						*/
						
						if((volume -= config.step_vol) < config.min_vol)
						{
							volume = config.min_vol;
						}
						
						log_push("Trying with " + to_string(volume) + "% volume...\r\n");
						
						frame_num = 0;
						
						buffers.clear();
						
						assert(sf_seek(wav, 0, SEEK_SET) == 0);
						
						assert(fseek(log, 0, SEEK_SET) == 0);
						
						goto RESTART;
					}
				}
			}
			
			/* Convert the mixed samples to int16_t and write to the
			 * output WAV.
			*/
			
			std::vector<int16_t> w_samples(m_samples.begin(), m_samples.end());
			sf_write_short(wav, &(w_samples[0]), w_samples.size());
		}
		
		switch(event.op)
		{
			case AUDIO_OP_INIT:
			{
				audio_buffer ab(event.e.init.size, event.e.init.sample_rate, event.e.init.sample_bits, event.e.init.channels, ((double)(volume) / 100));
				buffers.insert(std::make_pair(event.e.init.buf_id, ab));
				
				background_buffer = event.e.init.buf_id;
				
				break;
			}
			
			case AUDIO_OP_FREE:
			{
				buffers.erase(event.e.free.buf_id);
				
				break;
			}
			
			case AUDIO_OP_CLONE:
			{
				buffer_iter bi = buffers.find(event.e.clone.src_buf_id);
				
				if(bi == buffers.end())
				{
					log_push("Attempted to clone unknown buffer!\r\n");
					break;
				}
				
				buffers.insert(std::make_pair(event.e.clone.new_buf_id, bi->second));
				
				break;
			}
			
			case AUDIO_OP_LOAD:
			{
				unsigned char *tmp = new unsigned char[event.e.load.size];
				
				if(fread(tmp, 1, event.e.load.size, log) != event.e.load.size)
				{
					log_push("Unexpected end of log!\r\n");
					delete tmp;
					sf_close(wav);
					fclose(log);
					
					return false;
				}
				
				buffer_iter bi = buffers.find(event.e.load.buf_id);
				
				if(bi == buffers.end())
				{
					log_push("Attempted to load into unknown buffer!\r\n");
					delete tmp;
					
					break;
				}
				
				if((event.e.load.offset + event.e.load.size) > bi->second.size)
				{
					size_t max = bi->second.size - event.e.load.offset;
					
					log_push("Attempted to write past the end of a buffer!\r\n");
					log_push(std::string("Truncating write from ") + to_string(event.e.load.size) + " to " + to_string(max) + "\r\n");
					
					event.e.load.size = max;
				}
				
				memcpy(bi->second.buf + event.e.load.offset, tmp, event.e.load.size);
				
				delete tmp;
				
				break;
			}
			
			case AUDIO_OP_START:
			{
				buffer_iter bi = buffers.find(event.e.start.buf_id);
				
				if(bi == buffers.end())
				{
					log_push("Attempted to play unknown buffer!\r\n");
					break;
				}
				
				bi->second.playing = true;
				bi->second.looping = event.e.start.loop;
				
				break;
			}
			
			case AUDIO_OP_STOP:
			{
				buffer_iter bi = buffers.find(event.e.stop.buf_id);
				
				if(bi == buffers.end())
				{
					log_push("Attempted to stop unknown buffer!\r\n");
					break;
				}
				
				bi->second.playing = false;
				
				break;
			}
			
			case AUDIO_OP_JMP:
			{
				buffer_iter bi = buffers.find(event.e.jmp.buf_id);
				
				if(bi == buffers.end())
				{
					log_push("Attempted to set position of unknown buffer!\r\n");
					break;
				}
				
				if(event.e.jmp.offset >= bi->second.size)
				{
					log_push("Attempted to set position past end of buffer!\r\n");
					break;
				}
				
				bi->second.position = event.e.jmp.offset;
				
				break;
			}
			
			case AUDIO_OP_FREQ:
			{
				buffer_iter bi = buffers.find(event.e.freq.buf_id);
				
				if(bi == buffers.end())
				{
					log_push("Attempted to set frequency of unknown buffer!\r\n");
					break;
				}
				
				bi->second.sample_rate = event.e.freq.sample_rate;
				
				break;
			}
			
			case AUDIO_OP_GAIN:
			{
				buffer_iter bi = buffers.find(event.e.gain.buf_id);
				
				if(bi == buffers.end())
				{
					log_push("Attempted to set gain of unknown buffer!\r\n");
					break;
				}
				
				bi->second.gain = event.e.gain.gain * ((double)(volume) / 100);
				
				break;
			}
			
			default:
			{
				log_push("Unknown event ID in log!\r\n");
				break;
			}
		}
	}
	
	sf_close(wav);
	fclose(log);
	
	return true;
}

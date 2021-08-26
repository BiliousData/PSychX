#include "../audio.h"

#include "../io.h"
#include "../main.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define MA_NO_DECODING
#include "miniaudio.h"

//XA state
#define XA_STATE_PLAYING (1 << 0)
#define XA_STATE_LOOPS   (1 << 1)

static XA_Track xa_track;
static u8 xa_channel;

static u8 xa_state;

//Miniaudio
static ma_context xa_context;
static ma_device xa_device;
static ma_mutex xa_mutex;

static unsigned int xa_samplerate;

static double xa_lasttime, xa_interptime, xa_interpstart;

//MP3 decode
typedef struct
{
	boolean playing;
	short *data, *datap, *datae;
} MP3Decode;

MP3Decode xa_mp3[2];

extern FILE *IO_OpenFile(CdlFILE *file);

static boolean MP3Decode_Decode(MP3Decode *this, CdlFILE *file)
{
	//Open file and read contents
	FILE *fp = IO_OpenFile(file);
	if (fp == NULL)
		return true;
	
	fseek(fp, 0, SEEK_END);
	size_t size = ftell(fp);
	unsigned char *data = malloc(size);
	if (data == NULL)
	{
		sprintf(error_msg, "[MP3Decode_Decode] Failed to allocate \"%s\" buffer (size 0x%X)", file->path, (unsigned int)size);
		ErrorLock();
		return true;
	}
	fseek(fp, 0, SEEK_SET);
	if (fread(data, size, 1, fp) != 1)
	{
		sprintf(error_msg, "[MP3Decode_Decode] Failed to read \"%s\"", file->path);
		ErrorLock();
		return true;
	}
	fclose(fp);
	
	//Prepare dr_mp3 decoding
	drmp3 drmp3_instance;
	if (!drmp3_init_memory(&drmp3_instance, data, size, NULL))
	{
		sprintf(error_msg, "[MP3Decode_Decode] Failed to initialize dr_mp3 instance");
		ErrorLock();
		return true;
	}
	
	drmp3_uint64 decoded_frames = drmp3_get_pcm_frame_count(&drmp3_instance);
	drmp3_uint32 decoded_channels = drmp3_instance.channels;
	drmp3_uint32 decoded_samplerate = drmp3_instance.sampleRate;
	
	//Decode entire file to buffer
	short *decoded = malloc(decoded_frames * drmp3_instance.channels * sizeof(short));
	
	drmp3_read_pcm_frames_s16(&drmp3_instance, decoded_frames, decoded);
	drmp3_uninit(&drmp3_instance);
	free(data);
	
	//Convert buffer to output format
	ma_data_converter_config config = ma_data_converter_config_init(ma_format_s16, ma_format_s16, decoded_channels, 2, decoded_samplerate, xa_samplerate);
	ma_data_converter data_converter;
	if (ma_data_converter_init(&config, &data_converter) != MA_SUCCESS)
	{
		sprintf(error_msg, "[MP3Decode_Decode] Failed to initialize miniaudio data converter");
		ErrorLock();
	}
	
	//Convert to final buffer
	ma_uint64 output_frames = ma_data_converter_get_expected_output_frame_count(&data_converter, decoded_frames);
	this->data = malloc((output_frames << 1) * sizeof(short));
	
	ma_uint64 input_frames = decoded_frames;
	ma_data_converter_process_pcm_frames(&data_converter, decoded, &input_frames, this->data, &output_frames);
	ma_data_converter_uninit(&data_converter);
	free(decoded);
	
	this->datae = this->data + (output_frames << 1);
	
	return false;
}

static void MP3Decode_Mix(MP3Decode *this, short *stream, ma_uint32 frames_to_do)
{
	//Make sure data exists
	if (this->data == NULL || this->datap == NULL || this->datae == NULL)
		return;
	
	//Calculate frames left
	ma_uint32 frames_left = (this->datae - this->datap) >> 1;
	if (frames_left > frames_to_do)
		frames_left = frames_to_do;
	
	//Mix
	while (frames_left-- > 0)
	{
		*stream++ += *this->datap++;
		*stream++ += *this->datap++;
	}
}

static void MP3Decode_Skip(MP3Decode *this, ma_uint32 frames_to_skip)
{
	//Make sure data exists
	if (this->data == NULL || this->datap == NULL || this->datae == NULL)
		return;
	
	//Skip
	this->datap += (frames_to_skip << 1);
	if (this->datap >= this->datae)
		this->datap = this->datae;
}

//XA files and tracks
static CdlFILE xa_files[XA_TrackMax];

#include "../audio_def.h"

//Miniaudio callback
static void Audio_Callback(ma_device *device, void *output_buffer_void, const void *input_buffer, ma_uint32 frames_to_do)
{
	(void)input_buffer;
	
	//Lock mutex during mixing
	ma_mutex_lock(&xa_mutex);
	
	//Mix XA
	if (xa_state & XA_STATE_PLAYING)
	{
		//Update timing state
		xa_interptime = xa_lasttime;
		xa_interpstart = glfwGetTime();
		xa_lasttime = (double)(xa_mp3[xa_channel].datap - xa_mp3[xa_channel].data) / xa_samplerate / 2.0;
		
		//Mix
		MP3Decode_Mix(&xa_mp3[xa_channel], (short*)output_buffer_void, frames_to_do);
		MP3Decode_Skip(&xa_mp3[xa_channel ^ 1], frames_to_do);
		
		//Check if songs ended
		if ((xa_mp3[0].data == NULL || xa_mp3[0].datap >= xa_mp3[0].datae) && (xa_mp3[1].data == NULL || xa_mp3[1].datap >= xa_mp3[1].datae))
		{
			if (xa_state & XA_STATE_LOOPS)
			{
				//Reset pointers
				xa_mp3[0].datap = xa_mp3[0].data;
				xa_mp3[1].datap = xa_mp3[1].data;
			}
			else
			{
				//Stop playing
				xa_state &= ~XA_STATE_PLAYING;
			}
		}
	}
	
	//Unlock mutex
	ma_mutex_unlock(&xa_mutex);
}

//Audio functions
void Audio_Init(void)
{
	//Get file positions
	CdlFILE *filep = xa_files;
	for (const XA_Mp3 *mp3 = xa_mp3s; mp3->name != NULL; mp3++, filep++)
	{
		char apath[64];
		if (mp3->vocal)
		{
			sprintf(apath, "\\MUSIC\\%sv.mp3;1", mp3->name);
			IO_FindFile(filep, apath);
		}
		else
		{
			sprintf(apath, "\\MUSIC\\%s.mp3;1", mp3->name);
			IO_FindFile(filep, apath);
		}
	}
	
	//Initialize XA state
	xa_track = -1;
	xa_channel = 0;
	
	xa_state = 0;
	
	xa_mp3[0].data = NULL;
	xa_mp3[1].data = NULL;
	
	//Initialize miniaudio
	if (ma_context_init(NULL, 0, NULL, &xa_context) != MA_SUCCESS)
	{
		sprintf(error_msg, "[Audio_Init] Failed to initialize miniaudio");
		ErrorLock();
		return;
	}
	
	//Create miniaudio device
	ma_device_config config = ma_device_config_init(ma_device_type_playback);
	config.playback.pDeviceID = NULL;
	config.playback.format = ma_format_s16;
	config.playback.channels = 2;
	config.sampleRate = 0; //Use native sample rate
	config.noPreZeroedOutputBuffer = MA_FALSE;
	config.dataCallback = Audio_Callback;
	config.pUserData = NULL;
	
	if (ma_device_init(&xa_context, &config, &xa_device) != MA_SUCCESS)
	{
		sprintf(error_msg, "[Audio_Init] Failed to create miniaudio device");
		ErrorLock();
		return;
	}
	
	xa_samplerate = xa_device.sampleRate;
	
	if (ma_mutex_init(&xa_mutex) != MA_SUCCESS)
	{
		sprintf(error_msg, "[Audio_Init] Failed to create miniaudio mutex");
		ErrorLock();
		return;
	}
	
	ma_device_start(&xa_device);
}

void Audio_PlayXA_Track(XA_Track track, u8 volume, u8 channel, boolean loop)
{
	//Ensure track is loaded
	Audio_SeekXA_Track(track);
	
	//Lock mutex during state modification
	ma_mutex_lock(&xa_mutex);
	
	xa_state = XA_STATE_PLAYING | (loop ? XA_STATE_LOOPS : 0);
	xa_lasttime = xa_interptime = 0.0;
	xa_interpstart = glfwGetTime();
	
	ma_mutex_unlock(&xa_mutex);
}

void Audio_SeekXA_Track(XA_Track track)
{
	//Lock mutex during state modification
	ma_mutex_lock(&xa_mutex);
	
	//Reset XA state
	xa_state = 0;
	xa_channel = 0;
	
	//Read file if different track
	if (track != xa_track)
	{
		//Free previous track
		free(xa_mp3[0].data);
		free(xa_mp3[1].data);
		
		//Read new track
		if (xa_mp3s[track].vocal)
		{
			char *path = xa_files[track].path;
			path[strlen(path) - 5] = 'v';
			MP3Decode_Decode(&xa_mp3[0], &xa_files[track]);
			path[strlen(path) - 5] = 'i';
			MP3Decode_Decode(&xa_mp3[1], &xa_files[track]);
		}
		else
		{
			MP3Decode_Decode(&xa_mp3[0], &xa_files[track]);
			xa_mp3[1].data = NULL;
		}
		
		//Remember
		xa_track = track;
	}
	
	//Reset XA position
	xa_mp3[0].datap = xa_mp3[0].data;
	xa_mp3[1].datap = xa_mp3[1].data;
	
	//Unlock mutex
	ma_mutex_unlock(&xa_mutex);
}

void Audio_PauseXA(void)
{
	//Lock mutex during state modification
	ma_mutex_lock(&xa_mutex);
	xa_state &= ~XA_STATE_PLAYING;
	ma_mutex_unlock(&xa_mutex);
}

void Audio_StopXA(void)
{
	//Lock mutex during state modification
	ma_mutex_lock(&xa_mutex);
	
	//Set XA state
	xa_state = 0;
	xa_channel = 0;
	
	//Free previous track
	free(xa_mp3[0].data); xa_mp3[0].data = NULL;
	free(xa_mp3[1].data); xa_mp3[1].data = NULL;
	
	//Unlock mutex
	ma_mutex_unlock(&xa_mutex);
}

void Audio_ChannelXA(u8 channel)
{
	//Lock mutex during state modification
	ma_mutex_lock(&xa_mutex);
	xa_channel = channel & 1;
	ma_mutex_unlock(&xa_mutex);
}

s32 Audio_TellXA_Sector(void)
{
	return (s64)Audio_TellXA_Milli() * 75 / 1000; //trolled
}

s32 Audio_TellXA_Milli(void)
{
	//Lock mutex during state check
	ma_mutex_lock(&xa_mutex);
	
	double xa_timesec = xa_interptime + (glfwGetTime() - xa_interpstart);
	s32 pos = (s32)(xa_timesec * 1000);
	
	//Unlock mutex
	ma_mutex_unlock(&xa_mutex);
	return pos;
}

boolean Audio_PlayingXA(void)
{
	return (xa_state & XA_STATE_PLAYING) != 0;
}

void Audio_WaitPlayXA(void)
{
	
}

void Audio_ProcessXA(void)
{
	
}

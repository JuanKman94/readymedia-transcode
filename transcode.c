/* MiniDLNA media server
 * Copyright (C) 2012  Lukas Jirkovsky
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

#include "config.h"

#include "libav.h"

#include "upnpglobalvars.h"
#include "minidlnatypes.h"
#include "transcode.h"
#include "utils.h"
#include "log.h"

#define READ 0
#define WRITE 1

pid_t
popenvp(const char* file, char * const argv[], int *pipehandle)
{
	int fildes[2];
	pid_t pid;

	/* create a pipe */
	if(pipe(fildes)<0)
	{
		DPRINTF(E_ERROR, L_TRANSCODE, "Cannot create pipe (%s)\n", strerror(errno));
		return -1;
	}

	/* Invoke processs */
	pid=fork();
	if (pid < 0)
	{
		DPRINTF(E_ERROR, L_TRANSCODE, "Fork failed (%s)\n", strerror(errno));
		close(fildes[WRITE]);
		close(fildes[READ]);
		return pid;
	}
	/* child */
	if(pid == 0)
	{
		close(fildes[READ]);
		dup2(fildes[WRITE], WRITE);

		if (execvp(file, argv) < 0) {
			DPRINTF(E_ERROR, L_TRANSCODE, "Exec failed (%s)\n", strerror(errno));
			close(fildes[WRITE]);
			exit(1);
		}
	}
	else
	{
		close(fildes[WRITE]);
	}
	*pipehandle = fildes[READ];

	return pid;
}

/* NOTE: Partially based on Hiero's code */
pid_t
exec_transcode(char *transcoder, char *source_path, int offset, int end_offset, int *pipehandle)
{
	pid_t pid;
	int retval;
	char position[12], duration[12];
	static struct sigaction sa;
	char * args[5];

	sprintf(position, "%d.%03d", offset/1000, offset%1000);
	sprintf(duration, "%d.%03d", (end_offset - offset + 1)/1000,  (end_offset - offset + 1)%1000);

	args[0] = transcoder;
	args[1] = source_path;
	args[2] = position;
	args[3] = duration;
	args[4] = NULL;

	/* Invoke processs */
	pid = popenvp(args[0], args, pipehandle);

	return pid;
}

pid_t
exec_transcode_img(char *transcoder, char *source_path, char *dest_path)
{
	pid_t pid;
	int status, retval;
	static struct sigaction sa;
	char * args[4];

	args[0] = transcoder;
	args[1] = source_path;
	args[2] = dest_path;
	args[3] = NULL;

	/* Invoke processs */
	pid=fork();
	if (pid < 0)
	{
		DPRINTF(E_ERROR, L_TRANSCODE, "Fork failed (%s)\n", strerror(errno));
		return pid;
	}
	/* child */
	if(pid == 0)
	{
		if (execvp(args[0], args) < 0) {
			DPRINTF(E_ERROR, L_TRANSCODE, "Exec failed (%s)\n", strerror(errno));
			exit(1);
		}
	}

	return pid;
}

int
needs_transcode_image(const char* path, enum client_types client)
{
	int i;
	struct transcode_list_format_s *transcode_format_it = NULL;
	struct transcode_info_s *clients_info[] = {client_types[0].transcode_info, client_types[client].transcode_info};

	for ( i = 0; i < 2; i++ )
	{
		if (clients_info[i])
		{
			transcode_format_it = clients_info[i]->image_formats;
			/* test for the reserved value "all" */
			if ( transcode_format_it && strcmp(transcode_format_it->value, "all") == 0 )
			{
				return 1;
			}
			while( transcode_format_it )
			{
				if ( ends_with(path, transcode_format_it->value) )
				{
					return 1;
				}
				transcode_format_it = transcode_format_it->next;
			}
		}
	}

	return 0;
}

int
needs_transcode_audio(const char* path, enum client_types client)
{
	int ret;
	AVFormatContext *ctx = NULL;
	struct AVCodecParameters *ac = NULL;
	int i;
	struct transcode_list_format_s *transcode_format_it = NULL;
	const struct AVCodec *codec = NULL;
	struct transcode_info_s *clients_info[] = {client_types[0].transcode_info, client_types[client].transcode_info};

	/* prepare ffmpeg */
	ret = lav_open(&ctx, path);
	if( ret != 0 )
	{
		char err[128];
		av_strerror(ret, err, sizeof(err));
		DPRINTF(E_ERROR, L_TRANSCODE, "Opening %s failed! [%s]\n", path, err);
		return -1;
	}
	for( i=0; i<ctx->nb_streams; i++)
	{
		if( ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO )
		{
			ac = ctx->streams[i]->codecpar;
			break;
		}
	}
	if ( ac )
	{
		codec = avcodec_find_decoder(ac->codec_id);

		for ( i = 0; i < 2; i++ )
		{
			if (clients_info[i])
			{
				transcode_format_it = clients_info[i]->audio_codecs;
				/* test for the reserved value "all" */
				if ( transcode_format_it && strcmp(transcode_format_it->value, "all") == 0 )
				{
					lav_close(ctx);
					return 1;
				}

				while( transcode_format_it )
				{
					if ( strcmp(codec->name, transcode_format_it->value) == 0 )
					{
						lav_close(ctx);
						return 1;
					}
					transcode_format_it = transcode_format_it->next;
				}
			}
		}
	}

	lav_close(ctx);
	return 0;
}

int
needs_transcode_video(const char* path, enum client_types client)
{
	int ret;
	AVFormatContext *ctx = NULL;
	const struct AVCodec *codec = NULL;
	int audio_stream = -1, video_stream = -1;
	struct AVCodecParameters *ac = NULL;
	struct AVCodecParameters *vc = NULL;
	int i;
	struct transcode_list_format_s *transcode_format_it = NULL;
	struct transcode_info_s *clients_info[] = {client_types[0].transcode_info, client_types[client].transcode_info};

	/* prepare ffmpeg */
	ret = lav_open(&ctx, path);
	if( ret != 0 )
	{
		char err[128];
		av_strerror(ret, err, sizeof(err));
		DPRINTF(E_ERROR, L_TRANSCODE, "Opening %s failed! [%s]\n", path, err);
		return -1;
	}
	for( i=0; i<ctx->nb_streams; i++)
	{
		if( audio_stream == -1 &&
			ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO )
		{
			audio_stream = i;
			ac = ctx->streams[audio_stream]->codecpar;
			continue;
		}
		else if( video_stream == -1 &&
			ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
		{
			video_stream = i;
			vc = ctx->streams[video_stream]->codecpar;
			continue;
		}
	}
	if ( vc == NULL )
	{
		/* This must not be a video file. */
		DPRINTF(E_ERROR, L_TRANSCODE, "File does not contain a video stream.\n");
		lav_close(ctx);
		return 0;
	}

	/* check if the file needs to be transcoded */

	/* check the container */
	for ( i = 0; i < 2; i++ )
	{
		if (clients_info[i])
		{
			transcode_format_it = clients_info[i]->video_containers;
			/* test for the reserved value "all" */
			if ( transcode_format_it && strcmp(transcode_format_it->value, "all") == 0 )
			{
				lav_close(ctx);
				return 1;
			}

			while( transcode_format_it )
			{
				if ( strcmp(ctx->iformat->name, transcode_format_it->value) == 0 )
				{
					lav_close(ctx);
					return 1;
				}
				transcode_format_it = transcode_format_it->next;
			}
		}
	}

	/* check the video codec */
	for ( i = 0; i < 2; i++ )
	{
		if (clients_info[i])
		{
			transcode_format_it = clients_info[i]->video_codecs;
			/* test for the reserved value "all" */
			if ( transcode_format_it && strcmp(transcode_format_it->value, "all") == 0 )
			{
				lav_close(ctx);
				return 1;
			}

			codec = avcodec_find_decoder(vc->codec_id);
			while( transcode_format_it )
			{
				if ( strcmp(codec->name, transcode_format_it->value) == 0 )
				{
					lav_close(ctx);
					return 1;
				}
				transcode_format_it = transcode_format_it->next;
			}
		}
	}

	/* check the audio codec */
	for ( i = 0; i < 2; i++ )
	{
		if (clients_info[i])
		{
			transcode_format_it = clients_info[i]->audio_codecs;
			/* test for the reserved value "all" */
			if ( transcode_format_it && strcmp(transcode_format_it->value, "all") == 0 )
			{
				lav_close(ctx);
				return 1;
			}

			codec = avcodec_find_decoder(ac->codec_id);
			while( transcode_format_it )
			{
				if ( strcmp(codec->name, transcode_format_it->value) == 0 )
				{
					lav_close(ctx);
					return 1;
				}
				transcode_format_it = transcode_format_it->next;
			}
		}
	}

	lav_close(ctx);
	return 0;
}

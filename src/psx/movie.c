//This code relies upon strplay.c, which was created by Lameguy64 of Meido-Tek Productions.

/*
	I'll leave some documentation about this code here, so it shouldn't be too hard to understand.
	Keep in mind this is a rewrite of the movie.c from the Mind Games port, so things are different.

	This code assumes that any str file you give it is 320x240, so keep that in mind.
*/

#include "../movie.h"

#include "../strplay.c"
#include <sys/types.h>
#include <libetc.h>
#include <libgte.h>
#include <libgpu.h>
#include <libcd.h>

#define DEBUG 1

//Prepare to do movie
void Movie_Prep()
{
	// Reset and initialize stuff
	printf("Resetting for Movie\n");
	ResetCallback();
	CdInit();
	ResetGraph(0);
	SetGraphDebug(0);
}

boolean playing = 0;

void Movie_Play(const char *path, u32 length) //Play the movie, path is the location of the str file, and length is the frame count
{
	printf("[Movie_Play] starting file %s with %d frames\n", path, length);
	Movie_Prep();
	PlayStr(320, 240, 0, 0, path, length);
}

//move things along
void Movie_Tick()
{
	if (strPlayDone)
	{
		printf("That's all, folks!\n");
		playing = 1; //stop the movie
	}

}

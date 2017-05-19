#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <GL/glx.h>
#include <X11/keysym.h>


#define IMAGE_SIZE 160*120*3
#define MAX_NAME 20
#define MAX_REQUEST MAX_NAME
#define MAX_MOVIE MAX_NAME
#define MAX_PRIORITY 10

#ifndef BOOL
#define BOOL	unsigned char
#undef TRUE
#undef FALSE
#define TRUE	1
#define FALSE	0
#endif

#define SCALE 6

static int playerExit = 0;
int port = 0;
int clientCounter = SCALE;
struct package* requestPackage;

struct client_id{
	int pid;
	char name[MAX_NAME];    /* should be null-terminated */
};

struct package{
	struct client_id ID;
	int prior;
	char request[MAX_REQUEST];    /* should be null-terminated */
	union{
		bool repeat;
		char movie[MAX_MOVIE];    /* should be null-terminated */
		int num;
	}arg;
};

struct wrapper{
	struct package client;
	int index;
};

struct item{
	pthread_t pid;
	int sd;
	struct package client;
	/* Not in Ying's code but a flag of error */
	int finish;
};

Display *dpy[SCALE];
Window window[SCALE];

static int attributeList[] = { GLX_RGBA, GLX_RED_SIZE, 1, None };

void noborder (Display *dpy, Window win) {
  struct {
    long flags;
    long functions;
    long decorations;
    long input_mode;
  } *hints;

  int fmt;
  unsigned long nitems, byaf;
  Atom type;
  Atom mwmhints = XInternAtom (dpy, "_MOTIF_WM_HINTS", FALSE);

  XGetWindowProperty (dpy, win, mwmhints, 0, 4, FALSE, mwmhints,
		      &type, &fmt, &nitems, &byaf,
		      (unsigned char**)&hints);

  if (!hints)
    hints = (void *)malloc (sizeof *hints);

  hints->decorations = 0;
  hints->flags |= 2;

  XChangeProperty (dpy, win, mwmhints, mwmhints, 32, PropModeReplace,
		   (unsigned char *)hints, 4);
  XFlush (dpy);
  free (hints);
}


static void make_window (int width, int height, char *name, int border, int index) {
  XVisualInfo *vi;
  Colormap cmap;
  XSetWindowAttributes swa;
  GLXContext cx;
  XSizeHints sizehints;
  dpy[index] = XOpenDisplay (0);
  if (!(vi = glXChooseVisual (dpy[index], DefaultScreen(dpy[index]), attributeList))) {
    printf ("Can't find requested visual.\n");
    exit (1);
  }
  cx = glXCreateContext (dpy[index], vi, 0, GL_TRUE);

  swa.colormap = XCreateColormap (dpy[index], RootWindow (dpy[index], vi->screen),
				  vi->visual, AllocNone);
  sizehints.flags = 0;

  swa.border_pixel = 0;
  swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask;
  window[index] = XCreateWindow (dpy[index], RootWindow (dpy[index], vi->screen),
			  0, 0, width, height,
			  0, vi->depth, InputOutput, vi->visual,
			  CWBorderPixel|CWColormap|CWEventMask, &swa);
  XMapWindow (dpy[index], window[index]);
  XSetStandardProperties (dpy[index], window[index], name, name,
			  None, (void *)0, 0, &sizehints);

  if (!border)
    noborder (dpy[index], window[index]);

  glXMakeCurrent (dpy[index], window[index], cx);
}


void* Player(void *data);
void* StopPlayer( void *);
void* PlayerMenu();
int cliConn (char *, int );


int cliConn (char *host, int port) {

  struct sockaddr_in name;
  struct hostent *hent;
  int sd;

  if ((sd = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("(cliConn): socket() error");
    exit (-1);
  }

  if ((hent = gethostbyname (host)) == NULL)
    fprintf (stderr, "Host %s not found.\n", host);
  else
    bcopy (hent->h_addr, &name.sin_addr, hent->h_length);

  name.sin_family = AF_INET;
  name.sin_port = htons (port);

  /* connect port */
  if (connect (sd, (struct sockaddr *)&name, sizeof(name)) < 0) {
    perror("(cliConn): connect() error");
    exit (-1);
  }

  return (sd);
}

void* Player(void *data)
{
	struct wrapper *temp = (struct wrapper *)data;
	int sd = cliConn ("127.0.0.1", port);

	unsigned char* newbuffer = (unsigned char*) malloc(sizeof (unsigned char) * (IMAGE_SIZE));
		
	write (sd, &temp->client, sizeof(struct package));
	int counter = 1;
	
	make_window (160, 120, "window" , 1, temp->index);
    	glMatrixMode (GL_PROJECTION);
   	glOrtho (0, 160 , 0, 120, -1, 1);
   	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
   	glMatrixMode (GL_MODELVIEW);
   	glRasterPos2i (0, 0);
	
	do {
		read(sd, newbuffer, IMAGE_SIZE);
		glDrawPixels (160, 120, GL_RGB, GL_UNSIGNED_BYTE,  newbuffer);
		glFlush();
		usleep(200000);
	}while (counter++ != 100);
	
	free(newbuffer);
	close(sd);


/*	read(sd, &temp, sizeof(struct item));
	if (temp.finish == 1) {
		printf("[ERROR] Error response from server!\n Exiting now...");
		pthread_exit(0);
	}

	printf("-------------------------\n");
	printf("[RECEIVED SERVER PACKAGE]\n");
	printf(" id:%d, host:%s, priority:%d, request:%s\n",
		temp.client.ID.pid,
		temp.client.ID.name, temp.client.prior, temp.client.request);
	printf("[SENT CLIENT PACKAGE]\n");
	printf("id:%d, host:%s, priority:%d, request:%s\n",
		requestPackage->ID.pid,
		requestPackage->ID.name, requestPackage->prior, requestPackage->request);
	printf("-------------------------\n");
	int i;
	for(i = 1; i < 100; i++){
		read(sd, &temp, sizeof(struct item));
	}
*/
	clientCounter--;
}

int main(int argc, char **argv)
{
	if(argc < 2) return 0;
	else port = atoi(argv[1]);
	/* Create Player Menu thread*/
	printf("--------------------------------\n");
    	printf("Ying Ye-Fatih Cakir Movie Player Multiple Request Tester\n");
	printf("--------------------------------\n");

	int packetNumber = SCALE;
	char start[] = "start_movie";
	srand(time(NULL));

	struct package load[packetNumber];
	pthread_t player[packetNumber];
	struct wrapper data[packetNumber];

	int c;
	for(c = 0 ; c < packetNumber ; c++)
	{
		  load[c].ID.pid = c;
		  strcpy(load[c].ID.name,"Happy!");
		  load[c].prior = rand() % MAX_PRIORITY + 1;
		  strcpy(load[c].request,start);
		  data[c].client = load[c];
		  data[c].index = c;
		  pthread_create(&player[c], NULL, (void *) Player, &data[c]);

	}

	while(clientCounter != 0){}

	return 0;

}


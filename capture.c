/*
  capture.c

  A half-baked bit of code to pull X11 window framebuffers using
  shared memory.  Runs multi-threaded. Passes the data out raw to
  stdout.

  2014 JSK (kutani@projectkutani.com)

  Public domain. See LICENSE for details.
*/

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <linux/sched.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

const int POOL_MAX = 30;

void x_cleanup(Display* dpy);
static Display *dpy;

typedef struct timer {
	double end;
	double interval;
} timer;

struct timespec sleeptime;
struct timespec spec; 
double ctick;

void
get_millis() {
	clock_gettime(CLOCK_MONOTONIC, &spec);
	ctick = (spec.tv_sec*1000)+round(spec.tv_nsec/1.0e6);
}

timer*
new_timer(double len) {
	timer* t = malloc(sizeof(timer));
	t->end = ctick+len;
	t->interval = len;
	return t;
}

void
free_timer(timer* t) {
	free(t);
}

int
timer_is_done(timer* t) {
	return ( ctick >= t->end );
}

void
timer_reset(timer *t) {
	t->end = ctick+t->interval;
}


typedef struct pixbuf {
	unsigned char* 	data;
	unsigned int 	size;
	struct pixbuf* 	next;
} pixbuf;

void pixbuf_return_to_pool(pixbuf* p);
pixbuf* get_new_pixbuf(int size);
void queue_pixbuf(pixbuf* b);
pixbuf* pop_pixbuf();
int queue_size();

static pixbuf *first = NULL;
static pixbuf *last = NULL;
static int queuesize = 0;
static int fwps = 0;
static pixbuf *pool_f = NULL;
static pixbuf *pool_l = NULL;
static pthread_mutex_t b_lock;
static pthread_mutex_t p_lock;
static int exit_handler = 0;
static int exit_main = 0;

void catch_int(int sig) {
	if( sig == SIGINT ) {
		fputs("Caught SIGINT\n",stderr);
		exit_main = 1;
	}
}

void*
buf_handler() {
	pixbuf* p = NULL;
	int fcnt = 0;
	timer* timeout = new_timer(1000/30);
	timer* fwps_t = new_timer(1000);
	
	signal(SIGINT, catch_int);
	
	while(!exit_handler || first) {
		if( timer_is_done(fwps_t) ) {
			fwps = fcnt;
			fcnt = 0;
			timer_reset(fwps_t);
		}
		p = pop_pixbuf();
		if( p ) {
		
			write(1, p->data, p->size);
			fcnt++;
			//fwrite(p->data, p->size, 1, f);

			pixbuf_return_to_pool(p);
			timer_reset(timeout);
		} else {
			nanosleep(&sleeptime, NULL);
		}
	}
	fprintf(stderr, "handler exiting, queuesize is %i\n",queuesize);
	
	exit_handler = 0;
	return 0;
}


static int poolsize = 0;
void
pixbuf_return_to_pool(pixbuf* p) {
	if( ! p ) return;
	pthread_mutex_lock(&p_lock);
	if( poolsize >= POOL_MAX ) {
		free(p->data);
		free(p);
		pthread_mutex_unlock(&p_lock);
		return;
	}
	p->next = NULL;
	if( pool_f ) {
		pool_l->next = p;
		pool_l = p;
	} else {
		pool_f = p;
		pool_l = p;
	}
	
	poolsize++;
	pthread_mutex_unlock(&p_lock);
}

pixbuf*
get_new_pixbuf(int size) {
	pixbuf* ret;
	pthread_mutex_lock(&p_lock);
	if( pool_f ) {
		ret = pool_f;
		pool_f = pool_f->next;
		poolsize--;
	} else {
		ret = malloc(sizeof(pixbuf));
		if(!ret) {
			fputs("malloc() failed!\n",stderr);
			x_cleanup(dpy);
			exit(EXIT_FAILURE);
		}
		ret->data = malloc(sizeof(unsigned char)*size);
		if(!ret->data) {
			fputs("malloc() data failed!\n",stderr);
			x_cleanup(dpy);
			exit(EXIT_FAILURE);
		}
		ret->size = size;
		ret->next = NULL;
	}
	pthread_mutex_unlock(&p_lock);
	return ret;
}

void
queue_pixbuf(pixbuf* b) {
	pthread_mutex_lock(&b_lock);
	b->next = NULL;
	if( first ) {
		last->next = b;
		last = b;
	} else {
		first = b;
		last = b;
	}
	queuesize++;
	pthread_mutex_unlock(&b_lock);
}

pixbuf*
pop_pixbuf() {
	pixbuf* ret = NULL;
	pthread_mutex_lock(&b_lock);
	if( first ) {
		ret = first;
		first = first->next;
		if( ! first ) last = NULL;
		queuesize--;
	}
	pthread_mutex_unlock(&b_lock);
	return ret;
}


int
queue_size() {
	int r;
	pthread_mutex_lock(&b_lock);
	r = queuesize;
	pthread_mutex_unlock(&b_lock);
	return r;
}


void
free_pool() {
	pixbuf* p;
	pthread_mutex_lock(&p_lock);
	if( ! pool_f ) return;
	p = pool_f->next;
	while(pool_f) {
		free(pool_f->data);
		free(pool_f);
		pool_f = p;
		p = p->next;
	}
	pthread_mutex_unlock(&p_lock);
}

void
free_bufs() {
	pixbuf* p;
	pthread_mutex_lock(&b_lock);
	if( ! first ) return;
	p = first->next;
	while(first) {
		free(first->data);
		free(first);
		first = p;
		p = p->next;
	}
	pthread_mutex_unlock(&b_lock);
}

static XImage* img;
XShmSegmentInfo info;

int
create_shared_image(Display *dpy, int w, int h) {
	unsigned int size;
	img = XShmCreateImage(dpy, NULL, 32, ZPixmap, NULL, &info, w, h);
	
	if( img == NULL ) {
		fprintf(stderr,"XShmCreateImage failed\n");
		exit(EXIT_FAILURE);
	}
	
	size = img->bytes_per_line * img->height;
	info.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT|0777);
	if( info.shmid < 0 ) {
		perror("shmget");
		XDestroyImage(img);
		exit(EXIT_FAILURE);
	}
	
	info.shmaddr = img->data = (char*)shmat(info.shmid, 0, 0);
	if( info.shmaddr == (char*) -1 ) {
		perror("alloc_back_buffer");
		XDestroyImage(img);
		exit(EXIT_FAILURE);
	}
	
	info.readOnly = 0;
	shmctl(info.shmid, IPC_RMID, 0);
	
	if( ! XShmAttach(dpy, &info) ) {
		fputs("Failed attaching shared memory to display\n", stderr);
		XDestroyImage(img);
		exit(errno);
	}
	
	return size;
}

void
x_cleanup(Display* dpy) {
	XShmDetach(dpy, &info);
	XDestroyImage(img);
	XCloseDisplay(dpy);
	shmdt(info.shmaddr);
}

void
set_up_mutexes() {
	if( pthread_mutex_init(&b_lock, NULL) != 0 &&
		pthread_mutex_init(&p_lock, NULL) != 0 ) {
		printf("mutex init failed\n");
		exit(EXIT_FAILURE);
	}
}

void
clean_up_mutexes() {
	pthread_mutex_destroy(&b_lock);
	pthread_mutex_destroy(&p_lock);
}

int
main(int argc, char *argv[]) {
	Window win;
	int w, h;

	sleeptime.tv_sec = 0;
	sleeptime.tv_nsec = 1000000;
	
	signal(SIGINT, catch_int);


	if( ! (dpy = XOpenDisplay(0)) ) {
		fputs("capture: cannot open display\n", stderr);
		exit(EXIT_FAILURE);
	}

	switch(argc) {
	case 4:
		win = (Window)strtol(argv[1], NULL, 0);
		w = atoi(argv[2]);
		h = atoi(argv[3]);
		break;
	case 3:
		win = RootWindow(dpy, DefaultScreen(dpy));
		w = atoi(argv[1]);
		h = atoi(argv[2]);
		break;
	default:
		win = RootWindow(dpy, DefaultScreen(dpy));
		w = 1920;
		h = 1080;
	}
	
	unsigned int size = create_shared_image(dpy, w, h);


	/* Clone our buf handler */
	set_up_mutexes();
	pthread_t pt;
	pthread_create(&pt, NULL, buf_handler, NULL); 

	get_millis();
	timer* main_t = new_timer(1000);
	timer* frame = new_timer(1000/29.976f);
	pixbuf* p;
	int i = 0;
	int fcnt = 0;
	while(!exit_main) {
		get_millis();
		if( timer_is_done(main_t) ) {
			fprintf(stderr,"Capture FPS: %i  Buffer Size: %i  Write FPS: %i  Pool Size: %i\n", fcnt, queuesize, fwps, poolsize);
			fcnt = 0;
			timer_reset(main_t);
		}
		if( timer_is_done(frame) ) {
			XShmGetImage(dpy, win, img, 0, 0, 0xFFFFFFFF);
			p = get_new_pixbuf(size);
			memcpy(p->data, img->data, size);
			queue_pixbuf(p);
			timer_reset(frame);
			i++;
			fcnt++;
		} else {
			nanosleep(&sleeptime, NULL);
		}
	}
	
	exit_handler = 1;
	while(exit_handler) {}
	
	free_timer(main_t);
	free_timer(frame);
	free_pool();
	free_bufs();
	clean_up_mutexes();
	
	fprintf(stderr,"Captured %i frames\n",i);
	
	x_cleanup(dpy);
	
	exit(EXIT_SUCCESS);
}

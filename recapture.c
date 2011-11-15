/*
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 recapture.c 
 
 Coypright (C) 2009 Jeremy Hughes
 Copyright (C) 2009-2011 Guillaume Pellerin

 Play a signal through one port and simultaneously record from another.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sndfile.h>
#include <math.h>
#include <pthread.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>

// Standard includes plus sndfile.h, jack/jack.h and jack/ringbuffer.h. jack.h contains the general API and ringbuffer.h contains jack's implementation of a circular buffer. libsndfile is used for reading and writing audio data.

const char* usage =
 "usage: recapture [ -b bufsize ] [ -i <inports> ] [ -o <outports> ] infile outfile\n"
 "            <inports> and <outports> are `,' separated\n";

#if 1
#define DEBUG(...) (fprintf(stderr, "recapture: "), fprintf(stderr, __VA_ARGS__))
#else
#define DEBUG
#endif
#define MSG(...) (fprintf(stderr, "recapture: "), fprintf(stderr, __VA_ARGS__))
#define ERR(...) (fprintf(stderr, "recapture: error: "), fprintf(stderr, __VA_ARGS__))

// Usage notice and some useful macros.

#define MAX_PORTS 30

// A sensible number given 24 input and output ports. To be a truly general program this would need to be a variable able to be overridden by a command line argument.
// Structs and typedefs

typedef jack_default_audio_sample_t recap_sample_t;

// jack_default_audio_sample_t is a little long.

typedef enum _recap_status {
 IDLE, RUNNING, DONE
} recap_status_t;

// This program starts two extra threads. The main thread sets up a number of callbacks which the jack process runs in realtime. The two extra threads are a read thread and a write thread. This split is necessary because reading and writing cannot occur within a realtime thread without wrecking its realtime guarantees. The read and write threads are connected to the jack thread by a ringbuffer each.

// Before initial synchronization the read thread is IDLE, once reading has commenced it is RUNNING, and when reading is finished it is DONE. The jack processing thread also uses IDLE and DONE.

typedef struct _recap_state {
 volatile int can_play;
 volatile int can_capture;
 volatile int can_read;
 volatile recap_status_t reading;
 volatile recap_status_t playing;
} recap_state_t;

// A single instance of this struct is shared among all three threads. Play and record start when can_play, can_capture, and can_read (which correspond to the three threads) are all true; they finish when reading and playing are both DONE.

typedef struct _recap_io_info {
 pthread_t thread_id;
 char* path;
 SNDFILE* file;
 jack_ringbuffer_t* ring;
 long underruns;
 recap_state_t* state;
} recap_io_info_t;

// Both read and write threads receive an instance of this struct as their only argument.

typedef struct _recap_process_info {
 long overruns;
 long underruns;
 recap_io_info_t* writer_info;
 recap_io_info_t* reader_info;
 recap_state_t* state;
} recap_process_info_t;

// An instance of this struct is passed to the callback responsible for processing the signals.
// Global values

const size_t sample_size = sizeof(recap_sample_t);

// This works out to be the size of a 32 bit float. Not bad given the sample size of CD audio is 16 bits.

typedef recap_sample_t (*next_value_fn) (void*);

pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ready_to_read = PTHREAD_COND_INITIALIZER;
pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ready_to_write = PTHREAD_COND_INITIALIZER;

// Locks and condition variables for each IO thread.

recap_process_info_t* proc_info;

// Needs to be global for signal handling.

/* read only after initialization in main() */
int channel_count_r = 0;
int frame_size_r = 0;
int channel_count_w = 0;
int frame_size_w = 0;
jack_nframes_t ring_size = 65536; /* pow(4, 8) */
jack_port_t* recap_in_ports[MAX_PORTS];
jack_port_t* recap_out_ports[MAX_PORTS];
jack_client_t* client;

// Variables that are not changed subsequent to initialization and therefore do not need to be in thread specific structs.

// channel_count_r and channel_count_w hold the number of read and write signals. The frame_size of each is calculated by multiplying the channel count by the sample size. The default ring_size can be overridden by command line argument. recap_in_ports and recap_out_ports will hold the jack ports this client connects to.
// Helper functions

static size_t array_length(char** array) {
 int i = -1;
 while (array[++i] != NULL);
 return i;
}

// There is probably a more standard if not more general way to do this, but a) I couldn't find it, and b) this function does the job.



int uninterleave(recap_sample_t** buffers, size_t length, size_t count,
                next_value_fn next_value, void* arg) {
 int status = 0;
 int i;
 int k;
 for (i = 0; i < count; i++) {
   for (k = 0; k < length; k++) {
     recap_sample_t sample = next_value(arg);
     if (sample == -1) {
       status = -1;
       break;
     } else {
       buffers[k][i] = sample;
     }
   }
   if (status) break;
 }
 return status;
}

recap_sample_t next_value(void* arg) {
 recap_sample_t sample;
 jack_ringbuffer_t* rb = (jack_ringbuffer_t*) arg;
 size_t count = jack_ringbuffer_read(rb, (char*) &sample, sample_size);
 if (count < sample_size) sample = -1;
 return sample;
}

// libsndfile expects multichannel data to be interleaved. uninterleave() uses a supplied function to read interleaved data and writes it to an array of output buffers (one per channel).

// next_value() simply reads the next sample_size bytes from the supplied ringbuffer.

typedef int (*write_value_fn) (recap_sample_t, void*);

int interleave(recap_sample_t** buffers, size_t length, size_t count,
              write_value_fn write_value, void* arg) {
 int status = 0;
 int i;
 int k;
 for (i = 0; i < count; i++) {
   for (k = 0; k < length; k++) {
     status = write_value(buffers[k][i], arg);
     if (status) break;
   }
   if (status) break;
 }
 return status;
}

int write_value(recap_sample_t sample, void* arg) {
 int status = 0;
 jack_ringbuffer_t* rb = (jack_ringbuffer_t*) arg;
 size_t count = jack_ringbuffer_write(rb, (char*) &sample, sample_size);
 if (count < sample_size) status = -1;
 return status;
}

// interleave() reads data from an array of input buffers and uses the supplied function to write it interleaved.

// write_value() simple writes sample_size bytes to the supplied ringbuffer.

static void recap_mute(recap_sample_t** buffers, int count, jack_nframes_t nframes) {
 int i;
 size_t bufsize = nframes * sample_size;
 for (i = 0; i < count; i++)
   memset(buffers[i], 0, bufsize);
}

// After playing has finished, jack output must be muted (zeroed) otherwise jack will continue playing whatever is left in the output buffers, looping through the last cycle of whatever signal was being played. Definitely not what you want.
// Signal handling

static void cancel_process(recap_process_info_t* info) {
 pthread_cancel(info->reader_info->thread_id);
 pthread_cancel(info->writer_info->thread_id);
}

static void signal_handler(int sig) {
 MSG("signal received, exiting ...\n");
 cancel_process(proc_info);
}

static void jack_shutdown(void* arg) {
 recap_process_info_t* info = (recap_process_info_t*) arg;
 MSG("JACK shutdown\n");
 cancel_process(info);
}

// Signal handing. cancel_process() ensures things are cleaned up nicely. jack_shutdown() is a callback that the jack process calls on exit.
// Thread abstraction

typedef int (*io_thread_fn) (recap_io_info_t*);
typedef void (*cleanup_fn) (void*);

#define FINISHED -1

static void* common_thread(pthread_mutex_t* lock, pthread_cond_t* cond, io_thread_fn fn, cleanup_fn cu, void* arg) {
 int* exit = (int*) malloc(sizeof(int*));
 memset(exit, 0, sizeof(*exit));
 int status = 0;
 recap_io_info_t* info = (recap_io_info_t*) arg;
 pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
 pthread_cleanup_push(cu, arg);
 pthread_mutex_lock(lock);
 while (1) {
   if ((status = fn(info)) != 0) break;
   pthread_cond_wait(cond, lock);
 }
 pthread_mutex_unlock(lock);
 *exit = status;
 if (status == FINISHED) *exit = 0;
 pthread_exit(exit);
 pthread_cleanup_pop(1);
}

// This abstracts out the common parts of setting up a thread and its loop. The supplied io_thread_fn function is executed every iteration until it returns non zero. The supplied cleanup_fn is called whenever the thread is exited. After each iteration the thread waits until it is signalled to continue.

typedef int (*io_test_fn) (recap_io_info_t*);
typedef size_t (*io_size_fn) (recap_io_info_t*);
typedef int (*io_body_fn) (void*, size_t, recap_io_info_t*);

static int io_thread(io_test_fn can_run, io_test_fn is_done, io_size_fn ring_space, io_body_fn body, recap_io_info_t* info) {
 int status = 0;
 if (can_run(info)) {
   size_t space = ring_space(info);
   if (is_done(info)) {
     status = FINISHED;
   } else if (space > 0) {
     void* buf = malloc(space); /* TODO malloc and memset once only */
     status = body(buf, space, info);
     free(buf);
   }
 }
 return status;
}

// Further abstracted out is the code common to the read and write threads. io_test_fn checks when the thread is finished and should exit; io_size_fn returns how much read or write space is available; io_body_fn contains code specific to reading or writing.

// It would be good to malloc void* buf only once at the beginning of the thread to ensure no pagefaults. However, since the allocation does not occur in a realtime thread and no overruns or underruns (dropouts) were observed in testing, it was not a high priority to fix.
// Thread implementation

static int reader_can_run(recap_io_info_t* info) {
 return info->state->can_read;
}

static int writer_can_run(recap_io_info_t* info) {
 return info->state->can_capture;
}

static int reader_is_done(recap_io_info_t* info) {
 return 0;
}

static int writer_is_done(recap_io_info_t* info) {
 return jack_ringbuffer_read_space(info->ring) == 0 && info->state->playing == DONE;
}

static size_t reader_space(recap_io_info_t* info) {
 return jack_ringbuffer_write_space(info->ring);
}

static size_t writer_space(recap_io_info_t* info) {
 return jack_ringbuffer_read_space(info->ring);
}

// Read and write implementations of the above typedefs.

static int reader_body(void* buf, size_t space, recap_io_info_t* info) {
 static int underfill = 0;
 int status = 0;
 sf_count_t nframes = space / frame_size_r;
 sf_count_t frame_count = sf_readf_float(info->file, buf, nframes);
 if (frame_count == 0) {
   DEBUG("reached end of sndfile: %s\n", info->path);
   info->state->reading = DONE;
   status = FINISHED;
 } else if (underfill > 0) {
   ERR("cannot read sndfile: %s\n", info->path);
   status = EIO;
 } else {
   sf_count_t size = frame_count * frame_size_r;
   if (jack_ringbuffer_write(info->ring, buf, size) < size) {
     ++info->underruns;
     ERR("reader thread: buffer underrun\n");
   }
   DEBUG("read %6ld frames\n", (long int) frame_count);
   info->state->reading = RUNNING;
   if (frame_count < nframes && underfill == 0) {
     DEBUG("expected %ld frames but only read %ld,\n", (long int) nframes, (long int) frame_count);
     DEBUG("wait for one cycle to make sure.\n");
     ++underfill;
   }
 }
 return status;
}

// Reader implementation of io_body_fn. It is impossible to tell if the first underfill is caused by IO problems or by reaching the end of the sound file. If the frame count for the following cycle is zero, it is assumed that the end of the file has been reached; otherwise if underfill is greater than zero it must be an IO issue so the thread exits.

// The use of static int underfill means no more than one reader thread can be active during the lifetime of the program.

static int writer_body(void* buf, size_t space, recap_io_info_t* info) {
 int status = 0;
 sf_count_t nframes = space / frame_size_w;
 jack_ringbuffer_read(info->ring, buf, space);
 if (sf_writef_float(info->file, buf, nframes) < nframes) {
   ERR("cannot write sndfile (%s)\n", sf_strerror(info->file));
   status = EIO;
 }
 DEBUG("wrote %5ld frames\n", (long int) nframes);
 return status;
}

// Writer implementatino of io_body_fn.

static int reader_thread_fn(recap_io_info_t* info) {
 return io_thread(&reader_can_run, &reader_is_done,
                  &reader_space, &reader_body, info);
}

static int writer_thread_fn(recap_io_info_t* info) {
 return io_thread(&writer_can_run, &writer_is_done,
                  &writer_space, &writer_body, info);
}

// Read and write implementations of io_thread_fn. Due to the earlier abstraction these definitions are simple.

static void io_cleanup(void* arg) {
 recap_io_info_t* info = (recap_io_info_t*) arg;
 sf_close(info->file);
}

static void io_free(recap_io_info_t* info) {
 jack_ringbuffer_free(info->ring);
}

// io_cleanup() is passed to common_thread as the thread cleanup callback. io_free() is used at the end of main() to free resources which the jack thread may still be using after the IO threads exit.

static void* writer_thread(void* arg) {
 return common_thread(&write_lock, &ready_to_write,
                      &writer_thread_fn, &io_cleanup, arg);
}

static void* reader_thread(void* arg) {
 return common_thread(&read_lock, &ready_to_read,
                      &reader_thread_fn, &io_cleanup, arg);
}

// Functions to start writer and reader threads.
// Main jack callback

static int process(jack_nframes_t nframes, void* arg) {
 recap_process_info_t* info = (recap_process_info_t*) arg;
 recap_state_t* state = info->state;

// The main jack callback. This function must read nframes of signal from the connected input ports and write nframes of signal to the connected output ports. The size of nframes is determined by the caller (jack).

 if (!state->can_play || !state->can_capture || !state->can_read)
   return 0;

// No point reading or writing anything to jack?s buffers if everything isn?t ready to go.

 recap_sample_t* in[MAX_PORTS];
 recap_sample_t* out[MAX_PORTS];
 int i = 0;
 jack_port_t* prt;
 while ((prt = recap_in_ports[i]) != NULL) {
   in[i++] = (recap_sample_t*) jack_port_get_buffer(prt, nframes);
 }
 in[i] = NULL;

 i = 0;
 while ((prt = recap_out_ports[i]) != NULL) {
   out[i++] = (recap_sample_t*) jack_port_get_buffer(prt, nframes);
 }
 out[i] = NULL;

// Get the signal buffers of each input and output port. It is recommended in the jack documentation that these are not cached.

 if (state->playing != DONE && state->reading != IDLE) {

// If state->reading is IDLE there is nothing to play yet, and if state->playing is DONE it is time to exit the program.

   jack_ringbuffer_t* rring = info->reader_info->ring;
   size_t space = jack_ringbuffer_read_space(rring);
   if (space == 0 && state->reading == DONE) {
     state->playing = DONE;
     recap_mute(out, channel_count_r, nframes);
   } else {
     int err = uninterleave(out, channel_count_r, nframes, &next_value, rring);
     if (err) {
       ++info->underruns;
       ERR("control thread: buffer underrun\n");
     }
   }

// This, the guts of the processing is simply uninterleaving the file data and writing it to the buffers of the appropriate output ports. Jack handles the rest.

   jack_ringbuffer_t* wring = info->writer_info->ring;
   int err = interleave(in, channel_count_w, nframes, &write_value, wring);
   if (err) {
     ++info->overruns;
     ERR("control thread: buffer overrun\n");
   }
 }

// Similarly simple. Interleaving the input port data and writing to the writer thread?s ringbuffer.

 if (pthread_mutex_trylock(&read_lock) == 0) {
   pthread_cond_signal(&ready_to_read);
   pthread_mutex_unlock(&read_lock);
 }

 if (pthread_mutex_trylock(&write_lock) == 0) {
   pthread_cond_signal(&ready_to_write);
   pthread_mutex_unlock(&write_lock);
 }
 return 0;
}

// Data has been written to the writer thread?s ringbuffer and removed from the reader thread?s ringbuffer, so signal each thread to begin another iteration.
// Thread setup and running

static int setup_writer_thread(recap_io_info_t* info) {
 int status = 0;
 SF_INFO sf_info;
 sf_info.samplerate = jack_get_sample_rate(client);
 sf_info.channels = channel_count_w;
 sf_info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_32;
 if ((info->file = sf_open(info->path, SFM_WRITE, &sf_info)) == NULL) {
   ERR("cannot open sndfile \"%s\" for output (%s)\n",
       info->path, sf_strerror(info->file));
   jack_client_close(client);
   status = EIO;
 }
 DEBUG("opened to write: %s\n", info->path);
 DEBUG("writing %i channels\n", channel_count_w);
 info->ring = jack_ringbuffer_create(sample_size * ring_size);
 memset(info->ring->buf, 0, info->ring->size);
 info->state->can_capture = 0;
 pthread_create(&info->thread_id, NULL, writer_thread, info);
 return status;
}

// Set up resources for the writer thread then create the thread. This means opening a (multichannel) WAV file to write to, creating a ringbuffer, and touching all its allocated memory to prevent pagefaults later on.

static int setup_reader_thread(recap_io_info_t* info) {
 int status = 0;
 SF_INFO sf_info;
 sf_info.format = 0;
 DEBUG("opened to read: %s\n", info->path);
 if ((info->file = sf_open(info->path, SFM_READ, &sf_info)) == NULL) {
   ERR("cannot read sndfile: %s\n", info->path);
   status = EIO;
 }
 channel_count_r = sf_info.channels;
 frame_size_r = channel_count_r * sample_size;
 DEBUG("reading %i channels\n", channel_count_r);
 if (sf_info.samplerate != 44100) {
   ERR("jack sample rate must be 44100 (is %i)\n", sf_info.samplerate);
   cancel_process(proc_info);
 }
 info->ring = jack_ringbuffer_create(sample_size * ring_size);
 memset(info->ring->buf, 0, info->ring->size);
 info->state->can_read = 0;
 pthread_create(&info->thread_id, NULL, reader_thread, info);
 return status;
}

// Same purpose as the previous function but for the reader thread. Opens a (multichannel) WAV file to read from, creates a ringbuffer, and touches all its memory.

// Similarites between these two functions could probably be extracted into a general setup_io_thread() function.

static int run_io_thread(recap_io_info_t* info) {
 int* status;
 int ret;
 pthread_join(info->thread_id, (void**) &status);
 if (status == PTHREAD_CANCELED) {
   ret = EPIPE;
 } else {
   ret = *status;
   free(status);
 }
 return ret;
}

// Joins to a thread and returns its exit status.

// Port registration and connection

static jack_port_t* register_port(char* name, int flags) {
  jack_port_t* port;
  if ((port = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, flags, 0)) == 0) {
    DEBUG("cannot register port \"%s\"\n", name);
    jack_client_close(client);
    exit(1);
  }
  return port;
}

// Register the named port with the jack process. 

static void connect(const char* out, const char* in) {
 if (jack_connect(client, out, in)) {
   DEBUG("cannot connect port \"%s\" to \"%s\"\n", out, in);
   jack_client_close(client);
   exit(1);
 }
}

static void connect_ports(char** in_names, char** out_names) {
 int i;
 char* s;
 for (i = 0; i < channel_count_w; i++) {
   char prt[32];
   sprintf(prt, "recapture:input_%i", i);
   char* shrt = prt + strlen("recapture:");
   register_port(shrt, JackPortIsInput);
   recap_in_ports[i] = jack_port_by_name(client, prt);
   if ((s = in_names[i]) != NULL) connect(s, prt);
 }
 recap_in_ports[i] = NULL;
 for (i = 0; i < channel_count_r; i++) {
   char prt[32];
   sprintf(prt, "recapture:output_%i", i);
   char* shrt = prt + strlen("recapture:");
   register_port(shrt, JackPortIsOutput);
   recap_out_ports[i] = jack_port_by_name(client, prt);
   if ((s = out_names[i]) != NULL) connect(prt, s);
 }
 recap_out_ports[i] = NULL;
}

// Initialize recap_in_ports and recap_out_ports with this client?s in and out ports, and connect them to the supplied jack ports.
// Set callbacks and handlers

static void set_handlers(jack_client_t* client, recap_process_info_t* info) {
 jack_set_process_callback(client, process, info);
 jack_on_shutdown(client, jack_shutdown, info);
 signal(SIGQUIT, signal_handler);
 signal(SIGTERM, signal_handler);
 signal(SIGHUP, signal_handler);
 signal(SIGINT, signal_handler);
}

// Set jack callbacks and signal handlers.
// Run jack client

static int run_client(jack_client_t* client, recap_process_info_t* info) {
 recap_state_t* state = info->state;

 state->can_play    = 1;
 state->can_capture = 1;
 state->can_read    = 1;

 int reader_status = run_io_thread(info->reader_info);
 int writer_status = run_io_thread(info->writer_info);
 int other_status = 0;

 if (info->overruns > 0) {
   ERR("recapture failed with %ld overruns.\n", info->overruns);
   ERR("try a bigger buffer than -b %" PRIu32 ".\n", ring_size);
   other_status = EPIPE;
 }
 long underruns = info->underruns + info->reader_info->underruns;
 if (underruns > 0) {
   ERR("recapture failed with %ld underruns.\n", underruns);
   ERR("try a bigger buffer than -b %" PRIu32 ".\n", ring_size);
   other_status = EPIPE;
 }
 return reader_status || writer_status || other_status;
}

// Run reader and writer threads, and return their status.

// Looks like can_play, can_capture, and can_read could be collapsed in to one field.
// Argument parsing

static void split_names(char* str, char** list) {
 int i = 0;
 char* s = strtok(str, ",");
 while (s != NULL) {
   list[i++] = s;
   s = strtok(NULL, ",");
 }
 list[i] = NULL;
}

// Port names given on the commandline are comma separated.

static void parse_arguments(int argc, char** argv, char** in_names, char** out_names) {
 char* optstring = "b:i:o:h";
 struct option long_options[] = {
   { "help", 0, 0, 'h' },
   { "bufsize", 1, 0, 'b' },
   { "inports", 1, 0, 'i' },
   { "outports", 1, 0, 'o' },
   { 0, 0, 0, 0 }
 };

 int c;
 int longopt_index = 0;
 int show_usage = 0;
 extern int optind;
 extern int opterr;
 opterr = 0;
 while ((c = getopt_long(argc, argv, optstring, long_options, &longopt_index)) != -1) {
   switch (c) {
   case 1:
     break;
   case 'h':
     show_usage = 1;
     break;
   case 'b':
     ring_size = atoi(optarg);
     break;
   case 'i':
     split_names(optarg, in_names);
     break;
   case 'o':
     split_names(optarg, out_names);
     break;
   default:
     show_usage = 1;
     break;
   }
 }

 if (show_usage == 1 || argc - optind < 2) {
   MSG("%s", usage);
   exit(1);
 }
}

// Straightforward argument handling.
// main

int main(int argc, char** argv) {
 recap_process_info_t info;
 recap_io_info_t reader_info;
 recap_io_info_t writer_info;
 recap_state_t state;
 memset(&info, 0, sizeof(info));
 memset(&reader_info, 0, sizeof(reader_info));
 memset(&writer_info, 0, sizeof(writer_info));
 memset(&state, 0, sizeof(state));
 info.reader_info = &reader_info;
 info.writer_info = &writer_info;
 info.state = &state;
 reader_info.state = &state;
 writer_info.state = &state;
 state.can_play = 0;
 state.reading = IDLE;
 state.playing = IDLE;
 proc_info = &info;

// Initialize info instances and touch their memory to prevent pagefaults.

 char* in_port_names[MAX_PORTS];
 char* out_port_names[MAX_PORTS];
 parse_arguments(argc, argv, in_port_names, out_port_names);

 proc_info->reader_info->path = argv[optind];
 proc_info->writer_info->path = argv[++optind];

// Port names and file paths.

 channel_count_w = array_length(in_port_names);
 frame_size_w = channel_count_w * sample_size;

// Writer thread channel count and frame size. Those for the reader thread are taken from the input file in setup_reader_thread().

 DEBUG("%s\n", proc_info->reader_info->path);
 if ((client = jack_client_open("recapture", JackNullOption, NULL)) == 0) {
   ERR("jack server not running?\n");
   exit(1);
 }

 set_handlers(client, proc_info);

// Connect to jack and set up jack and signal callbacks.

 int status = 0;
 if (!(status = setup_writer_thread(proc_info->writer_info)) &&
     !(status = setup_reader_thread(proc_info->reader_info))) {
   if (jack_activate(client)) {
     ERR("cannot activate client\n");
     status = 1;
   } else {
     connect_ports(in_port_names, out_port_names);
     DEBUG("connected ports\n");
     status = run_client(client, proc_info);
     jack_client_close(client);
   }
 }

// Provided the IO threads execute ok, run this client and then close it once run_client() returns.

 io_free(proc_info->reader_info);
 io_free(proc_info->writer_info);
 return status;
}

 

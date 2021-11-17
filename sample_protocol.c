//usr/bin/gcc "$0" -lpthread && exec ./a.out; exit
/* for very simple code bases a makefile is not necessary. but some compile information
   still useful. for this purpose this is an interesting technique to execute a C source
   file in a unix like environment (after chmod +x)
   more information about this shebang: https://stackoverflow.com/a/29709521
*/

/* in order to allow the libc library to conform to multiple standards, feature macros
   are introduced to enable/disable certain feature sets of the library. these macros
   should be defined before any system related include files are included.
   see details: https://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html
   Macro: _GNU_SOURCE
   If you define this macro, everything is included: 
     ISO C89, ISO C99, POSIX.1, POSIX.2, BSD, SVID, X/Open, LFS, and GNU extensions. In the 
     cases where POSIX.1 conflicts with BSD, the POSIX definitions take precedence.
   here we need this macro to get the following features:
   gettid() - get thread identification
   fcntl() commands: F_SETSIG, F_OWNER_TID, F_SETOWN_EX 
*/
#define _GNU_SOURCE


#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <limits.h>


/* csv parser include only library
   https://github.com/gega/ccsv
*/
#include "ccsv.h" // wget https://raw.githubusercontent.com/gega/ccsv/main/ccsv.h

/* message framing include only library
   https://github.com/gega/mmfl
*/
#include "mmfl.h" // wget https://raw.githubusercontent.com/gega/mmfl/master/mmfl.h


/* this is the version of the slave part, and in real applications
   the version info usually located in a shared include, this is 
   why it is added here in the common area
*/
#define VERSION "1.0.3"


/* necessary macros for stringification. for details see the 
   function HAL_UART_Transmit() below
*/
#define xstr(s) str(s)
#define str(s) #s

/* static assert to make sure that some compile time known values are in sync
   in this code it is used to check the number of errors in the errnos enum are the
   same as the number of error strings in the errors array -- so if some forgot to 
   add the string description of a newly created errno, the code fails to compile.
   see: https://stackoverflow.com/a/3385694
*/
#define STATIC_ASSERT( condition, name )  typedef char assert_failed_ ## name [ (condition) ? 1 : -1 ];


/* Extra structure just for the simulation
   holds the two FIFOs for the two directions of the communication
   and a mutex which will start the master application after the 
   slave part fully initialized
*/ 
struct fifos
{
  char *f1;
  char *f2;
  pthread_mutex_t mut;
};

// TODO: protocol definition, must be sorted!
struct protocol
{
  char *command;                 // protocol commands
  int (*fn)(struct ccsv *,int);  // function pointer for processing the commands:
                                 //   struct ccsv *csv - csv pointer which has the rest of the parameters
                                 //   int fout         - file descriptor for the reply
};

// TODO: structure to store background 
struct task
{
  int progress;                  // task in progress flag
  int duration;                  // task duration in ms
  int p1;                        // parameters
  int p2;
  int p3;
  struct timespec start;         // start time of the task
};

// TODO: error enums
enum errnos
{
  ERR_OK,                        // no error: OK
  ERR_NOCOMMAND,
  ERR_NOSLOT,
  ERR_NOTSUPPORTED,
  ERR_IDXOUTOFRANGE,
  ERR_INPROGRESS,
  ERR_TASKNOTFOUND,
  ERRCNT                         // number of errors automatically assigned
};

// TODO: string names for the errors
static char *errors[]=
{
  // TODO: new type of array assignment
  [ERR_OK]="OK",
  [ERR_NOCOMMAND]="command not supported",
  [ERR_NOSLOT]="no task slots are available",
  [ERR_NOTSUPPORTED]="not supported",
  [ERR_IDXOUTOFRANGE]="index is out of range",
  [ERR_INPROGRESS]="task in progress",
  [ERR_TASKNOTFOUND]="task not found",
};

// protocol buffer size, must be large enough to hold the longest message in the protocol
#define BUFSIZE (64)

// transmission simulation: minimum and maximum waiting times between bytes transmitted (unit is us)
#define MINWAIT_US (10000)
#define MAXWAIT_US (100000)

// TODO: nice solution to get the size of a statically declared array
#define SIZEOF_ARR(arr) (sizeof((arr))/sizeof((arr)[0]))

/* convert struct timespec struct to milliseconds
   from man clock_gettime:
             struct timespec {
                 time_t   tv_sec;        // seconds
                 long     tv_nsec;       // nanoseconds
             };
   macro arguments are in parenthesis for safe(er) evaluation
   see: https://stackoverflow.com/a/10820434
*/
#define TS2MS(ts) (((ts)->tv_sec*1000L)+(((ts)->tv_nsec/1000000L)))


// check if there are the same number of errno enums as the number of strings in the errors[] array
// if not, perhaps one or more error string(s) are missing and the code will refuse to compile
STATIC_ASSERT(SIZEOF_ARR(errors)==ERRCNT,errors_array_and_errnos_enums_should_be_same_length);


// https://www.devcoons.com/find-elapsed-time-using-monotonic-clocks-linux/
// get the time difference between two timespec timestamps
static struct timespec get_elapsed_time(struct timespec* start, struct timespec* stop)
{
  struct timespec elapsed_time;
  if ((stop->tv_nsec - start->tv_nsec) < 0) 
  {
    elapsed_time.tv_sec = stop->tv_sec - start->tv_sec - 1;
    elapsed_time.tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
  } 
  else 
  {
    elapsed_time.tv_sec = stop->tv_sec - start->tv_sec;
    elapsed_time.tv_nsec = stop->tv_nsec - start->tv_nsec;
  }
  return elapsed_time;
}

// simulated uart receiver using mmfl, normally it's triggered by 
// UART hardware irq
static int HAL_UART_Receive(rb_t *rbv, char **msg)
{
  char *m=NULL;
  int len;
  // using mmfl library to do the message framing for us
  // here we are using the "read" call for reading from 
  // internal comm buffers, this should be replaced by a 
  // properly prepared posix compatible read() UART HAL call
  // (which doesn't have to be complex, it could work in a
  // minimalistic way when one read()-like call fills only
  // one byte)
  // when msg is NULL, a whole message is not received yet,
  // we need to wait for additional data and call the 
  // RB_READMSG() again with the same rb_t * context pointer
  // the subsequent calls are fills the buffer with more data
  // and when the length is enough for a full message, it will
  // return with a non-NULL msg pointer and the upper layers
  // can start processing the newly arrived message
  RB_READMSG(rbv,m,len,read);
  if(NULL!=msg) *msg=m;
  return(len);
}

// simulated uart transmitter with random delays and mmfl marshalling
static void HAL_UART_Transmit(int fd, char *string)
{
  int size,i,ps;
  // This technique is stringification and explained here:
  // https://gcc.gnu.org/onlinedocs/gcc-3.4.3/cpp/Stringification.html
  // xstr(INT_MAX) will return the string version of the maximum value
  // of an int in the current architecture (macro defined in limits.h)
  // this helps to allocate the minimum size of buffer we need but not
  // less than that. For the premark (which is message framing dependent
  // and here it is defined in mmfl.h) we need to allocate one byte for
  // message start marker, a large enough buffer to store the length of
  // the message (as ascii integer) and a space which marks the beginning
  // of the payload.
  char premark[]="\n" xstr(INT_MAX) " ";
  char *p;
  
  // defensive programming with yoda conditions
  // https://en.wikipedia.org/wiki/Yoda_conditions
  if(0<=fd&&NULL!=string)
  {
    size=strlen(string);

    // using snprintf() is the best way to prevent buffer overflows when
    // generating strings to statically allocated buffers. the following
    // call never overwrites the premark buffer. if the string would be
    // larger than the buffer size, the string will be truncated and the
    // return value of snprintf() will be larger or equal than the size 
    // of the buffer, this is a bug in the code, it should never happen,
    // as long as the size is "int" and positive. (INT_MIN can overflow 
    // the buffer as the additional '-' sign adds one more byte, but we
    // are not planning to handle negative sizes)
    ps=snprintf(premark,sizeof(premark),"\n%d ",size);
    // check if premark truncated, just in case...
    if(ps<sizeof(premark))
    {
      // sending the message out one byte at a time, in order to simulate
      // a slow physical layer
      for(i=0,p=premark;i<(ps+size);i++)
      {
        // when the premark pointer points to the closing '\0' we can
        // start sending out the layload bytes
        if(*p=='\0') write(fd,&string[i-ps],1);
        else write(fd,p++,1);
        // normally the content of a write will be read by the read on the other side
        // and this behavior hides the stream nature of the underlying layers
        // to use one byte writes, sync and delays we are trying to simulate a more
        // realistic environment where the read on the other side can return in the
        // middle of a message -- just like it can in real life as well just less often
        fsync(fd);
        // wait for a random time to simulate a slow environment
        usleep(MINWAIT_US+rand()%(MAXWAIT_US-MINWAIT_US));
      }
    }
    else
    {
      // runtime error, the code is unusable, we need to find and fix the bug in the
      // premark[] allocation before using the program
      fprintf(stderr,"ERROR: premark buffer is too small for %d size message!\n",size);
      exit(1);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// slave part
//
/* the two communicating parties are simulated here as threads. one thread is created for the slave
   and one for the master application. in a real application those are located in different machines
   or at least on different chips on the same board.
   here are the slave functions and variables which shouldn't be visible by the master part
*/


/* number of predefined background task slots
   this is the number of background tasks we can accept at a time. as tasks are finished
   the cleared slots can be reused for other bacground task requests. this is a common 
   technique in embedded solutions to reduce both the memory usage and the resource 
   overload on the target system
*/
#define BACKGROUND_TASK_COUNT (3)

// define a simulated random duration interval for the simulated background tasks
// here the background tasks will take 2 to 8 seconds to complete
#define SLOW_MIN_DUR_MS (2000)
#define SLOW_MAX_DUR_MS (8000)

/* allocate the memory for holding the housekeeping data for the background tasks
   and initalize the whole array to 0. initializing to 0 is not strictly needed here
   as it is defined as a static global (which is initilized to zero on startup anyway
   because 6.7.8/10 of C99 standard http://www.open-std.org/JTC1/sc22/wg14/www/docs/n1256.pdf
   requires than all static objects should be initialized prior program startup)
*/
static struct task slv_slow_in_progress[BACKGROUND_TASK_COUNT]={0};

/* command reading buffer for the slave part
*/
static char slv_buf[BUFSIZE];

/* volatile flag variable for moving the SIGIO signal out of signal context (or interrupt 
   context in an embedded environment). the signal/interrupt context is restrictive, the
   handler code should be small, quick and should not use complex library functions so the
   best is just to set a flag which the main code can check. the variable should be 
   volatile which prevents the compiler to cache the value of it and ensure that every time
   the main code reads this variable, it will be checked in the memory location associated
   with the variable instead of caching the last value in a register or optimize out the
   complete read
*/
static volatile int slave_sigio=0;


/* these are the protocol processing functions
   all have the same signature:
   return value: int -- used only for this sample application to request a quit
                        normal embedded protocols are never quit, the closest
                        usual functionality is the software update which often 
                        ends up in a "reboot"
   struct ccsv *csv  -- this is the ccsv structure pointer where the first column
                        is already consumed (that is the name of the command)
                        the remaining columns are the arguments of the commands
   int fout          -- file descriptor where the response should be written
*/

// VERSION command
/* returns the version of the slave side of the software
*/
static int slave_cmd_vers(struct ccsv *csv, int fout)
{
  snprintf(slv_buf,sizeof(slv_buf),"%d,%s",ERR_OK,VERSION);
  HAL_UART_Transmit(fout,slv_buf);

  return(0);
}

// SLOW command
/*
*/
static int slave_cmd_slow(struct ccsv *csv, int fout)
{
  int ret;
  int i;
  struct task *tsk;
  
  // look for next available task slot
  for(i=0,tsk=slv_slow_in_progress;i<(BACKGROUND_TASK_COUNT-1)&&tsk->progress!=0;++tsk,++i);
  if(tsk->progress==0)
  {
    ret=ERR_OK;
    tsk->progress=1;
    tsk->duration=SLOW_MIN_DUR_MS+rand()%(SLOW_MAX_DUR_MS-SLOW_MIN_DUR_MS);
    tsk->p1=atoi(ccsv_nextfield(csv,NULL));
    tsk->p2=atoi(ccsv_nextfield(csv,NULL));
    tsk->p3=atoi(ccsv_nextfield(csv,NULL));
    clock_gettime(CLOCK_MONOTONIC, &tsk->start);
    snprintf(slv_buf,sizeof(slv_buf),"%d,%d",ret,i);
  }
  else
  {
    ret=ERR_NOSLOT;
    snprintf(slv_buf,sizeof(slv_buf),"%d,%s",ret,errors[ret]);
  }
  HAL_UART_Transmit(fout,slv_buf);
  
  return(ret);
}

// QUERY command
/*
*/
static int slave_cmd_qery(struct ccsv *csv, int fout)
{
  int ret;
  int idx;
  struct timespec now,elapsed;

  idx=atoi(ccsv_nextfield(csv,NULL));
  if(idx>=0&&idx<BACKGROUND_TASK_COUNT)
  {
    if(slv_slow_in_progress[idx].progress!=0)
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      elapsed=get_elapsed_time(&slv_slow_in_progress[idx].start,&now);
      if(TS2MS(&elapsed)>=slv_slow_in_progress[idx].duration)
      {
        ret=ERR_OK;
        snprintf(slv_buf,sizeof(slv_buf),"%d,%d,%d,%d",ret, 1+slv_slow_in_progress[idx].p1, 1+slv_slow_in_progress[idx].p2, 1+slv_slow_in_progress[idx].p3);
        slv_slow_in_progress[idx].progress=0;
      }
      else
      {
        ret=ERR_INPROGRESS;
        snprintf(slv_buf,sizeof(slv_buf),"%d,%s",ret,errors[ret]);
      }
    }
    else
    {
      ret=ERR_TASKNOTFOUND;
      snprintf(slv_buf,sizeof(slv_buf),"%d,%s",ret,errors[ret]);
    }
  }
  else
  {
    ret=ERR_IDXOUTOFRANGE;
    snprintf(slv_buf,sizeof(slv_buf),"%d,%s",ret,errors[ret]);
  }
  HAL_UART_Transmit(fout,slv_buf);

  return(ret);
}

// QUIT command
/*
*/
static int slave_cmd_quit(struct ccsv *csv, int fout)
{
  snprintf(slv_buf,sizeof(slv_buf),"%d",ERR_OK);
  HAL_UART_Transmit(fout,slv_buf);

  return(-1);
}

/* compare the command of a protocol descriptor and a string key
   this is a helper function for the bsearch() used to identify the received command
   bsearch() will do a binary search in the protocol descriptor table to find the 
   command in it. for the search it needs a compare function.
   key   -- first parameter always the received command string
   proto -- protocol descriptor pointer, points to a valid element in the 
            struct protocol proto[] array
   return: same return is expected as from strcmp()
*/
static int cmp_protocol(const void *key, const void *proto)
{
  // strncmp() is used to compare only the first significant characters of the defined
  // protocol commands. casts needed to get the proper types from the generic void* 
  // arguments.
  return(strcmp((const char *)key,((struct protocol *)proto)->command));
}

/* simulated interrupt handler
   signals are used to simulate interrupts in this sample application
   this handler executed at any point of time and signal handlers are 
   allowed to use only a small subset of the C library so the easiest 
   tactic is to set a flag or increment a counter which later can be
   visible from the main application context.
   see man 7 signal; chapter "Execution of signal handlers"
   this is more or less true for interrupt handlers but that is an
   even more restricted environment.
*/
void slave_sigio_handler(int sig, siginfo_t *info, void *ucontext)
{
  // handler is executed when data became readable on the 
  // info.si_fd file descriptor
  slave_sigio++;
}

// slave entity (process/ecu/etc)
static void *uart_slave(void *ud)
{
  struct fifos *fs=(struct fifos *)ud;
  struct sigaction sact={0};
  struct f_owner_ex fex={0};
  rb_t rbv;
  int err;
  char *msg;
  char buf[BUFSIZE];
  struct ccsv csv;
  static struct protocol proto[]=
  {
    /* Important that the command list must be alphabetically sorted
       by the command name field in order to get binary search working
    */
    { "QUERY", 		slave_cmd_qery },
    { "QUIT", 		slave_cmd_quit },
    { "SLOW", 		slave_cmd_slow },
    { "VERSION", 	slave_cmd_vers },
  };
  struct protocol *cmd;

  // setting up our fifos
  // for slave f1 is input, f2 is output
  int fin=open(fs->f1,O_RDWR);
  int fout=open(fs->f2,O_RDWR);

  // setup signal handler for SIGIO on fin
  sact.sa_sigaction=slave_sigio_handler;
  sact.sa_flags=SA_NODEFER|SA_SIGINFO;
  if(0!=sigaction(SIGUSR1,&sact,NULL)) perror("sigaction SIGUSR1");

  // set async flag to receive signals
  if(0!=fcntl(fin,F_SETFL,fcntl(fin,F_GETFL)|O_ASYNC|O_NONBLOCK)) perror("fcntl ASYNC");

  // set receiver pid  
  fex.type=F_OWNER_TID;
  fex.pid=gettid();
  if(0!=fcntl(fin,F_SETOWN_EX,&fex)) perror("fcntl SETOWN"); 

  // ask kernel to send signals when fin became readable without blocking
  if(0!=fcntl(fin,F_SETSIG,SIGUSR1)) perror("fcntl SIGUSR1");
  
  // mmfl init needed
  RB_INIT(&rbv,buf,sizeof(buf),fin);
  
  // setup is ready, let's start the master
  pthread_mutex_unlock(&fs->mut);

  // business logic for slave --------------
  
  for(err=0;err>=0;)
  {
    if(0==slave_sigio) sleep(10);
    if(0<slave_sigio)
    {
      // we have something to read
      HAL_UART_Receive(&rbv,&msg);
      slave_sigio=0;
      if(NULL!=msg)
      {
        printf("%s: msg received: '%s'\n",__func__,msg);

        // ccsv used for csv parsing
        ccsv_init(&csv,msg);
        /* bsearch() is a standard library function but there in embedded environments sometimes
           only a minimal subset of the library functions are supperted. fortunately the function
           is simple and can be found easily searching for "bsearch.c"
           The libraries are organized in a way that every API function of them has a separate 
           .c source file. The reason behind it, that the linker has an object file granularity 
           and separating every API function to separate files can reduce size of the final
           linked binary executable.
           
           This is one possible implementation found in musl stdlib implementation

              void *bsearch(const void *key, 
                            const void *base, 
                            size_t nel, 
                            size_t width, 
                            int (*cmp)(const void *, const void *)
                           )
              {
                      void *try;
                      int sign;
                      while (nel > 0) {
                              try = (char *)base + width*(nel/2);
                              sign = cmp(key, try);
                              if (!sign) return try;
                              else if (nel == 1) break;
                              else if (sign < 0)
                                      nel /= 2;
                              else {
                                      base = try;
                                      nel -= nel/2;
                              }
                      }
                      return NULL;
              }

        */
        if(NULL!=(cmd=bsearch(ccsv_nextfield(&csv,NULL),proto,SIZEOF_ARR(proto),sizeof(struct protocol),cmp_protocol)))
        {
          // execute protocol command
          err=cmd->fn(&csv,fout);
        }
        else
        {
          snprintf(buf,sizeof(buf),"%d,%s",ERR_NOCOMMAND,errors[ERR_NOCOMMAND]);
          HAL_UART_Transmit(fout,buf);
        }
      }
    }
    else break; // not expecting other signals
  }

  // cleanup
  
  close(fin);
  close(fout);

  return(NULL);
}



///////////////////////////////////////////////////////////////////////////////////////////////////
// master part


// master entity (upper layer)
static void *uart_master(void *ud)
{
  struct fifos *fs=(struct fifos *)ud;
  char buf[BUFSIZE];
  rb_t rbv;
  char *msg,*fld;
  int i,j,st,sum;
  struct ccsv csv;
  int task[4];

  for(i=0;i<SIZEOF_ARR(task);i++) task[i]=-1;

  // wait for the slave to initialize  
  pthread_mutex_lock(&fs->mut);

  // setting up our fifos
  // for master f2 is input, f1 is output
  int fin=open(fs->f2,O_RDWR);
  int fout=open(fs->f1,O_RDWR);

  // mmfl init (buffer doesn't need to be large,
  // the minimum buffer size is the maximum length
  // of one valid message)
  RB_INIT(&rbv,buf,sizeof(buf),fin);
  
  // business logic for master -----------------------
  
  // VERSION
  snprintf(buf,sizeof(buf),"VERSION");
  HAL_UART_Transmit(fout,buf);
  HAL_UART_Receive(&rbv,&msg);
  printf("%s: msg received: '%s'\n",__func__,msg);

  // using ccsv for csv parsing
  ccsv_init(&csv,msg);
  for(i=0;NULL!=(fld=ccsv_nextfield(&csv,NULL));i++) printf("%s: field #%d: '%s'\n",__func__,i,fld);

  // SLOW command 1
  snprintf(buf,sizeof(buf),"SLOW,1,2,3");
  HAL_UART_Transmit(fout,buf);
  HAL_UART_Receive(&rbv,&msg);
  ccsv_init(&csv,msg);
  if((st=atoi(ccsv_nextfield(&csv,NULL)))==ERR_OK) task[0]=atoi(ccsv_nextfield(&csv,NULL));
  else printf("%s: ERROR %d: %s\n",__func__,st,ccsv_nextfield(&csv,NULL));
    
  // SLOW command 2
  snprintf(buf,sizeof(buf),"SLOW,6,7,8");
  HAL_UART_Transmit(fout,buf);
  HAL_UART_Receive(&rbv,&msg);
  ccsv_init(&csv,msg);
  if((st=atoi(ccsv_nextfield(&csv,NULL)))==ERR_OK) task[1]=atoi(ccsv_nextfield(&csv,NULL));
  else printf("%s: ERROR %d: %s\n",__func__,st,ccsv_nextfield(&csv,NULL));

  // SLOW command 3
  snprintf(buf,sizeof(buf),"SLOW,97,32,34");
  HAL_UART_Transmit(fout,buf);
  HAL_UART_Receive(&rbv,&msg);
  ccsv_init(&csv,msg);
  if((st=atoi(ccsv_nextfield(&csv,NULL)))==ERR_OK) task[2]=atoi(ccsv_nextfield(&csv,NULL));
  else printf("%s: ERROR %d: %s\n",__func__,st,ccsv_nextfield(&csv,NULL));

  // SLOW command 4
  snprintf(buf,sizeof(buf),"SLOW,0,0,0");
  HAL_UART_Transmit(fout,buf);
  HAL_UART_Receive(&rbv,&msg);
  ccsv_init(&csv,msg);
  if((st=atoi(ccsv_nextfield(&csv,NULL)))==ERR_OK) task[3]=atoi(ccsv_nextfield(&csv,NULL));
  else printf("%s: ERROR %d: %s\n",__func__,st,ccsv_nextfield(&csv,NULL));

  // check on tasks
  do 
  {
    sum=0;
    for(j=0;j<SIZEOF_ARR(task);j++)
    {
      if(task[j]>=0)
      {
        snprintf(buf,sizeof(buf),"QUERY,%d",task[j]);
        HAL_UART_Transmit(fout,buf);
        HAL_UART_Receive(&rbv,&msg);
        ccsv_init(&csv,msg);
        if((st=atoi(ccsv_nextfield(&csv,NULL)))==ERR_OK)
        {
          for(i=0;NULL!=(fld=ccsv_nextfield(&csv,NULL));i++) printf("%s: field #%d: '%s'\n",__func__,i,fld);
          task[j]=-1;
        }
        else
        {
          printf("%s: ERROR %d: %s\n",__func__,st,ccsv_nextfield(&csv,NULL));
          usleep(0);
        }
        sum+=st;
      }
    }
  } while(sum>0);

  // QUIT (last command)
  snprintf(buf,sizeof(buf),"QUIT");
  HAL_UART_Transmit(fout,buf);

  HAL_UART_Receive(&rbv,&msg);
  printf("%s: msg received: '%s'\n",__func__,msg);
  
  // cleanup  
  close(fin);
  close(fout);

  return(NULL);
}


// wrapper code
int main(void)
{
  char temp1[]="/tmp/uart1_XXXXXX";
  char temp2[]="/tmp/uart2_XXXXXX";
  char *temp1d,*temp2d;
  char fifoname1[64];
  char fifoname2[64];
  pthread_t t1,t2;
  struct fifos fs;
  
  // setup comm env
  temp1d=mkdtemp(temp1);
  temp2d=mkdtemp(temp2);
  snprintf(fifoname1,sizeof(fifoname1),"%s/uf",temp1d);
  snprintf(fifoname2,sizeof(fifoname2),"%s/uf",temp1d);
  fs.f1=fifoname1;
  fs.f2=fifoname2;
  mkfifo(fifoname1,0666);
  mkfifo(fifoname2,0666);
  
  // make sure that slave inited first
  pthread_mutex_init(&fs.mut,NULL);
  pthread_mutex_lock(&fs.mut);

  // start the entities
  pthread_create(&t2,NULL,uart_master,&fs);
  pthread_create(&t1,NULL,uart_slave,&fs);
  
  // collect the remains
  pthread_join(t1,NULL);
  pthread_join(t2,NULL);

  // cleanup  
  unlink(fifoname1);
  unlink(fifoname2);
  rmdir(temp1d);
  rmdir(temp2d);

  return(0);
}

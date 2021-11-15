// gcc -Wall -o sample_protocol sample_protocol.c -lpthread
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

// csv parser library
// https://github.com/gega/ccsv
#include "ccsv.h"

// message framing library
// https://github.com/gega/mmfl
#include "mmfl.h"


#define VERSION "1.0.2"


#define xstr(s) str(s)
#define str(s) #s

#define STATIC_ASSERT( condition, name )  typedef char assert_failed_ ## name [ (condition) ? 1 : -1 ];


struct fifos
{
  char *f1;
  char *f2;
  pthread_mutex_t mut;
};

struct protocol
{
  char command[5];
  int (*fn)(struct ccsv *,int);
};

struct task
{
  int progress;
  int duration;
  int p1;
  int p2;
  int p3;
  struct timespec start;
};

enum errnos
{
  ERR_OK,
  ERR_NOCOMMAND,
  ERR_NOSLOT,
  ERR_NOTSUPPORTED,
  ERR_IDXOUTOFRANGE,
  ERR_INPROGRESS,
  ERR_TASKNOTFOUND,
  ERRCNT
};

static char *errors[]=
{
  [ERR_OK]="OK",
  [ERR_NOCOMMAND]="command not supported",
  [ERR_NOSLOT]="no task slots are available",
  [ERR_NOTSUPPORTED]="not supported",
  [ERR_IDXOUTOFRANGE]="index is out of range",
  [ERR_INPROGRESS]="task in progress",
  [ERR_TASKNOTFOUND]="task not found",
};

#define BUFSIZE (64)
#define MINWAIT_US (10000)
#define MAXWAIT_US (100000)
#define SIZEOF_ARR(arr) (sizeof((arr))/sizeof((arr)[0]))
#define TS2MS(ts) (((ts)->tv_sec*1000L)+(((ts)->tv_nsec/1000000L)))


STATIC_ASSERT(SIZEOF_ARR(errors)==ERRCNT,errors_array_and_errnos_enums_should_be_same_length);


// https://www.devcoons.com/find-elapsed-time-using-monotonic-clocks-linux/
static struct timespec get_elapsed_time(struct timespec* start,struct timespec* stop)
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

// simulated uart receiver using mmfl, normally it's triggered by irq
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
  rb_readmsg(rbv,m,len,read);
  if(NULL!=msg) *msg=m;
  return(len);
}

// simulated uart transmitter with random delays and mmfl marshalling
static void HAL_UART_Transmit(int fd, char *string)
{
  int size,i,ps;
  char premark[]="\n" xstr(INT_MAX) " "; // enough for size
  char *p;
  
  if(0<=fd&&NULL!=string)
  {
    size=strlen(string);
    ps=snprintf(premark,sizeof(premark),"\n%d ",size);
    for(i=0,p=premark;i<(ps+size);i++)
    {
      if(*p=='\0') write(fd,&string[i-ps],1);
      else write(fd,p++,1);
      // normally the content of a write will be read by the read on the other side
      // and this behavior hides the stream nature of the underlying layers
      // to use one byte writes, sync and delays we are trying to simulate a more
      // realistic environment where the read on the other side can return in the
      // middle of a message -- just like it can in real life as well just less often
      fsync(fd);
      usleep(MINWAIT_US+rand()%(MAXWAIT_US-MINWAIT_US));
    }
  }
}

///////////////////
// slave globals
#define BACKGROUND_TASK_COUNT (3)
#define SLOW_MIN_DUR_MS (2000)
#define SLOW_MAX_DUR_MS (8000)
static struct task slv_slow_in_progress[BACKGROUND_TASK_COUNT]={0};
static char slv_buf[BUFSIZE];
static volatile int slave_sigio=0;


static int slave_cmd_vers(struct ccsv *csv, int fout)
{
  snprintf(slv_buf,sizeof(slv_buf),"%d,%s,%d",ERR_OK,VERSION,1+atoi(ccsv_nextfield(csv,NULL)));
  HAL_UART_Transmit(fout,slv_buf);

  return(0);
}

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

static int slave_cmd_quit(struct ccsv *csv, int fout)
{
  snprintf(slv_buf,sizeof(slv_buf),"%d",ERR_OK);
  HAL_UART_Transmit(fout,slv_buf);

  return(-1);
}

static int cmp_protocol(const void *key, const void *proto)
{
  return(strncmp((const char *)key,((struct protocol *)proto)->command,sizeof(((struct protocol *)0)->command)));
}

void slave_sigio_handler(int sig, siginfo_t *info, void *ucontext)
{
  /* SIGIO on info.si_fd */
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
  // command list must be sorted
  static struct protocol proto[]=
  {
    { "QERY", slave_cmd_qery },
    { "QUIT", slave_cmd_quit },
    { "SLOW", slave_cmd_slow },
    { "VERS", slave_cmd_vers },
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
  rb_init(&rbv,buf,sizeof(buf),fin);
  
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


////////////////////
// master globals


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
  rb_init(&rbv,buf,sizeof(buf),fin);
  
  // business logic for master -----------------------
  
  // VERSION
  snprintf(buf,sizeof(buf),"VERS,7");
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
        snprintf(buf,sizeof(buf),"QERY,%d",task[j]);
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
